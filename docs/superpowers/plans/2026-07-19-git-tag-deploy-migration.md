# Git-Tag / deploy_linux.sh Migration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `tools/deploy_linux.sh` and `tools/deploy_windows.sh`
resolve each packaged tool's version from its own `<tool-name>-vX.Y.Z` git
tag, instead of one repo-wide `git describe` shared across all three
tools — closing the gap identified in the version management contract §7.

**Architecture:** Both scripts currently compute a single `VERSION`
variable via `git describe --tags --always --dirty` (repo-wide, whatever
tag is most recent regardless of relevance) and reuse it for every
packaged tool's tarball/zip name. Replace this with a `resolve_version()`
helper called once per tool name (`git describe --tags --match
"<name>-v*" --always --dirty`), stored in a `VERSIONS` associative array,
and used everywhere the old shared `$VERSION` was referenced. No tags are
cut by this plan — the `--always` fallback to an abbreviated commit hash
is preserved exactly as before when a tool has no matching tag yet.

**Tech Stack:** Bash (`set -euo pipefail`, `declare -A` associative
arrays — requires bash 4+, confirmed available: this environment runs
bash 5.2.21).

## Global Constraints

- Design authority: `docs/superpowers/specs/2026-07-19-version-management-contract-design.md`
  §7 (`<tool-name>-vX.Y.Z` tag family, one per host-tools deliverable).
- Both `deploy_linux.sh` and `deploy_windows.sh` have the exact same
  single-shared-`VERSION` bug (same `APP_NAMES` array, same `git describe`
  line) — this plan fixes both scripts identically, not just the one named
  in casual conversation, since leaving one fixed and one broken would be
  a worse inconsistency than doing neither.
- This plan does not touch `tools/psb_modbus_core` versioning
  independently — it's a static library linked into the three packaged
  binaries, never distributed standalone by either script, so it has no
  `resolve_version()` call of its own here.
- This plan does not cut any git tags. `git describe --match` against a
  nonexistent tag pattern is not an error — it falls through to `--always`
  exactly like the current unscoped call does, confirmed live (Task 1).
- Build directories (`tools/build/linux-release`, `tools/build/mingw-release`)
  already exist in this environment from prior sessions, so the build
  steps below are incremental, not full rebuilds.

---

## Task 1: Fix deploy_linux.sh version resolution

**Files:**
- Modify: `tools/deploy_linux.sh`

**Interfaces:**
- Produces: a `resolve_version()` bash function and a `VERSIONS`
  associative array keyed by tool name — Task 2 (`deploy_windows.sh`)
  uses the identical pattern, not this file's array directly (each script
  is self-contained).

- [ ] **Step 1: Replace the single VERSION line with per-tool resolution**

Change (near the top of the file, right after `APP_NAMES=`):

```sh
APP_NAMES=("psb_demo_tui" "psb_demo_cli" "psb_factory_tui")
VERSION="$(git -C "$SCRIPT_DIR" describe --tags --always --dirty 2>/dev/null || echo "dev")"
ARCH="linux-x86_64"

BUILD_DIR="${TOOLS_DIR}/build/linux-release"
BIN_DIR="${TOOLS_DIR}/bin"
DEPLOY_DIR="${SCRIPT_DIR}/deploy"
```

to:

```sh
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
```

- [ ] **Step 2: Update the header echo block to show a version per tool**

Change:

```sh
echo "=== PSB CLI/TUI tools — Linux package ==="
echo "    Version : $VERSION"
echo "    Build   : $BUILD_DIR"
```

to:

```sh
echo "=== PSB CLI/TUI tools — Linux package ==="
for APP_NAME in "${APP_NAMES[@]}"; do
    echo "    Version (${APP_NAME}) : ${VERSIONS[$APP_NAME]}"
done
echo "    Build   : $BUILD_DIR"
```

- [ ] **Step 3: Update the staging loop's STAGE_DIR to use the per-tool version**

Change:

```sh
    STAGE_DIR="${DEPLOY_DIR}/${APP_NAME}-${VERSION}-${ARCH}"
    rm -rf "$STAGE_DIR"
```

to:

```sh
    STAGE_DIR="${DEPLOY_DIR}/${APP_NAME}-${VERSIONS[$APP_NAME]}-${ARCH}"
    rm -rf "$STAGE_DIR"
```

- [ ] **Step 4: Update the tarball-creation loop**

Change:

```sh
    STAGE_DIR="${DEPLOY_DIR}/${APP_NAME}-${VERSION}-${ARCH}"
    TARBALL="${DEPLOY_DIR}/${APP_NAME}-${VERSION}-${ARCH}.tar.gz"
    tar -czf "$TARBALL" -C "$DEPLOY_DIR" "$(basename "$STAGE_DIR")"
```

to:

```sh
    STAGE_DIR="${DEPLOY_DIR}/${APP_NAME}-${VERSIONS[$APP_NAME]}-${ARCH}"
    TARBALL="${DEPLOY_DIR}/${APP_NAME}-${VERSIONS[$APP_NAME]}-${ARCH}.tar.gz"
    tar -czf "$TARBALL" -C "$DEPLOY_DIR" "$(basename "$STAGE_DIR")"
```

- [ ] **Step 5: Update the final summary loop**

Change:

```sh
for APP_NAME in "${APP_NAMES[@]}"; do
    TARBALL="${DEPLOY_DIR}/${APP_NAME}-${VERSION}-${ARCH}.tar.gz"
    echo "  ${TARBALL}:"
```

