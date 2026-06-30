#pragma once
#include "widgets.h"
#include <algorithm>
#include <memory>
#include <string>

namespace hvb::tui {

// Build one row of the Monitor table for channel `ch`.
// Returns a Renderer wrapping a Container::Horizontal of interactive widgets.
// Invisible rows (ch >= numChannels) catch all events and render as empty.
inline Component makeMonitorRow(AppState& s, ConfigInputs& inputs, int ch) {
    auto refreshCh = [&s, &inputs, ch]() {
        uint16_t caps = s.data.chInfo[ch].chCapFlags;
        s.data.chCfg[ch] = s.client.readChannelConfig(ch, caps);
        s.data.chCalCfg[ch] = s.client.readChannelCalConfig(ch, caps);
        syncDataToInputs(s.data, inputs);
    };

    // ---- Status toggle button ----
    auto bopt = ButtonOption{};
    bopt.transform = [&s, ch](const EntryState& es) -> Element {
        uint16_t st = s.data.valid ? s.data.chInfo[ch].status : 0;
        bool on = (st & ChStatus::OUTPUT_DRIVE_NONZERO) != 0;
        std::string lbl = on ? "[ ON ]" : "[ OFF ]";
        auto e = text(lbl);
        if (on)      e = e | color(Color::Green) | bold;
        else         e = e | dim;
        if (es.focused) e = e | inverted;
        return e;
    };
    auto statusBtn = Button("", [&s, &inputs, ch, refreshCh] {
        if (!s.data.valid) return;
        uint16_t st = s.data.chInfo[ch].status;
        bool on = (st & ChStatus::OUTPUT_DRIVE_NONZERO) != 0;
        OutputAction act = on ? OutputAction::DisableGraceful : OutputAction::Enable;
        std::string lbl = on ? "Dis-Grace" : "Enable";
        writeSync(s, inputs, lbl,
            [&s, ch, act] { return s.client.sendOutputAction(ch, act); },
            refreshCh);
    }, bopt);

    // ---- Vset Input ----
    auto onVset = [&s, &inputs, ch, refreshCh] {
        try {
            auto raw = reg::voltageFromV(std::stod(inputs.targetV[ch]));
            writeSync(s, inputs, "Target V",
                [&s, ch, raw] { return s.client.writeConfiguredTargetVoltage(ch, raw); },
                refreshCh);
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid voltage"; }
    };
    auto vsetInp = CommitInput(&inputs.targetV[ch], "+0.0", onVset);

    // ---- Ramp Up Input ----
    auto onRampUp = [&s, &inputs, ch, refreshCh] {
        try {
            auto step = (uint16_t)std::stoul(inputs.ruStep[ch]);
            writeSync(s, inputs, "Ramp Up",
                [&s, ch, step] { return s.client.writeRampUp(ch, step, s.data.chCfg[ch].rampUpInterval); },
                refreshCh);
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid ramp-up value"; }
    };
    auto rampUpInp = CommitInput(&inputs.ruStep[ch], "0", onRampUp);

    // ---- Ramp Down Input ----
    auto onRampDown = [&s, &inputs, ch, refreshCh] {
        try {
            auto step = (uint16_t)std::stoul(inputs.rdStep[ch]);
            writeSync(s, inputs, "Ramp Down",
                [&s, ch, step] { return s.client.writeRampDown(ch, step, s.data.chCfg[ch].rampDownInterval); },
                refreshCh);
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid ramp-down value"; }
    };
    auto rampDownInp = CommitInput(&inputs.rdStep[ch], "0", onRampDown);

    // ---- I-limit Input ----
    auto onILimit = [&s, &inputs, ch, refreshCh] {
        try {
            auto mode   = static_cast<ProtectionMode>(inputs.iModeIdx[ch]);
            auto action = static_cast<OutputAction>(inputs.iActIdx[ch]);
            auto raw    = static_cast<int16_t>(std::stod(inputs.iThr[ch]) * 1000.0 + 0.5);
            writeSync(s, inputs, "I Limit",
                [&s, ch, mode, action, raw] { return s.client.writeCurrentProtection(ch, mode, action, raw); },
                refreshCh);
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid I-limit value"; }
    };
    auto iLimitInp = CommitInput(&inputs.iThr[ch], "0.000", onILimit);

    // ---- Kill button (DisableImmediate) ----
    auto kopt = ButtonOption{};
    kopt.transform = [](const EntryState& es) -> Element {
        auto e = text("Kill");
        e = e | color(Color::Red);
        if (es.focused) e = e | inverted;
        return e;
    };
    auto killBtn = Button("", [&s, &inputs, ch, refreshCh] {
        if (!s.data.valid) return;
        writeSync(s, inputs, "Kill",
            [&s, ch] { return s.client.sendOutputAction(ch, OutputAction::DisableImmediate); },
            refreshCh);
    }, kopt);

    // ---- Horizontal container of all interactive widgets ----
    auto rowWidgets = Container::Horizontal({
        statusBtn, vsetInp, rampUpInp, rampDownInp, iLimitInp, killBtn,
    });

    return Renderer(rowWidgets, [=, &s]() mutable {
        // Return empty element for invisible rows (ch >= numChannels or not connected)
        bool show = s.data.valid && ch < s.data.numChannels();
        if (!show) return emptyElement();

        const auto& ci = s.data.chInfo[ch];
        const uint16_t caps = ci.chCapFlags;
        bool hasV = (caps & CH_CAP_VOLTAGE_MEASUREMENT) != 0;
        bool hasI = (caps & CH_CAP_CURRENT_MEASUREMENT) != 0;
        bool hasOut = (caps & CH_CAP_OUTPUT_ENABLE) != 0;

        char chLabel[8];
        snprintf(chLabel, sizeof(chLabel), "CH%-2d", ch);

        auto chT  = text(chLabel)                               | size(WIDTH, EQUAL, 4);
        auto vmT  = hasV ? text(fmtVoltage(ci.voltageRaw))      | size(WIDTH, EQUAL, 13) : text("--") | size(WIDTH, EQUAL, 13) | dim;
        auto imT  = hasI ? text(fmtCurrentUA(ci.currentRaw))    | size(WIDTH, EQUAL, 16) : text("--") | size(WIDTH, EQUAL, 16) | dim;
        auto vtT  = hasV ? text(fmtVoltage(ci.operationalTargetVoltageRaw)) | size(WIDTH, EQUAL, 13) : text("--") | size(WIDTH, EQUAL, 13) | dim;
        auto fltT = text(faultStr(ci.activeFault));

        Elements parts;
        parts.push_back(chT);
        parts.push_back(vmT);
        parts.push_back(imT);
        if (hasOut) parts.push_back(statusBtn->Render());
        else        parts.push_back(text(" -- ") | size(WIDTH, EQUAL, 8) | dim);
        parts.push_back(vsetInp->Render()     | size(WIDTH, EQUAL, 13));
        parts.push_back(vtT);
        parts.push_back(rampUpInp->Render()   | size(WIDTH, EQUAL, 8));
        parts.push_back(rampDownInp->Render() | size(WIDTH, EQUAL, 8));
        if (hasI) parts.push_back(iLimitInp->Render() | size(WIDTH, EQUAL, 13));
        else      parts.push_back(text(" -- ") | size(WIDTH, EQUAL, 13) | dim);
        if (hasOut) parts.push_back(killBtn->Render());
        else        parts.push_back(text(" -- ") | size(WIDTH, EQUAL, 5) | dim);
        parts.push_back(fltT);

        return hbox(std::move(parts));
    }) | CatchEvent([&s, ch](Event) {
        // Swallow all events for invisible rows so focus never lands here.
        if (!s.data.valid || ch >= s.data.numChannels()) return true;
        return false;
    });
}

inline Component makeMonitorTab(AppState& s, ConfigInputs& inputs) {
    // Build all 16 rows upfront. Invisible rows catch events + render empty.
    Components rows;
    for (int ch = 0; ch < MAX_CHANNELS; ++ch) {
        rows.push_back(makeMonitorRow(s, inputs, ch));
    }
    auto tableContainer = Container::Vertical(rows);

    return Renderer(tableContainer, [=, &s]() {
        int n = s.data.numChannels();

        if (!s.data.valid) {
            return vbox({
                text(" Not connected \xe2\x80\x94 click [ Connect ] in the toolbar ") | center | bold,
                filler(),
            });
        }

        const auto& si = s.data.sysInfo;
        char tmp[16], hum[16];
        snprintf(tmp, sizeof(tmp), "%.1f", si.boardTempRaw * 0.1);
        snprintf(hum, sizeof(hum), "%.1f", si.boardHumidityRaw * 0.1);
        auto sysbar = hbox({
            text(" Proto: " + std::to_string(si.protoMajor) + "." + std::to_string(si.protoMinor)),
            text("  Variant: " + std::to_string(si.variantId)),
            text("  Ch: " + std::to_string(n)),
            text("  Uptime: " + std::to_string(si.uptimeSec) + " s"),
            text("  Mode: " + std::string(opModeName(si.activeOpMode))),
            text("  Temp: " + std::string(tmp) + " C"),
            text("  Humid: " + std::string(hum) + " %"),
            text("  Fault: " + faultStr(si.faultCause)),
        });

        if (n == 0)
            return vbox({ sysbar, text(" Discovering channels... ") | dim | center });

        // Column headers
        auto colHdr = hbox({
            text(" CH ")      | size(WIDTH, EQUAL, 4)  | bold,
            text(" Vm (V)     ") | size(WIDTH, EQUAL, 13) | bold,
            text(" Im (nA)        ") | size(WIDTH, EQUAL, 16) | bold,
            text(" Status  ") | size(WIDTH, EQUAL, 8)  | bold,
            text(" Vset (V)   ") | size(WIDTH, EQUAL, 13) | bold,
            text(" Vt (V)     ") | size(WIDTH, EQUAL, 13) | bold,
            text(" Ramp^") | size(WIDTH, EQUAL, 8)  | bold,
            text(" Rampv") | size(WIDTH, EQUAL, 8)  | bold,
            text(" I-lim (nA)  ") | size(WIDTH, EQUAL, 13) | bold,
            text(" Kill") | bold,
            text(" Fault") | bold,
        });

        return vbox({
            sysbar,
            separator(),
            colHdr,
            separator(),
            tableContainer->Render(),
        });
    });
}

} // namespace hvb::tui
