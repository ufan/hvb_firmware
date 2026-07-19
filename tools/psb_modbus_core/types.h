#pragma once

#include <cstdint>
#include <cmath>

// Firmware system capability bits — from reg_map.h
#include "reg_store/reg_map.h"
// Channel capability bits — from capabilities.h
#include "dt-bindings/voltage_control/capabilities.h"

namespace psb {

// ============================================================================
//  Enumerations — synced with firmware include/voltage_control/vc_types.h
// ============================================================================

enum class OpMode : uint16_t {
    Normal      = 0,
    Automatic   = 1,
    Calibration = 2,
};

enum class OutputAction : uint16_t {
    None               = 0,
    Enable             = 1,
    DisableGraceful    = 2,
    DisableImmediate   = 3,
    ForceOutputZero    = 4,
};

enum class ChannelFaultCommand : uint16_t {
    None                  = 0,
    ClearActiveFaultBlock = 1,
    ClearFaultHistory     = 2,
};

enum class ParamAction : uint16_t {
    None          = 0,
    Save          = 1,
    Load          = 2,
    FactoryReset  = 3,
    SoftwareReset = 255,
};

enum class ProtectionMode : uint16_t {
    Disabled           = 0,
    FlagOnly           = 1,
    ApplyOutputAction  = 2,
};

enum class RecoveryPolicy : uint16_t {
    ManualLatch     = 0,
    AutoRetry       = 1,
    AutoDerateRetry = 2,
    NeverRetry      = 3,
};

// Firmware uses enums directly; keep ActionContext for tool-side validation
enum class ActionContext {
    Host,
    Protection,
};

// ============================================================================
//  Bitmask constants — synced with firmware include/voltage_control/vc_types.h
// ============================================================================

namespace ChStatus {
    // Firmware status_bits layout (computed in vc_channel_run)
    inline constexpr uint16_t OUTPUT_DRIVE_NONZERO = 0x0001;
    inline constexpr uint16_t OUTPUT_ENABLE_ACTIVE = 0x0002;
    inline constexpr uint16_t RAMPING_ACTIVE       = 0x0004;
    inline constexpr uint16_t ACTIVE_FAULT         = 0x0008;
    inline constexpr uint16_t FAULT_HISTORY        = 0x0010;
    inline constexpr uint16_t COOLDOWN_ACTIVE      = 0x0020;
    inline constexpr uint16_t MEASUREMENT_STALE    = 0x0040;
}

// Mirror of firmware VC_FAULT_* defines (include/voltage_control/vc_types.h:17-23)
namespace FaultCause {
    inline constexpr uint16_t CURRENT        = 0x0002;
    inline constexpr uint16_t MEASUREMENT    = 0x0004;
    inline constexpr uint16_t HARDWARE       = 0x0008;
    inline constexpr uint16_t INTERLOCK      = 0x0010;
    inline constexpr uint16_t RETRY_EXHAUST  = 0x0020;
    inline constexpr uint16_t CFG_INVALID    = 0x0040;
    inline constexpr uint16_t STALE          = 0x0080;
}

// Mirror of firmware reg_map.h SYS_CAP_* defines
namespace SysCap {
    inline constexpr uint16_t AUTOMATIC_MODE   = SYS_CAP_AUTOMATIC_MODE;
    inline constexpr uint16_t ENV_SENSOR       = SYS_CAP_ENV_SENSOR;
    inline constexpr uint16_t CALIBRATION_MODE = SYS_CAP_CALIBRATION_MODE;
}

// Channel capabilities now sourced directly from firmware
// dt-bindings/voltage_control/capabilities.h via CH_CAP_* defines.
// Use CH_CAP_OUTPUT_ENABLE, CH_CAP_RAW_OUTPUT_DRIVE,
// CH_CAP_VOLTAGE_MEASUREMENT, CH_CAP_CURRENT_MEASUREMENT,
// CH_CAP_HARDWARE_STATUS directly.

// ============================================================================
//  Value structs — raw LSB values
// ============================================================================

struct SystemInfo {
    int protoMajor = 0;
    int protoMinor = 0;
    int variantId = 0;
    int boardHwRevision = 0;  // v3.3+; 0 on older firmware (reserved reg reads 0)
    uint16_t sysCapFlags = 0;
    int supportedChannels = 0;
    uint16_t activeChMask = 0;
    int16_t boardTempRaw = 0;        // INT16, degC x10
    uint16_t boardHumidityRaw = 0;   // UINT16, %RH x10
    uint32_t uptimeSec = 0;
    uint32_t fwVersion = 0;
    OpMode activeOpMode = OpMode::Normal;
    uint16_t sysStatus = 0;
    uint16_t faultCause = 0;
    // Decimal exponent: MEASURED_CURRENT/CURRENT_LIMIT_THRESHOLD registers
    // are in units of 10^currentUnitExp amperes/LSB (v3.2+; -10 = the fixed
    // 0.1nA/LSB every board used before this field existed, and what a
    // pre-v3.2 board is assumed to be since it has no way to report otherwise).
    int16_t currentUnitExp = -10;
};

struct ChannelInfo {
    int16_t voltageRaw = 0;                      // INT16, raw LSB
    int16_t currentRaw = 0;                      // INT16, raw LSB
    int16_t operationalTargetVoltageRaw = 0;     // INT16, raw LSB
    uint16_t status = 0;
    uint16_t activeFault = 0;
    uint16_t faultHistory = 0;
    uint16_t lastProtOutputAction = 0;
    int retryCount = 0;
    int cooldownSec = 0;
    uint32_t lastFaultTimestamp = 0;
    uint16_t chCapFlags = 0;
    int32_t rawAdcVoltage = 0;
    int32_t rawAdcCurrent = 0;
    /* v3: CH_CAL_SAMPLE_STATUS and CH_RAW_DAC_READBACK removed from FC04 input regs */
};

struct SystemConfig {
    OpMode operatingMode = OpMode::Normal;
    uint16_t startupChannelPolicy = 0;  /* 0=load NVS op-config, 1=factory reset op-config */
    uint16_t slaveAddr = 1;
    uint16_t baudRateCode = 0;
    /* v3: recoveryPolicy/retryDelay/retryMax/retryWindow/safeBandPct moved to per-channel */
};

struct ChannelConfig {
    int16_t configuredTargetVRaw = 0;                // INT16, raw LSB — DAC channels only (CH_CAP_RAW_OUTPUT_DRIVE)
    bool outputEnabledCfg = false;                   // CFG_OUTPUT_ENABLED — fixed-voltage channels only
                                                       // (CH_CAP_OUTPUT_ENABLE, no CH_CAP_RAW_OUTPUT_DRIVE);
                                                       // startup/AUTOMATIC-mode desired on/off state
    OutputAction outputAction = OutputAction::None;
    ChannelFaultCommand faultCommand = ChannelFaultCommand::None;
    uint16_t rampUpStepRaw = 0;       // UINT16, raw LSB
    uint16_t rampUpInterval = 0;      // UINT16, seconds x10
    uint16_t rampDownStepRaw = 0;     // UINT16, raw LSB
    uint16_t rampDownInterval = 0;    // UINT16, seconds x10
    /* v3: recovery fields moved from system to per-channel */
    RecoveryPolicy recoveryPolicyMode = RecoveryPolicy::ManualLatch;
    uint16_t autoRetryDelay = 0;      // seconds
    uint16_t autoRetryMaxCount = 0;
    uint16_t autoRetryWindow = 0;     // seconds
    uint16_t currentSafeBandPct = 10;
    ProtectionMode iProtMode = ProtectionMode::Disabled;
    OutputAction iProtOutputAction = OutputAction::None;
    int16_t iLimitThresholdRaw = 0;   // INT16, raw LSB
    uint16_t derateStepRaw = 0;       // UINT16, raw LSB
    /* v3: vProtMode/vLimitThreshold removed; saveTargetPolicy removed */
    /* v3: cal coefficients moved to ChannelCalConfig; cal session fields in CalibrationSnapshot */
};

/* Calibration coefficients — separate read via readChannelCalConfig().
 * Gain is kExp*10^kExp (decimal floating-point), not a fixed divisor:
 * calibrated = raw * k * 10^kExp + b. Defaults below are placeholders
 * overwritten by the first readChannelCalConfig() call; they carry no
 * special "unity" meaning. */
struct ChannelCalConfig {
    uint16_t outCalK = 1;        // UINT16 mantissa
    int16_t outCalKExp = 0;      // INT16 decimal exponent, valid range [-9, 4]
    int16_t outCalB = 0;         // INT16, DAC counts offset
    uint16_t measVCalK = 1;      // UINT16 mantissa
    int16_t measVCalKExp = 0;    // INT16 decimal exponent, valid range [-9, 4]
    int16_t measVCalB = 0;       // INT16, x100 mV offset
    uint16_t measICalK = 1;      // UINT16 mantissa
    int16_t measICalKExp = 0;    // INT16 decimal exponent, valid range [-9, 4]
    int16_t measICalB = 0;       // INT16, x0.1 nA offset
};

struct CalibrationSnapshot {
    bool outputEnabled = false;
    uint16_t rawDacCode = 0;
    int32_t rawAdcVoltage = 0;
    int32_t rawAdcCurrent = 0;
    /* v3: rawDacReadback = CAL_DAC_CODE holding reg (FC03), no separate input reg */
    /* v3: cal_sample_status removed */
    /* max DAC limit is firmware-internal session state, not a Modbus register */
};

// ============================================================================
//  Output action context validation
// ============================================================================

inline bool isValidOutputAction(OutputAction action, ActionContext ctx) {
    switch (action) {
    case OutputAction::None:
    case OutputAction::DisableGraceful:
    case OutputAction::DisableImmediate:
        return true;
    case OutputAction::Enable:
        return ctx == ActionContext::Host;
    case OutputAction::ForceOutputZero:
        return ctx == ActionContext::Protection;
    }
    return false;
}

// ============================================================================
//  String conversion helpers
// ============================================================================

inline const char* opModeName(OpMode m) {
    switch (m) {
    case OpMode::Normal:      return "Normal";
    case OpMode::Automatic:   return "Automatic";
    case OpMode::Calibration: return "Calibration";
    }
    return "?";
}

inline const char* outputActionName(OutputAction a) {
    switch (a) {
    case OutputAction::None:             return "None";
    case OutputAction::Enable:           return "Enable";
    case OutputAction::DisableGraceful:  return "DisableGraceful";
    case OutputAction::DisableImmediate: return "DisableImmediate";
    case OutputAction::ForceOutputZero:  return "ForceZero";
    }
    return "?";
}

inline const char* faultCommandName(ChannelFaultCommand c) {
    switch (c) {
    case ChannelFaultCommand::None:                  return "None";
    case ChannelFaultCommand::ClearActiveFaultBlock: return "ClearActive";
    case ChannelFaultCommand::ClearFaultHistory:     return "ClearHistory";
    }
    return "?";
}

inline const char* protectionModeName(ProtectionMode m) {
    switch (m) {
    case ProtectionMode::Disabled:          return "Disabled";
    case ProtectionMode::FlagOnly:          return "FlagOnly";
    case ProtectionMode::ApplyOutputAction: return "ApplyAction";
    }
    return "?";
}

inline const char* recoveryPolicyName(RecoveryPolicy p) {
    switch (p) {
    case RecoveryPolicy::ManualLatch:     return "ManualLatch";
    case RecoveryPolicy::AutoRetry:       return "AutoRetry";
    case RecoveryPolicy::AutoDerateRetry: return "AutoDerate";
    case RecoveryPolicy::NeverRetry:      return "NeverRetry";
    }
    return "?";
}

inline const char* paramActionName(ParamAction a) {
    switch (a) {
    case ParamAction::None:          return "None";
    case ParamAction::Save:          return "Save";
    case ParamAction::Load:          return "Load";
    case ParamAction::FactoryReset:  return "FactoryReset";
    case ParamAction::SoftwareReset: return "SoftwareReset";
    }
    return "?";
}

} // namespace psb
