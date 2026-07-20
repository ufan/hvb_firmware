# Factory Tool Bundle Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Physically reorganize the seven implemented factory bring-up tools into a numbered `tools/factory/01_flash/` .. `07_instrumental_cal/` bundle, fix every relative-path/build-graph reference that breaks as a result, and write one official sequential guide (`docs/guide/factory-procedures.md`) covering all seven steps with jw_hvb/jw_lvb branching inline.

**Architecture:** Pure file reorganization + documentation — no new application logic except one thin new wrapper script (`01_flash/flash.sh`). Six existing tool directories move via `mv` + `git add -A` (renames auto-detected); every relative path that assumed the old directory depth gets a targeted `Edit`; `tools/CMakeLists.txt` gets one `add_subdirectory` path update; eight living docs plus the six moved tools' own READMEs get their path references updated; a new top-level guide ties the numbered sequence together.

**Tech Stack:** Bash, Python (existing tools, unchanged logic), CMake, Markdown.

**Reference spec:** `docs/superpowers/specs/2026-07-20-factory-tool-bundle-design.md`

---

### Task 1: Move the six tool directories into tools/factory/

**Files:**
- Move: `tools/board_test/factory_bringup.sh` → `tools/factory/02_bringup/factory_bringup.sh`
- Move: `tools/board_test/board_test.sh` → `tools/factory/03_feature_test/board_test.sh`
- Move: `tools/board_test/README.md` → `tools/factory/03_feature_test/README.md`
- Move: `tools/board_test/reports/` → `tools/factory/03_feature_test/reports/` (gitignored, untracked)
- Move: `tools/dac_sweep_test/` → `tools/factory/05_sweep_test/`
- Move: `tools/stress_test/` → `tools/factory/04_stress_test/`
- Move: `tools/jw_hvb_selfcal/` → `tools/factory/06_self_cal/jw_hvb/`
- Move: `tools/jw_lvb_calib/` → `tools/factory/06_self_cal/jw_lvb/`
- Move: `tools/psb_factory_tool/` → `tools/factory/07_instrumental_cal/psb_factory_tool/`

- [ ] **Step 1: Create the target directory skeleton**

```bash
mkdir -p tools/factory/01_flash tools/factory/02_bringup tools/factory/03_feature_test tools/factory/04_stress_test tools/factory/05_sweep_test tools/factory/06_self_cal/jw_hvb tools/factory/06_self_cal/jw_lvb tools/factory/07_instrumental_cal
```

- [ ] **Step 2: Move board_test's two scripts into their separate numbered homes**

```bash
mv tools/board_test/factory_bringup.sh tools/factory/02_bringup/factory_bringup.sh
mv tools/board_test/board_test.sh tools/factory/03_feature_test/board_test.sh
mv tools/board_test/README.md tools/factory/03_feature_test/README.md
mv tools/board_test/reports tools/factory/03_feature_test/reports
rmdir tools/board_test
```

- [ ] **Step 3: Move the remaining five tool directories wholesale**

`mv <dir> <newname>` renames a directory in one step, so the destination
must *not* already exist as an empty directory — remove the empty
placeholders Step 1 created for these five first, except `06_self_cal/`
itself (its two per-board children are what get replaced):

```bash
rmdir tools/factory/05_sweep_test tools/factory/04_stress_test tools/factory/06_self_cal/jw_hvb tools/factory/06_self_cal/jw_lvb tools/factory/07_instrumental_cal
mv tools/dac_sweep_test tools/factory/05_sweep_test
mv tools/stress_test tools/factory/04_stress_test
mv tools/jw_hvb_selfcal tools/factory/06_self_cal/jw_hvb
mv tools/jw_lvb_calib tools/factory/06_self_cal/jw_lvb
mv tools/psb_factory_tool tools/factory/07_instrumental_cal/psb_factory_tool
```

- [ ] **Step 4: Verify nothing was left behind and the moves are clean**

```bash
ls tools/board_test tools/dac_sweep_test tools/stress_test tools/jw_hvb_selfcal tools/jw_lvb_calib tools/psb_factory_tool 2>&1
```

Expected: every path reports `No such file or directory` (all six old directories are gone).

```bash
find tools/factory -maxdepth 2 -type d | sort
```

Expected:
```
tools/factory
tools/factory/01_flash
tools/factory/02_bringup
tools/factory/03_feature_test
tools/factory/03_feature_test/reports
tools/factory/04_stress_test
tools/factory/05_sweep_test
tools/factory/05_sweep_test/tests
tools/factory/06_self_cal
tools/factory/06_self_cal/jw_hvb
tools/factory/06_self_cal/jw_lvb
tools/factory/07_instrumental_cal
tools/factory/07_instrumental_cal/psb_factory_tool
```

