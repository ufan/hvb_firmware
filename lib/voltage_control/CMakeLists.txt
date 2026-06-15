# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources(
	domain.c
	hvb_variant.c
)

zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../include)
