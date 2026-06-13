# SPDX-License-Identifier: Apache-2.0

if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/include)
  zephyr_include_directories(include)
endif()

if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/drivers/CMakeLists.txt)
  add_subdirectory(drivers)
endif()

if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/lib/CMakeLists.txt)
  add_subdirectory(lib)
endif()