- [ ] **Step 5: Stage the moves and confirm git detects them as renames**

```bash
git add -A tools/factory tools/board_test tools/dac_sweep_test tools/stress_test tools/jw_hvb_selfcal tools/jw_lvb_calib tools/psb_factory_tool
git status --short | grep -c "^R "
```

Expected: a count matching the tracked-file total (3 + 4 + 2 + 3 + 3 + 39 = 54 tracked files should show as `R ` renames, not separate `D`/`A` pairs — git's similarity-based rename detection handles this automatically on `git add -A` even though plain `mv` was used instead of `git mv`).

- [ ] **Step 6: Commit**

```bash
git commit -m "$(cat <<'EOF'
refactor(tools): move factory bring-up tools into tools/factory/

Physical reorganization only, no logic changes — path-reference fixes
follow in subsequent commits. See
docs/superpowers/specs/2026-07-20-factory-tool-bundle-design.md.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: Fix internal relative-path references in the moved scripts

**Files:**
- Modify: `tools/factory/03_feature_test/board_test.sh`
- Modify: `tools/factory/02_bringup/factory_bringup.sh`
- Modify: `tools/factory/05_sweep_test/dac_sweep_test.sh`
- Modify: `tools/factory/05_sweep_test/tests/test_dac_sweep_test.sh`
- Modify: `tools/factory/04_stress_test/stress_test_native.sh`

Every moved directory is now one level deeper than before (`tools/X/` →
`tools/factory/NN_stage/`), which breaks any `$SCRIPT_DIR/..`-style
relative path that assumed the old depth.

- [ ] **Step 1: Fix `board_test.sh`'s CLI binary path**

`tools/factory/03_feature_test/board_test.sh` currently has (line 45):
```bash
CLI="$SCRIPT_DIR/../bin/psb_demo_cli"
```
This resolved to `tools/bin/psb_demo_cli` from the old `tools/board_test/`
location; from the new `tools/factory/03_feature_test/` location it needs
one more `..`:
```bash
CLI="$SCRIPT_DIR/../../bin/psb_demo_cli"
```

- [ ] **Step 2: Fix `factory_bringup.sh`'s two broken references**

`tools/factory/02_bringup/factory_bringup.sh` line 98:
```bash
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
```
→
```bash
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
```

Line 129:
```bash
exec "$SCRIPT_DIR/board_test.sh" --port "$PORT" --assert-fresh "${PASSTHROUGH[@]}"
```
→
```bash
exec "$SCRIPT_DIR/../03_feature_test/board_test.sh" --port "$PORT" --assert-fresh "${PASSTHROUGH[@]}"
```

- [ ] **Step 3: Fix `dac_sweep_test.sh`'s CLI binary path**

`tools/factory/05_sweep_test/dac_sweep_test.sh` line 9:
```bash
CLI="$SCRIPT_DIR/../bin/psb_demo_cli"
```
→
```bash
CLI="$SCRIPT_DIR/../../bin/psb_demo_cli"
```

- [ ] **Step 4: Fix `test_dac_sweep_test.sh`'s root-dir and runner-path resolution**

`tools/factory/05_sweep_test/tests/test_dac_sweep_test.sh` currently:
```bash
TEST_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$TEST_DIR/../../.." && pwd)"
RUNNER="$ROOT_DIR/tools/dac_sweep_test/dac_sweep_test.sh"
```
→
```bash
TEST_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$TEST_DIR/../../../.." && pwd)"
RUNNER="$ROOT_DIR/tools/factory/05_sweep_test/dac_sweep_test.sh"
```

- [ ] **Step 5: Fix `stress_test_native.sh`'s CLI binary path**

`tools/factory/04_stress_test/stress_test_native.sh` line 28:
```bash
CLI="${CLI:-$(cd "$(dirname "$0")/../.." && pwd)/tools/bin/psb_demo_cli}"
```
→
```bash
CLI="${CLI:-$(cd "$(dirname "$0")/../../.." && pwd)/tools/bin/psb_demo_cli}"
```

- [ ] **Step 6: Verify the fixes with grep**

```bash
grep -n "SCRIPT_DIR/\.\./bin\|SCRIPT_DIR/\.\./\.\.\" && pwd\|/../..\" && pwd)/tools/bin\|tools/dac_sweep_test/dac_sweep_test.sh\|\"\$SCRIPT_DIR/board_test.sh\"" tools/factory/03_feature_test/board_test.sh tools/factory/02_bringup/factory_bringup.sh tools/factory/05_sweep_test/dac_sweep_test.sh tools/factory/05_sweep_test/tests/test_dac_sweep_test.sh tools/factory/04_stress_test/stress_test_native.sh
```

Expected: no output (none of the old, now-wrong path forms remain).

- [ ] **Step 7: Commit**

```bash
git add tools/factory/03_feature_test/board_test.sh tools/factory/02_bringup/factory_bringup.sh tools/factory/05_sweep_test/dac_sweep_test.sh tools/factory/05_sweep_test/tests/test_dac_sweep_test.sh tools/factory/04_stress_test/stress_test_native.sh
git commit -m "$(cat <<'EOF'
fix(tools): update relative paths broken by the factory/ move

Every moved script assumed its old directory depth for locating
tools/bin/psb_demo_cli, the repo root, or a sibling script — all now
one level deeper under tools/factory/NN_stage/.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: Fix the CMake build graph

**Files:**
- Modify: `tools/CMakeLists.txt`
- Modify: `tools/factory/07_instrumental_cal/psb_factory_tool/repl/CMakeLists.txt`
- Modify: `tools/factory/07_instrumental_cal/psb_factory_tool/gui/CMakeLists.txt`

- [ ] **Step 1: Update the add_subdirectory path**

`tools/CMakeLists.txt` currently:
```cmake
if(BUILD_FACTORY)
    add_subdirectory(psb_factory_tool)
endif()
```
→
```cmake
if(BUILD_FACTORY)
    add_subdirectory(factory/07_instrumental_cal/psb_factory_tool)
endif()
```

- [ ] **Step 2: Fix the REPL's relative include paths**

`tools/factory/07_instrumental_cal/psb_factory_tool/repl/CMakeLists.txt`
currently:
```cmake
target_include_directories(psb_factory_tui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../psb_modbus_core
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
    ${MODBUSLIB_INCLUDE_DIR}
)
```
→
```cmake
target_include_directories(psb_factory_tui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../psb_modbus_core
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../include
    ${MODBUSLIB_INCLUDE_DIR}
)
```

(`repl/` moved from `tools/psb_factory_tool/repl/` to
`tools/factory/07_instrumental_cal/psb_factory_tool/repl/` — two extra
directory levels inserted, so both `..`-chains need two more `..` each.)

- [ ] **Step 3: Fix the GUI's identical relative include paths**

`tools/factory/07_instrumental_cal/psb_factory_tool/gui/CMakeLists.txt` has
the same two lines in its own `target_include_directories(psb_factory_gui
PRIVATE ...)` block — apply the identical fix:
```cmake
target_include_directories(psb_factory_gui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../psb_modbus_core
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../include
    ${MODBUSLIB_INCLUDE_DIR}
)
```

- [ ] **Step 4: Clean rebuild to verify**

```bash
rm -rf tools/build
cmake -S tools -B tools/build -G Ninja
cmake --build tools/build --target psb_factory_tui -j
```

Expected: configure succeeds (no "could not find" include errors), build
succeeds, and `tools/bin/psb_factory_tui` exists afterward:
```bash
ls -la tools/bin/psb_factory_tui
```

- [ ] **Step 5: Commit**

```bash
git add tools/CMakeLists.txt tools/factory/07_instrumental_cal/psb_factory_tool/repl/CMakeLists.txt tools/factory/07_instrumental_cal/psb_factory_tool/gui/CMakeLists.txt
git commit -m "$(cat <<'EOF'
fix(tools): update CMake paths for psb_factory_tool's new location

add_subdirectory() and both repl/gui CMakeLists' relative includes to
psb_modbus_core/include need two more ../ levels after the move into
tools/factory/07_instrumental_cal/.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 4: Fix self-references inside the moved tools' own docs

**Files:**
- Modify: `tools/factory/03_feature_test/README.md`
- Modify: `tools/factory/05_sweep_test/README.md`
- Modify: `tools/factory/06_self_cal/jw_hvb/README.md`
- Modify: `tools/factory/06_self_cal/jw_lvb/README.md`
- Modify: `tools/factory/04_stress_test/stress_test.py`

These files moved along with their directories but still contain prose
referencing their *own* old path — fix each to the new path.

- [ ] **Step 1: Fix `03_feature_test/README.md`**

Three references, old → new:
```
tools/board_test/board_test.sh --port /dev/ttyUSB0
```
→
```
tools/factory/03_feature_test/board_test.sh --port /dev/ttyUSB0
```
```
Without `--report`, reports are written to `tools/board_test/reports/`.
```
→
```
Without `--report`, reports are written to `tools/factory/03_feature_test/reports/`.
```
```
tools/board_test/factory_bringup.sh --build-dir build_psb_lvb --port /dev/ttyUSB0
```
→
```
tools/factory/02_bringup/factory_bringup.sh --build-dir build_psb_lvb --port /dev/ttyUSB0
```
Also update the two sibling-tool mentions:
```
This complements `tools/stress_test/` (Modbus-protocol load/QA testing) and
`tools/dac_sweep_test/` (DAC linearity characterization) rather than
```
→
```
This complements `tools/factory/04_stress_test/` (Modbus-protocol load/QA testing) and
`tools/factory/05_sweep_test/` (DAC linearity characterization) rather than
```

- [ ] **Step 2: Fix `05_sweep_test/README.md`**

```
tools/dac_sweep_test/dac_sweep_test.sh --port /dev/ttyUSB0
```
→
```
tools/factory/05_sweep_test/dac_sweep_test.sh --port /dev/ttyUSB0
```
```
Without `--report`, reports are written to `tools/dac_sweep_test/reports/`.
```
→
```
Without `--report`, reports are written to `tools/factory/05_sweep_test/reports/`.
```
```
bash tools/dac_sweep_test/tests/test_dac_sweep_test.sh
```
→
```
bash tools/factory/05_sweep_test/tests/test_dac_sweep_test.sh
```

- [ ] **Step 3: Fix `06_self_cal/jw_hvb/README.md`**

```
pip install -r tools/jw_hvb_selfcal/requirements.txt
```
→
```
pip install -r tools/factory/06_self_cal/jw_hvb/requirements.txt
```
```
python3 tools/jw_hvb_selfcal/jw_hvb_selfcal.py --port /dev/ttyUSB0 --dry-run
```
→
```
python3 tools/factory/06_self_cal/jw_hvb/jw_hvb_selfcal.py --port /dev/ttyUSB0 --dry-run
```
```
python3 tools/jw_hvb_selfcal/jw_hvb_selfcal.py --port /dev/ttyUSB0
```
→
```
python3 tools/factory/06_self_cal/jw_hvb/jw_hvb_selfcal.py --port /dev/ttyUSB0
```

- [ ] **Step 4: Fix `06_self_cal/jw_lvb/README.md`**

```
pip install -r tools/jw_lvb_calib/requirements.txt
```
→
```
pip install -r tools/factory/06_self_cal/jw_lvb/requirements.txt
```
```
python3 tools/jw_lvb_calib/jw_lvb_calibrate.py --port /dev/ttyUSB0
```
→
```
python3 tools/factory/06_self_cal/jw_lvb/jw_lvb_calibrate.py --port /dev/ttyUSB0
```
```
`tools/stress_test/stress_test.py`) — the only third-party dependency,
```
→
```
`tools/factory/04_stress_test/stress_test.py`) — the only third-party dependency,
```

- [ ] **Step 5: Fix `04_stress_test/stress_test.py`'s two self-referencing report footers**

```python
    lines.append(f"\n---\n*`tools/stress_test/stress_test.py --mode ci`*\n")
