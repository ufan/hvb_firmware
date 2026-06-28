#pragma once
#include "types.h"
#include "register_map.h"
#include <cstdint>
#include <cstdio>
#include <string>

namespace hvb::tui {

struct ScannedData {
    hvb::SystemInfo      sysInfo{};
    hvb::ChannelInfo     chInfo[2]{};
    hvb::SystemConfig    sysCfg{};
    hvb::ChannelConfig   chCfg[2]{};
    hvb::ChannelCalConfig chCalCfg[2]{};
    bool valid = false;
};

inline std::string fmtVoltage(int16_t raw) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%+.1f V", hvb::reg::voltageToV(raw));
    return buf;
}

inline std::string fmtCurrentUA(int16_t raw) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%+.3f uA", hvb::reg::currentToA(raw) * 1e6);
    return buf;
}

inline std::string fmtInterval(uint16_t raw) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%.1f s", hvb::reg::intervalToS(raw));
    return buf;
}

inline std::string statusBadge(uint16_t status) {
    using namespace hvb::ChStatus;
    if (status & ACTIVE_FAULT)       return "FAULT";
    if (status & COOLDOWN_ACTIVE)    return "COOL";
    if (status & MEASUREMENT_STALE)  return "STALE";
    bool on   = (status & OUTPUT_DRIVE_NONZERO) != 0;
    bool ramp = (status & RAMPING_ACTIVE) != 0;
    if (on && ramp) return "ON RAMP";
    if (on)         return "ON";
    if (ramp)       return "RAMP";
    return "OFF";
}

inline std::string faultStr(uint16_t fault) {
    if (!fault) return "--";
    std::string s;
    if (fault & hvb::FaultCause::CURRENT)        s += "CL ";
    if (fault & hvb::FaultCause::MEASUREMENT)    s += "MI ";
    if (fault & hvb::FaultCause::HARDWARE)       s += "HW ";
    if (fault & hvb::FaultCause::INTERLOCK)      s += "IL ";
    if (fault & hvb::FaultCause::RETRY_EXHAUST)  s += "RE ";
    if (fault & hvb::FaultCause::CFG_INVALID)    s += "CI ";
    if (fault & hvb::FaultCause::STALE)          s += "ST ";
    if (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

inline std::string protCompact(ProtectionMode mode, OutputAction action) {
    switch (mode) {
    case ProtectionMode::Disabled:          return "Disabled";
    case ProtectionMode::FlagOnly:          return "FlagOnly";
    case ProtectionMode::ApplyOutputAction: break;
    }
    switch (action) {
    case OutputAction::DisableGraceful:  return "Apply/Dis-Gr";
    case OutputAction::DisableImmediate: return "Apply/Dis-Im";
    case OutputAction::ForceOutputZero:  return "Apply/Force0";
    default:                             return "Apply/None";
    }
}

} // namespace hvb::tui
