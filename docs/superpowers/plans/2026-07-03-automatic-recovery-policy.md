# Automatic Recovery Policy Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `recovery_policy_mode` (`AUTO_RETRY` / `AUTO_DERATE_RETRY`) actually do something. Today the field round-trips through Modbus/shell/TUI correctly but nothing in the domain layer ever reads it — every fault just latches until a manual clear, regardless of configured policy. `auto_retry_count` is hardcoded to 0, and the `VC_CHANNEL_SMF_RETRY_COOLDOWN` state is declared but never entered.

**Architecture:** Add a `tick_recovery()` step to the existing per-tick pipeline in `vc_channel_run()` (after `vc_channel_tick_ramp()`), gated to Automatic operating mode per the existing design spec (`docs/superpowers/specs/2026-06-15-voltage-control-domain-behavior.md`, "Automatic Recovery" section). Scope is deliberately narrowed to `VC_FAULT_CURRENT` — the only fault type this codebase has a corresponding "is it safe now" check for (`is_safe_to_clear_active()`, current safe-band only). Other fault causes (hardware, interlock, measurement, stale) always stay manual-latch; that's an explicit, documented decision, not a placeholder. A fixed-size ring buffer (`CONFIG_VC_MAX_RETRY_HISTORY`, default 8) tracks retry timestamps for the sliding-window count/derate-step math; a new `recovering`/`recovery_target` pair on `struct vc_channel` lets `vc_channel_tick_ramp()` drive toward a (possibly derated) retry target using the channel's existing ramp machinery, so a retry is a normal graceful ramp-up, not an instant jump.

