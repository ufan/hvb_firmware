# Voltage-Control Domain Behavior Specification

Date: 2026-06-15
Status: Draft for implementation planning

## Scope

This document defines protocol-neutral product behavior for Jianwei voltage-control boards. It applies to Modbus now and to future Ethernet or Bluetooth protocol adapters later.

Protocol-specific register layout, function codes, word packing, and Modbus exceptions belong in `ref/modbus_interface.md`. Domain language belongs in `UBIQUITOUS_LANGUAGE.md` and `CONTEXT.md`.

## Domain Concepts

The domain uses these canonical concepts:

| Concept | Meaning |
|---|---|
| Configured Target Voltage | Host/configured target stored in channel settings |
| Operational Target Voltage | Runtime target currently being pursued; may differ during auto-derate recovery |
| Output Drive Level | Immediate DAC/PWM/output-hardware drive value |
| Output Enable | Runtime gate that permits output to energize when a variant has one |
| Output Action | Requested output-state transition |
| Protection Mode | Whether a protection is disabled, history-only, or action-applying |
| Protection Output Action | Output Action applied by a protection event |
| Active Fault Block | Current blocking condition preventing enable/retry until cleared |
| Fault History | Record that a fault event occurred |

## Operating Modes

### Normal Mode

Normal Mode requires explicit remote commands. Any protection event that blocks output creates an Active Fault Block requiring explicit clear/re-enable. Auto-retry settings have no runtime effect in Normal Mode, but they remain configurable so a host can prepare Automatic Mode settings ahead of time.

### Automatic Mode

Automatic Mode may auto-start supported channels after safe startup and configuration load. A channel is eligible only when its Configured Target Voltage is nonzero and its configuration is valid.

Automatic startup behavior:

| Rule | Behavior |
|---|---|
| Eligible channels | Channels with nonzero Configured Target Voltage |
| Invalid channel config | Create configuration-invalid Active Fault Block for that channel only |
| Other channels | Valid channels may still start |
| Startup sequencing | Variant-defined; HVB initial default is parallel |
| Existing Active Fault Blocks | Not retried by switching into Automatic Mode |

Runtime mode changes do not disrupt currently running channels. Switching from Automatic Mode to Normal Mode cancels pending retries and converts them to manual Active Fault Blocks.

## Output Actions

Output Action values have context-specific validity.

| Value | Name | Host action validity | Protection action validity | Behavior |
|---:|---|---|---|---|
| 0 | None | Valid | Valid | No output-state change |
| 1 | Enable | Valid | Invalid | Request Output Enable and ramp toward Operational Target Voltage |
| 2 | Disable Graceful | Valid | Valid | Ramp output down, then disable Output Enable |
| 3 | Disable Immediate | Valid | Valid | Force Output Drive Level to zero and disable Output Enable |
| 4 | Force Output Zero | Invalid | Valid | Force Output Drive Level to zero without changing Output Enable |
| 5 | Clamp | Invalid | Voltage protection only | Clamp Operational Target Voltage to voltage limit threshold |

`Disable Graceful` and `Disable Immediate` do not modify Configured Target Voltage. A later `Enable` ramps toward the unchanged Configured Target Voltage unless blocked by safety.

Writing Configured Target Voltage to `0` is not the same as `Disable Graceful`. A zero target asks the channel to ramp toward 0 V while leaving Output Enable policy unchanged. `Disable Graceful` ramps down and then disables Output Enable.

## Protection Behavior

Protection handling separates detection, history, blocking state, and output-state action.

| Concept | Behavior |
|---|---|
| Protection Mode Disabled | Do not evaluate that protection |
| Protection Mode Flag Only | Record Fault History only; do not create Active Fault Block |
| Protection Mode Apply Output Action | Apply Protection Output Action and create Active Fault Block if blocking |

Current protection is evaluated before voltage protection. If current and voltage faults occur in the same measurement cycle, current determines the Protection Output Action. Both Fault History bits may be recorded.

`Clamp` is invalid for current protection. Current limiting should use Flag Only, Disable Graceful, Disable Immediate, or Force Output Zero until a future product spec defines current-specific derating behavior.

Protection uses calibrated measured voltage/current values, not raw ADC values.

## Fault Lifecycle

An Active Fault Block prevents enable and automatic retry until the domain clears it according to safety rules. Fault History records that a fault occurred and remains visible until explicitly cleared.

Clear behavior:

