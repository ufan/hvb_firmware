#pragma once

#include <QObject>
#include <QVariantMap>
#include <QString>
#include <QStringList>
#include "hvb_modbus_client.h"

namespace hvb::factory {

class CalibrationBackend : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)
    Q_PROPERTY(bool calUnlocked READ calUnlocked NOTIFY calStateChanged)
    Q_PROPERTY(bool calActive READ calActive NOTIFY calStateChanged)
    Q_PROPERTY(int activeChannel READ activeChannel WRITE setActiveChannel NOTIFY activeChannelChanged)
    Q_PROPERTY(QStringList ports READ ports NOTIFY portsChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusMessageChanged)

public:
    explicit CalibrationBackend(QObject* parent = nullptr);
    ~CalibrationBackend() override;

    bool connected() const;
    bool calUnlocked() const;
    bool calActive() const;
    int activeChannel() const;
    void setActiveChannel(int ch);
    QStringList ports() const;
    QString statusMessage() const;

public slots:
    void connectToDevice(const QString& port, int baud, int slaveId);
    void disconnectFromDevice();
    void scanPorts();
    void unlockStep1();
    void unlockStep2();
    void enterCalibrationMode();
    void exitCalibrationMode(const QString& targetMode);
    void enableOutput(bool enable);
    void writeRawDac(int code);
    void triggerSample();
    void commitChannel();
    void safeAll();
    void writeCoefficients(const QString& type, double k, double b);
    void refreshSnapshot();

signals:
    void connectedChanged();
    void calStateChanged();
    void activeChannelChanged();
    void portsChanged();
    void statusMessageChanged();
    void snapshotUpdated(QVariantMap snapshot);

private:
    HvbModbusClient m_client;
    bool m_calUnlocked = false;
    bool m_calActive = false;
    int m_activeChannel = 0;
    QStringList m_ports;
    QString m_statusMessage;
};

} // namespace hvb::factory