**Known simplification vs. the spec** (see "Deliberately out of scope" at the end): the spec says automatic recovery should apply "only to faults detected while Automatic Mode is already active" — a fault that latched in Normal mode should *not* become retryable just because the system later switches to Automatic. This plan's gate only checks the *current* operating mode on each tick, not the mode at the moment the fault was detected, so it doesn't make that distinction. Implementing it properly means threading a mode-transition timestamp from the controller down into the channel (`vc_channel_consume_current()` doesn't currently receive `sys_cfg` at all) and comparing it against `last_fault_timestamp` — a meaningfully bigger change than the rest of this plan. Flagged explicitly rather than silently skipped.

**Tech Stack:** C (Zephyr domain library), existing `ztest`/`native_posix` test suites (`tests/voltage_control/vc_channel_state`, `tests/voltage_control/domain`).

---

## File structure

- Modify `include/voltage_control/vc_channel.h`: add `recovering`, `recovery_target`, `retry_timestamps_ms[]`, `retry_timestamp_count` runtime fields to `struct vc_channel`.
- Modify `lib/voltage_control/Kconfig`: add `CONFIG_VC_MAX_RETRY_HISTORY`.
- Modify `lib/voltage_control/vc_channel.c`: uptime clock plumbing, `auto_retry_max_count` validation, new static helpers + `tick_recovery()`, `vc_channel_tick_ramp()` recovery-target support, `vc_channel_get_snapshot()` real retry count, `force_safe_state()` defensive reset.
- Modify `tests/voltage_control/vc_channel_state/src/main.c`: channel-level behavior tests.
- Modify `tests/voltage_control/domain/src/main.c`: controller-level integration test (Automatic-mode gating end to end via `vc_controller_tick`).
- Modify `docs/guide/parameter-reference.md`: document the now-real behavior in place of the current "unimplemented" silence.

---

### Task 1: Uptime clock, new fields, and `auto_retry_max_count` bound

**Files:**
- Modify: `include/voltage_control/vc_channel.h`
- Modify: `lib/voltage_control/Kconfig`
- Modify: `lib/voltage_control/vc_channel.c:349-369` (`vc_channel_run`)
- Modify: `lib/voltage_control/vc_channel.c:457-459` (`VC_FIELD_AUTO_RETRY_MAX_COUNT` case)
- Test: `tests/voltage_control/vc_channel_state/src/main.c`

`ch->uptime_ref` is declared but never incremented anywhere — it's always 0. The sliding-window retry math in later tasks needs a real monotonic clock. Follow the existing pattern (`ch->ramp_accum_ms += dt_ms` in `vc_channel_tick_ramp`): accumulate from the caller-supplied `dt_ms`, no direct kernel calls in this domain layer (keeps it host-testable under `native_posix`).

- [ ] **Step 1: Add the Kconfig bound**

In `lib/voltage_control/Kconfig`, insert after the `VC_DEFAULT_CURRENT_SAFE_BAND_PCT` block (after line 151, before `config VC_CAL_WATCHDOG_TIMEOUT_S`):

```
config VC_MAX_RETRY_HISTORY
	int "Max tracked auto-retry attempts per channel"
	default 8
	range 1 32
	help
	  Size of the fixed-size ring buffer used for the auto-retry sliding
	  window (Auto Retry Window / Auto Retry Max Count). auto_retry_max_count
	  writes above this value are rejected -- there is no dynamic allocation
	  in the domain layer, so this is a hard ceiling, not a soft default.
```

- [ ] **Step 2: Add the new runtime fields to `struct vc_channel`**

In `include/voltage_control/vc_channel.h`, the struct currently reads (around line 42-49):

```c
	bool output_enabled;
	bool ramping;
	bool ramp_to_disable;
	int16_t graceful_ramp_dest;   /* runtime-only, not a register */
	uint32_t ramp_accum_ms;
	uint32_t cooldown_remaining_ms;
	int16_t operational_target_voltage;
```

Change it to:

```c
	bool output_enabled;
	bool ramping;
	bool ramp_to_disable;
	int16_t graceful_ramp_dest;   /* runtime-only, not a register */
	uint32_t ramp_accum_ms;
	uint32_t cooldown_remaining_ms;
	bool recovering;                                             /* auto-retry ramp in progress */
	int16_t recovery_target;                                     /* ramp target while recovering */
	uint32_t retry_timestamps_ms[CONFIG_VC_MAX_RETRY_HISTORY];    /* sliding-window retry log */
	uint8_t retry_timestamp_count;
	int16_t operational_target_voltage;
```

`vc_channel_init()` already does `memset(ch, 0, sizeof(*ch))`, so these all start zero/false — no separate init code needed.

- [ ] **Step 3: Increment `uptime_ref` in `vc_channel_run()`**

In `lib/voltage_control/vc_channel.c`, `vc_channel_run()` currently reads:

```c
void vc_channel_run(struct vc_channel *ch, uint32_t dt_ms,
			    const struct vc_system_config *sys_cfg)
{
	if (ch->meas != NULL) {
```

Change to:

```c
void vc_channel_run(struct vc_channel *ch, uint32_t dt_ms,
			    const struct vc_system_config *sys_cfg)
{
	ch->uptime_ref += dt_ms;

	if (ch->meas != NULL) {
```

- [ ] **Step 4: Validate `auto_retry_max_count` against the Kconfig bound**

In `lib/voltage_control/vc_channel.c`, `vc_channel_set_field()` currently has:

```c
	case VC_FIELD_AUTO_RETRY_MAX_COUNT:
		ch->config.auto_retry_max_count = value;
		break;
```

Change to:

```c
	case VC_FIELD_AUTO_RETRY_MAX_COUNT:
		if (value > CONFIG_VC_MAX_RETRY_HISTORY) {
			return VC_ERR_INVALID_VALUE;
		}
		ch->config.auto_retry_max_count = value;
		break;
```

- [ ] **Step 5: Write the failing test**

In `tests/voltage_control/vc_channel_state/src/main.c`, add near the other `set_field` tests (after `test_set_field_target_voltage`, search for that name to find the spot):

```c
ZTEST(vc_channel_state, test_set_field_rejects_retry_max_count_above_history_cap)
{
	zassert_equal(vc_channel_set_field(&ch, VC_FIELD_AUTO_RETRY_MAX_COUNT,
					   CONFIG_VC_MAX_RETRY_HISTORY),
		      VC_OK, "exactly at the cap must be accepted");
	zassert_equal(vc_channel_set_field(&ch, VC_FIELD_AUTO_RETRY_MAX_COUNT,
					   CONFIG_VC_MAX_RETRY_HISTORY + 1),
		      VC_ERR_INVALID_VALUE, "above the cap must be rejected");
}
```

- [ ] **Step 6: Run and verify it fails for the right reason**

```bash
cd /home/yong/backup/src/xlab/jianwei/hvb_wkspc
source .venv/bin/activate
rm -rf /tmp/build_recovery_test
west build -b native_posix -d /tmp/build_recovery_test hvb_firmware.git/tests/voltage_control/vc_channel_state -t run 2>&1 | grep -E "FAIL -|error:"
```

Expected: either a build error (`CONFIG_VC_MAX_RETRY_HISTORY` undeclared, if step 1/2 not yet applied) or `FAIL - test_set_field_rejects_retry_max_count_above_history_cap` (if steps 1-2 done but step 4 not yet applied — the `+1` write would wrongly return `VC_OK`). Do steps 1-4 together before running this if you want a clean single RED; the important thing is confirming the test fails before step 4's validation exists.

- [ ] **Step 7: Run and verify GREEN**

```bash
west build -b native_posix -d /tmp/build_recovery_test hvb_firmware.git/tests/voltage_control/vc_channel_state -t run 2>&1 | grep -E "FAIL -|SUITE "
```

Expected: `SUITE PASS - 100.00% [vc_channel_state]`, no `FAIL -` lines.

- [ ] **Step 8: Commit**

```bash
cd /home/yong/backup/src/xlab/jianwei/hvb_wkspc/hvb_firmware.git
git add include/voltage_control/vc_channel.h lib/voltage_control/Kconfig lib/voltage_control/vc_channel.c tests/voltage_control/vc_channel_state/src/main.c
git commit -m "feat(voltage_control): recovery-policy runtime plumbing (uptime clock, retry history cap)"
```

---

### Task 2: `tick_recovery()` gating skeleton — no-op in every case except armed+eligible

**Files:**
- Modify: `lib/voltage_control/vc_channel.c:280-305` (insert after `is_safe_to_clear_active`, before `vc_channel_meas_ready`)
- Modify: `lib/voltage_control/vc_channel.c:349-369` (`vc_channel_run`, call the new function)
- Modify: `lib/voltage_control/vc_channel.c:280-289` (`force_safe_state`, defensive reset)
- Test: `tests/voltage_control/vc_channel_state/src/main.c`

This step adds the function and every early-return guard, but not the actual retry logic yet (that's Task 3). The point is proving each gate independently: wrong operating mode, wrong fault type, wrong policy, still ramping — none of them should do anything.

- [ ] **Step 1: Write the failing tests**

Add to `tests/voltage_control/vc_channel_state/src/main.c`, after `test_current_protection_graceful_disable_ramps_to_zero`:

```c
static void arm_current_fault(struct vc_channel *ch, enum vc_recovery_policy_mode recovery)
{
	struct vc_channel_config cfg;

	zassert_equal(vc_channel_set_cal_field(ch, VC_CAL_FIELD_MEASURED_I_K, 50000), VC_OK);

	vc_channel_get_config(ch, &cfg);
	cfg.configured_target_voltage = 5000;
	cfg.current_limit_threshold = 100;
	cfg.current_protection_mode = VC_PROTECTION_MODE_APPLY_OUTPUT_ACTION;
	cfg.current_protection_output_action = VC_OUTPUT_ACTION_DISABLE_IMMEDIATE;
	cfg.recovery_policy_mode = recovery;
	cfg.auto_retry_delay = 1;
	cfg.auto_retry_max_count = 3;
	cfg.auto_retry_window = 60;
	cfg.ramp_up_step = 5000;
	cfg.ramp_up_interval = 1;
	vc_channel_set_config(ch, &cfg);
	vc_channel_output_action(ch, VC_OUTPUT_ACTION_ENABLE);
	vc_channel_tick_ramp(ch, 1000, &default_sys);

	vc_channel_consume_current(ch, 5000); /* measured = 250, exceeds threshold 100 */
	zassert_true(ch->active_fault_cause & VC_FAULT_CURRENT);
}

ZTEST(vc_channel_state, test_recovery_stays_latched_in_normal_mode)
{
	arm_current_fault(&ch, VC_RECOVERY_AUTO_RETRY);

	for (int i = 0; i < 20; i++) {
		vc_channel_run(&ch, 1000, &default_sys); /* default_sys = NORMAL mode */
	}

	zassert_true(ch.active_fault_cause & VC_FAULT_CURRENT,
		     "Automatic-only recovery must never act in Normal mode");
	zassert_false(ch.output_enabled);
}

ZTEST(vc_channel_state, test_recovery_stays_latched_with_manual_policy)
{
	struct vc_system_config auto_sys = { .operating_mode = VC_OPERATING_MODE_AUTOMATIC };

	arm_current_fault(&ch, VC_RECOVERY_MANUAL_LATCH);

	for (int i = 0; i < 20; i++) {
		vc_channel_run(&ch, 1000, &auto_sys);
	}

	zassert_true(ch.active_fault_cause & VC_FAULT_CURRENT,
		     "MANUAL_LATCH must never auto-retry even in Automatic mode");
	zassert_false(ch.output_enabled);
}

ZTEST(vc_channel_state, test_recovery_ignores_non_current_fault)
{
	struct vc_system_config auto_sys = { .operating_mode = VC_OPERATING_MODE_AUTOMATIC };
	struct vc_channel_config cfg;

	vc_channel_get_config(&ch, &cfg);
	cfg.recovery_policy_mode = VC_RECOVERY_AUTO_RETRY;
	cfg.auto_retry_delay = 1;
	cfg.auto_retry_max_count = 3;
	cfg.auto_retry_window = 60;
	vc_channel_set_config(&ch, &cfg);
	vc_channel_output_action(&ch, VC_OUTPUT_ACTION_ENABLE);

	vc_channel_consume_fault(&ch, VC_FAULT_HARDWARE);
	zassert_true(ch.active_fault_cause & VC_FAULT_HARDWARE);

	for (int i = 0; i < 20; i++) {
		vc_channel_run(&ch, 1000, &auto_sys);
	}

	zassert_true(ch.active_fault_cause & VC_FAULT_HARDWARE,
		     "hardware faults are never auto-recoverable, regardless of policy");
	zassert_false(ch.output_enabled);
}
```

- [ ] **Step 2: Run and verify it fails to build**

```bash
west build -b native_posix -d /tmp/build_recovery_test hvb_firmware.git/tests/voltage_control/vc_channel_state -t run 2>&1 | tail -20
```

Expected: it currently builds and *passes* trivially, because these are exactly today's (broken) behavior — nothing auto-retries yet, so "stays latched" is already true. That's fine; these three tests exist to pin down the gates and will be joined by Task 3's tests that prove the *opposite* (does retry) once eligible. Confirm they pass now so you have a clean baseline before Task 3 changes behavior.

- [ ] **Step 3: Add the gating skeleton**

In `lib/voltage_control/vc_channel.c`, insert after `is_safe_to_clear_active()` (ends line 304) and before the `/* ---- Measurement callback` comment (line 306):

```c
static bool current_fault_only(const struct vc_channel *ch)
{
	return ch->active_fault_cause == VC_FAULT_CURRENT;
}

static void tick_recovery(struct vc_channel *ch, const struct vc_system_config *sys_cfg,
			   uint32_t dt_ms)
{
	const struct vc_channel_config *cfg = &ch->config;

	if (sys_cfg->operating_mode != VC_OPERATING_MODE_AUTOMATIC) {
		return;
	}
	if (!current_fault_only(ch)) {
		return;
	}
	if (cfg->recovery_policy_mode != VC_RECOVERY_AUTO_RETRY &&
	    cfg->recovery_policy_mode != VC_RECOVERY_AUTO_DERATE_RETRY) {
		return;
	}
	if (ch->ramping) {
		return;
	}

	ARG_UNUSED(dt_ms);
}
```

- [ ] **Step 4: Call it from `vc_channel_run()`**

```c
void vc_channel_run(struct vc_channel *ch, uint32_t dt_ms,
			    const struct vc_system_config *sys_cfg)
{
	ch->uptime_ref += dt_ms;

	if (ch->meas != NULL) {
		int32_t raw_voltage, raw_current;
		uint32_t voltage_ts, current_ts;

		vc_channel_buffer_read(ch->meas, &raw_voltage, &voltage_ts,
				       &raw_current, &current_ts);
		if (voltage_ts != ch->last_consumed_voltage_ts) {
			vc_channel_consume_voltage(ch, raw_voltage);
			ch->last_consumed_voltage_ts = voltage_ts;
		}
		if (current_ts != ch->last_consumed_current_ts) {
			vc_channel_consume_current(ch, raw_current);
			ch->last_consumed_current_ts = current_ts;
		}
	}

	vc_channel_tick_ramp(ch, dt_ms, sys_cfg);
	tick_recovery(ch, sys_cfg, dt_ms);
}
```

- [ ] **Step 5: Defensive reset in `force_safe_state()`**

A hardware/interlock fault (or entering calibration mode) must cancel any in-progress recovery ramp. `force_safe_state()` currently reads:

```c
static void force_safe_state(struct vc_channel *ch)
{
	ch->output_enabled = false;
	ch->ramping = false;
	ch->cal_output_enabled = 0;
	ch->raw_dac_readback = 0;
	ch->operational_target_voltage = 0;
	set_smf_state(ch, VC_CHANNEL_SMF_DISABLED_SAFE);
	apply_hw(ch);
}
```

Change to:

```c
static void force_safe_state(struct vc_channel *ch)
{
	ch->output_enabled = false;
	ch->ramping = false;
	ch->recovering = false;
	ch->cooldown_remaining_ms = 0;
	ch->cal_output_enabled = 0;
	ch->raw_dac_readback = 0;
	ch->operational_target_voltage = 0;
	set_smf_state(ch, VC_CHANNEL_SMF_DISABLED_SAFE);
	apply_hw(ch);
}
```

- [ ] **Step 6: Run tests, verify still green**

```bash
west build -b native_posix -d /tmp/build_recovery_test hvb_firmware.git/tests/voltage_control/vc_channel_state -t run 2>&1 | grep -E "FAIL -|SUITE "
```

Expected: `SUITE PASS - 100.00% [vc_channel_state]`.

- [ ] **Step 7: Commit**

```bash
cd /home/yong/backup/src/xlab/jianwei/hvb_wkspc/hvb_firmware.git
git add lib/voltage_control/vc_channel.c tests/voltage_control/vc_channel_state/src/main.c
git commit -m "feat(voltage_control): tick_recovery() gating skeleton (Automatic mode, current-only, policy check)"
```

---

### Task 3: Cooldown countdown, safe-band gate, and the AUTO_RETRY happy path

**Files:**
- Modify: `lib/voltage_control/vc_channel.c` (`tick_recovery()`)
- Test: `tests/voltage_control/vc_channel_state/src/main.c`

This is the core behavior: once eligible, wait `auto_retry_delay` seconds, then poll the current safe-band each tick until it's satisfied, then clear the fault and start ramping back to the full configured target.

- [ ] **Step 1: Write the failing test**

```c
ZTEST(vc_channel_state, test_recovery_auto_retry_clears_fault_after_cooldown_and_safe_band)
{
	struct vc_system_config auto_sys = { .operating_mode = VC_OPERATING_MODE_AUTOMATIC };

	arm_current_fault(&ch, VC_RECOVERY_AUTO_RETRY);
	zassert_false(ch.output_enabled);

	/* Still above the safe band (threshold 100, 10% band -> safe at <=90).
	 * Ticking through the 1s cooldown must not clear the fault yet. */
	for (int i = 0; i < 3; i++) {
		vc_channel_run(&ch, 1000, &auto_sys);
	}
	zassert_true(ch.active_fault_cause & VC_FAULT_CURRENT,
		     "still unsafe -- must not retry even after cooldown elapses");
	zassert_equal(vc_channel_get_smf_state(&ch), VC_CHANNEL_SMF_RETRY_COOLDOWN);

	/* Current drops to a safe level (raw 100 -> measured 5, well under 90). */
	vc_channel_consume_current(&ch, 100);
	vc_channel_run(&ch, 1000, &auto_sys);

	zassert_equal(ch.active_fault_cause, 0, "safe now -- retry must clear the fault");
	zassert_true(ch.output_enabled);
	zassert_equal(ch.recovery_target, 5000, "AUTO_RETRY targets the full configured value");

	/* Drive the recovery ramp to completion. */
	for (int i = 0; i < 5; i++) {
		vc_channel_run(&ch, 1000, &auto_sys);
	}
	zassert_equal(ch.operational_target_voltage, 5000);
	zassert_false(ch.recovering);
	zassert_equal(vc_channel_get_smf_state(&ch), VC_CHANNEL_SMF_ENABLED_HOLDING);
}
```

- [ ] **Step 2: Run and verify it fails**

```bash
west build -b native_posix -d /tmp/build_recovery_test hvb_firmware.git/tests/voltage_control/vc_channel_state -t run 2>&1 | grep -A3 "test_recovery_auto_retry_clears_fault"
```

Expected: `FAIL` — `ch.active_fault_cause` still nonzero (nothing clears it yet), since `tick_recovery()` is still a no-op past its gates.

- [ ] **Step 3: Implement cooldown + safe-band + retry**

Replace the skeleton body from Task 2 with:

```c
static void tick_recovery(struct vc_channel *ch, const struct vc_system_config *sys_cfg,
			   uint32_t dt_ms)
{
	const struct vc_channel_config *cfg = &ch->config;

	if (sys_cfg->operating_mode != VC_OPERATING_MODE_AUTOMATIC) {
		return;
	}
	if (!current_fault_only(ch)) {
		return;
	}
	if (cfg->recovery_policy_mode != VC_RECOVERY_AUTO_RETRY &&
	    cfg->recovery_policy_mode != VC_RECOVERY_AUTO_DERATE_RETRY) {
		return;
	}
	if (ch->ramping) {
		return;
	}

	if (vc_channel_get_smf_state(ch) != VC_CHANNEL_SMF_RETRY_COOLDOWN) {
		ch->cooldown_remaining_ms = (uint32_t)cfg->auto_retry_delay * 1000;
		set_smf_state(ch, VC_CHANNEL_SMF_RETRY_COOLDOWN);
		update_status_bits(ch);
	}

	if (ch->cooldown_remaining_ms > dt_ms) {
		ch->cooldown_remaining_ms -= dt_ms;
		return;
	}
	ch->cooldown_remaining_ms = 0;

	if (!is_safe_to_clear_active(ch)) {
		return;
	}

	int32_t target = cfg->configured_target_voltage;

	ch->active_fault_cause = 0;
	ch->recovering = true;
	ch->recovery_target = (int16_t)target;
	ch->output_enabled = true;
	apply_hw(ch);
	update_status_bits(ch);
}
```

(Sliding-window counting and derate math land in Tasks 4-5; this step always retries at the full target with no max-count check yet, which is enough to make this test pass.)

- [ ] **Step 4: Wire `vc_channel_tick_ramp()` to honor `recovering`**

`vc_channel_tick_ramp()` currently computes its target as:

```c
	target = ch->ramp_to_disable ? ch->graceful_ramp_dest : cfg->configured_target_voltage;
```

Change to:

```c
	target = ch->ramp_to_disable ? ch->graceful_ramp_dest
	       : ch->recovering ? ch->recovery_target
	       : cfg->configured_target_voltage;
```

Then, in the same function, both places that currently do:

```c
			if (ch->ramp_to_disable) {
				ch->ramp_to_disable = false;
				ch->output_enabled = false;
				set_smf_state(ch, VC_CHANNEL_SMF_DISABLED_SAFE);
				apply_hw(ch);
				update_status_bits(ch);
			} else {
				set_smf_state(ch, VC_CHANNEL_SMF_ENABLED_HOLDING);
			}
```

(the early-return "already at target" branch) and:

```c
		if (ch->ramp_to_disable) {
			ch->ramp_to_disable = false;
			ch->output_enabled = false;
			set_smf_state(ch, VC_CHANNEL_SMF_DISABLED_SAFE);
		} else {
			set_smf_state(ch, VC_CHANNEL_SMF_ENABLED_HOLDING);
		}
```

(the end-of-function "reached target this tick" branch) both need their `else` arm changed from:

```c
			} else {
				set_smf_state(ch, VC_CHANNEL_SMF_ENABLED_HOLDING);
			}
```

to:

```c
			} else {
				ch->recovering = false;
				set_smf_state(ch, VC_CHANNEL_SMF_ENABLED_HOLDING);
			}
```

(and correspondingly for the second, non-early-return occurrence — same `else` body change, matching that block's indentation).

- [ ] **Step 5: Run and verify GREEN**

```bash
west build -b native_posix -d /tmp/build_recovery_test hvb_firmware.git/tests/voltage_control/vc_channel_state -t run 2>&1 | grep -E "FAIL -|SUITE "
```

Expected: `SUITE PASS - 100.00% [vc_channel_state]`.

- [ ] **Step 6: Run the full suite (both channel and no_dac sub-suites) plus domain, to catch regressions**

```bash
rm -rf /tmp/build_domain_recovery
west build -b native_posix -d /tmp/build_domain_recovery hvb_firmware.git/tests/voltage_control/domain -t run 2>&1 | grep -E "FAIL -|SUITE "
```

Expected: both `SUITE PASS`. If `test_current_protection_graceful_disable_ramps_to_zero` or any existing ramp test breaks, it means the `target = ...` ternary or the `else` edits in Step 4 touched the wrong branch — re-check against the exact snippets above.

- [ ] **Step 7: Commit**

```bash
cd /home/yong/backup/src/xlab/jianwei/hvb_wkspc/hvb_firmware.git
git add lib/voltage_control/vc_channel.c tests/voltage_control/vc_channel_state/src/main.c
git commit -m "feat(voltage_control): AUTO_RETRY happy path -- cooldown, safe-band gate, ramp back to target"
```

---

### Task 4: Sliding-window retry counting, max-count exhaustion, and window expiry

**Files:**
- Modify: `lib/voltage_control/vc_channel.c` (`tick_recovery()`, new helpers)
- Modify: `lib/voltage_control/vc_channel.c:522` (`vc_channel_get_snapshot`, real `auto_retry_count`)
- Test: `tests/voltage_control/vc_channel_state/src/main.c`

- [ ] **Step 1: Write the failing tests**

```c
ZTEST(vc_channel_state, test_recovery_exhausts_after_max_retries)
{
	struct vc_system_config auto_sys = { .operating_mode = VC_OPERATING_MODE_AUTOMATIC };

	arm_current_fault(&ch, VC_RECOVERY_AUTO_RETRY);
	ch.config.auto_retry_max_count = 2;

	for (int attempt = 0; attempt < 2; attempt++) {
		vc_channel_consume_current(&ch, 100); /* safe */
		vc_channel_run(&ch, 1000, &auto_sys);  /* cooldown elapses, retries */
		zassert_equal(ch.active_fault_cause, 0,
			      "attempt %d must succeed (under max count)", attempt);

		/* Re-fault immediately so the next attempt has something to retry from. */
		vc_channel_consume_current(&ch, 5000);
		zassert_true(ch.active_fault_cause & VC_FAULT_CURRENT);
	}

	/* Third attempt: max_count (2) already used up inside the window. */
	vc_channel_consume_current(&ch, 100);
	vc_channel_run(&ch, 1000, &auto_sys);

	zassert_true(ch.active_fault_cause & VC_FAULT_RETRY_EXHAUST,
		     "third attempt must exhaust and latch, not retry again");
	zassert_true(ch.active_fault_cause & VC_FAULT_CURRENT);
	zassert_false(ch.output_enabled);
}

ZTEST(vc_channel_state, test_recovery_window_expiry_resets_count)
{
	struct vc_system_config auto_sys = { .operating_mode = VC_OPERATING_MODE_AUTOMATIC };
	struct vc_channel_snapshot snap;

	arm_current_fault(&ch, VC_RECOVERY_AUTO_RETRY);
	ch.config.auto_retry_max_count = 1;
	ch.config.auto_retry_window = 5; /* seconds */

	vc_channel_consume_current(&ch, 100);
	vc_channel_run(&ch, 1000, &auto_sys); /* first (and only allowed) retry */
	zassert_equal(ch.active_fault_cause, 0);

	vc_channel_get_snapshot(&ch, &snap);
	zassert_equal(snap.auto_retry_count, 1);

	/* Advance well past the 5s window with no further faults. */
	for (int i = 0; i < 10; i++) {
		vc_channel_run(&ch, 1000, &auto_sys);
	}

	vc_channel_get_snapshot(&ch, &snap);
	zassert_equal(snap.auto_retry_count, 0,
		      "retry timestamps older than the window must age out");
}
```

- [ ] **Step 2: Run and verify both fail**

```bash
west build -b native_posix -d /tmp/build_recovery_test hvb_firmware.git/tests/voltage_control/vc_channel_state -t run 2>&1 | grep -A3 "test_recovery_exhausts_after_max_retries\|test_recovery_window_expiry"
```

Expected: `test_recovery_exhausts_after_max_retries` fails (a third retry currently always succeeds, no max-count check yet). `test_recovery_window_expiry_resets_count` fails at the `snap.auto_retry_count, 1` check (still hardcoded to 0).

- [ ] **Step 3: Implement the sliding-window helpers and wire them in**

Insert directly above `tick_recovery()` (same location as before, after `is_safe_to_clear_active()`):

```c
static uint16_t count_active_retries(const struct vc_channel *ch)
{
	uint32_t window_ms = (uint32_t)ch->config.auto_retry_window * 1000;
	uint16_t count = 0;

	for (uint8_t i = 0; i < ch->retry_timestamp_count; i++) {
		if (ch->uptime_ref - ch->retry_timestamps_ms[i] <= window_ms) {
			count++;
		}
	}
	return count;
}

static void record_retry_attempt(struct vc_channel *ch)
{
	if (ch->retry_timestamp_count < CONFIG_VC_MAX_RETRY_HISTORY) {
		ch->retry_timestamps_ms[ch->retry_timestamp_count++] = ch->uptime_ref;
		return;
	}
	memmove(&ch->retry_timestamps_ms[0], &ch->retry_timestamps_ms[1],
		sizeof(ch->retry_timestamps_ms[0]) * (CONFIG_VC_MAX_RETRY_HISTORY - 1));
	ch->retry_timestamps_ms[CONFIG_VC_MAX_RETRY_HISTORY - 1] = ch->uptime_ref;
}
```

Then update `tick_recovery()`'s tail (everything from the `is_safe_to_clear_active` check onward):

```c
	if (!is_safe_to_clear_active(ch)) {
		return;
	}

	uint16_t retry_count = count_active_retries(ch);

	if (retry_count >= cfg->auto_retry_max_count) {
		ch->active_fault_cause |= VC_FAULT_RETRY_EXHAUST;
		set_smf_state(ch, VC_CHANNEL_SMF_FAULT_LATCHED);
		update_status_bits(ch);
		return;
	}

	int32_t target = cfg->configured_target_voltage;

	record_retry_attempt(ch);
	ch->active_fault_cause = 0;
	ch->recovering = true;
	ch->recovery_target = (int16_t)target;
	ch->output_enabled = true;
	apply_hw(ch);
	update_status_bits(ch);
```

Note `record_retry_attempt(ch)` is called for every attempt (whether it turns out to need derating or not) -- Task 5 inserts the derate math between the `retry_count >= max_count` check and this point, so record the attempt only once that's settled. For this task, the ordering above (record right before committing to retry) is correct.

- [ ] **Step 4: Fix `vc_channel_get_snapshot()`'s hardcoded retry count**

```c
	snap->auto_retry_count = 0;
```

becomes:

```c
	snap->auto_retry_count = count_active_retries(ch);
```

- [ ] **Step 5: Run and verify GREEN**

```bash
west build -b native_posix -d /tmp/build_recovery_test hvb_firmware.git/tests/voltage_control/vc_channel_state -t run 2>&1 | grep -E "FAIL -|SUITE "
```

Expected: `SUITE PASS - 100.00% [vc_channel_state]`.

- [ ] **Step 6: Commit**

```bash
cd /home/yong/backup/src/xlab/jianwei/hvb_wkspc/hvb_firmware.git
git add lib/voltage_control/vc_channel.c tests/voltage_control/vc_channel_state/src/main.c
git commit -m "feat(voltage_control): sliding-window retry count, max-count exhaustion, real snapshot count"
```

---

### Task 5: AUTO_DERATE_RETRY target stepping and floor exhaustion

**Files:**
- Modify: `lib/voltage_control/vc_channel.c` (`tick_recovery()`)
- Test: `tests/voltage_control/vc_channel_state/src/main.c`

Per spec: "Auto-derate lowers Operational Target Voltage by Auto Derate Step per retry. If derating would push Operational Target Voltage below the variant minimum, latch the channel with an Active Fault Block." This codebase has no separate "variant minimum" concept below 0, so the floor is 0: a derated target `<= 0` is treated as exhausted.

- [ ] **Step 1: Write the failing tests**

```c
ZTEST(vc_channel_state, test_recovery_auto_derate_lowers_target_each_retry)
{
	struct vc_system_config auto_sys = { .operating_mode = VC_OPERATING_MODE_AUTOMATIC };

	arm_current_fault(&ch, VC_RECOVERY_AUTO_DERATE_RETRY);
	ch.config.auto_derate_step = 1000;
	ch.config.auto_retry_max_count = 5;

	vc_channel_consume_current(&ch, 100); /* safe */
	vc_channel_run(&ch, 1000, &auto_sys);

	zassert_equal(ch.active_fault_cause, 0);
	zassert_equal(ch.recovery_target, 4000,
		      "first derate retry = configured(5000) - 1*step(1000)");
}

ZTEST(vc_channel_state, test_recovery_auto_derate_exhausts_at_floor)
{
	struct vc_system_config auto_sys = { .operating_mode = VC_OPERATING_MODE_AUTOMATIC };

	arm_current_fault(&ch, VC_RECOVERY_AUTO_DERATE_RETRY);
	ch.config.configured_target_voltage = 1000;
	ch.config.auto_derate_step = 2000; /* one derate step already exceeds the target */
	ch.config.auto_retry_max_count = 5;

	vc_channel_consume_current(&ch, 100); /* safe */
	vc_channel_run(&ch, 1000, &auto_sys);

	zassert_true(ch.active_fault_cause & VC_FAULT_RETRY_EXHAUST,
		     "derating below zero must exhaust immediately, not retry at a negative target");
	zassert_false(ch.output_enabled);
}
```

- [ ] **Step 2: Run and verify both fail**

```bash
west build -b native_posix -d /tmp/build_recovery_test hvb_firmware.git/tests/voltage_control/vc_channel_state -t run 2>&1 | grep -A3 "test_recovery_auto_derate"
```

Expected: `test_recovery_auto_derate_lowers_target_each_retry` fails (`recovery_target` is 5000, not 4000 — derate math doesn't exist yet). `test_recovery_auto_derate_exhausts_at_floor` fails (channel retries at a negative/invalid target instead of exhausting).

- [ ] **Step 3: Implement derate stepping**

In `tick_recovery()`, replace:

```c
	uint16_t retry_count = count_active_retries(ch);

	if (retry_count >= cfg->auto_retry_max_count) {
		ch->active_fault_cause |= VC_FAULT_RETRY_EXHAUST;
		set_smf_state(ch, VC_CHANNEL_SMF_FAULT_LATCHED);
		update_status_bits(ch);
		return;
	}

	int32_t target = cfg->configured_target_voltage;

	record_retry_attempt(ch);
```

with:

```c
	uint16_t retry_count = count_active_retries(ch);

	if (retry_count >= cfg->auto_retry_max_count) {
		ch->active_fault_cause |= VC_FAULT_RETRY_EXHAUST;
		set_smf_state(ch, VC_CHANNEL_SMF_FAULT_LATCHED);
		update_status_bits(ch);
		return;
	}

	int32_t target = cfg->configured_target_voltage;

	if (cfg->recovery_policy_mode == VC_RECOVERY_AUTO_DERATE_RETRY) {
		target -= (int32_t)(retry_count + 1) * cfg->auto_derate_step;
		if (target <= 0) {
			ch->active_fault_cause |= VC_FAULT_RETRY_EXHAUST;
			set_smf_state(ch, VC_CHANNEL_SMF_FAULT_LATCHED);
			update_status_bits(ch);
			return;
		}
	}

	record_retry_attempt(ch);
```

- [ ] **Step 4: Run and verify GREEN**

```bash
west build -b native_posix -d /tmp/build_recovery_test hvb_firmware.git/tests/voltage_control/vc_channel_state -t run 2>&1 | grep -E "FAIL -|SUITE "
```

Expected: `SUITE PASS - 100.00% [vc_channel_state]`.

- [ ] **Step 5: Commit**

```bash
cd /home/yong/backup/src/xlab/jianwei/hvb_wkspc/hvb_firmware.git
git add lib/voltage_control/vc_channel.c tests/voltage_control/vc_channel_state/src/main.c
git commit -m "feat(voltage_control): AUTO_DERATE_RETRY target stepping and floor exhaustion"
```

---

### Task 6: Domain-level integration test

**Files:**
- Modify: `tests/voltage_control/domain/src/main.c`

Prove the whole thing works end to end through `vc_controller_tick()` (not just direct `vc_channel_run()` calls), and that switching *out* of Automatic mode stops any in-progress recovery from continuing to act on old faults per spec ("Automatic recovery applies only to faults detected while Automatic Mode is already active").

- [ ] **Step 1: Write the failing test**

Add near `test_current_protection_skipped_during_ramping` in `tests/voltage_control/domain/src/main.c`:

```c
ZTEST(vc_domain, test_recovery_auto_retry_through_controller_tick)
{
	make_fresh();
	struct vc_channel_config cfg;
	struct vc_channel_snapshot snap;

	zassert_equal(vc_channel_set_cal_field(&ctrl->channels[0], VC_CAL_FIELD_MEASURED_I_K,
						50000), VC_OK);

	vc_controller_get_channel_config(ctrl, 0, &cfg);
	cfg.configured_target_voltage = 5000;
	cfg.current_limit_threshold = 100;
	cfg.current_protection_mode = VC_PROTECTION_MODE_APPLY_OUTPUT_ACTION;
	cfg.current_protection_output_action = VC_OUTPUT_ACTION_DISABLE_IMMEDIATE;
	cfg.recovery_policy_mode = VC_RECOVERY_AUTO_RETRY;
	cfg.auto_retry_delay = 1;
	cfg.auto_retry_max_count = 3;
	cfg.auto_retry_window = 60;
	cfg.ramp_up_step = 5000;
	cfg.ramp_up_interval = 1;
	vc_channel_set_config(&ctrl->channels[0], &cfg);

	zassert_equal(vc_controller_set_operating_mode(ctrl, VC_OPERATING_MODE_AUTOMATIC),
		      VC_OK);
	vc_controller_channel_output_action(ctrl, 0, VC_OUTPUT_ACTION_ENABLE);
	vc_controller_tick(ctrl, 1000);

	vc_channel_consume_current(&ctrl->channels[0], 5000); /* trips the fault */
	vc_controller_tick(ctrl, 0);
	vc_controller_get_channel_snapshot(ctrl, 0, &snap);
	zassert_true(snap.active_fault_cause & VC_FAULT_CURRENT);

	vc_channel_consume_current(&ctrl->channels[0], 100); /* safe */
	vc_controller_tick(ctrl, 1000); /* cooldown elapses, retry fires */

	vc_controller_get_channel_snapshot(ctrl, 0, &snap);
	zassert_equal(snap.active_fault_cause, 0,
		      "auto-retry must clear the fault through the normal controller tick path");
}
```

- [ ] **Step 2: Run and verify it fails**

```bash
rm -rf /tmp/build_domain_recovery
west build -b native_posix -d /tmp/build_domain_recovery hvb_firmware.git/tests/voltage_control/domain -t run 2>&1 | grep -A3 "test_recovery_auto_retry_through_controller_tick"
```

Expected: this should actually already pass if Tasks 1-5 are done correctly, since `vc_controller_tick()` just calls `vc_channel_run()` per channel — there's no new production code in this task. If it fails, it's telling you something about `vc_controller_tick`/`vc_channel_run` wiring that the channel-level tests didn't catch (e.g. `sys_cfg` not threaded through correctly) — investigate before proceeding, don't just add code to force it green.

- [ ] **Step 3: If it already passes, that's the expected outcome for this task — commit as a regression test**

```bash
cd /home/yong/backup/src/xlab/jianwei/hvb_wkspc/hvb_firmware.git
git add tests/voltage_control/domain/src/main.c
git commit -m "test(voltage_control): domain-level integration test for auto-retry via controller tick"
```

---

### Task 7: Documentation and final verification

**Files:**
- Modify: `docs/guide/parameter-reference.md`

- [ ] **Step 1: Document the now-real behavior**

In `docs/guide/parameter-reference.md`, find the `## Current Protection` section (added in an earlier session) and add a new section immediately after it:

```markdown
---

## Automatic Recovery

`recovery_policy_mode` only has an effect in **Automatic operating mode** — in
Normal mode every fault always latches and requires an explicit manual clear,
regardless of this setting. This matches the "explicit user action always
wins" behavior of manual host commands.

| Policy | Behavior |
|---|---|
| `MANUAL_LATCH` (default) | Fault always latches; manual clear required. |
| `NEVER_RETRY` | Same as `MANUAL_LATCH` in this implementation. |
| `AUTO_RETRY` | After `auto_retry_delay` seconds and once `measured_current` is back within the current safe band, clears the fault and ramps back to `configured_target_voltage`. |
| `AUTO_DERATE_RETRY` | Same as `AUTO_RETRY`, but each retry targets `configured_target_voltage - (attempt_number × auto_derate_step)`. If that would reach zero or below, the channel exhausts immediately instead of retrying at an invalid target. |

Only `VC_FAULT_CURRENT` is auto-recoverable — hardware, interlock, measurement,
and stale-data faults always require a manual clear, because this codebase
only has a "safe now?" check (the current safe-band) for current faults.

Retry attempts are counted in a sliding window: attempts older than
`auto_retry_window` seconds age out and don't count against
`auto_retry_max_count`. Exceeding the max count (or, for derate retry,
derating to zero or below) latches the channel with `VC_FAULT_RETRY_EXHAUST`
in addition to the original fault cause — that combination means "auto-retry
gave up," not "still faulted, waiting for a retry."
```

- [ ] **Step 2: Full regression run — both suites, both boards**

```bash
cd /home/yong/backup/src/xlab/jianwei/hvb_wkspc
source .venv/bin/activate
rm -rf /tmp/build_vcs_final /tmp/build_domain_final
west build -b native_posix -d /tmp/build_vcs_final hvb_firmware.git/tests/voltage_control/vc_channel_state -t run 2>&1 | grep -E "FAIL -|SUITE "
west build -b native_posix -d /tmp/build_domain_final hvb_firmware.git/tests/voltage_control/domain -t run 2>&1 | grep -E "FAIL -|SUITE "
```

Expected: both `SUITE PASS - 100.00%`, zero `FAIL -` lines.

- [ ] **Step 3: Full jw_hvb firmware build**

```bash
rm -rf build_hvb_controller
west build -b jw_hvb -d build_hvb_controller hvb_firmware.git/applications/hvb_controller 2>&1 | tail -12
```

Expected: links successfully, memory usage printed, no errors.

- [ ] **Step 4: Commit**

```bash
cd /home/yong/backup/src/xlab/jianwei/hvb_wkspc/hvb_firmware.git
git add docs/guide/parameter-reference.md
git commit -m "docs(voltage_control): document Automatic Recovery behavior"
```

---

## Deliberately out of scope for this plan

- **Distinguishing "fault detected while already in Automatic mode" from "fault detected in Normal mode, then switched to Automatic."** The spec requires the former to be retryable and the latter to stay manual-latch forever. This plan's mode gate checks only the current tick's operating mode, so both cases are treated identically (retryable once mode is Automatic). Fixing this needs a mode-transition timestamp on `vc_system_config`, threaded into `vc_channel_consume_current()` (which doesn't currently receive `sys_cfg`), compared against `last_fault_timestamp`. Noted here so it isn't mistaken for an oversight; see the Architecture section above for the same point in more detail.
- **Voltage-based recovery gating.** The design spec mentions a "Voltage Safe Band %" setting, but no `voltage_safe_band_pct` config field exists in this codebase and there's no voltage-limit protection mechanism to recover from in the first place (only current-limit protection is implemented). Adding voltage protection is a separate, larger feature.
- **TUI/host-tool exposure of the new `VC_FAULT_RETRY_EXHAUST` cause and `RETRY_COOLDOWN` SMF state in the Monitor table.** `faultStr()` in `tools/hvb_demo_app/tui/tui_format.h` already has a case for `FaultCause::RETRY_EXHAUST` (`"RE"`) and `statusBadge()` doesn't currently special-case cooldown — once this plan lands, cooldown will show as neither ON nor OFF in the existing status button logic. Worth a follow-up pass once the firmware behavior exists to test against.
- **Persisting in-flight recovery state (`recovering`, `retry_timestamps_ms`) across a reboot.** Per spec, "Active Fault Blocks and Fault History" are runtime-only; recovery bookkeeping is naturally the same category and isn't listed as needing persistence.
