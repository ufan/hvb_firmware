/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>
#include <zephyr/sys/iterable_sections.h>

#include "voltage_control/provider_bus.h"
#include "voltage_control/vc_channel.h"

extern struct k_msgq vc_provider_msgq;

/* ---- Fake provider device ---- */

static int fake_start_count;
static int fake_start_ret;
static int fake_notify_count;
static uint32_t fake_notify_last_version;
static int fake_notify_ret;

static int fake_start(const struct device *dev)
{
	ARG_UNUSED(dev);
	fake_start_count++;
	return fake_start_ret;
}

static int fake_notify_config_changed(const struct device *dev, uint32_t version)
{
	ARG_UNUSED(dev);
	fake_notify_count++;
	fake_notify_last_version = version;
	return fake_notify_ret;
}

static const struct vc_channel_api fake_api = {
	.start = fake_start,
	.notify_config_changed = fake_notify_config_changed,
};

static const struct vc_channel_api fake_api_start_only = {
	.start = fake_start,
};

static const struct vc_channel_api fake_api_null = {0};

DEVICE_DEFINE(fake_ch0, "fake_ch0", NULL, NULL, NULL, NULL,
	      POST_KERNEL, 99, &fake_api);
DEVICE_DEFINE(fake_ch1, "fake_ch1", NULL, NULL, NULL, NULL,
	      POST_KERNEL, 99, &fake_api_start_only);
DEVICE_DEFINE(fake_ch_null_api, "fake_ch_null", NULL, NULL, NULL, NULL,
	      POST_KERNEL, 99, &fake_api_null);

STRUCT_SECTION_ITERABLE(vc_provider_binding, fake_binding_0) = {
	.channel = 0,
	.dev = DEVICE_GET(fake_ch0),
	.config_slot = &vc_runtime_config_slots[0],
	.route_bit = BIT(0),
};

STRUCT_SECTION_ITERABLE(vc_provider_binding, fake_binding_1) = {
	.channel = 1,
	.dev = DEVICE_GET(fake_ch1),
	.config_slot = &vc_runtime_config_slots[1],
	.route_bit = BIT(1),
};

STRUCT_SECTION_ITERABLE_NAMED(vc_measurement_buffer_entry,
	_0_, test_meas_buf_0) = {0};
STRUCT_SECTION_ITERABLE_NAMED(vc_measurement_buffer_entry,
	_1_, test_meas_buf_1) = {0};

static void reset_fakes(void *fixture)
{
	ARG_UNUSED(fixture);
	fake_start_count = 0;
	fake_start_ret = 0;
	fake_notify_count = 0;
	fake_notify_last_version = 0;
	fake_notify_ret = 0;
	vc_provider_bus_init();
}

ZTEST_SUITE(voltage_control_provider_bus, NULL, NULL, reset_fakes, NULL, NULL);

/* ---- binding_count ---- */

ZTEST(voltage_control_provider_bus, test_binding_count_reflects_registered_bindings)
{
	zassert_equal(vc_provider_bus_binding_count(), 2);
}

/* ---- start_all ---- */

ZTEST(voltage_control_provider_bus, test_start_all_calls_start_on_each_binding)
{
	zassert_equal(vc_provider_bus_start_all(), VC_OK);
	zassert_equal(fake_start_count, 2);
}

ZTEST(voltage_control_provider_bus, test_start_all_stops_on_first_failure)
{
	fake_start_ret = -EIO;

	zassert_equal(vc_provider_bus_start_all(), VC_ERR_UNSAFE_STATE);
	zassert_equal(fake_start_count, 1);
}

/* ---- notify_channel ---- */

ZTEST(voltage_control_provider_bus, test_notify_channel_dispatches_to_matching_binding)
{
	zassert_equal(vc_provider_bus_notify_channel(0, 42), VC_OK);
	zassert_equal(fake_notify_count, 1);
	zassert_equal(fake_notify_last_version, 42);
}

ZTEST(voltage_control_provider_bus, test_notify_channel_returns_error_on_unknown_channel)
{
	zassert_equal(vc_provider_bus_notify_channel(99, 1),
		      VC_ERR_UNSUPPORTED_CHANNEL);
	zassert_equal(fake_notify_count, 0);
}

ZTEST(voltage_control_provider_bus, test_notify_channel_propagates_driver_error)
{
	fake_notify_ret = -EIO;

	zassert_equal(vc_provider_bus_notify_channel(0, 1),
		      VC_ERR_UNSAFE_STATE);
}

ZTEST(voltage_control_provider_bus, test_notify_channel_tolerates_null_notify_callback)
{
	/* Channel 1 has fake_api_start_only with no notify_config_changed */
	zassert_equal(vc_provider_bus_notify_channel(1, 5), VC_OK);
	zassert_equal(fake_notify_count, 0);
}

/* ---- dispatch_one ---- */

