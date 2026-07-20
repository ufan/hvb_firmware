#pragma once

#include "psb_modbus_client.h"
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
    void doWriteOutputEnabled(int ch, bool enabled);
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
    void doExitCalibrationMode();
    void doSendParamAction(int chScope, int action);
    void doScanPorts();
    void doRawReadFc04(int addr, int count);
    void doRawReadFc03(int addr, int count);
    void doRawWriteFc06(int addr, int value);
    void doPollStatus();   // realtime registers only — called by poll timer

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
    psb::PsbModbusClient m_client;

    // Cached structs for realtime poll — populated by doRefreshSystemInfo/doRefreshChannelInfo
    static constexpr int WORKER_MAX_CH = 16;
    psb::SystemInfo   m_cachedSysInfo{};
    psb::ChannelInfo  m_cachedChInfo[WORKER_MAX_CH]{};
    // Cached alongside m_cachedChInfo so post-write narrow refreshes (see
    // refreshChannelXxx below) can use the client's merge-on-success,
    // reference-taking block reads instead of a wholesale re-read.
    psb::ChannelConfig m_cachedChConfig[WORKER_MAX_CH]{};
    int m_channelCount = 0;

    // Offline-channel detection (mirrors demo_tui's tab_monitor.h/main.cpp):
    // a channel that fails kChannelOfflineThreshold polls in a row is shown
    // as explicitly offline rather than silently keeping stale cached values
    // indistinguishable from a healthy, freshly-polled channel. One failure
    // is expected noise on real hardware; a run of them is a real signal.
    int m_chFailCount[WORKER_MAX_CH]{};
    bool m_chOffline[WORKER_MAX_CH]{};
    static constexpr int kChannelOfflineThreshold = 5;

    // Poll timeout override (ms) for doPollStatus()'s routine reads — short,
    // matching demo_tui's kPollTimeoutMs, so a single unresponsive poll
    // transaction fails fast and retries next cycle instead of blocking for
    // the connection's full timeout (set from the Connect dialog, typically
    // 3000ms) on every poll tick.
    static constexpr int kPollTimeoutMs = 300;

    QVariantMap systemInfoToMap(const psb::SystemInfo& info);
    QVariantMap channelInfoToMap(int ch, const psb::ChannelInfo& info);
    QVariantMap systemConfigToMap(const psb::SystemConfig& cfg);
    QVariantMap channelConfigToMap(int ch, const psb::ChannelConfig& cfg);

    void onFrame(bool tx, const std::vector<uint8_t>& data);

    // Merges read-only calibration coefficients (outCalK/B, measVCalK/B,
    // measICalK/B + their decimal exponents) into a channel config map, for
    // display only — writing them is a factory-tool-only operation (see
    // docs/guide/calibration-guide.md). Only reads if the board's sysCapFlags
    // report SysCap::CALIBRATION_MODE, so boards without it pay no extra
    // transactions. Called at connect-time full read and after Save/Load/
    // Factory (the only places a full channel-config refresh happens) —
    // never from the narrow per-write refreshes, since none of those writes
    // touch calibration registers.
    void mergeCalConfig(int ch, QVariantMap& map);

    // Narrow, action-specific post-write refreshes — mirrors demo_tui's
    // tab_monitor.h/tab_channel.h refreshXxx lambdas. Each re-reads only the
    // Modbus block the corresponding write actually touched (merging into
    // m_cachedChInfo/m_cachedChConfig in place via the client's
    // reference-taking methods, never a wholesale struct replace) and emits
    // the matching Ready signal so QML picks up the confirmed device state
    // immediately instead of waiting for the next poll tick — which never
    // arrives for config fields, since doPollStatus() only reads realtime
    // status/measurement registers, not config.
    void refreshChannelStatus(int ch);
    void refreshChannelOutput(int ch);
    void refreshChannelOutputEnabled(int ch);
    void refreshChannelProtection(int ch);
    void refreshChannelRecovery(int ch);
    void refreshChannelDerate(int ch);
    void refreshChannelFull(int ch);
    void refreshSystemConfig();
};
