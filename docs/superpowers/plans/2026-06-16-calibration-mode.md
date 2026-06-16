# Calibration Mode Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add Modbus-visible Calibration Mode for factory/service raw DAC/ADC calibration while preserving hard safety rails and keeping the mode volatile.

**Architecture:** The voltage-control domain owns Calibration Mode behavior, unlock state, raw calibration runtime state, coefficient write restrictions, and Calibration Commit rules. The Modbus adapter owns register decoding, command readback, protocol exception mapping, and access control translation, but must not implement product policy. Hardware-facing raw DAC/ADC execution can start as domain/runtime state for the first slice and later connect to board services.

**Tech Stack:** Zephyr 3.7.2, C, ztest on `native_posix`, Modbus RTU adapter, shared C register map header.

---

## References

- PRD: `docs/superpowers/specs/2026-06-16-calibration-mode-prd.md`
- Domain vocabulary: `CONTEXT.md`
- Domain behavior spec: `docs/superpowers/specs/2026-06-15-voltage-control-domain-behavior.md`
- Modbus protocol reference: `ref/modbus_interface.md`
- Register map: `include/regmap/hvb_regs.h`
- Domain API: `include/voltage_control/domain.h`
- Domain implementation: `lib/voltage_control/domain.c`
- HVB variant defaults: `lib/voltage_control/hvb_variant.c`
- Modbus adapter: `applications/hvb_controller/src/modbus_adapter.c`
- Domain tests: `tests/voltage_control/domain/src/main.c`
- Test guide: `tests/README.md`

## File Structure

- Modify `include/regmap/hvb_regs.h`: add protocol minor/register definitions for Calibration Mode, raw ADC/DAC registers, and `CAL_UNLOCK`.
- Modify `include/voltage_control/domain.h`: add operating mode value, calibration sample status enum, calibration command APIs, and calibration snapshot/config fields needed by the adapter.
- Modify `lib/voltage_control/domain.c`: implement unlock state, volatile Calibration Mode entry/exit, raw calibration runtime state, coefficient write restrictions, single-active-channel rule, sample status, and Calibration Commit semantics.
- Modify `lib/voltage_control/hvb_variant.c`: report protocol minor `1`, Calibration Mode capability, and raw DAC limit defaults if variant profile grows a field.
- Modify `include/voltage_control/variant.h`: add variant raw DAC maximum only if no existing variant field can express it.
- Modify `applications/hvb_controller/src/modbus_adapter.c`: map protocol `2.1` registers to domain APIs and enforce register access/exception behavior.
- Modify `tests/voltage_control/domain/src/main.c`: add ztests for domain Calibration Mode behavior.
- Modify or add `tests/integration/modbus/calibration.sh`: add hardware-facing Modbus checks if integration tests are in scope for the slice; otherwise document as manual follow-up.

## Task 1: Register Map Constants

**Files:**

- Modify: `include/regmap/hvb_regs.h`
- Reference: `ref/modbus_interface.md`

- [ ] **Step 1: Add register constants**

Add the protocol and register constants below near the existing block definitions and channel offsets.

```c
#define HVB_PROTOCOL_MAJOR             2
#define HVB_PROTOCOL_MINOR             1

#define SYS_CAP_AUTOMATIC_MODE         0x0001
#define SYS_CAP_ENV_SENSOR             0x0002
#define SYS_CAP_CALIBRATION_MODE       0x0004

#define CH_RAW_ADC_VOLTAGE_HI          12
#define CH_RAW_ADC_VOLTAGE_LO          13
#define CH_RAW_ADC_CURRENT_HI          14
#define CH_RAW_ADC_CURRENT_LO          15
#define CH_CAL_SAMPLE_STATUS           16
#define CH_RAW_DAC_READBACK            17

#define CH_CAL_OUTPUT_ENABLE           21
#define CH_RAW_DAC_CODE                22
#define CH_CAL_SAMPLE_CMD              23
#define CH_CAL_COMMIT_CMD              24
#define CH_CAL_MAX_RAW_DAC_LIMIT       25

#define EXT_CAL_UNLOCK                 0
#define EXT_CAL_UNLOCK_ABS             (EXT_BLOCK_BASE + EXT_CAL_UNLOCK)

#define CAL_UNLOCK_STEP1               0xCA1B
#define CAL_UNLOCK_STEP2               0xA11B
#define CAL_COMMAND_NONE               0
#define CAL_COMMAND_EXECUTE            1
```

