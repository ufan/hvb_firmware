# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources(
	domain_state.c
	modbus_adapter.c
)
zephyr_library_sources_ifdef(CONFIG_VC_CHANNEL_CONTROLLER controller.c)
zephyr_library_sources_ifdef(CONFIG_VC_RUNTIME domain_runtime.c)
zephyr_library_sources_ifdef(CONFIG_VC_PROVIDER_BUS provider_bus.c)
zephyr_linker_sources_ifdef(CONFIG_VC_PROVIDER_BUS RAM_SECTIONS provider_bus_sections.ld)

zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../include)
zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../dts/bindings/include)
