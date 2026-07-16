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
| Output cal gain mantissa | `CONFIG_VC_DEFAULT_OUTPUT_CAL_K` | 32768 | mantissa | 1–65535 |
| Output cal gain exponent | `CONFIG_VC_DEFAULT_OUTPUT_CAL_K_EXP` | -4 | decimal exponent | -9..4 |
| Voltage measurement cal gain mantissa | `CONFIG_VC_DEFAULT_MEASURED_V_CAL_K` | 1 | mantissa | 1–65535 |
| Voltage measurement cal gain exponent | `CONFIG_VC_DEFAULT_MEASURED_V_CAL_K_EXP` | -6 | decimal exponent | -9..4 |
| Current measurement cal gain mantissa | `CONFIG_VC_DEFAULT_MEASURED_I_CAL_K` | 1 | mantissa | 1–65535 |
| Current measurement cal gain exponent | `CONFIG_VC_DEFAULT_MEASURED_I_CAL_K_EXP` | -6 | decimal exponent | -9..4 |
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

Calibration gain is a decimal floating-point value, `gain = k × 10^k_exp` —
a `uint16_t` mantissa (`k`, 1–65535) paired with an `int16_t` decimal
exponent (`k_exp`, range -9..4), not a single fixed divisor. This replaced a
pre-v3.1 format where each axis had a hardcoded divisor baked into the C
formula (`÷10000` for output, `÷1000000` for both measurement axes) — that
older format is still exactly what you get if you never touch `k_exp`, since
each axis's default `k_exp` reproduces the old divisor precisely
(`k_exp=-4` ⟺ `÷10000`, `k_exp=-6` ⟺ `÷1000000`). The reason a variable
exponent exists at all: a single fixed divisor per axis can't cover both a
sub-unity attenuating front-end *and* a super-unity amplifying one (or an
output stage needing far more DAC-code resolution per volt than a 2000 V
board does) in a 16-bit mantissa — see "Why a variable exponent" below.

- **Output** (`output_calib_k` / `output_calib_k_exp`) defaults to `k_exp =
  -4` (÷10000 equivalent). This axis operates near unity (mapping ~100 mV
  target units to ~1 DAC count), so a 1-in-10000 step size gives fine
  relative resolution at the default exponent, and `k = 10000` is an exact,
  representable identity gain there.

  | Stored k (at default k_exp=-4) | Effective multiplier |
  |-------------|---------------------|
  | 10000 | 1.0000× (unity, no scaling) |
  | 32768 | 3.2768× |
  | 5000 | 0.5000× |

- **Measurement** (`measured_voltage_calib_k`/`_k_exp`,
  `measured_current_calib_k`/`_k_exp`) defaults to `k_exp = -6` (÷1000000
  equivalent). These axes typically convert an attenuated raw ADC reading
  (a gain around 0.001–0.01, per the HVB sense network) into physical units,
  so they need much finer resolution than the output axis at a much smaller
  magnitude at that default. Unlike the pre-v3.1 format, unity gain (or a
  super-unity gain, for a front-end with no attenuating divider) **is**
  representable here — raise `k_exp` instead of trying to push `k` past
  65535. See "Measurement Calibration" below for the derivation.

Offset (b) terms are unaffected by this change — output axis in raw DAC
counts, measurement axes in the same units as the calibrated output (×100 mV
or ×0.1 nA).

### Why a variable exponent

A single fixed divisor per axis (the pre-v3.1 design) has a hard ceiling: the
maximum representable gain is `65535 / D`. For the measurement axes at
`D = 1000000`, that's ≈0.0655 — fine for jw_hvb's attenuating sense network,
but it means **no board with an amplifying (super-unity) front-end could ever
be calibrated correctly**, regardless of `k`. Widening `k` itself to 32-bit
was considered and rejected — this repo's register-write plumbing has no
existing support for atomic multi-register writes (every current 32-bit
register is read-only), and building that from scratch is disproportionate
to the problem. A second, independently-writable 16-bit exponent register
solves the same range problem without that risk: `k` and `k_exp` are two
independently meaningful values, exactly like `k` and `b` already are today,
so there's no atomicity concern in writing them separately.

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
  output_calib formula:  raw_dac = target × out_cal_k × 10^out_cal_k_exp + out_cal_b
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
  voltage_calib formula:  measured_voltage = raw_v × v_cal_k × 10^v_cal_k_exp + v_cal_b
  current_calib formula:  measured_current = raw_i × i_cal_k × 10^i_cal_k_exp + i_cal_b
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
raw_dac = (int64_t)(target_voltage × out_cal_k) × 10^out_cal_k_exp + out_cal_b
```

Result is clamped to [0, 65535] before being sent to the DAC driver.

| Field | Type | Default | Notes |
|-------|------|---------|------|
| `out_cal_k` | `uint16_t` | 32768 | mantissa |
| `out_cal_k_exp` | `int16_t` | -4 | decimal exponent; -4 ⟺ pre-v3.1 ÷10000 |
| `out_cal_b` | `int16_t` | 0 | raw DAC counts |

**Why k = 32768, k_exp = -4 by default:** at the HVB design point of 2000 V
full scale (`target = 20000`) with a 16-bit DAC:

```
raw = 20000 × 32768 × 10^-4 = 65536 ≈ 65535  (full DAC range)
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

