# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources(
	domain_state.c
	vc_channel_state.c
	vc_controller.c
)
zephyr_library_sources_ifdef(CONFIG_VC_CHANNEL_CONTROLLER controller.c)
zephyr_library_sources_ifdef(CONFIG_VC_CHANNEL_CONTROLLER vc_channel_table.c)
zephyr_library_sources_ifdef(CONFIG_VC_RUNTIME domain_runtime.c)
zephyr_library_sources_ifdef(CONFIG_VC_RUNTIME vc.c)
zephyr_library_sources_ifdef(CONFIG_VC_RUNTIME vc_register.c)
zephyr_library_sources_ifdef(CONFIG_VC_SETTINGS_PERSISTENCE vc_storage_settings.c)

zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../include)
