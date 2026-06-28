# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources(reg_catalog.c)
zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../include)
zephyr_linker_sources(SECTIONS ${CMAKE_CURRENT_SOURCE_DIR}/reg_catalog.ld)
