# Factory Tool Bundle Reorganization — Design

## Problem

The tools that make up a new board's factory bring-up sequence (flash,
deterministic bring-up, feature test, stress test, sweep test, self-cal,
instrumental cal) exist today, but are scattered across `tools/` with no
shared naming or ordering (`tools/board_test/`, `tools/dac_sweep_test/`,
`tools/stress_test/`, `tools/jw_hvb_selfcal/`, `tools/jw_lvb_calib/`,
`tools/psb_factory_tool/`), and there is no single document walking through
the procedure end-to-end — each tool has its own README, and the closest
thing to a sequence is prose scattered across `docs/guide/test-tools.md`
and `docs/guide/calibration-guide.md`. There's also no visibility into
which steps are automated vs. manual, or which steps apply to which board
variant (jw_hvb has a DAC/HV output axis needing external-instrument
calibration; jw_lvb has no DAC and only needs an automated current
zero-offset self-cal).

## Goals

1. Physically reorganize the seven implemented tools into a single
   numbered `tools/factory/` bundle whose directory names encode procedure
   order.
2. Write one official guide (`docs/guide/factory-procedures.md`) walking
   through the full sequence, branching inline per step where board
   variants diverge, and explicitly marking the one known gap (no
   automated real-instrumental calibration tool for jw_hvb).
3. Update every living cross-reference (docs, `tools/CMakeLists.txt`,
   internal script self-references) to the new paths, so nothing breaks.

## Non-goals

- Building the missing jw_hvb instrumental-calibration automation tool —
  documented as a gap, left for a future task.
- Touching `tools/psb_modbus_core` or `tools/psb_demo_app` — these are
  general-purpose host library/apps used well beyond factory workflows,
  not factory-specific tools.
- Editing historical dated docs under `docs/superpowers/plans/` or
  `docs/superpowers/specs/` — those are point-in-time records of past
  decisions, not living reference material; rewriting their paths would
  misrepresent what was true when they were written.
- Leaving compatibility symlinks at the old paths — this is a clean move.

## Final directory structure

```
tools/factory/
├── README.md                          # index: the 7-step sequence, links to the guide
├── 01_flash/
│   └── flash.sh                       # new: wraps `west flash`/`west debug`
├── 02_bringup/
│   └── factory_bringup.sh             # was tools/board_test/factory_bringup.sh
├── 03_feature_test/
│   ├── board_test.sh                  # was tools/board_test/board_test.sh
│   ├── README.md                      # was tools/board_test/README.md
│   └── reports/                       # was tools/board_test/reports/ (~50 files, moved wholesale)
├── 04_stress_test/
│   ├── stress_test.py                 # was tools/stress_test/stress_test.py
│   └── stress_test_native.sh          # was tools/stress_test/stress_test_native.sh
├── 05_sweep_test/                     # jw_hvb only (jw_lvb has no DAC)
│   ├── dac_sweep_test.sh              # was tools/dac_sweep_test/dac_sweep_test.sh
│   ├── README.md                      # was tools/dac_sweep_test/README.md
│   ├── tests/                         # was tools/dac_sweep_test/tests/
│   └── reports/                       # was tools/dac_sweep_test/reports/
├── 06_self_cal/
│   ├── jw_hvb/
│   │   ├── jw_hvb_selfcal.py          # was tools/jw_hvb_selfcal/jw_hvb_selfcal.py
│   │   ├── README.md
│   │   └── requirements.txt
│   └── jw_lvb/
│       ├── jw_lvb_calibrate.py        # was tools/jw_lvb_calib/jw_lvb_calibrate.py
│       ├── README.md
│       └── requirements.txt
└── 07_instrumental_cal/
    └── psb_factory_tool/              # was tools/psb_factory_tool/ (repl/ + gui/, moved as one unit)
        ├── CMakeLists.txt
        ├── repl/
        └── gui/
```

## 01_flash/flash.sh (new)

The only step without a pre-existing script — today it's just `west
flash`/`west debug` plus the board.cmake/openocd.cfg config already in
`boards/`, which stays there (build-system files, not tools). Interface:

```
flash.sh --board jw_hvb|jw_lvb --build-dir <path> [--runner jlink|openocd] [--debug]
```

Resolves the runner (default `jlink`) and calls `west flash -d <build-dir>
-r <runner>` (or `west debug` with `--debug`). Thin — its purpose is a
consistent invocation style matching the other six numbered steps, not new
logic.

## Internal path fixes required

Moving directories breaks several scripts' own relative-path arithmetic —
these are concrete edits, not just doc updates:

| File | Current | Required |
|---|---|---|
| `03_feature_test/board_test.sh` | `CLI="$SCRIPT_DIR/../bin/psb_demo_cli"` | `$SCRIPT_DIR/../../bin/psb_demo_cli` (one more level: `factory/03_feature_test` → `tools`) |
| `02_bringup/factory_bringup.sh` | `"$SCRIPT_DIR/board_test.sh"` | `"$SCRIPT_DIR/../03_feature_test/board_test.sh"` |
| `02_bringup/factory_bringup.sh` | `REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"` | `"$SCRIPT_DIR/../../.." ` (one more level to reach repo root) |
| `05_sweep_test/dac_sweep_test.sh` | `CLI="$SCRIPT_DIR/../bin/psb_demo_cli"` | `$SCRIPT_DIR/../../bin/psb_demo_cli` |
| `05_sweep_test/tests/test_dac_sweep_test.sh` | `ROOT_DIR="$(cd "$TEST_DIR/../../.." && pwd)"` | one more `../`; `RUNNER` path updated to `tools/factory/05_sweep_test/dac_sweep_test.sh` |
| `04_stress_test/stress_test_native.sh` | `$(cd "$(dirname "$0")/../.." && pwd)/tools/bin/...` | `$(dirname "$0")/../../..` (one more level to reach repo root) |
| `07_instrumental_cal/psb_factory_tool/repl/CMakeLists.txt` | `${CMAKE_CURRENT_SOURCE_DIR}/../../psb_modbus_core` | `../../../../psb_modbus_core` |
| `07_instrumental_cal/psb_factory_tool/repl/CMakeLists.txt` | `${CMAKE_CURRENT_SOURCE_DIR}/../../../include` | `../../../../../include` |
| `07_instrumental_cal/psb_factory_tool/gui/CMakeLists.txt` | same two lines | same two fixes |

