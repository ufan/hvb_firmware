#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QTimer>
#include <QThread>
#include <QMap>

class ModbusWorker;

class ModbusBackend : public QObject
{
    Q_OBJECT

    // Connection state
    Q_PROPERTY(bool connected READ isConnected NOTIFY connectedChanged)
    Q_PROPERTY(QStringList ports READ ports NOTIFY portsChanged)
    Q_PROPERTY(QString selectedPort READ selectedPort WRITE setSelectedPort NOTIFY selectedPortChanged)
    Q_PROPERTY(int baudRate READ baudRate WRITE setBaudRate NOTIFY baudRateChanged)
    Q_PROPERTY(int slaveId READ slaveId WRITE setSlaveId NOTIFY slaveIdChanged)
    Q_PROPERTY(int pollIntervalMs READ pollIntervalMs WRITE setPollIntervalMs NOTIFY pollIntervalChanged)

    // System Info (FC04)
    Q_PROPERTY(QVariantMap sysInfo READ sysInfo NOTIFY sysInfoChanged)

    // Dynamic channel data — length = channelCount after connection.
    Q_PROPERTY(int channelCount READ channelCount NOTIFY channelDataChanged)
    Q_PROPERTY(QVariantList channelInfoList READ channelInfoList NOTIFY channelDataChanged)
    Q_PROPERTY(QVariantList channelConfigList READ channelConfigList NOTIFY channelDataChanged)

    // System Config
    Q_PROPERTY(QVariantMap sysConfig READ sysConfig NOTIFY sysConfigChanged)

    // Raw log
    Q_PROPERTY(QString rawLog READ rawLog NOTIFY rawLogChanged)

    // Status message
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusMessageChanged)

public:
    static constexpr int MAX_CHANNELS = 16;

    explicit ModbusBackend(QObject* parent = nullptr);
    ~ModbusBackend() override;

    bool isConnected() const { return m_connected; }
    QStringList ports() const { return m_ports; }
    QString selectedPort() const { return m_selectedPort; }
    int baudRate() const { return m_baud; }
    int slaveId() const { return m_slaveId; }
    int pollIntervalMs() const { return m_pollInterval; }

    QVariantMap sysInfo() const { return m_sysInfo; }

    int channelCount() const;
    QVariantList channelInfoList() const;
    QVariantList channelConfigList() const;

    QVariantMap sysConfig() const { return m_sysConfig; }

    QString rawLog() const { return m_rawLogLines.join("\n"); }
    QString statusMessage() const { return m_statusMessage; }

    void setSelectedPort(const QString& p);
    void setBaudRate(int b);
    void setSlaveId(int id);
    void setPollIntervalMs(int ms);

public slots:
    void connectToDevice();
    void disconnectFromDevice();
    void scanPorts();
    void refreshAll();

    // System writes
    void writeOperatingMode(int mode);
    void writeStartupChannelPolicy(int policy);
    void writeSlaveAddress(int addr);
    void writeBaudRate(int code);
    void saveSystem();
    void loadSystem();
    void factoryResetSystem();
    void softwareReset();

    // Channel writes
    void sendOutputAction(int ch, int action);
    void sendFaultCmd(int ch, int cmd);
    void writeTargetVoltage(int ch, int raw);
    void writeRampUp(int ch, int stepRaw, int interval);
    void writeRampDown(int ch, int stepRaw, int interval);
    void writeChannelRecovery(int ch, int policy, int delay, int max, int window);
    void writeChannelSafeBand(int ch, int pct);
    void writeCurrentProtection(int ch, int mode, int action, int thresholdRaw);
    void writeDerateStep(int ch, int stepRaw);
    void writeCalOutput(int ch, int k, int b);
    void writeCalMeasV(int ch, int k, int b);
    void writeCalMeasI(int ch, int k, int b);
    void exitCalibrationMode();
    void saveChannel(int ch);
    void loadChannel(int ch);
    void factoryResetChannel(int ch);

    // Raw
    void rawReadFc04(int addr, int count);
    void rawReadFc03(int addr, int count);
    void rawWriteFc06(int addr, int value);

signals:
    void connectedChanged();
    void portsChanged();
    void selectedPortChanged();
    void baudRateChanged();
    void slaveIdChanged();
    void pollIntervalChanged();
    void sysInfoChanged();
    void channelDataChanged();   // fires when channelCount, channelInfoList, or channelConfigList change
    void sysConfigChanged();
    void rawLogChanged();
    void statusMessageChanged();
    void rawHexReady(const QString& hex);

private slots:
    void onConnected(bool ok, const QString& error);
    void onDisconnected();
    void onSysInfoReady(const QVariantMap& info);
    void onChInfoReady(int ch, const QVariantMap& info);
    void onSysConfigReady(const QVariantMap& cfg);
    void onChConfigReady(int ch, const QVariantMap& cfg);
    void onOperationComplete(bool ok, const QString& msg);
    void onPortsScanned(const QStringList& ports);
    void onRawFrameLog(const QString& msg);
    void onRawHexResult(const QString& hex);

private:
    void setStatus(const QString& msg);
    void pollTick();
    void refreshChannels();

    // Connection
    bool m_connected = false;
    QString m_selectedPort;
    int m_baud = 115200;
    int m_slaveId = 1;
    int m_pollInterval = 2000;
    QStringList m_ports;

    // Worker thread
    QThread* m_thread = nullptr;
    ModbusWorker* m_worker = nullptr;

    // Cached data — indexed by channel number
    QVariantMap m_sysInfo;
    QVariantMap m_chInfo[MAX_CHANNELS];
    QVariantMap m_sysConfig;
    QVariantMap m_chConfig[MAX_CHANNELS];

    // UI state
    QStringList m_rawLogLines;
    QString m_statusMessage;
    QTimer m_pollTimer;
};
