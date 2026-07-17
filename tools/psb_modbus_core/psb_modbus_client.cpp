#include "psb_modbus_client.h"
#include "register_map.h"

#include <ModbusClientPort.h>
#include <ModbusPort.h>
#include <Modbus.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <sstream>
#include <thread>

namespace psb {

// The non-blocking port's request state machine (ModbusClientPort::process())
// is explicitly designed to be pumped from an external timer/event loop —
// it returns Status_Processing immediately while waiting for I/O rather than
// blocking (that's the whole point of non-blocking mode; see its STATE_TIMEOUT
// case, which only sleeps in blocking mode). Driving it with a zero-delay
// do/while spin — as a naive port of the intended "poll until done" pattern
// would — pegs a CPU core at ~100% for the full duration of every single
// transaction. Measured live: this doesn't just waste CPU, it visibly
// degrades real transaction latency under sustained continuous polling
// (confirmed by comparing against mbpoll's much faster, non-spinning
// isolated reads on the same board/cable) — almost certainly by starving
// the kernel's USB-serial interrupt servicing of CPU time on whatever core
// the spin lands on. This tiny yield between checks is cheap next to a
// Modbus RTU round-trip (tens of ms) and eliminates the spin entirely.
static constexpr auto kNonBlockingPollInterval = std::chrono::milliseconds(1);

struct PsbModbusClient::Impl {
    ModbusClientPort* port = nullptr;
    int slaveId = 1;
    bool connected = false;
    std::string errorText;
    FrameCallback frameCb;
    int16_t currentUnitExp = -10;  // see PsbModbusClient::currentUnitExp()

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

PsbModbusClient::PsbModbusClient() : m_impl(std::make_unique<Impl>()) {}
PsbModbusClient::~PsbModbusClient() = default;

// ============================================================================
//  Connection
// ============================================================================

bool PsbModbusClient::connect(const std::string& portName, int baud, int slaveId, int timeoutMs) {
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
    // timeoutInterByte governs how long ModbusLib waits for each subsequent
    // byte within an already-started response before concluding the frame is
    // complete — a property of the serial link and adapter, unrelated to
    // timeoutFirstByte (how long to wait for a response to even begin) and
    // must not be derived from it. The previous timeoutMs/10 formula happened
    // to produce 300ms at this class's typical 3000ms caller default (the
    // TUI) — confirmed via syscall-level tracing that EVERY transaction, not
    // just failures, paid this in full (measured ~306-308ms per transaction,
    // matching 3000/10 almost exactly), since the library waits out the full
    // inter-byte timeout after the last byte arrives before returning, even
    // for an already-complete, healthy response. This is why the CLI's
    // commands (default timeoutMs=500 -> 50ms) looked fine while the TUI's
    // (3000ms -> 300ms) was consistently ~6x slower than a healthy transaction
    // needs to be. 50ms is exactly what the CLI's default timeoutMs=500 has
    // always produced via this same formula — already a proven-safe value in
    // this codebase, not a new guess — with generous margin over both the
    // Modbus RTU spec's frame-silence threshold (~0.3ms at 115200 baud) and
    // the several-ms USB-serial adapter buffering/latency-timer behavior
    // (e.g. CH340) that non-blocking mode exists to tolerate (see the
    // mode-choice comment below).
    settings.timeoutInterByte = 50;

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

    // Mark connected so readRegsInternal allows the probe transaction.
    m_impl->connected = true;

    // Probe: verify the board actually responds before claiming success.
    // Read one well-known input register (PROTOCOL_MAJOR at address 0).
    uint16_t probe = 0xFFFF;
    if (!readRegsInternal(false, reg::sysAddr(0), 1, &probe)) {
        m_impl->errorText = "no response from board — check baud rate, slave ID, and cabling";
        m_impl->disconnect();
        return false;
    }
    if (probe < 1 || probe > 15) {
        m_impl->errorText = "unexpected protocol version " + std::to_string(probe)
                            + " — check baud rate and slave ID";
        m_impl->disconnect();
        return false;
    }

    return true;
}

void PsbModbusClient::disconnect()    { m_impl->disconnect(); }
bool PsbModbusClient::isConnected() const { return m_impl->connected || m_impl->testInputRegs; }
std::string PsbModbusClient::lastError() const { return m_impl->errorText; }
int PsbModbusClient::slaveId() const { return m_impl->slaveId; }
int16_t PsbModbusClient::currentUnitExp() const { return m_impl->currentUnitExp; }

void PsbModbusClient::attachTestArrays(uint16_t* inputRegs, uint16_t* holdingRegs, int maxAddr) {
    m_impl->testInputRegs = inputRegs;
    m_impl->testHoldingRegs = holdingRegs;
    m_impl->testMaxAddr = maxAddr;
    m_impl->connected = true;
}

void PsbModbusClient::detachTestArrays() {
    m_impl->testInputRegs = nullptr;
    m_impl->testHoldingRegs = nullptr;
    m_impl->testMaxAddr = 0;
    m_impl->connected = false;
}

// ============================================================================
//  Internal
// ============================================================================

bool PsbModbusClient::checkConnected() {
    if (m_impl->testInputRegs || m_impl->testHoldingRegs) return true;
    if (!m_impl->connected || !m_impl->port) {
        m_impl->errorText = "not connected";
        return false;
    }
    return true;
}

namespace {
// RAII helper: temporarily overrides a ModbusPort's response timeout for the
// duration of a single request, restoring the previous value on scope exit
// (including early returns). No-op if port is null or override is negative.
class ScopedPortTimeout {
public:
    ScopedPortTimeout(ModbusPort* port, int overrideMs) : m_port(nullptr) {
        if (port && overrideMs >= 0) {
            m_port = port;
            m_saved = m_port->timeout();
            m_port->setTimeout(static_cast<uint32_t>(overrideMs));
        }
    }
    ~ScopedPortTimeout() {
        if (m_port) m_port->setTimeout(m_saved);
    }
    ScopedPortTimeout(const ScopedPortTimeout&) = delete;
    ScopedPortTimeout& operator=(const ScopedPortTimeout&) = delete;

private:
    ModbusPort* m_port;
    uint32_t m_saved = 0;
};
} // namespace

bool PsbModbusClient::readRegsInternal(bool holding, uint16_t addr, uint16_t count, uint16_t* out,
                                       int timeoutOverrideMs) {
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
    ScopedPortTimeout timeoutGuard(m_impl->port ? m_impl->port->port() : nullptr, timeoutOverrideMs);
    Modbus::StatusCode s;
    auto unit = static_cast<uint8_t>(m_impl->slaveId);
    // Non-blocking port: drive the request to completion. ModbusLib's internal
    // timeout/retry logic guarantees this terminates (Good or Bad). The sleep
    // is required, not cosmetic — see kNonBlockingPollInterval's comment.
    do {
        if (holding)
            s = m_impl->port->readHoldingRegisters(unit, addr, count, out);
        else
            s = m_impl->port->readInputRegisters(unit, addr, count, out);
        if (Modbus::StatusIsProcessing(s)) std::this_thread::sleep_for(kNonBlockingPollInterval);
    } while (Modbus::StatusIsProcessing(s));
    if (!Modbus::StatusIsGood(s)) {
        m_impl->errorText = std::string("Modbus error: ") + m_impl->port->lastErrorText();
        return false;
    }
    return true;
}

bool PsbModbusClient::writeRegsInternal(uint16_t addr, uint16_t count, const uint16_t* values,
                                         int timeoutOverrideMs) {
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
    ScopedPortTimeout timeoutGuard(m_impl->port ? m_impl->port->port() : nullptr, timeoutOverrideMs);
    auto unit = static_cast<uint8_t>(m_impl->slaveId);
    for (uint16_t i = 0; i < count; i++) {
        // Non-blocking port: poll until the write request completes. The
        // sleep is required, not cosmetic — see kNonBlockingPollInterval's
        // comment.
        Modbus::StatusCode s;
        do {
            s = m_impl->port->writeSingleRegister(unit, addr + i, values[i]);
            if (Modbus::StatusIsProcessing(s)) std::this_thread::sleep_for(kNonBlockingPollInterval);
        } while (Modbus::StatusIsProcessing(s));
        if (!Modbus::StatusIsGood(s)) {
            m_impl->errorText = std::string("Modbus error: ") + m_impl->port->lastErrorText();
            return false;
        }
    }
    return true;
}

// ============================================================================
//  Lightweight poll helpers — read only dynamic registers
// ============================================================================

bool PsbModbusClient::readSystemStatus(SystemInfo& info, int timeoutOverrideMs) {
    if (!checkConnected()) return false;

    /* Read the dynamic tail of the system input block:
       offsets 6..14 (9 registers) in one batch.
       Static fields at 10-11 (FW_VERSION) are read but discarded. */
    uint16_t buf[9] = {};
    if (!readRegsInternal(false, reg::sysAddr(SYS_BOARD_TEMPERATURE), 9, buf, timeoutOverrideMs)) return false;

    info.boardTempRaw     = static_cast<int16_t>(buf[SYS_BOARD_TEMPERATURE   - SYS_BOARD_TEMPERATURE]);
    info.boardHumidityRaw = buf[SYS_BOARD_HUMIDITY    - SYS_BOARD_TEMPERATURE];
    info.uptimeSec        = reg::uint32FromRegs(buf[SYS_UPTIME_HI - SYS_BOARD_TEMPERATURE],
                                                buf[SYS_UPTIME_LO - SYS_BOARD_TEMPERATURE]);
    info.activeOpMode     = static_cast<OpMode>(buf[SYS_ACTIVE_OPERATING_MODE - SYS_BOARD_TEMPERATURE]);
    info.sysStatus        = buf[SYS_STATUS              - SYS_BOARD_TEMPERATURE];
    info.faultCause       = buf[SYS_FAULT_CAUSE         - SYS_BOARD_TEMPERATURE];
    return true;
}

bool PsbModbusClient::readChannelStatus(int ch, uint16_t caps, ChannelInfo& info, int timeoutOverrideMs) {
    if (!checkConnected()) return false;

    uint16_t base = reg::chAddr(ch, 0);
    bool hasV = (caps & CH_CAP_VOLTAGE_MEASUREMENT) != 0;
    bool hasI = (caps & CH_CAP_CURRENT_MEASUREMENT) != 0;

    if (hasV && hasI) {
        uint16_t buf[12] = {};
        if (!readRegsInternal(false, base, 12, buf, timeoutOverrideMs)) return false;

        info.status                      = buf[CH_STATUS_BITS];
        info.activeFault                 = buf[CH_ACTIVE_FAULT_CAUSE];
        info.faultHistory                = buf[CH_FAULT_HISTORY_CAUSE];
        info.lastProtOutputAction        = buf[CH_LAST_PROT_OUT_ACTION];
        info.retryCount                  = static_cast<int>(buf[CH_AUTO_RETRY_COUNT]);
        info.cooldownSec                 = static_cast<int>(buf[CH_AUTO_COOLDOWN_REMAINING]);
        info.lastFaultTimestamp          = reg::uint32FromRegs(buf[CH_LAST_FAULT_TIMESTAMP_HI],
                                                               buf[CH_LAST_FAULT_TIMESTAMP_LO]);
        info.operationalTargetVoltageRaw = static_cast<int16_t>(buf[CH_OPER_TARGET_VOLTAGE]);
        info.chCapFlags = caps;
        info.voltageRaw = static_cast<int16_t>(buf[CH_MEASURED_VOLTAGE]);
        info.currentRaw = static_cast<int16_t>(buf[CH_MEASURED_CURRENT]);
        return true;
    }

    /* Read 9 runtime_state registers in one batch (offsets 0..8).
       Capability flags (offset 9) are hardware-fixed - use cached caps. */
    uint16_t buf[9] = {};
    if (!readRegsInternal(false, base, 9, buf, timeoutOverrideMs)) return false;

    info.status                      = buf[CH_STATUS_BITS];
    info.activeFault                 = buf[CH_ACTIVE_FAULT_CAUSE];
    info.faultHistory                = buf[CH_FAULT_HISTORY_CAUSE];
    info.lastProtOutputAction        = buf[CH_LAST_PROT_OUT_ACTION];
    info.retryCount                  = static_cast<int>(buf[CH_AUTO_RETRY_COUNT]);
    info.cooldownSec                 = static_cast<int>(buf[CH_AUTO_COOLDOWN_REMAINING]);
    info.lastFaultTimestamp          = reg::uint32FromRegs(buf[CH_LAST_FAULT_TIMESTAMP_HI],
                                                           buf[CH_LAST_FAULT_TIMESTAMP_LO]);
    info.operationalTargetVoltageRaw = static_cast<int16_t>(buf[CH_OPER_TARGET_VOLTAGE]);
    info.chCapFlags = caps;

    /* Measured voltage / current - conditional on channel capabilities. */
    if (hasV) {
        uint16_t v = 0;
        if (readRegsInternal(false, base + CH_MEASURED_VOLTAGE, 1, &v, timeoutOverrideMs))
            info.voltageRaw = static_cast<int16_t>(v);
    }
    if (hasI) {
        uint16_t c = 0;
        if (readRegsInternal(false, base + CH_MEASURED_CURRENT, 1, &c, timeoutOverrideMs))
            info.currentRaw = static_cast<int16_t>(c);
    }
    return true;
}

// ============================================================================
//  High-level reads — all single UINT16, no INT32 packing
// ============================================================================

SystemInfo PsbModbusClient::readSystemInfo() {
    SystemInfo info;
    if (!checkConnected()) return info;

    uint16_t buf[16] = {};
    if (!readRegsInternal(false, reg::sysAddr(0), 16, buf)) return info;

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
    // v3.2+: declares the decimal exponent for MEASURED_CURRENT/
    // CURRENT_LIMIT_THRESHOLD (10^exp amperes/LSB), board-specific. Offset 15
    // is "reserved, reads as 0" on a pre-v3.2 board (not a read failure — the
    // protocol convention is reserved registers read 0, not an exception), so
    // reading 0 there does NOT mean "0.1nA/LSB" for an old board; it means
    // "no such register." Gate on the protocol version, not the raw value.
    info.currentUnitExp = (info.protoMajor > 3 ||
                           (info.protoMajor == 3 && info.protoMinor >= 2))
        ? static_cast<int16_t>(buf[SYS_CURRENT_UNIT_EXP])
        : -10;
    m_impl->currentUnitExp = info.currentUnitExp;
    return info;
}

bool PsbModbusClient::readChannelCapabilities(int ch, uint16_t& caps, int timeoutOverrideMs) {
    if (!checkConnected()) return false;
    uint16_t v = 0;
    if (!readRegsInternal(false, reg::chAddr(ch, CH_CAPABILITY_FLAGS), 1, &v, timeoutOverrideMs)) return false;
    caps = v;
    return true;
}

ChannelInfo PsbModbusClient::readChannelInfo(int ch, uint16_t caps) {
    ChannelInfo info;
    if (!checkConnected()) return info;

    uint16_t base = reg::chAddr(ch, 0);

    if (caps == 0) {
        /* Full read — fetch capability flags from device (connect / Refresh). */
        uint16_t buf[10] = {};
        if (!readRegsInternal(false, base, 10, buf)) return info;

        info.status                      = buf[CH_STATUS_BITS];
        info.activeFault                 = buf[CH_ACTIVE_FAULT_CAUSE];
        info.faultHistory                = buf[CH_FAULT_HISTORY_CAUSE];
        info.lastProtOutputAction        = buf[CH_LAST_PROT_OUT_ACTION];
        info.retryCount                  = static_cast<int>(buf[CH_AUTO_RETRY_COUNT]);
        info.cooldownSec                 = static_cast<int>(buf[CH_AUTO_COOLDOWN_REMAINING]);
        info.lastFaultTimestamp          = reg::uint32FromRegs(buf[CH_LAST_FAULT_TIMESTAMP_HI],
                                                               buf[CH_LAST_FAULT_TIMESTAMP_LO]);
        info.operationalTargetVoltageRaw = static_cast<int16_t>(buf[CH_OPER_TARGET_VOLTAGE]);
        caps = buf[CH_CAPABILITY_FLAGS];
        info.chCapFlags = caps;
    } else {
        /* Poll read — caps are fixed hardware; use cached value, skip offset 9. */
        uint16_t buf[9] = {};
        if (!readRegsInternal(false, base, 9, buf)) return info;

        info.status                      = buf[CH_STATUS_BITS];
        info.activeFault                 = buf[CH_ACTIVE_FAULT_CAUSE];
        info.faultHistory                = buf[CH_FAULT_HISTORY_CAUSE];
        info.lastProtOutputAction        = buf[CH_LAST_PROT_OUT_ACTION];
        info.retryCount                  = static_cast<int>(buf[CH_AUTO_RETRY_COUNT]);
        info.cooldownSec                 = static_cast<int>(buf[CH_AUTO_COOLDOWN_REMAINING]);
        info.lastFaultTimestamp          = reg::uint32FromRegs(buf[CH_LAST_FAULT_TIMESTAMP_HI],
                                                               buf[CH_LAST_FAULT_TIMESTAMP_LO]);
        info.operationalTargetVoltageRaw = static_cast<int16_t>(buf[CH_OPER_TARGET_VOLTAGE]);
        info.chCapFlags = caps;
    }

    /* Measured voltage / current — conditional on channel capabilities.
       Reading offsets 10-11 without the matching cap causes the firmware
       to reject the entire batch with Exception 0x02. */
    bool hasV = (caps & CH_CAP_VOLTAGE_MEASUREMENT) != 0;
    bool hasI = (caps & CH_CAP_CURRENT_MEASUREMENT) != 0;

    if (hasV && hasI) {
        uint16_t m[2] = {};
        if (readRegsInternal(false, base + CH_MEASURED_VOLTAGE, 2, m)) {
            info.voltageRaw = static_cast<int16_t>(m[0]);
            info.currentRaw = static_cast<int16_t>(m[1]);
        }
    } else {
        if (hasV) {
            uint16_t v = 0;
            if (readRegsInternal(false, base + CH_MEASURED_VOLTAGE, 1, &v))
                info.voltageRaw = static_cast<int16_t>(v);
        }
        if (hasI) {
            uint16_t c = 0;
            if (readRegsInternal(false, base + CH_MEASURED_CURRENT, 1, &c))
                info.currentRaw = static_cast<int16_t>(c);
        }
    }

    /* rawAdcVoltage / rawAdcCurrent are calibration-mode-gated and belong
       in readCalibrationSnapshot(); not requested here. */
    return info;
}

SystemConfig PsbModbusClient::readSystemConfig() {
    SystemConfig cfg;
    if (!checkConnected()) return cfg;

    /* v3: sys holding 0-3: operating mode, startup policy, slave addr, baud */
    uint16_t buf[4] = {};
    if (!readRegsInternal(true, reg::sysAddr(0), 4, buf)) return cfg;

    cfg.operatingMode        = static_cast<OpMode>(buf[SYS_OPERATING_MODE]);
    cfg.startupChannelPolicy = buf[SYS_STARTUP_CHANNEL_POLICY];
    cfg.slaveAddr            = buf[SYS_SLAVE_ADDRESS];
    cfg.baudRateCode         = buf[SYS_BAUD_RATE_CODE];
    return cfg;
}

void PsbModbusClient::readChannelOutputBlock(int ch, uint16_t caps, ChannelConfig& out) {
    if (!checkConnected()) return;
    uint16_t base = reg::chAddr(ch, 0);
    bool haveDrive = (caps & CH_CAP_RAW_OUTPUT_DRIVE) != 0;

    /* Offsets 0-2 (commands) + optionally 3-7 (ramp/target, need RAW_OUTPUT_DRIVE) */
    if (haveDrive) {
        uint16_t buf[8] = {};
        if (!readRegsInternal(true, base, 8, buf)) return;
        out.outputAction         = static_cast<OutputAction>(buf[CH_OUTPUT_ACTION]);
        out.faultCommand         = static_cast<ChannelFaultCommand>(buf[CH_FAULT_CMD]);
        out.configuredTargetVRaw = static_cast<int16_t>(buf[CH_CFG_TARGET_VOLTAGE]);
        out.rampUpStepRaw        = buf[CH_RAMP_UP_STEP];
        out.rampUpInterval       = buf[CH_RAMP_UP_INTERVAL];
        out.rampDownStepRaw      = buf[CH_RAMP_DOWN_STEP];
        out.rampDownInterval     = buf[CH_RAMP_DOWN_INTERVAL];
    } else {
        uint16_t buf[3] = {};
        if (!readRegsInternal(true, base, 3, buf)) return;
        out.outputAction = static_cast<OutputAction>(buf[CH_OUTPUT_ACTION]);
        out.faultCommand = static_cast<ChannelFaultCommand>(buf[CH_FAULT_CMD]);
    }
}

void PsbModbusClient::readChannelRecoveryBlock(int ch, ChannelConfig& out) {
    if (!checkConnected()) return;
    uint16_t base = reg::chAddr(ch, 0);

    /* Offsets 8-12: recovery policy + retry + safe-band (always accessible) */
    uint16_t buf[5] = {};
    if (!readRegsInternal(true, base + CH_RECOVERY_POLICY_MODE, 5, buf)) return;
    out.recoveryPolicyMode = static_cast<RecoveryPolicy>(buf[CH_RECOVERY_POLICY_MODE - CH_RECOVERY_POLICY_MODE]);
    out.autoRetryDelay     = buf[CH_AUTO_RETRY_DELAY - CH_RECOVERY_POLICY_MODE];
    out.autoRetryMaxCount  = buf[CH_AUTO_RETRY_MAX_COUNT - CH_RECOVERY_POLICY_MODE];
    out.autoRetryWindow    = buf[CH_AUTO_RETRY_WINDOW - CH_RECOVERY_POLICY_MODE];
    out.currentSafeBandPct = buf[CH_CURRENT_SAFE_BAND_PCT - CH_RECOVERY_POLICY_MODE];
}

void PsbModbusClient::readChannelProtectionBlock(int ch, uint16_t caps, ChannelConfig& out) {
    if (!checkConnected()) return;
    bool haveI = (caps & CH_CAP_CURRENT_MEASUREMENT) != 0;
    if (!haveI) return;

    /* Offsets 13-15: current protection (need CURRENT_MEASUREMENT) */
    uint16_t base = reg::chAddr(ch, 0);
    uint16_t buf[3] = {};
    if (!readRegsInternal(true, base + CH_CURRENT_PROTECTION_MODE, 3, buf)) return;
    out.iProtMode          = static_cast<ProtectionMode>(buf[0]);
    out.iProtOutputAction  = static_cast<OutputAction>(buf[1]);
    out.iLimitThresholdRaw = static_cast<int16_t>(buf[2]);
}

void PsbModbusClient::readChannelDerateBlock(int ch, uint16_t caps, ChannelConfig& out) {
    if (!checkConnected()) return;
    bool haveDrive = (caps & CH_CAP_RAW_OUTPUT_DRIVE) != 0;
    bool haveV     = (caps & CH_CAP_VOLTAGE_MEASUREMENT) != 0;
    if (!(haveDrive && haveV)) return;

    /* Offset 16: auto-derate step (need RAW_OUTPUT_DRIVE + VOLTAGE_MEASUREMENT) */
    uint16_t base = reg::chAddr(ch, 0);
    uint16_t d = 0;
    if (readRegsInternal(true, base + CH_AUTO_DERATE_STEP, 1, &d))
        out.derateStepRaw = d;
}

void PsbModbusClient::readChannelOutputEnabledBlock(int ch, uint16_t caps, ChannelConfig& out) {
    if (!checkConnected()) return;
    bool haveDrive  = (caps & CH_CAP_RAW_OUTPUT_DRIVE) != 0;
    bool haveOutput = (caps & CH_CAP_OUTPUT_ENABLE) != 0;
    if (!(!haveDrive && haveOutput)) return;

    /* Offset 17: CFG_OUTPUT_ENABLED — fixed-voltage channels only (no DAC,
       but can be switched). Mutually exclusive with configuredTargetVRaw
       above; the firmware rejects this register on DAC channels and rejects
       CFG_TARGET_VOLTAGE on channels that lack RAW_OUTPUT_DRIVE. */
    uint16_t base = reg::chAddr(ch, 0);
    uint16_t e = 0;
    if (readRegsInternal(true, base + CH_CFG_OUTPUT_ENABLED, 1, &e))
        out.outputEnabledCfg = (e != 0);
}

void PsbModbusClient::readChannelConfig(int ch, uint16_t caps, ChannelConfig& out) {
    if (!checkConnected()) return;

    /* Acquire capability flags if caller didn't supply them. */
    if (caps == 0) {
        uint16_t base = reg::chAddr(ch, 0);
        if (!readRegsInternal(false, base + CH_CAPABILITY_FLAGS, 1, &caps))
            caps = 0;
    }

    readChannelOutputBlock(ch, caps, out);
    readChannelRecoveryBlock(ch, out);
    readChannelProtectionBlock(ch, caps, out);
    readChannelDerateBlock(ch, caps, out);
    readChannelOutputEnabledBlock(ch, caps, out);
}

ChannelConfig PsbModbusClient::readChannelConfig(int ch, uint16_t caps) {
    ChannelConfig cfg;
    readChannelConfig(ch, caps, cfg);
    return cfg;
}

void PsbModbusClient::readChannelCalConfig(int ch, uint16_t caps, ChannelCalConfig& out) {
    if (!checkConnected()) return;

    uint16_t base = reg::chAddr(ch, 0);

    /* Acquire capability flags if caller didn't supply them. */
    if (caps == 0) {
        if (!readRegsInternal(false, base + CH_CAPABILITY_FLAGS, 1, &caps))
            caps = 0;
    }

    bool haveDrive = (caps & CH_CAP_RAW_OUTPUT_DRIVE) != 0;
    bool haveV     = (caps & CH_CAP_VOLTAGE_MEASUREMENT) != 0;
    bool haveI     = (caps & CH_CAP_CURRENT_MEASUREMENT) != 0;

    /* Offsets 20-28: calibration coefficients (K/B pairs at 20-25, decimal
       exponents at 26-28 — contiguous, so the fast path below reads all 9 in
       one transaction). Each triplet (K, B, K_EXP) is gated by a different
       capability; read contiguous supported blocks. */
    if (haveDrive && haveV && haveI) {
        uint16_t buf[9] = {};
        if (readRegsInternal(true, base + CH_OUTPUT_CAL_K, 9, buf)) {
            out.outCalK      = buf[0];
            out.outCalB      = static_cast<int16_t>(buf[1]);
            out.measVCalK    = buf[2];
            out.measVCalB    = static_cast<int16_t>(buf[3]);
            out.measICalK    = buf[4];
            out.measICalB    = static_cast<int16_t>(buf[5]);
            out.outCalKExp   = static_cast<int16_t>(buf[6]);
            out.measVCalKExp = static_cast<int16_t>(buf[7]);
            out.measICalKExp = static_cast<int16_t>(buf[8]);
        }
        return;
    }

    if (haveDrive) {
        uint16_t buf[2] = {};
        if (readRegsInternal(true, base + CH_OUTPUT_CAL_K, 2, buf)) {
            out.outCalK = buf[0];
            out.outCalB = static_cast<int16_t>(buf[1]);
        }
        uint16_t expBuf = 0;
        if (readRegsInternal(true, base + CH_OUTPUT_CAL_K_EXP, 1, &expBuf)) {
            out.outCalKExp = static_cast<int16_t>(expBuf);
        }
    }
    if (haveV) {
        uint16_t buf[2] = {};
        if (readRegsInternal(true, base + CH_MEASURED_V_CAL_K, 2, buf)) {
            out.measVCalK = buf[0];
            out.measVCalB = static_cast<int16_t>(buf[1]);
        }
        uint16_t expBuf = 0;
        if (readRegsInternal(true, base + CH_MEASURED_V_CAL_K_EXP, 1, &expBuf)) {
            out.measVCalKExp = static_cast<int16_t>(expBuf);
        }
    }
    if (haveI) {
        uint16_t buf[2] = {};
        if (readRegsInternal(true, base + CH_MEASURED_I_CAL_K, 2, buf)) {
            out.measICalK = buf[0];
            out.measICalB = static_cast<int16_t>(buf[1]);
        }
        uint16_t expBuf = 0;
        if (readRegsInternal(true, base + CH_MEASURED_I_CAL_K_EXP, 1, &expBuf)) {
            out.measICalKExp = static_cast<int16_t>(expBuf);
        }
    }
}

ChannelCalConfig PsbModbusClient::readChannelCalConfig(int ch, uint16_t caps) {
    ChannelCalConfig cfg;
    readChannelCalConfig(ch, caps, cfg);
    return cfg;
}

// ============================================================================
//  High-level writes — system
// ============================================================================

#define WR1(addr,val)  writeRegsInternal(reg::sysAddr(addr), 1, (val))
#define WR(addr,n,val) writeRegsInternal(reg::sysAddr(addr), n, (val))

bool PsbModbusClient::writeOperatingMode(OpMode m) {
    uint16_t v = static_cast<uint16_t>(m);
    return WR1(SYS_OPERATING_MODE, &v);
}
bool PsbModbusClient::writeStartupChannelPolicy(uint16_t policy) {
    return WR1(SYS_STARTUP_CHANNEL_POLICY, &policy);
}
bool PsbModbusClient::writeSlaveAddress(uint16_t a)  { return WR1(SYS_SLAVE_ADDRESS, &a); }
bool PsbModbusClient::writeBaudRateCode(uint16_t c)   { return WR1(SYS_BAUD_RATE_CODE, &c); }
bool PsbModbusClient::sendParamAction(int chScope, ParamAction action) {
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

bool PsbModbusClient::writeConfiguredTargetVoltage(int ch, int16_t raw) {
    uint16_t v = static_cast<uint16_t>(raw);
    return CHW(CH_CFG_TARGET_VOLTAGE, &v);
}

bool PsbModbusClient::writeOutputEnabled(int ch, bool enabled) {
    uint16_t v = enabled ? 1 : 0;
    return CHW(CH_CFG_OUTPUT_ENABLED, &v);
}
bool PsbModbusClient::sendOutputAction(int ch, OutputAction action) {
    if (!isValidOutputAction(action, ActionContext::Host)) {
        m_impl->errorText = "invalid output action for host context";
        return false;
    }
    uint16_t v = static_cast<uint16_t>(action);
    return CHW(CH_OUTPUT_ACTION, &v);
}
bool PsbModbusClient::sendChannelFaultCommand(int ch, ChannelFaultCommand cmd) {
    uint16_t v = static_cast<uint16_t>(cmd);
    return CHW(CH_FAULT_CMD, &v);
}
bool PsbModbusClient::writeRampUp(int ch, uint16_t stepRaw, uint16_t interval) {
    uint16_t buf[2] = { stepRaw, interval };
    return CHWN(CH_RAMP_UP_STEP, 2, buf);
}
bool PsbModbusClient::writeRampDown(int ch, uint16_t stepRaw, uint16_t interval) {
    uint16_t buf[2] = { stepRaw, interval };
    return CHWN(CH_RAMP_DOWN_STEP, 2, buf);
}
bool PsbModbusClient::writeChannelRecovery(int ch, RecoveryPolicy policy, int delay, int max, int window) {
    uint16_t buf[4] = { static_cast<uint16_t>(policy), static_cast<uint16_t>(delay),
                        static_cast<uint16_t>(max),    static_cast<uint16_t>(window) };
    return CHWN(CH_RECOVERY_POLICY_MODE, 4, buf);
}
bool PsbModbusClient::writeChannelSafeBand(int ch, uint16_t pct) {
    return CHW(CH_CURRENT_SAFE_BAND_PCT, &pct);
}
bool PsbModbusClient::writeCurrentProtection(int ch, ProtectionMode mode, OutputAction action, int16_t thresholdRaw) {
    if (!isValidOutputAction(action, ActionContext::Protection)) {
        m_impl->errorText = "invalid output action for protection context";
        return false;
    }
    uint16_t buf[3] = { static_cast<uint16_t>(mode), static_cast<uint16_t>(action), static_cast<uint16_t>(thresholdRaw) };
    return CHWN(CH_CURRENT_PROTECTION_MODE, 3, buf);
}
bool PsbModbusClient::writeDerateStep(int ch, uint16_t stepRaw) {
    return CHW(CH_AUTO_DERATE_STEP, &stepRaw);
}
bool PsbModbusClient::writeCalibrationOutput(int ch, uint16_t k, int16_t b) {
    uint16_t buf[2] = { k, static_cast<uint16_t>(b) };
    return CHWN(CH_OUTPUT_CAL_K, 2, buf);
}
bool PsbModbusClient::writeCalibrationMeasV(int ch, uint16_t k, int16_t b) {
    uint16_t buf[2] = { k, static_cast<uint16_t>(b) };
    return CHWN(CH_MEASURED_V_CAL_K, 2, buf);
}
bool PsbModbusClient::writeCalibrationMeasI(int ch, uint16_t k, int16_t b) {
    uint16_t buf[2] = { k, static_cast<uint16_t>(b) };
    return CHWN(CH_MEASURED_I_CAL_K, 2, buf);
}
bool PsbModbusClient::writeCalibrationOutputExp(int ch, int16_t exp) {
    uint16_t reg = static_cast<uint16_t>(exp);
    return CHW(CH_OUTPUT_CAL_K_EXP, &reg);
}
bool PsbModbusClient::writeCalibrationMeasVExp(int ch, int16_t exp) {
    uint16_t reg = static_cast<uint16_t>(exp);
    return CHW(CH_MEASURED_V_CAL_K_EXP, &reg);
}
bool PsbModbusClient::writeCalibrationMeasIExp(int ch, int16_t exp) {
    uint16_t reg = static_cast<uint16_t>(exp);
    return CHW(CH_MEASURED_I_CAL_K_EXP, &reg);
}

#undef CHW
#undef CHWN

// ============================================================================
//  Calibration Mode operations (v2.1)
// ============================================================================

bool PsbModbusClient::unlockCalibrationStep(uint16_t value) {
    return writeRegsInternal(reg::extAddr(EXT_CAL_UNLOCK), 1, &value);
}

bool PsbModbusClient::enterCalibrationMode() {
    return writeOperatingMode(OpMode::Calibration);
}

bool PsbModbusClient::exitCalibrationMode() {
    uint16_t v = 1;
    return writeRegsInternal(reg::extAddr(EXT_CAL_EXIT), 1, &v);
}

bool PsbModbusClient::writeCalibrationOutputEnable(int ch, bool enable) {
    uint16_t v = enable ? 1 : 0;
    return writeRegsInternal(reg::chAddr(ch, CH_CAL_OUTPUT_ENABLE), 1, &v);
}

bool PsbModbusClient::writeRawDacCode(int ch, uint16_t code) {
    return writeRegsInternal(reg::chAddr(ch, CH_CAL_DAC_CODE), 1, &code);
}

bool PsbModbusClient::sendCalibrationSampleCommand(int ch) {
    // The firmware blocks its Modbus response for up to
    // CONFIG_VC_CAL_SAMPLE_TIMEOUT_MS (1000 ms default, see lib/voltage_control/
    // Kconfig) while it waits for a fresh ADC reading, which can exceed the
    // port's normal response timeout even though the device is healthy.
    // Scope a longer timeout to just this one request so every other command
    // keeps the fast default.
    constexpr int kCalSampleTimeoutMs = 2000;
    uint16_t v = CAL_COMMAND_EXECUTE;
    return writeRegsInternal(reg::chAddr(ch, CH_CAL_SAMPLE_CMD), 1, &v, kCalSampleTimeoutMs);
}

bool PsbModbusClient::sendCalibrationCommitCommand(int ch) {
    uint16_t v = CAL_COMMAND_EXECUTE;
    return writeRegsInternal(reg::chAddr(ch, CH_CAL_COMMIT_CMD), 1, &v);
}

CalibrationSnapshot PsbModbusClient::readCalibrationSnapshot(int ch) {
    CalibrationSnapshot snap;
    if (!checkConnected()) return snap;

    /* v3: 4 input regs for raw ADC; CH_CAL_SAMPLE_STATUS and CH_RAW_DAC_READBACK removed */
    uint16_t ibuf[4] = {};
    if (!readRegsInternal(false, reg::chAddr(ch, CH_RAW_ADC_VOLTAGE_HI), 4, ibuf)) return snap;

    snap.rawAdcVoltage  = reg::int32FromRegs(ibuf[0], ibuf[1]);
    snap.rawAdcCurrent  = reg::int32FromRegs(ibuf[2], ibuf[3]);

    /* offsets 30-33: CAL_OUTPUT_ENABLE, CAL_DAC_CODE, CAL_SAMPLE_CMD, CAL_COMMIT_CMD */
    uint16_t hbuf[4] = {};
    if (!readRegsInternal(true, reg::chAddr(ch, CH_CAL_OUTPUT_ENABLE), 4, hbuf)) return snap;

    snap.outputEnabled = hbuf[0] != 0;
    snap.rawDacCode    = hbuf[1];
    /* v3: rawDacReadback field removed — CAL_DAC_CODE (holding:31) is the readback register */
    return snap;
}

// ============================================================================
//  Low-level
// ============================================================================

bool PsbModbusClient::readInputRegs(uint16_t addr, uint16_t count, uint16_t* out) {
    return readRegsInternal(false, addr, count, out);
}
bool PsbModbusClient::readHoldingRegs(uint16_t addr, uint16_t count, uint16_t* out) {
    return readRegsInternal(true, addr, count, out);
}
bool PsbModbusClient::writeReg16(uint16_t addr, uint16_t value) {
    return writeRegsInternal(addr, 1, &value);
}

void PsbModbusClient::setFrameCallback(FrameCallback cb) {
    m_impl->frameCb = std::move(cb);
}

std::vector<std::string> PsbModbusClient::scanPorts() {
    std::vector<std::string> result;
    for (const auto& p : Modbus::availableSerialPorts()) {
        std::string path(p);
#if defined(_WIN32)
        if (path.rfind("COM", 0) == 0)
            result.push_back(path);
#else
        /* ttyUSB: USB-to-serial adapters (FTDI, CH340, etc.) using a
         * separate UART chip. ttyACM: USB CDC-ACM devices — boards with
         * native USB, and multi-port USB-serial adapters (e.g. WCH
         * "Quad_Serial") that enumerate as ACM rather than USB. Both are
         * real external serial links worth listing; ttyS* (onboard,
         * non-USB serial) stays excluded. */
        if (path.rfind("/dev/ttyUSB", 0) == 0 || path.rfind("/dev/ttyACM", 0) == 0)
            result.push_back(path);
#endif
    }
    return result;
}

std::vector<int> PsbModbusClient::availableBaudRates() {
    std::vector<int> result;
    for (auto r : Modbus::availableBaudRate())
        result.push_back(static_cast<int>(r));
    return result;
}

} // namespace psb
