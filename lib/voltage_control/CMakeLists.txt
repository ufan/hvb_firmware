# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources(
	domain.c
	modbus_adapter.c
)
zephyr_library_sources_ifdef(CONFIG_VC_CHANNEL_CONTROLLER controller.c)
zephyr_library_sources_ifdef(CONFIG_VC_RUNTIME runtime.c)

zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../include)
