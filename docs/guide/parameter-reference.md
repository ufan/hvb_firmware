# Parameter Reference — Jianwei Voltage-Control Firmware

This document covers every tunable parameter in the firmware: what it controls,
its unit, its factory default, and its Kconfig symbol. It is intended for four
audiences:

- **End users** — understand what the numbers mean and how the system behaves
- **Firmware writers** — understand internal representation and signal flow
- **Calibration technicians** — understand the coefficient math and how to set values
- **Maintainers** — quick lookup of defaults, ranges, and Kconfig symbols

---

## Quick-Reference Table

| Parameter | Kconfig symbol | Default | Unit | Range |
|-----------|---------------|---------|------|-------|
| Max target voltage (validation bound) | `CONFIG_VC_MAX_TARGET_VOLTAGE` | 20000 | ×100 mV | 1–32767 |
| Cal DAC ceiling (build-time) | `CONFIG_VC_CAL_MAX_RAW_DAC` | 65535 | raw DAC count | 1–65535 |
| Output cal gain | `CONFIG_VC_DEFAULT_OUTPUT_CAL_K` | 32768 | ×10⁻⁴ | 1–65535 |
| Voltage measurement cal gain | `CONFIG_VC_DEFAULT_MEASURED_V_CAL_K` | 1 | ×10⁻⁶ | 1–65535 |
| Current measurement cal gain | `CONFIG_VC_DEFAULT_MEASURED_I_CAL_K` | 1 | ×10⁻⁶ | 1–65535 |
| Current limit threshold | `CONFIG_VC_DEFAULT_CURRENT_LIMIT` | 10000 | ×0.1 nA (post-cal) | 1–32767 |
| Ramp step | `CONFIG_VC_DEFAULT_RAMP_STEP` | 50000 | ×100 mV | 1–65535 |
| Ramp interval | — (hardcoded) | 1 | seconds | — |
| Current safe-band | `CONFIG_VC_DEFAULT_CURRENT_SAFE_BAND_PCT` | 10 | % | 0–100 |
| Cal watchdog timeout | `CONFIG_VC_CAL_WATCHDOG_TIMEOUT_S` | 300 | seconds | 1–3600 |

The output cal b-offset and measurement b-offsets default to 0 (no Kconfig symbol;
set at factory calibration time only).

---

## Unit Conventions

All voltage quantities in the firmware use a fixed-point encoding of
**×100 mV per count** (100 mV resolution):

| Value | Encoding | Physical meaning |
|-------|----------|-----------------|
| 100 | 100 | 10 V |
| 1000 | 1000 | 100 V |
| 20000 | 20000 | 2000 V |

Current values are in **×0.1 nA per count** after factory calibration (see §4)
— the same ×0.1 scaling convention as voltage. There is no unity-gain default
for this axis; see below.

Calibration gain coefficients are **not** on a single shared scale — the
output axis and the two measurement axes use different fixed-point encodings
because they operate at very different magnitudes:

- **Output** (`output_calib_k`) uses **×10⁻⁴** (divide by 10000). This axis
  operates near unity (mapping ~100 mV target units to ~1 DAC count), so a
  1-in-10000 step size gives fine relative resolution and `k = 10000` is an
  exact, representable identity gain.

  | Stored value | Effective multiplier |
  |-------------|---------------------|
  | 10000 | 1.0000× (unity, no scaling) |
  | 32768 | 3.2768× |
  | 5000 | 0.5000× |

- **Measurement** (`measured_voltage_calib_k`, `measured_current_calib_k`)
  uses **×10⁻⁶** (divide by 1000000). These axes convert an attenuated raw
  ADC reading (typically a gain around 0.001–0.01, per the HVB sense
  network) into physical units, so they need much finer resolution than
  the output axis at a much smaller magnitude. Because `uint16_t` tops out
  at 65535, unity gain (`k = 1000000`) is **not representable** on this
  axis — it can only ever scale a raw reading *down*, never pass it through
  unchanged. See "Measurement Calibration" below for the derivation.

