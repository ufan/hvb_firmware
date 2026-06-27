/*
 * Copyright (c) 2026 Jianwei
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>

#include "reg_store/reg_store.h"

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

/*
 * Wire addresses for unconfigured channel slots (ch2..15) must be rejected.
 * Wire addresses past the extension block must also be rejected.
 */
ZTEST(reg_store, test_out_of_range_read_returns_false)
{
	uint16_t val = 0xFFFF;

	/* ch2 wire addr — only 2 channels configured in this test build */
	zassert_false(reg_store_read_input(CH_BLOCK_BASE(2), &val));
	zassert_false(reg_store_read_holding(CH_BLOCK_BASE(2), &val));
	/* past end of extension block */
	zassert_false(reg_store_read_input(EXT_BLOCK_BASE + 80U, &val));
	zassert_false(reg_store_read_holding(EXT_BLOCK_BASE + 80U, &val));
	zassert_equal(val, 0xFFFF, "out must not be modified on false return");
}

ZTEST(reg_store, test_out_of_range_write_is_noop)
{
	uint16_t val = 0;
	uint16_t last_ext = EXT_BLOCK_BASE + 79U;

	reg_store_write_input(last_ext, 0x5555);
	reg_store_write_input(EXT_BLOCK_BASE + 80U, 0xDEAD);  /* noop */
	zassert_true(reg_store_read_input(last_ext, &val));
	zassert_equal(val, 0x5555);
}

/*
 * Verify compact channel mapping: configured channel slots are accessible at
 * their protocol wire addresses; unconfigured slots are rejected; the extension
 * block is accessible at its fixed protocol wire address (680) regardless of
 * how many channels are configured; channel and extension blocks are independent.
 */
ZTEST(reg_store, test_compact_ch_mapping)
{
	uint16_t val = 0;

	/* ch0 and ch1 wire addresses are accessible */
	reg_store_write_input(CH_BLOCK_BASE(0), 0xAAAA);
	reg_store_write_input(CH_BLOCK_BASE(1) + CH_BLOCK_SIZE - 1, 0xBBBB);
	zassert_true(reg_store_read_input(CH_BLOCK_BASE(0), &val));
	zassert_equal(val, 0xAAAA);
	zassert_true(reg_store_read_input(CH_BLOCK_BASE(1) + CH_BLOCK_SIZE - 1, &val));
	zassert_equal(val, 0xBBBB);

	/* ch2 wire address is rejected (only 2 channels in this test build) */
	zassert_false(reg_store_read_input(CH_BLOCK_BASE(2), &val));

	/* Extension block is accessible at its protocol wire address (always 680) */
	zassert_equal(EXT_BLOCK_BASE, 680U);
	reg_store_write_input(EXT_BLOCK_BASE, 0x1111);
	zassert_true(reg_store_read_input(EXT_BLOCK_BASE, &val));
	zassert_equal(val, 0x1111);

	reg_store_write_input(EXT_BLOCK_BASE + 79U, 0x2222);
	zassert_true(reg_store_read_input(EXT_BLOCK_BASE + 79U, &val));
	zassert_equal(val, 0x2222);

	/* ch0 and ext block are independent — no aliasing */
	zassert_true(reg_store_read_input(CH_BLOCK_BASE(0), &val));
	zassert_equal(val, 0xAAAA);
}

ZTEST(reg_store, test_null_out_returns_false)
{
	reg_store_write_input(0, 42);
	zassert_false(reg_store_read_input(0, NULL));
	zassert_false(reg_store_read_holding(0, NULL));
}
