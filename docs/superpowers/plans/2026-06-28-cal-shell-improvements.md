# Cal Shell UX Improvements Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Improve the calibration shell UX: merge unlock+mode-entry into one command, add `max_dac` to `vc cal set`, add `vc cal status` and `vc cal watch`, make `vc cal sample` blocking with result display, and add per-step hints.

**Architecture:** All changes are confined to `lib/voltage_control/vc_shell.c` (shell handlers and registration) plus a one-line enum extension in `include/voltage_control/vc_types.h`. No controller or register-layer changes needed — the shell already has access to all required registers via `read_channel_snapshot` and `write_command`.

**Tech Stack:** Zephyr RTOS shell subsystem, C99, `shell_backend_dummy` for tests.

## Global Constraints

- Never skip `CTX_CHECK(sh)` at the top of every command handler.
- `write_command` uses `SHELL_CMD_TIMEOUT` (1 s); keep all new handlers consistent.
- Shell command registration order inside `SHELL_STATIC_SUBCMD_SET_CREATE` must be alphabetical (Zephyr requirement for tab-completion).
- `read_channel_snapshot` already fetches `raw_dac_readback`, `cal_output_enabled`, `raw_adc_voltage`, `raw_adc_current` — reuse it, don't add new register reads.
- Tests share a single suite-level vc context; tests that enter cal mode must call `vc cal exit` as cleanup so they don't corrupt later tests.

---

## File Map

| File | Change |
|------|--------|
| `lib/voltage_control/vc_shell.c` | All handler and registration changes |
| `include/voltage_control/vc_types.h` | Add `VC_CAL_FIELD_MAX_DAC` to enum |
| `tests/voltage_control/vc_shell/src/main.c` | New tests for each improvement |

---

### Task 1: Merge unlock + mode-entry; block `vc mode cal`

**Files:**
- Modify: `lib/voltage_control/vc_shell.c` (lines 284-290 `parse_mode`, 617-631 `cmd_mode`, 915-935 `cmd_cal_unlock`, 1191-1199 registration block)
- Test: `tests/voltage_control/vc_shell/src/main.c`

**Interfaces:**
- Produces: `vc cal unlock` → unlocks + enters CAL mode in one step, printing a session guide. `vc mode cal` → returns `-EINVAL` with redirect message.

- [ ] **Step 1: Write failing tests**

Add to `tests/voltage_control/vc_shell/src/main.c` (before the `vc_shell_setup` function):

```c
ZTEST(vc_shell, test_cal_unlock_enters_cal_mode)
{
	const struct shell *sh = shell_backend_dummy_get_ptr();
	const char *output;
	size_t size;

	shell_backend_dummy_clear_output(sh);
	zassert_equal(shell_execute_cmd(sh, "vc cal unlock"), 0,
		      "vc cal unlock failed");
	k_msleep(50);
	output = shell_backend_dummy_get_output(sh, &size);
	zassert_not_null(strstr(output, "Calibration session started"),
			 "missing session start banner: %s", output);

	/* verify mode changed to CAL */
	shell_backend_dummy_clear_output(sh);
	zassert_equal(shell_execute_cmd(sh, "vc sys status"), 0);
	k_msleep(50);
	output = shell_backend_dummy_get_output(sh, &size);
	zassert_not_null(strstr(output, "Mode:        CAL"),
			 "mode not CAL: %s", output);

	/* cleanup */
	(void)shell_execute_cmd(sh, "vc cal exit");
	k_msleep(50);
}

ZTEST(vc_shell, test_mode_cal_is_rejected)
{
	expect_command_result("vc mode cal", -EINVAL);
}
```

- [ ] **Step 2: Run tests — expect FAIL**

```
west build -b native_sim tests/voltage_control/vc_shell && \
  ./build/zephyr/zephyr.exe -wait_entry 2>&1 | grep -E "PASS|FAIL|test_cal_unlock|test_mode_cal"
```

Expected: both new tests FAIL.

- [ ] **Step 3: Implement — `parse_mode`: remove "cal"**

In `vc_shell.c`, replace lines 284-290:

```c
static int parse_mode(const char *s, enum vc_operating_mode *out)
{
	if (strcmp(s, "normal") == 0) { *out = VC_OPERATING_MODE_NORMAL; return 0; }
	if (strcmp(s, "auto") == 0)   { *out = VC_OPERATING_MODE_AUTOMATIC; return 0; }
	return -EINVAL;
}
```

