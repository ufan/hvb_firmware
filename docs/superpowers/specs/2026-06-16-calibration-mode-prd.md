# Calibration Mode Product Requirements Document

Date: 2026-06-16
Status: Draft for spec approval

## Problem Statement

Factory manufacturing and professional repair workflows need a dedicated firmware mode for raw board calibration and debug. The current voltage-control design exposes Normal Mode and Automatic Mode, both of which operate through calibrated product behavior. That is not suitable for deriving calibration coefficients because the factory tool needs direct raw DAC control and raw ADC observation.

Before a board is handed to an end user, factory technicians must calibrate output and measurement paths, calculate coefficients externally, and save those coefficients into the board. When a board is returned for unresolved malfunction, service technicians need a professional debug path that exposes detailed raw board information without making those controls available during normal end-user operation.

## Solution

Add a volatile Calibration Mode as operating mode value `2`, available through Modbus protocol `2.1` after a two-step unlock sequence. Calibration Mode is intended for factory manufacturing and professional service/debug only.

Calibration Mode provides raw hardware access while keeping hard safety rails active:

- Raw DAC writes use native DAC codes.
- Raw ADC sample reads expose signed raw conversion values.
- Calibration coefficient writes are allowed only in Calibration Mode.
- Per-channel Calibration Commit persists approved coefficients.
- Only one channel may have calibration output enabled at a time.
- Hardware interlocks, hard faults, raw DAC bounds, and safe-disable behavior remain active.

Calibration Mode bypasses the normal calibrated target, ramp, automatic recovery, and calibrated protection-action loop. It is not persisted across reboot and must not become an end-user operating state.

Authoritative behavior should be added to the existing domain and protocol specs rather than implemented directly from this PRD:

- Domain behavior spec: `docs/superpowers/specs/2026-06-15-voltage-control-domain-behavior.md`
- Modbus protocol reference: `ref/modbus_interface.md`
- Domain vocabulary: `CONTEXT.md`

## User Stories

1. As a factory technician, I want to enter Calibration Mode with an explicit unlock sequence, so that accidental end-user entry is prevented.
2. As a factory technician, I want Calibration Mode to force outputs off on entry, so that raw control starts from a known safe baseline.
3. As a factory technician, I want the firmware to reject Calibration Mode entry if it cannot confirm outputs are disabled, so that raw debug control is never active while a channel is stuck energized.
4. As a factory technician, I want to enable calibration output for one channel at a time, so that channel characterization is isolated and safe.
5. As a factory technician, I want to write native raw DAC codes, so that I can characterize the output path without calibrated voltage-control policy.
6. As a factory technician, I want nonzero raw DAC writes rejected until calibration output is enabled, so that a nonzero code cannot be preloaded before output enable.
7. As a factory technician, I want raw DAC writes bounded by the variant maximum, so that firmware prevents values outside the hardware-safe range.
8. As a factory technician, I want to trigger a per-channel raw sample, so that I can correlate ADC readings with a known raw DAC operating point.
9. As a factory technician, I want the sample command to capture both raw voltage and raw current values, so that calibration tooling can use correlated measurements.
10. As a factory technician, I want raw ADC values exposed as signed 32-bit values, so that factory calibration does not lose precision from 24-bit ADC conversions.
11. As a factory technician, I want raw sample status, so that tooling can distinguish no sample, valid sample, busy sample, and sample error.
12. As a factory technician, I want to write calibration coefficients in Calibration Mode, so that I can test coefficients calculated by external factory tooling.
13. As a factory technician, I want coefficient writes to update the current in-memory configuration immediately, so that I can verify trial coefficients before committing them.
14. As a factory technician, I want a per-channel Calibration Commit action, so that only validated coefficients are persisted.
15. As a factory technician, I want Calibration Commit rejected while raw output is active, so that persistence occurs only from a safe idle state.
16. As a factory technician, I want Calibration Commit to save only calibration coefficients, so that temporary raw-control or debug state is never persisted accidentally.
17. As a factory technician, I want Calibration Mode to be volatile across reboot, so that a restarted board never boots into raw-control mode.
18. As a factory technician, I want to exit Calibration Mode into Normal or Automatic Mode with raw output forced off, so that raw DAC state cannot leak into product operation.
19. As a repair technician, I want raw ADC, raw DAC readback, and detailed calibration status, so that I can diagnose malfunctioning returned boards.
20. As a repair technician, I want existing fault history preserved when entering Calibration Mode, so that useful diagnostic evidence is not lost.
21. As a repair technician, I want hard interlocks and hardware faults to continue blocking output in Calibration Mode, so that diagnostic work remains safe.
22. As an end user, I want calibration coefficients to be readable but not writable in Normal or Automatic Mode, so that normal tools can inspect calibration without corrupting it.
23. As an end user, I want raw ADC/DAC/debug registers unavailable outside Calibration Mode, so that end-user workflows cannot access professional debug controls.
24. As a host-tool developer, I want protocol version `2.1` to indicate Calibration Mode support, so that tooling can refuse unsupported firmware.
25. As a host-tool developer, I want one global unlock register that reads back as zero, so that the unlock flow behaves like a simple self-clearing command surface.
26. As a firmware maintainer, I want coefficient calculation to remain outside firmware, so that firmware only stores and applies coefficients while factory tooling owns fitting and reports.
27. As a firmware maintainer, I want normal protection configuration left untouched by Calibration Mode, so that temporary debug operation does not silently mutate product settings.
28. As a firmware maintainer, I want calibrated protection actions ignored only while Calibration Mode is active, so that untrusted or incomplete coefficients do not prevent calibration.
29. As a firmware maintainer, I want Automatic retry and cooldown state cleared on Calibration Mode entry, so that automatic behavior cannot run inside raw-control mode.
30. As a production owner, I eventually want a factory handoff action and calibration-complete enforcement, so that production firmware can block normal output before calibration is complete.

