# Async Runtime SMF Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the voltage-control runtime a serialized asynchronous domain writer and introduce Zephyr `smf` structure for domain/channel state transitions.

**Architecture:** `vc_runtime` owns a dedicated worker thread, command queue, evidence queue, and tick timing. Production mutation enters through queued runtime commands; the worker calls existing domain APIs in FIFO order. Zephyr `smf` is introduced inside the domain as policy structure, not as a message bus or provider interface.

**Tech Stack:** Zephyr 3.7.2, `k_thread`, `k_msgq`, `k_sem`, `k_mutex`, `k_poll_signal`, `smf`, ztest, native_posix, `jw_hvb` board build.

---

## File Structure

- Modify `include/voltage_control/runtime.h`: add command types, command payloads, blocking command submission API, runtime destroy semantics, and tick interval config surface.
- Modify `lib/voltage_control/runtime.c`: replace direct mutex-only facade with a worker thread, command queue, evidence queue, stop path, and serialized domain API dispatch.
- Modify `lib/voltage_control/Kconfig`: add runtime stack size, thread priority, tick interval, and queue depth defaults.
- Modify `tests/voltage_control/runtime/prj.conf`: set small queue depths for overflow tests and deterministic runtime behavior.
- Modify `tests/voltage_control/runtime/src/main.c`: add TDD coverage for queued commands, FIFO processing, queued evidence, overflow, and clean stop.
- Modify `lib/voltage_control/domain.c`: add internal `smf` state definitions and transition scaffolding while preserving public behavior.
- Modify `tests/voltage_control/domain/src/main.c`: add public behavior tests around mode/channel transitions that must keep passing after `smf` is introduced.
- Modify `applications/hvb_controller/src/main.c`: remove direct heartbeat-time domain mutation and create runtime with the worker active.

---

### Task 1: Runtime Command Contracts

**Files:**
- Modify: `include/voltage_control/runtime.h`
- Test: `tests/voltage_control/runtime/src/main.c`

- [ ] **Step 1: Add failing compile test for command contracts**

Append this test after `test_runtime_create_and_destroy` in `tests/voltage_control/runtime/src/main.c`:

```c
ZTEST(voltage_control_runtime, test_runtime_command_contract_defaults_are_zeroable)
{
	struct vc_runtime_command cmd = {0};

	zassert_equal(cmd.type, VC_RUNTIME_CMD_SET_OPERATING_MODE);
	zassert_equal(cmd.channel, 0);
	zassert_equal(cmd.payload.operating_mode, VC_OPERATING_MODE_NORMAL);
	zassert_is_null(cmd.result_sem);
	zassert_is_null(cmd.result);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `/home/yong/backup/src/xlab/jianwei/hvb_wkspc/.venv/bin/west build -b native_posix -d build/test-runtime tests/voltage_control/runtime`

Expected: FAIL at compile time because `struct vc_runtime_command` and `VC_RUNTIME_CMD_SET_OPERATING_MODE` do not exist.

- [ ] **Step 3: Add runtime command contracts**

Modify `include/voltage_control/runtime.h` by inserting this block after `struct vc_measurement_snapshot`:

```c
enum vc_runtime_command_type {
	VC_RUNTIME_CMD_SET_OPERATING_MODE = 0,
	VC_RUNTIME_CMD_SET_SYSTEM_CONFIG,
	VC_RUNTIME_CMD_SET_CHANNEL_CONFIG,
	VC_RUNTIME_CMD_OUTPUT_ACTION,
	VC_RUNTIME_CMD_FAULT_COMMAND,
	VC_RUNTIME_CMD_CALIBRATION_UNLOCK,
	VC_RUNTIME_CMD_CALIBRATION_OUTPUT_ENABLE,
	VC_RUNTIME_CMD_CALIBRATION_RAW_DAC,
	VC_RUNTIME_CMD_CALIBRATION_SAMPLE,
	VC_RUNTIME_CMD_CALIBRATION_COMMIT,
	VC_RUNTIME_CMD_CALIBRATION_MAX_RAW_DAC,
	VC_RUNTIME_CMD_SYSTEM_PARAM_ACTION,
	VC_RUNTIME_CMD_CHANNEL_PARAM_ACTION,
	VC_RUNTIME_CMD_SET_UPTIME,
};

