cmake_minimum_required(VERSION 3.20)
project(hvb_tools LANGUAGES CXX)

option(BUILD_GUI "Build Qt Quick GUIs (requires Qt 6.x)" OFF)
option(BUILD_FACTORY "Build factory calibration tool" ON)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries (default OFF → static ModbusLib)")

include(FetchContent)

# ModbusLib — cross-platform C++ Modbus library (RTU client)
FetchContent_Declare(modbuslib
    GIT_REPOSITORY https://github.com/serhmarch/ModbusLib.git
    GIT_TAG v0.4.8
)
set(MB_QT_ENABLED OFF CACHE BOOL "" FORCE)
set(MB_EXAMPLES_ENABLED OFF CACHE BOOL "" FORCE)
set(MB_TESTS_ENABLED OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(modbuslib)

# Workaround: ModbusLib uses Win32 headers with mixed case (e.g. <Windows.h>,
# <WS2tcpip.h>) but MinGW headers on case-sensitive Linux filesystems are
# lowercase (<windows.h>, <ws2tcpip.h>).
if(MINGW)
    set(MB_CASE_FIXES
        "Windows.h|windows.h"
        "WS2tcpip.h|ws2tcpip.h"
    )
    foreach(MBFILE IN ITEMS
        "${modbuslib_SOURCE_DIR}/src/win/ModbusSerialPort_p_win.h"
        "${modbuslib_SOURCE_DIR}/src/win/Modbus_win.cpp"
        "${modbuslib_SOURCE_DIR}/src/win/ModbusTCP_win.h"
    )
        file(READ "${MBFILE}" MBFBUF)
        foreach(MBPAIR IN LISTS MB_CASE_FIXES)
            string(REPLACE "|" ";" MBPAIR "${MBPAIR}")
            list(GET MBPAIR 0 MBFROM)
            list(GET MBPAIR 1 MBTO)
            string(REPLACE "<${MBFROM}>" "<${MBTO}>" MBFBUF "${MBFBUF}")
        endforeach()
        file(WRITE "${MBFILE}" "${MBFBUF}")
    endforeach()

    # MinGW on case-sensitive Linux filesystems has lowercase library names
    # (libws2_32.a) but CMake's Windows platform module generates mixed-case
    # -l flags (-lWs2_32). Create case-mapped symlinks.
    set(MINGW_CASE_DIR "${CMAKE_BINARY_DIR}/mingw-case-libs")
    file(MAKE_DIRECTORY "${MINGW_CASE_DIR}")
    foreach(LIB Ws2_32 Winmm Setupapi)
        string(TOLOWER "${LIB}" LOWER_NAME)
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E create_symlink
            "/usr/x86_64-w64-mingw32/lib/lib${LOWER_NAME}.a"
            "${MINGW_CASE_DIR}/lib${LIB}.a"
        )
    endforeach()
    link_directories("${MINGW_CASE_DIR}")
endif()

set(MODBUSLIB_INCLUDE_DIR ${modbuslib_SOURCE_DIR}/src)

# CLI11 — header-only argument parser
FetchContent_Declare(cli11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG v2.4.2
)
FetchContent_MakeAvailable(cli11)

# toml++ — header-only TOML parser
FetchContent_Declare(tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG v3.4.0
)
FetchContent_MakeAvailable(tomlplusplus)

# ftxui — C++ TUI library
FetchContent_Declare(ftxui
    GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
    GIT_TAG v5.0.0
)
set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(ftxui)

# Catch2 — test framework
FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.7.1
)
FetchContent_MakeAvailable(Catch2)

if(BUILD_GUI)
    find_package(Qt6 REQUIRED COMPONENTS Core SerialPort Quick QuickControls2 Charts PrintSupport)
endif()

if(BUILD_FACTORY)
    # daniele77/cli — header-only Cisco-style REPL
    FetchContent_Declare(daniele77cli
        GIT_REPOSITORY https://github.com/daniele77/cli.git
        GIT_TAG v2.2.0
    )
    FetchContent_MakeAvailable(daniele77cli)

    # Workaround: cli/detail/rang.h includes <VersionHelpers.h> (Win32's
    # mixed-case convention) but MinGW headers on case-sensitive Linux
    # filesystems are lowercase (<versionhelpers.h>) — same class of issue
    # as the ModbusLib case-fixes above.
    if(MINGW)
        set(DANIELE77CLI_CASE_DIR "${CMAKE_BINARY_DIR}/mingw-case-includes")
        file(MAKE_DIRECTORY "${DANIELE77CLI_CASE_DIR}")
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E create_symlink
            "/usr/x86_64-w64-mingw32/include/versionhelpers.h"
            "${DANIELE77CLI_CASE_DIR}/VersionHelpers.h"
        )
        include_directories(BEFORE SYSTEM "${DANIELE77CLI_CASE_DIR}")
    endif()
endif()

add_subdirectory(hvb_modbus_core)
add_subdirectory(hvb_demo_app)

if(BUILD_FACTORY)
    add_subdirectory(hvb_factory_tool)
endif()
