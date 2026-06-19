# Static Provider Bus Design

Date: 2026-06-19
Status: Draft for review

## Purpose

This document defines the active provider-worker slice after the async Domain Runtime Worker and domain `smf` scaffolding. It replaces runtime pointer wiring with Zephyr static build-time composition.

The goal is to let Virtual Channel Providers act as independent actuators and samplers while the Domain Runtime coordinates them through static Zephyr kernel objects, devicetree-derived topology, and linker-collected binding records.

## Design Decision

Use a static Provider Bus.

The Provider Bus is a small Zephyr-native cross-layer seam made of:

- Static provider command queue or event object for runtime-to-provider notifications.
- Static runtime evidence queue for provider-to-runtime measurement evidence.
- Static per-channel Runtime Config Slots.
- Iterable-section provider binding records derived from devicetree/device instances.

Messages carry routing and notification metadata only. Runtime config payloads are not copied through queues. Providers borrow a const pointer to the current per-channel config slot when they need to apply hardware state.

Providers do not know `struct vc_runtime`, domain internals, Modbus, or frontend adapters. The Domain Runtime does not manually patch provider pointers at boot.

## Static Topology

Channel topology is declared at build time:

- `vc-controller.channels` in devicetree defines ordered logical channels.
- Provider devices are Zephyr device instances.
- Provider binding records connect logical channel index, provider device, config slot, and provider-bus route.
- Kconfig controls queue depths, stack/work behavior, and enabled bus features.

Runtime/domain channel tables and provider bus routing must be generated from static sources. No runtime discovery pass should assign provider pointers, and no product code should rely on magic channel-count constants where devicetree or iterable records can provide the value.

## Shared Static Objects

The Provider Bus owns static objects such as:

```c
struct vc_runtime_config_slot {
	struct k_mutex lock;
	struct vc_runtime_config_snapshot snapshot;
};

enum vc_provider_msg_type {
	VC_PROVIDER_MSG_CONFIG_CHANGED,
	VC_PROVIDER_MSG_SAMPLE_NOW,
	VC_PROVIDER_MSG_STOP,
};

struct vc_provider_msg {
	enum vc_provider_msg_type type;
	uint8_t channel;
	uint32_t config_version;
};
```

The implementation may use:

```c
K_MSGQ_DEFINE(vc_provider_msgq,
	      sizeof(struct vc_provider_msg),
	      CONFIG_VC_PROVIDER_MSGQ_DEPTH,
	      4);

K_MSGQ_DEFINE(vc_runtime_evidence_msgq,
	      sizeof(struct vc_measurement_snapshot),
	      CONFIG_VC_RUNTIME_EVIDENCE_QUEUE_DEPTH,
	      4);
```

If measurement snapshots grow too large for copying later, the evidence path can move to `k_fifo` plus statically allocated message objects. That is not required for this slice because measurement snapshots are small and bounded.

## Zero-Copy Config Access

Runtime config is published into a static per-channel slot:

```text
Domain Runtime Worker
  -> domain_get_runtime_config(channel)
  -> lock slot[channel]
  -> update slot[channel].snapshot
  -> unlock slot[channel]
  -> enqueue CONFIG_CHANGED(channel, version)
```

Provider workers do not receive the config as a queue payload. They receive only a config-changed notification and borrow the slot:

```c
const struct vc_runtime_config_snapshot *vc_provider_bus_acquire_config(uint8_t channel);
void vc_provider_bus_release_config(uint8_t channel);
```

Rules:

- The acquired pointer is valid only until release.
- The provider treats the pointer as read-only.
- The provider may apply config while holding the per-channel slot lock.
- Locks are per channel so one channel's hardware apply does not block publishing config for other channels.
- Providers compare `cfg->version` against their local `applied_config_version` and apply only new versions.

This avoids copying runtime config across the runtime/provider boundary while preserving latest-wins semantics.

## Provider Binding Records

Each provider contributes a static binding record, preferably from DTS-aware macros:

```c
struct vc_provider_binding {
	uint8_t channel;
	const struct device *dev;
	struct vc_runtime_config_slot *config_slot;
	uint32_t route_bit;
};
```

