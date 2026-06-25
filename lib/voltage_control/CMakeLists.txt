# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources(
	vc_channel_state.c
	vc_controller.c
)
zephyr_library_sources_ifdef(CONFIG_VC_RUNTIME vc_runtime.c)
zephyr_library_sources_ifdef(CONFIG_VC_RUNTIME vc.c)
zephyr_library_sources_ifdef(CONFIG_VC_RUNTIME vc_register.c)
zephyr_library_sources_ifdef(CONFIG_VC_SETTINGS_PERSISTENCE vc_storage_settings.c)

zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../include)
zephyr_linker_sources(DATA_SECTIONS ${CMAKE_CURRENT_SOURCE_DIR}/vc_meas_buffer.ld)