to:

```sh
for APP_NAME in "${APP_NAMES[@]}"; do
    TARBALL="${DEPLOY_DIR}/${APP_NAME}-${VERSIONS[$APP_NAME]}-${ARCH}.tar.gz"
    echo "  ${TARBALL}:"
```

- [ ] **Step 6: Run the script end to end and verify per-tool tarball names**

```sh
cd tools
bash -n deploy_linux.sh   # syntax check
./deploy_linux.sh
```

Expected: builds all three tools, then output shows one tarball per tool
named `<tool>-<resolved-version>-linux-x86_64.tar.gz`. With no matching
tags in the repo yet, all three currently resolve to the same abbreviated
commit hash (e.g. `f850d3a-dirty` if the tree has uncommitted changes) —
that's expected; the mechanism, not today's specific output, is what this
step verifies.

- [ ] **Step 7: Prove per-tool differentiation with a throwaway tag**

```sh
cd /path/to/hvb_firmware.git
git tag -a psb_demo_tui-v1.0.0 -m "test tag for validation, will be deleted"
git describe --tags --match "psb_demo_tui-v*" --always --dirty
git describe --tags --match "psb_demo_cli-v*" --always --dirty
git tag -d psb_demo_tui-v1.0.0
```

Expected: the first `describe` shows `psb_demo_tui-v1.0.0[-dirty]`; the
second still shows the abbreviated-commit-hash fallback (no matching tag
for `psb_demo_cli`) — proof the two tools resolve independently. Delete
the tag immediately after — this step never leaves a tag behind.

- [ ] **Step 8: Clean up generated deploy artifacts from Step 6**

```sh
rm -rf tools/deploy
```

(`tools/deploy/` is build output, not committed — confirm with
`git status --short tools/` that only `deploy_linux.sh` shows as modified
before committing.)

- [ ] **Step 9: Commit**

```bash
git add tools/deploy_linux.sh
git commit -m "fix(deploy): resolve each packaged tool's version from its own git tag"
```

---

## Task 2: Fix deploy_windows.sh version resolution (same bug, same fix)

**Files:**
- Modify: `tools/deploy_windows.sh`

**Interfaces:** same `resolve_version()` / `VERSIONS` pattern as Task 1,
independently duplicated in this script (each deploy script is
self-contained, no shared library file between them today).

- [ ] **Step 1: Replace the single VERSION line with per-tool resolution and update the header echo**

Change:

```sh
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
```

to:

```sh
APP_NAMES=("psb_demo_tui" "psb_demo_cli" "psb_factory_tui")
ARCH="win-x86_64"

BUILD_DIR="${TOOLS_DIR}/build/mingw-release"
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

echo "=== PSB CLI/TUI tools — Windows cross-compile package ==="
for APP_NAME in "${APP_NAMES[@]}"; do
    echo "    Version (${APP_NAME}) : ${VERSIONS[$APP_NAME]}"
done
echo "    Build   : $BUILD_DIR"
echo "    Output  : $DEPLOY_DIR"
echo ""
```

- [ ] **Step 2: Update the zip-creation loop**

Change:

```sh
    BINARY="${BIN_DIR}/${APP_NAME}.exe"
    ZIPFILE="${DEPLOY_DIR}/${APP_NAME}-${VERSION}-${ARCH}.zip"
    rm -f "$ZIPFILE"
```

to:

```sh
    BINARY="${BIN_DIR}/${APP_NAME}.exe"
    ZIPFILE="${DEPLOY_DIR}/${APP_NAME}-${VERSIONS[$APP_NAME]}-${ARCH}.zip"
    rm -f "$ZIPFILE"
```

- [ ] **Step 3: Update the trailing usage-example echo**

Change:

```sh
echo "  1. Unzip the .exe you want (e.g. psb_demo_tui-${VERSION}-${ARCH}.zip)"
```

to:

```sh
echo "  1. Unzip the .exe you want (e.g. psb_demo_tui-${VERSIONS[psb_demo_tui]}-${ARCH}.zip)"
```

- [ ] **Step 4: Run the script end to end**

```sh
cd tools
bash -n deploy_windows.sh   # syntax check
./deploy_windows.sh
```

Expected: cross-compiles all three tools with MinGW, then output shows
one zip per tool named `<tool>-<resolved-version>-win-x86_64.zip`. Skip
this step (document why) if `x86_64-w64-mingw32-g++` isn't installed in
your environment — the script itself already checks for this and exits
with an install hint if missing.

- [ ] **Step 5: Clean up generated deploy artifacts**

```sh
rm -rf tools/deploy
```

- [ ] **Step 6: Commit**

```bash
git add tools/deploy_windows.sh
git commit -m "fix(deploy): resolve each packaged tool's version from its own git tag (Windows)"
```

---

## Task 3: Full verification

**Files:** none (verification only).

- [ ] **Step 1: Confirm no stray deploy artifacts remain tracked**

```sh
git status --short tools/
```

Expected: clean (no `tools/deploy/` entries — that directory is build
output and was removed at the end of Tasks 1 and 2).

- [ ] **Step 2: Confirm both scripts pass shellcheck-equivalent sanity (bash -n) one more time post-commit**

```sh
bash -n tools/deploy_linux.sh
bash -n tools/deploy_windows.sh
```

Expected: both exit 0, no output.

- [ ] **Step 3: No further commit needed**

This task is verification-only.
