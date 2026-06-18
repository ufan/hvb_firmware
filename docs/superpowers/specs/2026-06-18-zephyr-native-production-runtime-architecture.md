# Zephyr-Native Production Runtime Architecture

Date: 2026-06-18
Status: Draft for review

## Purpose

This document refines the production runtime architecture for Jianwei voltage-control firmware after hardware bring-up. It replaces the earlier assumption that the reusable domain must be mostly Zephyr-independent. This firmware repository is a Zephyr application/module repository, so production architecture should use Zephyr's build system, devicetree, Kconfig, device model, kernel services, settings, shell, and protocol facilities directly where they fit.

The target is a reusable Zephyr-native voltage-control domain runtime library with a small subsystem-like virtual voltage channel layer. The first concrete product app is `hvb_controller`; the same architecture should support future board variants whose physical channels may be discrete DAC/ADC/GPIO circuits or smart external modules exposed over buses such as I2C, SPI, CAN, or Ethernet.

## Layer Responsibilities

### Application Orchestration

`hvb_controller` remains thin. It initializes Zephyr devices, starts the voltage-control domain runtime, wires configured virtual channels, starts frontend adapters, and starts work processing. It does not own product policy, Modbus register meaning, calibration behavior, protection rules, or board-specific channel hardware behavior.

The app startup sequence is:

1. Let Zephyr initialize devices according to init priorities.
2. Ensure physical outputs are in their safe state before frontend adapters can accept commands.
3. Discover or bind virtual voltage channels described by devicetree and enabled by Kconfig.
4. Load persistent settings when storage is enabled.
5. Initialize the domain runtime with channel capabilities and default/effective configuration.
6. Start channel runtime processing.
7. Start Modbus and shell adapters.

### Frontend Adapters

Frontend adapters translate external or local user interaction into domain commands and Domain Snapshot reads. V1 adapters are Modbus and embedded shell. Future adapters may include CAN, TCP/IP, and local display/buttons.

Adapters must not call Virtual Channel Providers directly. They may only use the domain runtime facade. Protocol-specific packing, register layout, command syntax, exception mapping, and shell text belong in adapters. Product behavior belongs below the adapter boundary.

### Domain Runtime Library

The domain runtime lives under `lib/voltage_control` as a Zephyr-native library. It is not a formal Zephyr subsystem yet. It may use Zephyr APIs directly, including workqueues, kernel data-passing primitives, logging, settings, device handles, devicetree-derived configuration, and Kconfig feature selection.

The domain runtime owns:

- Product operating modes.
- Command validation.
- Capability interpretation.
- Runtime configuration state.
- Ramping policy.
- Protection and recovery policy.
- Calibration policy and calibrated unit conversion.
- Persistence semantics.
- Host-visible domain snapshots.
- Mapping raw virtual channel evidence into product state.

The public domain runtime facade should remain smaller than the internal implementation. Internally, the domain may split configuration, runtime state, protection, recovery, calibration, persistence, and snapshot projection into focused modules.

### Virtual Voltage Channel Layer

The virtual voltage channel layer is the board-variant plug-in boundary. It should follow Zephyr device/subsystem style without becoming overly broad. A virtual channel provider has devicetree-derived static configuration, runtime data, and API callbacks.

A virtual voltage channel represents one controllable voltage output path as seen by the product domain. The physical implementation is hidden behind the virtual channel provider. HVB can implement a virtual channel using GPIO on/off, AD5541 DAC, ADS1232 ADC, and associated calibration/raw measurement paths. A future board may implement the same virtual channel API using a smart module reached through I2C, SPI, CAN, or another transport.

The mandatory virtual channel capability is on/off control. Other capabilities are advertised and used only when present:

- Raw output drive set.
- Raw voltage measurement.
- Raw current measurement.
- Hardware status/fault reporting.

Calibration-mode raw access is derived from system Calibration Mode support plus the relevant raw output and measurement capabilities; it is not a separate broad channel capability.