```
→
```python
    lines.append(f"\n---\n*`tools/factory/04_stress_test/stress_test.py --mode ci`*\n")
```
```python
    lines.append(f"\n---\n*`tools/stress_test/stress_test.py --mode qa`*\n")
```
→
```python
    lines.append(f"\n---\n*`tools/factory/04_stress_test/stress_test.py --mode qa`*\n")
```

- [ ] **Step 6: Verify with grep**

```bash
grep -rn "tools/board_test\|tools/dac_sweep_test\|tools/stress_test\|tools/jw_hvb_selfcal\|tools/jw_lvb_calib" tools/factory/
```

Expected: no output.

- [ ] **Step 7: Commit**

```bash
git add tools/factory/03_feature_test/README.md tools/factory/05_sweep_test/README.md tools/factory/06_self_cal/jw_hvb/README.md tools/factory/06_self_cal/jw_lvb/README.md tools/factory/04_stress_test/stress_test.py
git commit -m "$(cat <<'EOF'
docs(tools): fix self-referencing paths in moved tools' own docs

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 5: Create tools/factory/01_flash/flash.sh

**Files:**
- Create: `tools/factory/01_flash/flash.sh`

- [ ] **Step 1: Write the script**

```bash
#!/usr/bin/env bash
# flash.sh — step 01: flash (or debug-attach) a build onto real hardware.
#
# Thin wrapper around `west flash`/`west debug` giving this step the same
# --board/--build-dir/--runner invocation style as the rest of the numbered
# factory sequence. See docs/guide/flashing-and-debug-guide.md for probe
# setup (J-Link vs CMSIS-DAP/OpenOCD) and troubleshooting.
#
# Usage: ./flash.sh --board jw_hvb|jw_lvb --build-dir PATH [--runner jlink|openocd] [--debug]
set -euo pipefail

BOARD=""
BUILD_DIR=""
RUNNER="jlink"
DEBUG=0

usage() {
    cat <<EOF
Usage: $(basename "$0") --board jw_hvb|jw_lvb --build-dir PATH [options]
  --board NAME     Board variant, jw_hvb or jw_lvb (required — validated
                    against the actual build directory's own board, since
                    west resolves the real flash target from --build-dir)
  --build-dir PATH West build directory to flash/debug (required)
  --runner NAME    west runner: jlink (default) or openocd
  --debug          Run 'west debug' (gdb attach) instead of 'west flash'
  -h, --help       Show this help
EOF
}

while (($#)); do
    case "$1" in
    --board) BOARD="$2"; shift 2 ;;
    --build-dir) BUILD_DIR="$2"; shift 2 ;;
    --runner) RUNNER="$2"; shift 2 ;;
    --debug) DEBUG=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [[ -z "$BOARD" ]]; then
    echo "Error: --board is required (jw_hvb or jw_lvb)" >&2
    usage >&2
    exit 2
fi
if [[ "$BOARD" != "jw_hvb" && "$BOARD" != "jw_lvb" ]]; then
    echo "Error: --board must be jw_hvb or jw_lvb, got '$BOARD'" >&2
    exit 2
fi
if [[ -z "$BUILD_DIR" ]]; then
    echo "Error: --build-dir is required" >&2
    usage >&2
    exit 2
fi
if ! command -v west >/dev/null 2>&1; then
    echo "Error: 'west' not found on PATH — activate your Zephyr venv first" >&2
    exit 2
fi
CACHE_FILE="$BUILD_DIR/CMakeCache.txt"
if [[ -f "$CACHE_FILE" ]]; then
    CACHED_BOARD="$(sed -n 's/^CACHED_BOARD:STRING=//p' "$CACHE_FILE")"
    if [[ -n "$CACHED_BOARD" && "$CACHED_BOARD" != "$BOARD" ]]; then
        echo "Error: --board $BOARD doesn't match build dir's board '$CACHED_BOARD'" >&2
        exit 2
    fi
fi

if (( DEBUG )); then
    echo "== west debug ($BOARD, runner: $RUNNER) =="
    exec west debug -d "$BUILD_DIR" -r "$RUNNER"
else
    echo "== west flash ($BOARD, runner: $RUNNER) =="
    exec west flash -d "$BUILD_DIR" -r "$RUNNER"
fi
```

