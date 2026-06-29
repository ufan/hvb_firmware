/*
 * End-to-end calibration shell verification test.
 * Drives the full cal sequence and prints every line of shell output
 * exactly as it would appear on the real UART console.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_dummy.h>
#include <zephyr/ztest.h>

#include "voltage_control/vc.h"
#include "voltage_control/vc_shell.h"

static struct vc_ctx *ctx;

/* Run a command and print its return code + full output */
static int run(const char *cmd)
{
	const struct shell *sh = shell_backend_dummy_get_ptr();
	size_t size;

	shell_backend_dummy_clear_output(sh);
	int ret = shell_execute_cmd(sh, cmd);
	k_msleep(50);
	const char *out = shell_backend_dummy_get_output(sh, &size);

	printk(">>> %s  (ret=%d)\n", cmd, ret);
	if (size > 0) {
		printk("%s", out);
		/* ensure trailing newline */
		if (out[size - 1] != '\n') {
			printk("\n");
		}
	}
	return ret;
}

ZTEST(cal_verify, test_full_cal_sequence)
{
	int ret;

	printk("\n========== CAL SEQUENCE VERIFICATION ==========\n\n");

	/* --- 1. Reject vc mode cal --- */
	printk("--- 1. vc mode cal must be rejected ---\n");
	ret = run("vc mode cal");
	zassert_equal(ret, -EINVAL, "vc mode cal should return -EINVAL, got %d", ret);

	/* --- 2. Enter via vc cal unlock --- */
	printk("\n--- 2. vc cal unlock (enters CAL mode + prints guide) ---\n");
	ret = run("vc cal unlock");
	zassert_equal(ret, 0, "vc cal unlock failed: %d", ret);

	/* --- 3. cal status --- */
	printk("\n--- 3. vc cal status ---\n");
	ret = run("vc cal status");
	zassert_equal(ret, 0, "vc cal status failed: %d", ret);

	/* --- 4. enable output (should print hint) --- */
	printk("\n--- 4. vc cal 0 output on (expect hint: vc cal dac) ---\n");
	ret = run("vc cal 0 output on");
	zassert_equal(ret, 0, "vc cal output on failed: %d", ret);

	/* --- 5. set DAC code (should print hint) --- */
	printk("\n--- 5. vc cal 0 dac 500 (expect hint: vc cal sample) ---\n");
	ret = run("vc cal 0 dac 500");
	zassert_equal(ret, 0, "vc cal dac failed: %d", ret);

	/* --- 6. sample (blocking: should print dac/raw_v/raw_i + hint) --- */
	printk("\n--- 6. vc cal 0 sample (blocking, expect raw values) ---\n");
	ret = run("vc cal 0 sample");
	zassert_equal(ret, 0, "vc cal sample failed: %d", ret);

	/* --- 7. cal status after DAC set --- */
	printk("\n--- 7. vc cal status (should show dac=500 out=ON) ---\n");
	ret = run("vc cal status");
	zassert_equal(ret, 0, "vc cal status failed: %d", ret);

	/* --- 8. set a coefficient --- */
	printk("\n--- 8. vc cal 0 set out_cal_k 10000 ---\n");
	ret = run("vc cal 0 set out_cal_k 10000");
	zassert_equal(ret, 0, "vc cal set out_cal_k failed: %d", ret);

	/* --- 9. cal config to verify stored values --- */
	printk("\n--- 9. vc cal 0 config ---\n");
	ret = run("vc cal 0 config");
	zassert_equal(ret, 0, "vc cal config failed: %d", ret);

	/* --- 10. commit requires output off + dac=0 first --- */
	printk("\n--- 10. vc cal 0 output off ---\n");
	ret = run("vc cal 0 output off");
	zassert_equal(ret, 0, "vc cal output off failed: %d", ret);

	ret = run("vc cal 0 dac 0");
	zassert_equal(ret, 0, "vc cal dac 0 failed: %d", ret);

	/* --- 11. commit (expect hint: vc cal exit) --- */
	printk("\n--- 11. vc cal 0 commit (expect hint: vc cal exit) ---\n");
	ret = run("vc cal 0 commit");
	zassert_equal(ret, 0, "vc cal commit failed: %d", ret);

	/* --- 12. exit --- */
	printk("\n--- 12. vc cal exit ---\n");
	ret = run("vc cal exit");
	zassert_equal(ret, 0, "vc cal exit failed: %d", ret);

	/* --- 13. verify mode returned to normal --- */
	printk("\n--- 13. vc sys status (should show NORMAL) ---\n");
	ret = run("vc sys status");
	zassert_equal(ret, 0, "vc sys status failed: %d", ret);

	printk("\n========== VERIFICATION COMPLETE ==========\n");
}

static void *setup(void)
{
	ctx = vc_init();
	__ASSERT_NO_MSG(ctx != NULL);
	vc_shell_init();
	return NULL;
}

static void teardown(void *f)
{
	ARG_UNUSED(f);
	vc_destroy(ctx);
}

ZTEST_SUITE(cal_verify, NULL, setup, NULL, NULL, teardown);
