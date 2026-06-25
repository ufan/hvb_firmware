# SPDX-License-Identifier: Apache-2.0

zephyr_library_named(shell_adapter)
zephyr_library_sources_ifdef(CONFIG_VC_SHELL shell_adapter.c)
