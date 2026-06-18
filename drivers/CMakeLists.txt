# SPDX-License-Identifier: Apache-2.0

add_subdirectory_ifdef(CONFIG_AD5541 dac/ad5541)
add_subdirectory_ifdef(CONFIG_ADS1232 sensor/ads1232)
add_subdirectory_ifdef(CONFIG_VC_CHANNEL_PROVIDER voltage_control)
