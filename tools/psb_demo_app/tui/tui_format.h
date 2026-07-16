#pragma once
#include "types.h"
#include "register_map.h"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

namespace psb::tui {

static constexpr int MAX_CHANNELS = static_cast<int>(psb::reg::MAX_CHANNELS);

struct ScannedData {
    psb::SystemInfo       sysInfo{};
    psb::ChannelInfo      chInfo[MAX_CHANNELS]{};
    psb::SystemConfig     sysCfg{};
    psb::ChannelConfig    chCfg[MAX_CHANNELS]{};
    psb::ChannelCalConfig chCalCfg[MAX_CHANNELS]{};
    std::atomic<bool> valid{false};

    // Number of channels to iterate over — 0 before first sysInfo read.
    int numChannels() const {
        int n = sysInfo.supportedChannels;
        return (n > 0 && n <= MAX_CHANNELS) ? n : 0;
    }
};

inline std::string fmtVoltage(int16_t raw) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%+.1f V", psb::reg::voltageToV(raw));
    return buf;
}

// unitExp is the board's declared decimal exponent (SystemInfo::currentUnitExp
// / PsbModbusClient::currentUnitExp()) — a hardcoded "uA"/"nA" label is wrong
// for any board that isn't jw_hvb-style nA-scale (e.g. jw_lvb's real load
// currents are amp-scale), so this auto-selects nA/uA/mA/A per formatAmpsAuto().
inline std::string fmtCurrentAuto(int16_t raw, int16_t unitExp) {
    return psb::reg::formatAmpsAuto(psb::reg::currentToA(raw, unitExp));
}

inline std::string fmtInterval(uint16_t raw) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%.1f s", psb::reg::intervalToS(raw));
    return buf;
}

inline std::string statusBadge(uint16_t status) {
    using namespace psb::ChStatus;
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
    if (fault & psb::FaultCause::CURRENT)        s += "CL ";
    if (fault & psb::FaultCause::MEASUREMENT)    s += "MI ";
    if (fault & psb::FaultCause::HARDWARE)       s += "HW ";
    if (fault & psb::FaultCause::INTERLOCK)      s += "IL ";
    if (fault & psb::FaultCause::RETRY_EXHAUST)  s += "RE ";
    if (fault & psb::FaultCause::CFG_INVALID)    s += "CI ";
    if (fault & psb::FaultCause::STALE)          s += "ST ";
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

} // namespace psb::tui