Both voltage and current measurement use the same linear formula, defaulting
to a **finer decimal exponent than the output axis** (`k_exp = -6`, versus
the output axis's `-4`):

```
measured = (int64_t)(raw_adc × cal_k) × 10^cal_k_exp + cal_b
```

At the default exponent, this axis converts a raw, attenuated ADC reading
(e.g. HVB's sense network produces roughly 129 raw counts per DAC code, or a
gain on the order of 0.001–0.01 once referred to physical units) into
physical units. The output axis's coarser default exponent (`-4`) only gives
`k` about two significant digits of resolution at this magnitude, which was
measured to produce ~0.5–0.7% systematic gain error on jw_hvb hardware;
`-6` pushes that down to roughly 0.01%. Unlike the pre-v3.1 fixed-÷1000000
format, unity gain (or higher — a super-unity front-end with no attenuating
divider) **is** representable on this axis: raise `k_exp` rather than trying
to push `k` past 65535. The Kconfig default (`k = 1`, `k_exp = -6`)
intentionally yields a near-zero reading until real per-unit calibration is
loaded.

### Voltage Measurement

| Field | Type | Default | Notes |
|-------|------|---------|------|
| `v_cal_k` | `uint16_t` | 1 | mantissa |
| `v_cal_k_exp` | `int16_t` | -6 | decimal exponent; -6 ⟺ pre-v3.1 ÷1000000 |
| `v_cal_b` | `int16_t` | 0 | ×100 mV |

Factory calibration derives k and b from a DAC sweep against a reference
voltmeter (see `tools/dac_sweep_test/`), accounting for the ADC input divider
ratio and offset.

### Current Measurement

| Field | Type | Default | Notes |
|-------|------|---------|------|
| `i_cal_k` | `uint16_t` | 1 | mantissa |
| `i_cal_k_exp` | `int16_t` | -6 | decimal exponent; -6 ⟺ pre-v3.1 ÷1000000 |
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
vc cal <ch> set out_cal_k <value>       # output gain mantissa
vc cal <ch> set out_cal_k_exp <value>   # output gain decimal exponent (-9..4)
vc cal <ch> set out_cal_b <value>       # output offset (DAC counts)
vc cal <ch> set v_cal_k <value>         # voltage measurement gain mantissa
vc cal <ch> set v_cal_k_exp <value>     # voltage measurement decimal exponent (-9..4)
vc cal <ch> set v_cal_b <value>         # voltage measurement offset
vc cal <ch> set i_cal_k <value>         # current measurement gain mantissa
vc cal <ch> set i_cal_k_exp <value>     # current measurement decimal exponent (-9..4)
vc cal <ch> set i_cal_b <value>         # current measurement offset
```

Leave `*_k_exp` untouched (at its factory default, -4 for output, -6 for both
measurement axes) unless the fitted `k` below doesn't fit in `uint16_t` at
that exponent.

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

where `D = 10^(-k_exp)`, using the channel's *current* `k_exp` — normally its
factory default (**-4** ⟹ `D=10000` for output, **-6** ⟹ `D=1000000` for
voltage/current measurement; see "Measurement Calibration" above for why the
two axes differ by default). This is exactly the pre-v3.1 formula with `D`
now derived from `k_exp` rather than hardcoded — if you're using each axis's
default `k_exp`, the arithmetic is unchanged from before.

`k` must be stored as an integer (round to nearest) and fit in `uint16_t`
(1–65535). **If it doesn't fit at the current `k_exp`**, adjust `k_exp` (a
few steps in either direction is usually enough — see "Why a variable
exponent" above) and recompute `k` with the new `D`; this is the case a fixed
divisor couldn't handle before v3.1 (a super-unity measurement front-end, or
an output stage needing far more DAC-code resolution per volt than a 2000 V
board does). `b` must fit in `int16_t` (−32768 to 32767) — unaffected by
`k_exp`. For output calibration, ref is in ×100 mV and raw is the DAC code;
for measurement calibration, ref is in ×0.1 nA or ×100 mV depending on axis,
and raw is the ADC reading.

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
