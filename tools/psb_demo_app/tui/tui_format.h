#pragma once
#include "types.h"
#include "register_map.h"
#include <atomic>
#include <chrono>
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

    // Timestamp of the last successful readSystemStatus() (uptime/temp/
    // humidity) — set at connect time and on every successful poll (see
    // doFullScan()/doPollScan() in tui/main.cpp). Lets the menu bar's
    // breathing connection indicator distinguish "still connected, still
    // getting live data" from "board went unresponsive (e.g. powered off)
    // but nothing ever called Disconnect" — g_connected alone can't tell
    // those apart, since it only flips on an explicit user action.
    std::chrono::steady_clock::time_point lastSysUpdate{};

    // Number of channels to iterate over — 0 before first sysInfo read.
    int numChannels() const {
        int n = sysInfo.supportedChannels;
        return (n > 0 && n <= MAX_CHANNELS) ? n : 0;
    }

    // True once longer than `threshold` has passed since the last successful
    // system-status poll — the same "stale means treat as gone" idea as
    // kChannelOfflineThreshold, but time-based rather than a consecutive-
    // failure count. A fixed count doesn't translate to one fixed duration
    // here: doPollScan's per-cycle cost scales with channel count (each
    // failing read pays its own timeout), so 5 consecutive failures is
    // ~1.5s on a 2-channel board but ~15s on a 10-channel one. Wall-clock
    // time since the last successful update is the same regardless of
    // channel count.
    bool sysStale(std::chrono::milliseconds threshold) const {
        return std::chrono::steady_clock::now() - lastSysUpdate > threshold;
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

// Same value as fmtVoltage(), no unit suffix — for columns whose header
// already carries the unit (Monitor's "V (V)"), so it isn't repeated in
// every cell.
inline std::string fmtVoltageBare(int16_t raw) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%+.1f", psb::reg::voltageToV(raw));
    return buf;
}

// unitExp is the board's declared decimal exponent (SystemInfo::currentUnitExp
// / PsbModbusClient::currentUnitExp()) — a hardcoded "uA"/"nA" label is wrong
// for any board that isn't jw_hvb-style nA-scale (e.g. jw_lvb's real load
// currents are amp-scale), so this auto-selects nA/uA/mA/A per formatAmpsAuto().
inline std::string fmtCurrentAuto(int16_t raw, int16_t unitExp) {
    return psb::reg::formatAmpsAuto(psb::reg::currentToA(raw, unitExp));
}

// A single fixed nA/uA/mA/A label + scale for the whole board, derived from
// its declared LSB magnitude (10^unitExp amperes/LSB) rather than any one
// reading's magnitude. Unlike fmtCurrentAuto()/formatAmpsAuto() — which
// auto-range per value and so can pick a different unit for two channels on
// the same board — this gives one unit that's valid for every cell in a
// column, so it can live in the column header instead of every cell. Same
// bucket thresholds as formatAmpsAuto(), just applied to the unit itself.
struct CurrentUnit { const char* label; double scale; };  // scale: amps -> displayed unit

inline CurrentUnit currentUnitFor(int16_t unitExp) {
    double mag = std::pow(10.0, unitExp);
    if (mag >= 1.0)       return {"A",  1.0};
    if (mag >= 1e-3)      return {"mA", 1e3};
    if (mag >= 1e-6)      return {"uA", 1e6};
    return {"nA", 1e9};
}

// Current value in the board's fixed unit (see currentUnitFor), number only
// — for columns whose header already carries the unit label.
inline std::string fmtCurrentBare(int16_t raw, int16_t unitExp) {
    CurrentUnit u = currentUnitFor(unitExp);
    double amps = psb::reg::currentToA(raw, unitExp);
    char buf[24];
    snprintf(buf, sizeof(buf), "%+.1f", amps * u.scale);
    return buf;
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
