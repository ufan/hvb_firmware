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
