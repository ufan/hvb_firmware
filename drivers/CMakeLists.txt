# SPDX-License-Identifier: Apache-2.0

add_subdirectory_ifdef(CONFIG_AD5541 dac/ad5541)
add_subdirectory_ifdef(CONFIG_ADS1232 sensor/ads1232)
add_subdirectory_ifdef(CONFIG_HVB_VC_CHANNEL voltage_control)