(`RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"` in both `repl/` and
`gui/` CMakeLists is unaffected — `CMAKE_SOURCE_DIR` is the top of the
CMake project, not depth-relative.)

## Build graph update

`tools/CMakeLists.txt`: `add_subdirectory(psb_factory_tool)` →
`add_subdirectory(factory/07_instrumental_cal/psb_factory_tool)` (still
guarded by `BUILD_FACTORY`). No other moved tool is a CMake target.

## Living docs updated to new paths

`docs/guide/test-tools.md`, `calibration-guide.md`, `stress-test.md`,
`flashing-and-debug-guide.md`, `parameter-reference.md`,
`channel-capability-model.md`, `ref/jw_lvb/board-design.md`,
`tools/psb_demo_app/README.md`.

## New guide: docs/guide/factory-procedures.md

One document, steps 01–07 in order. Structure per step: what it does, the
command, which board variant(s) it applies to (branching inline, same
pattern `calibration-guide.md` §7 already uses), and a link to the
step's own detailed README for full flag reference. Step outline:

1. **Flash** — `01_flash/flash.sh`, both variants, `jlink` default /
   `openocd` alternative (points to `flashing-and-debug-guide.md` for probe
   setup detail).
2. **Deterministic bring-up** — `02_bringup/factory_bringup.sh`, both
   variants, mass-erase + flash + `--assert-fresh` verify.
3. **Feature test** — `03_feature_test/board_test.sh`, both variants,
   full command-surface exercise.
4. **Stress test** — `04_stress_test/stress_test.py`, both variants, `--mode
   ci` (fast gate) vs `--mode qa` (broader pass).
5. **Sweep test** — `05_sweep_test/dac_sweep_test.sh`, **jw_hvb only** —
   explicitly noted as N/A for jw_lvb (no DAC), not just omitted.
6. **Self-calibration** — branches per variant: jw_lvb runs
   `06_self_cal/jw_lvb/jw_lvb_calibrate.py` as its *normal* per-unit
   procedure (no instruments needed by design); jw_hvb's
   `06_self_cal/jw_hvb/jw_hvb_selfcal.py` is explicitly a *fallback* for
   when no DMM/reference load is available, and drives real HV — the guide
   carries over the safety callout from the tool's own docstring.
7. **Real-instrumental calibration** — jw_hvb only (jw_lvb's self-cal in
   step 6 is its complete, official procedure). Documents the current
   manual workflow via `07_instrumental_cal/psb_factory_tool`'s REPL
   (`psb_factory_tui`), referencing `calibration-guide.md` §1-6 for the
   full command reference and worked fit examples. **Explicit gap
   callout**: no automated tool exists today (unlike step 6's jw_lvb path)
   — the technician manually reads the DMM/reference source and types
   `cal coeff` commands back in.

`tools/factory/README.md` is a short index mirroring this same list with
just paths, for anyone browsing the directory rather than reading the
guide.

## Validation plan

1. `rm -rf tools/build && cmake -S tools -B tools/build && cmake --build
   tools/build --target psb_factory_tui` — confirms the CMake path/relative
   include fixes are correct.
2. Run `tools/factory/03_feature_test/board_test.sh --port <port>` and
   `tools/factory/02_bringup/factory_bringup.sh --build-dir <dir> --port
   <port>` against real jw_hvb hardware (on the bench) — confirms the
   cross-directory relative-path fixes (`board_test.sh` from
   `factory_bringup.sh`, `psb_demo_cli` from `board_test.sh`, `REPO_ROOT`
   for the openocd mass-erase path) all resolve correctly and reports land
   in the new `03_feature_test/reports/` location.
3. `grep -rn "tools/board_test\|tools/dac_sweep_test\|tools/stress_test\|tools/jw_hvb_selfcal\|tools/jw_lvb_calib\|tools/psb_factory_tool"` across the repo
   (excluding `docs/superpowers/plans/`, `docs/superpowers/specs/`, and
   `.claude/worktrees/`) — should return nothing, confirming no missed
   living reference.
