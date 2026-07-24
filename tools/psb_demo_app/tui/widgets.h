#pragma once
#include "message_log.h"
#include "tui_format.h"
#include "psb_modbus_client.h"
#include "psb_board_session.h"
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace psb::tui {

using namespace ftxui;

struct AppState {
    psb::PsbBoardSession&                    client;
    std::atomic<bool>&                       connected;
    ScannedData&                             data;
    std::string&                             statusMsg;
    std::mutex&                              statusMutex;  // guards statusMsg
    psb::MessageCenter&                      messages;
    std::queue<std::function<void()>>&       workQueue;    // Modbus worker queue
    std::mutex&                              workMutex;    // guards workQueue
    std::condition_variable&                 workCv;       // wakes modbusWorker
    ftxui::ScreenInteractive&               screen;
};

// Shared config-input state — single source of truth for all tabs.
// Populated from ScannedData on connect/refresh/write-success.
// Each tab's Input/InlineCycler widgets bind to fields in this struct.
struct ConfigInputs {
    // Monitor table editable columns
    std::string targetV [MAX_CHANNELS];  // Vset  — configured target voltage in V — DAC channels
    int outputEnabledIdx[MAX_CHANNELS]{}; // Vset slot for fixed-voltage channels — 0=Off, 1=On
                                          // (CH_CAP_OUTPUT_ENABLE without CH_CAP_RAW_OUTPUT_DRIVE;
                                          // mutually exclusive with targetV — see tab_monitor.h)
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

// Maps a raw OutputAction ordinal to the 4-slot cycler index used by InlineCycler widgets.
// kIActVals = {None(0), DisGraceful(1), DisImm(2), ForceZero(3)} — Enable is excluded.
inline int outputActionToIdx(OutputAction a) {
    switch (a) {
    case OutputAction::DisableGraceful:  return 1;
    case OutputAction::DisableImmediate: return 2;
    case OutputAction::ForceOutputZero:  return 3;
    default:                             return 0;
    }
}

inline void syncDataToInputs(const ScannedData& data, ConfigInputs& cfg) {
    if (!data.valid) return;

    // System fields
    const auto& sc = data.sysCfg;
    cfg.slaveAddr  = std::to_string(sc.slaveAddr);
    cfg.opModeIdx  = std::min(static_cast<int>(sc.operatingMode), 1);  // kOpModes has 2 entries
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
        cfg.outputEnabledIdx[ch] = cc.outputEnabledCfg ? 1 : 0;
        {
            double v = reg::voltageToV(cc.rampUpStepRaw);
            char buf[16];
            snprintf(buf, sizeof(buf), "%.1f", v);
            cfg.ruStep[ch] = buf;
        }
        {
            double v = reg::voltageToV(cc.rampDownStepRaw);
            char buf[16];
            snprintf(buf, sizeof(buf), "%.1f", v);
            cfg.rdStep[ch] = buf;
        }
        {
            // In the board's fixed display unit (currentUnitFor), not plain
            // amperes — typing/reading raw amps was unusable for nA-scale
            // boards like jw_hvb (values like 0.0000000123). This must stay
            // in lockstep with the parse-back in tab_monitor.h/tab_channel.h,
            // which converts through the same currentUnitFor() scale.
            CurrentUnit u = currentUnitFor(data.sysInfo.currentUnitExp);
            double a = reg::currentToA(cc.iLimitThresholdRaw, data.sysInfo.currentUnitExp);
            char buf[24];
            snprintf(buf, sizeof(buf), "%.9g", a * u.scale);
            cfg.iThr[ch] = buf;
        }

        // Channel tab extras
        cfg.ruInt[ch]      = std::to_string(cc.rampUpInterval);
        cfg.rdInt[ch]      = std::to_string(cc.rampDownInterval);
        cfg.derateStep[ch] = std::to_string(cc.derateStepRaw);
        cfg.iModeIdx[ch]   = static_cast<int>(cc.iProtMode);
        cfg.iActIdx[ch]    = outputActionToIdx(cc.iProtOutputAction);
        cfg.recovIdx[ch]   = static_cast<int>(cc.recoveryPolicyMode);
        cfg.retryDelay[ch]  = std::to_string(cc.autoRetryDelay);
        cfg.retryMax[ch]    = std::to_string(cc.autoRetryMaxCount);
        cfg.retryWindow[ch] = std::to_string(cc.autoRetryWindow);
        cfg.iBand[ch]       = std::to_string(cc.currentSafeBandPct);
    }
}

// Non-blocking write — enqueues a work item on the Modbus worker thread and
// returns immediately so the FTXUI event loop is never stalled.
inline void postWrite(AppState& s, ConfigInputs& inputs,
                      const std::string& label,
                      std::function<bool()> writeFn,
                      std::function<void()> refreshFn) {
    // Show "Writing..." before the worker starts — UI thread is free to render it.
    uint64_t action = s.messages.beginAction("board", "Writing " + label + "...");
    { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg.clear(); }
    s.screen.PostEvent(Event::Custom);

    std::function<void()> item = [&s, &inputs, label, writeFn, refreshFn, action] {
        bool ok = writeFn();
        if (ok) refreshFn();
        if (ok) {
            syncDataToInputs(s.data, inputs);
            s.messages.publish(action, psb::MessageSeverity::Success, "board", "OK: " + label);
        } else {
            s.messages.publish(action, psb::MessageSeverity::Error, "board",
                               "Error: " + s.client.lastError());
        }
        s.screen.PostEvent(Event::Custom);
    };

    { std::lock_guard<std::mutex> lk(s.workMutex); s.workQueue.push(std::move(item)); }
    s.workCv.notify_one();
}

inline void publishActionStatus(AppState& s,
                                psb::MessageSeverity severity,
                                const std::string& text) {
    uint64_t action = s.messages.beginAction("board");
    s.messages.publish(action, severity, "board", text);
    { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg.clear(); }
    s.screen.PostEvent(Event::Custom);
}

inline void saveChannelConfig(AppState& s, ConfigInputs& inputs, int ch,
                              std::function<void()> refreshFn) {
    postWrite(s, inputs, "Save",
        [&s, ch] { return s.client.sendParamAction(ch, ParamAction::Save); },
        std::move(refreshFn));
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
// autoCommit=true: commits on every cycle event (click/Space/Arrow), not just Enter.
// Use for cyclers that should take effect immediately (e.g. mode selector in menu bar).
inline Component InlineCycler(std::vector<std::string> opts,
                               int* sel,
                               std::function<void()> onCommit,
                               bool autoCommit = false) {
    auto optsPtr = std::make_shared<std::vector<std::string>>(std::move(opts));
    auto bopt    = ButtonOption{};
    bopt.transform = [sel, optsPtr](const EntryState& es) -> Element {
        std::string lbl = "[" + optsPtr->at(*sel) + " \xe2\x96\xbe]"; // UTF-8 U+25BE ▾
        auto e = text(lbl);
        if (es.focused) e = e | inverted;
        return e;
    };
    // onClick: cycle forward (mouse-click support)
    auto btn = Button("", [sel, optsPtr, onCommit, autoCommit] {
        *sel = (*sel + 1) % static_cast<int>(optsPtr->size());
        if (autoCommit) onCommit();
    }, bopt);
    return CatchEvent(btn, [sel, optsPtr, onCommit, autoCommit](Event e) {
        int n = static_cast<int>(optsPtr->size());
        if (e == Event::Character(' ') || e == Event::ArrowRight) {
            *sel = (*sel + 1) % n;
            if (autoCommit) onCommit();
            return true;
        }
        if (e == Event::ArrowLeft) {
            *sel = (*sel - 1 + n) % n;
            if (autoCommit) onCommit();
            return true;
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

inline Component MouseOnlyActionButton(const std::string& label, std::function<void()> onClick) {
    return CatchEvent(ActionButton(label, std::move(onClick)), [](Event e) {
        return e == Event::Return;
    });
}

} // namespace psb::tui