Offset (b) terms for the output axis are raw DAC counts; for measurement axes,
they are in the same units as the calibrated output (×100 mV or ×0.1 nA).

---

## Signal Chain

```
Operator sets target voltage (×100 mV)
        │
        ▼
  [ramp controller]  applies step/interval limits to reach target gradually
        │
        ▼
  operational_target_voltage  (×100 mV, int16)
        │
        ▼
  output_calib formula:  raw_dac = (target × out_cal_k) / 10000 + out_cal_b
        │
        ▼
  16-bit DAC code (0–65535)
        │
        ▼
  [hardware DAC → HV amplifier → high-voltage output]
        │
        ▼  (measurement feedback path)
  [ADC raw readings: raw_adc_voltage, raw_adc_current]
        │
        ▼
  voltage_calib formula:  measured_voltage = (raw_v × v_cal_k) / 1000000 + v_cal_b
  current_calib formula:  measured_current = (raw_i × i_cal_k) / 1000000 + i_cal_b
        │
        ▼
  [current protection, snapshots, Modbus reporting]
```

All arithmetic is performed in `int64_t` before clamping to `int16_t` to prevent
overflow at intermediate values.

---

## Output Calibration

### DAC Drive Formula

```
raw_dac = (int64_t)(target_voltage × out_cal_k) / 10000 + out_cal_b
```

Result is clamped to [0, 65535] before being sent to the DAC driver.

| Field | Type | Default | Unit |
|-------|------|---------|------|
| `out_cal_k` | `uint16_t` | 32768 | ×10⁻⁴ |
| `out_cal_b` | `int16_t` | 0 | raw DAC counts |

**Why k = 32768 by default:** at the HVB design point of 2000 V full scale
(`target = 20000`) with a 16-bit DAC:

```
raw = (20000 × 32768) / 10000 = 65536 ≈ 65535  (full DAC range)
```

This gives approximately 1 DAC count per 100 mV = 1 DAC count per 1 target unit,
mapping the full voltage range to the full DAC range without calibration.
After factory calibration, `k` is adjusted to remove hardware gain error and
`b` is set to remove the zero-offset.

### Cal Mode DAC Ceiling

`CONFIG_VC_CAL_MAX_RAW_DAC` (default 65535) is a **build-time** hard limit on
the raw DAC code accepted during calibration mode (`vc cal dac <ch> <code>`).
It is not adjustable at runtime. Set it lower in the project Kconfig if the
calibration fixture imposes a voltage safety ceiling lower than the hardware
maximum.

---

## Measurement Calibration

Both voltage and current measurement use the same linear formula, but on a
**finer fixed-point scale than the output axis** (÷1000000, not ÷10000):

```
measured = (int64_t)(raw_adc × cal_k) / 1000000 + cal_b
```

This axis converts a raw, attenuated ADC reading (e.g. HVB's sense network
produces roughly 129 raw counts per DAC code, or a gain on the order of
0.001–0.01 once referred to physical units) into physical units. A ÷10000
divisor — fine enough for the near-unity output axis — only gives k about two
significant digits of resolution at this magnitude, which was measured to
produce ~0.5–0.7% systematic gain error on jw_hvb hardware. ÷1000000 pushes
that down to roughly 0.01%. The trade-off: `uint16_t cal_k` tops out at
65535, so the maximum representable gain is 65535/1000000 ≈ 0.0655, and
**unity gain (k = 1000000) cannot be represented** — this axis can only ever
scale a raw reading down, never pass it through unchanged. The Kconfig
default (`k = 1`) intentionally yields a near-zero reading until real
per-unit calibration is loaded.

### Voltage Measurement

| Field | Type | Default | Unit |
|-------|------|---------|------|
| `v_cal_k` | `uint16_t` | 1 | ×10⁻⁶ |
| `v_cal_b` | `int16_t` | 0 | ×100 mV |

Factory calibration derives k and b from a DAC sweep against a reference
voltmeter (see `tools/dac_sweep_test/`), accounting for the ADC input divider
ratio and offset.

### Current Measurement

