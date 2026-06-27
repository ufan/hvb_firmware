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
#include "voltage_control/vc_shell.h"
#endif

#if IS_ENABLED(CONFIG_SYS_STATUS_SHELL)
#include "sys_status/sys_status.h"
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

#if IS_ENABLED(CONFIG_MODBUS_ADAPTER_SHELL)
	modbus_adapter_shell_init();
#endif

#if IS_ENABLED(CONFIG_SYS_STATUS_SHELL)
	sys_status_shell_init();
#endif

	printk("hvb_controller ready\n");
	return 0;
}
