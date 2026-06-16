set(CMAKE_AUTOMOC ON)

qt_add_executable(hvb_gui
    main.cpp
    modbus_backend.cpp modbus_backend.h
    modbus_worker.cpp modbus_worker.h
)

qt_add_qml_module(hvb_gui
    URI HvbTool
    NO_PLUGIN
    QML_FILES
        qml/Theme.qml
        qml/main.qml
        qml/ConnectionBar.qml
        qml/SystemInfoTab.qml
        qml/SystemConfigTab.qml
        qml/ChannelTab.qml
        qml/RawDebugDialog.qml
        qml/components/ReadOnlyField.qml
        qml/components/EditableField.qml
        qml/components/EnumCombo.qml
        qml/components/StatusBadge.qml
    SOURCES
        main.cpp
        modbus_backend.cpp modbus_backend.h
        modbus_worker.cpp modbus_worker.h
)

target_link_libraries(hvb_gui PRIVATE
    hvb_modbus_core
    Qt6::Core
    Qt6::Quick
    Qt6::QuickControls2
)

target_compile_features(hvb_gui PRIVATE cxx_std_17)
target_compile_options(hvb_gui PRIVATE -Wall -Wextra)

set_target_properties(hvb_gui PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
)
