#include "modbus_backend.h"

// QApplication (not QGuiApplication) is required here: the QML ChartView
// (Qt Charts) used by ChannelGraph.qml is built on the Graphics View
// Framework and constructs widget-based items (QGraphicsTextItem ->
// QWidgetTextControl) for its title/legend. Without a QApplication
// instance those constructors dereference uninitialized widget-subsystem
// state and segfault the instant a chart is created (i.e. right after a
// successful connect, when the channel-tab Repeater builds its charts).
#include <QApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QIcon>

int main(int argc, char* argv[])
{
    QQuickStyle::setStyle("Material");
    qputenv("QT_QUICK_CONTROLS_MATERIAL_THEME", "Dark");
    qputenv("QT_QUICK_CONTROLS_MATERIAL_ACCENT", "Cyan");

    QApplication app(argc, argv);
    app.setApplicationName("HVB Modbus Tool");
    app.setOrganizationName("jianwei");

    QQmlApplicationEngine engine;

    ModbusBackend backend;
    engine.rootContext()->setContextProperty("backend", &backend);

    const QUrl url("qrc:/PsbTool/qml/main.qml");
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed,
        &app, []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);
    engine.load(url);

    return app.exec();
}