struct vc_runtime_command {
	enum vc_runtime_command_type type;
	uint8_t channel;
	union {
		enum vc_operating_mode operating_mode;
		struct vc_system_config system_config;
		struct vc_channel_config channel_config;
		enum vc_output_action output_action;
		enum vc_channel_fault_command fault_command;
		uint16_t calibration_unlock_value;
		bool calibration_output_enable;
		uint16_t calibration_raw_dac;
		uint16_t calibration_max_raw_dac;
		enum vc_param_action param_action;
		uint32_t uptime_seconds;
	} payload;
	struct k_sem *result_sem;
	enum vc_status *result;
};
```

Modify the API declarations near the bottom of `include/voltage_control/runtime.h` to include:

```c
enum vc_status vc_runtime_submit_command(struct vc_runtime *runtime,
						 const struct vc_runtime_command *cmd,
						 k_timeout_t timeout);
enum vc_status vc_runtime_set_operating_mode(struct vc_runtime *runtime,
					     enum vc_operating_mode mode,
					     k_timeout_t timeout);
enum vc_status vc_runtime_set_uptime(struct vc_runtime *runtime,
					 uint32_t seconds,
					 k_timeout_t timeout);
```

- [ ] **Step 4: Run test to verify it passes**

Run: `/home/yong/backup/src/xlab/jianwei/hvb_wkspc/.venv/bin/west build -b native_posix -d build/test-runtime tests/voltage_control/runtime && ./build/test-runtime/zephyr/zephyr.exe`

Expected: PASS, including `test_runtime_command_contract_defaults_are_zeroable`.

- [ ] **Step 5: Commit**

```bash
git add include/voltage_control/runtime.h tests/voltage_control/runtime/src/main.c
git commit -m "feat: add runtime command contracts"
```

---

### Task 2: Runtime Worker Thread Lifecycle

**Files:**
- Modify: `lib/voltage_control/Kconfig`
- Modify: `lib/voltage_control/runtime.c`
- Modify: `tests/voltage_control/runtime/prj.conf`
- Test: `tests/voltage_control/runtime/src/main.c`

- [ ] **Step 1: Add failing test for clean runtime stop**

Append this test after `test_runtime_create_and_destroy`:

```c
ZTEST(voltage_control_runtime, test_runtime_destroy_stops_worker_thread)
{
	struct domain *domain = domain_create(test_channels, 1);
	struct vc_runtime *runtime;

	zassert_not_null(domain);
	runtime = vc_runtime_create(domain);
	zassert_not_null(runtime);

	vc_runtime_destroy(runtime);
	zassert_equal(domain_get_supported_channel_count(domain), 1);

	free(domain);
}
```

- [ ] **Step 2: Add runtime Kconfig symbols**

Append to `lib/voltage_control/Kconfig` after `VC_RUNTIME_EVIDENCE_QUEUE_DEPTH`:

```kconfig
config VC_RUNTIME_THREAD_STACK_SIZE
	int "VC runtime worker stack size"
	default 2048
	range 1024 8192
	depends on VC_RUNTIME
	help
	  Stack size in bytes for the voltage-control runtime worker.

config VC_RUNTIME_THREAD_PRIORITY
	int "VC runtime worker priority"
	default 5
	depends on VC_RUNTIME
	help
	  Zephyr thread priority for the voltage-control runtime worker.

config VC_RUNTIME_TICK_INTERVAL_MS
	int "VC runtime policy tick interval"
	default 100
	range 1 60000
	depends on VC_RUNTIME
	help
	  Runtime policy tick interval in milliseconds.
```

Modify `tests/voltage_control/runtime/prj.conf` to:

```conf
CONFIG_ZTEST=y
CONFIG_VC_RUNTIME=y
CONFIG_VC_RUNTIME_COMMAND_QUEUE_DEPTH=2
CONFIG_VC_RUNTIME_EVIDENCE_QUEUE_DEPTH=2
CONFIG_VC_RUNTIME_THREAD_STACK_SIZE=2048
CONFIG_VC_RUNTIME_THREAD_PRIORITY=5
CONFIG_VC_RUNTIME_TICK_INTERVAL_MS=1000
```

- [ ] **Step 3: Implement worker lifecycle with no command dispatch yet**

Replace `struct vc_runtime` and lifecycle functions in `lib/voltage_control/runtime.c` with this shape:

```c
K_KERNEL_STACK_DEFINE(vc_runtime_stack, CONFIG_VC_RUNTIME_THREAD_STACK_SIZE);

