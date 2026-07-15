#!/usr/bin/env bash
# deploy_windows.sh — Cross-compile and package psb_demo_tui, psb_demo_cli, and
# psb_factory_tui for Windows (MinGW).
#
# Uses MinGW-w64 cross-compiler on Linux to produce statically-linked .exe
# files. Each is self-contained: copy to any Windows 10/11 machine and run
# in Windows Terminal (supports ANSI/UTF-8) — no other DLLs required beyond
# stock Windows system DLLs.
#
# Each ZIP contains only the self-contained executable. Users can run it
# directly without a launcher script or additional runtime dependencies.
#
# Prerequisites (Ubuntu/Debian):
#   sudo apt install g++-mingw-w64-x86-64 ninja-build zip
#
# Usage:
#   ./deploy_windows.sh              # cross-compile + create zips in deploy/
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TOOLS_DIR="$SCRIPT_DIR"
APP_NAMES=("psb_demo_tui" "psb_demo_cli" "psb_factory_tui")
VERSION="$(git -C "$SCRIPT_DIR" describe --tags --always --dirty 2>/dev/null || echo "dev")"
ARCH="win-x86_64"

BUILD_DIR="${TOOLS_DIR}/build/mingw-release"
BIN_DIR="${TOOLS_DIR}/bin"
DEPLOY_DIR="${SCRIPT_DIR}/deploy"

echo "=== PSB CLI/TUI tools — Windows cross-compile package ==="
echo "    Version : $VERSION"
echo "    Build   : $BUILD_DIR"
echo "    Output  : $DEPLOY_DIR"
echo ""

if ! command -v x86_64-w64-mingw32-g++ &>/dev/null; then
    echo "ERROR: MinGW cross-compiler not found."
    echo "Install: sudo apt install g++-mingw-w64-x86-64"
    exit 1
fi

echo "[1/3] Cross-compiling ${APP_NAMES[*]} (static)..."
cmake --preset mingw-release -S "$TOOLS_DIR"
cmake --build "$BUILD_DIR" --target "${APP_NAMES[@]}"

echo ""
echo "[2/3] Verifying binaries..."
for APP_NAME in "${APP_NAMES[@]}"; do
    BINARY="${BIN_DIR}/${APP_NAME}.exe"
    if [ ! -f "$BINARY" ]; then
        echo "ERROR: $BINARY not found after build"
        exit 1
    fi
    echo "    + ${APP_NAME}.exe ($(du -h "$BINARY" | cut -f1))"
done

echo ""
echo "[3/3] Creating single-executable zips..."
mkdir -p "$DEPLOY_DIR"
for APP_NAME in "${APP_NAMES[@]}"; do
    BINARY="${BIN_DIR}/${APP_NAME}.exe"
    ZIPFILE="${DEPLOY_DIR}/${APP_NAME}-${VERSION}-${ARCH}.zip"
    rm -f "$ZIPFILE"
    zip -j "$ZIPFILE" "$BINARY"
    echo "    Created: $ZIPFILE"
done

echo ""
echo "Done. Packages in: $DEPLOY_DIR"
echo ""
echo "To run on Windows:"
echo "  1. Unzip the .exe you want (e.g. psb_demo_tui-${VERSION}-${ARCH}.zip)"
echo "  2. Double-click the .exe"
