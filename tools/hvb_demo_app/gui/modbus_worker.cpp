#include "modbus_worker.h"
#include "register_map.h"

#include <QDateTime>

ModbusWorker::ModbusWorker(QObject* parent) : QObject(parent)
{
    m_client.setFrameCallback([this](bool tx, const std::vector<uint8_t>& data) {
        onFrame(tx, data);
    });
}

ModbusWorker::~ModbusWorker()
{
    m_client.disconnect();
}

void ModbusWorker::onFrame(bool tx, const std::vector<uint8_t>& data)
{
    auto now = QDateTime::currentDateTime().toString("hh:mm:ss.zzz");
    std::string msg = now.toStdString() + (tx ? " Tx: " : " Rx: ");
    for (size_t i = 0; i < data.size(); ++i) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%s%02X", i > 0 ? " " : "", data[i]);
        msg += buf;
    }
    emit rawFrameLog(QString::fromStdString(msg));
}

// ---------------------------------------------------------------------------
//  Connection
// ---------------------------------------------------------------------------

void ModbusWorker::doConnect(const QString& port, int baud, int slaveId, int timeoutMs)
{
    bool ok = m_client.connect(port.toStdString(), baud, slaveId, timeoutMs);
    emit connected(ok, ok ? QString() : QString::fromStdString(m_client.lastError()));
}

void ModbusWorker::doDisconnect()
{
    m_client.disconnect();
    emit disconnected();
}

void ModbusWorker::doScanPorts()
{
    auto ports = hvb::HvbModbusClient::scanPorts();
    QStringList list;
    for (const auto& p : ports) list << QString::fromStdString(p);
    emit portsScanned(list);
}

// ---------------------------------------------------------------------------
//  Reads
// ---------------------------------------------------------------------------

QVariantMap ModbusWorker::systemInfoToMap(const hvb::SystemInfo& info)
{
    QVariantMap m;
    m["protoMajor"] = info.protoMajor;
    m["protoMinor"] = info.protoMinor;
    m["variantId"] = info.variantId;
    m["supportedChannels"] = info.supportedChannels;
    m["activeChMask"] = info.activeChMask;
    m["boardTempC"] = static_cast<double>(info.boardTempRaw) * 0.1;  // raw LSB, estimate
    m["boardHumidityPct"] = static_cast<double>(info.boardHumidityRaw) * 0.1;
    m["uptimeSec"] = info.uptimeSec;
    m["fwVersion"] = info.fwVersion;
    m["activeOpMode"] = static_cast<int>(info.activeOpMode);
    m["sysStatus"] = info.sysStatus;
    m["faultCause"] = info.faultCause;
    m["sysCapFlags"] = info.sysCapFlags;

    m["capAutoMode"] = (info.sysCapFlags & hvb::SysCap::AUTO_MODE_SUPPORTED) != 0;
    m["capEnvSensor"] = (info.sysCapFlags & hvb::SysCap::ENV_SENSOR_PRESENT) != 0;
    return m;
}