struct vc_runtime_work_item {
	struct vc_runtime_command command;
};

struct vc_runtime_evidence_item {
	struct vc_measurement_snapshot measurement;
};

struct vc_runtime {
	struct domain *domain;
	struct k_mutex lock;
	struct k_msgq command_queue;
	struct k_msgq evidence_queue;
	struct k_sem wake;
	struct k_thread thread;
	k_tid_t tid;
	bool stop_requested;
	char command_buffer[CONFIG_VC_RUNTIME_COMMAND_QUEUE_DEPTH * sizeof(struct vc_runtime_work_item)];
	char evidence_buffer[CONFIG_VC_RUNTIME_EVIDENCE_QUEUE_DEPTH * sizeof(struct vc_runtime_evidence_item)];
};

static enum vc_status vc_runtime_dispatch_command(struct vc_runtime *runtime,
							  const struct vc_runtime_command *cmd)
{
	ARG_UNUSED(runtime);
	ARG_UNUSED(cmd);
	return VC_ERR_INVALID_COMMAND;
}

static void vc_runtime_worker(void *p1, void *p2, void *p3)
{
	struct vc_runtime *runtime = p1;
	struct vc_runtime_work_item work;
	struct vc_runtime_evidence_item evidence;
	int wake_ret;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (!runtime->stop_requested) {
		wake_ret = k_sem_take(&runtime->wake, K_MSEC(CONFIG_VC_RUNTIME_TICK_INTERVAL_MS));

		while (k_msgq_get(&runtime->command_queue, &work, K_NO_WAIT) == 0) {
			enum vc_status result;

			k_mutex_lock(&runtime->lock, K_FOREVER);
			result = vc_runtime_dispatch_command(runtime, &work.command);
			k_mutex_unlock(&runtime->lock);

			if (work.command.result != NULL) {
				*work.command.result = result;
			}
			if (work.command.result_sem != NULL) {
				k_sem_give(work.command.result_sem);
			}
		}

		while (k_msgq_get(&runtime->evidence_queue, &evidence, K_NO_WAIT) == 0) {
			k_mutex_lock(&runtime->lock, K_FOREVER);
			(void)domain_consume_measurement(runtime->domain, &evidence.measurement);
			k_mutex_unlock(&runtime->lock);
		}


		if (wake_ret == -EAGAIN) {
			k_mutex_lock(&runtime->lock, K_FOREVER);
			domain_tick(runtime->domain, CONFIG_VC_RUNTIME_TICK_INTERVAL_MS, NULL, NULL);
			k_mutex_unlock(&runtime->lock);
		}
	}
}
```

Replace `vc_runtime_create()` body after allocation with:

```c
runtime->domain = domain;
runtime->stop_requested = false;
k_mutex_init(&runtime->lock);
k_sem_init(&runtime->wake, 0, 1);
k_msgq_init(&runtime->command_queue, runtime->command_buffer,
	    sizeof(struct vc_runtime_work_item), CONFIG_VC_RUNTIME_COMMAND_QUEUE_DEPTH);
k_msgq_init(&runtime->evidence_queue, runtime->evidence_buffer,
	    sizeof(struct vc_runtime_evidence_item), CONFIG_VC_RUNTIME_EVIDENCE_QUEUE_DEPTH);
runtime->tid = k_thread_create(&runtime->thread, vc_runtime_stack,
				       K_KERNEL_STACK_SIZEOF(vc_runtime_stack),
				       vc_runtime_worker, runtime, NULL, NULL,
				       CONFIG_VC_RUNTIME_THREAD_PRIORITY, 0, K_NO_WAIT);