- [ ] **Step 2: Build the domain test target**

Run: `west build -b native_posix -t run -d build/test-domain tests/voltage_control/domain`

Expected: existing tests still build or fail only because later tasks have not implemented new behavior. If this fails from unrelated local worktree state, record the failure before continuing.

## Task 2: Domain API Shape

**Files:**

- Modify: `include/voltage_control/domain.h`
- Modify if needed: `include/voltage_control/variant.h`

- [ ] **Step 1: Add public enum values and structs**

Add Calibration Mode and sample status definitions.

```c
enum vc_operating_mode {
	VC_OPERATING_MODE_NORMAL = 0,
	VC_OPERATING_MODE_AUTOMATIC = 1,
	VC_OPERATING_MODE_CALIBRATION = 2,
};

enum vc_cal_sample_status {
	VC_CAL_SAMPLE_NONE = 0,
	VC_CAL_SAMPLE_VALID = 1,
	VC_CAL_SAMPLE_BUSY = 2,
	VC_CAL_SAMPLE_ERROR = 3,
};
```

Extend `struct vc_channel_snapshot` with calibration readback fields.

```c
	int32_t raw_adc_voltage;
	int32_t raw_adc_current;
	enum vc_cal_sample_status cal_sample_status;
	uint16_t raw_dac_readback;
	uint16_t cal_output_enabled;
```

- [ ] **Step 2: Add domain API declarations**

Add these declarations near the existing domain command APIs.

```c
enum vc_status vc_domain_calibration_unlock(struct vc_domain *domain,
						    uint16_t value);
enum vc_status vc_domain_calibration_set_output_enable(struct vc_domain *domain,
							       uint8_t channel,
							       bool enabled);
enum vc_status vc_domain_calibration_set_raw_dac(struct vc_domain *domain,
							 uint8_t channel,
							 uint16_t code);
enum vc_status vc_domain_calibration_sample(struct vc_domain *domain,
						    uint8_t channel);
enum vc_status vc_domain_calibration_commit(struct vc_domain *domain,
						    uint8_t channel);
enum vc_status vc_domain_calibration_set_max_raw_dac(struct vc_domain *domain,
							     uint8_t channel,
							     uint16_t limit);
```

- [ ] **Step 3: Add variant raw DAC maximum if needed**

If `struct vc_variant_profile` has no raw DAC maximum, add a field in `include/voltage_control/variant.h`.

```c
	uint16_t max_raw_dac_code;
```

- [ ] **Step 4: Run compile check**

Run: `west build -b native_posix -t run -d build/test-domain tests/voltage_control/domain`

Expected: compile fails until implementation symbols are added in later tasks, or passes if no tests reference the new APIs yet.

## Task 3: Domain Tests for Mode Entry and Unlock

**Files:**

- Modify: `tests/voltage_control/domain/src/main.c`
- Modify: `lib/voltage_control/domain.c`

- [ ] **Step 1: Write failing tests**

Add tests that verify unlock gating, wrong sequence clearing, and entry to Calibration Mode.

