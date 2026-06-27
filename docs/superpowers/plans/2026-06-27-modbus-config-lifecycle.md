# Modbus Configuration Lifecycle Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Separate technician-controlled live Modbus configuration from end-user next-boot configuration, and move delayed system reset out of the voltage-control domain into the `sys_status` module.

**Architecture:** The Modbus adapter keeps two static four-byte snapshots: active/live and next-boot/persisted. Shell APIs modify the active snapshot and require explicit save; Modbus register writes atomically persist and then publish the next-boot snapshot. A separately selectable reset source in `lib/sys_status/` owns one static delayable work item used by `ss reset` and Modbus system action 255.

**Tech Stack:** Zephyr 3.7, C11, Kconfig, static `k_work_delayable`, Settings/NVS, Zephyr shell, Ztest/Twister, native_posix, STM32 `jw_hvb` build.

---

## File Map

- `include/sys_status/sys_status.h`: public system-reset request API.
- `lib/sys_status/sys_reset.c`: static delayed reset scheduling and platform reboot hook.
- `lib/sys_status/Kconfig`: reset enable and response-drain delay.
- `lib/sys_status/CMakeLists.txt`: static reset-source composition.
- `lib/sys_status/sys_status_shell.c`: retain `ss` status output and add `ss reset`.
- `include/reg_store/reg_map.h`: protocol constant for system action 255.
- `include/voltage_control/vc_types.h`: remove reboot from domain parameter actions.
- `lib/voltage_control/vc_controller.c`: remove inline `sys_reboot()` paths.
- `lib/voltage_control/vc_shell.c`: remove `vc reset` and `vc sys reset`.
- `include/modbus_adapter/modbus_adapter.h`: explicit active and next-boot query APIs; error-returning factory operation.
- `lib/modbus_adapter/modbus_adapter.c`: dual snapshots, source-specific persistence policy, direct system reset routing.
- `lib/modbus_adapter/Kconfig`: require reset support for the product Modbus frontend.
- `lib/modbus_adapter/modbus_adapter_shell.c`: show both snapshots and pending-restart state.
- `tests/voltage_control/ss_shell/src/main.c`: reset command and delayed-execution regression coverage.
- `tests/voltage_control/ss_shell/prj.conf`: enable reset with a short deterministic test delay.
- `tests/voltage_control/vc_shell/src/main.c`: prove legacy reset commands are absent.
- `tests/voltage_control/modbus_adapter/src/main.c`: storage-off immutability and reset-scope tests.
- `tests/voltage_control/modbus_adapter/prj_settings.conf`: NVS-backed settings test variant.
- `tests/voltage_control/modbus_adapter/testcase.yaml`: add settings-enabled scenario.
- `tests/voltage_control/mb_shell/src/main.c`: technician active/save/factory behavior and status output.
- `applications/hvb_controller/prj.conf`: enable system reset in the product image.
- `ref/modbus_interface.md`: state next-boot readback, auto-save, reset, and storage-disabled behavior.

### Task 1: Move reset ownership to the system module

**Files:**
- Create: `lib/sys_status/sys_reset.c`
- Modify: `include/sys_status/sys_status.h`
- Modify: `lib/sys_status/Kconfig`
- Modify: `lib/sys_status/CMakeLists.txt`
- Modify: `lib/sys_status/sys_status_shell.c`
- Modify: `include/reg_store/reg_map.h`
- Modify: `include/voltage_control/vc_types.h`
- Modify: `lib/voltage_control/vc_controller.c`
- Modify: `lib/voltage_control/vc_shell.c`
- Modify: `applications/hvb_controller/prj.conf`
- Test: `tests/voltage_control/ss_shell/src/main.c`
- Test: `tests/voltage_control/vc_shell/src/main.c`

- [ ] **Step 1: Write failing shell ownership tests**

Extend `ss_shell` with a strong test override for the weak platform hook and verify that `ss reset` returns before the delayed callback runs:

```c
static struct k_sem reset_called;

void sys_status_platform_reset(void)
{
	k_sem_give(&reset_called);
}

ZTEST(ss_shell, test_reset_is_acknowledged_before_execution)
{
	k_sem_init(&reset_called, 0, 1);
	expect_command_result("ss reset", 0);
	zassert_equal(k_sem_take(&reset_called, K_NO_WAIT), -EBUSY);
	zassert_equal(k_sem_take(&reset_called, K_MSEC(100)), 0);
}
```