- [ ] **Step 2: Make it executable**

```bash
chmod +x tools/factory/01_flash/flash.sh
```

- [ ] **Step 3: Verify the help text and argument validation work**

```bash
tools/factory/01_flash/flash.sh --help
```
Expected: usage text prints, exit code 0.

```bash
tools/factory/01_flash/flash.sh --board bogus --build-dir /tmp/nonexistent; echo "exit: $?"
```
Expected: `Error: --board must be jw_hvb or jw_lvb, got 'bogus'` and `exit: 2`.

- [ ] **Step 4: Commit**

```bash
git add tools/factory/01_flash/flash.sh
git commit -m "$(cat <<'EOF'
feat(tools): add tools/factory/01_flash/flash.sh

Thin west flash/debug wrapper giving step 01 the same --board/
--build-dir/--runner invocation style as the rest of the numbered
factory sequence (steps 02-07 already existed as scripts).

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 6: Create tools/factory/README.md index

**Files:**
- Create: `tools/factory/README.md`

- [ ] **Step 1: Write the index**

```markdown
# Factory Tool Bundle

The full sequence a new board goes through after being soldered, in the
order you run them. Full procedure detail, per-board-variant branching,
and the known real-instrumental-calibration gap for jw_hvb:
[`docs/guide/factory-procedures.md`](../../docs/guide/factory-procedures.md).

