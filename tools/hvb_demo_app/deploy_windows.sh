#!/usr/bin/env bash
# deploy_windows.sh — Cross-compile and package hvb_tui for Windows (MinGW).
#
# Uses MinGW-w64 cross-compiler on Linux to produce a statically-linked .exe.
# The resulting binary is self-contained: copy to any Windows 10/11 machine
# and run in Windows Terminal (supports ANSI/UTF-8).
#
# Includes a .bat launcher so users can double-click to start after editing
# their COM port and baud rate at the top of the file.
#
# Prerequisites (Ubuntu/Debian):
#   sudo apt install g++-mingw-w64-x86-64 ninja-build zip
#
# Usage:
#   ./deploy_windows.sh              # cross-compile + create zip in deploy/
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TOOLS_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
APP_NAME="hvb_tui"
VERSION="$(git -C "$SCRIPT_DIR" describe --tags --always --dirty 2>/dev/null || echo "dev")"
ARCH="win-x86_64"

BUILD_DIR="${TOOLS_DIR}/build/mingw-release"
BIN_DIR="${TOOLS_DIR}/bin"
DEPLOY_DIR="${SCRIPT_DIR}/deploy"
STAGE_DIR="${DEPLOY_DIR}/${APP_NAME}-${VERSION}-${ARCH}"

echo "=== HVB TUI — Windows cross-compile package ==="
echo "    Version : $VERSION"
echo "    Build   : $BUILD_DIR"
echo "    Output  : $DEPLOY_DIR"
echo ""

if ! command -v x86_64-w64-mingw32-g++ &>/dev/null; then
    echo "ERROR: MinGW cross-compiler not found."
    echo "Install: sudo apt install g++-mingw-w64-x86-64"
    exit 1
fi

echo "[1/4] Cross-compiling TUI (static)..."
cmake --preset mingw-release -S "$TOOLS_DIR"
cmake --build "$BUILD_DIR" --target "$APP_NAME"

echo ""
echo "[2/4] Staging binary..."
rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR"

BINARY="${BIN_DIR}/${APP_NAME}.exe"
if [ ! -f "$BINARY" ]; then
    echo "ERROR: $BINARY not found after build"
    exit 1
fi
cp "$BINARY" "$STAGE_DIR/"
echo "    + ${APP_NAME}.exe ($(du -h "$BINARY" | cut -f1))"

echo ""
echo "[3/4] Creating .bat launcher..."
cat > "$STAGE_DIR/${APP_NAME}.bat" << 'BATEOF'
@echo off
setlocal
title HVB TUI
hvb_tui.exe
if errorlevel 1 (
    echo hvb_tui.exe exited with error (code %errorlevel%).
    pause
)
endlocal
BATEOF
echo "    + ${APP_NAME}.bat"

echo ""
echo "[4/4] Creating zip..."
mkdir -p "$DEPLOY_DIR"
ZIPFILE="${DEPLOY_DIR}/${APP_NAME}-${VERSION}-${ARCH}.zip"
(cd "$DEPLOY_DIR" && zip -r "$(basename "$ZIPFILE")" "$(basename "$STAGE_DIR")")
echo "    Created: $ZIPFILE"

echo ""
echo "Done. Package: $ZIPFILE"
echo ""
echo "To run on Windows:"
echo "  1. Unzip ${APP_NAME}-${VERSION}-${ARCH}.zip"
echo "  2. Double-click ${APP_NAME}.bat"
