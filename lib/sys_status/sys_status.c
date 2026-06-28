/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include "sys_status/sys_status.h"
#include "reg_store/reg_catalog.h"
#include "reg_store/reg_schema.h"

LOG_MODULE_REGISTER(sys_status, LOG_LEVEL_INF);

#define SYS_STATUS_FW_VERSION_HIGH 0
#define SYS_STATUS_FW_VERSION_LOW  1

#define SYS_STATUS_TEMP_SENTINEL  INT16_MIN
#define SYS_STATUS_HUMID_SENTINEL 0xFFFF

static const struct gpio_dt_spec sys_run =
	GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

#define SHT31_NODE DT_CHILD(DT_NODELABEL(i2c1), sht3xd_44)
static const struct device *sht31_dev = DEVICE_DT_GET_OR_NULL(SHT31_NODE);

static atomic_t env_temperature = ATOMIC_INIT(SYS_STATUS_TEMP_SENTINEL);
static atomic_t env_humidity = ATOMIC_INIT(SYS_STATUS_HUMID_SENTINEL);
static const uint32_t firmware_version =
	((uint32_t)SYS_STATUS_FW_VERSION_HIGH << 16) | SYS_STATUS_FW_VERSION_LOW;

static enum reg_status sys_status_reg_read(const struct reg_descriptor *desc,
					   union reg_value *value)
{
	switch (REG_ID_FIELD(desc->id)) {
	case REG_SYS_FIELD_UPTIME:
		value->u32 = (uint32_t)(k_uptime_get() / 1000);
		return REG_OK;
	case REG_SYS_FIELD_BOARD_TEMPERATURE:
		value->s16 = (int16_t)atomic_get(&env_temperature);
		return REG_OK;
	case REG_SYS_FIELD_BOARD_HUMIDITY:
		value->u16 = (uint16_t)atomic_get(&env_humidity);
		return REG_OK;
	default:
		return REG_NOT_FOUND;
	}
}

static const struct reg_owner sys_status_reg_owner = {
	.read = sys_status_reg_read,
};

REG_DESCRIPTOR_DEFINE(sys_status_temperature_reg,
	REG_SYS_ID(REG_SYS_FIELD_BOARD_TEMPERATURE),
	REG_S16, REG_RO, REG_MEASUREMENT_RAW, NULL, &sys_status_reg_owner);
REG_DESCRIPTOR_DEFINE(sys_status_humidity_reg,
	REG_SYS_ID(REG_SYS_FIELD_BOARD_HUMIDITY),
	REG_U16, REG_RO, REG_MEASUREMENT_RAW, NULL, &sys_status_reg_owner);
REG_DESCRIPTOR_DEFINE(sys_status_uptime_reg,
	REG_SYS_ID(REG_SYS_FIELD_UPTIME),
	REG_U32, REG_RO, REG_RUNTIME_STATE, NULL, &sys_status_reg_owner);
REG_DESCRIPTOR_DEFINE(sys_status_firmware_version_reg,
	REG_SYS_ID(REG_SYS_FIELD_FW_VERSION),
	REG_U32, REG_RO, REG_FIXED, &firmware_version, NULL);

static K_KERNEL_STACK_DEFINE(sys_status_stack,
			     CONFIG_SYS_STATUS_THREAD_STACK_SIZE);
static struct k_thread sys_status_thread;
static struct k_sem wake_sem;
static struct k_timer sys_status_timer;

static void sys_status_timer_handler(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	k_sem_give(&wake_sem);
}

