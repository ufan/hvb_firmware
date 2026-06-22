/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/modbus/modbus.h>
#include <zephyr/sys/printk.h>

#include "voltage_control/modbus_adapter.h"
#include "voltage_control/vc.h"

#define HEARTBEAT_INTERVAL_MS 500
#define MODBUS_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(zephyr_modbus_serial)

static const struct gpio_dt_spec sys_run = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

static struct vc_mb_adapter *mb;

static void heartbeat_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);

	gpio_pin_toggle_dt(&sys_run);
	k_work_schedule(dwork, K_MSEC(HEARTBEAT_INTERVAL_MS));
}

static K_WORK_DELAYABLE_DEFINE(heartbeat_work, heartbeat_handler);

static int input_reg_rd(uint16_t addr, uint16_t *reg)
{
	enum vc_mb_result r = vc_mb_input_rd(mb, addr, reg);
	return r ? -EINVAL : 0;
}

static int holding_reg_rd(uint16_t addr, uint16_t *reg)
{
	enum vc_mb_result r = vc_mb_holding_rd(mb, addr, reg);
	return r ? -EINVAL : 0;
}

static int holding_reg_wr(uint16_t addr, uint16_t reg)
{
	enum vc_mb_result r = vc_mb_holding_wr(mb, addr, reg);
	return r ? -EINVAL : 0;
}

static struct modbus_user_callbacks modbus_callbacks = {
	.input_reg_rd = input_reg_rd,
	.holding_reg_rd = holding_reg_rd,
	.holding_reg_wr = holding_reg_wr,
};

static const struct modbus_iface_param server_param = {
	.mode = MODBUS_MODE_RTU,
	.server = {
		.user_cb = &modbus_callbacks,
		.unit_id = CONFIG_VC_MODBUS_UNIT_ID,
	},
	.serial = {
		.baud = CONFIG_VC_MODBUS_BAUD_RATE,
		.parity = UART_CFG_PARITY_NONE,
	},
};

static int init_modbus_server(void)
{
	const char iface_name[] = DEVICE_DT_NAME(MODBUS_NODE);
	int iface = modbus_iface_get_by_name(iface_name);

	if (iface < 0) {
		printk("Failed to get Modbus iface %s: %d\n", iface_name, iface);
		return iface;
	}

	return modbus_init_server(iface, server_param);
}

int main(void)
{
	struct vc_system_snapshot snap;
	int ret;

	if (!gpio_is_ready_dt(&sys_run)) {
		printk("SYS_RUN GPIO device is not ready\n");
		return 0;
	}

	ret = gpio_pin_configure_dt(&sys_run, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		printk("Failed to configure SYS_RUN GPIO: %d\n", ret);
		return 0;
	}

	struct vc_ctx *ctx = vc_init();
	if (!ctx) {
		printk("Failed to init vc\n");
		return 0;
	}

	ret = vc_ctx_start(ctx);
	if (ret != VC_OK) {
		printk("Failed to start vc: %d\n", ret);
		return 0;
	}

	mb = vc_mb_adapter_create(ctx);
	if (!mb) {
		printk("Failed to create Modbus adapter\n");
		return 0;
	}

	ret = init_modbus_server();
	if (ret < 0) {
		printk("Modbus RTU server initialization failed: %d\n", ret);
		return 0;
	}

	vc_query(ctx, vc_q_system_snapshot(&snap));
	printk("hvb_controller ready: slave=%d baud=%d"
	       " variant=%u channels=%u protocol=%u.%u\n",
	       CONFIG_VC_MODBUS_UNIT_ID, CONFIG_VC_MODBUS_BAUD_RATE,
	       snap.variant_id, snap.supported_channel_count,
	       snap.protocol_major, snap.protocol_minor);

	k_work_schedule(&heartbeat_work, K_NO_WAIT);
	return 0;
}
