#include "hvb_modbus_client.h"
#include "register_map.h"

#include <ModbusClientPort.h>
#include <ModbusClient.h>
#include <Modbus.h>

#include <algorithm>
#include <sstream>

namespace hvb {

struct HvbModbusClient::Impl {
    ModbusClientPort* port = nullptr;
    ModbusClient* client = nullptr;
    int slaveId = 1;
    bool connected = false;
    std::string errorText;
    FrameCallback frameCb;

    ~Impl() { disconnect(); }
    void disconnect() {
        if (client) { delete client; client = nullptr; }
        if (port) { port->close(); delete port; port = nullptr; }
        connected = false;
    }
};

HvbModbusClient::HvbModbusClient() : m_impl(std::make_unique<Impl>()) {}
HvbModbusClient::~HvbModbusClient() = default;

// ============================================================================
//  Connection
// ============================================================================

bool HvbModbusClient::connect(const std::string& portName, int baud, int slaveId, int timeoutMs) {
    m_impl->disconnect();
    m_impl->slaveId = slaveId;

    Modbus::SerialSettings settings;
    settings.portName = portName.c_str();
    settings.baudRate = baud;
    settings.dataBits = 8;
    settings.parity = Modbus::NoParity;
    settings.stopBits = Modbus::OneStop;
    settings.flowControl = Modbus::NoFlowControl;
    settings.timeoutFirstByte = static_cast<uint32_t>(timeoutMs);
    settings.timeoutInterByte = std::max(10u, static_cast<uint32_t>(timeoutMs) / 10);

    m_impl->port = Modbus::createClientPort(Modbus::RTU, &settings, true);
    if (!m_impl->port) {
        m_impl->errorText = "failed to create RTU client port";
        return false;
    }
    m_impl->client = new ModbusClient(static_cast<uint8_t>(slaveId), m_impl->port);
    m_impl->connected = true;
    return true;
}

void HvbModbusClient::disconnect()    { m_impl->disconnect(); }
bool HvbModbusClient::isConnected() const { return m_impl->connected && m_impl->port && m_impl->port->isOpen(); }
std::string HvbModbusClient::lastError() const { return m_impl->errorText; }
int HvbModbusClient::slaveId() const { return m_impl->slaveId; }

// ============================================================================
//  Internal
// ============================================================================

bool HvbModbusClient::checkConnected() {
    if (!m_impl->connected || !m_impl->client) {
        m_impl->errorText = "not connected";
        return false;
    }
    return true;
}

bool HvbModbusClient::readRegsInternal(bool holding, uint16_t addr, uint16_t count, uint16_t* out) {
    if (!checkConnected()) return false;
    Modbus::StatusCode s;
    auto unit = static_cast<uint8_t>(m_impl->slaveId);
    if (holding)
        s = m_impl->port->readHoldingRegisters(unit, addr, count, out);
    else
        s = m_impl->port->readInputRegisters(unit, addr, count, out);
    if (!Modbus::StatusIsGood(s)) {
        m_impl->errorText = std::string("Modbus error: ") + m_impl->port->lastErrorText();
        return false;
    }
    return true;
}

bool HvbModbusClient::writeRegsInternal(uint16_t addr, uint16_t count, const uint16_t* values) {
    if (!checkConnected()) return false;
    Modbus::StatusCode s;
    auto unit = static_cast<uint8_t>(m_impl->slaveId);
    if (count == 1)
        s = m_impl->port->writeSingleRegister(unit, addr, values[0]);
    else
        s = m_impl->port->writeMultipleRegisters(unit, addr, count, values);
    if (!Modbus::StatusIsGood(s)) {
        m_impl->errorText = std::string("Modbus error: ") + m_impl->port->lastErrorText();
        return false;
    }
    return true;
}

// ============================================================================
//  High-level reads — all single UINT16, no INT32 packing
// ============================================================================

SystemInfo HvbModbusClient::readSystemInfo() {
    SystemInfo info;
    if (!checkConnected()) return info;

    uint16_t buf[15] = {};
    if (!readRegsInternal(false, reg::sysAddr(0), 15, buf)) return info;

    info.protoMajor       = static_cast<int>(buf[SYS_PROTOCOL_MAJOR]);
    info.protoMinor       = static_cast<int>(buf[SYS_PROTOCOL_MINOR]);
    info.variantId        = static_cast<int>(buf[SYS_VARIANT_ID]);
    info.sysCapFlags      = buf[SYS_CAPABILITY_FLAGS];
    info.supportedChannels = static_cast<int>(buf[SYS_SUPPORTED_CHANNELS]);
    info.activeChMask     = buf[SYS_ACTIVE_CHANNEL_MASK];
    info.boardTempRaw     = static_cast<int16_t>(buf[SYS_BOARD_TEMPERATURE]);
    info.boardHumidityRaw = buf[SYS_BOARD_HUMIDITY];
    info.uptimeSec        = reg::uint32FromRegs(buf[SYS_UPTIME_HI], buf[SYS_UPTIME_LO]);
    info.fwVersion        = reg::uint32FromRegs(buf[SYS_FW_VERSION_HI], buf[SYS_FW_VERSION_LO]);
    info.activeOpMode     = static_cast<OpMode>(buf[SYS_ACTIVE_OPERATING_MODE]);
    info.sysStatus        = buf[SYS_STATUS];
    info.faultCause       = buf[SYS_FAULT_CAUSE];
    return info;
}

ChannelInfo HvbModbusClient::readChannelInfo(int ch) {
    ChannelInfo info;
    if (!checkConnected()) return info;

    uint16_t base = reg::chAddr(ch, 0);
    uint16_t buf[12] = {};  // offsets 0..11
    if (!readRegsInternal(false, base, 12, buf)) return info;

    info.voltageRaw                   = static_cast<int16_t>(buf[CH_MEASURED_VOLTAGE]);
    info.currentRaw                   = static_cast<int16_t>(buf[CH_MEASURED_CURRENT]);
    info.operationalTargetVoltageRaw  = static_cast<int16_t>(buf[CH_OPER_TARGET_VOLTAGE]);
    info.status                       = buf[CH_STATUS_BITS];
    info.activeFault                  = buf[CH_ACTIVE_FAULT_CAUSE];
    info.faultHistory                 = buf[CH_FAULT_HISTORY_CAUSE];
    info.lastProtOutputAction         = buf[CH_LAST_PROT_OUT_ACTION];
    info.retryCount                   = static_cast<int>(buf[CH_AUTO_RETRY_COUNT]);
    info.cooldownSec                  = static_cast<int>(buf[CH_AUTO_COOLDOWN_REMAINING]);
    info.lastFaultTimestamp           = reg::uint32FromRegs(buf[CH_LAST_FAULT_TIMESTAMP_HI],
                                                            buf[CH_LAST_FAULT_TIMESTAMP_LO]);
    info.chCapFlags                   = buf[CH_CAPABILITY_FLAGS];
    return info;
}

SystemConfig HvbModbusClient::readSystemConfig() {
    SystemConfig cfg;
    if (!checkConnected()) return cfg;

    uint16_t buf[9] = {};
    if (!readRegsInternal(true, reg::sysAddr(0), 9, buf)) return cfg;

    cfg.operatingMode      = static_cast<OpMode>(buf[SYS_OPERATING_MODE]);
    cfg.slaveAddr          = buf[SYS_SLAVE_ADDRESS];
    cfg.baudRateCode       = buf[SYS_BAUD_RATE_CODE];
    cfg.recoveryPolicy     = static_cast<RecoveryPolicy>(buf[SYS_RECOVERY_POLICY_MODE]);
    cfg.retryDelay         = static_cast<int>(buf[SYS_AUTO_RETRY_DELAY]);
    cfg.retryMax           = static_cast<int>(buf[SYS_AUTO_RETRY_MAX_COUNT]);
    cfg.retryWindow        = static_cast<int>(buf[SYS_AUTO_RETRY_WINDOW]);
    cfg.voltageSafeBandPct = buf[SYS_VOLTAGE_SAFE_BAND_PCT];
    cfg.currentSafeBandPct = buf[SYS_CURRENT_SAFE_BAND_PCT];
    return cfg;
}

ChannelConfig HvbModbusClient::readChannelConfig(int ch) {
    ChannelConfig cfg;
    if (!checkConnected()) return cfg;

    uint16_t base = reg::chAddr(ch, 0);
    uint16_t buf[21] = {};  // offsets 0..20
    if (!readRegsInternal(true, base, 21, buf)) return cfg;

    cfg.configuredTargetVRaw = static_cast<int16_t>(buf[CH_CFG_TARGET_VOLTAGE]);
    cfg.outputAction         = static_cast<OutputAction>(buf[CH_OUTPUT_ACTION]);
    cfg.faultCommand         = static_cast<ChannelFaultCommand>(buf[CH_FAULT_CMD]);
    cfg.rampUpStepRaw        = buf[CH_RAMP_UP_STEP];
    cfg.rampUpInterval       = buf[CH_RAMP_UP_INTERVAL];
    cfg.rampDownStepRaw      = buf[CH_RAMP_DOWN_STEP];
    cfg.rampDownInterval     = buf[CH_RAMP_DOWN_INTERVAL];
    cfg.vProtMode            = static_cast<ProtectionMode>(buf[CH_VOLTAGE_PROTECTION_MODE]);
    cfg.vProtOutputAction    = static_cast<OutputAction>(buf[CH_VOLTAGE_PROT_OUT_ACTION]);
    cfg.vLimitThresholdRaw   = static_cast<int16_t>(buf[CH_VOLTAGE_LIMIT_THRESHOLD]);
    cfg.iProtMode            = static_cast<ProtectionMode>(buf[CH_CURRENT_PROTECTION_MODE]);
    cfg.iProtOutputAction    = static_cast<OutputAction>(buf[CH_CURRENT_PROT_OUT_ACTION]);
    cfg.iLimitThresholdRaw   = static_cast<int16_t>(buf[CH_CURRENT_LIMIT_THRESHOLD]);
    cfg.derateStepRaw        = buf[CH_AUTO_DERATE_STEP];
    cfg.saveTargetPolicy     = buf[CH_SAVE_TARGET_POLICY] != 0;
    cfg.outCalK   = buf[CH_OUTPUT_CAL_K];
    cfg.outCalB   = static_cast<int16_t>(buf[CH_OUTPUT_CAL_B]);
    cfg.measVCalK = buf[CH_MEASURED_V_CAL_K];
    cfg.measVCalB = static_cast<int16_t>(buf[CH_MEASURED_V_CAL_B]);
    cfg.measICalK = buf[CH_MEASURED_I_CAL_K];
    cfg.measICalB = static_cast<int16_t>(buf[CH_MEASURED_I_CAL_B]);
    return cfg;
}

// ============================================================================
//  High-level writes — system
// ============================================================================

#define WR1(addr,val)  writeRegsInternal(reg::sysAddr(addr), 1, (val))
#define WR(addr,n,val) writeRegsInternal(reg::sysAddr(addr), n, (val))

bool HvbModbusClient::writeOperatingMode(OpMode m) {
    uint16_t v = static_cast<uint16_t>(m);
    return WR1(SYS_OPERATING_MODE, &v);
}
bool HvbModbusClient::writeSlaveAddress(uint16_t a)     { return WR1(SYS_SLAVE_ADDRESS, &a); }
bool HvbModbusClient::writeBaudRateCode(uint16_t c)      { return WR1(SYS_BAUD_RATE_CODE, &c); }
bool HvbModbusClient::writeSystemRecoveryPolicy(RecoveryPolicy p, int delay, int max, int window) {
    uint16_t buf[4] = { static_cast<uint16_t>(p), static_cast<uint16_t>(delay),
                        static_cast<uint16_t>(max), static_cast<uint16_t>(window) };
    return WR(SYS_RECOVERY_POLICY_MODE, 4, buf);
}
bool HvbModbusClient::writeSafeBands(uint16_t vPct, uint16_t iPct) {
    uint16_t buf[2] = { vPct, iPct };
    return WR(SYS_VOLTAGE_SAFE_BAND_PCT, 2, buf);
}
bool HvbModbusClient::sendParamAction(int chScope, ParamAction action) {
    uint16_t v = static_cast<uint16_t>(action);
    uint16_t addr = chScope < 0 ? reg::sysAddr(SYS_PARAM_ACTION)
                                : reg::chAddr(chScope, CH_PARAM_ACTION);
    return writeRegsInternal(addr, 1, &v);
}
#undef WR1
#undef WR

// ============================================================================
//  High-level writes — channel
// ============================================================================

#define CHW(a,val) writeRegsInternal(reg::chAddr(ch, a), 1, (val))
#define CHWN(a,n,val) writeRegsInternal(reg::chAddr(ch, a), n, (val))

bool HvbModbusClient::writeConfiguredTargetVoltage(int ch, int16_t raw) {
    uint16_t v = static_cast<uint16_t>(raw);
    return CHW(CH_CFG_TARGET_VOLTAGE, &v);
}
bool HvbModbusClient::sendOutputAction(int ch, OutputAction action) {
    if (!isValidOutputAction(action, ActionContext::Host)) {
        m_impl->errorText = "invalid output action for host context";
        return false;
    }
    uint16_t v = static_cast<uint16_t>(action);
    return CHW(CH_OUTPUT_ACTION, &v);
}
bool HvbModbusClient::sendChannelFaultCommand(int ch, ChannelFaultCommand cmd) {
    uint16_t v = static_cast<uint16_t>(cmd);
    return CHW(CH_FAULT_CMD, &v);
}
bool HvbModbusClient::writeRampUp(int ch, uint16_t stepRaw, uint16_t interval) {
    uint16_t buf[2] = { stepRaw, interval };
    return CHWN(CH_RAMP_UP_STEP, 2, buf);
}
bool HvbModbusClient::writeRampDown(int ch, uint16_t stepRaw, uint16_t interval) {
    uint16_t buf[2] = { stepRaw, interval };
    return CHWN(CH_RAMP_DOWN_STEP, 2, buf);
}
bool HvbModbusClient::writeVoltageProtection(int ch, ProtectionMode mode, OutputAction action, int16_t thresholdRaw) {
    if (!isValidOutputAction(action, ActionContext::VoltageProtection)) {
        m_impl->errorText = "invalid output action for voltage protection";
        return false;
    }
    uint16_t buf[3] = { static_cast<uint16_t>(mode), static_cast<uint16_t>(action), static_cast<uint16_t>(thresholdRaw) };
    return CHWN(CH_VOLTAGE_PROTECTION_MODE, 3, buf);
}
bool HvbModbusClient::writeCurrentProtection(int ch, ProtectionMode mode, OutputAction action, int16_t thresholdRaw) {
    if (!isValidOutputAction(action, ActionContext::CurrentProtection)) {
        m_impl->errorText = "invalid output action for current protection";
        return false;
    }
    uint16_t buf[3] = { static_cast<uint16_t>(mode), static_cast<uint16_t>(action), static_cast<uint16_t>(thresholdRaw) };
    return CHWN(CH_CURRENT_PROTECTION_MODE, 3, buf);
}
bool HvbModbusClient::writeDerateStep(int ch, uint16_t stepRaw) {
    return CHW(CH_AUTO_DERATE_STEP, &stepRaw);
}
bool HvbModbusClient::writeSaveTargetPolicy(int ch, bool saveTarget) {
    uint16_t v = saveTarget ? 1 : 0;
    return CHW(CH_SAVE_TARGET_POLICY, &v);
}
bool HvbModbusClient::writeCalibrationOutput(int ch, uint16_t k, int16_t b) {
    uint16_t buf[2] = { k, static_cast<uint16_t>(b) };
    return CHWN(CH_OUTPUT_CAL_K, 2, buf);
}
bool HvbModbusClient::writeCalibrationMeasV(int ch, uint16_t k, int16_t b) {
    uint16_t buf[2] = { k, static_cast<uint16_t>(b) };
    return CHWN(CH_MEASURED_V_CAL_K, 2, buf);
}
bool HvbModbusClient::writeCalibrationMeasI(int ch, uint16_t k, int16_t b) {
    uint16_t buf[2] = { k, static_cast<uint16_t>(b) };
    return CHWN(CH_MEASURED_I_CAL_K, 2, buf);
}

#undef CHW
#undef CHWN

// ============================================================================
//  Low-level
// ============================================================================

bool HvbModbusClient::readInputRegs(uint16_t addr, uint16_t count, uint16_t* out) {
    return readRegsInternal(false, addr, count, out);
}
bool HvbModbusClient::readHoldingRegs(uint16_t addr, uint16_t count, uint16_t* out) {
    return readRegsInternal(true, addr, count, out);
}
bool HvbModbusClient::writeReg16(uint16_t addr, uint16_t value) {
    return writeRegsInternal(addr, 1, &value);
}

void HvbModbusClient::setFrameCallback(FrameCallback cb) {
    m_impl->frameCb = std::move(cb);
}

std::vector<std::string> HvbModbusClient::scanPorts() {
    std::vector<std::string> result;
    for (const auto& p : Modbus::availableSerialPorts()) {
        std::string path(p);
#if defined(_WIN32)
        if (path.rfind("COM", 0) == 0)
            result.push_back(path);
#else
        if (path.rfind("/dev/ttyUSB", 0) == 0)
            result.push_back(path);
#endif
    }
    return result;
}

std::vector<int> HvbModbusClient::availableBaudRates() {
    std::vector<int> result;
    for (auto r : Modbus::availableBaudRate())
        result.push_back(static_cast<int>(r));
    return result;
}

} // namespace hvb