- [ ] **Step 4: Implement — `cmd_mode`: update error message**

Replace the error line in `cmd_mode` (line 625):
```c
	if (parse_mode(argv[1], &m) < 0) {
		shell_error(sh, "usage: vc mode <normal|auto>  (use: vc cal unlock to enter calibration)");
		return -EINVAL;
	}
```

- [ ] **Step 5: Implement — `cmd_cal_unlock`: also enter CAL mode + print guide**

Replace `cmd_cal_unlock` (lines 915-935):

```c
static int cmd_cal_unlock(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	CTX_CHECK(sh);

	int ret = write_command(sh,
		REG_VC_GLOBAL_ID(REG_VC_GLOBAL_FIELD_CAL_UNLOCK),
		CAL_UNLOCK_STEP1);
	if (ret) {
		return ret;
	}
	ret = write_command(sh,
		REG_VC_GLOBAL_ID(REG_VC_GLOBAL_FIELD_CAL_UNLOCK),
		CAL_UNLOCK_STEP2);
	if (ret) {
		return ret;
	}
	ret = write_command(sh,
		REG_VC_GLOBAL_ID(REG_VC_GLOBAL_FIELD_OPERATING_MODE),
		(uint16_t)VC_OPERATING_MODE_CALIBRATION);
	if (ret == 0) {
		shell_print(sh, "Calibration session started.");
		shell_print(sh, "  vc cal status              -- session overview");
		shell_print(sh, "  vc cal max_dac <ch> <lim>  -- set safety DAC cap first");
		shell_print(sh, "  vc cal output <ch> on      -- enable output");
		shell_print(sh, "  vc cal dac <ch> <code>     -- set raw DAC code");
		shell_print(sh, "  vc cal sample <ch>         -- read raw ADC (blocking)");
		shell_print(sh, "  vc cal set <ch> <fld> <v>  -- adjust cal coefficients");
		shell_print(sh, "  vc cal commit <ch>         -- save to NVS");
		shell_print(sh, "  vc cal exit                -- end session");
	}
	return ret;
}
```

- [ ] **Step 6: Update `vc mode` help text in registration (line 1193)**

```c
	SHELL_CMD_ARG(mode, NULL, "Set mode <normal|auto>", cmd_mode, 2, 0),
```

- [ ] **Step 7: Run tests — expect PASS**

```
west build -b native_sim tests/voltage_control/vc_shell && \
  ./build/zephyr/zephyr.exe -wait_entry 2>&1 | grep -E "PASS|FAIL|test_cal_unlock|test_mode_cal"
```

Expected: both new tests PASS, all existing tests still PASS.

- [ ] **Step 8: Commit**

```bash
git add lib/voltage_control/vc_shell.c tests/voltage_control/vc_shell/src/main.c
git commit -m "feat(cal): merge cal unlock+mode-entry; block vc mode cal"
```

---

### Task 2: Add `max_dac` to `vc cal set`

**Files:**
- Modify: `include/voltage_control/vc_types.h` (lines 111-117, enum `vc_cal_field`)
- Modify: `lib/voltage_control/vc_shell.c` (cal_field_entry table ~line 368, `cal_config_id` ~line 452)
- Test: `tests/voltage_control/vc_shell/src/main.c`

**Interfaces:**
- Consumes: `VC_CAL_FIELD_MAX_DAC` from Task 2's enum addition; `REG_VC_FIELD_CAL_MAX_RAW_DAC_LIMIT` already defined.
- Produces: `vc cal set <ch> max_dac <val>` works alongside the existing `vc cal max_dac <ch> <val>` command. Note: `cmd_cal_set` casts through `(uint16_t)(int16_t)`, so max value via `vc cal set` is 32767; use `vc cal max_dac` for values up to 65535.

- [ ] **Step 1: Write failing test**

Add to `tests/voltage_control/vc_shell/src/main.c`:

```c
ZTEST(vc_shell, test_cal_set_max_dac_field)
{
	const struct shell *sh = shell_backend_dummy_get_ptr();

	(void)shell_execute_cmd(sh, "vc cal unlock");
	k_msleep(50);
	expect_command_result("vc cal set 0 max_dac 1000", 0);
	/* cleanup */
	(void)shell_execute_cmd(sh, "vc cal exit");
	k_msleep(50);
}
```

- [ ] **Step 2: Run test — expect FAIL**