```c
ZTEST(voltage_control_domain, test_calibration_mode_requires_unlock)
{
	struct vc_domain *domain = vc_domain_create(&hvb_variant_profile);

	zassert_equal(vc_domain_set_operating_mode(domain,
						 VC_OPERATING_MODE_CALIBRATION),
		      VC_ERR_INVALID_COMMAND);
	zassert_equal(vc_domain_get_operating_mode(domain),
		      VC_OPERATING_MODE_NORMAL);
}

ZTEST(voltage_control_domain, test_calibration_unlock_sequence_allows_entry)
{
	struct vc_domain *domain = vc_domain_create(&hvb_variant_profile);

	zassert_equal(vc_domain_calibration_unlock(domain, CAL_UNLOCK_STEP1), VC_OK);
	zassert_equal(vc_domain_calibration_unlock(domain, CAL_UNLOCK_STEP2), VC_OK);
	zassert_equal(vc_domain_set_operating_mode(domain,
						 VC_OPERATING_MODE_CALIBRATION),
		      VC_OK);
	zassert_equal(vc_domain_get_operating_mode(domain),
		      VC_OPERATING_MODE_CALIBRATION);
}

ZTEST(voltage_control_domain, test_wrong_calibration_unlock_value_clears_sequence)
{
	struct vc_domain *domain = vc_domain_create(&hvb_variant_profile);

	zassert_equal(vc_domain_calibration_unlock(domain, CAL_UNLOCK_STEP1), VC_OK);
	zassert_equal(vc_domain_calibration_unlock(domain, 0x1234),
		      VC_ERR_INVALID_VALUE);
	zassert_equal(vc_domain_calibration_unlock(domain, CAL_UNLOCK_STEP2),
		      VC_ERR_INVALID_VALUE);
	zassert_equal(vc_domain_set_operating_mode(domain,
						 VC_OPERATING_MODE_CALIBRATION),
		      VC_ERR_INVALID_COMMAND);
}
```

- [ ] **Step 2: Run tests to verify red**

Run: `west build -b native_posix -t run -d build/test-domain tests/voltage_control/domain`

Expected: FAIL or compile error because Calibration Mode APIs and constants are not implemented yet.

- [ ] **Step 3: Implement minimal unlock state**

In `lib/voltage_control/domain.c`, add unlock progress to `struct vc_domain`.

```c
	uint8_t cal_unlock_step;
	enum vc_operating_mode persisted_operating_mode;
```

Implement `vc_domain_calibration_unlock`.

```c
enum vc_status vc_domain_calibration_unlock(struct vc_domain *domain,
						    uint16_t value)
{
	if (value == CAL_UNLOCK_STEP1) {
		domain->cal_unlock_step = 1;
		return VC_OK;
	}

	if (value == CAL_UNLOCK_STEP2 && domain->cal_unlock_step == 1) {
		domain->cal_unlock_step = 2;
		return VC_OK;
	}

	domain->cal_unlock_step = 0;
	return VC_ERR_INVALID_VALUE;
}
```

Update `is_valid_operating_mode` to accept `VC_OPERATING_MODE_CALIBRATION`, and update `vc_domain_set_operating_mode` so Calibration Mode requires `cal_unlock_step == 2`.

- [ ] **Step 4: Run tests to verify green**

Run: `west build -b native_posix -t run -d build/test-domain tests/voltage_control/domain`

Expected: PASS for the new unlock tests and existing tests.

- [ ] **Step 5: Commit**

Run: `git add include/regmap/hvb_regs.h include/voltage_control/domain.h include/voltage_control/variant.h lib/voltage_control/domain.c tests/voltage_control/domain/src/main.c && git commit -m "feat: add calibration mode unlock domain state"`

Expected: commit succeeds if the user requested commits for plan execution. If commits are not requested, skip this step and leave changes unstaged.

## Task 4: Domain Calibration Runtime State

**Files:**

- Modify: `lib/voltage_control/domain.c`
- Modify: `tests/voltage_control/domain/src/main.c`

- [ ] **Step 1: Write failing tests for raw output rules**

Add tests for entry cleanup, raw DAC gating, and single-active-channel enforcement.

