# jw_hvb Channel-Count DTS Overlays Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make jw_hvb's channel count a real, explicit build-time
devicetree parameter (1ch / 2ch SKUs) instead of an unrealized assumption,
per the version management contract's §5.

**Architecture:** `jw_hvb.dts` stays the 2-channel base (unchanged — it
already matches the fully-populated SKU). A new `jw_hvb_1ch.overlay`
disables the HV2-side DAC, ADC, and logical vc-controller channel for the
1-channel BOM population, applied via `-DEXTRA_DTC_OVERLAY_FILE=` at build
time. No new board name, no Kconfig, no register changes — channel count
is pure DTS data, consistent with `VC_MAX_CHANNELS =
DT_CHILD_NUM_STATUS_OKAY(vc_controller)` already deriving from enabled
channel nodes.

**Tech Stack:** Zephyr 3.7.2 devicetree overlays, `arm-zephyr-eabi` cross
build (`ZEPHYR_TOOLCHAIN_VARIANT=zephyr`).

## Global Constraints

- Design authority: `docs/superpowers/specs/2026-07-19-version-management-contract-design.md`
  §5 (channel count as build parameter, not identity dimension).
- **Real-hardware finding that narrows this plan's scope below what §5
  originally sketched**: `ref/jw_hvb/board_design.md` §10 documents a real
  `SYS_MOD0`/`SYS_MOD1` DIP switch with a 1ch/2ch/4ch mode table, but
  `board_design.md` and `ref/jw_hvb/pin_map.md` only give real net/pin
  assignments for HV1 and HV2 (dedicated SPI bus + 7-GPIO ADS1232 bit-bang
  set + isolators U5/U6, U18/U19 each). **No HV3/HV4 SPI bus, ADC GPIO
  set, or isolator part numbers are documented anywhere in this repo.**
  Writing `channel@2`/`channel@3` DTS nodes would mean fabricating pin
  assignments with no schematic backing — this plan explicitly does not do
  that. Only the 1-channel overlay (removing HV2, for which real wiring is
  fully documented) is implemented. A 4-channel overlay is future work,
  blocked on real HV3/HV4 schematic data — see Task 3.
- Zephyr instantiation macros already used by the touched drivers
  (`DT_INST_FOREACH_STATUS_OKAY` in both `drivers/sensor/ads1232/ads1232.c`
  and `drivers/dac/ad5541/ad5541.c`) mean a `status = "disabled"` node is
  fully excluded from build/init — confirmed live, not assumed (see Task 1
  verification).
- `lib/voltage_control/vc_runtime.c`'s `VC_ASSERT_CONTIGUOUS_CHANNEL` (via
  `DT_FOREACH_CHILD_STATUS_OKAY`) requires enabled channel `reg` values to
  be contiguous from 0. Disabling `channel@1` while keeping `channel@0`
  enabled satisfies this (single channel at reg 0) — disabling
  `channel@0` instead would not, so the 1ch overlay must disable HV2, not
  HV1.
- Build/test commands use the ARM cross build directly (no `west` binary in
  this environment) — activate the venv first for `pykwalify`:
  `source /home/yong/backup/src/xlab/jianwei/hvb_wkspc/.venv/bin/activate`.

---

## Task 1: Add jw_hvb_1ch.overlay and verify it disables HV2 correctly

**Files:**
- Create: `boards/jianwei/jw_hvb/jw_hvb_1ch.overlay`

**Interfaces:**
- Produces: a devicetree overlay applied via
  `-DEXTRA_DTC_OVERLAY_FILE=boards/jianwei/jw_hvb/jw_hvb_1ch.overlay`
  (absolute or relative path). No C/Kconfig interface — pure DTS.

- [ ] **Step 1: Create the overlay**