QVariantMap ModbusWorker::channelInfoToMap(int /*ch*/, const hvb::ChannelInfo& info)
{
    QVariantMap m;
    m["voltageRaw"] = info.voltageRaw;
    m["currentRaw"] = info.currentRaw;
    m["operationalTargetVRaw"] = info.operationalTargetVoltageRaw;
    m["voltageV"] = hvb::reg::voltageToV(info.voltageRaw);
    m["currentA"] = hvb::reg::currentToA(info.currentRaw);
    m["operationalTargetV"] = hvb::reg::voltageToV(info.operationalTargetVoltageRaw);
    m["status"] = info.status;
    m["activeFault"] = info.activeFault;
    m["faultHistory"] = info.faultHistory;
    m["lastProtOutputAction"] = info.lastProtOutputAction;
    m["retryCount"] = info.retryCount;
    m["cooldownSec"] = info.cooldownSec;
    m["lastFaultTimestamp"] = info.lastFaultTimestamp;
    m["chCapFlags"] = info.chCapFlags;

    m["statusOutDrive"] = (info.status & hvb::ChStatus::OUTPUT_DRIVE_NONZERO) != 0;
    m["statusOutEn"] = (info.status & hvb::ChStatus::OUTPUT_ENABLE_ACTIVE) != 0;
    m["statusRamping"] = (info.status & hvb::ChStatus::RAMPING_ACTIVE) != 0;
    m["statusActiveFault"] = (info.status & hvb::ChStatus::ACTIVE_FAULT) != 0;
    m["statusFaultHistory"] = (info.status & hvb::ChStatus::FAULT_HISTORY) != 0;
    m["statusCooldown"] = (info.status & hvb::ChStatus::COOLDOWN_ACTIVE) != 0;
    m["statusMeasStale"] = (info.status & hvb::ChStatus::MEASUREMENT_STALE) != 0;

    m["faultVLimit"] = (info.activeFault & hvb::FaultCause::VOLTAGE_LIMIT) != 0;
    m["faultILimit"] = (info.activeFault & hvb::FaultCause::CURRENT_LIMIT) != 0;
    m["faultMeasInvalid"] = (info.activeFault & hvb::FaultCause::MEASUREMENT_INVALID) != 0;
    m["faultHw"] = (info.activeFault & hvb::FaultCause::OUTPUT_HW_FAULT) != 0;
    m["faultInterlock"] = (info.activeFault & hvb::FaultCause::VARIANT_INTERLOCK) != 0;
    m["faultRetryExhausted"] = (info.activeFault & hvb::FaultCause::AUTO_RETRY_EXHAUSTED) != 0;
    m["faultConfigInvalid"] = (info.activeFault & hvb::FaultCause::CONFIG_INVALID_AUTO) != 0;
    m["faultMeasStale"] = (info.activeFault & hvb::FaultCause::MEASUREMENT_STALE) != 0;

    m["capOutEn"] = (info.chCapFlags & hvb::ChCap::OUTPUT_ENABLE_CTRL) != 0;
    m["capCurrentMeas"] = (info.chCapFlags & hvb::ChCap::CURRENT_MEAS) != 0;
    m["capAutoRec"] = (info.chCapFlags & hvb::ChCap::AUTO_RECOVERY) != 0;
    return m;
}

QVariantMap ModbusWorker::systemConfigToMap(const hvb::SystemConfig& cfg)
{
    QVariantMap m;
    m["operatingMode"]        = static_cast<int>(cfg.operatingMode);
    m["startupChannelPolicy"] = cfg.startupChannelPolicy;
    m["slaveAddr"]            = cfg.slaveAddr;
    m["baudRateCode"]         = cfg.baudRateCode;
    return m;
}

QVariantMap ModbusWorker::channelConfigToMap(int /*ch*/, const hvb::ChannelConfig& cfg)
{
    QVariantMap m;
    m["configuredTargetVRaw"]  = cfg.configuredTargetVRaw;
    m["configuredTargetV"]     = hvb::reg::voltageToV(cfg.configuredTargetVRaw);
    m["outputAction"]          = static_cast<int>(cfg.outputAction);
    m["faultCommand"]          = static_cast<int>(cfg.faultCommand);
    m["rampUpStepRaw"]         = cfg.rampUpStepRaw;
    m["rampUpInterval"]        = cfg.rampUpInterval;
    m["rampDownStepRaw"]       = cfg.rampDownStepRaw;
    m["rampDownInterval"]      = cfg.rampDownInterval;
    m["recoveryPolicyMode"]    = static_cast<int>(cfg.recoveryPolicyMode);
    m["autoRetryDelay"]        = cfg.autoRetryDelay;
    m["autoRetryMaxCount"]     = cfg.autoRetryMaxCount;
    m["autoRetryWindow"]       = cfg.autoRetryWindow;
    m["currentSafeBandPct"]    = cfg.currentSafeBandPct;
    m["iProtMode"]             = static_cast<int>(cfg.iProtMode);
    m["iProtOutputAction"]     = static_cast<int>(cfg.iProtOutputAction);
    m["iLimitThresholdRaw"]    = cfg.iLimitThresholdRaw;
    m["iLimitThresholdA"]      = hvb::reg::currentToA(cfg.iLimitThresholdRaw);
    m["derateStepRaw"]         = cfg.derateStepRaw;
    m["derateStepV"]           = hvb::reg::voltageToV(cfg.derateStepRaw);
    return m;
}

void ModbusWorker::doRefreshSystemInfo()
{
    auto info = m_client.readSystemInfo();
    if (!m_client.isConnected()) { emit operationComplete(false, "Read failed"); return; }
    emit systemInfoReady(systemInfoToMap(info));
}