ZTEST(voltage_control_provider_bus, test_dispatch_one_config_changed_notifies_channel)
{
	struct vc_runtime_config_snapshot cfg = {
		.channel = 0,
		.version = 10,
	};

	zassert_equal(vc_provider_bus_publish_config(0, &cfg), VC_OK);
	zassert_equal(vc_provider_bus_dispatch_one(K_NO_WAIT), VC_OK);
	zassert_equal(fake_notify_count, 1);
	zassert_equal(fake_notify_last_version, 10);
}

ZTEST(voltage_control_provider_bus, test_dispatch_one_returns_error_on_empty_queue)
{
	zassert_equal(vc_provider_bus_dispatch_one(K_NO_WAIT),
		      VC_ERR_UNSAFE_STATE);
}

ZTEST(voltage_control_provider_bus, test_dispatch_one_stop_message_does_not_notify)
{
	struct vc_provider_msg msg = {
		.type = VC_PROVIDER_MSG_STOP,
		.channel = 0,
	};

	zassert_equal(k_msgq_put(&vc_provider_msgq, &msg, K_NO_WAIT), 0);
	zassert_equal(vc_provider_bus_dispatch_one(K_NO_WAIT), VC_OK);
	zassert_equal(fake_notify_count, 0);
}

ZTEST(voltage_control_provider_bus, test_dispatch_one_sample_now_notifies_channel)
{
	struct vc_provider_msg msg = {
		.type = VC_PROVIDER_MSG_SAMPLE_NOW,
		.channel = 0,
		.config_version = 7,
	};

	zassert_equal(k_msgq_put(&vc_provider_msgq, &msg, K_NO_WAIT), 0);
	zassert_equal(vc_provider_bus_dispatch_one(K_NO_WAIT), VC_OK);
	zassert_equal(fake_notify_count, 1);
	zassert_equal(fake_notify_last_version, 7);
}

/* ---- config slot round-trip through binding ---- */

ZTEST(voltage_control_provider_bus, test_publish_config_is_visible_through_binding_slot)
{
	struct vc_runtime_config_snapshot cfg = {
		.channel = 0,
		.version = 15,
		.raw_output_drive = 999,
	};
	const struct vc_runtime_config_snapshot *borrowed;

	zassert_equal(vc_provider_bus_publish_config(0, &cfg), VC_OK);

	borrowed = vc_provider_bus_acquire_config(0);
	zassert_not_null(borrowed);
	zassert_equal(borrowed->version, 15);
	zassert_equal(borrowed->raw_output_drive, 999);
	vc_provider_bus_release_config(0);
}

/* ---- validation ---- */

ZTEST(voltage_control_provider_bus, test_publish_config_rejects_bad_channel)
{
	struct vc_runtime_config_snapshot cfg = { .channel = 99 };

	zassert_equal(vc_provider_bus_publish_config(99, &cfg),
		      VC_ERR_INVALID_VALUE);
}

ZTEST(voltage_control_provider_bus, test_publish_config_rejects_null)
{
	zassert_equal(vc_provider_bus_publish_config(0, NULL),
		      VC_ERR_INVALID_VALUE);
}

ZTEST(voltage_control_provider_bus, test_acquire_config_rejects_bad_channel)
{
	zassert_is_null(vc_provider_bus_acquire_config(99));
}

/* ---- measurement buffer ---- */

ZTEST(voltage_control_provider_bus, test_measurement_buffer_count_matches_entries)
{
	zassert_equal(vc_measurement_buffer_count(), 2);
}

ZTEST(voltage_control_provider_bus, test_measurement_buffer_store_read_round_trip)
{
	struct vc_measurement_snapshot in = {
		.channel = 0,
		.generation = 3,
		.timestamp_ms = 500,
		.present_mask = VC_MEAS_PRESENT_VOLTAGE,
		.raw_voltage = 42,
	};
	struct vc_measurement_snapshot out;

	vc_measurement_buffer_init();
	zassert_equal(vc_measurement_buffer_store(0, &in), VC_OK);
	zassert_equal(vc_measurement_buffer_read(0, &out), VC_OK);
	zassert_equal(out.generation, 3);
	zassert_equal(out.timestamp_ms, 500);
	zassert_equal(out.raw_voltage, 42);
}

ZTEST(voltage_control_provider_bus, test_measurement_buffer_indexed_access)
{
	struct vc_measurement_snapshot in0 = {
		.channel = 0, .generation = 1, .timestamp_ms = 100,
	};
	struct vc_measurement_snapshot in1 = {
		.channel = 1, .generation = 2, .timestamp_ms = 200,
	};
	struct vc_measurement_snapshot out;

	vc_measurement_buffer_init();
	zassert_equal(vc_measurement_buffer_store(0, &in0), VC_OK);
	zassert_equal(vc_measurement_buffer_store(1, &in1), VC_OK);

	zassert_equal(vc_measurement_buffer_read(0, &out), VC_OK);
	zassert_equal(out.generation, 1);

	zassert_equal(vc_measurement_buffer_read(1, &out), VC_OK);
	zassert_equal(out.generation, 2);
}