| Field | Type | Default | Unit |
|-------|------|---------|------|
| `i_cal_k` | `uint16_t` | 1 | ×10⁻⁶ |
| `i_cal_b` | `int16_t` | 0 | ×0.1 nA |

Factory calibration adjusts k to match the shunt resistor value and ADC
scaling. The physical unit of `measured_current` (and therefore
`current_limit_threshold`) is **×0.1 nA after calibration** — the same ×0.1
per-count convention as voltage. There is no unity-gain default; every
channel needs a real per-unit `i_cal_k` before `measured_current` is
meaningful.

---

## Current Protection

The trip condition itself has no hysteresis: the firmware compares
`measured_current` directly against `current_limit_threshold`.

```
if (measured_current > current_limit_threshold) { /* fault fires */ }
```

`current_safe_band_pct` is not part of the trip condition — it only gates
*clearing* an already-active current fault, so the channel can't be
re-enabled while still hovering right at the limit:

```
safe_to_clear = measured_current <= current_limit_threshold × (100 − current_safe_band_pct) / 100
```

Protection is re-evaluated only on fresh current samples
(`vc_channel_consume_current()`), never synchronously on a config write —
so writing mode/action/threshold as separate register writes always lands
as a complete, consistent set before the next evaluation.

| Parameter | Kconfig | Default | Unit | Notes |
|-----------|---------|---------|------|-------|
| `current_limit_threshold` | `CONFIG_VC_DEFAULT_CURRENT_LIMIT` | 10000 | ×0.1 nA | 10000 = 1000 nA (1 µA) |
| `current_safe_band_pct` | `CONFIG_VC_DEFAULT_CURRENT_SAFE_BAND_PCT` | 10 | % | Hysteresis band below limit, for re-clearing only |
| `current_protection_mode` | — | `DISABLED` | enum | Set to `FLAG_ONLY` or `APPLY_OUTPUT_ACTION` to activate |
| `current_protection_output_action` | — | `DISABLE_IMMEDIATE` | enum | Action taken when protection fires |

**Default behavior:** the current limit defaults to 1000 nA (1 µA). Set
`current_protection_mode` to `FLAG_ONLY` or `APPLY_OUTPUT_ACTION` via Modbus or
shell to activate protection at that limit.

---

## Automatic Recovery

`recovery_policy_mode` only has an effect in **Automatic operating mode** — in
Normal mode every fault always latches and requires an explicit manual clear,
regardless of this setting. This matches the "explicit user action always
wins" behavior of manual host commands.

| Policy | Behavior |
|---|---|
| `MANUAL_LATCH` (default) | Fault always latches; manual clear required. |
| `NEVER_RETRY` | Same as `MANUAL_LATCH` in this implementation. |
| `AUTO_RETRY` | After `auto_retry_delay` seconds and once `measured_current` is back within the current safe band, clears the fault and ramps back to `configured_target_voltage`. |
| `AUTO_DERATE_RETRY` | Same as `AUTO_RETRY`, but each retry targets `configured_target_voltage - (attempt_number × auto_derate_step)`. If that would reach zero or below, the channel exhausts immediately instead of retrying at an invalid target. |

Only `VC_FAULT_CURRENT` is auto-recoverable — hardware, interlock, measurement,
and stale-data faults always require a manual clear, because this codebase
only has a "safe now?" check (the current safe-band) for current faults.

Retry attempts are counted in a sliding window: attempts older than
`auto_retry_window` seconds age out and don't count against
`auto_retry_max_count`. Exceeding the max count (or, for derate retry,
derating to zero or below) latches the channel with `VC_FAULT_RETRY_EXHAUST`
in addition to the original fault cause — that combination means "auto-retry
gave up," not "still faulted, waiting for a retry."

**Known gap:** this does not distinguish "fault detected while already in
Automatic mode" from "fault detected in Normal mode, then switched to
Automatic" — the spec calls for only the former to be retryable. The current
gate checks only the operating mode at evaluation time, not the mode at the
moment the fault was detected. See
`docs/superpowers/plans/2026-07-03-automatic-recovery-policy.md` for what
fixing this would require.

---

## Ramp Control

The ramp controller steps `operational_target_voltage` toward
`configured_target_voltage` on each tick.

