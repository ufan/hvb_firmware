# SPDX-License-Identifier: Apache-2.0

zephyr_library_named(sys_status)
zephyr_library_sources_ifdef(CONFIG_SYS_STATUS sys_status.c)