```c
ZTEST(voltage_control_domain, test_entering_calibration_clears_raw_outputs)
{
	struct vc_domain *domain = vc_domain_create(&hvb_variant_profile);
	struct vc_channel_snapshot snap;

	vc_domain_calibration_unlock(domain, CAL_UNLOCK_STEP1);
	vc_domain_calibration_unlock(domain, CAL_UNLOCK_STEP2);
	zassert_equal(vc_domain_set_operating_mode(domain,
						 VC_OPERATING_MODE_CALIBRATION), VC_OK);
	zassert_equal(vc_domain_get_channel_snapshot(domain, 0, &snap), VC_OK);
	zassert_equal(snap.raw_dac_readback, 0);
	zassert_equal(snap.cal_output_enabled, 0);
}

ZTEST(voltage_control_domain, test_raw_dac_nonzero_requires_calibration_output_enable)
{
	struct vc_domain *domain = vc_domain_create(&hvb_variant_profile);

	vc_domain_calibration_unlock(domain, CAL_UNLOCK_STEP1);
	vc_domain_calibration_unlock(domain, CAL_UNLOCK_STEP2);
	vc_domain_set_operating_mode(domain, VC_OPERATING_MODE_CALIBRATION);
	zassert_equal(vc_domain_calibration_set_raw_dac(domain, 0, 100),
		      VC_ERR_UNSAFE_STATE);
	zassert_equal(vc_domain_calibration_set_raw_dac(domain, 0, 0), VC_OK);
}

ZTEST(voltage_control_domain, test_only_one_calibration_output_enabled)
{
	struct vc_domain *domain = vc_domain_create(&hvb_variant_profile);

	vc_domain_calibration_unlock(domain, CAL_UNLOCK_STEP1);
	vc_domain_calibration_unlock(domain, CAL_UNLOCK_STEP2);
	vc_domain_set_operating_mode(domain, VC_OPERATING_MODE_CALIBRATION);
	zassert_equal(vc_domain_calibration_set_output_enable(domain, 0, true), VC_OK);
	zassert_equal(vc_domain_calibration_set_output_enable(domain, 1, true),
		      VC_ERR_UNSAFE_STATE);
}
```

- [ ] **Step 2: Run tests to verify red**

Run: `west build -b native_posix -t run -d build/test-domain tests/voltage_control/domain`

Expected: FAIL because raw calibration runtime APIs are not implemented.

- [ ] **Step 3: Implement calibration runtime state**

Add to `struct vc_channel_runtime`.

```c
	bool cal_output_enabled;
	uint16_t raw_dac_code;
	uint16_t cal_max_raw_dac_limit;
	int32_t raw_adc_voltage;
	int32_t raw_adc_current;
	enum vc_cal_sample_status cal_sample_status;
```

Implement helper behavior in `domain.c`.

```c
static void disable_calibration_output(struct vc_domain *domain, uint8_t ch)
{
	domain->runtime[ch].cal_output_enabled = false;
	domain->runtime[ch].raw_dac_code = 0;
	domain->runtime[ch].cal_sample_status = VC_CAL_SAMPLE_NONE;
}

static void disable_all_calibration_outputs(struct vc_domain *domain)
{
	for (int i = 0; i < domain->variant->num_channels; i++) {
		disable_calibration_output(domain, i);
		domain->runtime[i].output_enabled = false;
		domain->runtime[i].ramping = false;
		domain->runtime[i].cooldown_remaining_ms = 0;
	}
}
```

Implement the raw DAC and output-enable APIs with these rules: require Calibration Mode, reject unsupported channels, reject second enabled channel, reject nonzero DAC while disabled, reject DAC above current limit, force raw DAC zero on disable.

- [ ] **Step 4: Populate snapshots**

In `vc_domain_get_channel_snapshot`, copy runtime raw fields into `struct vc_channel_snapshot`.

```c
	snap->raw_adc_voltage = rt->raw_adc_voltage;
	snap->raw_adc_current = rt->raw_adc_current;
	snap->cal_sample_status = rt->cal_sample_status;
	snap->raw_dac_readback = rt->raw_dac_code;
	snap->cal_output_enabled = rt->cal_output_enabled ? 1 : 0;
```

- [ ] **Step 5: Run tests to verify green**

Run: `west build -b native_posix -t run -d build/test-domain tests/voltage_control/domain`

Expected: PASS.

## Task 5: Coefficient Access and Calibration Commit

**Files:**

- Modify: `lib/voltage_control/domain.c`
- Modify: `tests/voltage_control/domain/src/main.c`

- [ ] **Step 1: Write failing tests for coefficient restrictions**

Add tests that reject coefficient writes outside Calibration Mode and accept them inside Calibration Mode.

