# SPDX-License-Identifier: Apache-2.0

add_subdirectory_ifdef(CONFIG_AD5541 dac/ad5541)
add_subdirectory_ifdef(CONFIG_ADS1232 sensor/ads1232)
if(CONFIG_HVB_VC_CHANNEL OR CONFIG_LVB_VC_CHANNEL)
  add_subdirectory(voltage_control)
endif()
