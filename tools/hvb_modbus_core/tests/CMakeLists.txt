add_executable(hvb_tests
    test_connection.cpp
    test_system_reads.cpp
    test_channel_reads.cpp
    test_writes.cpp
    test_enum_validation.cpp
    test_error_handling.cpp
    test_monitor_output.cpp
    test_tui_format.cpp
    test_tui_policy.cpp
    test_calibration.cpp
)

target_include_directories(hvb_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${MODBUSLIB_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
)

target_link_libraries(hvb_tests PRIVATE
    hvb_modbus_core
    Catch2::Catch2WithMain
)

target_compile_features(hvb_tests PRIVATE cxx_std_17)
target_compile_options(hvb_tests PRIVATE -Wall -Wextra)
