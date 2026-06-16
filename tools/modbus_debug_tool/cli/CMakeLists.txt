add_executable(hvbctrl main.cpp)
target_link_libraries(hvbctrl PRIVATE hvb_modbus_core CLI11::CLI11)
target_compile_features(hvbctrl PRIVATE cxx_std_17)
target_compile_options(hvbctrl PRIVATE -Wall -Wextra)

set_target_properties(hvbctrl PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
)
