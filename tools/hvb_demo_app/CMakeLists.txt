add_subdirectory(cli)
add_subdirectory(tui)

if(BUILD_GUI)
    add_subdirectory(gui)
endif()
