/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "voltage_control/vc.h"

#if IS_ENABLED(CONFIG_UI_MODBUS_RTU)
#include "modbus_adapter/modbus_adapter.h"
#endif

#if IS_ENABLED(CONFIG_VC_SHELL)
#include "shell_adapter/shell_adapter.h"
#endif

int main(void)
{
	struct vc_ctx *ctx = vc_init();

	if (!ctx) {
		printk("Failed to init vc\n");
		return 0;
	}

	int ret = vc_ctx_start(ctx);

	if (ret != VC_OK) {
		printk("Failed to start vc: %d\n", ret);
		return 0;
	}

#if IS_ENABLED(CONFIG_UI_MODBUS_RTU)
	ret = modbus_adapter_init(ctx);
	if (ret < 0) {
		return 0;
	}
#endif

#if IS_ENABLED(CONFIG_VC_SHELL)
	vc_shell_init(ctx);
#endif

	printk("hvb_controller ready\n");
	return 0;
}