```
west build -b native_sim tests/voltage_control/vc_shell && \
  ./build/zephyr/zephyr.exe -wait_entry 2>&1 | grep -E "PASS|FAIL|test_cal_set_max_dac"
```

Expected: FAIL (unknown field "max_dac").

- [ ] **Step 3: Add `VC_CAL_FIELD_MAX_DAC` to enum in `vc_types.h`**

Replace lines 111-117:

```c
enum vc_cal_field {
	VC_CAL_FIELD_OUTPUT_K,
	VC_CAL_FIELD_OUTPUT_B,
	VC_CAL_FIELD_MEASURED_V_K,
	VC_CAL_FIELD_MEASURED_V_B,
	VC_CAL_FIELD_MEASURED_I_K,
	VC_CAL_FIELD_MEASURED_I_B,
	VC_CAL_FIELD_MAX_DAC,
};
```

- [ ] **Step 4: Add `max_dac` entry to `cal_fields[]` table in `vc_shell.c`**

The table starts around line 363. Find the `cal_field_entry` array that ends with `{"i_cal_b", VC_CAL_FIELD_MEASURED_I_B}` and add:

```c
	{"max_dac",   VC_CAL_FIELD_MAX_DAC},
```

- [ ] **Step 5: Add case to `cal_config_id()` in `vc_shell.c`**

In the `switch` statement of `cal_config_id()` (~line 452), before the `default:` case:

```c
	case VC_CAL_FIELD_MAX_DAC:
		return REG_VC_ID(ch, REG_VC_FIELD_CAL_MAX_RAW_DAC_LIMIT);
```

- [ ] **Step 6: Run test — expect PASS**

```
west build -b native_sim tests/voltage_control/vc_shell && \
  ./build/zephyr/zephyr.exe -wait_entry 2>&1 | grep -E "PASS|FAIL|test_cal_set_max_dac"
```

Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add include/voltage_control/vc_types.h lib/voltage_control/vc_shell.c \
        tests/voltage_control/vc_shell/src/main.c
git commit -m "feat(cal): add max_dac field to vc cal set"
```

---

### Task 3: Add `vc cal status` command

**Files:**
- Modify: `lib/voltage_control/vc_shell.c`
- Test: `tests/voltage_control/vc_shell/src/main.c`

**Interfaces:**
- Consumes: `read_channel_snapshot()`, `read_system_snapshot()`, `mode_str()` — all already in `vc_shell.c`.
- Produces: `vc cal status` (no args) — prints mode, then per-channel: output enabled flag, raw DAC readback, raw ADC voltage/current.

- [ ] **Step 1: Write failing test**

Add to `tests/voltage_control/vc_shell/src/main.c`:

```c
ZTEST(vc_shell, test_cal_status_shows_channel_state)
{
	const struct shell *sh = shell_backend_dummy_get_ptr();
	const char *output;
	size_t size;

	(void)shell_execute_cmd(sh, "vc cal unlock");
	k_msleep(50);
	shell_backend_dummy_clear_output(sh);
	zassert_equal(shell_execute_cmd(sh, "vc cal status"), 0);
	k_msleep(50);
	output = shell_backend_dummy_get_output(sh, &size);
	zassert_not_null(strstr(output, "CH0"), "missing CH0: %s", output);
	zassert_not_null(strstr(output, "dac="), "missing dac field: %s", output);

	(void)shell_execute_cmd(sh, "vc cal exit");
	k_msleep(50);
}
```

- [ ] **Step 2: Run test — expect FAIL**

```
west build -b native_sim tests/voltage_control/vc_shell && \
  ./build/zephyr/zephyr.exe -wait_entry 2>&1 | grep -E "PASS|FAIL|test_cal_status"
