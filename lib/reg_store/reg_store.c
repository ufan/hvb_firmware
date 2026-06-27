/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>

#include "reg_store/reg_store.h"

/*
 * N_CHANNELS is the number of channels configured in DTS for this build.
 * The store is sized to exactly fit: sys(40) + N*ch(40) + ext(80).
 *
 * Wire addresses (from reg_map.h) are mapped to compact store indices:
 *   sys [0, 39]:              idx = addr           (identity)
 *   ch  [40, 40+N*40-1]:     idx = addr           (identity — channels are contiguous)
 *   ch  [40+N*40, 679]:      → rejected           (unconfigured channel slots)
 *   ext [680, 759]:           idx = addr - 680 + 40 + N*40
 */
#define N_CHANNELS  DT_CHILD_NUM_STATUS_OKAY(DT_NODELABEL(vc_controller))
#define CH_END      (40U + (uint16_t)(N_CHANNELS) * 40U)
#define STORE_SIZE  (CH_END + 80U)

static uint16_t _input[STORE_SIZE];
static uint16_t _holding[STORE_SIZE];
static struct k_spinlock store_lock;

static bool addr_to_idx(uint16_t addr, uint16_t *idx)
{
	if (addr < CH_END) {
		*idx = addr;
		return true;
	}
	if (addr >= EXT_BLOCK_BASE && addr < EXT_BLOCK_BASE + 80U) {
		*idx = CH_END + (addr - EXT_BLOCK_BASE);
		return true;
	}
	return false;
}

void reg_store_write_input(uint16_t addr, uint16_t val)
{
	uint16_t idx;

	if (!addr_to_idx(addr, &idx)) {
		return;
	}
	k_spinlock_key_t key = k_spin_lock(&store_lock);
	_input[idx] = val;
	k_spin_unlock(&store_lock, key);
}

void reg_store_write_holding(uint16_t addr, uint16_t val)
{
	uint16_t idx;

	if (!addr_to_idx(addr, &idx)) {
		return;
	}
	k_spinlock_key_t key = k_spin_lock(&store_lock);
	_holding[idx] = val;
	k_spin_unlock(&store_lock, key);
}

bool reg_store_read_input(uint16_t addr, uint16_t *out)
{
	uint16_t idx;

	if (out == NULL || !addr_to_idx(addr, &idx)) {
		return false;
	}
	k_spinlock_key_t key = k_spin_lock(&store_lock);
	*out = _input[idx];
	k_spin_unlock(&store_lock, key);
	return true;
}

bool reg_store_read_holding(uint16_t addr, uint16_t *out)
{
	uint16_t idx;

	if (out == NULL || !addr_to_idx(addr, &idx)) {
		return false;
	}
	k_spinlock_key_t key = k_spin_lock(&store_lock);
	*out = _holding[idx];
	k_spin_unlock(&store_lock, key);
	return true;
}
