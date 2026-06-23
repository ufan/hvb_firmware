add_library(hvb_modbus_core STATIC
    config_manager.cpp
    hvb_modbus_client.cpp
    monitor_render.cpp
    register_meta.cpp
)
target_include_directories(hvb_modbus_core
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PUBLIC ${MODBUSLIB_INCLUDE_DIR}
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)
target_link_libraries(hvb_modbus_core PUBLIC modbus)
target_link_libraries(hvb_modbus_core PUBLIC tomlplusplus::tomlplusplus)
target_compile_features(hvb_modbus_core PUBLIC cxx_std_17)
target_compile_options(hvb_modbus_core PRIVATE -Wall -Wextra)
target_compile_options(hvb_modbus_core PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wno-missing-field-initializers>)

add_subdirectory(tests)
