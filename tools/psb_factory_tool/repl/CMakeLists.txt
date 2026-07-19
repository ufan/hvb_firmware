add_executable(psb_factory_tui
    main.cpp
    FactorySession.cpp
    FactoryCommands.cpp
)
tool_version(psb_factory_tui)
target_include_directories(psb_factory_tui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../psb_modbus_core
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
    ${MODBUSLIB_INCLUDE_DIR}
)
target_link_libraries(psb_factory_tui PRIVATE
    psb_modbus_core
    cli::cli
)
target_compile_features(psb_factory_tui PRIVATE cxx_std_17)
target_compile_options(psb_factory_tui PRIVATE -Wall -Wextra)

if(WIN32)
    target_link_options(psb_factory_tui PRIVATE -static-libgcc -static-libstdc++ -static)
endif()

set_target_properties(psb_factory_tui PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
)