Change the VC shell regression expectations:

```c
ZTEST(vc_shell, test_reset_commands_are_not_registered)
{
	expect_command_result("vc reset", SHELL_CMD_HELP_PRINTED);
	expect_command_result("vc sys reset", SHELL_CMD_HELP_PRINTED);
}
```

- [ ] **Step 2: Run RED**

Run:

```bash
west twister -p native_posix -T tests/voltage_control/ss_shell -T tests/voltage_control/vc_shell --outdir /tmp/twister-system-reset --clobber-output
```

Expected: `ss reset` is not found and the VC reset commands still return `-ENODEV`.

- [ ] **Step 3: Add the static reset service**

Add these Kconfig symbols:

```kconfig
config SYS_RESET
	bool "System reset service"
	depends on REBOOT
	default n
	help
	  Provide a delayed system-level cold-reset request service.

config SYS_RESET_DELAY_MS
	int "System reset response-drain delay (ms)"
	depends on SYS_RESET
	default 250
	range 1 5000
```

Add the public API:

```c
int sys_status_request_reset(void);
void sys_status_platform_reset(void);
```

Implement `sys_reset.c` with one static work item:

```c
#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>

#include "sys_status/sys_status.h"

__weak void sys_status_platform_reset(void)
{
	sys_reboot(SYS_REBOOT_COLD);
}

static void reset_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	sys_status_platform_reset();
}

K_WORK_DELAYABLE_DEFINE(reset_work, reset_work_handler);

int sys_status_request_reset(void)
{
	(void)k_work_reschedule(&reset_work, K_MSEC(CONFIG_SYS_RESET_DELAY_MS));
	return 0;
}
```

Compile it with `zephyr_library_sources_ifdef(CONFIG_SYS_RESET sys_reset.c)` and enable `CONFIG_REBOOT=y` plus `CONFIG_SYS_RESET=y` in the product app.

- [ ] **Step 4: Register `ss reset` and remove VC reset**

Convert `ss` to a static subcommand set while retaining its top-level status handler:

```c
static int cmd_ss_reset(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	int ret = sys_status_request_reset();
	if (ret == 0) {
		shell_print(sh, "system reset requested");
	}
	return ret;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_ss,
	SHELL_CMD(reset, NULL, "Reset system", cmd_ss_reset),
	SHELL_SUBCMD_SET_END
);
SHELL_CMD_REGISTER(ss, &sub_ss,
	"System status (uptime, temp, humidity, fw)", cmd_ss);
```

Delete `cmd_software_reset()` and both reset registrations from `vc_shell.c`. Delete `VC_PARAM_ACTION_SOFTWARE_RESET` from `enum vc_param_action`, the reboot include, and both reboot cases from `vc_controller.c`. Define the protocol value in `reg_map.h`:

```c
#define SYS_PARAM_ACTION_SOFTWARE_RESET 255U
```

- [ ] **Step 5: Run GREEN and commit**

Run the Twister command from Step 2. Expected: all `ss_shell` and `vc_shell` cases pass.

Commit:

```bash
git add include/sys_status include/reg_store/reg_map.h include/voltage_control/vc_types.h \
  lib/sys_status lib/voltage_control applications/hvb_controller/prj.conf \
  tests/voltage_control/ss_shell tests/voltage_control/vc_shell
git commit -m "refactor(system): move reset out of voltage control"
```

### Task 2: Separate Active and Next-Boot Modbus Configuration

**Files:**
- Modify: `include/modbus_adapter/modbus_adapter.h`
- Modify: `lib/modbus_adapter/modbus_adapter.c`
- Modify: `lib/modbus_adapter/modbus_adapter_shell.c`
- Test: `tests/voltage_control/modbus_adapter/src/main.c`
- Test: `tests/voltage_control/mb_shell/src/main.c`

- [ ] **Step 1: Write failing storage-disabled tests**

Add explicit query coverage:

```c
ZTEST(modbus_adapter, test_config_writes_are_read_only_without_settings)
{
	struct vc_ctx *ctx = make_ctx();
	struct vc_mb_adapter *mb = vc_mb_adapter_create(ctx);
	uint16_t reg;

	zassert_not_null(mb);
	zassert_equal(vc_mb_holding_wr(mb, SYS_SLAVE_ADDRESS, 42),
		      VC_MB_ILLEGAL_ADDRESS);
	zassert_equal(vc_mb_holding_rd(mb, SYS_SLAVE_ADDRESS, &reg), VC_MB_OK);
	zassert_equal(reg, 1);
	destroy_ctx(ctx);
}

ZTEST(modbus_adapter, test_channel_reset_action_is_rejected)
{
	struct vc_ctx *ctx = make_ctx();
	struct vc_mb_adapter *mb = vc_mb_adapter_create(ctx);
	zassert_equal(vc_mb_holding_wr(mb, CH_BLOCK_BASE(0) + CH_PARAM_ACTION,
				       SYS_PARAM_ACTION_SOFTWARE_RESET),
		      VC_MB_ILLEGAL_VALUE);
	destroy_ctx(ctx);
}
```

- [ ] **Step 2: Run RED**

Run:

```bash
west twister -W -p native_posix -T tests/voltage_control/modbus_adapter --outdir /tmp/twister-mb-policy-red --clobber-output
```

Expected: the current adapter accepts the configuration write.

- [ ] **Step 3: Replace the ambiguous singleton state**

Change the adapter state to:

```c
struct vc_mb_adapter {
	struct vc_ctx *ctx;
	struct mb_adapter_config active_cfg;
	struct mb_adapter_config boot_cfg;
};
```

Add explicit public queries:

```c
int modbus_adapter_get_active_config(struct mb_adapter_config *cfg);
int modbus_adapter_get_next_boot_config(struct mb_adapter_config *cfg);
bool modbus_adapter_config_is_persistent(void);
```

Construct defaults through one helper that maps the configured baud in hertz to
the protocol enum, guarded by a build assertion accepting only 9600 or 115200.

- [ ] **Step 4: Implement storage-disabled behavior and build hygiene**

When `CONFIG_SETTINGS=n`, FC03 reads `boot_cfg` initialized from compiled defaults
and FC06 writes to slave/baud return `VC_MB_ILLEGAL_ADDRESS`. Shell setters update
`active_cfg` and call the live reconfiguration helper. Shell save/load return
`-ENOTSUP`; shell factory applies compiled defaults live.

Move `baud_code_to_rate()` inside `#ifdef CONFIG_MODBUS` so native default Twister
does not emit an unused-function warning.

- [ ] **Step 5: Update shell status and factory error handling**

Change `modbus_adapter_config_factory()` to return `int`. Make `mb status` print:

```text
Modbus Adapter
  active:    slave=<n> baud=<code> (<hz> Hz)
  next boot: volatile defaults
```

With persistence enabled it prints the next-boot values and appends
`restart required` when either field differs.

- [ ] **Step 6: Run GREEN and commit**

Run the Task 2 Twister command. Expected: all Modbus-adapter tests pass under
default warnings-as-errors.

Commit:

```bash
git add include/modbus_adapter lib/modbus_adapter \
  tests/voltage_control/modbus_adapter tests/voltage_control/mb_shell
git commit -m "refactor(modbus): separate live and next-boot config"
```

### Task 3: Add settings-backed next-boot policy

**Files:**
- Modify: `lib/modbus_adapter/modbus_adapter.c`
- Modify: `lib/modbus_adapter/Kconfig`
- Create: `tests/voltage_control/modbus_adapter/prj_settings.conf`
- Modify: `tests/voltage_control/modbus_adapter/testcase.yaml`
- Modify: `tests/voltage_control/modbus_adapter/src/main.c`

- [ ] **Step 1: Add the settings-enabled test variant**

Create `prj_settings.conf`:

```conf
CONFIG_FLASH=y
CONFIG_FLASH_MAP=y
CONFIG_FLASH_SIMULATOR=y
CONFIG_NVS=y
CONFIG_SETTINGS=y
CONFIG_SETTINGS_RUNTIME=y
CONFIG_SETTINGS_NVS=y
CONFIG_VC_SETTINGS_PERSISTENCE=y
```

Add a second scenario with
`extra_args: OVERLAY_CONFIG=prj_settings.conf` and `platform_allow: native_posix`.

- [ ] **Step 2: Write failing persistence and source-policy tests**

Under `#if defined(CONFIG_SETTINGS)`, add tests that:

