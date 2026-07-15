cmake_minimum_required(VERSION 3.20)
project(psb_tools)
add_subdirectory(cli)
add_subdirectory(tui)

if(BUILD_GUI)
    add_subdirectory(gui)
endif()