| Step | Directory | Tool | Board(s) |
|---|---|---|---|
| 1 | `01_flash/` | `flash.sh` | jw_hvb, jw_lvb |
| 2 | `02_bringup/` | `factory_bringup.sh` | jw_hvb, jw_lvb |
| 3 | `03_feature_test/` | `board_test.sh` | jw_hvb, jw_lvb |
| 4 | `04_stress_test/` | `stress_test.py` | jw_hvb, jw_lvb |
| 5 | `05_sweep_test/` | `dac_sweep_test.sh` | jw_hvb only |
| 6 | `06_self_cal/` | `jw_hvb/jw_hvb_selfcal.py` (fallback), `jw_lvb/jw_lvb_calibrate.py` (official) | both, different roles |
| 7 | `07_instrumental_cal/` | `psb_factory_tool` (`psb_factory_tui` REPL) | jw_hvb only |

Each numbered directory has its own README with the full flag reference for
that step's tool.
```

- [ ] **Step 2: Commit**

```bash
git add tools/factory/README.md
git commit -m "$(cat <<'EOF'
docs(tools): add tools/factory/README.md sequence index

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 7: Update the eight living docs

**Files:**
- Modify: `docs/guide/test-tools.md`
- Modify: `docs/guide/calibration-guide.md`
- Modify: `docs/guide/stress-test.md`
- Modify: `docs/guide/flashing-and-debug-guide.md`
- Modify: `docs/guide/parameter-reference.md`
- Modify: `docs/guide/channel-capability-model.md`
- Modify: `ref/jw_lvb/board-design.md`
- Modify: `tools/psb_demo_app/README.md`