| Command | Behavior |
|---|---|
| Clear Active Fault Block | Allowed only when the measured condition is inside the Safe Band and domain policy allows |
| Clear Fault History | Clears historical fault records and counters |

Clearing an Active Fault Block while the measured condition is still unsafe is rejected. Clearing an Active Fault Block does not clear Fault History.

## Automatic Recovery

Automatic recovery applies only to faults detected while Automatic Mode is already active. Existing Active Fault Blocks do not become retryable just because the mode changes to Automatic.

Recovery settings are system-wide for the initial design:

| Setting | Behavior |
|---|---|
| Recovery Policy Mode | Manual latch, auto retry, auto derate retry, or never retry |
| Auto Retry Delay | Cooldown before retry |
| Auto Retry Max Count | Maximum retries inside the sliding retry window |
| Auto Retry Window | Sliding window used to count retries |
| Voltage Safe Band % | Voltage band around target required before retry |
| Current Safe Band % | Current below-limit margin required before retry |

Auto Derate Step is per-channel because it is voltage-range-specific.

Retry counting uses a sliding window. Each retry attempt records a retry timestamp. Retry timestamps older than Auto Retry Window are discarded. Auto Retry Count is the number of retry attempts still inside the window. If the next retry would exceed Auto Retry Max Count, the channel gets an Active Fault Block with automatic-retry-exhausted cause.

If no retry occurs for longer than Auto Retry Window, Auto Retry Count naturally returns to 0. Fault History remains until explicitly cleared.

Auto-derate lowers Operational Target Voltage by Auto Derate Step per retry. If derating would push Operational Target Voltage below the variant minimum, latch the channel with an Active Fault Block.

## Safe Band

Safe Band is configurable and separate for current and voltage. Valid range is 0-50 percent. Default is 10 percent for both.

Current recovery is allowed only when measured current is less than:

```text
current_limit * (100 - Current Safe Band %) / 100
```

Voltage recovery is allowed only when measured voltage is within:

```text
target * (100 +/- Voltage Safe Band %) / 100
```

The target used for voltage recovery is the relevant runtime target selected by domain policy, usually Operational Target Voltage during recovery.

## Target Changes While Running

Writing Configured Target Voltage while running is accepted if the value is valid for the variant. The channel service treats the new value as a new target and ramps up or down according to ramp settings.

A host target write does not reset derated/cooldown state while an Active Fault Block or automatic cooldown is present.

## Persistence Scope

Runtime enable state is internal and volatile.

| Parameter class | Persistence rule |
|---|---|
| Configured Target Voltage | Persist only when Save Target Policy is enabled; otherwise save variant safe target default |
| Ramp settings | Persist on channel save action |
| Protection modes, Protection Output Actions, and thresholds | Persist on channel save action |
| Auto Derate Step | Persist on channel save action |
| Calibration coefficients | Persist on channel save action |
| Operating Mode | Persist on system save action |
| Recovery policy and Safe Band settings | Persist on system save action |
| Slave address and baud rate | Persist on system save action and become effective after restart |
| Active Fault Blocks and Fault History | Runtime only unless a future spec defines otherwise |

System save/load/factory reset affects system parameters only. Channel save/load/factory reset affects that channel only. Whole-board operations are performed by iterating system plus supported channels.

Parameter actions are synchronous for the initial implementation. Storage failure is reported to the protocol adapter as a device failure.

## Calibration Behavior

Calibration is per-channel runtime state with variant-provided factory defaults.

| Calibration | Type | Applies to |
|---|---|---|
| Output Calibration K | UINT16 x10000 | Configured/Operational Target Voltage to Output Drive Level path |
| Output Calibration B | INT16 x1000 | Configured/Operational Target Voltage to Output Drive Level path |
| Measured Voltage Calibration K | UINT16 x10000 | Raw voltage measurement to calibrated measured voltage |
| Measured Voltage Calibration B | INT16 x1000 | Raw voltage measurement to calibrated measured voltage |
| Measured Current Calibration K | UINT16 x10000 | Raw current measurement to calibrated measured current |
| Measured Current Calibration B | INT16 x1000 | Raw current measurement to calibrated measured current |

Channel factory reset restores that channel's calibration to variant defaults.

## Open Decisions

| Decision | Why it matters |
|---|---|
| Retryable versus non-retryable fault classes | Requires product and hardware safety decisions per board variant |
| Variant-specific calibration defaults | Factory reset needs authoritative defaults |
| Whether Automatic Mode behavior ships in the first firmware slice | Protocol fields can exist before behavior is fully enabled, but host behavior must be clear |