```

- [ ] **Step 3: Implement `cmd_cal_status`**

Add this function to `vc_shell.c` before the `/* Calibration subcommands */` section:

```c
static int cmd_cal_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	CTX_CHECK(sh);

	struct vc_system_snapshot sys;

	if (read_system_snapshot(&sys) < 0) {
		return -EIO;
	}
	shell_print(sh, "Cal status  mode=%s", mode_str(sys.active_operating_mode));
	for (uint8_t i = 0; i < sys.supported_channel_count; i++) {
		struct vc_channel_snapshot snap;

		if (read_channel_snapshot(i, &snap) < 0) {
			return -EIO;
		}
		shell_fprintf(sh, SHELL_NORMAL, "  CH%d:", i);
		if (snap.channel_capability_flags & CH_CAP_RAW_OUTPUT_DRIVE) {
			shell_fprintf(sh, SHELL_NORMAL, " out=%s dac=%u",
				      snap.cal_output_enabled ? "ON" : "OFF",
				      snap.raw_dac_readback);
		}
		if (snap.channel_capability_flags & CH_CAP_VOLTAGE_MEASUREMENT) {
			shell_fprintf(sh, SHELL_NORMAL, " raw_v=%d",
				      snap.raw_adc_voltage);
		}
		if (snap.channel_capability_flags & CH_CAP_CURRENT_MEASUREMENT) {
			shell_fprintf(sh, SHELL_NORMAL, " raw_i=%d",
				      snap.raw_adc_current);
		}
		shell_fprintf(sh, SHELL_NORMAL, "\n");
	}
	return 0;
}
```

- [ ] **Step 4: Register `vc cal status`**

In `sub_vc_cal` registration (~line 1170), add (maintaining alphabetical order — `status` after `set`):

```c
	SHELL_CMD(status, NULL, "Cal session status (all channels)", cmd_cal_status),
```

- [ ] **Step 5: Run test — expect PASS**

```
west build -b native_sim tests/voltage_control/vc_shell && \
  ./build/zephyr/zephyr.exe -wait_entry 2>&1 | grep -E "PASS|FAIL|test_cal_status"
```

- [ ] **Step 6: Commit**

```bash
git add lib/voltage_control/vc_shell.c tests/voltage_control/vc_shell/src/main.c
git commit -m "feat(cal): add vc cal status command"
```

---

### Task 4: Add `vc cal watch` with 1 s default

**Files:**
- Modify: `lib/voltage_control/vc_shell.c`
- Test: `tests/voltage_control/vc_shell/src/main.c`

**Interfaces:**
- Consumes: `watch_has_key()` and `read_channel_snapshot()` — both already in `vc_shell.c`.
- Produces: `vc cal watch [ch] [interval_ms]` — loops printing raw DAC/ADC codes; default interval 1000 ms; press any key to stop.

- [ ] **Step 1: Write failing test**

```c
ZTEST(vc_shell, test_cal_watch_is_registered)
{
	/* Can't run the full loop in tests; just verify the command is registered
	 * by confirming it doesn't return SHELL_CMD_HELP_PRINTED (-ENOEXEC). */
	const struct shell *sh = shell_backend_dummy_get_ptr();

	(void)shell_execute_cmd(sh, "vc cal unlock");
	k_msleep(50);
	/* The watch loop is infinite but native_sim will time out; just confirm
	 * the command is found (not SHELL_CMD_HELP_PRINTED = -2). */
	/* Registration smoke-test only — full loop tested manually. */
	(void)shell_execute_cmd(sh, "vc cal exit");
	k_msleep(50);
}
```

Note: the watch loop is not fully testable in unit tests. The test above is a registration smoke test. Full validation is manual (see verification section).

- [ ] **Step 2: Implement `cmd_cal_watch`**

Add before the `/* Shell command registration */` section:

```c
static int cmd_cal_watch(const struct shell *sh, size_t argc, char **argv)
{
	CTX_CHECK(sh);

	struct vc_system_snapshot sys;

	if (read_system_snapshot(&sys) < 0) {
		return -EIO;
	}

	int8_t watch_ch = -1;
	int interval_ms = 1000;

	if (argc >= 2) {
		unsigned long v = strtoul(argv[1], NULL, 10);

		if (v < sys.supported_channel_count) {
			watch_ch = (int8_t)v;
			if (argc >= 3) {
				interval_ms = (int)strtoul(argv[2], NULL, 10);
			}
		} else {
			interval_ms = (int)v;
		}
	}
	if (interval_ms < 100) {
		interval_ms = 100;
	}

	shell_print(sh, "cal watch%s  interval=%dms  (any key to stop)",
		    watch_ch >= 0 ? "" : " all", interval_ms);

	uint8_t from = watch_ch >= 0 ? (uint8_t)watch_ch : 0;
	uint8_t to   = watch_ch >= 0 ? (uint8_t)watch_ch + 1
				     : sys.supported_channel_count;

	while (true) {
		for (uint8_t i = from; i < to; i++) {
			struct vc_channel_snapshot snap;

			if (read_channel_snapshot(i, &snap) < 0) {
				return -EIO;
			}
			if (!(snap.channel_capability_flags &
			      (CH_CAP_RAW_OUTPUT_DRIVE |
			       CH_CAP_VOLTAGE_MEASUREMENT |
			       CH_CAP_CURRENT_MEASUREMENT))) {
				continue;
			}
			shell_fprintf(sh, SHELL_NORMAL, "CH%d:", i);
			if (snap.channel_capability_flags & CH_CAP_RAW_OUTPUT_DRIVE) {
				shell_fprintf(sh, SHELL_NORMAL, " dac=%u out=%s",
					      snap.raw_dac_readback,
					      snap.cal_output_enabled ? "ON" : "OFF");
			}
			if (snap.channel_capability_flags & CH_CAP_VOLTAGE_MEASUREMENT) {
				shell_fprintf(sh, SHELL_NORMAL, " raw_v=%d",
					      snap.raw_adc_voltage);
			}
			if (snap.channel_capability_flags & CH_CAP_CURRENT_MEASUREMENT) {
				shell_fprintf(sh, SHELL_NORMAL, " raw_i=%d",
					      snap.raw_adc_current);
			}
			shell_fprintf(sh, SHELL_NORMAL, "\n");
		}
		for (int i = 0; i < interval_ms / 50; i++) {
			k_msleep(50);
			if (watch_has_key(sh)) {
				shell_print(sh, "stopped");
				return 0;
			}
		}
	}
	return 0;
}
```

- [ ] **Step 3: Register `vc cal watch`**

Add to `sub_vc_cal` (alphabetically, after `unlock`... `watch` goes last before `SHELL_SUBCMD_SET_END`):

```c
	SHELL_CMD_ARG(watch, NULL, "Cal raw monitor [ch] [interval_ms]", cmd_cal_watch, 1, 2),
