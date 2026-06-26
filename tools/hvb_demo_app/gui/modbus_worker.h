#pragma once

#include "hvb_modbus_client.h"
#include "types.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>
#include <QVector>

class ModbusWorker : public QObject
{
    Q_OBJECT

public:
    explicit ModbusWorker(QObject* parent = nullptr);
    ~ModbusWorker() override;

public slots:
    void doConnect(const QString& port, int baud, int slaveId, int timeoutMs);
    void doDisconnect();
    void doRefreshSystemInfo();
    void doRefreshChannelInfo(int ch);
    void doReadSystemConfig();
    void doReadChannelConfig(int ch);
    void doSendOutputAction(int ch, int action);
    void doSendFaultCmd(int ch, int cmd);
    void doWriteTargetVoltage(int ch, int raw);
    void doWriteOperatingMode(int mode);
    void doWriteStartupChannelPolicy(int policy);
    void doWriteSlaveAddress(int addr);
    void doWriteBaudRate(int code);
    void doWriteRampUp(int ch, int stepRaw, int interval);
    void doWriteRampDown(int ch, int stepRaw, int interval);
    void doWriteChannelRecovery(int ch, int policy, int delay, int max, int window);
    void doWriteChannelSafeBand(int ch, int pct);
    void doWriteCurrentProtection(int ch, int mode, int action, int thresholdRaw);
    void doWriteDerateStep(int ch, int stepRaw);
    void doWriteCalOutput(int ch, int k, int b);
    void doWriteCalMeasV(int ch, int k, int b);
    void doWriteCalMeasI(int ch, int k, int b);
    void doSendParamAction(int chScope, int action);
    void doScanPorts();
    void doRawReadFc04(int addr, int count);
    void doRawReadFc03(int addr, int count);
    void doRawWriteFc06(int addr, int value);

signals:
    void connected(bool ok, const QString& error);
    void disconnected();
    void systemInfoReady(const QVariantMap& info);
    void channelInfoReady(int ch, const QVariantMap& info);
    void systemConfigReady(const QVariantMap& cfg);
    void channelConfigReady(int ch, const QVariantMap& cfg);
    void operationComplete(bool ok, const QString& msg);
    void portsScanned(const QStringList& ports);
    void rawHexResult(const QString& hex);
    void rawFrameLog(const QString& msg);

private:
    hvb::HvbModbusClient m_client;

    QVariantMap systemInfoToMap(const hvb::SystemInfo& info);
    QVariantMap channelInfoToMap(int ch, const hvb::ChannelInfo& info);
    QVariantMap systemConfigToMap(const hvb::SystemConfig& cfg);
    QVariantMap channelConfigToMap(int ch, const hvb::ChannelConfig& cfg);

    void onFrame(bool tx, const std::vector<uint8_t>& data);
};
