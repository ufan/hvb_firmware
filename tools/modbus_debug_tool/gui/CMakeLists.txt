set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

add_executable(hvb_modbus_gui
    main.cpp
    modbus_backend.cpp
    modbus_worker.cpp
    resources/qml.qrc
)

target_link_libraries(hvb_modbus_gui PRIVATE
    hvb_modbus_core
    Qt6::Core
    Qt6::Quick
    Qt6::QuickControls2
)

target_compile_features(hvb_modbus_gui PRIVATE cxx_std_17)
target_compile_options(hvb_modbus_gui PRIVATE -Wall -Wextra)