```

Replace `vc_runtime_destroy()` with:

```c
void vc_runtime_destroy(struct vc_runtime *runtime)
{
	if (runtime == NULL) {
		return;
	}

	runtime->stop_requested = true;
	k_sem_give(&runtime->wake);
	(void)k_thread_join(&runtime->thread, K_FOREVER);
	free(runtime);
}
```

- [ ] **Step 4: Run test to verify lifecycle passes**

Run: `/home/yong/backup/src/xlab/jianwei/hvb_wkspc/.venv/bin/west build -b native_posix -d build/test-runtime tests/voltage_control/runtime && ./build/test-runtime/zephyr/zephyr.exe`

Expected: PASS for create/destroy tests. Existing measurement tests may fail because measurement submission is not queued yet; if so, continue to Task 4 before finalizing runtime behavior.

- [ ] **Step 5: Commit**

```bash
git add lib/voltage_control/Kconfig lib/voltage_control/runtime.c tests/voltage_control/runtime/prj.conf tests/voltage_control/runtime/src/main.c
git commit -m "feat: start voltage-control runtime worker"
```

---

### Task 3: Blocking Runtime Command Submission

**Files:**
- Modify: `lib/voltage_control/runtime.c`
- Test: `tests/voltage_control/runtime/src/main.c`

- [ ] **Step 1: Add failing test for queued operating mode command**

Append this test after `test_runtime_command_contract_defaults_are_zeroable`:

```c
ZTEST(voltage_control_runtime, test_runtime_set_operating_mode_is_processed_by_worker)
{
	struct domain *d = domain_create(test_channels, 1);
	struct vc_runtime *rt;

	zassert_not_null(d);
	rt = vc_runtime_create(d);
	zassert_not_null(rt);

	zassert_equal(vc_runtime_set_operating_mode(rt, VC_OPERATING_MODE_AUTOMATIC, K_SECONDS(1)), VC_OK);
	zassert_equal(domain_get_operating_mode(d), VC_OPERATING_MODE_AUTOMATIC);

	vc_runtime_destroy(rt);
	free(d);
}
```

- [ ] **Step 2: Add failing test for queue overflow**

Append this test after the queued operating mode test:

```c
ZTEST(voltage_control_runtime, test_runtime_submit_command_rejects_null)
{
	struct domain *d = domain_create(test_channels, 1);
	struct vc_runtime *rt;

	zassert_not_null(d);
	rt = vc_runtime_create(d);
	zassert_not_null(rt);

	zassert_equal(vc_runtime_submit_command(NULL, NULL, K_NO_WAIT), VC_ERR_INVALID_VALUE);
	zassert_equal(vc_runtime_submit_command(rt, NULL, K_NO_WAIT), VC_ERR_INVALID_VALUE);

	vc_runtime_destroy(rt);
	free(d);
}
```

- [ ] **Step 3: Implement command dispatch**

Replace `vc_runtime_dispatch_command()` in `lib/voltage_control/runtime.c` with:

```c
static enum vc_status vc_runtime_dispatch_command(struct vc_runtime *runtime,
							  const struct vc_runtime_command *cmd)
{
	switch (cmd->type) {
	case VC_RUNTIME_CMD_SET_OPERATING_MODE:
		return domain_set_operating_mode(runtime->domain, cmd->payload.operating_mode);
	case VC_RUNTIME_CMD_SET_SYSTEM_CONFIG:
		return domain_set_system_config(runtime->domain, &cmd->payload.system_config);
	case VC_RUNTIME_CMD_SET_CHANNEL_CONFIG:
		return domain_set_channel_config(runtime->domain, cmd->channel,
						 &cmd->payload.channel_config);
	case VC_RUNTIME_CMD_OUTPUT_ACTION:
		return domain_channel_output_action(runtime->domain, cmd->channel,
						    cmd->payload.output_action);
	case VC_RUNTIME_CMD_FAULT_COMMAND:
		return domain_channel_fault_command(runtime->domain, cmd->channel,
						    cmd->payload.fault_command);
	case VC_RUNTIME_CMD_CALIBRATION_UNLOCK:
		return domain_calibration_unlock(runtime->domain,
						 cmd->payload.calibration_unlock_value);
	case VC_RUNTIME_CMD_CALIBRATION_OUTPUT_ENABLE:
		return domain_calibration_set_output_enable(runtime->domain, cmd->channel,
							    cmd->payload.calibration_output_enable);
	case VC_RUNTIME_CMD_CALIBRATION_RAW_DAC:
		return domain_calibration_set_raw_dac(runtime->domain, cmd->channel,
						      cmd->payload.calibration_raw_dac);
	case VC_RUNTIME_CMD_CALIBRATION_SAMPLE:
		return domain_calibration_sample(runtime->domain, cmd->channel);
	case VC_RUNTIME_CMD_CALIBRATION_COMMIT:
		return domain_calibration_commit(runtime->domain, cmd->channel);
	case VC_RUNTIME_CMD_CALIBRATION_MAX_RAW_DAC:
		return domain_calibration_set_max_raw_dac(runtime->domain, cmd->channel,
							 cmd->payload.calibration_max_raw_dac);
	case VC_RUNTIME_CMD_SYSTEM_PARAM_ACTION:
		return domain_system_param_action(runtime->domain, cmd->payload.param_action);
	case VC_RUNTIME_CMD_CHANNEL_PARAM_ACTION:
		return domain_channel_param_action(runtime->domain, cmd->channel,
						   cmd->payload.param_action);
	case VC_RUNTIME_CMD_SET_UPTIME:
		domain_set_uptime(runtime->domain, cmd->payload.uptime_seconds);
		return VC_OK;
	default:
		return VC_ERR_INVALID_COMMAND;
	}
}
```

Add this implementation below `vc_runtime_destroy()`:

```c
enum vc_status vc_runtime_submit_command(struct vc_runtime *runtime,
						 const struct vc_runtime_command *cmd,
						 k_timeout_t timeout)
{
	struct vc_runtime_work_item work;
	struct k_sem result_sem;
	enum vc_status result = VC_ERR_UNSAFE_STATE;

	if (runtime == NULL || cmd == NULL) {
		return VC_ERR_INVALID_VALUE;
	}

	work.command = *cmd;
	work.command.result = &result;
	work.command.result_sem = &result_sem;
	k_sem_init(&result_sem, 0, 1);

	if (k_msgq_put(&runtime->command_queue, &work, timeout) != 0) {
		return VC_ERR_UNSAFE_STATE;
	}
	k_sem_give(&runtime->wake);

	if (k_sem_take(&result_sem, timeout) != 0) {
		return VC_ERR_UNSAFE_STATE;
	}

	return result;
}

