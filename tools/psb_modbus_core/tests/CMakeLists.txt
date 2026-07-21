add_executable(psb_tests
    test_connection.cpp
    test_serial_bus.cpp
    test_board_session.cpp
    test_topology_config.cpp
    test_app_preferences.cpp
    test_system_reads.cpp
    test_channel_reads.cpp
    test_writes.cpp
    test_enum_validation.cpp
    test_error_handling.cpp
    test_monitor_output.cpp
    test_tui_format.cpp
    test_tui_policy.cpp
    test_tui_modbus_settings.cpp
    test_calibration.cpp
    test_wizard_state.cpp
    test_wizard_scan.cpp
)

target_include_directories(psb_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${MODBUSLIB_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
)

target_link_libraries(psb_tests PRIVATE
    psb_modbus_core
    Catch2::Catch2WithMain
)

target_compile_features(psb_tests PRIVATE cxx_std_17)
target_compile_options(psb_tests PRIVATE -Wall -Wextra)