```c
ZTEST(voltage_control_domain, test_calibration_coefficients_writable_only_in_calibration_mode)
{
	struct vc_domain *domain = vc_domain_create(&hvb_variant_profile);
	struct vc_channel_config cfg;

	vc_domain_get_channel_config(domain, 0, &cfg);
	cfg.output_calib_k = 11000;
	zassert_equal(vc_domain_set_channel_config(domain, 0, &cfg),
		      VC_ERR_INVALID_COMMAND);

	vc_domain_calibration_unlock(domain, CAL_UNLOCK_STEP1);
	vc_domain_calibration_unlock(domain, CAL_UNLOCK_STEP2);
	vc_domain_set_operating_mode(domain, VC_OPERATING_MODE_CALIBRATION);
	zassert_equal(vc_domain_set_channel_config(domain, 0, &cfg), VC_OK);
}

ZTEST(voltage_control_domain, test_calibration_commit_requires_idle_raw_output)
{
	struct vc_domain *domain = vc_domain_create(&hvb_variant_profile);

	vc_domain_calibration_unlock(domain, CAL_UNLOCK_STEP1);
	vc_domain_calibration_unlock(domain, CAL_UNLOCK_STEP2);
	vc_domain_set_operating_mode(domain, VC_OPERATING_MODE_CALIBRATION);
	vc_domain_calibration_set_output_enable(domain, 0, true);
	zassert_equal(vc_domain_calibration_commit(domain, 0), VC_ERR_UNSAFE_STATE);
	vc_domain_calibration_set_output_enable(domain, 0, false);
	zassert_equal(vc_domain_calibration_commit(domain, 0), VC_OK);
}
```

- [ ] **Step 2: Run tests to verify red**

Run: `west build -b native_posix -t run -d build/test-domain tests/voltage_control/domain`

Expected: FAIL because coefficient restriction and commit logic are not implemented.

- [ ] **Step 3: Implement coefficient change detection**

In `vc_domain_set_channel_config`, compare calibration fields against current channel config.

```c
static bool calibration_fields_changed(const struct vc_channel_config *old_cfg,
					       const struct vc_channel_config *new_cfg)
{
	return old_cfg->output_calib_k != new_cfg->output_calib_k ||
	       old_cfg->output_calib_b != new_cfg->output_calib_b ||
	       old_cfg->measured_voltage_calib_k != new_cfg->measured_voltage_calib_k ||
	       old_cfg->measured_voltage_calib_b != new_cfg->measured_voltage_calib_b ||
	       old_cfg->measured_current_calib_k != new_cfg->measured_current_calib_k ||
	       old_cfg->measured_current_calib_b != new_cfg->measured_current_calib_b;
}
```

Reject changed calibration fields unless `domain->operating_mode == VC_OPERATING_MODE_CALIBRATION`.

- [ ] **Step 4: Implement Calibration Commit stub behavior**

Implement `vc_domain_calibration_commit` as a synchronous domain action. Until real storage is wired, return `VC_OK` after validating mode, channel, output disabled, raw DAC zero, and no hard safety fault. Preserve the storage integration point for the later NVM slice.

- [ ] **Step 5: Run tests to verify green**

Run: `west build -b native_posix -t run -d build/test-domain tests/voltage_control/domain`

Expected: PASS.

## Task 6: Explicit Raw Sample Command

**Files:**

- Modify: `lib/voltage_control/domain.c`
- Modify: `tests/voltage_control/domain/src/main.c`

- [ ] **Step 1: Write failing test for sample command**

Add a test for sample status. The initial implementation may capture simulated zero values until hardware services exist.

```c
ZTEST(voltage_control_domain, test_calibration_sample_captures_raw_values)
{
	struct vc_domain *domain = vc_domain_create(&hvb_variant_profile);
	struct vc_channel_snapshot snap;

	vc_domain_calibration_unlock(domain, CAL_UNLOCK_STEP1);
	vc_domain_calibration_unlock(domain, CAL_UNLOCK_STEP2);
	vc_domain_set_operating_mode(domain, VC_OPERATING_MODE_CALIBRATION);
	zassert_equal(vc_domain_calibration_sample(domain, 0), VC_OK);
	zassert_equal(vc_domain_get_channel_snapshot(domain, 0, &snap), VC_OK);
	zassert_equal(snap.cal_sample_status, VC_CAL_SAMPLE_VALID);
}
```

