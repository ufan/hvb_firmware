/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT adi_ad5541

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/dac.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ad5541, CONFIG_DAC_LOG_LEVEL);

struct ad5541_config {
	struct spi_dt_spec bus;
};

struct ad5541_data {
	uint32_t channel_count;
	uint32_t resolution;
};

static int ad5541_channel_setup(const struct device *dev,
				const struct dac_channel_cfg *channel_cfg)
{
	struct ad5541_data *data = dev->data;

	if (channel_cfg->channel_id != 0) {
		LOG_ERR("Unsupported channel %u", channel_cfg->channel_id);
		return -ENOTSUP;
	}
	if (channel_cfg->resolution != 16) {
		LOG_ERR("Unsupported resolution %u", channel_cfg->resolution);
		return -ENOTSUP;
	}

	data->channel_count = 1;
	data->resolution = channel_cfg->resolution;
	return 0;
}

static int ad5541_write_value(const struct device *dev, uint8_t channel,
			      uint32_t value)
{
	const struct ad5541_config *config = dev->config;
	uint16_t tx_data;

	if (channel != 0) {
		LOG_ERR("Unsupported channel %u", channel);
		return -ENOTSUP;
	}

	tx_data = (uint16_t)(value & 0xFFFF);

	const struct spi_buf tx_buf = {
		.buf = &tx_data,
		.len = sizeof(tx_data),
	};
	const struct spi_buf_set tx = {
		.buffers = &tx_buf,
		.count = 1,
	};

	return spi_write_dt(&config->bus, &tx);
}

static int ad5541_init(const struct device *dev)
{
	const struct ad5541_config *config = dev->config;
	struct ad5541_data *data = dev->data;

	if (!spi_is_ready_dt(&config->bus)) {
		LOG_ERR("SPI bus not ready");
		return -ENODEV;
	}

	data->channel_count = 1;
	data->resolution = 16;

	LOG_DBG("AD5541 initialized");
	return 0;
}

static const struct dac_driver_api ad5541_api = {
	.channel_setup = ad5541_channel_setup,
	.write_value = ad5541_write_value,
};

#define AD5541_INIT(n)							\
	static const struct ad5541_config ad5541_config_##n = {	\
		.bus = SPI_DT_SPEC_INST_GET(n,			\
			SPI_OP_MODE_MASTER |				\
			SPI_WORD_SET(16) |				\
			SPI_TRANSFER_MSB |				\
			SPI_LINES_SINGLE,				\
			0),						\
	};								\
									\
	static struct ad5541_data ad5541_data_##n;			\
									\
	DEVICE_DT_INST_DEFINE(n, ad5541_init, NULL,			\
		&ad5541_data_##n, &ad5541_config_##n,			\
		POST_KERNEL, CONFIG_DAC_INIT_PRIORITY,			\
		&ad5541_api);

DT_INST_FOREACH_STATUS_OKAY(AD5541_INIT)