static void read_environment(void)
{
	struct sensor_value temp, hum;
	int ret;

	if (sht31_dev == NULL || !device_is_ready(sht31_dev)) {
		return;
	}

	ret = sensor_sample_fetch(sht31_dev);
	if (ret < 0) {
		LOG_WRN("SHT3x fetch failed: %d", ret);
		atomic_set(&env_temperature, SYS_STATUS_TEMP_SENTINEL);
		atomic_set(&env_humidity, SYS_STATUS_HUMID_SENTINEL);
		return;
	}

	sensor_channel_get(sht31_dev, SENSOR_CHAN_AMBIENT_TEMP, &temp);
	sensor_channel_get(sht31_dev, SENSOR_CHAN_HUMIDITY, &hum);

	int32_t t = temp.val1 * 10 + temp.val2 / 100000;
	int32_t h = hum.val1 * 10 + hum.val2 / 100000;

	atomic_set(&env_temperature, (atomic_val_t)t);
	atomic_set(&env_humidity, (atomic_val_t)h);
}

static void sys_status_worker(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	uint32_t tick_count = 0;
	uint32_t sensor_ticks = CONFIG_SYS_STATUS_SENSOR_INTERVAL_MS /
				CONFIG_SYS_STATUS_HEARTBEAT_INTERVAL_MS;

	if (sensor_ticks == 0) {
		sensor_ticks = 1;
	}

	read_environment();

	while (true) {
		k_sem_take(&wake_sem, K_FOREVER);

		gpio_pin_toggle_dt(&sys_run);
		tick_count++;

		if (tick_count % sensor_ticks == 0) {
			read_environment();
		}
	}
}

struct sys_status_snapshot sys_status_get(void)
{
	return (struct sys_status_snapshot){
		.board_temperature = (int16_t)atomic_get(&env_temperature),
		.board_humidity = (uint16_t)atomic_get(&env_humidity),
		.uptime = (uint32_t)(k_uptime_get() / 1000),
		.fw_version_high = SYS_STATUS_FW_VERSION_HIGH,
		.fw_version_low = SYS_STATUS_FW_VERSION_LOW,
	};
}

/* SHT31 warm-reset workaround: force sensor out of periodic mode. */
#define SHT3XD_CMD_BREAK 0x3093
#define SHT3XD_I2C_ADDR  0x44

static int sht31_pre_init(void)
{
	const struct device *i2c = DEVICE_DT_GET(DT_NODELABEL(i2c1));

	if (!device_is_ready(i2c)) {
		return -ENODEV;
	}

	uint8_t cmd[2] = { SHT3XD_CMD_BREAK >> 8, SHT3XD_CMD_BREAK & 0xFF };

	i2c_write(i2c, cmd, sizeof(cmd), SHT3XD_I2C_ADDR);
	k_busy_wait(1000);
	return 0;
}

SYS_INIT(sht31_pre_init, POST_KERNEL, 80);

static int sys_status_init(void)
{
	if (!gpio_is_ready_dt(&sys_run)) {
		LOG_ERR("SYS_RUN GPIO not ready");
		return -ENODEV;
	}

	gpio_pin_configure_dt(&sys_run, GPIO_OUTPUT_INACTIVE);

	if (sht31_dev == NULL || !device_is_ready(sht31_dev)) {
		LOG_WRN("SHT3x not ready; environment data unavailable");
	}

	k_sem_init(&wake_sem, 0, 1);
	k_timer_init(&sys_status_timer, sys_status_timer_handler, NULL);
	k_timer_start(&sys_status_timer, K_NO_WAIT,
		      K_MSEC(CONFIG_SYS_STATUS_HEARTBEAT_INTERVAL_MS));

	k_thread_create(&sys_status_thread, sys_status_stack,
			K_KERNEL_STACK_SIZEOF(sys_status_stack),
			sys_status_worker, NULL, NULL, NULL,
			CONFIG_SYS_STATUS_THREAD_PRIORITY, 0, K_NO_WAIT);

	LOG_INF("initialized (heartbeat=%dms sensor=%dms)",
		CONFIG_SYS_STATUS_HEARTBEAT_INTERVAL_MS,
		CONFIG_SYS_STATUS_SENSOR_INTERVAL_MS);

	return 0;
}

SYS_INIT(sys_status_init, POST_KERNEL, 90);
