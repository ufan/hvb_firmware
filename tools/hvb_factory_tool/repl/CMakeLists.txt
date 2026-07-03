add_executable(hvb_factory_tui
    main.cpp
    FactorySession.cpp
    FactoryCommands.cpp
)
target_include_directories(hvb_factory_tui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../hvb_modbus_core
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
    ${MODBUSLIB_INCLUDE_DIR}
)
target_link_libraries(hvb_factory_tui PRIVATE
    hvb_modbus_core
    cli::cli
)
target_compile_features(hvb_factory_tui PRIVATE cxx_std_17)
target_compile_options(hvb_factory_tui PRIVATE -Wall -Wextra)

if(WIN32)
    target_link_options(hvb_factory_tui PRIVATE -static-libgcc -static-libstdc++ -static)
endif()

set_target_properties(hvb_factory_tui PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
)
