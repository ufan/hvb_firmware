#pragma once
#include "types.h"
#include "register_map.h"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <algorithm>
#include <ftxui/dom/elements.hpp>

namespace hvb::tui {

static constexpr int MAX_CHANNELS = static_cast<int>(hvb::reg::MAX_CHANNELS);

struct ScannedData {
    hvb::SystemInfo       sysInfo{};
    hvb::ChannelInfo      chInfo[MAX_CHANNELS]{};
    hvb::SystemConfig     sysCfg{};
    hvb::ChannelConfig    chCfg[MAX_CHANNELS]{};
    hvb::ChannelCalConfig chCalCfg[MAX_CHANNELS]{};
    std::atomic<bool> valid{false};

    // Number of channels to iterate over — 0 before first sysInfo read.
    int numChannels() const {
        int n = sysInfo.supportedChannels;
        return (n > 0 && n <= MAX_CHANNELS) ? n : 0;
    }
};

// Rolling time-series buffer — 5-minute window at ~1 sample/s
struct ChannelTimeSeries {
    static constexpr int WINDOW = 300;
    float vsetBuf [WINDOW]{};
    float vopBuf  [WINDOW]{};
    float vmeasBuf[WINDOW]{};
    float imeasBuf[WINDOW]{};
    int head  = 0;
    int count = 0;

    void sample(float vs, float vo, float vm, float im) {
        vsetBuf[head]  = vs;
        vopBuf[head]   = vo;
        vmeasBuf[head] = vm;
        imeasBuf[head] = im;
        head = (head + 1) % WINDOW;
        if (count < WINDOW) count++;
    }

    // Read current sample window (ordered oldest→newest) into caller-provided float* arrays.
    // Caller must ensure bufs each have enough room (count elements).
    void readWindow(float* vsetOut, float* vopOut, float* vmeasOut, float* imeasOut) const {
        int start = (head - count + WINDOW) % WINDOW;
        for (int i = 0; i < count; ++i) {
            int idx = (start + i) % WINDOW;
            if (vsetOut)  vsetOut[i]  = vsetBuf[idx];
            if (vopOut)   vopOut[i]   = vopBuf[idx];
            if (vmeasOut) vmeasOut[i] = vmeasBuf[idx];
            if (imeasOut) imeasOut[i] = imeasBuf[idx];
        }
    }
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

inline std::string fmtCurrentNA(int16_t raw) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%+.0f nA", hvb::reg::currentToA(raw) * 1e9);
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

// Render a multi-line time-series graph from a ring buffer.
// seriesMasks bits: 0=Vset, 1=Vop, 2=V, 3=I
inline ftxui::Element renderGraph(const ChannelTimeSeries& ts, uint8_t seriesMasks, int height = 6) {
    using namespace ftxui;
    if (ts.count < 2) return text(" collecting data... ") | dim | center;

    const int n = ts.count;
    int start = (ts.head - n + ts.WINDOW) % ts.WINDOW;

    // Collect selected series as float arrays + build graph callbacks
    struct SeriesRef { const float* buf; int count; int start; int window; };
    std::vector<SeriesRef> selected;
    if (seriesMasks & 1) selected.push_back({ts.vsetBuf,  n, start, ts.WINDOW});
    if (seriesMasks & 2) selected.push_back({ts.vopBuf,   n, start, ts.WINDOW});
    if (seriesMasks & 4) selected.push_back({ts.vmeasBuf, n, start, ts.WINDOW});
    if (seriesMasks & 8) selected.push_back({ts.imeasBuf, n, start, ts.WINDOW});
    if (selected.empty()) return text(" tick a checkbox to plot ") | dim | center;

    // Find global min/max
    float lo = 1e30f, hi = -1e30f;
    for (const auto& s : selected) {
        for (int i = 0; i < n; ++i) {
            float v = s.buf[(s.start + i) % s.window];
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
    }
    if (hi - lo < 1e-6f) { lo -= 1; hi += 1; }

    // Build a graph function per selected series
    auto mkFn = [=](const float* buf) {
        return [=](int w, int h) -> std::vector<int> {
            std::vector<int> out(w);
            float step = n > w ? static_cast<float>(n) / w : 1.0f;
            for (int x = 0; x < w; ++x) {
                int idx = static_cast<int>(x * step + 0.5f);
                if (idx >= n) idx = n - 1;
                float v = buf[(start + idx) % ts.WINDOW];
                int y = static_cast<int>((v - lo) / (hi - lo) * (h - 1) + 0.5f);
                if (y < 0) y = 0;
                if (y >= h) y = h - 1;
                out[x] = y;
            }
            return out;
        };
    };

    // graph() in FTXUI v5 accepts 1..5 callbacks; overlay extras via dbox
    Element g = graph(mkFn(selected[0].buf));
    for (size_t i = 1; i < selected.size() && i < 5; ++i)
        g = dbox({std::move(g), graph(mkFn(selected[i].buf))});
    return g | size(HEIGHT, EQUAL, height);
}

} // namespace hvb::tui