enum vc_status vc_runtime_set_operating_mode(struct vc_runtime *runtime,
					     enum vc_operating_mode mode,
					     k_timeout_t timeout)
{
	struct vc_runtime_command cmd = {
		.type = VC_RUNTIME_CMD_SET_OPERATING_MODE,
		.payload.operating_mode = mode,
	};

	return vc_runtime_submit_command(runtime, &cmd, timeout);
}

enum vc_status vc_runtime_set_uptime(struct vc_runtime *runtime,
					 uint32_t seconds,
					 k_timeout_t timeout)
{
	struct vc_runtime_command cmd = {
		.type = VC_RUNTIME_CMD_SET_UPTIME,
		.payload.uptime_seconds = seconds,
	};

	return vc_runtime_submit_command(runtime, &cmd, timeout);
}
```

- [ ] **Step 4: Run runtime tests**

Run: `/home/yong/backup/src/xlab/jianwei/hvb_wkspc/.venv/bin/west build -b native_posix -d build/test-runtime tests/voltage_control/runtime && ./build/test-runtime/zephyr/zephyr.exe`

Expected: PASS for command contract, null command, and queued operating mode tests.

- [ ] **Step 5: Commit**

```bash
git add lib/voltage_control/runtime.c tests/voltage_control/runtime/src/main.c
git commit -m "feat: process runtime commands on worker"
```

---

### Task 4: Queued Measurement Evidence

**Files:**
- Modify: `lib/voltage_control/runtime.c`
- Test: `tests/voltage_control/runtime/src/main.c`

- [ ] **Step 1: Update measurement test to prove async processing**

Replace the body of `test_runtime_submit_measurement_updates_domain_snapshot` with:

```c
{
	struct domain *d = domain_create(test_channels, 1);
	struct vc_runtime *rt;
	struct vc_measurement_snapshot meas = {
		.channel = 0,
		.generation = 1,
		.timestamp_ms = 10,
		.present_mask = VC_MEAS_PRESENT_VOLTAGE,
		.raw_voltage = 77,
	};
	struct vc_channel_snapshot snap;

	zassert_not_null(d);
	rt = vc_runtime_create(d);
	zassert_not_null(rt);

	zassert_equal(vc_runtime_submit_measurement(rt, &meas), VC_OK);
	k_msleep(20);
	zassert_equal(domain_get_channel_snapshot(d, 0, &snap), VC_OK);
	zassert_equal(snap.raw_adc_voltage, 77);
	zassert_equal(snap.measured_voltage, 77);

	vc_runtime_destroy(rt);
	free(d);
}
```

- [ ] **Step 2: Implement queued measurement submission**

Replace `vc_runtime_submit_measurement()` in `lib/voltage_control/runtime.c` with:

```c
enum vc_status vc_runtime_submit_measurement(
	struct vc_runtime *runtime,
	const struct vc_measurement_snapshot *meas)
{
	struct vc_runtime_evidence_item evidence;

	if (runtime == NULL || meas == NULL) {
		return VC_ERR_INVALID_VALUE;
	}

	evidence.measurement = *meas;
	if (k_msgq_put(&runtime->evidence_queue, &evidence, K_NO_WAIT) != 0) {
		return VC_ERR_UNSAFE_STATE;
	}
	k_sem_give(&runtime->wake);

	return VC_OK;
}
```

- [ ] **Step 3: Run runtime tests**

Run: `/home/yong/backup/src/xlab/jianwei/hvb_wkspc/.venv/bin/west build -b native_posix -d build/test-runtime tests/voltage_control/runtime && ./build/test-runtime/zephyr/zephyr.exe`

Expected: PASS. `test_runtime_submit_measurement_updates_domain_snapshot` proves queued evidence reaches the domain.

- [ ] **Step 4: Commit**

```bash
git add lib/voltage_control/runtime.c tests/voltage_control/runtime/src/main.c
git commit -m "feat: queue runtime measurement evidence"
```

---

### Task 5: Domain SMF Scaffolding Without Behavior Change

**Files:**
- Modify: `lib/voltage_control/domain.c`
- Modify: `tests/voltage_control/domain/src/main.c`

- [ ] **Step 1: Add public behavior regression test for calibration command rejection**

Append this test to `tests/voltage_control/domain/src/main.c` near other calibration tests:

```c
ZTEST(voltage_control_domain, test_smf_preserves_calibration_output_rejection)
{
	struct domain *d = domain_setup_fresh();

	zassert_equal(domain_calibration_unlock(d, 0xA55A), VC_OK);
	zassert_equal(domain_calibration_unlock(d, 0x5AA5), VC_OK);
	zassert_equal(domain_set_operating_mode(d, VC_OPERATING_MODE_CALIBRATION), VC_OK);
	zassert_equal(domain_channel_output_action(d, 0, VC_OUTPUT_ACTION_ENABLE),
		      VC_ERR_INVALID_COMMAND);

	free(d);
}
```

- [ ] **Step 2: Add public behavior regression test for fault safe config**

Append this test near runtime config tests:

```c
ZTEST(voltage_control_domain, test_smf_preserves_fault_safe_runtime_config)
{
	struct domain *d = domain_setup_fresh();
	struct vc_measurement_snapshot meas = {
		.channel = 0,
		.generation = 1,
		.present_mask = VC_MEAS_PRESENT_PROVIDER_STATUS,
		.provider_status = VC_PROVIDER_STATUS_INTERLOCK,
		.provider_fault_cause = VC_FAULT_INTERLOCK,
	};
	struct vc_runtime_config_snapshot cfg;

	zassert_equal(domain_consume_measurement(d, &meas), VC_OK);
	zassert_equal(domain_get_runtime_config(d, 0, &cfg), VC_OK);
	zassert_true(cfg.force_safe_state);
	zassert_false(cfg.output_enable);
	zassert_equal(cfg.raw_output_drive, 0);

	free(d);
}
```

- [ ] **Step 3: Include Zephyr SMF**

Modify the includes at the top of `lib/voltage_control/domain.c` to add:

```c
#include <zephyr/smf.h>
```

- [ ] **Step 4: Add internal SMF enums and contexts**

Insert after `#define VC_CHANNEL_MASK(c)`:

