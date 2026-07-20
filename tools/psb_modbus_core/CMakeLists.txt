add_library(psb_modbus_core STATIC
    config_manager.cpp
    psb_modbus_client.cpp
    psb_serial_bus.cpp
    monitor_render.cpp
)
target_include_directories(psb_modbus_core
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PUBLIC ${MODBUSLIB_INCLUDE_DIR}
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)
target_link_libraries(psb_modbus_core PUBLIC modbus)
target_link_libraries(psb_modbus_core PUBLIC tomlplusplus::tomlplusplus)
target_compile_features(psb_modbus_core PUBLIC cxx_std_17)
target_compile_options(psb_modbus_core PRIVATE -Wall -Wextra)
target_compile_options(psb_modbus_core PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wno-missing-field-initializers>)

add_subdirectory(tests)