- [ ] **Step 2: Run tests to verify red**

Run: `west build -b native_posix -t run -d build/test-domain tests/voltage_control/domain`

Expected: FAIL because sample command is not implemented.

- [ ] **Step 3: Implement minimal sample behavior**

Implement `vc_domain_calibration_sample` to require Calibration Mode and supported channel, then set raw values and status.

```c
enum vc_status vc_domain_calibration_sample(struct vc_domain *domain,
						    uint8_t channel)
{
	if (!channel_valid(domain, channel)) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}
	if (domain->operating_mode != VC_OPERATING_MODE_CALIBRATION) {
		return VC_ERR_INVALID_COMMAND;
	}

	domain->runtime[channel].cal_sample_status = VC_CAL_SAMPLE_VALID;
	return VC_OK;
}
```

- [ ] **Step 4: Run tests to verify green**

Run: `west build -b native_posix -t run -d build/test-domain tests/voltage_control/domain`

Expected: PASS.

## Task 7: Modbus Adapter Mapping

**Files:**

- Modify: `applications/hvb_controller/src/modbus_adapter.c`
- Modify: `tests/integration/modbus/validation.sh` or add `tests/integration/modbus/calibration.sh` if hardware testing is available

- [ ] **Step 1: Update system operating mode write validation**

Change the `SYS_OPERATING_MODE` case to accept value `2` and rely on the domain for unlock rejection.

```c
case SYS_OPERATING_MODE:
	if (val > VC_OPERATING_MODE_CALIBRATION) return -1;
	cfg.operating_mode = (enum vc_operating_mode)val;
	break;
```

- [ ] **Step 2: Map raw calibration input registers**

In `read_ch_input`, reject calibration-only reads outside Calibration Mode, then map the raw fields.

```c
case CH_RAW_ADC_VOLTAGE_HI:
	*reg = (uint16_t)((uint32_t)snap.raw_adc_voltage >> 16);
	break;
case CH_RAW_ADC_VOLTAGE_LO:
	*reg = (uint16_t)((uint32_t)snap.raw_adc_voltage & 0xFFFF);
	break;
case CH_RAW_ADC_CURRENT_HI:
	*reg = (uint16_t)((uint32_t)snap.raw_adc_current >> 16);
	break;
case CH_RAW_ADC_CURRENT_LO:
	*reg = (uint16_t)((uint32_t)snap.raw_adc_current & 0xFFFF);
	break;
case CH_CAL_SAMPLE_STATUS:
	*reg = snap.cal_sample_status;
	break;
case CH_RAW_DAC_READBACK:
	*reg = snap.raw_dac_readback;
	break;
```

- [ ] **Step 3: Map calibration holding registers**

In `read_ch_holding`, return calibration output enable, raw DAC readback, command readback zero, and calibration max raw DAC limit. In `write_ch_holding`, route calibration registers to the new domain APIs.

```c
case CH_CAL_OUTPUT_ENABLE:
	return vc_domain_calibration_set_output_enable(d, ch, val != 0) == VC_OK ? 0 : -1;
case CH_RAW_DAC_CODE:
	return vc_domain_calibration_set_raw_dac(d, ch, val) == VC_OK ? 0 : -1;
case CH_CAL_SAMPLE_CMD:
	if (val == CAL_COMMAND_NONE) return 0;
	if (val != CAL_COMMAND_EXECUTE) return -1;
	return vc_domain_calibration_sample(d, ch) == VC_OK ? 0 : -1;
case CH_CAL_COMMIT_CMD:
	if (val == CAL_COMMAND_NONE) return 0;
	if (val != CAL_COMMAND_EXECUTE) return -1;
	return vc_domain_calibration_commit(d, ch) == VC_OK ? 0 : -1;
case CH_CAL_MAX_RAW_DAC_LIMIT:
	return vc_domain_calibration_set_max_raw_dac(d, ch, val) == VC_OK ? 0 : -1;
```