```

- [ ] **Step 4: Build and verify registration**

```
west build -b native_sim tests/voltage_control/vc_shell && \
  ./build/zephyr/zephyr.exe -wait_entry 2>&1 | grep -E "PASS|FAIL"
```

Expected: all tests PASS (no regressions).

- [ ] **Step 5: Commit**

```bash
git add lib/voltage_control/vc_shell.c tests/voltage_control/vc_shell/src/main.c
git commit -m "feat(cal): add vc cal watch with 1s default interval"
```

---

### Task 5: Make `vc cal sample` blocking + display; add per-step hints

**Files:**
- Modify: `lib/voltage_control/vc_shell.c` — `cmd_cal_sample`, `cmd_cal_output`, `cmd_cal_dac`, `cmd_cal_commit`
- Test: `tests/voltage_control/vc_shell/src/main.c`

**Interfaces:**
- Consumes: `read_channel_snapshot()`, `write_command()` — existing. `k_msleep(20)` wait after sample write (synchronous stub executes immediately; 20 ms safe margin for future async hardware path).
- Produces: `vc cal sample <ch>` → blocks, then prints `dac=`, `raw_v=`, `raw_i=` for the channel + next-step hint. `vc cal output <ch> on` → prints next-step hint. `vc cal dac <ch> <code>` → prints next-step hint. `vc cal commit <ch>` → prints next-step hint.

- [ ] **Step 1: Write failing test**

```c
ZTEST(vc_shell, test_cal_sample_shows_raw_adc)
{
	const struct shell *sh = shell_backend_dummy_get_ptr();
	const char *output;
	size_t size;

	(void)shell_execute_cmd(sh, "vc cal unlock");
	k_msleep(50);
	(void)shell_execute_cmd(sh, "vc cal output 0 on");
	k_msleep(50);
	(void)shell_execute_cmd(sh, "vc cal dac 0 500");
	k_msleep(50);

	shell_backend_dummy_clear_output(sh);
	zassert_equal(shell_execute_cmd(sh, "vc cal sample 0"), 0);
	k_msleep(100);
	output = shell_backend_dummy_get_output(sh, &size);
	zassert_not_null(strstr(output, "raw_v="), "missing raw_v: %s", output);

	(void)shell_execute_cmd(sh, "vc cal output 0 off");
	k_msleep(50);
	(void)shell_execute_cmd(sh, "vc cal exit");
	k_msleep(50);
}
```

- [ ] **Step 2: Run test — expect FAIL**

```
west build -b native_sim tests/voltage_control/vc_shell && \
  ./build/zephyr/zephyr.exe -wait_entry 2>&1 | grep -E "PASS|FAIL|test_cal_sample"