```c
enum vc_domain_smf_state {
	VC_DOMAIN_SMF_NORMAL,
	VC_DOMAIN_SMF_AUTOMATIC,
	VC_DOMAIN_SMF_CALIBRATION,
	VC_DOMAIN_SMF_COUNT,
};

enum vc_channel_smf_state {
	VC_CHANNEL_SMF_DISABLED_SAFE,
	VC_CHANNEL_SMF_ENABLED_HOLDING,
	VC_CHANNEL_SMF_RAMPING,
	VC_CHANNEL_SMF_FAULT_LATCHED,
	VC_CHANNEL_SMF_RETRY_COOLDOWN,
	VC_CHANNEL_SMF_CALIBRATION_OUTPUT,
	VC_CHANNEL_SMF_UNAVAILABLE,
	VC_CHANNEL_SMF_COUNT,
};

struct vc_domain_smf_ctx {
	struct smf_ctx ctx;
	struct domain *domain;
};

struct vc_channel_smf_ctx {
	struct smf_ctx ctx;
	struct domain *domain;
	uint8_t channel;
};
```

- [ ] **Step 5: Add contexts to structs**

Modify `struct vc_channel_runtime` to include:

```c
	struct vc_channel_smf_ctx smf;
```

Modify `struct domain` to include before `const struct vc_channel_entry *ch_entry;`:

