#pragma once

#include <cstdint>
#include <cmath>

namespace hvb {

// ============================================================================
//  Enumerations (from ref/modbus_interface.md §8)
// ============================================================================

enum class OpMode : uint16_t {
    Normal    = 0,
    Automatic = 1,
};

enum class OutputAction : uint16_t {
    None               = 0,
    Enable             = 1,
    DisableGraceful    = 2,
    DisableImmediate   = 3,
    ForceOutputZero    = 4,
    Clamp              = 5,
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

enum class ActionContext {
    Host,
    VoltageProtection,
    CurrentProtection,
};

// ============================================================================
//  Bitmask constants
// ============================================================================

namespace ChStatus {
    inline constexpr uint16_t OUTPUT_DRIVE_NONZERO = 0x0001;
    inline constexpr uint16_t OUTPUT_ENABLE_ACTIVE = 0x0002;
    inline constexpr uint16_t RAMPING_ACTIVE       = 0x0004;
    inline constexpr uint16_t ACTIVE_FAULT         = 0x0008;
    inline constexpr uint16_t FAULT_HISTORY        = 0x0010;
    inline constexpr uint16_t COOLDOWN_ACTIVE      = 0x0020;
    inline constexpr uint16_t RETRY_EXHAUSTED      = 0x0040;
    inline constexpr uint16_t UNSUPPORTED          = 0x0080;
}

namespace FaultCause {
    inline constexpr uint16_t VOLTAGE_LIMIT         = 0x0001;
    inline constexpr uint16_t CURRENT_LIMIT         = 0x0002;
    inline constexpr uint16_t MEASUREMENT_INVALID   = 0x0004;
    inline constexpr uint16_t OUTPUT_HW_FAULT       = 0x0008;
    inline constexpr uint16_t VARIANT_INTERLOCK     = 0x0010;
    inline constexpr uint16_t AUTO_RETRY_EXHAUSTED  = 0x0020;
    inline constexpr uint16_t CONFIG_INVALID_AUTO   = 0x0040;
}

namespace SysCap {
    inline constexpr uint16_t AUTO_MODE_SUPPORTED = 0x0001;
    inline constexpr uint16_t ENV_SENSOR_PRESENT  = 0x0002;
}

namespace ChCap {
    inline constexpr uint16_t OUTPUT_ENABLE_CTRL = 0x0001;
    inline constexpr uint16_t CURRENT_MEAS       = 0x0002;
    inline constexpr uint16_t AUTO_RECOVERY      = 0x0004;
}

// ============================================================================
//  Value structs — raw LSB values
// ============================================================================

struct SystemInfo {
    int protoMajor = 0;
    int protoMinor = 0;
    int variantId = 0;
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
};

struct SystemConfig {
    OpMode operatingMode = OpMode::Normal;
    uint16_t slaveAddr = 1;
    uint16_t baudRateCode = 0;
    RecoveryPolicy recoveryPolicy = RecoveryPolicy::ManualLatch;
    int retryDelay = 0;
    int retryMax = 0;
    int retryWindow = 0;
    uint16_t voltageSafeBandPct = 10;
    uint16_t currentSafeBandPct = 10;
};

struct ChannelConfig {
    int16_t configuredTargetVRaw = 0;                // INT16, raw LSB
    OutputAction outputAction = OutputAction::None;
    ChannelFaultCommand faultCommand = ChannelFaultCommand::None;
    uint16_t rampUpStepRaw = 0;       // UINT16, raw LSB
    uint16_t rampUpInterval = 0;      // UINT16, seconds x10
    uint16_t rampDownStepRaw = 0;     // UINT16, raw LSB
    uint16_t rampDownInterval = 0;    // UINT16, seconds x10
    ProtectionMode vProtMode = ProtectionMode::Disabled;
    OutputAction vProtOutputAction = OutputAction::None;
    int16_t vLimitThresholdRaw = 0;   // INT16, raw LSB
    ProtectionMode iProtMode = ProtectionMode::Disabled;
    OutputAction iProtOutputAction = OutputAction::None;
    int16_t iLimitThresholdRaw = 0;   // INT16, raw LSB
    uint16_t derateStepRaw = 0;       // UINT16, raw LSB
    bool saveTargetPolicy = false;
    uint16_t outCalK = 10000;          // UINT16, x10000
    int16_t outCalB = 0;               // INT16, x1000
    uint16_t measVCalK = 10000;
    int16_t measVCalB = 0;
    uint16_t measICalK = 10000;
    int16_t measICalB = 0;
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
        return ctx != ActionContext::Host;
    case OutputAction::Clamp:
        return ctx == ActionContext::VoltageProtection;
    }
    return false;
}

// ============================================================================
//  String conversion helpers
// ============================================================================

inline const char* opModeName(OpMode m) {
    switch (m) {
    case OpMode::Normal:    return "Normal";
    case OpMode::Automatic: return "Automatic";
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
    case OutputAction::Clamp:            return "Clamp";
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

} // namespace hvb
