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

#define HEARTBEAT_INTERVAL_MS 500
#define MODBUS_SLAVE_ID 1
#define MODBUS_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(zephyr_modbus_serial)

static const struct gpio_dt_spec sys_run = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

static bool is_channel_status_register(uint16_t addr)
{
	return (addr >= 40 && addr <= 46) || (addr >= 80 && addr <= 86);
}

static int input_reg_rd(uint16_t addr, uint16_t *reg)
{
	uint32_t uptime_s = (uint32_t)(k_uptime_get() / 1000U);

	switch (addr) {
	case 0 ... 5:
		*reg = 0;
		break;
	case 6:
		*reg = 250; /* 25.0 C */
		break;
	case 7:
		*reg = 500; /* 50.0 %RH */
		break;
	case 8:
		*reg = uptime_s >> 16;
		break;
	case 9:
		*reg = uptime_s & 0xffff;
		break;
	case 10:
		*reg = 1; /* firmware version high */
		break;
	case 11:
		*reg = 0; /* firmware version low */
		break;
	case 12:
		*reg = 0; /* OTA idle */
		break;
	case 13:
		*reg = 0; /* OTA no error */
		break;
	case 14:
		*reg = 0; /* OTA packet sequence */
		break;
	default:
		if (!is_channel_status_register(addr)) {
			return -ENOTSUP;
		}
		*reg = 0;
		break;
	}

	printk("modbus FC04 read addr=%u value=%u\n", addr, *reg);
	return 0;
}

static int holding_reg_rd(uint16_t addr, uint16_t *reg)
{
	switch (addr) {
	case 0 ... 16:
		*reg = 0;
		break;
	case 17:
		*reg = MODBUS_SLAVE_ID;
		break;
	case 18:
		*reg = 0; /* 0 = 115200 bps */
		break;
	case 19:
		*reg = 0; /* no parameter action */
		break;
	default:
		if (!is_channel_status_register(addr)) {
			return -ENOTSUP;
		}
		*reg = 0;
		break;
	}

	printk("modbus FC03 read addr=%u value=%u\n", addr, *reg);
	return 0;
}

static struct modbus_user_callbacks modbus_callbacks = {
	.input_reg_rd = input_reg_rd,
	.holding_reg_rd = holding_reg_rd,
};

static const struct modbus_iface_param server_param = {
	.mode = MODBUS_MODE_RTU,
	.server = {
		.user_cb = &modbus_callbacks,
		.unit_id = MODBUS_SLAVE_ID,
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

int main(void)
{
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

	ret = init_modbus_server();
	if (ret < 0) {
		printk("Modbus RTU server initialization failed: %d\n", ret);
		return 0;
	}

	printk("jw_hvb Modbus RTU smoke server ready: slave=1 USART6 115200 8N1 RS485_DIR=PG11\n");

	while (1) {
		ret = gpio_pin_toggle_dt(&sys_run);
		if (ret < 0) {
			printk("Failed to toggle SYS_RUN GPIO: %d\n", ret);
		}

		printk("modbus smoke alive\n");
		k_msleep(HEARTBEAT_INTERVAL_MS);
	}
}