```c
	struct vc_domain_smf_ctx smf;
```

- [ ] **Step 6: Add no-op SMF states and initializer**

Insert before `domain_create()`:

```c
static void vc_domain_smf_noop(void *obj)
{
	ARG_UNUSED(obj);
}

static const struct smf_state vc_domain_states[VC_DOMAIN_SMF_COUNT] = {
	[VC_DOMAIN_SMF_NORMAL] = SMF_CREATE_STATE(NULL, vc_domain_smf_noop, NULL, NULL, NULL),
	[VC_DOMAIN_SMF_AUTOMATIC] = SMF_CREATE_STATE(NULL, vc_domain_smf_noop, NULL, NULL, NULL),
	[VC_DOMAIN_SMF_CALIBRATION] = SMF_CREATE_STATE(NULL, vc_domain_smf_noop, NULL, NULL, NULL),
};

static const struct smf_state vc_channel_states[VC_CHANNEL_SMF_COUNT] = {
	[VC_CHANNEL_SMF_DISABLED_SAFE] = SMF_CREATE_STATE(NULL, vc_domain_smf_noop, NULL, NULL, NULL),
	[VC_CHANNEL_SMF_ENABLED_HOLDING] = SMF_CREATE_STATE(NULL, vc_domain_smf_noop, NULL, NULL, NULL),
	[VC_CHANNEL_SMF_RAMPING] = SMF_CREATE_STATE(NULL, vc_domain_smf_noop, NULL, NULL, NULL),
	[VC_CHANNEL_SMF_FAULT_LATCHED] = SMF_CREATE_STATE(NULL, vc_domain_smf_noop, NULL, NULL, NULL),
	[VC_CHANNEL_SMF_RETRY_COOLDOWN] = SMF_CREATE_STATE(NULL, vc_domain_smf_noop, NULL, NULL, NULL),
	[VC_CHANNEL_SMF_CALIBRATION_OUTPUT] = SMF_CREATE_STATE(NULL, vc_domain_smf_noop, NULL, NULL, NULL),
	[VC_CHANNEL_SMF_UNAVAILABLE] = SMF_CREATE_STATE(NULL, vc_domain_smf_noop, NULL, NULL, NULL),
};

static void domain_init_smf(struct domain *domain)
{
	domain->smf.domain = domain;
	smf_set_initial(SMF_CTX(&domain->smf), &vc_domain_states[VC_DOMAIN_SMF_NORMAL]);

	for (size_t ch = 0; ch < domain->channel_count; ch++) {
		domain->runtime[ch].smf.domain = domain;
		domain->runtime[ch].smf.channel = ch;
		smf_set_initial(SMF_CTX(&domain->runtime[ch].smf),
				&vc_channel_states[VC_CHANNEL_SMF_DISABLED_SAFE]);
	}
}
```