- [ ] **Step 4: Map extension block unlock register**

Handle absolute address `EXT_CAL_UNLOCK_ABS` in holding read/write paths. Reads return `0`. Writes call `vc_domain_calibration_unlock`.

- [ ] **Step 5: Preserve exception mapping**

Keep unsupported/reserved register behavior as `0x02`, invalid values as `0x03`, and storage/internal failures as `0x04`. If the adapter currently collapses all failures to `-1`, add a small local mapping only if the existing adapter abstraction can return distinct exceptions.

- [ ] **Step 6: Run tests/build**

Run: `west build -b native_posix -t run -d build/test-domain tests/voltage_control/domain`

Expected: PASS.

If hardware is available, run: `PORT=/dev/ttyUSB0 tests/integration/modbus/run_all.sh`

Expected: existing smoke/simulation/protection/validation checks pass, plus any new calibration checks.

## Task 8: Protocol and Variant Snapshot Updates

**Files:**

- Modify: `lib/voltage_control/hvb_variant.c`
- Modify: `lib/voltage_control/domain.c`

- [ ] **Step 1: Report protocol minor 1**

Where system snapshots are populated, set `protocol_major` and `protocol_minor` from the register map constants.

```c
	snap->protocol_major = HVB_PROTOCOL_MAJOR;
	snap->protocol_minor = HVB_PROTOCOL_MINOR;
```

- [ ] **Step 2: Report Calibration Mode capability**

Include `SYS_CAP_CALIBRATION_MODE` in the system capability flags for HVB.

```c
	snap->system_capability_flags |= SYS_CAP_CALIBRATION_MODE;
```

- [ ] **Step 3: Initialize raw DAC limit**

On domain creation, set each channel runtime `cal_max_raw_dac_limit` to the variant raw DAC maximum.

- [ ] **Step 4: Run tests**

Run: `west build -b native_posix -t run -d build/test-domain tests/voltage_control/domain`

Expected: PASS.

## Task 9: Documentation Cross-Check

**Files:**

- Review: `docs/superpowers/specs/2026-06-16-calibration-mode-prd.md`
- Review: `docs/superpowers/specs/2026-06-15-voltage-control-domain-behavior.md`
- Review: `ref/modbus_interface.md`
- Review: `CONTEXT.md`

- [ ] **Step 1: Check implementation against specs**

Confirm every implemented behavior is described in the PRD and specs. If implementation needed a new decision, update the relevant spec before finalizing.

- [ ] **Step 2: Run final verification**

Run: `west build -b native_posix -t run -d build/test-domain tests/voltage_control/domain`

Expected: PASS.

If hardware is available, run: `PORT=/dev/ttyUSB0 tests/integration/modbus/run_all.sh`

Expected: PASS. If hardware is not available, record that hardware verification was not run.

- [ ] **Step 3: Review diff**

Run: `git diff -- include/regmap/hvb_regs.h include/voltage_control/domain.h include/voltage_control/variant.h lib/voltage_control/domain.c lib/voltage_control/hvb_variant.c applications/hvb_controller/src/modbus_adapter.c tests/voltage_control/domain/src/main.c docs/superpowers/specs/2026-06-16-calibration-mode-prd.md docs/superpowers/specs/2026-06-15-voltage-control-domain-behavior.md ref/modbus_interface.md CONTEXT.md`

Expected: diff contains only Calibration Mode related changes.

## Self-Review

- Spec coverage: This plan covers Calibration Mode enum value, unlock, volatile entry/exit, raw DAC/ADC controls, coefficient write restrictions, Calibration Commit, Modbus protocol `2.1`, and deferred production factory handoff.
- Placeholder scan: No task relies on unspecified behavior. Hardware ADC/DAC integration is intentionally scoped as a later service connection; the initial domain sample behavior is explicit.
- Type consistency: `VC_OPERATING_MODE_CALIBRATION`, `vc_domain_calibration_*`, `VC_CAL_SAMPLE_*`, `CH_CAL_*`, and `EXT_CAL_UNLOCK_ABS` names are used consistently across tasks.