The channel layer reports raw hardware evidence and accepts raw channel commands. It does not implement product policy. In particular, it does not own protection latching, recovery decisions, calibrated unit policy, Modbus behavior, or user-facing state.

### Hardware And Zephyr Driver Layer

Low-level drivers remain hardware-shaped and policy-free. They should use Zephyr-standard APIs whenever practical:

- GPIO for enable pins and board inputs.
- DAC API for AD5541-style raw output drive.
- ADC or sensor-style APIs for measurement devices, depending on the final driver shape.
- Sensor API for SHT31 environmental telemetry.
- SPI, I2C, UART, and GPIO devicetree specs for physical bus wiring.
- Zephyr logging for driver diagnostics.

Board-specific pin/peripheral mismatches remain captured in board devicetree and bindings, not in product logic.

## Zephyr Facility Mapping

The architecture should lean on Zephyr rather than recreate framework code.

| Need | Zephyr facility | Architecture use |
| --- | --- | --- |
| Board topology and static composition | Devicetree | Describe virtual channels, physical device phandles, channel capabilities, limits, timing hints, and board topology. |
| Feature selection | Kconfig | Enable adapters, storage, simulator support, domain feature groups, and channel backends. |
| Pluggable virtual channels | Device model pattern | Use read-only config, runtime data, and API callbacks for channel providers. |
| Deferred work and execution contexts | Workqueues/threads | Separate frontend command handling from channel runtime communication and sampling. |
| Fixed-size command/event transfer | `k_msgq` | Candidate for small commands or events where every item must be processed. |
| Pointer-based event transfer | `k_fifo` | Candidate when event payloads are large or ownership transfer is useful. |
| Wakeup/condition signaling | `k_event` | Use for notification flags only; not for payload storage. |
| Typed publish/subscribe | `zbus` | Candidate for typed pub/sub where it reduces coupling; evaluate before making mandatory. |
| Persistence | Settings with NVS backend | Preferred direction for saved product configuration and calibration data. |
| Local service/debug frontend | Zephyr shell | Shell adapter over the domain runtime facade. |
| RS-485 protocol frontend | Zephyr Modbus serial | RTU framing, CRC, serial handling, and DE GPIO via devicetree. |

## Data Pipeline

The architecture uses named data concepts first. Exact C structs, field names, queue types, and memory ownership details are deferred to implementation specs.

### Command Path

```text
Adapter Command
  -> Domain Command
  -> Runtime Config Snapshot
  -> Virtual Channel Command
```

An adapter command is frontend-specific. A domain command is product intent, such as set target, enable/disable, clear fault, enter Calibration Mode, or save parameters. The domain validates the command against product rules and channel capabilities. When command handling changes runtime behavior, the domain publishes a complete runtime configuration snapshot rather than partial field updates. The virtual channel runtime applies the latest complete snapshot at a safe boundary and executes raw channel commands as needed.

Frontend command completion means the domain has accepted or rejected the command and, when accepted, has made the resulting Runtime Config Snapshot available to virtual channel runtimes. It does not mean the physical channel has completed the requested action or that measured output has reached the Configured Target Voltage. Progress remains visible through Domain Snapshots, including Operational Target Voltage during ramping or derating.

Domain Snapshots must make command progress and physical evidence explicit enough that host tools do not infer physical completion from write success. At minimum, the read model must distinguish accepted intent, runtime progress such as ramping or cooldown, the current Operational Target Voltage, measured voltage/current evidence and freshness, and any Active Fault Block. Status bits should be standardized for in-progress state and stale measurement state before production host tooling depends on them.

Freshness is tracked per evidence source and projected into host-visible per-channel status. For example, voltage measurement, current measurement, hardware status, and provider availability may each become stale or unavailable independently. Freshness only applies to evidence sources that a channel advertises as capabilities; an on/off-only channel must not be treated as stale just because it has no voltage or current measurement capability. A single global stale bit is insufficient because it hides partial channel degradation and makes host diagnostics ambiguous.

