#pragma once
#include "tui_format.h"
#include "hvb_modbus_client.h"
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hvb::tui {

using namespace ftxui;

struct AppState {
    hvb::HvbModbusClient&     client;
    std::atomic<bool>&        connected;
    ScannedData&              data;
    std::string&              statusMsg;
    std::mutex&               statusMutex;  // guards statusMsg cross-thread writes
    std::mutex&               scanMutex;    // serialises all Modbus transactions
    ftxui::ScreenInteractive& screen;
};

// Shared config-input state — single source of truth for all tabs.
// Populated from ScannedData on connect/refresh/write-success.
// Each tab's Input/InlineCycler widgets bind to fields in this struct.
struct ConfigInputs {
    // Monitor table editable columns
    std::string targetV [MAX_CHANNELS];  // Vset  — configured target voltage in V
    std::string ruStep  [MAX_CHANNELS];  // Ramp↑ — ramp-up step raw LSB
    std::string rdStep  [MAX_CHANNELS];  // Ramp↓ — ramp-down step raw LSB
    std::string iThr    [MAX_CHANNELS];  // I-limit — current threshold in nA

    // System tab
    std::string slaveAddr;
    int opModeIdx       = 0;
    int baudIdx         = 0;
    int startupIdx      = 0;

    // Channel tab extra fields
    std::string ruInt      [MAX_CHANNELS];
    std::string rdInt      [MAX_CHANNELS];
    std::string derateStep [MAX_CHANNELS];
    int iModeIdx           [MAX_CHANNELS]{};
    int iActIdx            [MAX_CHANNELS]{};
    int recovIdx           [MAX_CHANNELS]{};
    std::string retryDelay  [MAX_CHANNELS];
    std::string retryMax    [MAX_CHANNELS];
    std::string retryWindow [MAX_CHANNELS];
    std::string iBand       [MAX_CHANNELS];
};

inline void syncDataToInputs(const ScannedData& data, ConfigInputs& cfg) {
    if (!data.valid) return;

    // System fields
    const auto& sc = data.sysCfg;
    cfg.slaveAddr  = std::to_string(sc.slaveAddr);
    cfg.opModeIdx  = static_cast<int>(sc.operatingMode);
    cfg.baudIdx    = static_cast<int>(sc.baudRateCode);
    cfg.startupIdx = static_cast<int>(sc.startupChannelPolicy);

    // Per-channel fields
    int n = data.numChannels();
    for (int ch = 0; ch < n; ++ch) {
        const auto& cc = data.chCfg[ch];

        // Monitor editable
        {
            double v = reg::voltageToV(cc.configuredTargetVRaw);
            char buf[16];
            snprintf(buf, sizeof(buf), "%+.1f", v);
            cfg.targetV[ch] = buf;
        }
        cfg.ruStep[ch] = std::to_string(cc.rampUpStepRaw);
        cfg.rdStep[ch] = std::to_string(cc.rampDownStepRaw);
        {
            double a = reg::currentToA(cc.iLimitThresholdRaw);
            char buf[16];
            snprintf(buf, sizeof(buf), "%.3f", a * 1e9);
            cfg.iThr[ch] = buf;
        }

        // Channel tab extras
        cfg.ruInt[ch]      = std::to_string(cc.rampUpInterval);
        cfg.rdInt[ch]      = std::to_string(cc.rampDownInterval);
        cfg.derateStep[ch] = std::to_string(cc.derateStepRaw);
        cfg.iModeIdx[ch]   = static_cast<int>(cc.iProtMode);
        cfg.iActIdx[ch]    = static_cast<int>(cc.iProtOutputAction);
        cfg.recovIdx[ch]   = static_cast<int>(cc.recoveryPolicyMode);
        cfg.retryDelay[ch]  = std::to_string(cc.autoRetryDelay);
        cfg.retryMax[ch]    = std::to_string(cc.autoRetryMaxCount);
        cfg.retryWindow[ch] = std::to_string(cc.autoRetryWindow);
        cfg.iBand[ch]       = std::to_string(cc.currentSafeBandPct);
    }
}

// Blocking write — acquires scanMutex, runs writeFn, reads back config on success.
inline void writeSync(AppState& s, ConfigInputs& inputs,
                      const std::string& label,
                      std::function<bool()> writeFn,
                      std::function<void()> refreshFn) {
    { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Writing " + label + "..."; }
    s.screen.PostEvent(Event::Custom);

    bool ok;
    { std::lock_guard<std::mutex> lk(s.scanMutex); ok = writeFn(); }

    if (ok) {
        refreshFn();
        syncDataToInputs(s.data, inputs);
        { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "OK: " + label; }
    } else {
        { std::lock_guard<std::mutex> lk(s.statusMutex);
          s.statusMsg = "Error: " + s.client.lastError(); }
    }
    s.screen.PostEvent(Event::Custom);
}



// Input that calls onCommit when Enter is pressed (instead of inserting newline).
inline Component CommitInput(std::string* val,
                             const std::string& placeholder,
                             std::function<void()> onCommit) {
    auto inp = Input(val, placeholder);
    return CatchEvent(inp, [onCommit](Event e) {
        if (e == Event::Return) { onCommit(); return true; }
        return false;
    });
}

// Button-backed inline dropdown.
// Renders as "[current ▾]". Space/←/→ cycles; Enter commits.
// Uses Button so it participates in FTXUI's Tab-focus chain.
inline Component InlineCycler(std::vector<std::string> opts,
                               int* sel,
                               std::function<void()> onCommit) {
    auto optsPtr = std::make_shared<std::vector<std::string>>(std::move(opts));
    auto bopt    = ButtonOption{};
    bopt.transform = [sel, optsPtr](const EntryState& es) -> Element {
        std::string lbl = "[" + optsPtr->at(*sel) + " \xe2\x96\xbe]"; // UTF-8 U+25BE ▾
        auto e = text(lbl);
        if (es.focused) e = e | inverted;
        return e;
    };
    // onClick: cycle forward (mouse-click support)
    auto btn = Button("", [sel, optsPtr] { *sel = (*sel + 1) % static_cast<int>(optsPtr->size()); }, bopt);
    return CatchEvent(btn, [sel, optsPtr, onCommit](Event e) {
        int n = static_cast<int>(optsPtr->size());
        if (e == Event::Character(' ') || e == Event::ArrowRight) {
            *sel = (*sel + 1) % n; return true;
        }
        if (e == Event::ArrowLeft) {
            *sel = (*sel - 1 + n) % n; return true;
        }
        if (e == Event::Return) { onCommit(); return true; }
        return false;
    });
}

// Styled action button: "[ label ]", inverted when focused.
inline Component ActionButton(const std::string& label, std::function<void()> onClick) {
    auto bopt = ButtonOption{};
    bopt.transform = [](const EntryState& es) -> Element {
        auto e = text("[ " + es.label + " ]");
        if (es.focused) e = e | inverted;
        return e;
    };
    return Button(label, std::move(onClick), bopt);
}

} // namespace hvb::tui
