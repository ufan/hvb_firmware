# SPDX-License-Identifier: Apache-2.0

zephyr_library_named(modbus_adapter)
zephyr_library_sources(modbus_adapter.c)
zephyr_library_sources(modbus_register.c)
zephyr_library_sources_ifdef(CONFIG_MODBUS_ADAPTER_SHELL modbus_adapter_shell.c)
