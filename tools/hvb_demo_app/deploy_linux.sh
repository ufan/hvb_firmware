#!/usr/bin/env bash
# deploy_linux.sh — Build and package hvb_tui (TUI) for Linux.
#
# Usage:
#   ./deploy_linux.sh              # build + create tarball in deploy/
#   ./deploy_linux.sh --install    # also install to /usr/local/bin
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TOOLS_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
APP_NAME="hvb_tui"
VERSION="$(git -C "$SCRIPT_DIR" describe --tags --always --dirty 2>/dev/null || echo "dev")"
ARCH="linux-x86_64"

BUILD_DIR="${TOOLS_DIR}/build/linux-release"
BIN_DIR="${TOOLS_DIR}/bin"
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

echo "=== HVB TUI — Linux package ==="
echo "    Version : $VERSION"
echo "    Build   : $BUILD_DIR"
echo "    Output  : $DEPLOY_DIR"
echo ""

echo "[1/3] Building TUI..."
cmake --preset linux-release -S "$TOOLS_DIR"
cmake --build "$BUILD_DIR" --target "$APP_NAME"

echo ""
echo "[2/3] Staging binary..."
rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR"

if [ ! -f "$BIN_DIR/$APP_NAME" ]; then
    echo "ERROR: $BIN_DIR/$APP_NAME not found after build"
    exit 1
fi
cp "$BIN_DIR/$APP_NAME" "$STAGE_DIR/"
echo "    + $APP_NAME ($(du -h "$BIN_DIR/$APP_NAME" | cut -f1))"

if ldd "$STAGE_DIR/$APP_NAME" 2>/dev/null | grep -v 'linux-vdso\|ld-linux\|libstdc\|libm\|libgcc\|libc' | grep '=>' ; then
    echo "WARNING: $APP_NAME has unexpected shared library dependencies (listed above)"
fi

echo ""
echo "[3/3] Creating tarball..."
mkdir -p "$DEPLOY_DIR"
TARBALL="${DEPLOY_DIR}/${APP_NAME}-${VERSION}-${ARCH}.tar.gz"
tar -czf "$TARBALL" -C "$DEPLOY_DIR" "$(basename "$STAGE_DIR")"
echo "    Created: $TARBALL"

if $INSTALL; then
    echo ""
    echo "[+] Installing to /usr/local/bin..."
    sudo cp "$STAGE_DIR/$APP_NAME" /usr/local/bin/
    echo "    Installed $APP_NAME"
fi

echo ""
echo "Done. Package: $TARBALL"
echo "Contents:"
tar -tzf "$TARBALL" | sed 's/^/    /'