void ModbusWorker::doRefreshChannelInfo(int ch)
{
    auto info = m_client.readChannelInfo(ch);
    if (!m_client.isConnected()) { emit operationComplete(false, "Read failed"); return; }
    emit channelInfoReady(ch, channelInfoToMap(ch, info));
}

void ModbusWorker::doReadSystemConfig()
{
    auto cfg = m_client.readSystemConfig();
    if (!m_client.isConnected()) { emit operationComplete(false, "Read failed"); return; }
    emit systemConfigReady(systemConfigToMap(cfg));
}

void ModbusWorker::doReadChannelConfig(int ch)
{
    auto cfg = m_client.readChannelConfig(ch);
    auto cal = m_client.readChannelCalConfig(ch);
    if (!m_client.isConnected()) { emit operationComplete(false, "Read failed"); return; }
    auto map = channelConfigToMap(ch, cfg);
    // Merge cal coefficients so QML channel tab can display them from the same config map
    map["outCalK"]  = cal.outCalK;
    map["outCalB"]  = cal.outCalB;
    map["measVCalK"] = cal.measVCalK;
    map["measVCalB"] = cal.measVCalB;
    map["measICalK"] = cal.measICalK;
    map["measICalB"] = cal.measICalB;
    emit channelConfigReady(ch, map);
}

// ---------------------------------------------------------------------------
//  Writes — System
// ---------------------------------------------------------------------------

void ModbusWorker::doWriteOperatingMode(int mode) {
    bool ok = m_client.writeOperatingMode(static_cast<hvb::OpMode>(mode));
    emit operationComplete(ok, ok ? "OK" : QString::fromStdString(m_client.lastError()));
}

void ModbusWorker::doWriteStartupChannelPolicy(int policy) {
    bool ok = m_client.writeStartupChannelPolicy(static_cast<uint16_t>(policy));
    emit operationComplete(ok, ok ? "OK" : QString::fromStdString(m_client.lastError()));
}

void ModbusWorker::doWriteSlaveAddress(int addr) {
    bool ok = m_client.writeSlaveAddress(static_cast<uint16_t>(addr));
    emit operationComplete(ok, ok ? "OK" : QString::fromStdString(m_client.lastError()));
}

void ModbusWorker::doWriteBaudRate(int code) {
    bool ok = m_client.writeBaudRateCode(static_cast<uint16_t>(code));
    emit operationComplete(ok, ok ? "OK" : QString::fromStdString(m_client.lastError()));
}

// ---------------------------------------------------------------------------
//  Writes — Channel
// ---------------------------------------------------------------------------

void ModbusWorker::doSendOutputAction(int ch, int action) {
    bool ok = m_client.sendOutputAction(ch, static_cast<hvb::OutputAction>(action));
    emit operationComplete(ok, ok ? "OK" : QString::fromStdString(m_client.lastError()));
}

void ModbusWorker::doSendFaultCmd(int ch, int cmd) {
    bool ok = m_client.sendChannelFaultCommand(ch, static_cast<hvb::ChannelFaultCommand>(cmd));
    emit operationComplete(ok, ok ? "OK" : QString::fromStdString(m_client.lastError()));
}

void ModbusWorker::doWriteTargetVoltage(int ch, int raw) {
    bool ok = m_client.writeConfiguredTargetVoltage(ch, static_cast<int16_t>(raw));
    emit operationComplete(ok, ok ? "OK" : QString::fromStdString(m_client.lastError()));
}

void ModbusWorker::doWriteRampUp(int ch, int stepRaw, int interval) {
    bool ok = m_client.writeRampUp(ch, static_cast<uint16_t>(stepRaw), static_cast<uint16_t>(interval));
    emit operationComplete(ok, ok ? "OK" : QString::fromStdString(m_client.lastError()));
}

void ModbusWorker::doWriteRampDown(int ch, int stepRaw, int interval) {
    bool ok = m_client.writeRampDown(ch, static_cast<uint16_t>(stepRaw), static_cast<uint16_t>(interval));
    emit operationComplete(ok, ok ? "OK" : QString::fromStdString(m_client.lastError()));
}

