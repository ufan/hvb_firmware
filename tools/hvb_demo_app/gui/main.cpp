#include "modbus_backend.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QIcon>

int main(int argc, char* argv[])
{
    QQuickStyle::setStyle("Material");
    qputenv("QT_QUICK_CONTROLS_MATERIAL_THEME", "Dark");
    qputenv("QT_QUICK_CONTROLS_MATERIAL_ACCENT", "Cyan");

    QGuiApplication app(argc, argv);
    app.setApplicationName("HVB Modbus Tool");
    app.setOrganizationName("jianwei");

    QQmlApplicationEngine engine;

    ModbusBackend backend;
    engine.rootContext()->setContextProperty("backend", &backend);

    const QUrl url("qrc:/HvbTool/qml/main.qml");
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed,
        &app, []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);
    engine.load(url);

    return app.exec();
}