Bindings are linker-collected:

```c
STRUCT_SECTION_ITERABLE(vc_provider_binding, vc_provider_0) = {
	.channel = 0,
	.dev = DEVICE_DT_GET(DT_NODELABEL(vc_ch0)),
	.config_slot = &vc_runtime_config_slots[0],
	.route_bit = BIT(0),
};
```

Production code iterates provider bindings instead of assigning provider pointers at runtime. The binding record should remain simple data. Complex policy stays in the domain; hardware behavior stays in providers.

## Runtime To Provider Flow

When domain state changes provider-visible runtime intent:

1. Runtime updates the static config slot for the channel.
2. Runtime posts `VC_PROVIDER_MSG_CONFIG_CHANGED` with channel and config version.
3. Provider bus dispatcher routes the message to the provider's work item or event bit.
4. Provider wakes, borrows the config slot, applies new config if needed, then releases it.

The runtime never calls ADC/DAC/GPIO operations directly.

## Provider To Runtime Flow

Each provider owns its hardware timing:

1. Provider delayed work wakes because of config notification or sample timeout.
2. Provider applies latest config if version changed.
3. Provider samples supported raw voltage/current/status.
4. Provider publishes a `vc_measurement_snapshot` to the static evidence queue.
5. Domain Runtime Worker drains the evidence queue and calls `domain_consume_measurement()`.

Provider evidence remains raw. The provider does not latch product faults, derate, retry, or map errors to frontend behavior.

## Provider Worker Shape

HVB providers should keep runtime-independent state:

```c
struct hvb_vc_data {
	struct k_work_delayable work;
	uint8_t channel;
	uint32_t applied_config_version;
	uint32_t generation;
	uint16_t provider_status;
};
```

The provider driver uses the neutral Provider Bus API, not `vc_runtime_*()` APIs.

Provider work loop:

```text
wake
  -> acquire config slot for channel
  -> if config version changed, apply hardware config
  -> release config slot
  -> sample supported evidence
  -> publish measurement snapshot
  -> reschedule by sample-rate-ms
```

## Error Handling

- Config apply failure sets provider status `VC_PROVIDER_STATUS_APPLY_FAILED` in the next evidence snapshot.
- ADC/sample failure sets `VC_PROVIDER_STATUS_SAMPLE_ERROR` and omits unsupported or invalid measurement fields from `present_mask`.
- Interlock or hard hardware evidence is reported as provider status/fault cause. Domain decides safe-state and user-visible faults.
- Full provider message queue means a notification was dropped. Since config is latest-wins, providers still recover on the next periodic wake by reading the latest config slot.
- Full evidence queue drops evidence and should increment a provider-local dropped-evidence counter or status bit in a later slice.

## Startup Behavior

This slice does not implement full startup safe-state confirmation. It must still preserve boot safety:

- Provider init configures hardware to safe output state.
- Provider workers do not require runtime pointer attachment.
- Runtime publishes initial safe config slots.
- Provider workers can apply initial safe config from static slots when started.

Startup confirmation remains a later slice in the agreed roadmap.

## Testing Strategy

Native tests should verify stable seams, not private helper functions:

- Runtime publishes config into static slots and posts config-changed provider messages when version changes.
- Provider bus config acquire/release returns a stable read-only pointer for a channel.
- Provider bus message carries channel/version only, not a copied config payload.
- Fake provider worker applies config only when version changes.
- Fake provider publishes measurement evidence through the static evidence queue.
- Domain Runtime drains static evidence queue and updates domain snapshots.
- Queue-full config notification does not lose latest config because provider can still read the slot.
- DTS/iterable provider binding count matches expected static channel topology for test fixtures.

Board build verification remains:

- `west build -b jw_hvb -d build/hvb_controller applications/hvb_controller`

## Out Of Scope

This slice does not implement:

- Settings persistence.
- Per-source stale evidence policy.
- Startup safe-state confirmation.
- Generic dynamic provider registration.
- Hot-plug provider lifecycle.
- A full `zbus` migration.

Those remain later roadmap items unless a future hardware requirement makes them necessary.
