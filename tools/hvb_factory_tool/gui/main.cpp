#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include "CalibrationBackend.h"

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    QQuickStyle::setStyle("Material");

    // B-6 fix: use qmlRegisterSingletonInstance instead of deprecated setContextProperty
    hvb::factory::CalibrationBackend backend;
    qmlRegisterSingletonInstance<hvb::factory::CalibrationBackend>(
        "HvbFactory", 1, 0, "Backend", &backend);

    QQmlApplicationEngine engine;
    engine.loadFromModule("HvbFactory", "MainWindow");

    return app.exec();
}
