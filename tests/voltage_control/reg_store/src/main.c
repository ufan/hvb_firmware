/*
 * Copyright (c) 2026 Jianwei
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>

#include "reg_store/reg_store.h"
#include "regmap/vc_regs.h"

ZTEST_SUITE(reg_store, NULL, NULL, NULL, NULL, NULL);

ZTEST(reg_store, test_write_read_input)
{
	uint16_t val = 0;

	reg_store_write_input(0, 0xABCD);
	zassert_true(reg_store_read_input(0, &val));
	zassert_equal(val, 0xABCD);
}

ZTEST(reg_store, test_write_read_holding)
{
	uint16_t val = 0;

	reg_store_write_holding(5, 0x1234);
	zassert_true(reg_store_read_holding(5, &val));
	zassert_equal(val, 0x1234);
}

ZTEST(reg_store, test_out_of_range_read_returns_false)
{
	uint16_t val = 0xFFFF;

	zassert_false(reg_store_read_input(REG_STORE_SIZE, &val));
	zassert_false(reg_store_read_holding(REG_STORE_SIZE, &val));
	zassert_equal(val, 0xFFFF, "out must not be modified on false return");
}

ZTEST(reg_store, test_out_of_range_write_is_noop)
{
	uint16_t val = 0;

	/* Write to valid address, then attempt to corrupt via out-of-range write */
	reg_store_write_input(REG_STORE_SIZE - 1, 0x5555);
	reg_store_write_input(REG_STORE_SIZE, 0xDEAD);  /* noop */
	zassert_true(reg_store_read_input(REG_STORE_SIZE - 1, &val));
	zassert_equal(val, 0x5555);
}

ZTEST(reg_store, test_ch_block_mapping)
{
	uint16_t val = 0;

	/* Last register of ch15 block */
	uint16_t last_ch_addr = CH_BLOCK_BASE(15) + CH_BLOCK_SIZE - 1;

	zassert_equal(last_ch_addr, 679,
		      "ch15 last register should be 679");
	reg_store_write_input(last_ch_addr, 0x9999);
	zassert_true(reg_store_read_input(last_ch_addr, &val));
	zassert_equal(val, 0x9999);

	/* Extension block starts at 680 */
	zassert_equal(EXT_BLOCK_BASE, 680,
		      "extension block should start at 680");
	reg_store_write_input(EXT_BLOCK_BASE, 0x1111);
	zassert_true(reg_store_read_input(EXT_BLOCK_BASE, &val));
	zassert_equal(val, 0x1111);
}

ZTEST(reg_store, test_null_out_returns_false)
{
	reg_store_write_input(0, 42);
	zassert_false(reg_store_read_input(0, NULL));
	zassert_false(reg_store_read_holding(0, NULL));
}