- [ ] **Step 1: Update `docs/guide/test-tools.md`**

Table rows (lines 11-14), old → new path column only, rest of each row unchanged:
```
| `board_test.sh` | `tools/board_test/` | ...
| `factory_bringup.sh` | `tools/board_test/` | ...
| `dac_sweep_test.sh` | `tools/dac_sweep_test/` | ...
| `stress_test.py` | `tools/stress_test/` | ...
```
→
```
| `board_test.sh` | `tools/factory/03_feature_test/` | ...
| `factory_bringup.sh` | `tools/factory/02_bringup/` | ...
| `dac_sweep_test.sh` | `tools/factory/05_sweep_test/` | ...
| `stress_test.py` | `tools/factory/04_stress_test/` | ...
```

Command/link lines:
```
tools/board_test/board_test.sh --port /dev/ttyUSB0
```
→
```
tools/factory/03_feature_test/board_test.sh --port /dev/ttyUSB0
```
```
...), and the safety model: [`../../tools/board_test/README.md`](../../tools/board_test/README.md).
```
→
```
...), and the safety model: [`../../tools/factory/03_feature_test/README.md`](../../tools/factory/03_feature_test/README.md).
```
```
tools/board_test/factory_bringup.sh --build-dir build_psb_lvb --port /dev/ttyUSB0
```
→
```
tools/factory/02_bringup/factory_bringup.sh --build-dir build_psb_lvb --port /dev/ttyUSB0
```
```
See [`../../tools/board_test/README.md`](../../tools/board_test/README.md#clean-bring-up)
```
→
```
See [`../../tools/factory/03_feature_test/README.md`](../../tools/factory/03_feature_test/README.md#clean-bring-up)
```
```
tools/board_test/factory_bringup.sh --runner openocd --build-dir build_psb_lvb --port /dev/ttyACM0
```
→
```
tools/factory/02_bringup/factory_bringup.sh --runner openocd --build-dir build_psb_lvb --port /dev/ttyACM0
```
```
tools/dac_sweep_test/dac_sweep_test.sh --port /dev/ttyUSB0
```
→
```
tools/factory/05_sweep_test/dac_sweep_test.sh --port /dev/ttyUSB0
```
```
hardware-free self-test (`tools/dac_sweep_test/tests/test_dac_sweep_test.sh`,
```
→
```
hardware-free self-test (`tools/factory/05_sweep_test/tests/test_dac_sweep_test.sh`,
```
```
details: [`../../tools/dac_sweep_test/README.md`](../../tools/dac_sweep_test/README.md).
```
→
```
details: [`../../tools/factory/05_sweep_test/README.md`](../../tools/factory/05_sweep_test/README.md).
```

- [ ] **Step 2: Update `docs/guide/calibration-guide.md`**

