#include "hvb_modbus_client.h"
#include "register_map.h"

#include <ModbusClientPort.h>
#include <Modbus.h>

#include <algorithm>
#include <cstring>
#include <sstream>

namespace hvb {

struct HvbModbusClient::Impl {
    ModbusClientPort* port = nullptr;
    int slaveId = 1;
    bool connected = false;
    std::string errorText;
    FrameCallback frameCb;

    // Test mode — direct array access (bypasses Modbus RTU)
    uint16_t* testInputRegs = nullptr;
    uint16_t* testHoldingRegs = nullptr;
    int testMaxAddr = 0;

    ~Impl() { disconnect(); }
    void disconnect() {
        if (port) { delete port; port = nullptr; }
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

    // Non-blocking mode: engages ModbusLib's inter-byte accumulation loop
    // (nonBlockingRead). The blocking path does a single ::read() and treats a
    // partial frame as complete, which truncates responses >~31 bytes (>=14
    // registers) over USB-serial and yields spurious CRC errors. Our read/write
    // wrappers poll until the request stops returning Status_Processing.
    m_impl->port = Modbus::createClientPort(Modbus::RTU, &settings, false);
    if (!m_impl->port) {
        m_impl->errorText = "failed to create RTU client port";
        return false;
    }
    m_impl->connected = true;
    return true;
}

void HvbModbusClient::disconnect()    { m_impl->disconnect(); }
bool HvbModbusClient::isConnected() const { return m_impl->connected || m_impl->testInputRegs; }
std::string HvbModbusClient::lastError() const { return m_impl->errorText; }
int HvbModbusClient::slaveId() const { return m_impl->slaveId; }

void HvbModbusClient::attachTestArrays(uint16_t* inputRegs, uint16_t* holdingRegs, int maxAddr) {
    m_impl->testInputRegs = inputRegs;
    m_impl->testHoldingRegs = holdingRegs;
    m_impl->testMaxAddr = maxAddr;
    m_impl->connected = true;
}

void HvbModbusClient::detachTestArrays() {
    m_impl->testInputRegs = nullptr;
    m_impl->testHoldingRegs = nullptr;
    m_impl->testMaxAddr = 0;
    m_impl->connected = false;
}

// ============================================================================
//  Internal
// ============================================================================

bool HvbModbusClient::checkConnected() {
    if (m_impl->testInputRegs || m_impl->testHoldingRegs) return true;
    if (!m_impl->connected || !m_impl->port) {
        m_impl->errorText = "not connected";
        return false;
    }
    return true;
}

bool HvbModbusClient::readRegsInternal(bool holding, uint16_t addr, uint16_t count, uint16_t* out) {
    // Test mode — direct array access
    if (m_impl->testInputRegs || m_impl->testHoldingRegs) {
        uint16_t* src = holding ? m_impl->testHoldingRegs : m_impl->testInputRegs;
        if (!src || addr + count > static_cast<uint16_t>(m_impl->testMaxAddr)) {
            m_impl->errorText = "test: address out of range";
            return false;
        }
        memcpy(out, src + addr, count * sizeof(uint16_t));
        return true;
    }
    if (!checkConnected()) return false;
    Modbus::StatusCode s;
    auto unit = static_cast<uint8_t>(m_impl->slaveId);
    // Non-blocking port: drive the request to completion. ModbusLib's internal
    // timeout/retry logic guarantees this terminates (Good or Bad).
    do {
        if (holding)
            s = m_impl->port->readHoldingRegisters(unit, addr, count, out);
        else
            s = m_impl->port->readInputRegisters(unit, addr, count, out);
    } while (Modbus::StatusIsProcessing(s));
    if (!Modbus::StatusIsGood(s)) {
        m_impl->errorText = std::string("Modbus error: ") + m_impl->port->lastErrorText();
        return false;
    }
    return true;
}

bool HvbModbusClient::writeRegsInternal(uint16_t addr, uint16_t count, const uint16_t* values) {
    // Test mode — direct array access
    if (m_impl->testHoldingRegs) {
        if (addr + count > static_cast<uint16_t>(m_impl->testMaxAddr)) {
            m_impl->errorText = "test: address out of range";
            return false;
        }
        memcpy(m_impl->testHoldingRegs + addr, values, count * sizeof(uint16_t));
        return true;
    }
    if (!checkConnected()) return false;
    auto unit = static_cast<uint8_t>(m_impl->slaveId);
    for (uint16_t i = 0; i < count; i++) {
        // Non-blocking port: poll until the write request completes.
        Modbus::StatusCode s;
        do {
            s = m_impl->port->writeSingleRegister(unit, addr + i, values[i]);
        } while (Modbus::StatusIsProcessing(s));
        if (!Modbus::StatusIsGood(s)) {
            m_impl->errorText = std::string("Modbus error: ") + m_impl->port->lastErrorText();
            return false;
        }
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
    uint16_t buf[18] = {};  // offsets 0..17
    if (!readRegsInternal(false, base, 18, buf)) return info;

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
    info.rawAdcVoltage               = reg::int32FromRegs(buf[CH_RAW_ADC_VOLTAGE_HI],
                                                           buf[CH_RAW_ADC_VOLTAGE_LO]);
    info.rawAdcCurrent               = reg::int32FromRegs(buf[CH_RAW_ADC_CURRENT_HI],
                                                           buf[CH_RAW_ADC_CURRENT_LO]);
    /* v3: CH_CAL_SAMPLE_STATUS and CH_RAW_DAC_READBACK removed from FC04 input regs */
    return info;
}

SystemConfig HvbModbusClient::readSystemConfig() {
    SystemConfig cfg;
    if (!checkConnected()) return cfg;

    /* v3: sys holding has only 4 registers (operating mode, startup policy, slave addr, baud) */
    uint16_t buf[4] = {};
    if (!readRegsInternal(true, reg::sysAddr(0), 4, buf)) return cfg;

    cfg.operatingMode = static_cast<OpMode>(buf[SYS_OPERATING_MODE]);
    cfg.slaveAddr     = buf[SYS_SLAVE_ADDRESS];
    cfg.baudRateCode  = buf[SYS_BAUD_RATE_CODE];
    /* v3: recoveryPolicy/retryDelay/retryMax/retryWindow/safeBandPct moved to channel level */
    return cfg;
}

ChannelConfig HvbModbusClient::readChannelConfig(int ch) {
    ChannelConfig cfg;
    if (!checkConnected()) return cfg;

    uint16_t base = reg::chAddr(ch, 0);
    // Board limits single-read count; split into two batches
    uint16_t buf[12] = {};
    if (!readRegsInternal(true, base, 12, buf)) return cfg;

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

    // Batch 2: offsets 12..23
    uint16_t buf2[12] = {};
    if (!readRegsInternal(true, base + 12, 12, buf2)) return cfg;

    cfg.iLimitThresholdRaw   = static_cast<int16_t>(buf2[CH_CURRENT_LIMIT_THRESHOLD - 12]);
    cfg.derateStepRaw        = buf2[CH_AUTO_DERATE_STEP - 12];
    /* v3: CH_SAVE_TARGET_POLICY removed */
    cfg.outCalK              = buf2[CH_OUTPUT_CAL_K - 12];
    cfg.outCalB              = static_cast<int16_t>(buf2[CH_OUTPUT_CAL_B - 12]);
    cfg.measVCalK            = buf2[CH_MEASURED_V_CAL_K - 12];
    cfg.measVCalB            = static_cast<int16_t>(buf2[CH_MEASURED_V_CAL_B - 12]);
    cfg.measICalK            = buf2[CH_MEASURED_I_CAL_K - 12];
    cfg.measICalB            = static_cast<int16_t>(buf2[CH_MEASURED_I_CAL_B - 12]);
    cfg.calOutputEnabled     = buf2[CH_CAL_OUTPUT_ENABLE - 12] != 0;
    cfg.rawDacCode           = buf2[CH_CAL_DAC_CODE - 12];

    // Batch 3: offsets 24..25
    uint16_t buf3[2] = {};
    if (!readRegsInternal(true, base + 24, 2, buf3)) return cfg;

    cfg.maxRawDacLimit       = buf3[CH_CAL_MAX_RAW_DAC_LIMIT - 24];
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
    /* v3: recovery policy moved to per-channel; writes to channel 0 as a best-effort */
    uint16_t buf[4] = { static_cast<uint16_t>(p), static_cast<uint16_t>(delay),
                        static_cast<uint16_t>(max), static_cast<uint16_t>(window) };
    return writeRegsInternal(reg::chAddr(0, CH_RECOVERY_POLICY_MODE), 4, buf);
}
bool HvbModbusClient::writeSafeBands(uint16_t vPct, uint16_t iPct) {
    /* v3: voltage safe band removed; writes current safe band to channel 0 */
    (void)vPct;
    return writeRegsInternal(reg::chAddr(0, CH_CURRENT_SAFE_BAND_PCT), 1, &iPct);
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
    (void)ch; (void)saveTarget;
    return false; /* v3: CH_SAVE_TARGET_POLICY removed */
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
//  Calibration Mode operations (v2.1)
// ============================================================================

bool HvbModbusClient::unlockCalibrationStep(uint16_t value) {
    return writeRegsInternal(reg::extAddr(EXT_CAL_UNLOCK), 1, &value);
}

bool HvbModbusClient::enterCalibrationMode() {
    return writeOperatingMode(OpMode::Calibration);
}

bool HvbModbusClient::exitCalibrationMode(OpMode targetMode) {
    if (targetMode == OpMode::Calibration) {
        m_impl->errorText = "target mode cannot be Calibration";
        return false;
    }
    return writeOperatingMode(targetMode);
}

bool HvbModbusClient::writeCalibrationOutputEnable(int ch, bool enable) {
    uint16_t v = enable ? 1 : 0;
    return writeRegsInternal(reg::chAddr(ch, CH_CAL_OUTPUT_ENABLE), 1, &v);
}

bool HvbModbusClient::writeRawDacCode(int ch, uint16_t code) {
    return writeRegsInternal(reg::chAddr(ch, CH_CAL_DAC_CODE), 1, &code);
}

bool HvbModbusClient::sendCalibrationSampleCommand(int ch) {
    uint16_t v = CAL_COMMAND_EXECUTE;
    return writeRegsInternal(reg::chAddr(ch, CH_CAL_SAMPLE_CMD), 1, &v);
}

bool HvbModbusClient::sendCalibrationCommitCommand(int ch) {
    uint16_t v = CAL_COMMAND_EXECUTE;
    return writeRegsInternal(reg::chAddr(ch, CH_CAL_COMMIT_CMD), 1, &v);
}

bool HvbModbusClient::writeCalibrationMaxDacLimit(int ch, uint16_t limit) {
    return writeRegsInternal(reg::chAddr(ch, CH_CAL_MAX_RAW_DAC_LIMIT), 1, &limit);
}

CalibrationSnapshot HvbModbusClient::readCalibrationSnapshot(int ch) {
    CalibrationSnapshot snap;
    if (!checkConnected()) return snap;

    /* v3: 4 input regs for raw ADC; CH_CAL_SAMPLE_STATUS and CH_RAW_DAC_READBACK removed */
    uint16_t ibuf[4] = {};
    if (!readRegsInternal(false, reg::chAddr(ch, CH_RAW_ADC_VOLTAGE_HI), 4, ibuf)) return snap;

    snap.rawAdcVoltage  = reg::int32FromRegs(ibuf[0], ibuf[1]);
    snap.rawAdcCurrent  = reg::int32FromRegs(ibuf[2], ibuf[3]);

    uint16_t hbuf[5] = {};
    if (!readRegsInternal(true, reg::chAddr(ch, CH_CAL_OUTPUT_ENABLE), 5, hbuf)) return snap;

    snap.outputEnabled  = hbuf[0] != 0;
    snap.rawDacCode     = hbuf[1];
    snap.rawDacReadback = hbuf[1]; /* v3: rawDacReadback is now the same as rawDacCode (FC03) */
    snap.maxRawDacLimit = hbuf[4];
    return snap;
}

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
