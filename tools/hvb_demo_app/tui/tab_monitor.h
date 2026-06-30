#pragma once
#include "widgets.h"
#include <ftxui/dom/table.hpp>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace hvb::tui {

struct MonitorRow {
    Component row;  // Container::Horizontal — focus chain
    Component statusBtn, vsetInp, rampUpInp, rampDownInp, iLimitInp, killBtn;
};

// Build one row — creates all widgets, returns them in a MonitorRow.
// Invisible rows (ch >= numChannels) catch all events; table renderer skips them.
inline MonitorRow makeMonitorRow(AppState& s, ConfigInputs& inputs, int ch) {
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
    auto vsetInp = CommitInput(&inputs.targetV[ch], "+0.0", [&s, &inputs, ch, refreshCh] {
        try {
            auto raw = reg::voltageFromV(std::stod(inputs.targetV[ch]));
            writeSync(s, inputs, "Target V",
                [&s, ch, raw] { return s.client.writeConfiguredTargetVoltage(ch, raw); },
                refreshCh);
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid voltage"; }
    });

    // ---- Ramp Up Input ----
    auto rampUpInp = CommitInput(&inputs.ruStep[ch], "0.0", [&s, &inputs, ch, refreshCh] {
        try {
            auto stepRaw = reg::voltageFromV(std::stod(inputs.ruStep[ch]));
            writeSync(s, inputs, "Ramp Up",
                [&s, ch, stepRaw] { return s.client.writeRampUp(ch, stepRaw, s.data.chCfg[ch].rampUpInterval); },
                refreshCh);
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid ramp-up value"; }
    });

    // ---- Ramp Down Input ----
    auto rampDownInp = CommitInput(&inputs.rdStep[ch], "0.0", [&s, &inputs, ch, refreshCh] {
        try {
            auto stepRaw = reg::voltageFromV(std::stod(inputs.rdStep[ch]));
            writeSync(s, inputs, "Ramp Down",
                [&s, ch, stepRaw] { return s.client.writeRampDown(ch, stepRaw, s.data.chCfg[ch].rampDownInterval); },
                refreshCh);
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid ramp-down value"; }
    });

    // ---- I-limit Input ----
    auto iLimitInp = CommitInput(&inputs.iThr[ch], "0", [&s, &inputs, ch, refreshCh] {
        try {
            auto mode   = static_cast<ProtectionMode>(inputs.iModeIdx[ch]);
            auto action = static_cast<OutputAction>(inputs.iActIdx[ch]);
            auto raw    = static_cast<int16_t>(std::stod(inputs.iThr[ch]) + 0.5);
            writeSync(s, inputs, "I Limit",
                [&s, ch, mode, action, raw] { return s.client.writeCurrentProtection(ch, mode, action, raw); },
                refreshCh);
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid I-limit value"; }
    });

    // ---- Kill button (ForceOutputZero) ----
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
            [&s, ch] {
                uint16_t v = static_cast<uint16_t>(OutputAction::ForceOutputZero);
                return s.client.writeReg16(reg::chAddr(ch, CH_OUTPUT_ACTION), v);
            },
            refreshCh);
    }, kopt);

    auto rowWidgets = Container::Horizontal({
        statusBtn, vsetInp, rampUpInp, rampDownInp, iLimitInp, killBtn,
    });

    return MonitorRow{
        CatchEvent(rowWidgets, [&s, ch](Event) {
            return !s.data.valid || ch >= s.data.numChannels();
        }),
        statusBtn, vsetInp, rampUpInp, rampDownInp, iLimitInp, killBtn,
    };
}

inline Component makeMonitorTab(AppState& s, ConfigInputs& inputs) {
    // Pre-build all 16 rows.
    auto rows = std::make_shared<std::vector<MonitorRow>>();
    Components rowComps;
    for (int ch = 0; ch < MAX_CHANNELS; ++ch) {
        auto r = makeMonitorRow(s, inputs, ch);
        rowComps.push_back(r.row);
        rows->push_back(std::move(r));
    }
    auto tableContainer = Container::Vertical(rowComps);

    static const std::vector<std::string> kHeaders = {
        "CH", "Vm (V)", "Im (nA)", "Status", "Vset (V)", "Vt (V)",
        "Ru", "Rd", "I-lim (nA)", "Kill", "Fault",
    };

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

        // Build table grid: header row + one data row per active channel.
        std::vector<std::vector<Element>> grid;

        // Header row
        {
            std::vector<Element> hdr;
            for (const auto& h : kHeaders)
                hdr.push_back(text(h) | bold);
            grid.push_back(std::move(hdr));
        }

        // Data rows
        for (int ch = 0; ch < n; ++ch) {
            const auto& ci = s.data.chInfo[ch];
            const uint16_t caps = ci.chCapFlags;
            bool hasV = (caps & CH_CAP_VOLTAGE_MEASUREMENT) != 0;
            bool hasI = (caps & CH_CAP_CURRENT_MEASUREMENT) != 0;
            bool hasOut = (caps & CH_CAP_OUTPUT_ENABLE) != 0;

            char chLabel[8];
            snprintf(chLabel, sizeof(chLabel), "CH%-2d", ch);

            std::vector<Element> cells;
            cells.push_back(text(chLabel));
            cells.push_back(hasV ? text(fmtVoltage(ci.voltageRaw)) : text("--") | dim);
            cells.push_back(hasI ? text(fmtCurrentNA(ci.currentRaw)) : text("--") | dim);
            cells.push_back(hasOut ? rows->at(ch).statusBtn->Render() : text(" -- ") | dim);
            cells.push_back(hasOut ? rows->at(ch).vsetInp->Render() : text(" -- ") | dim);
            cells.push_back(hasV ? text(fmtVoltage(ci.operationalTargetVoltageRaw)) : text("--") | dim);
            cells.push_back(hasOut ? rows->at(ch).rampUpInp->Render() : text(" -- ") | dim);
            cells.push_back(hasOut ? rows->at(ch).rampDownInp->Render() : text(" -- ") | dim);
            cells.push_back(hasI ? rows->at(ch).iLimitInp->Render() : text(" -- ") | dim);
            cells.push_back(hasOut ? rows->at(ch).killBtn->Render() : text(" -- ") | dim);
            cells.push_back(text(faultStr(ci.activeFault)));

            grid.push_back(std::move(cells));
        }

        auto table = Table(std::move(grid));
        table.SelectAll().Separator(LIGHT);
        table.SelectRow(0).Decorate(bold);
        table.SelectRow(0).SeparatorVertical(LIGHT);
        table.SelectRow(0).Border(DOUBLE);
        // Right-align numeric columns
        table.SelectColumn(1).DecorateCells(align_right);
        table.SelectColumn(5).DecorateCells(align_right);

        return vbox({
            sysbar,
            table.Render(),
        });
    });
}

} // namespace hvb::tui
