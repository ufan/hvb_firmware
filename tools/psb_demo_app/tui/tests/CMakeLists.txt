add_executable(psb_demo_tui_tests
    test_tui_format.cpp
    test_tui_policy.cpp
    test_tui_modbus_settings.cpp
    test_tui_widgets.cpp
    test_wizard_scan.cpp
)

target_include_directories(psb_demo_tui_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../psb_modbus_core
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../include
    ${MODBUSLIB_INCLUDE_DIR}
)

target_link_libraries(psb_demo_tui_tests PRIVATE
    psb_modbus_core
    ftxui::screen
    ftxui::dom
    ftxui::component
    Catch2::Catch2WithMain
)

target_compile_features(psb_demo_tui_tests PRIVATE cxx_std_17)
target_compile_options(psb_demo_tui_tests PRIVATE -Wall -Wextra)
