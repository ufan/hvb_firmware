# Test Tools — Jianwei Voltage-Control Board

An inventory of everything under `tools/` whose job is validating firmware
or host-tool behavior, rather than being a host application itself
(`psb_demo_cli`/`tui`/`gui`, `psb_factory_tui`/`gui` — see
[`build-tools.md`](build-tools.md) for those). Five distinct tools, each
answering a different question:

| Tool | Location | Answers | Needs real hardware? |
|---|---|---|---|
| `board_test.sh` | `tools/factory/03_feature_test/` | Does `psb_demo_cli`'s whole command surface and capability-gating logic work against this board, right now? | Yes |
| `factory_bringup.sh` | `tools/factory/02_bringup/` | Does this board come up at its true, clean factory-default state after a firmware change? | Yes (+ SWD: J-Link or CMSIS-DAP) |
| `dac_sweep_test.sh` | `tools/factory/05_sweep_test/` | Is DAC output linear across its range, and what's the raw-ADC-vs-DAC-code fit per channel? | Yes |
| `stress_test.py` | `tools/factory/04_stress_test/` | Is Modbus communication fast/reliable enough — deadtime, blocking time, throughput? | Yes |
| `psb_tests` | `tools/psb_modbus_core/tests/` | Does the shared host library (register map, scaling, parsing) behave correctly in isolation? | No (mock virtual board) |

**Quick picks:**
- Just changed firmware or a host tool, want a fast pass/fail sanity check → `board_test.sh`
- Just reflashed and need to know the board's config is genuinely clean, not left over from a prior session → `factory_bringup.sh`
- Characterizing or sanity-checking DAC/ADC linearity, or generating a calibration-fit report → `dac_sweep_test.sh`
- Gating CI on Modbus timing, or running a technician-facing QA pass → `stress_test.py`
- Changed `tools/psb_modbus_core/` logic and want fast feedback with no board attached → `psb_tests`

---

## `board_test.sh` — end-to-end CLI/capability test

Drives the compiled `psb_demo_cli` binary itself — every subcommand, the
same way an operator would type them — against a live board over Modbus
RTU. Variant-agnostic by construction: channel count and every per-channel
`CH_CAP_*` flag are discovered at runtime, so the same script covers
`jw_hvb`, `jw_lvb`, and any future variant unmodified.

```bash
tools/factory/03_feature_test/board_test.sh --port /dev/ttyUSB0
```

Phase 0 connectivity, Phase 1 system-level commands (`info`, `status`,
`monitor`, ...), Phase 2 per-channel tests (config round trips,
capability-gated rejections, the always-on safety invariant). All
per-channel writes are same-value round trips — read the current value,
write it back, verify unchanged — so it never changes configured behavior.
Full details, flags (`--read-only`, `--exercise-outputs`, `--assert-fresh`,
...), and the safety model: [`../../tools/factory/03_feature_test/README.md`](../../tools/factory/03_feature_test/README.md).

## `factory_bringup.sh` — deterministic clean bring-up

A plain `west flash` only programs the application image — the NVS
partition holding persisted config/calibration is deliberately outside that
region so it survives field updates. During bring-up that means a reflash
can silently leave a channel at whatever an earlier test session left it
at, indistinguishable from a genuine factory-default boot. This wraps
`west flash --erase` (mass-erases the NVS partition too) + a settle delay +
`board_test.sh --assert-fresh` (which checks live values against documented
Kconfig defaults, not just self-consistency) into one command:

```bash
tools/factory/02_bringup/factory_bringup.sh --build-dir build_psb_lvb --port /dev/ttyUSB0
```

See [`../../tools/factory/03_feature_test/README.md`](../../tools/factory/03_feature_test/README.md#clean-bring-up)
for flags, and
[`../superpowers/plans/2026-07-16-board-lifecycle-state-management.md`](../superpowers/plans/2026-07-16-board-lifecycle-state-management.md)
for the broader board-lifecycle-state roadmap this is the first step of.

### Alternative debug probe: CMSIS-DAP / OpenOCD

J-Link is the default flash/debug runner for both `jw_hvb` and `jw_lvb`; a
generic CMSIS-DAP probe (e.g. the Raspberry Pi Debug Probe) is also
available via `-r openocd` when a J-Link isn't on the bench:

```bash
tools/factory/02_bringup/factory_bringup.sh --runner openocd --build-dir build_psb_lvb --port /dev/ttyACM0
```

Full host setup, wiring, flashing, and gdb-debugging instructions:
[`flashing-and-debug-guide.md`](flashing-and-debug-guide.md).

## `dac_sweep_test.sh` — DAC linearity characterization

Sweeps every DAC-capable channel through a fixed set of DAC codes, sampling
raw ADC voltage/current at each point, and fits each supported axis against
DAC code via ordinary least squares (slope, intercept, R²) — an engineering
characterization tool, not a calibration-writing one (it enters Calibration
Mode to sample but never writes coefficients, commits, or saves to NVS).

```bash
tools/factory/05_sweep_test/dac_sweep_test.sh --port /dev/ttyUSB0
```

Requires `gnuplot` (`pngcairo` support) for the report's plots. Has its own
hardware-free self-test (`tools/factory/05_sweep_test/tests/test_dac_sweep_test.sh`,
against a mock `psb_demo_cli`) for changes to the script itself. Full
details: [`../../tools/factory/05_sweep_test/README.md`](../../tools/factory/05_sweep_test/README.md).

## `stress_test.py` — Modbus timing and throughput

Two modes against real hardware: `--mode ci` (fast, single-channel,
exit-code driven, meant for a CI gate on every commit) and `--mode qa`
(broader, technician-facing pass). Measures deadtime, blocking time, and
throughput, not just correctness. Full details, register tables, and report
format: [`stress-test.md`](stress-test.md).

## `psb_tests` — host library unit tests

Catch2 unit tests for `tools/psb_modbus_core` (register map/scale
constants, calibration parsing, client read/write logic, TUI formatting)
against a mocked virtual Modbus board — no hardware, no serial port. Built
alongside the other host tools (see [`build-tools.md`](build-tools.md));
run the resulting binary directly:

```bash
tools/build/linux-debug/psb_modbus_core/tests/psb_tests
```

This is the tool to reach for when changing shared host-library logic
(scaling constants, register catalog entries, calibration read/write
plumbing) and wanting fast feedback before touching real hardware at all.

---

## Related docs

- [`build-tools.md`](build-tools.md) — building the host applications these tools drive or link against
- [`flashing-and-debug-guide.md`](flashing-and-debug-guide.md) — getting firmware onto the board in the first place (J-Link or CMSIS-DAP/OpenOCD), which every real-hardware tool here assumes has already happened
- [`calibration-guide.md`](calibration-guide.md) — the factory calibration *procedure* (a human workflow, not a test tool)
- [`channel-capability-model.md`](channel-capability-model.md) — the `CH_CAP_*` semantics `board_test.sh`'s capability-branch checks verify against
- [`modbus-reference.md`](modbus-reference.md) — the wire protocol every one of these tools ultimately speaks
