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
    // Per-channel connect-scan progress. Publishing is atomic — all flip
    // from false to true together once the whole sweep finishes (see
    // doFullScan() in tui/main.cpp) — Monitor shows a single "Scanning..."
    // message via allChannelsLoaded() below rather than revealing rows one
    // at a time, which was confusing (looked like a partial/inconsistent
    // table rather than a still-loading one).
    bool chLoaded[MAX_CHANNELS]{};
    // Recovery-policy and derate-step fields (ChannelConfig) are shown only
    // on the Channel tab, never on Monitor — deferred out of the connect
    // scan and lazily fetched the first time that channel's tab is opened
    // (see tab_channel.h) to keep connect-time down to just what Monitor
    // actually displays.
    bool chDetailLoaded[MAX_CHANNELS]{};
    // How many channels doFullScan has finished reading so far — drives the
    // "Scanning channels... X/N" message while a connect scan is in flight.
    int scanProgress{0};
    // Consecutive failed status-poll count per channel, and whether it has
    // crossed the offline threshold (see doPollScan() in tui/main.cpp) —
    // Monitor renders an OFFLINE row instead of stale live values once set.
    int  chPollFailCount[MAX_CHANNELS]{};
    bool chOffline[MAX_CHANNELS]{};
    std::atomic<bool> valid{false};

    // Number of channels to iterate over — 0 before first sysInfo read.
    int numChannels() const {
        int n = sysInfo.supportedChannels;
        return (n > 0 && n <= MAX_CHANNELS) ? n : 0;
    }

    bool allChannelsLoaded() const {
        int n = numChannels();
        if (n == 0) return false;
        for (int ch = 0; ch < n; ++ch)
            if (!chLoaded[ch]) return false;
        return true;
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
