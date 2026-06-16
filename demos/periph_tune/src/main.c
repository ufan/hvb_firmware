/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/printk.h>

/* SHT31 via existing Zephyr sht3xd driver on I2C1 */
#define SHT31_NODE DT_CHILD(DT_NODELABEL(i2c1), sht3xd_44)
static const struct device *sht31_dev = DEVICE_DT_GET_OR_NULL(SHT31_NODE);

static int cmd_sht31_read(const struct shell *sh, size_t argc, char **argv)
{
	int ret;
	struct sensor_value temp, hum;

	if (!device_is_ready(sht31_dev)) {
		shell_fprintf(sh, SHELL_ERROR, "SHT31 not ready\n");
		return -ENODEV;
	}

	ret = sensor_sample_fetch(sht31_dev);
	if (ret < 0) {
		shell_fprintf(sh, SHELL_ERROR, "SHT31 fetch failed: %d\n", ret);
		return ret;
	}

	ret = sensor_channel_get(sht31_dev, SENSOR_CHAN_AMBIENT_TEMP, &temp);
	if (ret < 0) {
		shell_fprintf(sh, SHELL_ERROR, "SHT31 temp read failed: %d\n", ret);
		return ret;
	}

	ret = sensor_channel_get(sht31_dev, SENSOR_CHAN_HUMIDITY, &hum);
	if (ret < 0) {
		shell_fprintf(sh, SHELL_ERROR, "SHT31 humidity read failed: %d\n", ret);
		return ret;
	}

	shell_fprintf(sh, SHELL_NORMAL,
		"T=%.2f C  H=%.2f %%\n",
		sensor_value_to_double(&temp),
		sensor_value_to_double(&hum));
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_sht31,
	SHELL_CMD(read, NULL, "Read temperature and humidity", cmd_sht31_read),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(sht31, &sub_sht31, "SHT31 temperature/humidity sensor", NULL);

int main(void)
{
	printk("=== periph_tune: jw_hvb peripheral tuning shell ===\n");

	if (!device_is_ready(sht31_dev)) {
		printk("SHT31 device not ready\n");
	} else {
		printk("SHT31 ready on I2C1 addr 0x44\n");
	}

	printk("Type 'help' for commands\n");
	return 0;
}