```c
/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 1-channel BOM population of jw_hvb: only HV1 is populated, HV2's DAC/ADC
 * and the vc-controller channel bound to it are unused. See
 * docs/superpowers/specs/2026-07-19-version-management-contract-design.md
 * §5 — channel count is a build-time DTS parameter, not a board variant or
 * hardware revision, since the DAC/ADC/isolator architecture is identical
 * to the 2-channel base config; only how many of those identical circuits
 * are populated differs.
 */

&ads1232_hv2 {
	status = "disabled";
};

&dac_hv2 {
	status = "disabled";
};

&vc_ch1 {
	status = "disabled";
};
```

- [ ] **Step 2: Configure and build a 1ch target, confirm it builds clean**

```sh
source /home/yong/backup/src/xlab/jianwei/hvb_wkspc/.venv/bin/activate
ZEPHYR_BASE=/home/yong/backup/src/xlab/jianwei/hvb_wkspc/zephyr \
ZEPHYR_TOOLCHAIN_VARIANT=zephyr \
ZEPHYR_SDK_INSTALL_DIR=/home/yong/.local/zephyr-sdk-0.16.4 \
cmake -S applications/psb_controller \
      -B /home/yong/backup/src/xlab/jianwei/hvb_wkspc/build_psb_hvb_1ch \
      -GNinja -DBOARD=jw_hvb \
      "-DEXTRA_DTC_OVERLAY_FILE=$(pwd)/boards/jianwei/jw_hvb/jw_hvb_1ch.overlay"

ninja -C /home/yong/backup/src/xlab/jianwei/hvb_wkspc/build_psb_hvb_1ch
```

Expected: links successfully, memory report printed, FLASH/RAM usage
slightly *smaller* than the 2ch build (fewer driver instances compiled
in) — e.g. 2ch is ~106520 B FLASH / ~20240 B RAM; 1ch should be lower on
both.

- [ ] **Step 3: Confirm VC_MAX_CHANNELS actually resolves to 1**

```sh
grep "CHILD_NUM_STATUS_OKAY" \
  /home/yong/backup/src/xlab/jianwei/hvb_wkspc/build_psb_hvb_1ch/zephyr/include/generated/zephyr/devicetree_generated.h \
  | grep vc_controller
```

Expected output includes exactly:
```
#define DT_N_S_vc_controller_CHILD_NUM_STATUS_OKAY 1
```
(vs `2` in a build without the overlay — `VC_MAX_CHANNELS` is
`DT_CHILD_NUM_STATUS_OKAY(vc_controller)`, so this is the direct proof the
overlay changed the channel count the firmware sees.)

- [ ] **Step 4: Confirm the merged DTS shows vc_ch1 disabled, vc_ch0 untouched**

```sh
grep -A9 "vc_ch1: channel@1" \
  /home/yong/backup/src/xlab/jianwei/hvb_wkspc/build_psb_hvb_1ch/zephyr/zephyr.dts
```

Expected: the `channel@1` node block ends with `status = "disabled";`.
`channel@0` (grep the same file for `vc_ch0: channel@0`) must show no
`status` override (implicitly `"okay"`).

- [ ] **Step 5: Confirm the base (no-overlay) 2ch build is unaffected**

```sh
ninja -C /home/yong/backup/src/xlab/jianwei/hvb_wkspc/build_psb_hvb
```

Expected: builds clean, memory usage unchanged from before this plan
(FLASH ~106520 B, RAM ~20240 B, board: jw_hvb) — the overlay file existing
in the board directory must not affect a build that doesn't reference it.

- [ ] **Step 6: Commit**

```bash
git add boards/jianwei/jw_hvb/jw_hvb_1ch.overlay
git commit -m "feat(jw_hvb): add 1-channel BOM-population DTS overlay"
```

---

## Task 2: Document the overlay build command and the channel-count model

**Files:**
- Modify: `README.md`

**Interfaces:** none — documentation only.

- [ ] **Step 1: Add a "Board SKU / channel-count overlays" subsection to README.md**

Insert after the existing "Use a separate build directory..." block (the
last paragraph in the `## Builds` section):

