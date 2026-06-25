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
#include <zephyr/random/random.h>
#include <zephyr/sys/printk.h>

#include <dt-bindings/voltage_control/capabilities.h>
#include "voltage_control/vc.h"
#include "voltage_control/modbus_adapter.h"

#define HEARTBEAT_INTERVAL_MS 500
#define MODBUS_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(zephyr_modbus_serial)

static const struct gpio_dt_spec sys_run = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static struct vc_mb_adapter *mb;

static int16_t gen_noise(int16_t amplitude)
{
	uint32_t r = sys_rand32_get();

	return (int16_t)(r % (uint32_t)(amplitude * 2 + 1)) - amplitude;
}

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
		.unit_id = 1,
	},
	.serial = {
		.baud = 115200,
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

static const struct vc_channel_entry sim_channels[] = {
	{ .dev = NULL, .index = 0,
	  .capabilities = CH_CAP_OUTPUT_ENABLE | CH_CAP_RAW_OUTPUT_DRIVE |
			  CH_CAP_VOLTAGE_MEASUREMENT |
			  CH_CAP_CURRENT_MEASUREMENT },
	{ .dev = NULL, .index = 1,
	  .capabilities = CH_CAP_OUTPUT_ENABLE | CH_CAP_RAW_OUTPUT_DRIVE |
			  CH_CAP_VOLTAGE_MEASUREMENT |
			  CH_CAP_CURRENT_MEASUREMENT },
};

int main(void)
{
	struct vc_ctx *ctx;
	struct vc_system_snapshot system_snapshot;
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

	ctx = vc_init_custom(sim_channels, ARRAY_SIZE(sim_channels));
	if (!ctx) {
		printk("Failed to initialize voltage control\n");
		return 0;
	}
	vc_query(ctx, vc_q_system_snapshot(&system_snapshot));

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

	printk("modbus_sim ready: slave=1 USART6 115200 8N1 RS485_DIR=PG11"
	       " variant=%u channels=%u protocol=%u.%u simulated_runtime=1\n",
	       system_snapshot.variant_id,
	       system_snapshot.supported_channel_count,
	       system_snapshot.protocol_major, system_snapshot.protocol_minor);

	while (1) {
		ret = gpio_pin_toggle_dt(&sys_run);
		if (ret < 0) {
			printk("Failed to toggle SYS_RUN GPIO: %d\n", ret);
		}

		for (uint8_t ch = 0; ch < system_snapshot.supported_channel_count;
		     ch++) {
			struct vc_channel_snapshot snap;

			if (vc_query(ctx, vc_q_channel_snapshot(ch, &snap))
			    == VC_OK) {
				struct vc_measurement_snapshot meas = {
					.channel = ch,
					.generation = 1,
					.timestamp_ms = (uint32_t)k_uptime_get_32(),
					.present_mask = VC_MEAS_PRESENT_VOLTAGE |
							VC_MEAS_PRESENT_CURRENT,
				};
				int32_t v = snap.operational_target_voltage +
					    gen_noise(5);
				int32_t i = snap.operational_target_voltage / 2 +
					    gen_noise(20);

				if (v > 20000) v = 20000;
				if (v < 0) v = 0;
				if (i > 32767) i = 32767;
				if (i < 0) i = 0;
				meas.raw_voltage = v;
				meas.raw_current = i;
				vc_dispatch(ctx, vc_cmd_measurement(&meas),
					    K_FOREVER);
			}
		}

		k_msleep(HEARTBEAT_INTERVAL_MS);
	}
}