1. delete `mb/cfg`, create the adapter, and verify compiled defaults are stored;
2. write slave 42 over Modbus, verify FC03/next-boot report 42 while active remains
   the compiled default;
3. recreate the adapter and verify both snapshots now report 42;
4. set active slave 77 through the shell-facing API, verify next-boot remains 42,
   call save, and verify next-boot becomes 77;
5. invoke Modbus factory action and verify next-boot becomes compiled defaults while
   active stays 77.

- [ ] **Step 3: Run RED**

Run:

```bash
west twister -p native_posix -T tests/voltage_control/modbus_adapter --outdir /tmp/twister-mb-settings-red --clobber-output
```

Expected: the settings scenario fails because current writes reconfigure one shared
snapshot and factory deletes the key.

- [ ] **Step 4: Implement atomic next-boot persistence**

For Modbus config writes, create a candidate from `boot_cfg`, validate it, call
`settings_save_one("mb/cfg", &candidate, sizeof(candidate))`, and assign
`boot_cfg = candidate` only on success. Map save failure to
`VC_MB_DEVICE_FAILURE`.

At startup, load and validate `mb/cfg`. If absent, wrong-sized, or invalid, store
compiled defaults. A storage error makes adapter creation fail rather than silently
claiming persistent configuration.

Shell save persists `active_cfg` then copies it to `boot_cfg`. Shell load reads and
validates `boot_cfg`, applies it live, then updates `active_cfg`. Shell factory saves
compiled defaults, updates `boot_cfg`, and applies them live without deleting the
key.

Modbus parameter save performs no adapter write; load refreshes only `boot_cfg`;
factory persists defaults to `boot_cfg` without changing `active_cfg`.

- [ ] **Step 5: Route Modbus system reset directly**

In `handle_sys_param_action()`, handle
`SYS_PARAM_ACTION_SOFTWARE_RESET` before casting to `enum vc_param_action`:

```c
if (val == SYS_PARAM_ACTION_SOFTWARE_RESET) {
	return sys_status_request_reset() == 0 ? VC_MB_OK : VC_MB_DEVICE_FAILURE;
}
```

Never send action 255 through `vc_reg_write_sys_holding()` or `vc_dispatch()`.
Guard the call with `IS_ENABLED(CONFIG_SYS_RESET)` so non-product unit-test builds
return `VC_MB_DEVICE_FAILURE` without introducing a link dependency. Make
`UI_MODBUS_RTU` select `REBOOT` and `SYS_RESET` so the product frontend always
provides the documented reset operation.

- [ ] **Step 6: Run GREEN and commit**

Run the Task 3 Twister command. Expected: both storage-disabled and settings-enabled
scenarios pass.

Commit:

```bash
git add lib/modbus_adapter tests/voltage_control/modbus_adapter
git commit -m "fix(modbus): persist end-user config for next boot"
```

### Task 4: Update protocol documentation and verify the product

**Files:**
- Modify: `ref/modbus_interface.md`

- [ ] **Step 1: Update observable protocol behavior**

Document that FC03 slave/baud values are persisted next-boot values, FC06 writes
are automatically saved when settings are enabled, writes are rejected when
settings are disabled, factory action stores compiled defaults, system action 255
is acknowledged before delayed reboot, and channel action 255 is invalid.

- [ ] **Step 2: Run all focused suites**

```bash
west twister -p native_posix \
  -T tests/voltage_control/modbus_adapter \
  -T tests/voltage_control/mb_shell \
  -T tests/voltage_control/ss_shell \
  -T tests/voltage_control/vc_shell \
  --outdir /tmp/twister-modbus-lifecycle --clobber-output
```

Expected: all scenarios and cases pass with warnings treated as errors.

- [ ] **Step 3: Clean-build the product**

```bash
west build -p always -b jw_hvb applications/hvb_controller \
  -d /tmp/hvb-build-modbus-lifecycle
```

Expected: build succeeds; FLASH/RAM usage is reported.

- [ ] **Step 4: Run static checks**

```bash
git diff --check
rg -n "VC_PARAM_ACTION_SOFTWARE_RESET|cmd_software_reset|vc reset|vc sys reset" \
  include lib applications tests
```

Expected: `git diff --check` succeeds and the legacy reset search returns no code
matches.

- [ ] **Step 5: Commit documentation**

```bash
git add ref/modbus_interface.md
git commit -m "docs(modbus): clarify configuration activation policy"
```
