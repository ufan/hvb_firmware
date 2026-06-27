/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>

#include "reg_store/reg_store.h"

static uint16_t _input[REG_STORE_SIZE];
static uint16_t _holding[REG_STORE_SIZE];
static struct k_spinlock store_lock;

void reg_store_write_input(uint16_t addr, uint16_t val)
{
	if (addr >= REG_STORE_SIZE) {
		return;
	}
	k_spinlock_key_t key = k_spin_lock(&store_lock);
	_input[addr] = val;
	k_spin_unlock(&store_lock, key);
}

void reg_store_write_holding(uint16_t addr, uint16_t val)
{
	if (addr >= REG_STORE_SIZE) {
		return;
	}
	k_spinlock_key_t key = k_spin_lock(&store_lock);
	_holding[addr] = val;
	k_spin_unlock(&store_lock, key);
}

bool reg_store_read_input(uint16_t addr, uint16_t *out)
{
	if (addr >= REG_STORE_SIZE || out == NULL) {
		return false;
	}
	k_spinlock_key_t key = k_spin_lock(&store_lock);
	*out = _input[addr];
	k_spin_unlock(&store_lock, key);
	return true;
}

bool reg_store_read_holding(uint16_t addr, uint16_t *out)
{
	if (addr >= REG_STORE_SIZE || out == NULL) {
		return false;
	}
	k_spinlock_key_t key = k_spin_lock(&store_lock);
	*out = _holding[addr];
	k_spin_unlock(&store_lock, key);
	return true;
}
