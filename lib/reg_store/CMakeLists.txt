# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources_ifdef(CONFIG_REG_STORE reg_store.c)
zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../include)
