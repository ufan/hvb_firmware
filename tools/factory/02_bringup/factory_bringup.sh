#!/usr/bin/env bash
# factory_bringup.sh — deterministic clean bring-up: mass-erase + flash + verify
#
# Why this exists: `west flash` alone only programs the application image.
# Every board variant's DTS defines a separate NVS `storage_partition` for
# persisted config/calibration, which is deliberately OUTSIDE that image
# region so it survives ordinary firmware updates. That's correct for field
# updates, but it means a plain reflash during bring-up/testing can leave
# stale config from a board's prior test history in place — indistinguishable
# from a genuine factory-default boot unless you know to pass `--erase`. This
# script makes that the only path, so nobody has to remember the flag.
#
# What it does:
#   1. Mass-erases the whole chip (including the NVS partition) before
#      flashing, guaranteeing every persisted field reverts to its
#      Kconfig-defined default: `west flash -d <build-dir> -r jlink --erase`
#      for the default runner. The `openocd` runner has no --erase
#      equivalent, so for `--runner openocd` this instead runs the chip's
#      flash driver `mass_erase` command directly via a raw openocd
#      invocation before `west flash -r openocd` (no --erase flag).
#   2. Waits for the board to reboot and settle.
#   3. Runs board_test.sh with --assert-fresh, which additionally checks that
#      every channel's CFG_OUTPUT_ENABLED/CFG_TARGET_VOLTAGE actually matches
#      its documented factory default — not just that reads/writes round-trip
#      self-consistently (which board_test.sh's normal checks do, and which
#      would happily "pass" even on a channel stuck at the wrong default).
#
# This is a bring-up/dev-loop tool, not a manufacturing-line tool: it assumes
# you have direct SWD/jlink access to one bench unit and are OK discarding
# whatever NVS content is currently on it. See
# docs/superpowers/plans/2026-07-16-board-lifecycle-state-management.md for
# the broader lifecycle-state roadmap this is the first (cheapest) step of.
#
# Usage: ./factory_bringup.sh --build-dir PATH [--port /dev/ttyUSB0]
#                              [--runner jlink] [-y] [-- board_test.sh args...]
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR=""
PORT="/dev/ttyUSB0"
RUNNER="jlink"
ASSUME_YES=0

usage() {
    cat <<EOF
Usage: $(basename "$0") --build-dir PATH [options] [-- board_test.sh-args...]
  --build-dir PATH      west build directory to flash (required)
  --port PATH           Serial port for board_test.sh (default: /dev/ttyUSB0)
  --runner NAME          west flash runner (default: jlink; also: openocd
                         for CMSIS-DAP probes, e.g. Raspberry Pi Debug Probe)
  -y, --yes              Skip the "this erases everything" confirmation prompt
  -h, --help              Show this help

Any arguments after "--" are passed through to board_test.sh unchanged
(e.g. --channel N, --exercise-outputs). --assert-fresh is always added.

Example:
  $(basename "$0") --build-dir build_psb_lvb --port /dev/ttyUSB0
EOF
}

PASSTHROUGH=()
while (($#)); do
    case "$1" in
    --build-dir) BUILD_DIR="$2"; shift 2 ;;
    --port) PORT="$2"; shift 2 ;;
    --runner) RUNNER="$2"; shift 2 ;;
    -y|--yes) ASSUME_YES=1; shift ;;
    -h|--help) usage; exit 0 ;;
    --) shift; PASSTHROUGH+=("$@"); break ;;
    *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [[ -z "$BUILD_DIR" ]]; then
    echo "Error: --build-dir is required" >&2
    usage >&2
    exit 2
fi
if ! command -v west >/dev/null 2>&1; then
    echo "Error: 'west' not found on PATH — activate your Zephyr venv first" >&2
    exit 2
fi

if (( ! ASSUME_YES )); then
    echo "This will MASS-ERASE the entire chip at build dir '$BUILD_DIR'"
    echo "(runner: $RUNNER) — all persisted config/calibration on this board"
    echo "will be lost and replaced with firmware factory defaults."
    read -r -p "Continue? [y/N] " reply
    case "$reply" in
        [yY]|[yY][eE][sS]) ;;
        *) echo "Aborted."; exit 1 ;;
    esac
fi

echo "== Mass-erase + flash =="
if [[ "$RUNNER" == "openocd" ]]; then
    REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
    CACHE_FILE="$BUILD_DIR/CMakeCache.txt"
    if [[ ! -f "$CACHE_FILE" ]]; then
        echo "Error: $CACHE_FILE not found — build the project first" >&2
        exit 2
    fi
    BOARD="$(sed -n 's/^CACHED_BOARD:STRING=//p' "$CACHE_FILE")"
    OPENOCD_CFG="$REPO_ROOT/boards/jianwei/$BOARD/support/openocd.cfg"
    if [[ ! -f "$OPENOCD_CFG" ]]; then
        echo "Error: no openocd.cfg for board '$BOARD' at $OPENOCD_CFG" >&2
        exit 2
    fi
    # west's openocd runner has no --erase equivalent (only jlink does), so
    # mass-erase directly via the chip's flash driver first. The driver name
    # matches the sourced target config's basename (e.g. stm32f4x, stm32f1x).
    FLASH_DRIVER="$(sed -n 's#.*target/\([a-z0-9]*\)\.cfg.*#\1#p' "$OPENOCD_CFG" | tail -1)"
    if [[ -z "$FLASH_DRIVER" ]]; then
        echo "Error: could not determine flash driver from $OPENOCD_CFG" >&2
        exit 2
    fi
    echo "-- mass-erasing via openocd ($FLASH_DRIVER) --"
    openocd -f "$OPENOCD_CFG" -c "init; reset halt; $FLASH_DRIVER mass_erase 0; exit"
    west flash -d "$BUILD_DIR" -r openocd
else
    west flash -d "$BUILD_DIR" -r "$RUNNER" --erase
fi

echo "== Waiting for board to settle =="
sleep 3

echo "== Verifying clean factory-default state =="
exec "$SCRIPT_DIR/board_test.sh" --port "$PORT" --assert-fresh "${PASSTHROUGH[@]}"
