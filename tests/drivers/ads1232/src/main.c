/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/ztest.h>

#define ADS_NODE DT_NODELABEL(ads1232_test)

static const struct device *const adc = DEVICE_DT_GET(ADS_NODE);
static const struct gpio_dt_spec drdy = GPIO_DT_SPEC_GET(ADS_NODE, drdy_gpios);
static const struct gpio_dt_spec sclk = GPIO_DT_SPEC_GET(ADS_NODE, sclk_gpios);

static struct gpio_callback sclk_cb;
static unsigned int rising_edges;
static uint32_t sample_bits;
static bool force_high_after_data;

static void ads_model_sclk_changed(const struct device *port,
				   struct gpio_callback *cb,
				   gpio_port_pins_t pins)
{
	int level;

	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	level = gpio_emul_output_get(port, sclk.pin);
	if (level != 1) {
		return;
	}

	rising_edges++;
	if (rising_edges <= 24U) {
		unsigned int shift = 24U - rising_edges;

		zassert_ok(gpio_emul_input_set(drdy.port, drdy.pin,
					       (sample_bits >> shift) & 1U));
	} else if (rising_edges == 25U && force_high_after_data) {
		zassert_ok(gpio_emul_input_set(drdy.port, drdy.pin, 1));
	}
}

static void *ads1232_setup(void)
{
	zassert_true(device_is_ready(adc));

	gpio_init_callback(&sclk_cb, ads_model_sclk_changed, BIT(sclk.pin));
	zassert_ok(gpio_add_callback(sclk.port, &sclk_cb));
	return NULL;
}

static void ads1232_before(void *fixture)
{
	ARG_UNUSED(fixture);

	rising_edges = 0U;
	sample_bits = 0x123456U;
	force_high_after_data = true;
	zassert_ok(gpio_emul_input_set(drdy.port, drdy.pin, 0));
}

ZTEST(ads1232, test_read_uses_exactly_25_clocks_when_dout_stays_low)
{
	struct adc_sequence sequence = { 0 };
	int32_t sample;

	force_high_after_data = false;
	sequence.channels = BIT(0);
	sequence.buffer = &sample;
	sequence.buffer_size = sizeof(sample);
	sequence.resolution = 24;

	zassert_ok(adc_read(adc, &sequence));
	zassert_equal(rising_edges, 25U,
		      "unexpected clocks can start offset calibration: %u",
		      rising_edges);
}

ZTEST(ads1232, test_read_returns_signed_24_bit_sample)
{
	struct adc_sequence sequence = { 0 };
	int32_t sample;

	sample_bits = 0x923456U;
	sequence.channels = BIT(0);
	sequence.buffer = &sample;
	sequence.buffer_size = sizeof(sample);
	sequence.resolution = 24;

	zassert_ok(adc_read(adc, &sequence));
	zassert_equal(sample, (int32_t)0xff923456);
	zassert_equal(rising_edges, 25U);
}

ZTEST_SUITE(ads1232, NULL, ads1232_setup, ads1232_before, NULL, NULL);
