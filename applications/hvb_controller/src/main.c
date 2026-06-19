/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

 /*
 * This is the main application for the HVB controller firmware. It initializes the system and starts up the modbus server.
 * Other components are initialized through the domain and runtime initialization separately, and the main function is kept clean for better readability and maintainability.
 */

#include <errno.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/modbus/modbus.h>
#include <zephyr/sys/printk.h>

#include "regmap/hvb_regs.h"
#include "voltage_control/domain.h"
#include "voltage_control/modbus_adapter.h"
#include "voltage_control/runtime.h"
#include "voltage_control/vc_channel.h"

#define HEARTBEAT_INTERVAL_MS 500
#define MODBUS_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(zephyr_modbus_serial)

static const struct gpio_dt_spec sys_run = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

extern const struct vc_channel_entry vc_domain_channels[];
extern const size_t vc_domain_channel_count;
// todo: dynamically allocate object should be avoided in MCU firmware, refactor to have global static objects instead of heap allocated ones
static struct vc_mb_adapter *mb;
static struct vc_runtime *runtime;


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

// todo: slave id and braudrate should be configurable: either through device tree or compile-time configuration, 
// or even runtime configuration through a special modbus register, refactor to support that
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

int main(void)
{
	struct domain *domain; // todo: bad idea to use daynmacilly allocated domain in mcu firemware, refactor to have a global static domain instead
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

	// todo: do we really need to dynamically allocate the domain and explicitly manage its lifecycle?
	domain = domain_create(vc_domain_channels, vc_domain_channel_count);
	if (!domain) {
		printk("Failed to create voltage-control domain\n");
		return 0;
	}

	runtime = vc_runtime_create(domain);
	if (!runtime) {
		printk("Failed to create runtime\n");
		return 0;
	}

	domain_get_system_snapshot(domain, &system_snapshot);

	mb = vc_mb_adapter_create(domain);
	if (!mb) {
		printk("Failed to create Modbus adapter\n");
		return 0;
	}

	ret = init_modbus_server();
	if (ret < 0) {
		printk("Modbus RTU server initialization failed: %d\n", ret);
		return 0;
	}

	printk("hvb_controller ready: slave=1 USART6 115200 8N1 RS485_DIR=PG11"
	       " variant=%u channels=%u protocol=%u.%u hardware_runtime=pending\n",
	       domain_get_variant_id(domain),
	       domain_get_supported_channel_count(domain),
	       system_snapshot.protocol_major, system_snapshot.protocol_minor);

	// todo: prefer heartbeat code in a separate task and use k_work for it (also separate file, keep main function clean)
	while (1) {
		ret = gpio_pin_toggle_dt(&sys_run);
		if (ret < 0) {
			printk("Failed to toggle SYS_RUN GPIO: %d\n", ret);
		}

		// todo: meaningless, domain can fetch actual uptime from k_uptime_get() internally, refactor to remove this
		domain_set_uptime(domain, (uint32_t)(k_uptime_get() / 1000));
		k_msleep(HEARTBEAT_INTERVAL_MS);
	}
}
