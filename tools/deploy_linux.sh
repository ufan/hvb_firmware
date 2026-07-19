#!/usr/bin/env bash
# deploy_linux.sh — Build and package psb_demo_tui, psb_demo_cli, and
# psb_factory_tui for Linux.
#
# Usage:
#   ./deploy_linux.sh              # build + create tarballs in deploy/
#   ./deploy_linux.sh --install    # also install to /usr/local/bin
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TOOLS_DIR="$SCRIPT_DIR"
APP_NAMES=("psb_demo_tui" "psb_demo_cli" "psb_factory_tui")
ARCH="linux-x86_64"

BUILD_DIR="${TOOLS_DIR}/build/linux-release"
BIN_DIR="${TOOLS_DIR}/bin"
DEPLOY_DIR="${SCRIPT_DIR}/deploy"

# Each host tool is released independently under its own <tool-name>-vX.Y.Z
# tag (see docs/superpowers/specs/2026-07-19-version-management-contract-
# design.md §7) — a shared repo-wide tag would conflate unrelated tools'
# release cadence. --always falls back to the abbreviated commit hash when
# that tool has no matching tag reachable, same fallback behavior as before.
resolve_version() {
    git -C "$SCRIPT_DIR" describe --tags --match "$1-v*" --always --dirty 2>/dev/null || echo "dev"
}
declare -A VERSIONS
for APP_NAME in "${APP_NAMES[@]}"; do
    VERSIONS[$APP_NAME]="$(resolve_version "$APP_NAME")"
done

INSTALL=false
while [[ $# -gt 0 ]]; do
    case $1 in
        -i|--install) INSTALL=true ;;
        *) echo "Usage: $0 [-i|--install]"; exit 1 ;;
    esac
    shift
done

echo "=== PSB CLI/TUI tools — Linux package ==="
for APP_NAME in "${APP_NAMES[@]}"; do
    echo "    Version (${APP_NAME}) : ${VERSIONS[$APP_NAME]}"
done
echo "    Build   : $BUILD_DIR"
echo "    Output  : $DEPLOY_DIR"
echo ""

echo "[1/3] Building ${APP_NAMES[*]}..."
cmake --preset linux-release -S "$TOOLS_DIR"
cmake --build "$BUILD_DIR" --target "${APP_NAMES[@]}"

echo ""
echo "[2/3] Staging binaries..."
mkdir -p "$DEPLOY_DIR"
for APP_NAME in "${APP_NAMES[@]}"; do
    if [ ! -f "$BIN_DIR/$APP_NAME" ]; then
        echo "ERROR: $BIN_DIR/$APP_NAME not found after build"
        exit 1
    fi

    STAGE_DIR="${DEPLOY_DIR}/${APP_NAME}-${VERSIONS[$APP_NAME]}-${ARCH}"
    rm -rf "$STAGE_DIR"
    mkdir -p "$STAGE_DIR"
    cp "$BIN_DIR/$APP_NAME" "$STAGE_DIR/"
    echo "    + $APP_NAME ($(du -h "$BIN_DIR/$APP_NAME" | cut -f1))"

    if ldd "$STAGE_DIR/$APP_NAME" 2>/dev/null | grep -v 'linux-vdso\|ld-linux\|libstdc\|libm\|libgcc\|libc' | grep '=>' ; then
        echo "WARNING: $APP_NAME has unexpected shared library dependencies (listed above)"
    fi
done

echo ""
echo "[3/3] Creating tarballs..."
for APP_NAME in "${APP_NAMES[@]}"; do
    STAGE_DIR="${DEPLOY_DIR}/${APP_NAME}-${VERSIONS[$APP_NAME]}-${ARCH}"
    TARBALL="${DEPLOY_DIR}/${APP_NAME}-${VERSIONS[$APP_NAME]}-${ARCH}.tar.gz"
    tar -czf "$TARBALL" -C "$DEPLOY_DIR" "$(basename "$STAGE_DIR")"
    echo "    Created: $TARBALL"
done

if $INSTALL; then
    echo ""
    echo "[+] Installing to /usr/local/bin..."
    for APP_NAME in "${APP_NAMES[@]}"; do
        sudo cp "$BIN_DIR/$APP_NAME" /usr/local/bin/
        echo "    Installed $APP_NAME"
    done
fi

echo ""
echo "Done. Packages in: $DEPLOY_DIR"
for APP_NAME in "${APP_NAMES[@]}"; do
    TARBALL="${DEPLOY_DIR}/${APP_NAME}-${VERSIONS[$APP_NAME]}-${ARCH}.tar.gz"
    echo "  ${TARBALL}:"
    tar -tzf "$TARBALL" | sed 's/^/    /'
done