| Parameter | Kconfig | Default | Unit | Notes |
|-----------|---------|---------|------|-------|
| `ramp_up_step` | `CONFIG_VC_DEFAULT_RAMP_STEP` | 50000 | ×100 mV | Per interval |
| `ramp_up_interval` | — (hardcoded) | 1 | seconds | |
| `ramp_down_step` | `CONFIG_VC_DEFAULT_RAMP_STEP` | 50000 | ×100 mV | Per interval |
| `ramp_down_interval` | — (hardcoded) | 1 | seconds | |

**Default behavior:** step size 50000 ×100 mV = 5000 V exceeds the 2000 V
full-scale range, so the default ramp is effectively instant — the output
reaches its target in a single tick. Set a smaller step to enforce a controlled
ramp rate. Example: `ramp_up_step = 200` (20 V per second) with a 1 s interval
ramps from 0 to 2000 V in 100 seconds.

The policy tick interval is `CONFIG_VC_RUNTIME_TICK_INTERVAL_MS` (default
100 ms). Ramp intervals are in whole seconds regardless of tick rate.

---

## Target Voltage Validation

`CONFIG_VC_MAX_TARGET_VOLTAGE` (default 20000, ×100 mV = 2000 V) is the
firmware's upper bound check on `configured_target_voltage` writes in normal
and automatic mode. A write exceeding this value is rejected with
`REG_INVALID_VALUE`. The lower bound is always 0 (no negative targets).

Calibration mode bypasses this check; the DAC code is bounded instead by
`CONFIG_VC_CAL_MAX_RAW_DAC`.

---

## Calibration Technician Reference

### Reading Current Values

```
vc cal <ch> config       # print all cal coefficients for channel <ch>
vc ch <ch> status        # print measured voltage and current
```

### Setting Coefficients

```
vc cal <ch> set out_cal_k <value>   # output gain
vc cal <ch> set out_cal_b <value>   # output offset (DAC counts)
vc cal <ch> set v_cal_k <value>     # voltage measurement gain
vc cal <ch> set v_cal_b <value>     # voltage measurement offset
vc cal <ch> set i_cal_k <value>     # current measurement gain
vc cal <ch> set i_cal_b <value>     # current measurement offset
```

Then commit and exit:

```
vc cal <ch> commit
vc cal exit
```

### Computing Coefficients

Given two calibration points (raw₁, ref₁) and (raw₂, ref₂) measured with a
reference instrument, the linear fit is:

```
k = (ref₂ − ref₁) × D / (raw₂ − raw₁)
b = ref₁ − (raw₁ × k) / D
```

where `D` is the axis's divisor: **10000 for output calibration**, **1000000
for voltage/current measurement calibration** (see "Measurement Calibration"
above for why the two axes differ).

`k` must be stored as an integer (round to nearest) and fit in `uint16_t`
(1–65535) — for the measurement axes this caps the representable gain at
65535/1000000 ≈ 0.0655, so `k` will always be a small integer relative to
`D`. `b` must fit in `int16_t` (−32768 to 32767). For output calibration, ref
is in ×100 mV and raw is the DAC code; for measurement calibration, ref is in
×0.1 nA or ×100 mV depending on axis, and raw is the ADC reading.

---

## Firmware Writer Notes

- All cal coefficients live in `struct vc_channel_cal_config`
  (`include/voltage_control/vc_types.h`).
- All operational parameters live in `struct vc_channel_config`.
- Default values are applied in `vc_channel_default_config()` and
  `default_cal_config()` in `lib/voltage_control/vc_channel.c`.
- The b-offsets default to 0 (C struct zero-initialization); only the k gains
  have Kconfig options.
- `CONFIG_VC_CAL_MAX_RAW_DAC` is enforced in `vc_channel_cal_set_raw_dac()`
  and is not a runtime field — there is no register for it.
- The vc_types.h comments on `configured_target_voltage` and related fields say
  "mV" but the actual unit is ×100 mV (100 mV per count). Do not confuse the
  two; always treat the stored integer as a ×100 mV count.
