#include "psb_board_session.h"
#include "psb_serial_bus.h"
#include "register_map.h"

namespace psb {

struct PsbBoardSession::Impl {
    std::shared_ptr<PsbSerialBus> bus;
    int slaveId = 1;
    bool verified = false;
    std::string errorText;
    int16_t currentUnitExp = -10;  // see PsbBoardSession::currentUnitExp()
};

PsbBoardSession::PsbBoardSession(std::shared_ptr<PsbSerialBus> bus, int slaveId)
    : m_impl(std::make_unique<Impl>()) {
    m_impl->bus = std::move(bus);
    m_impl->slaveId = slaveId;
}
PsbBoardSession::~PsbBoardSession() = default;

// ============================================================================
//  Connection
// ============================================================================

// Reads PROTOCOL_MAJOR/MINOR (input offsets 0-1) in one transaction and
// validates compatibility — moved here verbatim from
// PsbModbusClient::connect(), which used to run this inline right after
// opening the port. Now decoupled from opening the port at all: the bus is
// already connected (or in test mode) by the time this runs.
bool PsbBoardSession::verifyProtocol() {
    uint16_t probe[2] = {0xFFFF, 0xFFFF};
    if (!readRegsInternal(false, reg::sysAddr(0), 2, probe)) {
        m_impl->errorText = "no response from board — check baud rate, slave ID, and cabling";
        m_impl->verified = false;
        return false;
    }
    int protoMajor = static_cast<int>(probe[0]);
    int protoMinor = static_cast<int>(probe[1]);
    if (protoMajor < 1 || protoMajor > 15) {
        m_impl->errorText = "unexpected protocol version " + std::to_string(protoMajor)
                            + " — check baud rate and slave ID";
        m_impl->verified = false;
        return false;
    }
    // Real compatibility gate (design spec §6): refuse to operate, not just
    // warn, on a protocol mismatch — an exact major mismatch means this
    // client cannot correctly speak the wire format at all, and a lower
    // minor means the firmware predates a register this client may rely on.
    if (!reg::protocolCompatible(protoMajor, protoMinor)) {
        m_impl->errorText = "firmware protocol v" + std::to_string(protoMajor) + "."
                            + std::to_string(protoMinor) + " is incompatible with this tool "
                            + "(requires v" + std::to_string(VC_PROTOCOL_MAJOR) + "."
                            + std::to_string(VC_PROTOCOL_MINOR) + " or newer, same major version)";
        m_impl->verified = false;
        return false;
    }
    m_impl->verified = true;
    return true;
}

void PsbBoardSession::disconnect() { m_impl->verified = false; }
void PsbBoardSession::rebind(int slaveId) { m_impl->slaveId = slaveId; m_impl->verified = false; }
bool PsbBoardSession::isConnected() const { return m_impl->verified; }
std::string PsbBoardSession::lastError() const { return m_impl->errorText; }
int PsbBoardSession::slaveId() const { return m_impl->slaveId; }
int16_t PsbBoardSession::currentUnitExp() const { return m_impl->currentUnitExp; }

// Bypasses verifyProtocol() entirely, matching PsbModbusClient's existing
// contract (attachTestArrays() has always marked a client connected
// immediately, with no probe).
void PsbBoardSession::attachTestArrays(uint16_t* inputRegs, uint16_t* holdingRegs, int maxAddr) {
    m_impl->bus->attachTestArrays(m_impl->slaveId, inputRegs, holdingRegs, maxAddr);
    m_impl->verified = true;
}

void PsbBoardSession::detachTestArrays() {
    m_impl->bus->detachTestArrays(m_impl->slaveId);
    m_impl->verified = false;
}

// ============================================================================
//  Internal
// ============================================================================

// A cheap early bail-out before attempting a doomed transaction — not the
// only safety net. readRegsInternal/writeRegsInternal below still fail
// cleanly (via the bus's own "not connected" check) even without this,
// since they always go through m_impl->bus regardless. The one imprecision
// here — this checks *the bus's* connectivity, not specifically whether
// *this session's* slave ID has anything listening — is harmless: an
// unconfigured session sharing a bus with a verified one would still get a
// correct "not connected" a layer deeper, from the bus's own per-slave-ID
// lookup finding nothing.
bool PsbBoardSession::checkConnected() {
    if (!m_impl->bus || !m_impl->bus->isConnected()) {
        m_impl->errorText = "not connected";
        return false;
    }
    return true;
}

bool PsbBoardSession::readRegsInternal(bool holding, uint16_t addr, uint16_t count, uint16_t* out,
                                       int timeoutOverrideMs) {
    if (!checkConnected()) return false;
    bool ok = holding
        ? m_impl->bus->readHoldingRegs(m_impl->slaveId, addr, count, out, timeoutOverrideMs)
        : m_impl->bus->readInputRegs(m_impl->slaveId, addr, count, out, timeoutOverrideMs);
    if (!ok) m_impl->errorText = m_impl->bus->lastError();
    return ok;
}

bool PsbBoardSession::writeRegsInternal(uint16_t addr, uint16_t count, const uint16_t* values,
                                        int timeoutOverrideMs) {
    if (!checkConnected()) return false;
    bool ok = m_impl->bus->writeRegs(m_impl->slaveId, addr, count, values, timeoutOverrideMs);
    if (!ok) m_impl->errorText = m_impl->bus->lastError();
    return ok;
}

// ============================================================================
//  Lightweight poll helpers — read only dynamic registers
// ============================================================================

bool PsbBoardSession::readSystemStatus(SystemInfo& info, int timeoutOverrideMs) {
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

bool PsbBoardSession::readChannelStatus(int ch, uint16_t caps, ChannelInfo& info, int timeoutOverrideMs) {
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

void PsbBoardSession::readSystemInfo(SystemInfo& out) {
    if (!checkConnected()) return;

    uint16_t buf[17] = {};
    if (!readRegsInternal(false, reg::sysAddr(0), 17, buf)) return;

    out.protoMajor       = static_cast<int>(buf[SYS_PROTOCOL_MAJOR]);
    out.protoMinor       = static_cast<int>(buf[SYS_PROTOCOL_MINOR]);
    out.variantId        = static_cast<int>(buf[SYS_VARIANT_ID]);
    out.boardHwRevision  = static_cast<int>(buf[SYS_BOARD_HW_REVISION]);
    out.sysCapFlags      = buf[SYS_CAPABILITY_FLAGS];
    out.supportedChannels = static_cast<int>(buf[SYS_SUPPORTED_CHANNELS]);
    out.activeChMask     = buf[SYS_ACTIVE_CHANNEL_MASK];
    out.boardTempRaw     = static_cast<int16_t>(buf[SYS_BOARD_TEMPERATURE]);
    out.boardHumidityRaw = buf[SYS_BOARD_HUMIDITY];
    out.uptimeSec        = reg::uint32FromRegs(buf[SYS_UPTIME_HI], buf[SYS_UPTIME_LO]);
    out.fwVersion        = reg::uint32FromRegs(buf[SYS_FW_VERSION_HI], buf[SYS_FW_VERSION_LO]);
    out.activeOpMode     = static_cast<OpMode>(buf[SYS_ACTIVE_OPERATING_MODE]);
    out.sysStatus        = buf[SYS_STATUS];
    out.faultCause       = buf[SYS_FAULT_CAUSE];
    // v3.2+: declares the decimal exponent for MEASURED_CURRENT/
    // CURRENT_LIMIT_THRESHOLD (10^exp amperes/LSB), board-specific. Offset 15
    // is "reserved, reads as 0" on a pre-v3.2 board (not a read failure — the
    // protocol convention is reserved registers read 0, not an exception), so
    // reading 0 there does NOT mean "0.1nA/LSB" for an old board; it means
    // "no such register." Gate on the protocol version, not the raw value.
    out.currentUnitExp = (out.protoMajor > 3 ||
                          (out.protoMajor == 3 && out.protoMinor >= 2))
        ? static_cast<int16_t>(buf[SYS_CURRENT_UNIT_EXP])
        : -10;
    m_impl->currentUnitExp = out.currentUnitExp;
}

SystemInfo PsbBoardSession::readSystemInfo() {
    SystemInfo info;
    readSystemInfo(info);
    return info;
}

bool PsbBoardSession::readChannelCapabilities(int ch, uint16_t& caps, int timeoutOverrideMs) {
    if (!checkConnected()) return false;
    uint16_t v = 0;
    if (!readRegsInternal(false, reg::chAddr(ch, CH_CAPABILITY_FLAGS), 1, &v, timeoutOverrideMs)) return false;
    caps = v;
    return true;
}

void PsbBoardSession::readChannelInfo(int ch, uint16_t caps, ChannelInfo& out) {
    if (!checkConnected()) return;

    uint16_t base = reg::chAddr(ch, 0);

    if (caps == 0) {
        /* Full read — fetch capability flags from device (connect / Refresh).
           On failure, fall back to whatever caps `out` already carries (0 on
           a first-ever connect attempt, a real value on a later Refresh) so
           the measurement-gating below still has something sane to work
           with instead of silently gating everything off. */
        uint16_t buf[10] = {};
        if (readRegsInternal(false, base, 10, buf)) {
            out.status                      = buf[CH_STATUS_BITS];
            out.activeFault                 = buf[CH_ACTIVE_FAULT_CAUSE];
            out.faultHistory                = buf[CH_FAULT_HISTORY_CAUSE];
            out.lastProtOutputAction        = buf[CH_LAST_PROT_OUT_ACTION];
            out.retryCount                  = static_cast<int>(buf[CH_AUTO_RETRY_COUNT]);
            out.cooldownSec                 = static_cast<int>(buf[CH_AUTO_COOLDOWN_REMAINING]);
            out.lastFaultTimestamp          = reg::uint32FromRegs(buf[CH_LAST_FAULT_TIMESTAMP_HI],
                                                                   buf[CH_LAST_FAULT_TIMESTAMP_LO]);
            out.operationalTargetVoltageRaw = static_cast<int16_t>(buf[CH_OPER_TARGET_VOLTAGE]);
            caps = buf[CH_CAPABILITY_FLAGS];
            out.chCapFlags = caps;
        } else {
            caps = out.chCapFlags;
        }
    } else {
        /* Poll read — caps are fixed hardware; use cached value, skip offset 9. */
        uint16_t buf[9] = {};
        if (readRegsInternal(false, base, 9, buf)) {
            out.status                      = buf[CH_STATUS_BITS];
            out.activeFault                 = buf[CH_ACTIVE_FAULT_CAUSE];
            out.faultHistory                = buf[CH_FAULT_HISTORY_CAUSE];
            out.lastProtOutputAction        = buf[CH_LAST_PROT_OUT_ACTION];
            out.retryCount                  = static_cast<int>(buf[CH_AUTO_RETRY_COUNT]);
            out.cooldownSec                 = static_cast<int>(buf[CH_AUTO_COOLDOWN_REMAINING]);
            out.lastFaultTimestamp          = reg::uint32FromRegs(buf[CH_LAST_FAULT_TIMESTAMP_HI],
                                                                   buf[CH_LAST_FAULT_TIMESTAMP_LO]);
            out.operationalTargetVoltageRaw = static_cast<int16_t>(buf[CH_OPER_TARGET_VOLTAGE]);
            out.chCapFlags = caps;
        }
    }

    /* Measured voltage / current — conditional on channel capabilities.
       Reading offsets 10-11 without the matching cap causes the firmware
       to reject the entire batch with Exception 0x02. */
    bool hasV = (caps & CH_CAP_VOLTAGE_MEASUREMENT) != 0;
    bool hasI = (caps & CH_CAP_CURRENT_MEASUREMENT) != 0;

    if (hasV && hasI) {
        uint16_t m[2] = {};
        if (readRegsInternal(false, base + CH_MEASURED_VOLTAGE, 2, m)) {
            out.voltageRaw = static_cast<int16_t>(m[0]);
            out.currentRaw = static_cast<int16_t>(m[1]);
        }
    } else {
        if (hasV) {
            uint16_t v = 0;
            if (readRegsInternal(false, base + CH_MEASURED_VOLTAGE, 1, &v))
                out.voltageRaw = static_cast<int16_t>(v);
        }
        if (hasI) {
            uint16_t c = 0;
            if (readRegsInternal(false, base + CH_MEASURED_CURRENT, 1, &c))
                out.currentRaw = static_cast<int16_t>(c);
        }
    }

    /* rawAdcVoltage / rawAdcCurrent are calibration-mode-gated and belong
       in readCalibrationSnapshot(); not requested here. */
}

ChannelInfo PsbBoardSession::readChannelInfo(int ch, uint16_t caps) {
    ChannelInfo info;
    readChannelInfo(ch, caps, info);
    return info;
}

SystemConfig PsbBoardSession::readSystemConfig() {
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

void PsbBoardSession::readChannelOutputBlock(int ch, uint16_t caps, ChannelConfig& out) {
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

void PsbBoardSession::readChannelRecoveryBlock(int ch, ChannelConfig& out) {
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

void PsbBoardSession::readChannelProtectionBlock(int ch, uint16_t caps, ChannelConfig& out) {
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

void PsbBoardSession::readChannelDerateBlock(int ch, uint16_t caps, ChannelConfig& out) {
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

void PsbBoardSession::readChannelOutputEnabledBlock(int ch, uint16_t caps, ChannelConfig& out) {
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

void PsbBoardSession::readChannelConfig(int ch, uint16_t caps, ChannelConfig& out) {
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

ChannelConfig PsbBoardSession::readChannelConfig(int ch, uint16_t caps) {
    ChannelConfig cfg;
    readChannelConfig(ch, caps, cfg);
    return cfg;
}

void PsbBoardSession::readChannelCalConfig(int ch, uint16_t caps, ChannelCalConfig& out) {
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

ChannelCalConfig PsbBoardSession::readChannelCalConfig(int ch, uint16_t caps) {
    ChannelCalConfig cfg;
    readChannelCalConfig(ch, caps, cfg);
    return cfg;
}

// ============================================================================
//  High-level writes — system
// ============================================================================

#define WR1(addr,val)  writeRegsInternal(reg::sysAddr(addr), 1, (val))
#define WR(addr,n,val) writeRegsInternal(reg::sysAddr(addr), n, (val))

bool PsbBoardSession::writeOperatingMode(OpMode m) {
    uint16_t v = static_cast<uint16_t>(m);
    return WR1(SYS_OPERATING_MODE, &v);
}
bool PsbBoardSession::writeStartupChannelPolicy(uint16_t policy) {
    return WR1(SYS_STARTUP_CHANNEL_POLICY, &policy);
}
bool PsbBoardSession::writeSlaveAddress(uint16_t a)  { return WR1(SYS_SLAVE_ADDRESS, &a); }
bool PsbBoardSession::writeBaudRateCode(uint16_t c)   { return WR1(SYS_BAUD_RATE_CODE, &c); }
bool PsbBoardSession::sendParamAction(int chScope, ParamAction action) {
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

bool PsbBoardSession::writeConfiguredTargetVoltage(int ch, int16_t raw) {
    uint16_t v = static_cast<uint16_t>(raw);
    return CHW(CH_CFG_TARGET_VOLTAGE, &v);
}

bool PsbBoardSession::writeOutputEnabled(int ch, bool enabled) {
    uint16_t v = enabled ? 1 : 0;
    return CHW(CH_CFG_OUTPUT_ENABLED, &v);
}
bool PsbBoardSession::sendOutputAction(int ch, OutputAction action) {
    if (!isValidOutputAction(action, ActionContext::Host)) {
        m_impl->errorText = "invalid output action for host context";
        return false;
    }
    uint16_t v = static_cast<uint16_t>(action);
    return CHW(CH_OUTPUT_ACTION, &v);
}
bool PsbBoardSession::sendChannelFaultCommand(int ch, ChannelFaultCommand cmd) {
    uint16_t v = static_cast<uint16_t>(cmd);
    return CHW(CH_FAULT_CMD, &v);
}
bool PsbBoardSession::writeRampUp(int ch, uint16_t stepRaw, uint16_t interval) {
    uint16_t buf[2] = { stepRaw, interval };
    return CHWN(CH_RAMP_UP_STEP, 2, buf);
}
bool PsbBoardSession::writeRampDown(int ch, uint16_t stepRaw, uint16_t interval) {
    uint16_t buf[2] = { stepRaw, interval };
    return CHWN(CH_RAMP_DOWN_STEP, 2, buf);
}
bool PsbBoardSession::writeChannelRecovery(int ch, RecoveryPolicy policy, int delay, int max, int window) {
    uint16_t buf[4] = { static_cast<uint16_t>(policy), static_cast<uint16_t>(delay),
                        static_cast<uint16_t>(max),    static_cast<uint16_t>(window) };
    return CHWN(CH_RECOVERY_POLICY_MODE, 4, buf);
}
bool PsbBoardSession::writeChannelSafeBand(int ch, uint16_t pct) {
    return CHW(CH_CURRENT_SAFE_BAND_PCT, &pct);
}
bool PsbBoardSession::writeCurrentProtection(int ch, ProtectionMode mode, OutputAction action, int16_t thresholdRaw) {
    if (!isValidOutputAction(action, ActionContext::Protection)) {
        m_impl->errorText = "invalid output action for protection context";
        return false;
    }
    uint16_t buf[3] = { static_cast<uint16_t>(mode), static_cast<uint16_t>(action), static_cast<uint16_t>(thresholdRaw) };
    return CHWN(CH_CURRENT_PROTECTION_MODE, 3, buf);
}
bool PsbBoardSession::writeDerateStep(int ch, uint16_t stepRaw) {
    return CHW(CH_AUTO_DERATE_STEP, &stepRaw);
}
bool PsbBoardSession::writeCalibrationOutput(int ch, uint16_t k, int16_t b) {
    uint16_t buf[2] = { k, static_cast<uint16_t>(b) };
    return CHWN(CH_OUTPUT_CAL_K, 2, buf);
}
bool PsbBoardSession::writeCalibrationMeasV(int ch, uint16_t k, int16_t b) {
    uint16_t buf[2] = { k, static_cast<uint16_t>(b) };
    return CHWN(CH_MEASURED_V_CAL_K, 2, buf);
}
bool PsbBoardSession::writeCalibrationMeasI(int ch, uint16_t k, int16_t b) {
    uint16_t buf[2] = { k, static_cast<uint16_t>(b) };
    return CHWN(CH_MEASURED_I_CAL_K, 2, buf);
}
bool PsbBoardSession::writeCalibrationOutputExp(int ch, int16_t exp) {
    uint16_t reg = static_cast<uint16_t>(exp);
    return CHW(CH_OUTPUT_CAL_K_EXP, &reg);
}
bool PsbBoardSession::writeCalibrationMeasVExp(int ch, int16_t exp) {
    uint16_t reg = static_cast<uint16_t>(exp);
    return CHW(CH_MEASURED_V_CAL_K_EXP, &reg);
}
bool PsbBoardSession::writeCalibrationMeasIExp(int ch, int16_t exp) {
    uint16_t reg = static_cast<uint16_t>(exp);
    return CHW(CH_MEASURED_I_CAL_K_EXP, &reg);
}

#undef CHW
#undef CHWN

// ============================================================================
//  Calibration Mode operations (v2.1)
// ============================================================================

bool PsbBoardSession::unlockCalibrationStep(uint16_t value) {
    return writeRegsInternal(reg::extAddr(EXT_CAL_UNLOCK), 1, &value);
}

bool PsbBoardSession::enterCalibrationMode() {
    return writeOperatingMode(OpMode::Calibration);
}

bool PsbBoardSession::exitCalibrationMode() {
    uint16_t v = 1;
    return writeRegsInternal(reg::extAddr(EXT_CAL_EXIT), 1, &v);
}

bool PsbBoardSession::writeCalibrationOutputEnable(int ch, bool enable) {
    uint16_t v = enable ? 1 : 0;
    return writeRegsInternal(reg::chAddr(ch, CH_CAL_OUTPUT_ENABLE), 1, &v);
}

bool PsbBoardSession::writeRawDacCode(int ch, uint16_t code) {
    return writeRegsInternal(reg::chAddr(ch, CH_CAL_DAC_CODE), 1, &code);
}

bool PsbBoardSession::sendCalibrationSampleCommand(int ch) {
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

bool PsbBoardSession::sendCalibrationCommitCommand(int ch) {
    uint16_t v = CAL_COMMAND_EXECUTE;
    return writeRegsInternal(reg::chAddr(ch, CH_CAL_COMMIT_CMD), 1, &v);
}

CalibrationSnapshot PsbBoardSession::readCalibrationSnapshot(int ch) {
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

bool PsbBoardSession::readInputRegs(uint16_t addr, uint16_t count, uint16_t* out) {
    return readRegsInternal(false, addr, count, out);
}
bool PsbBoardSession::readHoldingRegs(uint16_t addr, uint16_t count, uint16_t* out) {
    return readRegsInternal(true, addr, count, out);
}
bool PsbBoardSession::writeReg16(uint16_t addr, uint16_t value) {
    return writeRegsInternal(addr, 1, &value);
}

} // namespace psb