```markdown

### Board SKU / channel-count overlays

Some boards ship in multiple channel-count SKUs from the same DTS base —
e.g. jw_hvb's 2-channel config is the default; a 1-channel BOM population
uses an explicit overlay:

```sh
west build -b jw_hvb applications/psb_controller \
    -- -DEXTRA_DTC_OVERLAY_FILE=boards/jianwei/jw_hvb/jw_hvb_1ch.overlay
```

Channel count is a devicetree build parameter, not a board variant or
hardware revision — see
`docs/superpowers/specs/2026-07-19-version-management-contract-design.md`
§5 for the full rationale and the mechanical test for when a hardware
change needs a new overlay vs. a new board name vs. a new
`board.yml` revision.
```

- [ ] **Step 2: Verify the doc renders sensibly (visual check, no build step)**

```sh
sed -n '/Board SKU/,/§5 for the full/p' README.md
```

Expected: the inserted block reads correctly, code fence closes properly,
no stray markdown.

- [ ] **Step 3: Commit**

```bash
git add README.md
git commit -m "docs: document jw_hvb channel-count overlay build command"
```

---

## Task 3: Record the deferred 4-channel overlay as a tracked gap

**Files:**
- Modify: `docs/superpowers/specs/2026-07-19-version-management-contract-design.md`

**Interfaces:** none — documentation only.

- [ ] **Step 1: Add a short accuracy note to §5**

Append immediately after §5's existing final paragraph (the one ending
"...the same idiomatic mechanism Zephyr uses for any optional-hardware-
population case."):

```markdown

**2026-07-19 implementation note**: `jw_hvb_1ch.overlay` was implemented
(disables HV2's DAC/ADC/logical channel — full pin data was already
documented for HV1/HV2 in `ref/jw_hvb/pin_map.md` and
`ref/jw_hvb/board_design.md`). A `jw_hvb_4ch.overlay` was **not**
implemented: `board_design.md` §10 documents a real `SYS_MOD0`/`SYS_MOD1`
DIP switch with a 4-channel mode entry, but no HV3/HV4 SPI bus, ADS1232
GPIO set, or isolator part numbers are documented anywhere in this repo.
Writing `channel@2`/`channel@3` DTS nodes without that schematic data
would mean fabricating pin assignments. Blocked on real hardware
documentation for HV3/HV4 before a 4ch overlay can be written correctly.
```

- [ ] **Step 2: Commit**

```bash
git add docs/superpowers/specs/2026-07-19-version-management-contract-design.md
git commit -m "docs: record jw_hvb 4-channel overlay as blocked on missing HV3/HV4 schematic data"
```

---

## Task 4: Full verification

**Files:** none (verification only).

- [ ] **Step 1: Rebuild both board configurations one more time from a clean state to confirm no drift**

```sh
source /home/yong/backup/src/xlab/jianwei/hvb_wkspc/.venv/bin/activate
ninja -C /home/yong/backup/src/xlab/jianwei/hvb_wkspc/build_psb_hvb
ninja -C /home/yong/backup/src/xlab/jianwei/hvb_wkspc/build_psb_hvb_1ch
```

Expected: both link successfully with a memory report; 1ch strictly lower
FLASH/RAM usage than 2ch.

- [ ] **Step 2: Run the existing native regression suite (unaffected by DTS-only changes, but confirms no accidental collateral damage)**

```sh
for d in build_reg_store build_ss_shell build_modbus_adapter_test; do
	ninja -C /home/yong/backup/src/xlab/jianwei/hvb_wkspc/$d
	/home/yong/backup/src/xlab/jianwei/hvb_wkspc/$d/zephyr/zephyr.exe
done
```

Expected: all three suites still pass 100% (12/12, 4/4, 22/22) — these
tests build against `native_posix`, not `jw_hvb`, so they're unaffected by
a jw_hvb-only overlay; this step is a pure regression guard.

- [ ] **Step 3: No further commit needed**

This task is verification-only.