## Implementation Decisions

- Calibration Mode is added as operating mode value `2`.
- Calibration Mode is for factory manufacturing and professional repair/debug, not ordinary end-user operation.
- Initial control surface is Modbus only.
- Protocol major remains `2`; protocol minor is bumped to `1`.
- Calibration Mode entry is protected by one global `CAL_UNLOCK` register in the reserved extension block.
- The unlock sequence is writing `0xCA1B`, then `0xA11B`, then writing operating mode `2`.
- `CAL_UNLOCK` always reads back as `0`.
- A wrong unlock value clears unlock progress.
- Successful Calibration Mode entry clears unlock progress.
- Unlock gates only entry; calibration-only registers do not require repeated unlock while Calibration Mode remains active.
- Calibration Mode is volatile and never persisted as boot operating mode.
- Entering Calibration Mode immediately disables all outputs and forces raw DAC state to zero.
- If firmware cannot confirm outputs are disabled during entry, mode entry fails into a hard safe fault state.
- Exiting Calibration Mode forces calibration output disabled and raw DAC state to zero.
- Normal calibrated target, ramp, automatic recovery, and calibrated protection-action behavior are bypassed while Calibration Mode is active.
- Normal protection configuration is not changed by entering, using, or exiting Calibration Mode.
- Hard safety rails remain active, including variant raw DAC bounds, output-enable gating, hardware interlocks, and hardware fault handling.
- Raw DAC code is a `UINT16` native DAC device code with a variant-defined maximum.
- Raw ADC voltage and current values are signed 32-bit values exposed as split Modbus registers.
- Initial raw ADC workflow is explicit sample command first, not continuous raw telemetry.
- One per-channel sample command captures all supported raw measurement paths for that channel.
- Calibration subfeatures are capability-specific. Channels may support only a subset of raw output drive, voltage measurement, and current measurement paths; unsupported calibration registers return unsupported-address behavior instead of fake values.
- Sample status uses four initial states: no valid sample, sample valid, sample busy, and sample error.
- Sample valid means all supported raw measurement paths for the channel succeeded. Sample error means at least one supported raw measurement path failed. Unsupported paths are not treated as failed paths.
- Only one channel may have calibration output enabled or nonzero raw DAC output at a time.
- Calibration output enable is a readable/writable state register.
- While calibration output is disabled, only raw DAC code `0` may be written.
- Disabling calibration output forces raw DAC state/readback to `0`.
- Calibration coefficient registers are readable in Normal Mode and Automatic Mode.
- Calibration coefficient registers are writable only in Calibration Mode.
- Coefficient writes in Calibration Mode update current in-memory configuration immediately.
- Reboot before Calibration Commit restores the previously committed coefficients.
- Per-channel Calibration Commit persists only that channel's calibration coefficients.
- Calibration Commit is allowed only when the target channel is in a safe idle state: Calibration Mode active, calibration output disabled, raw DAC zero, and no hard safety fault active for that channel.
- Factory tooling, not firmware, calculates calibration coefficients.
- A system-level factory handoff action and calibration-complete enforcement are deferred to a future production-grade iteration.

## Testing Decisions

Tests should verify public behavior at stable seams rather than private helper functions. The first implementation should prefer domain behavior tests for product rules and Modbus adapter tests for register-level protocol behavior.

Domain behavior tests should cover:

- Calibration Mode entry requires the unlock sequence.
- Wrong unlock values clear unlock progress.
- Calibration Mode entry disables outputs and raw DAC state.
- Calibration Mode is not persisted as boot operating mode.
- Failed safe-disable during entry produces hard safe fault behavior.
- Exiting Calibration Mode forces raw output off.
- Normal and Automatic modes reject calibration coefficient writes.
- Calibration Mode accepts calibration coefficient writes and applies them to current RAM config.
- Raw DAC nonzero writes require calibration output enable.
- Raw DAC writes enforce variant maximum bounds.
- Single-active-channel raw output is enforced.
- Calibration Commit rejects active output and nonzero raw DAC state.
- Calibration Commit persists only calibration coefficients.
- Normal protection configuration remains unchanged across Calibration Mode use.

Modbus adapter tests should cover:

- Protocol minor reports `1` when Calibration Mode registers are supported.
- `SYS_OPERATING_MODE = 2` is rejected before unlock.
- `CAL_UNLOCK` accepts the two-step sequence and reads back `0`.
- Calibration-only raw registers are inaccessible outside Calibration Mode.
- Calibration coefficient registers are readable outside Calibration Mode but writable only inside Calibration Mode.
- Raw ADC signed 32-bit values are packed and unpacked through HI/LO registers.
- Sample status values map to the documented enum.
- Calibration output enable, raw DAC code, sample command, and commit command map to domain behavior.
- Unsupported channels and reserved writes continue to return the expected Modbus exceptions.

Existing test patterns to reuse:

- Domain tests under `tests/voltage_control/domain/`
- Modbus integration scripts under `tests/integration/modbus/`

## Out Of Scope

- Cryptographic authentication or user account authorization.
- Shell/debug CLI control surface.
- Firmware-side coefficient fitting or multi-point calibration workflow.
- Continuous raw telemetry streaming.
- Production report generation or audit log storage.
- Factory handoff action.
- Calibration-complete enforcement before Normal/Automatic output enable.
- Manufacturing metadata tables or extended debug records beyond the initial raw-control surface.
- End-user UI behavior.

## Further Notes

The implementation should follow the project spec-driven workflow. The next concrete step is to update the domain vocabulary, domain behavior spec, and Modbus protocol reference, then write an implementation plan and implement test-first.
