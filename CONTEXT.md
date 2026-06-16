# Jianwei Voltage-Control Firmware

This context defines the shared language for Jianwei voltage-control board firmware. It covers high-voltage and low-voltage board variants, single-channel and multi-channel boards, and protocol adapters such as Modbus.

## Language

**Voltage-Control Board**:
A Jianwei board that controls one or more voltage output channels and reports voltage/current telemetry. The term covers high-voltage and low-voltage variants.
_Avoid_: HVB when referring to the whole product family

**HVB Variant**:
The current Jianwei high-voltage board variant used as the first concrete implementation of the voltage-control architecture.
_Avoid_: Treating HVB as the whole architecture

**Variant Profile**:
A board-specific description of capabilities such as channel count, voltage/current ranges, precision, safety capabilities, and hardware bindings.
_Avoid_: Board config, hardware config when discussing product capabilities

**Channel**:
One independently controlled voltage output path with its own target, telemetry, protection settings, and runtime state.
_Avoid_: Output, rail, module when independence matters

**Configured Target Voltage**:
The host- or configuration-provided target voltage stored in channel settings.
_Avoid_: Setpoint, ambiguous command wording

**Operational Target Voltage**:
The voltage a channel is currently trying to reach. It can differ from Configured Target Voltage during automatic recovery, such as auto-derating.
_Avoid_: Setpoint when the value may be derated

**Output Drive Level**:
The immediate hardware drive value sent to output hardware, such as a DAC code or PWM duty cycle.
_Avoid_: Output command, setpoint

**Output Enable**:
The runtime gate that allows a channel output to be energized, such as an HV module run/stop signal when the variant has one.
_Avoid_: HW switch, run state

**Output Action**:
A requested output-state transition, such as enable, graceful disable, immediate disable, force output zero, or clamp.
_Avoid_: Mixing output actions with fault-clear commands

**Protection Mode**:
Whether a voltage or current protection is disabled, records only fault history, or applies a protection output action.
_Avoid_: Limit mode when it hides the difference between detection and action

**Protection Output Action**:
The output action applied when a protection event occurs and its Protection Mode applies an action.
_Avoid_: Fault command

**Active Fault Block**:
A current blocking condition that prevents enabling or automatic recovery until cleared according to domain rules.
_Avoid_: Flag when the condition blocks operation

**Fault History**:
The record that a fault event occurred. It remains visible until explicitly cleared and is not the same as an Active Fault Block.
_Avoid_: Latched fault when the condition is historical only

**Safe Band**:
A configured hysteresis margin used before automatic recovery retries. Current recovery uses a below-threshold percentage; voltage recovery uses a percentage band around the target.
_Avoid_: Tolerance when discussing retry eligibility

**Normal Mode**:
The operating mode where channel outputs are explicitly controlled by a remote frontend and protection disables require manual clear/re-enable.
_Avoid_: Manual mode

**Automatic Mode**:
The operating mode where configured channels may auto-start after boot and selected protection events may recover using bounded retry policy.
_Avoid_: Auto mode when formal register names are needed

**Calibration Mode**:
The volatile professional operating mode used during factory manufacturing and service debug. It bypasses the normal calibrated voltage-control loop and exposes raw DAC/ADC control surfaces while hard safety rails remain active. Calibration Mode is not an end-user mode and must not be persisted as a boot mode.
_Avoid_: Factory mode when discussing the domain operating mode, raw debug mode when it hides calibration coefficient workflow

**Calibration Unlock**:
The volatile two-step Modbus guard that must be completed before entering Calibration Mode. It prevents accidental entry by normal tools; it is not cryptographic authentication.
_Avoid_: Password, login, authentication

**Calibration Output Enable**:
The per-channel runtime gate that permits raw DAC output in Calibration Mode. Only one channel may have Calibration Output Enable active at a time.
_Avoid_: Reusing Output Enable when the control path is raw calibration rather than normal product output

**Calibration Commit**:
The per-channel action that persists approved calibration coefficients after factory or service tooling has calculated them externally. It saves calibration coefficients only, not raw debug state or temporary output values.
_Avoid_: Channel save when the intent is specifically to persist calibration coefficients

**Protocol Adapter**:
A frontend-specific translation layer, such as Modbus, that maps protocol requests to protocol-neutral domain commands and snapshots.
_Avoid_: UI when discussing firmware module boundaries