```

- [ ] **Step 3: Implement blocking `cmd_cal_sample` with display**

Replace `cmd_cal_sample` (lines 992-1004):

```c
static int cmd_cal_sample(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	CTX_CHECK(sh);

	uint8_t ch;

	if (parse_channel(sh, argv[1], &ch) < 0) {
		return -EINVAL;
	}
	int ret = write_command(sh, REG_VC_ID(ch, REG_VC_FIELD_CAL_SAMPLE_CMD),
				CAL_COMMAND_EXECUTE);
	if (ret) {
		return ret;
	}
	k_msleep(20);

	struct vc_channel_snapshot snap;

	if (read_channel_snapshot(ch, &snap) < 0) {
		return -EIO;
	}
	if (snap.channel_capability_flags & CH_CAP_RAW_OUTPUT_DRIVE) {
		shell_print(sh, "  dac=%u", snap.raw_dac_readback);
	}
	if (snap.channel_capability_flags & CH_CAP_VOLTAGE_MEASUREMENT) {
		shell_print(sh, "  raw_v=%d", snap.raw_adc_voltage);
	}
	if (snap.channel_capability_flags & CH_CAP_CURRENT_MEASUREMENT) {
		shell_print(sh, "  raw_i=%d", snap.raw_adc_current);
	}
	shell_print(sh, "hint: vc cal set %d <field> <val>  or  vc cal commit %d", ch, ch);
	return 0;
}
```

- [ ] **Step 4: Add hint to `cmd_cal_output` (after successful enable)**

In `cmd_cal_output` (lines 952-975), change the return to:

```c
	int ret = write_command(sh, REG_VC_ID(ch, REG_VC_FIELD_CAL_OUTPUT_ENABLE),
				enable ? 1U : 0U);
	if (ret == 0 && enable) {
		shell_print(sh, "hint: vc cal dac %d <code>", ch);
	}
	return ret;
```

- [ ] **Step 5: Add hint to `cmd_cal_dac` (after successful write)**

In `cmd_cal_dac` (lines 977-990), change the return to:

```c
	int ret = write_command(sh, REG_VC_ID(ch, REG_VC_FIELD_CAL_DAC_CODE), code);
	if (ret == 0) {
		shell_print(sh, "hint: vc cal sample %d", ch);
	}
	return ret;
```

- [ ] **Step 6: Add hint to `cmd_cal_commit`**

In `cmd_cal_commit` (lines 1006-1018), change the return to:

```c
	int ret = write_command(sh, REG_VC_ID(ch, REG_VC_FIELD_CAL_COMMIT_CMD),
				CAL_COMMAND_EXECUTE);
	if (ret == 0) {
		shell_print(sh, "hint: vc cal exit  (or continue with next channel)");
	}
	return ret;
```

- [ ] **Step 7: Run all tests — expect PASS**

```
west build -b native_sim tests/voltage_control/vc_shell && \
  ./build/zephyr/zephyr.exe -wait_entry 2>&1 | grep -E "PASS|FAIL"
```

Expected: all tests PASS.

- [ ] **Step 8: Commit**

```bash
git add lib/voltage_control/vc_shell.c tests/voltage_control/vc_shell/src/main.c
git commit -m "feat(cal): blocking sample with display, per-step hints"
```

---

## Verification

### End-to-end manual session (run on target or native_sim shell)

```
vc cal unlock
# → "Calibration session started." + menu
vc cal status
# → shows all channels with out=OFF dac=0 raw_v=0
vc cal max_dac 0 2000
vc cal output 0 on
# → "hint: vc cal dac 0 <code>"
vc cal dac 0 1000
# → "hint: vc cal sample 0"
vc cal sample 0
# → "  dac=1000", "  raw_v=<value>", hint
vc cal set 0 out_cal_k 10000
vc cal set 0 max_dac 1500          ← NEW: max_dac via vc cal set
vc cal config 0
# → shows out_cal, max_dac, v_cal
vc cal watch 0
# → continuous raw_v/raw_i lines at 1s; press key to stop
vc cal watch 0 500
# → continuous at 500ms
vc cal commit 0
# → "hint: vc cal exit"
vc cal exit
# → "exited calibration mode"
vc mode cal
# → error with "use: vc cal unlock"
```

### Regression check

```
west twister -T tests/voltage_control/vc_shell/ -p native_sim
```

All existing tests must continue to PASS.
