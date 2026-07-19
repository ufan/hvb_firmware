# Firmware Version & Register Plumbing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Wire the firmware-side pieces of the version management contract:
a real SemVer `FW_VERSION` register (currently a hardcoded `0.1` stub)
derived from a `firmware-vX.Y.Z` git tag at build time, and a new
`BOARD_HW_REVISION` register reporting the Zephyr `board.yml` hardware
revision within a board variant.

**Architecture:** A new CMake include (`cmake/fw_version.cmake`) runs
`git describe --match "firmware-v*"` at configure time and generates a
header (`include/generated/fw_version.h`) with `FW_VERSION_MAJOR/MINOR/
PATCH` macros, falling back to `0.0.0` when untagged. `lib/sys_status/
sys_uptime.c` packs those into the existing `FW_VERSION` register
(major:8|minor:8|patch:16). `BOARD_HW_REVISION` is added as a new
`VC_GLOBAL_REG` following the exact same pattern as the existing
`VARIANT_ID`/`CURRENT_UNIT_EXP` registers, sourced from a new
`CONFIG_VC_BOARD_HW_REVISION` Kconfig int (default 0, since no board in
this tree declares `board.yml` revisions yet). Adding the new register to
a previously-reserved wire offset is a protocol MINOR bump (3.2 → 3.3),
consistent with the existing additive-only convention documented in
`modbus_view.def`.

**Tech Stack:** Zephyr 3.7.2 (T2 topology), CMake 3.20+, C (Zephyr Kconfig
+ devicetree + register-catalog X-macro system), Ztest (native_posix).

## Global Constraints

- Design authority: `docs/superpowers/specs/2026-07-19-version-management-contract-design.md`
  (§2 dimensions table, §3 board identity, §4 wire registers, §6 compat
  rules). Do not deviate from its register/Kconfig naming without updating
  that doc first.
- This plan does **not** cut any git tag, touch `deploy_linux.sh`, touch any
  `boards/jianwei/*` files, or add real `board.yml` revisions — those are
  separate plans per the design doc's four-way split.
- `FW_VERSION` packing is major:8 bits (24-31) | minor:8 bits (16-23) |
  patch:16 bits (0-15) inside the existing 32-bit register. This exact
  layout was chosen during this plan's authoring and must be used
  consistently in every task below — don't invent a different split.
- Every new/changed register follows the existing `VC_GLOBAL_REG` /
  `MODBUS_SYS16` X-macro pattern already used by `VARIANT_ID` and
  `CURRENT_UNIT_EXP` — do not hand-roll a different registration
  mechanism.
- Build/test commands below use pre-configured build directories at
  `/home/yong/backup/src/xlab/jianwei/hvb_wkspc/build_*` (sibling to this
  repo checkout, `hvb_firmware.git/`). If a referenced build directory is
  missing in your environment, configure it first — commands to do so are
  included in each task.
- The Python venv at `/home/yong/backup/src/xlab/jianwei/hvb_wkspc/.venv`
  must be active (`source /home/yong/backup/src/xlab/jianwei/hvb_wkspc/.venv/bin/activate`)
  before any `cmake`/`ninja` invocation below — the system Python lacks
  `pykwalify`, which Zephyr's board-discovery script requires, and CMake
  configure fails without it.

---

## Task 1: Build-time FW_VERSION generation from git tag

**Files:**
- Create: `cmake/fw_version.cmake`
- Create: `cmake/fw_version.h.in`
- Modify: `CMakeLists.txt` (repo root)

**Interfaces:**
- Produces: a generated header `include/generated/fw_version.h` (under the
  active build directory) defining `FW_VERSION_MAJOR`, `FW_VERSION_MINOR`,
  `FW_VERSION_PATCH` (all plain integer macros), visible to every Zephyr
  module library via `zephyr_include_directories()`. Task 2 consumes these
  three macro names directly.

- [ ] **Step 1: Create `cmake/fw_version.cmake`**

