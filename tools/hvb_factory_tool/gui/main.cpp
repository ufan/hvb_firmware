#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include "CalibrationBackend.h"

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    QQuickStyle::setStyle("Material");

    hvb::factory::CalibrationBackend backend;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.loadFromModule("HvbFactory", "MainWindow");

    return app.exec();
}
