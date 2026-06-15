cmake_minimum_required(VERSION 3.20)
project(hvb_modbus_tool LANGUAGES CXX)

option(BUILD_GUI "Build Qt Quick GUI (requires Qt 6.x)" OFF)

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

# ModbusLib doesn't export target_include_directories; add manually
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

add_subdirectory(core)
add_subdirectory(cli)

if(BUILD_GUI)
    find_package(Qt6 REQUIRED COMPONENTS Core SerialPort Quick QuickControls2)
    add_subdirectory(gui)
endif()