```
**Automated tool (recommended):** `tools/jw_lvb_calib/jw_lvb_calibrate.py`
— see `tools/jw_lvb_calib/README.md`. jw_lvb-only; jw_hvb needs external
```
→
```
**Automated tool (recommended):** `tools/factory/06_self_cal/jw_lvb/jw_lvb_calibrate.py`
— see `tools/factory/06_self_cal/jw_lvb/README.md`. jw_lvb-only; jw_hvb needs external
```
```
python3 tools/jw_lvb_calib/jw_lvb_calibrate.py --port /dev/ttyUSB0 --dry-run
python3 tools/jw_lvb_calib/jw_lvb_calibrate.py --port /dev/ttyUSB0
```
→
```
python3 tools/factory/06_self_cal/jw_lvb/jw_lvb_calibrate.py --port /dev/ttyUSB0 --dry-run
python3 tools/factory/06_self_cal/jw_lvb/jw_lvb_calibrate.py --port /dev/ttyUSB0
```

- [ ] **Step 3: Update `docs/guide/stress-test.md`**

All five occurrences of `tools/stress_test/stress_test.py` → `tools/factory/04_stress_test/stress_test.py`, and `tools/stress_test/stress_test_native.sh` → `tools/factory/04_stress_test/stress_test_native.sh` (same substitution applied at each of the five call sites listed in the file — CI invocation, `--mode qa` invocation, two further `stress_test.py` examples, and the native-script example).

- [ ] **Step 4: Update `docs/guide/flashing-and-debug-guide.md`**

```
tools/board_test/board_test.sh --port /dev/ttyACM0 --read-only | grep variant
```
→
```
tools/factory/03_feature_test/board_test.sh --port /dev/ttyACM0 --read-only | grep variant
```
```
tools/board_test/factory_bringup.sh --runner openocd --build-dir <build-dir> --port /dev/ttyACM0
```
→
```
tools/factory/02_bringup/factory_bringup.sh --runner openocd --build-dir <build-dir> --port /dev/ttyACM0
```
```
inspection via `tools/board_test/board_test.sh` / `psb_demo_cli`.
```
→
```
inspection via `tools/factory/03_feature_test/board_test.sh` / `psb_demo_cli`.
```

- [ ] **Step 5: Update `docs/guide/parameter-reference.md`**

```
voltmeter (see `tools/dac_sweep_test/`), accounting for the ADC input divider
```
→
```
voltmeter (see `tools/factory/05_sweep_test/`), accounting for the ADC input divider
```

- [ ] **Step 6: Update `docs/guide/channel-capability-model.md`**

```
- `tools/psb_factory_tool/repl` — capability pre-checks before
```
→
```
- `tools/factory/07_instrumental_cal/psb_factory_tool/repl` — capability pre-checks before
```
```
- `tools/board_test/board_test.sh` — asserts the expected accept/reject
```
→
```
- `tools/factory/03_feature_test/board_test.sh` — asserts the expected accept/reject
```

- [ ] **Step 7: Update `ref/jw_lvb/board-design.md`**

```
`tools/jw_lvb_calib/jw_lvb_calibrate.py` — each channel genuinely on and
```
→
```
`tools/factory/06_self_cal/jw_lvb/jw_lvb_calibrate.py` — each channel genuinely on and
```

- [ ] **Step 8: Update `tools/psb_demo_app/README.md`**

```
demo tools — for factory calibration, see `tools/psb_factory_tool` and
```
→
```
demo tools — for factory calibration, see `tools/factory/07_instrumental_cal/psb_factory_tool` and
```

- [ ] **Step 9: Verify with grep across the whole repo, excluding historical docs and the worktree**

```bash
grep -rln "tools/board_test\|tools/dac_sweep_test\|tools/stress_test\|tools/jw_hvb_selfcal\|tools/jw_lvb_calib\|tools/psb_factory_tool" . \
  --exclude-dir=.git --exclude-dir=build --exclude-dir=tools/build \
  --exclude-dir=.claude 2>/dev/null \
  | grep -v "^\./docs/superpowers/plans/\|^\./docs/superpowers/specs/"
```

Expected: no output.

- [ ] **Step 10: Commit**

```bash
git add docs/guide/test-tools.md docs/guide/calibration-guide.md docs/guide/stress-test.md docs/guide/flashing-and-debug-guide.md docs/guide/parameter-reference.md docs/guide/channel-capability-model.md ref/jw_lvb/board-design.md tools/psb_demo_app/README.md
git commit -m "$(cat <<'EOF'
docs: update tool paths for the tools/factory/ reorganization

Historical docs/superpowers/plans/ and specs/ entries are left as-is —
they're point-in-time records, not living reference material.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 8: Write docs/guide/factory-procedures.md

**Files:**
- Create: `docs/guide/factory-procedures.md`

- [ ] **Step 1: Write the guide**

```markdown
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
```

- [ ] **Step 2: Commit**

```bash
git add docs/guide/factory-procedures.md
git commit -m "$(cat <<'EOF'
docs: add factory-procedures.md, the official new-board bring-up guide