void ModbusWorker::doWriteChannelRecovery(int ch, int policy, int delay, int max, int window) {
    bool ok = m_client.writeChannelRecovery(ch,
        static_cast<hvb::RecoveryPolicy>(policy), delay, max, window);
    emit operationComplete(ok, ok ? "OK" : QString::fromStdString(m_client.lastError()));
}

void ModbusWorker::doWriteChannelSafeBand(int ch, int pct) {
    bool ok = m_client.writeChannelSafeBand(ch, static_cast<uint16_t>(pct));
    emit operationComplete(ok, ok ? "OK" : QString::fromStdString(m_client.lastError()));
}

void ModbusWorker::doWriteCurrentProtection(int ch, int mode, int action, int thresholdRaw) {
    bool ok = m_client.writeCurrentProtection(ch,
        static_cast<hvb::ProtectionMode>(mode), static_cast<hvb::OutputAction>(action), static_cast<int16_t>(thresholdRaw));
    emit operationComplete(ok, ok ? "OK" : QString::fromStdString(m_client.lastError()));
}

void ModbusWorker::doWriteDerateStep(int ch, int stepRaw) {
    bool ok = m_client.writeDerateStep(ch, static_cast<uint16_t>(stepRaw));
    emit operationComplete(ok, ok ? "OK" : QString::fromStdString(m_client.lastError()));
}

void ModbusWorker::doWriteCalOutput(int ch, int k, int b) {
    bool ok = m_client.writeCalibrationOutput(ch, static_cast<uint16_t>(k), static_cast<int16_t>(b));
    emit operationComplete(ok, ok ? "OK" : QString::fromStdString(m_client.lastError()));
}

void ModbusWorker::doWriteCalMeasV(int ch, int k, int b) {
    bool ok = m_client.writeCalibrationMeasV(ch, static_cast<uint16_t>(k), static_cast<int16_t>(b));
    emit operationComplete(ok, ok ? "OK" : QString::fromStdString(m_client.lastError()));
}

void ModbusWorker::doWriteCalMeasI(int ch, int k, int b) {
    bool ok = m_client.writeCalibrationMeasI(ch, static_cast<uint16_t>(k), static_cast<int16_t>(b));
    emit operationComplete(ok, ok ? "OK" : QString::fromStdString(m_client.lastError()));
}

void ModbusWorker::doExitCalibrationMode() {
    bool ok = m_client.exitCalibrationMode();
    emit operationComplete(ok, ok ? "OK" : QString::fromStdString(m_client.lastError()));
}

void ModbusWorker::doSendParamAction(int chScope, int action) {
    bool ok = m_client.sendParamAction(chScope, static_cast<hvb::ParamAction>(action));
    emit operationComplete(ok, ok ? "OK" : QString::fromStdString(m_client.lastError()));
}

// ---------------------------------------------------------------------------
//  Raw
// ---------------------------------------------------------------------------

void ModbusWorker::doRawReadFc04(int addr, int count) {
    QVector<uint16_t> buf(count);
    if (!m_client.readInputRegs(static_cast<uint16_t>(addr), static_cast<uint16_t>(count), buf.data())) {
        emit rawHexResult("Error: " + QString::fromStdString(m_client.lastError()));
        return;
    }
    QString hex;
    for (int i = 0; i < count; ++i) {
        if (i > 0) hex += " ";
        hex += QString::asprintf("%04X", buf[i]);
        if (i % 8 == 7) hex += "\n";
    }
    emit rawHexResult(hex);
}

void ModbusWorker::doRawReadFc03(int addr, int count) {
    QVector<uint16_t> buf(count);
    if (!m_client.readHoldingRegs(static_cast<uint16_t>(addr), static_cast<uint16_t>(count), buf.data())) {
        emit rawHexResult("Error: " + QString::fromStdString(m_client.lastError()));
        return;
    }
    QString hex;
    for (int i = 0; i < count; ++i) {
        if (i > 0) hex += " ";
        hex += QString::asprintf("%04X", buf[i]);
        if (i % 8 == 7) hex += "\n";
    }
    emit rawHexResult(hex);
}

void ModbusWorker::doRawWriteFc06(int addr, int value) {
    bool ok = m_client.writeReg16(static_cast<uint16_t>(addr), static_cast<uint16_t>(value));
    emit rawHexResult(ok ? "OK" : ("Error: " + QString::fromStdString(m_client.lastError())));
}
