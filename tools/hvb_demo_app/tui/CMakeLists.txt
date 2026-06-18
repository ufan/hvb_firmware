add_executable(hvb_demo_tui main.cpp)
target_include_directories(hvb_demo_tui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../hvb_modbus_core
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
    ${MODBUSLIB_INCLUDE_DIR}
)
target_link_libraries(hvb_demo_tui PRIVATE hvb_modbus_core ftxui::screen ftxui::dom ftxui::component)
target_compile_features(hvb_demo_tui PRIVATE cxx_std_17)
target_compile_options(hvb_demo_tui PRIVATE -Wall -Wextra)

set_target_properties(hvb_demo_tui PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
)
