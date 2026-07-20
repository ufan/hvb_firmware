# Factory Procedures Guide — New Board Bring-Up

The complete sequence a Jianwei voltage-control board goes through after
being soldered, from first flash to final calibration. Each step lives in
`tools/factory/`, numbered in the order you run them; this guide walks
through the sequence once, branching per step where jw_hvb and jw_lvb
diverge.

*Prerequisite: firmware built for the target board (`west build -b
jw_hvb|jw_lvb applications/psb_controller -d <build-dir>`) and, for the
host-tool steps, `tools/build` built (`cmake -S tools -B tools/build &&
cmake --build tools/build`) — see [`build-tools.md`](build-tools.md).*

---

## 1. Flash

```bash
tools/factory/01_flash/flash.sh --board jw_hvb --build-dir <build-dir>
```

Both boards, either probe. J-Link is the default runner; pass `--runner
openocd` for a CMSIS-DAP probe (e.g. Raspberry Pi Debug Probe). Probe setup,
host udev configuration, and troubleshooting:
[`flashing-and-debug-guide.md`](flashing-and-debug-guide.md).

## 2. Deterministic bring-up

```bash
tools/factory/02_bringup/factory_bringup.sh --build-dir <build-dir> --port <port> [--runner jlink|openocd]
```

Both boards. Mass-erases the whole chip (including the NVS
config/calibration partition) before reflashing, then verifies every
channel actually landed at its documented Kconfig factory default — not
just that it round-trips self-consistently. This is what tells you whether
the board is genuinely at a clean out-of-box state, which matters before
any of the steps below (a stale NVS blob from a previous bring-up attempt
would silently pass a plain feature test). Full flag reference:
[`../../tools/factory/03_feature_test/README.md`](../../tools/factory/03_feature_test/README.md)
(bring-up's `--assert-fresh` verification is the same tool as step 3).

## 3. Feature test

```bash
tools/factory/03_feature_test/board_test.sh --port <port> [--exercise-outputs]
```

Both boards, variant-agnostic (channel count and capability flags are
discovered at runtime). Exercises the full command surface: connectivity,
system commands, per-channel config round-trips, capability-gated
rejections, the always-on safety invariant. Pass `--exercise-outputs` to
also toggle real outputs live, not just round-trip their configured values.

## 4. Stress test

```bash
python3 tools/factory/04_stress_test/stress_test.py --mode ci --port <port>   # fast CI gate
python3 tools/factory/04_stress_test/stress_test.py --mode qa --port <port>   # broader technician pass
```

Both boards. Modbus deadtime, blocking time, and throughput — not just
correctness. `--mode ci` is what should gate a firmware change before it
reaches a bench; `--mode qa` is the broader pass worth running once per
physical unit during bring-up. Full detail:
[`stress-test.md`](stress-test.md).

## 5. Sweep test — jw_hvb only

```bash
tools/factory/05_sweep_test/dac_sweep_test.sh --port <port>
```

**Not applicable to jw_lvb** — jw_lvb has no DAC (`CH_CAP_RAW_OUTPUT_DRIVE`
absent; its channels are fixed-voltage, switchable on/off only), so there's
no output axis to sweep. For jw_hvb, this characterizes every DAC-capable
channel's linearity (raw-ADC-vs-DAC-code fit, slope/intercept/R²) — an
engineering characterization step, not a calibration-writing one; it never
touches calibration coefficients. Worth running once per new jw_hvb unit to
catch a genuinely nonlinear channel before spending time calibrating it.

## 6. Self-calibration

Branches by board — these are two different tools with two different
roles, not the same procedure applied twice.

**jw_lvb — this is the normal, complete procedure, not a fallback:**

```bash
python3 tools/factory/06_self_cal/jw_lvb/jw_lvb_calibrate.py --port <port> --dry-run   # preview first
python3 tools/factory/06_self_cal/jw_lvb/jw_lvb_calibrate.py --port <port>
```

jw_lvb's only per-unit-variable axis is the ACS712 current sensor's
zero-current offset (real chip-to-chip tolerance); gain and voltage are
fixed-resistor-divider stable enough to leave at their factory nominal. No
reference instrument needed by design — this fully automates
measure→compute→write. Full procedure detail (why it measures with the
channel genuinely on, not via Calibration Mode's forced-off state):
[`calibration-guide.md`](calibration-guide.md) §7.

**jw_hvb — explicitly a fallback, not the official procedure:**

```bash
python3 tools/factory/06_self_cal/jw_hvb/jw_hvb_selfcal.py --port <port> --dry-run
python3 tools/factory/06_self_cal/jw_hvb/jw_hvb_selfcal.py --port <port>
```

**Use only when a real DMM/reference load genuinely isn't available.** This
tool self-references against the DAC's *own* existing (assumed-accurate)
voltage mapping rather than true ground truth — it cannot detect an error
common to both the output and measurement paths. **It commands real high
voltage (up to ~90% of the board's rated max, ~1800V at default settings)
on the channel's HV output terminals** — ensure the output is safely
terminated/isolated before running it. The official jw_hvb procedure is
step 7 below.

## 7. Real-instrumental calibration — jw_hvb only

jw_lvb has no instrumental-calibration step — step 6 above is its complete
official procedure.

```bash
tools/bin/psb_factory_tui -p <port>
```

(built from `tools/factory/07_instrumental_cal/psb_factory_tool`, REPL
target `psb_factory_tui` — the Qt GUI variant in the same directory exists
but is not yet release-ready, per [`calibration-guide.md`](calibration-guide.md))

The official jw_hvb procedure: a two-point linear fit of raw DAC/ADC codes
against a real DMM (voltage axis) and a reference current source or
precision load (current axis), entered via the REPL's `cal` command family
and persisted with `cal commit`. Full command reference, the
unlock/enable/sample/coeff/commit workflow, and worked examples for
computing `k`/`exp`/`b` by hand:
[`calibration-guide.md`](calibration-guide.md) §1-6, §8 (post-calibration
verification).

**Known gap:** unlike step 6's jw_lvb path, there is no automated tool for
this step today. The technician manually reads the DMM/reference source at
each point and types the resulting `cal coeff` commands into the REPL by
hand — `dac_sweep_test.sh` (step 5) automates the *sampling and fitting*
half of this same job but deliberately never writes calibration
coefficients (it's a characterization tool). Closing this gap would look
like combining step 5's sweep/fit logic with a real-instrument reading
input and step 7's REPL write path into a new automated tool — not built
yet.

---

## Quick reference — full sequence

```bash
# jw_hvb
tools/factory/01_flash/flash.sh --board jw_hvb --build-dir <dir>
tools/factory/02_bringup/factory_bringup.sh --build-dir <dir> --port <port>
tools/factory/03_feature_test/board_test.sh --port <port>
python3 tools/factory/04_stress_test/stress_test.py --mode qa --port <port>
tools/factory/05_sweep_test/dac_sweep_test.sh --port <port>
python3 tools/factory/06_self_cal/jw_hvb/jw_hvb_selfcal.py --port <port>   # only if no DMM available
tools/bin/psb_factory_tui -p <port>                                        # official calibration

# jw_lvb
tools/factory/01_flash/flash.sh --board jw_lvb --build-dir <dir>
tools/factory/02_bringup/factory_bringup.sh --build-dir <dir> --port <port>
tools/factory/03_feature_test/board_test.sh --port <port>
python3 tools/factory/04_stress_test/stress_test.py --mode qa --port <port>
python3 tools/factory/06_self_cal/jw_lvb/jw_lvb_calibrate.py --port <port>  # official, no instruments needed
```

## Related docs

- [`test-tools.md`](test-tools.md) — deeper detail on steps 2-5
- [`calibration-guide.md`](calibration-guide.md) — deeper detail on steps 6-7
- [`flashing-and-debug-guide.md`](flashing-and-debug-guide.md) — step 1, probe setup
- [`build-tools.md`](build-tools.md) — building the host tools these steps invoke