### Measurement Path

```text
Virtual Channel Measurement Snapshot
  -> Domain Runtime State
  -> Domain Snapshot / Read Model
  -> Adapter Response
```

Virtual channel measurement snapshots are raw hardware evidence. They include the concepts of `generation` and `timestamp`. `generation` identifies a new publication even if values did not change. `timestamp` supports freshness and stale-data policy. The domain consumes already-published measurement evidence, applies calibration and product policy, updates protection/recovery state, and publishes the host-visible read model. Measurement acquisition must not block host request handling or the domain policy path; slow hardware sampling publishes later snapshots instead of stalling domain execution.

Adapter reads return the latest coherent Domain Snapshot. Reads do not synchronously trigger hardware sampling. This keeps Modbus, shell, and future UI behavior consistent and avoids making protocol latency depend on slow measurement hardware.

### Data Ownership

| Data concept | Producer | Owner of meaning | Consumer | Persistence |
| --- | --- | --- | --- | --- |
| Adapter command | Modbus/shell/future UI | Adapter for syntax, domain for product intent | Domain runtime | Runtime only |
| Domain command | Adapter translation | Domain runtime | Domain runtime | Runtime only |
| Runtime config snapshot | Domain runtime | Domain runtime | Virtual channel runtime | Derived from persistent/effective config |
| Virtual channel command | Domain runtime | Domain runtime for policy, channel for raw execution | Virtual channel provider | Runtime only |
| Measurement snapshot | Virtual channel provider | Channel for raw evidence, domain for product meaning | Domain runtime | Runtime only |
| Domain snapshot/read model | Domain runtime | Domain runtime | Adapters | Runtime only |
| Saved configuration | Domain runtime/settings handler | Domain runtime | Domain runtime on load | Persistent when settings enabled |

## Capability And Unit Model

The domain sees each virtual channel through static board-design capabilities and limits. Capabilities are data, not inferred from which C function happens to be non-null, and they are not dynamically negotiated at runtime. Devicetree and Kconfig derive channel count, available channel providers, static capabilities, and the allowed protection-policy surface for each board build. The domain runtime accepts that build-composed capability model as input and only exposes or accepts policies that are valid for it.

Virtual Channel Providers report raw units. The domain applies calibration coefficients and exposes product units. This keeps calibration policy reusable and prevents each board backend from embedding product semantics differently.

If a frontend requests behavior outside the build-composed capability and policy surface, the domain rejects the command and the adapter maps that result into its frontend-specific error shape. For Modbus, the register map header remains a stable protocol address dictionary; offsets do not change per board build. Capability-specific registers that are address-defined but unsupported for a board or channel map to protocol-defined exceptions rather than fake disabled behavior.

Capability enforcement exists at two boundaries. Protocol adapters gate frontend addressability and representation, such as returning Modbus `0x02` for unsupported capability-specific registers. The domain runtime independently validates semantic commands and configuration against the same build-composed capability model so non-Modbus frontends cannot bypass product rules. Domain validation distinguishes nonexistent channel indexes from existing channels that lack a capability; capability misses return `VC_ERR_UNSUPPORTED_CAPABILITY`.

Domain errors are mapped to adapter results through a shared translation. Unsupported channel and unsupported capability errors map to illegal-address protocol exceptions. Invalid-value, invalid-command, and unsafe-state errors map to illegal-data exceptions. Storage failures map to device-failure exceptions. Each adapter translates domain errors consistently to its frontend error representation.

For Modbus, `CH_CAPABILITY_FLAGS` is the primary per-channel discovery mechanism for physical/provider capabilities. The shared register map header defines stable `CH_CAP_*` bits so host tools can hide unsupported controls before touching capability-specific registers. Variant ID and protocol version identify the product/profile, but host tools should not infer per-channel support from variant ID alone. Product policy features such as Automatic Mode or Calibration Mode belong in system/product capability surfaces, not in `CH_CAPABILITY_FLAGS`. Calibration register availability is derived from `SYS_CAP_CALIBRATION_MODE` plus the relevant raw output and measurement capability bits, rather than a broad per-channel calibration bit.

