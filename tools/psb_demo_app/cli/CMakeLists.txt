add_executable(psb_demo_cli main.cpp)
target_link_libraries(psb_demo_cli PRIVATE psb_modbus_core CLI11::CLI11)
target_compile_features(psb_demo_cli PRIVATE cxx_std_17)
target_compile_options(psb_demo_cli PRIVATE -Wall -Wextra)

if(WIN32)
    target_link_options(psb_demo_cli PRIVATE -static-libgcc -static-libstdc++ -static)
endif()

set_target_properties(psb_demo_cli PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
)
