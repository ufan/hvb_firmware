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
endif()

add_subdirectory(hvb_modbus_core)
add_subdirectory(hvb_demo_app)

if(BUILD_FACTORY)
    add_subdirectory(hvb_factory_tool)
endif()