Modbus channel blocks keep core/discovery fields in a contiguous front range and capability-specific fields in the tail. Supported channels must allow broad reads of the core/discovery range. Tail block reads fail as unsupported if any included register is unsupported for that channel, so host tools should read `CH_CAPABILITY_FLAGS` first and then select capability-specific tail groups.

## Concurrency Model

Frontend commands may be serialized through a shared command path because Modbus, shell, and local UI are effectively mutually exclusive in practical operation. Channel runtime communication runs independently so measurement acquisition and hardware communication are not blocked by frontend interaction.

The implementation should prefer complete versioned snapshots over ad hoc shared mutable state:

- Runtime config snapshots move domain-approved intent toward channel execution.
- Measurement snapshots move raw channel evidence toward domain policy.
- Wakeup primitives notify work that new data exists.
- Queue or bus primitives are selected per path based on whether every item must be processed or only the latest state matters.

The exact transport choice is intentionally deferred. `k_msgq`, `k_fifo`, `k_event`, and `zbus` are all valid Zephyr-native tools for different paths.

## Startup And Failure Behavior

Safe startup is mandatory. Physical outputs must be safe before adapters can accept user commands. Runtime enable state is volatile and must not be restored blindly after boot.

Failure rules:

- Missing mandatory virtual channel provider prevents that channel from becoming available.
- Missing optional capability disables the related behavior at build composition time. Missing measurement capability is different from stale measurement evidence.
- Unsupported capability causes the domain command to fail.
- Stale measurements are represented in the Domain Snapshot and are not treated as fresh evidence. Stale evidence can only affect behavior for capabilities the channel advertises and that the configured product policy requires.
- Channel hardware faults become domain-visible fault evidence.
- Storage failure returns a domain error and must not silently mutate saved/effective runtime state.

## Test Strategy

Tests should verify public behavior and stable seams rather than private helper implementation details.

Required coverage areas:

- Domain command validation and capability interpretation.
- Runtime config snapshot publication.
- Measurement snapshot consumption.
- Generation/timestamp freshness behavior.
- Stale measurement handling.
- Protection and recovery behavior.
- Calibration policy and raw/calibrated unit separation.
- Fake virtual channel providers for on/off-only, raw-output-capable, measurement-capable, unavailable, stale, and fault states.
- Adapter architecture checks proving Modbus and shell depend on the domain facade, not virtual channel providers.
- Build checks for `hvb_controller`, `modbus_sim`, and the domain test suite.

## Non-Goals

This document does not define:

- Final C structs or function names.
- Final devicetree binding property names.
- Final choice of `k_msgq`, `k_fifo`, `k_event`, or `zbus` for each path.
- Settings/NVS record schema.
- Exact persistence migration/versioning format.
- Full implementation plan for Virtual Channel Providers.
- Formal promotion of the domain runtime into a Zephyr subsystem.

## References

- Zephyr device driver model: https://docs.zephyrproject.org/3.7.0/kernel/drivers/index.html
- Zephyr devicetree API: https://docs.zephyrproject.org/3.7.0/build/dts/api/api.html
- Zephyr Kconfig: https://docs.zephyrproject.org/3.7.0/build/kconfig/index.html
- Zephyr workqueues: https://docs.zephyrproject.org/3.7.0/kernel/services/threads/workqueue.html
- Zephyr message queues: https://docs.zephyrproject.org/3.7.0/kernel/services/data_passing/message_queues.html
- Zephyr zbus: https://docs.zephyrproject.org/3.7.0/services/zbus/index.html
- Zephyr settings: https://docs.zephyrproject.org/3.7.0/services/settings/index.html
- ZMK behavior pattern reference: https://zmk.dev/docs/development/new-behavior