```cmake
# SPDX-License-Identifier: Apache-2.0
#
# Generates include/generated/fw_version.h with FW_VERSION_MAJOR/MINOR/PATCH
# derived from the firmware-vX.Y.Z git tag. See docs/superpowers/specs/
# 2026-07-19-version-management-contract-design.md sections 2 and 7.
# Falls back to 0.0.0 when no matching tag is reachable (fresh checkout,
# shallow clone, or git unavailable) so the build never fails on this step.

set(FW_VERSION_MAJOR 0)
set(FW_VERSION_MINOR 0)
set(FW_VERSION_PATCH 0)

find_package(Git QUIET)
if(GIT_FOUND)
	execute_process(
		COMMAND ${GIT_EXECUTABLE} describe --tags --match "firmware-v*" --always
		WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
		OUTPUT_VARIABLE FW_GIT_DESCRIBE
		OUTPUT_STRIP_TRAILING_WHITESPACE
		ERROR_QUIET
		RESULT_VARIABLE FW_GIT_DESCRIBE_RESULT
	)
	if(FW_GIT_DESCRIBE_RESULT EQUAL 0 AND
	   FW_GIT_DESCRIBE MATCHES "^firmware-v([0-9]+)\\.([0-9]+)\\.([0-9]+)")
		set(FW_VERSION_MAJOR ${CMAKE_MATCH_1})
		set(FW_VERSION_MINOR ${CMAKE_MATCH_2})
		set(FW_VERSION_PATCH ${CMAKE_MATCH_3})
	endif()
endif()

set(FW_VERSION_GENERATED_DIR ${CMAKE_BINARY_DIR}/include/generated)
file(MAKE_DIRECTORY ${FW_VERSION_GENERATED_DIR})
configure_file(
	${CMAKE_CURRENT_LIST_DIR}/fw_version.h.in
	${FW_VERSION_GENERATED_DIR}/fw_version.h
	@ONLY
)
zephyr_include_directories(${FW_VERSION_GENERATED_DIR})
```

- [ ] **Step 2: Create `cmake/fw_version.h.in`**

```c
/* SPDX-License-Identifier: Apache-2.0
 *
 * Generated by cmake/fw_version.cmake from the firmware-vX.Y.Z git tag —
 * do not edit directly.
 */
#ifndef FW_VERSION_GENERATED_H
#define FW_VERSION_GENERATED_H

#define FW_VERSION_MAJOR @FW_VERSION_MAJOR@
#define FW_VERSION_MINOR @FW_VERSION_MINOR@
#define FW_VERSION_PATCH @FW_VERSION_PATCH@

#endif /* FW_VERSION_GENERATED_H */
```

- [ ] **Step 3: Wire it into the root CMakeLists.txt**

Modify `CMakeLists.txt` (currently just two `add_subdirectory` calls):

```cmake
# SPDX-License-Identifier: Apache-2.0

include(${CMAKE_CURRENT_LIST_DIR}/cmake/fw_version.cmake)

add_subdirectory(lib)
add_subdirectory(drivers)
```

- [ ] **Step 4: Configure and build a test target, confirm the header is generated correctly**

If `/home/yong/backup/src/xlab/jianwei/hvb_wkspc/build_ss_shell` does not
already exist, configure it:

```sh
cd /path/to/hvb_firmware.git
source /home/yong/backup/src/xlab/jianwei/hvb_wkspc/.venv/bin/activate
ZEPHYR_BASE=/home/yong/backup/src/xlab/jianwei/hvb_wkspc/zephyr \
ZEPHYR_TOOLCHAIN_VARIANT=host \
cmake -S tests/voltage_control/ss_shell \
      -B /home/yong/backup/src/xlab/jianwei/hvb_wkspc/build_ss_shell \
      -GNinja -DBOARD=native_posix
```

Then build:

```sh
ninja -C /home/yong/backup/src/xlab/jianwei/hvb_wkspc/build_ss_shell
```

Expected: build succeeds (this task alone doesn't change any `.c` file, so
this just confirms the CMake generation step itself doesn't break the
configure/build). Inspect the generated header:

```sh
cat /home/yong/backup/src/xlab/jianwei/hvb_wkspc/build_ss_shell/include/generated/fw_version.h
```

Expected output (no `firmware-v*` tag exists in this repo yet, so the
fallback path is exercised):

```c
#define FW_VERSION_MAJOR 0
#define FW_VERSION_MINOR 0
#define FW_VERSION_PATCH 0
```

- [ ] **Step 5: Commit**

```bash
git add cmake/fw_version.cmake cmake/fw_version.h.in CMakeLists.txt
git commit -m "build: generate FW_VERSION_MAJOR/MINOR/PATCH from firmware-vX.Y.Z git tag"
```

---

## Task 2: Wire FW_VERSION register to real SemVer (major.minor.patch)

