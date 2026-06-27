# SPDX-License-Identifier: Apache-2.0

zephyr_library_named(sys_status)
zephyr_library_sources_ifdef(CONFIG_SYS_RESET sys_reset.c)
zephyr_library_sources_ifdef(CONFIG_SYS_STATUS sys_status.c)
zephyr_library_sources_ifdef(CONFIG_SYS_STATUS_SHELL sys_status_shell.c)
