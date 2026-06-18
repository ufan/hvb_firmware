set(CMAKE_AUTOMOC ON)

qt_add_executable(hvb_factory_gui
    main.cpp
    CalibrationBackend.cpp CalibrationBackend.h
)

qt_add_qml_module(hvb_factory_gui
    URI HvbFactory
    NO_PLUGIN
    QML_FILES
        qml/MainWindow.qml
    SOURCES
        main.cpp
        CalibrationBackend.cpp CalibrationBackend.h
)

target_include_directories(hvb_factory_gui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../hvb_modbus_core
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
    ${MODBUSLIB_INCLUDE_DIR}
)

target_link_libraries(hvb_factory_gui PRIVATE
    hvb_modbus_core
    Qt6::Core
    Qt6::Quick
    Qt6::QuickControls2
)

target_compile_features(hvb_factory_gui PRIVATE cxx_std_17)
target_compile_options(hvb_factory_gui PRIVATE -Wall -Wextra)

set_target_properties(hvb_factory_gui PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
)
