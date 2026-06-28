/*
 * Copyright (c) 2026 Jianwei
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include "modbus_adapter/modbus_adapter.h"
#include "reg_store/reg_catalog.h"
#include "reg_store/reg_schema.h"

#define MB_SHELL_TIMEOUT K_SECONDS(1)

static int read_u16(enum reg_modbus_field field, uint16_t *out)
{
	union reg_value value = {};
	enum reg_status status = reg_read(REG_MODBUS_ID(field), &value);

	if (status != REG_OK) {
		return -EIO;
	}
	*out = value.u16;
	return 0;
}

static int write_u16(enum reg_modbus_field field, uint16_t input)
{
	union reg_value value = { .u16 = input };

	return reg_write(REG_MODBUS_ID(field), value, MB_SHELL_TIMEOUT) == REG_OK
		? 0 : -EIO;
}

static uint32_t baud_code_to_hz(uint16_t code)
{
	switch (code) {
	case VC_BAUD_RATE_9600:   return 9600;
	case VC_BAUD_RATE_115200: return 115200;
	default:                  return 0;
	}
}

/* ------------------------------------------------------------------ */
/* mb status                                                            */
/* ------------------------------------------------------------------ */

static int cmd_mb_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	uint16_t active_slave;
	uint16_t active_baud;
	uint16_t boot_slave;
	uint16_t boot_baud;
	int ret = read_u16(REG_MODBUS_FIELD_ACTIVE_SLAVE_ADDRESS,
			   &active_slave);

	if (ret < 0 ||
	    read_u16(REG_MODBUS_FIELD_ACTIVE_BAUD_RATE_CODE, &active_baud) < 0) {
		return ret;
	}
	shell_print(sh, "Modbus Adapter");
	shell_print(sh, "  active:    slave=%d baud=%d (%d Hz)",
		    active_slave, active_baud, baud_code_to_hz(active_baud));
	if (IS_ENABLED(CONFIG_SETTINGS) &&
	    read_u16(REG_MODBUS_FIELD_NEXT_BOOT_SLAVE_ADDRESS, &boot_slave) == 0 &&
	    read_u16(REG_MODBUS_FIELD_NEXT_BOOT_BAUD_RATE_CODE, &boot_baud) == 0) {
		bool differs = active_slave != boot_slave || active_baud != boot_baud;

		shell_print(sh, "  next boot: slave=%d baud=%d (%d Hz)%s",
			    boot_slave, boot_baud, baud_code_to_hz(boot_baud),
			    differs ? " — restart required" : "");
	} else {
		shell_print(sh, "  next boot: volatile defaults");
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* mb set slave <1-247>                                                  */
/* ------------------------------------------------------------------ */

static int cmd_mb_set_slave(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	unsigned long val = strtoul(argv[1], NULL, 10);

	if (val < 1 || val > 247) {
		shell_error(sh, "slave address must be 1-247");
		return -EINVAL;
	}

	int ret = write_u16(REG_MODBUS_FIELD_ACTIVE_SLAVE_ADDRESS,
			    (uint16_t)val);

	if (ret < 0) {
		shell_error(sh, "failed to set slave address: %d", ret);
		return -EIO;
	}
	shell_print(sh, "slave address set to %lu", val);
	return 0;
}

/* ------------------------------------------------------------------ */
/* mb set baud <code>                                                    */
/* ------------------------------------------------------------------ */

static int cmd_mb_set_baud(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	unsigned long val = strtoul(argv[1], NULL, 10);

	switch ((uint16_t)val) {
	case VC_BAUD_RATE_115200:
	case VC_BAUD_RATE_9600:
		break;
	default:
		shell_error(sh, "baud rate code must be 0 (115200) or 1 (9600)");
		return -EINVAL;
	}

	int ret = write_u16(REG_MODBUS_FIELD_ACTIVE_BAUD_RATE_CODE,
			    (uint16_t)val);

	if (ret < 0) {
		shell_error(sh, "failed to set baud rate: %d", ret);
		return -EIO;
	}
	shell_print(sh, "baud rate set to %lu (%u Hz)", val, baud_code_to_hz((uint16_t)val));
	return 0;
}

/* ------------------------------------------------------------------ */
/* mb save                                                               */
/* ------------------------------------------------------------------ */

static int cmd_mb_save(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	int ret = write_u16(REG_MODBUS_FIELD_CONFIG_SAVE, 1U);

	if (ret < 0) {
		shell_error(sh, "save failed: %d", ret);
		return -EIO;
	}
	shell_print(sh, "config saved");
	return 0;
}

/* ------------------------------------------------------------------ */
/* mb load                                                               */
/* ------------------------------------------------------------------ */

static int cmd_mb_load(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	int ret = write_u16(REG_MODBUS_FIELD_CONFIG_LOAD, 1U);

	if (ret < 0) {
		shell_error(sh, "load failed: %d", ret);
		return -EIO;
	}

	uint16_t slave;
	uint16_t baud;

	(void)read_u16(REG_MODBUS_FIELD_ACTIVE_SLAVE_ADDRESS, &slave);
	(void)read_u16(REG_MODBUS_FIELD_ACTIVE_BAUD_RATE_CODE, &baud);
	shell_print(sh, "config loaded: slave=%d baud=%d Hz",
		    slave, baud_code_to_hz(baud));
	return 0;
}

/* ------------------------------------------------------------------ */
/* mb factory                                                            */
/* ------------------------------------------------------------------ */

static int cmd_mb_factory(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	int ret = write_u16(REG_MODBUS_FIELD_CONFIG_FACTORY, 1U);

	if (ret < 0) {
		shell_error(sh, "factory reset failed: %d", ret);
		return -EIO;
	}

	uint16_t slave;
	uint16_t baud;

	(void)read_u16(REG_MODBUS_FIELD_ACTIVE_SLAVE_ADDRESS, &slave);
	(void)read_u16(REG_MODBUS_FIELD_ACTIVE_BAUD_RATE_CODE, &baud);
	shell_print(sh, "factory reset: slave=%d baud=%d Hz",
		    slave, baud_code_to_hz(baud));
	return 0;
}

/* ------------------------------------------------------------------ */
/* Shell command registration                                          */
/* ------------------------------------------------------------------ */

SHELL_STATIC_SUBCMD_SET_CREATE(sub_mb_set,
	SHELL_CMD_ARG(slave, NULL, "Set slave address <1-247>", cmd_mb_set_slave, 2, 0),
	SHELL_CMD_ARG(baud, NULL, "Set baud rate <0=115200|1=9600>", cmd_mb_set_baud, 2, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_mb,
	SHELL_CMD(status, NULL, "Show Modbus adapter config", cmd_mb_status),
	SHELL_CMD(set, &sub_mb_set, "Set config field", NULL),
	SHELL_CMD(save, NULL, "Save adapter config to NVS", cmd_mb_save),
	SHELL_CMD(load, NULL, "Load adapter config from NVS", cmd_mb_load),
	SHELL_CMD(factory, NULL, "Factory-reset adapter config", cmd_mb_factory),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(mb, &sub_mb, "Modbus adapter config", NULL);