**Files:**
- Modify: `lib/sys_status/sys_uptime.c`
- Modify: `lib/sys_status/sys_status_shell.c`
- Modify: `tests/voltage_control/ss_shell/src/main.c`

**Interfaces:**
- Consumes: `FW_VERSION_MAJOR`/`FW_VERSION_MINOR`/`FW_VERSION_PATCH` macros
  from Task 1's generated `fw_version.h`.
- Produces: `sys_status_firmware_version_reg` (existing descriptor, value
  now real) — no signature change, same `REG_SYS_STATUS_ID(REG_SYS_STATUS_FIELD_FW_VERSION)`
  read path every caller already uses.

- [ ] **Step 1: Update the failing assertion first — remove the hardcoded stub value from the test**

The current test at `tests/voltage_control/ss_shell/src/main.c:37-43`
asserts the raw register value equals the literal stub `1U`
(`SYS_STATUS_FW_VERSION_HIGH=0, LOW=1` packed as `0<<16|1`). That
assumption is what Task 2 removes, so update the test to stop depending on
it:

```c
ZTEST(ss_shell, test_system_status_is_exposed_through_catalog)
{
	union reg_value value = {};

	/* FW_VERSION is major:8|minor:8|patch:16, sourced from the
	 * firmware-vX.Y.Z git tag at build time (falls back to 0.0.0 when
	 * untagged) — its exact value depends on repo tag state, not a
	 * fixed constant, so only readability is asserted here. */
	zassert_equal(reg_read(REG_SYS_STATUS_ID(REG_SYS_STATUS_FIELD_FW_VERSION), &value),
		      REG_OK);
	zassert_equal(reg_read(REG_SYS_STATUS_ID(REG_SYS_STATUS_FIELD_UPTIME), &value), REG_OK);
	zassert_equal(reg_read(REG_SYS_STATUS_ID(REG_SYS_STATUS_FIELD_BOARD_TEMPERATURE),
			       &value), REG_OK);
	zassert_equal(reg_read(REG_SYS_STATUS_ID(REG_SYS_STATUS_FIELD_BOARD_HUMIDITY),
			       &value), REG_OK);
}
```

(This removes the `zassert_equal(value.u32, 1U);` line; everything else in
the test is unchanged.)

- [ ] **Step 2: Rebuild and run — confirm this alone doesn't yet fail or pass meaningfully**

```sh
ninja -C /home/yong/backup/src/xlab/jianwei/hvb_wkspc/build_ss_shell
/home/yong/backup/src/xlab/jianwei/hvb_wkspc/build_ss_shell/zephyr/zephyr.exe
```