Call `domain_init_smf(domain);` in `domain_create()` after `domain->channel_count` is assigned and before returning the domain.

- [ ] **Step 7: Run domain tests**

Run: `/home/yong/backup/src/xlab/jianwei/hvb_wkspc/.venv/bin/west build -b native_posix -d build/test-domain tests/voltage_control/domain && ./build/test-domain/zephyr/zephyr.exe`

Expected: PASS. Behavior must be unchanged; this task only adds SMF scaffolding.

- [ ] **Step 8: Commit**

```bash
git add lib/voltage_control/domain.c tests/voltage_control/domain/src/main.c
git commit -m "refactor: add domain smf scaffolding"
```

---

### Task 6: Production App Uses Runtime For Uptime Mutation

**Files:**
- Modify: `applications/hvb_controller/src/main.c`
- Test: `tests/voltage_control/runtime/src/main.c`

- [ ] **Step 1: Add runtime uptime test**

Append this test after `test_runtime_set_operating_mode_is_processed_by_worker`:

```c
ZTEST(voltage_control_runtime, test_runtime_set_uptime_is_processed_by_worker)
{
	struct domain *d = domain_create(test_channels, 1);
	struct vc_runtime *rt;
	struct vc_system_snapshot snap;

	zassert_not_null(d);
	rt = vc_runtime_create(d);
	zassert_not_null(rt);

	zassert_equal(vc_runtime_set_uptime(rt, 42, K_SECONDS(1)), VC_OK);
	zassert_equal(domain_get_system_snapshot(d, &snap), VC_OK);
	zassert_equal(snap.uptime, 42);

	vc_runtime_destroy(rt);
	free(d);
}
```

- [ ] **Step 2: Replace direct uptime mutation in app**

In `applications/hvb_controller/src/main.c`, replace:

```c
		domain_set_uptime(domain, (uint32_t)(k_uptime_get() / 1000));
```

with:

```c
		(void)vc_runtime_set_uptime(runtime, (uint32_t)(k_uptime_get() / 1000), K_MSEC(100));
```

- [ ] **Step 3: Run runtime tests**

Run: `/home/yong/backup/src/xlab/jianwei/hvb_wkspc/.venv/bin/west build -b native_posix -d build/test-runtime tests/voltage_control/runtime && ./build/test-runtime/zephyr/zephyr.exe`

Expected: PASS, including uptime test.

- [ ] **Step 4: Run board build**

Run: `/home/yong/backup/src/xlab/jianwei/hvb_wkspc/.venv/bin/west build -b jw_hvb -d build/hvb_controller applications/hvb_controller`

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add applications/hvb_controller/src/main.c tests/voltage_control/runtime/src/main.c
git commit -m "feat: route app uptime through runtime"
```

---

### Task 7: Final Verification

**Files:**
- No source changes expected.

- [ ] **Step 1: Run domain tests**

Run: `/home/yong/backup/src/xlab/jianwei/hvb_wkspc/.venv/bin/west build -b native_posix -d build/test-domain tests/voltage_control/domain && ./build/test-domain/zephyr/zephyr.exe`

Expected: `SUITE PASS - 100.00% [voltage_control_domain]`.

- [ ] **Step 2: Run runtime tests**

Run: `/home/yong/backup/src/xlab/jianwei/hvb_wkspc/.venv/bin/west build -b native_posix -d build/test-runtime tests/voltage_control/runtime && ./build/test-runtime/zephyr/zephyr.exe`

Expected: `SUITE PASS - 100.00% [voltage_control_runtime]`.

- [ ] **Step 3: Run production board build**

Run: `/home/yong/backup/src/xlab/jianwei/hvb_wkspc/.venv/bin/west build -b jw_hvb -d build/hvb_controller applications/hvb_controller`

Expected: build completes and links `zephyr/zephyr.elf`.

- [ ] **Step 4: Inspect git status**

Run: `git status --short --branch`

Expected: only intentional changes are present. Existing independent untracked host-tools docs/diagrams may still appear and must not be staged for this work.
