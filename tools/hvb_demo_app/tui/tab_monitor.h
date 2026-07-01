#pragma once
#include "tui_policy.h"
#include "widgets.h"
#include <ftxui/dom/table.hpp>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace hvb::tui {

struct MonitorRow {
    Component row;  // Container::Horizontal — focus chain
    Component statusBtn, vsetInp, rampUpInp, rampDownInp, iLimitInp;
};

// Build one row — creates all widgets, returns them in a MonitorRow.
inline MonitorRow makeMonitorRow(AppState& s, ConfigInputs& inputs, int ch) {
    auto refreshCh = [&s, &inputs, ch]() {
        // Read live status first so the button label updates immediately after toggle.
        // doPollScan skips inactive channels (not in activeChMask), so without this
        // call a just-disabled channel would stay showing "ON" until the next full scan.
        s.client.readChannelStatus(ch, s.data.chInfo[ch].chCapFlags, s.data.chInfo[ch]);
        s.data.chCfg[ch] = s.client.readChannelConfig(ch, s.data.chInfo[ch].chCapFlags);
        syncDataToInputs(s.data, inputs);
    };

    // ---- Vset Input ----
    auto vsetInp = CommitInput(&inputs.targetV[ch], "+0.0", [&s, &inputs, ch, refreshCh] {
        try {
            auto raw = reg::voltageFromV(std::stod(inputs.targetV[ch]));
            postWrite(s, inputs, "Target V",
                [&s, ch, raw] { return s.client.writeConfiguredTargetVoltage(ch, raw); },
                refreshCh);
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid voltage"; }
    });

    // ---- Status toggle button ----
    auto bopt = ButtonOption{};
    bopt.transform = [&s, ch](const EntryState& es) -> Element {
        uint16_t st = s.data.valid ? s.data.chInfo[ch].status : 0;
        int16_t tv  = s.data.valid ? s.data.chInfo[ch].operationalTargetVoltageRaw : 0;
        bool ramp   = (st & ChStatus::RAMPING_ACTIVE) != 0;
        bool on     = (st & ChStatus::OUTPUT_DRIVE_NONZERO) != 0 && tv != 0;
        Element e;
        if (ramp)      e = text("[ RAMP ]") | color(Color::Yellow) | bold;
        else if (on)   e = text("[  ON  ]") | color(Color::Green) | bold;
        else           e = text("[ OFF ]") | dim;
        if (es.focused) e = e | inverted;
        return e;
    };
    auto statusBtn = Button("", [&s, &inputs, ch, refreshCh] {
        const uint16_t st = s.data.chInfo[ch].status;
        const auto action = statusClickAction(
            s.data.valid,
            (st & ChStatus::RAMPING_ACTIVE) != 0,
            s.data.chCfg[ch].configuredTargetVRaw,
            s.data.chInfo[ch].operationalTargetVoltageRaw,
            (st & ChStatus::OUTPUT_DRIVE_NONZERO) != 0);
        if (action == StatusClickAction::None) return;

        const bool disabling = action == StatusClickAction::DisableGraceful;
        const OutputAction outputAction = disabling
            ? OutputAction::DisableGraceful
            : OutputAction::Enable;
        postWrite(s, inputs, disabling ? "Dis-Grace" : "Enable",
            [&s, ch, outputAction] {
                return s.client.sendOutputAction(ch, outputAction);
            },
            refreshCh);
    }, bopt);

    // ---- Ramp Up Input ----
    auto rampUpInp = CommitInput(&inputs.ruStep[ch], "0.0", [&s, &inputs, ch, refreshCh] {
        try {
            auto stepRaw = reg::voltageFromV(std::stod(inputs.ruStep[ch]));
            postWrite(s, inputs, "Ramp Up",
                [&s, ch, stepRaw] { return s.client.writeRampUp(ch, stepRaw, s.data.chCfg[ch].rampUpInterval); },
                refreshCh);
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid ramp-up value"; }
    });

    // ---- Ramp Down Input ----
    auto rampDownInp = CommitInput(&inputs.rdStep[ch], "0.0", [&s, &inputs, ch, refreshCh] {
        try {
            auto stepRaw = reg::voltageFromV(std::stod(inputs.rdStep[ch]));
            postWrite(s, inputs, "Ramp Down",
                [&s, ch, stepRaw] { return s.client.writeRampDown(ch, stepRaw, s.data.chCfg[ch].rampDownInterval); },
                refreshCh);
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid ramp-down value"; }
    });

    // ---- I-limit Input ----
    auto iLimitInp = CommitInput(&inputs.iThr[ch], "0", [&s, &inputs, ch, refreshCh] {
        try {
            static constexpr OutputAction kIActVals[] = {
                OutputAction::None, OutputAction::DisableGraceful,
                OutputAction::DisableImmediate, OutputAction::ForceOutputZero
            };
            auto mode   = static_cast<ProtectionMode>(inputs.iModeIdx[ch]);
            int  iIdx   = inputs.iActIdx[ch];
            auto action = kIActVals[iIdx >= 0 && iIdx < 4 ? iIdx : 0];
            auto raw    = static_cast<int16_t>(std::stod(inputs.iThr[ch]) + 0.5);
            postWrite(s, inputs, "I Limit",
                [&s, ch, mode, action, raw] { return s.client.writeCurrentProtection(ch, mode, action, raw); },
                refreshCh);
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid I-limit value"; }
    });

    auto rowWidgets = Container::Horizontal({
        vsetInp, statusBtn, rampUpInp, rampDownInp, iLimitInp,
    });

    return MonitorRow{
        CatchEvent(rowWidgets, [&s, ch](Event e) {
            if (e.is_mouse()) return false;  // let mouse events pass; parent checks bounds
            return !s.data.valid || ch >= s.data.numChannels();
        }),
        statusBtn, vsetInp, rampUpInp, rampDownInp, iLimitInp,
    };
}

inline Component makeMonitorTab(AppState& s, ConfigInputs& inputs) {
    auto rows = std::make_shared<std::vector<MonitorRow>>();
    Components rowComps;
    for (int ch = 0; ch < MAX_CHANNELS; ++ch) {
        auto r = makeMonitorRow(s, inputs, ch);
        rowComps.push_back(r.row);
        rows->push_back(std::move(r));
    }
    auto tableContainer = Container::Vertical(rowComps);

    static const std::vector<std::string> kHeaders = {
        "", "Vset", "Status", "Vop", "V (V)", "I (nA)",
        "Ru", "Rd", "Limit", "Fault",
    };

    return Renderer(tableContainer, [=, &s]() {
        int n = s.data.numChannels();

        if (!s.data.valid) {
            return vbox({
                text(" Not connected \xe2\x80\x94 click [ Connect ] in the toolbar ") | center | bold,
                filler(),
            });
        }

        if (n == 0)
            return text(" Discovering channels... ") | dim | center;

        std::vector<std::vector<Element>> grid;

        // Header row
        {
            std::vector<Element> hdr;
            for (const auto& h : kHeaders)
                hdr.push_back(text(h) | bold | center);
            grid.push_back(std::move(hdr));
        }

        // Data rows
        for (int ch = 0; ch < n; ++ch) {
            const auto& ci = s.data.chInfo[ch];
            const uint16_t caps = ci.chCapFlags;
            bool hasOut  = (caps & CH_CAP_OUTPUT_ENABLE) != 0;
            bool hasVolt = (caps & CH_CAP_VOLTAGE_MEASUREMENT) != 0;
            bool hasCurr = (caps & CH_CAP_CURRENT_MEASUREMENT) != 0;

            char chLabel[8];
            snprintf(chLabel, sizeof(chLabel), "CH%-2d", ch);

            std::vector<Element> cells;
            cells.push_back(text(chLabel) | center);
            cells.push_back(hasOut ? rows->at(ch).vsetInp->Render() | center
                                   : text(" -- ") | dim | center);
            cells.push_back(hasOut ? rows->at(ch).statusBtn->Render() | center
                                   : text(" -- ") | dim | center);
            cells.push_back(text(fmtVoltage(ci.operationalTargetVoltageRaw)) | center);
            cells.push_back(hasVolt ? text(fmtVoltage(ci.voltageRaw)) | center
                                    : text(" -- ") | dim | center);
            cells.push_back(hasCurr ? text(fmtCurrentNA(ci.currentRaw)) | center
                                    : text(" -- ") | dim | center);
            cells.push_back(hasOut ? rows->at(ch).rampUpInp->Render() | center
                                   : text(" -- ") | dim | center);
            cells.push_back(hasOut ? rows->at(ch).rampDownInp->Render() | center
                                   : text(" -- ") | dim | center);
            cells.push_back(hasCurr ? rows->at(ch).iLimitInp->Render() | center
                                    : text(" -- ") | dim | center);
            cells.push_back(text(faultStr(ci.activeFault)) | center);

            grid.push_back(std::move(cells));
        }

        auto table = Table(std::move(grid));
        table.SelectAll().Separator(LIGHT);
        table.SelectRow(0).Decorate(bold);
        table.SelectRow(0).SeparatorVertical(LIGHT);
        table.SelectRow(0).Border(DOUBLE);

        for (size_t c = 0; c < kHeaders.size(); ++c)
            table.SelectColumn(static_cast<int>(c)).Decorate(flex);

        return vbox({
            table.Render(),
            filler(),
        });
    });
}

} // namespace hvb::tui
