set(CMAKE_AUTOMOC ON)

qt_add_executable(psb_demo_gui
    main.cpp
    modbus_backend.cpp modbus_backend.h
    modbus_worker.cpp modbus_worker.h
)

set_source_files_properties(qml/PsbTheme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(psb_demo_gui
    URI PsbTool
    NO_PLUGIN
    QML_FILES
        qml/PsbTheme.qml
        qml/main.qml
        qml/ConnectionModal.qml
        qml/SysConfigDialog.qml
        qml/MonitorTab.qml
        qml/ChannelTab.qml
        qml/RawDebugDialog.qml
        qml/components/BreathingIndicator.qml
        qml/components/LabeledValue.qml
        qml/components/ChannelGraph.qml
        qml/components/ReadOnlyField.qml
        qml/components/EditableField.qml
        qml/components/EnumCombo.qml
        qml/components/StatusBadge.qml
    SOURCES
        main.cpp
        modbus_backend.cpp modbus_backend.h
        modbus_worker.cpp modbus_worker.h
)

target_link_libraries(psb_demo_gui PRIVATE
    psb_modbus_core
    Qt6::Core
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Charts
)

target_compile_features(psb_demo_gui PRIVATE cxx_std_17)
target_compile_options(psb_demo_gui PRIVATE -Wall -Wextra)

set_target_properties(psb_demo_gui PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
)
