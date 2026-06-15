#include "modbus_backend.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>

int main(int argc, char* argv[])
{
    QGuiApplication app(argc, argv);
    app.setApplicationName("HVB Modbus Tool");
    app.setOrganizationName("jianwei");

    qmlRegisterType<ModbusBackend>("HvbTool", 1, 0, "ModbusBackend");

    QQmlApplicationEngine engine;

    ModbusBackend backend;
    engine.rootContext()->setContextProperty("backend", &backend);

    engine.load(QUrl("qrc:/qml/main.qml"));
    if (engine.rootObjects().isEmpty())
        return -1;

    return app.exec();
}
