#!/usr/bin/env bash
# deploy_linux.sh — Build and package hvbctrl (CLI) + hvb_tui (TUI) for Linux.
#
# Both binaries are statically linked against ModbusLib (BUILD_SHARED_LIBS=OFF)
# so the package is self-contained: copy to any Linux x86-64 machine and run.
#
# Usage:
#   ./deploy_linux.sh              # build + create tarball in deploy/
#   ./deploy_linux.sh --install    # also install to /usr/local/bin
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
APP_NAME="hvb_modbus_tool"
VERSION="$(git -C "$SCRIPT_DIR" describe --tags --always --dirty 2>/dev/null || echo "dev")"
ARCH="linux-x86_64"

BUILD_DIR="${SCRIPT_DIR}/build/linux-release"
BIN_DIR="${SCRIPT_DIR}/bin"
STAGE_DIR="${SCRIPT_DIR}/deploy/${APP_NAME}-${VERSION}-${ARCH}"
DEPLOY_DIR="${SCRIPT_DIR}/deploy"

INSTALL=false
while [[ $# -gt 0 ]]; do
    case $1 in
        -i|--install) INSTALL=true ;;
        *) echo "Usage: $0 [-i|--install]"; exit 1 ;;
    esac
    shift
done

echo "=== HVB Modbus Tool — Linux package ==="
echo "    Version : $VERSION"
echo "    Build   : $BUILD_DIR"
echo "    Output  : $DEPLOY_DIR"
echo ""

echo "[1/3] Building CLI + TUI (static)..."
cmake --preset linux-release -S "$SCRIPT_DIR"
cmake --build "$BUILD_DIR" --target hvbctrl hvb_tui

echo ""
echo "[2/3] Staging binaries..."
rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR"

for bin in hvbctrl hvb_tui; do
    if [ ! -f "$BIN_DIR/$bin" ]; then
        echo "ERROR: $BIN_DIR/$bin not found after build"
        exit 1
    fi
    cp "$BIN_DIR/$bin" "$STAGE_DIR/"
    echo "    + $bin ($(du -h "$BIN_DIR/$bin" | cut -f1))"
done

# Verify truly static (no private-rpath deps)
for bin in hvbctrl hvb_tui; do
    if ldd "$STAGE_DIR/$bin" 2>/dev/null | grep -v 'linux-vdso\|ld-linux\|libstdc\|libm\|libgcc\|libc' | grep '=>' ; then
        echo "WARNING: $bin has unexpected shared library dependencies (listed above)"
    fi
done

echo ""
echo "[3/3] Creating tarball..."
mkdir -p "$DEPLOY_DIR"
TARBALL="${DEPLOY_DIR}/${APP_NAME}-${VERSION}-${ARCH}.tar.gz"
tar -czf "$TARBALL" -C "$DEPLOY_DIR" "$(basename "$STAGE_DIR")"
echo "    Created: $TARBALL"

if $INSTALL; then
    echo ""
    echo "[+] Installing to /usr/local/bin..."
    sudo cp "$STAGE_DIR/hvbctrl" /usr/local/bin/
    sudo cp "$STAGE_DIR/hvb_tui"  /usr/local/bin/
    echo "    Installed hvbctrl and hvb_tui"
fi

echo ""
echo "Done. Package: $TARBALL"
echo "Contents:"
tar -tzf "$TARBALL" | sed 's/^/    /'
