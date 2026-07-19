# SPDX-License-Identifier: Apache-2.0
#
# Embeds a host tool's own version into its binary, derived from that
# tool's own <tool-name>-vX.Y.Z git tag (see docs/guide/
# version-management-guide.md) — the same resolve_version() pattern
# already used by deploy_linux.sh/deploy_windows.sh at packaging time,
# now also available at compile time so a running tool can report its
# own version, not just the firmware's.
#
# Usage: tool_version(psb_demo_tui) — call after add_executable()/
# qt_add_executable(). Adds an include dir exposing tool_version.h with
# TOOL_VERSION_STRING, e.g. "v1.0.0" (falls back to an abbreviated commit
# hash, or "dev" if git is unavailable — never fails the build).

function(tool_version TARGET_NAME)
	set(_tv_version "dev")

	find_package(Git QUIET)
	if(GIT_FOUND)
		execute_process(
			COMMAND ${GIT_EXECUTABLE} describe --tags --match "${TARGET_NAME}-v*" --always --dirty
			WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
			OUTPUT_VARIABLE _tv_describe_out
			OUTPUT_STRIP_TRAILING_WHITESPACE
			ERROR_QUIET
			RESULT_VARIABLE _tv_describe_result
		)
		if(_tv_describe_result EQUAL 0 AND _tv_describe_out)
			# git describe's output is "<tag-name>-<commits>-g<hash>[-dirty]",
			# i.e. it repeats the full tag ("psb_demo_tui-v1.0.0-5-g709b081")
			# — strip the "<target-name>-" prefix since the UI showing this
			# string already labels which tool it belongs to.
			string(REGEX REPLACE "^${TARGET_NAME}-" "" _tv_version "${_tv_describe_out}")
		endif()
	endif()

	set(_tv_gen_dir ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}_version_generated)
	file(MAKE_DIRECTORY ${_tv_gen_dir})
	configure_file(
		${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tool_version.h.in
		${_tv_gen_dir}/tool_version.h
		@ONLY
	)
	target_include_directories(${TARGET_NAME} PRIVATE ${_tv_gen_dir})
endfunction()
