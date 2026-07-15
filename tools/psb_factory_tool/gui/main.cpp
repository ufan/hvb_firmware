// QApplication (not QGuiApplication) is required here: the QML ChartView
// (Qt Charts, used by components/LiveChart.qml) is built on the Graphics
// View Framework and constructs widget-based items (QGraphicsTextItem ->
// QWidgetTextControl) for its title/legend, which segfaults without a
// QApplication instance the instant a chart is created.
#include <QApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include "CalibrationBackend.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QQuickStyle::setStyle("Material");

    // B-6 fix: use qmlRegisterSingletonInstance instead of deprecated setContextProperty
    psb::factory::CalibrationBackend backend;
    qmlRegisterSingletonInstance<psb::factory::CalibrationBackend>(
        "PsbFactory", 1, 0, "Backend", &backend);

    QQmlApplicationEngine engine;
    engine.loadFromModule("PsbFactory", "MainWindow");

    return app.exec();
}
