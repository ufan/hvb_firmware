# SPDX-License-Identifier: Apache-2.0
#
# Generates include/generated/fw_version.h with FW_VERSION_MAJOR/MINOR/PATCH
# derived from the firmware-vX.Y.Z git tag. See docs/superpowers/specs/
# 2026-07-19-version-management-contract-design.md sections 2 and 7.
# Falls back to 0.0.0 when no matching tag is reachable (fresh checkout,
# shallow clone, or git unavailable) so the build never fails on this step.

set(FW_VERSION_MAJOR 0)
set(FW_VERSION_MINOR 0)
set(FW_VERSION_PATCH 0)

find_package(Git QUIET)
if(GIT_FOUND)
	execute_process(
		COMMAND ${GIT_EXECUTABLE} describe --tags --match "firmware-v*" --always
		WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
		OUTPUT_VARIABLE FW_GIT_DESCRIBE
		OUTPUT_STRIP_TRAILING_WHITESPACE
		ERROR_QUIET
		RESULT_VARIABLE FW_GIT_DESCRIBE_RESULT
	)
	if(FW_GIT_DESCRIBE_RESULT EQUAL 0 AND
	   FW_GIT_DESCRIBE MATCHES "^firmware-v([0-9]+)\\.([0-9]+)\\.([0-9]+)")
		set(FW_VERSION_MAJOR ${CMAKE_MATCH_1})
		set(FW_VERSION_MINOR ${CMAKE_MATCH_2})
		set(FW_VERSION_PATCH ${CMAKE_MATCH_3})
	endif()
endif()

set(FW_VERSION_GENERATED_DIR ${CMAKE_BINARY_DIR}/include/generated)
file(MAKE_DIRECTORY ${FW_VERSION_GENERATED_DIR})
configure_file(
	${CMAKE_CURRENT_LIST_DIR}/fw_version.h.in
	${FW_VERSION_GENERATED_DIR}/fw_version.h
	@ONLY
)
zephyr_include_directories(${FW_VERSION_GENERATED_DIR})