Expected: `PROJECT EXECUTION SUCCESSFUL`, 4/4 pass — the test still passes
because the register still returns the old stub value (`REG_OK` is all
that's asserted now). This step confirms the test edit alone is inert;
Step 3-4 below are what actually changes the register's value.

- [ ] **Step 3: Replace the FW_VERSION stub in `sys_uptime.c` with the real SemVer packing**

Modify `lib/sys_status/sys_uptime.c`:

```c
#include <zephyr/kernel.h>

#include "reg_store/reg_catalog.h"
#include "reg_store/reg_schema.h"

#include "fw_version.h"

/* Packed as major:8 | minor:8 | patch:16 — see docs/superpowers/specs/
 * 2026-07-19-version-management-contract-design.md §4. FW_VERSION_MAJOR/
 * MINOR/PATCH come from the firmware-vX.Y.Z git tag via
 * cmake/fw_version.cmake, falling back to 0.0.0 when untagged. */
static const uint32_t firmware_version =
	((uint32_t)(FW_VERSION_MAJOR & 0xFFU) << 24) |
	((uint32_t)(FW_VERSION_MINOR & 0xFFU) << 16) |
	((uint32_t)(FW_VERSION_PATCH & 0xFFFFU));
```

(Replace only the `#define SYS_STATUS_FW_VERSION_HIGH/LOW` + old
`firmware_version` initializer block; the rest of the file — the
`sys_uptime_reg_read` function and both `REG_DESCRIPTOR_DEFINE` calls at
the bottom — is unchanged.)

- [ ] **Step 4: Update the shell display to show major.minor.patch**

Modify `lib/sys_status/sys_status_shell.c`, the `cmd_ss` function's final
print statement:

```c
	shell_print(sh, "fw:      %u.%u.%u",
		    (unsigned)((version.u32 >> 24) & 0xFFU),
		    (unsigned)((version.u32 >> 16) & 0xFFU),
		    (unsigned)(version.u32 & 0xFFFFU));
```

(Replaces the old two-field `"fw:      %d.%d"` line; everything else in
`cmd_ss` is unchanged.)

- [ ] **Step 5: Rebuild and run — confirm still passing with the real value wired**

```sh
ninja -C /home/yong/backup/src/xlab/jianwei/hvb_wkspc/build_ss_shell
/home/yong/backup/src/xlab/jianwei/hvb_wkspc/build_ss_shell/zephyr/zephyr.exe
```

Expected: `SUITE PASS - 100.00% [ss_shell]: pass = 4, fail = 0`. Since no
`firmware-v*` tag exists yet in this repo, the register now reads `0.0.0`
(all-zero) rather than the old stub `0.1` — this is expected and correct;
the first real value appears once a `firmware-vX.Y.Z` tag is cut (a later,
separate plan per the design doc §7).

- [ ] **Step 6: Commit**

```bash
git add lib/sys_status/sys_uptime.c lib/sys_status/sys_status_shell.c tests/voltage_control/ss_shell/src/main.c
git commit -m "feat(sys_status): wire FW_VERSION register to real SemVer from git tag"
```

---

## Task 3: Add BOARD_HW_REVISION register (protocol v3.3)

**Files:**
- Modify: `lib/voltage_control/Kconfig`
- Modify: `include/reg_store/vc_global_regs.def`
- Modify: `include/reg_store/modbus_view.def`
- Modify: `include/reg_store/reg_map.h`
- Modify: `lib/voltage_control/vc_runtime.c`
- Modify: `lib/modbus_adapter/modbus_register.c`
- Modify: `tests/voltage_control/reg_store/src/main.c`

**Interfaces:**
- Consumes: nothing from Task 1/2 (independent register).
- Produces: `SYS_BOARD_HW_REVISION` wire offset (system input offset 16),
  `REG_VC_GLOBAL_FIELD_BOARD_HW_REVISION` / `REG_VC_GLOBAL_ORD_BOARD_HW_REVISION`
  semantic IDs, `CONFIG_VC_BOARD_HW_REVISION` Kconfig — these exact names
  are what any later host-tools task (a separate plan) will read.

- [ ] **Step 1: Add the failing test first — reference the not-yet-existing offset macro**

Modify `tests/voltage_control/reg_store/src/main.c`, in
`test_sys_input_wire_offsets_are_unique` (the last line of that test
function, currently ending at `SYS_FAULT_CAUSE`):

```c
	zassert_equal(used[SYS_FAULT_CAUSE]++,           0, "offset %u reused", SYS_FAULT_CAUSE);
	zassert_equal(used[SYS_BOARD_HW_REVISION]++,     0, "offset %u reused", SYS_BOARD_HW_REVISION);
}
```

- [ ] **Step 2: Confirm this fails to compile (SYS_BOARD_HW_REVISION doesn't exist yet)**

```sh
source /home/yong/backup/src/xlab/jianwei/hvb_wkspc/.venv/bin/activate
ninja -C /home/yong/backup/src/xlab/jianwei/hvb_wkspc/build_reg_store
```

If `build_reg_store` doesn't exist yet in your environment, configure it
first:

```sh
ZEPHYR_BASE=/home/yong/backup/src/xlab/jianwei/hvb_wkspc/zephyr \
ZEPHYR_TOOLCHAIN_VARIANT=host \
cmake -S tests/voltage_control/reg_store \
      -B /home/yong/backup/src/xlab/jianwei/hvb_wkspc/build_reg_store \
      -GNinja -DBOARD=native_posix
```

Expected: compile error, `'SYS_BOARD_HW_REVISION' undeclared`.

- [ ] **Step 3: Add the Kconfig option**

Modify `lib/voltage_control/Kconfig`, inserting a new config block right
before the existing `config VC_CURRENT_UNIT_EXP` block:

```
config VC_BOARD_HW_REVISION
	int "Hardware revision index reported over Modbus"
	default 0
	range 0 65535
	help
	  Index of this build's Zephyr board.yml revision (0 = default/revA,
	  1 = revB, ...), reported in the BOARD_HW_REVISION Modbus register.
	  Distinct from VC_VARIANT_ID: this identifies a hardware iteration
	  within one board variant (same DTS topology/drivers, e.g. a
	  calibration-nominal constant change), not a different board. See
	  docs/superpowers/specs/2026-07-19-version-management-contract-design.md
	  §3. Defaults to 0 on every board today since no board in this tree
	  declares board.yml revisions yet.

config VC_CURRENT_UNIT_EXP
```

- [ ] **Step 4: Add the semantic register field**

Modify `include/reg_store/vc_global_regs.def`, appending after the
existing `CURRENT_UNIT_EXP` line:

```
VC_GLOBAL_REG(CURRENT_UNIT_EXP,       0x0009, S16, RO, FIXED)
VC_GLOBAL_REG(BOARD_HW_REVISION,      0x000A, U16, RO, FIXED)
```

- [ ] **Step 5: Add the wire-address mapping**

Modify `include/reg_store/modbus_view.def`. First, update the system
input section header comment (currently `/* ---- System input (FC04,
offsets 0..15; 16..39 reserved) ----------- */`) to:

```
/* ---- System input (FC04, offsets 0..16; 17..39 reserved) ----------- */
```

Then replace the `CURRENT_UNIT_EXP` entry's trailing reserved-comment
block:

```
MODBUS_SYS16(CURRENT_UNIT_EXP,       INPUT,  15, FIXED)
/* Board.yml hardware-revision index within this board variant (0 = default/
 * revA). Distinct from VARIANT_ID (board type). Added in v3.3, additive-
 * only — see docs/superpowers/specs/2026-07-19-version-management-contract-
 * design.md §3, §4. */
MODBUS_SYS16(BOARD_HW_REVISION,      INPUT,  16, FIXED)
/* 17..39 reserved */
```

- [ ] **Step 6: Bump protocol minor version and document the change**

Modify `include/reg_store/reg_map.h`. Change:

```c
#define VC_PROTOCOL_MINOR             2
```

to:

```c
#define VC_PROTOCOL_MINOR             3
```

And append a new changelog block after the existing "Additive changes in
v3.2" comment block (immediately before the closing `*/` of the file's
top-of-file doc comment):

```c
 *
 * Additive changes in v3.3 (non-breaking):
 *   - SYS_BOARD_HW_REVISION added at system input offset 16 (previously
 *     reserved). Reports the Zephyr board.yml hardware-revision index
 *     within this board variant (0 = default/revA), distinct from
 *     SYS_VARIANT_ID (board type). See docs/superpowers/specs/
 *     2026-07-19-version-management-contract-design.md sections 3-4.
 */
```

- [ ] **Step 7: Wire the register's value source**

Modify `lib/voltage_control/vc_runtime.c`. Add the static value next to the
other `VC_GLOBAL`-backing statics:

```c
static const int16_t vc_current_unit_exp = CONFIG_VC_CURRENT_UNIT_EXP;
static const uint16_t vc_board_hw_revision = CONFIG_VC_BOARD_HW_REVISION;
```

And add the corresponding `VC_GLOBAL_VALUE_*` macro next to the others
(same file, further down near `#define VC_GLOBAL_VALUE_CURRENT_UNIT_EXP`):

```c
#define VC_GLOBAL_VALUE_CURRENT_UNIT_EXP (&vc_current_unit_exp)
#define VC_GLOBAL_VALUE_BOARD_HW_REVISION (&vc_board_hw_revision)
```

- [ ] **Step 8: Wire the Modbus adapter's view handle**

Modify `lib/modbus_adapter/modbus_register.c`, adding next to the other
`SYS_VIEW_HANDLE_*` macros:

```c
#define SYS_VIEW_HANDLE_CURRENT_UNIT_EXP VC_GLOBAL_HANDLE(CURRENT_UNIT_EXP)
#define SYS_VIEW_HANDLE_BOARD_HW_REVISION VC_GLOBAL_HANDLE(BOARD_HW_REVISION)
```

- [ ] **Step 9: Rebuild and confirm the test from Step 1 now passes**

```sh
ninja -C /home/yong/backup/src/xlab/jianwei/hvb_wkspc/build_reg_store
/home/yong/backup/src/xlab/jianwei/hvb_wkspc/build_reg_store/zephyr/zephyr.exe
```

Expected: `SUITE PASS - 100.00% [reg_store]: pass = 12, fail = 0`.

- [ ] **Step 10: Verify the register is actually readable through the full Modbus adapter path**

`vc_runtime.c` (where `vc_catalog_global_regs` and the `VC_GLOBAL_VALUE_*`
wiring live) only compiles under `CONFIG_VC_RUNTIME=y`, which
`tests/voltage_control/reg_store` doesn't enable — Step 9 only proves the
wire-offset/semantic-ID plumbing. Build and run the modbus_adapter test
suite (which does enable `CONFIG_VC_RUNTIME`) to prove the full register
actually compiles and links end to end:

```sh
ninja -C /home/yong/backup/src/xlab/jianwei/hvb_wkspc/build_modbus_adapter_test
/home/yong/backup/src/xlab/jianwei/hvb_wkspc/build_modbus_adapter_test/zephyr/zephyr.exe
```

If this build directory doesn't exist in your environment, configure it
against `tests/voltage_control/modbus_adapter` the same way as Step 2
above (swap `-S` and `-B` paths, keep `-DBOARD=native_posix`).

Expected: full `modbus_adapter` test suite passes (all cases, including
`test_sys_input_reads_protocol_version` — that test checks
`minor == VC_PROTOCOL_MINOR` symbolically, so the v3.3 bump in Step 6
doesn't break it).

- [ ] **Step 11: Commit**

```bash
git add lib/voltage_control/Kconfig include/reg_store/vc_global_regs.def \
        include/reg_store/modbus_view.def include/reg_store/reg_map.h \
        lib/voltage_control/vc_runtime.c lib/modbus_adapter/modbus_register.c \
        tests/voltage_control/reg_store/src/main.c
git commit -m "feat(reg_store): add BOARD_HW_REVISION register, bump protocol to v3.3"
```

---

## Task 4: Full verification across board targets

**Files:** none (verification only).

**Interfaces:** none — this task only builds and tests, it doesn't change
any source.

- [ ] **Step 1: Run the full native test matrix**

```sh
source /home/yong/backup/src/xlab/jianwei/hvb_wkspc/.venv/bin/activate
for d in build_reg_store build_ss_shell build_modbus_adapter_test build_vc_shell_test build_vc_state build_domain build_domain_np; do
	echo "=== $d ==="
	ninja -C /home/yong/backup/src/xlab/jianwei/hvb_wkspc/$d
	/home/yong/backup/src/xlab/jianwei/hvb_wkspc/$d/zephyr/zephyr.exe
done
```

Expected: every suite reports `PROJECT EXECUTION SUCCESSFUL` with 0
failures. `build_vc_shell_test`/`build_vc_state`/`build_domain*` weren't
touched by Tasks 1-3's files directly, but they link against
`lib/voltage_control/vc_runtime.c` and reference `VC_PROTOCOL_MINOR`
symbolically (never a hardcoded `2`), so they should be unaffected — this
step is a regression guard, not expected to surface new failures.

- [ ] **Step 2: Rebuild both real board targets**

```sh
ninja -C /home/yong/backup/src/xlab/jianwei/hvb_wkspc/build_psb_hvb
ninja -C /home/yong/backup/src/xlab/jianwei/hvb_wkspc/build_psb_lvb
```

Expected: both link successfully with a memory usage report (FLASH/RAM
percentages) printed at the end, no errors. This proves the ARM
cross-toolchain build path (distinct from the native_posix test builds
above) also compiles the new `fw_version.h` include and
`CONFIG_VC_BOARD_HW_REVISION` Kconfig cleanly.

- [ ] **Step 3: (Optional, requires attached hardware) Flash and confirm over Modbus**

If a `jw_hvb` or `jw_lvb` board is attached, flash it and confirm the new
registers read back correctly — not required to close this plan, but
useful real-hardware confirmation:

```sh
west flash -d /home/yong/backup/src/xlab/jianwei/hvb_wkspc/build_psb_hvb -r jlink
```

Then, using any Modbus client, read system input register offset 16
(`SYS_BOARD_HW_REVISION`) — expect `0`. Read input registers 10-11
(`SYS_FW_VERSION_HI/LO` combined) — expect `0` (no `firmware-v*` tag
exists in the repo yet, so the build-time fallback is `0.0.0`).

- [ ] **Step 4: No commit needed**

This task is verification-only; nothing to commit. If any step above
surfaces a regression, fix it as part of the task that introduced it
(Task 1, 2, or 3) and re-commit there rather than adding a new commit here.