Walks the full seven-step tools/factory/ sequence in order, branching
per step where jw_hvb and jw_lvb diverge, with an explicit callout of
the one known gap (no automated real-instrumental calibration tool for
jw_hvb).

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 9: Repo-wide validation sweep

**Files:** none (verification only)

- [ ] **Step 1: Confirm no stray old-path references remain anywhere live**

```bash
grep -rln "tools/board_test\|tools/dac_sweep_test\|tools/stress_test\b\|tools/jw_hvb_selfcal\|tools/jw_lvb_calib\|tools/psb_factory_tool" . \
  --exclude-dir=.git --exclude-dir=build --exclude-dir=tools/build \
  --exclude-dir=.claude 2>/dev/null \
  | grep -v "^\./docs/superpowers/plans/\|^\./docs/superpowers/specs/"
```

Expected: no output. (`tools/stress_test\b` word-boundary avoids
false-matching the still-correct `tools/factory/04_stress_test/stress_test.py`.)

- [ ] **Step 2: Full clean rebuild of the host tools**

```bash
rm -rf tools/build tools/bin
cmake -S tools -B tools/build -G Ninja
cmake --build tools/build -j
```

Expected: build succeeds with no errors, and the following exist afterward:
```bash
ls tools/bin/psb_demo_cli tools/bin/psb_factory_tui
```

- [ ] **Step 3: Run the dac_sweep_test self-test (hardware-free) to confirm its path fixes work**

```bash
bash tools/factory/05_sweep_test/tests/test_dac_sweep_test.sh
```

Expected: exits 0, prints its own pass summary (this test runs entirely
against a mock CLI, no real board needed — it's the fastest signal that
Task 2's `ROOT_DIR`/`RUNNER` fix in `test_dac_sweep_test.sh` is correct).

- [ ] **Step 4: No commit this step** — this task is verification-only; if
any check fails, fix the specific broken reference and re-run before
moving to Task 10.

---

### Task 10: Real-hardware validation

**Files:** none (verification only) — requires a jw_hvb board on the bench with a debug probe attached and its Modbus port known (per the flashing/bring-up sessions earlier in this project, the jw_hvb board's Modbus port on this bench is `/dev/ttyACM3`; confirm current attachment before running).

- [ ] **Step 1: Verify the moved feature-test script works standalone**

```bash
tools/factory/03_feature_test/board_test.sh --port /dev/ttyACM3 --read-only
```

Expected: `PASS=` count matching the board's normal read-only baseline
(connectivity, per-channel info/config/cal reads all pass) — confirms
`board_test.sh`'s `CLI` path fix (Task 2 Step 1) resolves
`tools/bin/psb_demo_cli` correctly from its new location.

- [ ] **Step 2: Verify the moved bring-up script's cross-directory reference to board_test.sh**

```bash
tools/factory/02_bringup/factory_bringup.sh --build-dir build_psb_hvb --port /dev/ttyACM3 --runner openocd -y
```

Expected: mass-erase via openocd succeeds, `west flash -r openocd`
succeeds, then the script's final `exec` line successfully invokes
`../03_feature_test/board_test.sh --assert-fresh` (confirms Task 2 Step 2's
two path fixes — `REPO_ROOT` for locating `openocd.cfg`, and the
cross-directory `board_test.sh` reference — both resolve correctly). Report
written under `tools/factory/03_feature_test/reports/`.

- [ ] **Step 3: Confirm the report landed in the new location**

```bash
ls -t tools/factory/03_feature_test/reports/ | head -1
```

Expected: a report file with a timestamp matching the run just performed.

- [ ] **Step 4: No commit this step** — verification-only. If either
script fails, the failure output will point at which relative path in
Task 2 or Task 3 is still wrong; fix it there (don't patch around it here)
and re-run this task from Step 1.

---

### Task 11: Final review commit (if anything was fixed during validation)

**Files:** whatever Tasks 9-10 caused you to touch, if anything

- [ ] **Step 1: Check for uncommitted changes**

```bash
git status --short
```

- [ ] **Step 2: If clean, nothing to do — the plan is complete.** If
Task 9 or 10 required a fix, stage and commit it:

```bash
git add -A
git commit -m "$(cat <<'EOF'
fix(tools): correct path reference found during factory bundle validation

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```
