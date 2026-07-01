set(CMAKE_AUTOMOC ON)

set(FACTORY_GUI_SOURCES
    main.cpp
    CalibrationBackend.cpp CalibrationBackend.h
    SweepData.cpp          SweepData.h
    CalibrationWorker.cpp  CalibrationWorker.h
    TestEngine.cpp         TestEngine.h
    TestResult.h
    ReportEngine.cpp       ReportEngine.h
    ReportData.h
)

set(FACTORY_GUI_QML
    qml/MainWindow.qml
    qml/WorkModeSidebar.qml
    qml/ReportPage.qml
    qml/cal/CalibrationWorkspace.qml
    qml/cal/CalConnectPage.qml
    qml/cal/CalUnlockPage.qml
    qml/cal/CalChannelPage.qml
    qml/cal/CalCommitPage.qml
    qml/test/TestingWorkspace.qml
    qml/test/FunctionalTestPage.qml
    qml/test/StressTestPage.qml
    qml/components/DeviceInfoCard.qml
    qml/components/StepIndicator.qml
    qml/components/SweepTable.qml
    qml/components/FitResultRow.qml
    qml/components/LiveChart.qml
    qml/components/ResultsTable.qml
)

qt_add_executable(hvb_factory_gui ${FACTORY_GUI_SOURCES})

qt_add_qml_module(hvb_factory_gui
    URI HvbFactory
    NO_PLUGIN
    QML_FILES ${FACTORY_GUI_QML}
    SOURCES ${FACTORY_GUI_SOURCES}
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
    Qt6::Charts
    Qt6::PrintSupport
)

target_compile_features(hvb_factory_gui PRIVATE cxx_std_17)
target_compile_options(hvb_factory_gui PRIVATE -Wall -Wextra)

set_target_properties(hvb_factory_gui PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
)
