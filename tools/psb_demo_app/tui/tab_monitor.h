#pragma once
#include "tui_policy.h"
#include "widgets.h"
#include <ftxui/dom/table.hpp>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace psb::tui {

struct MonitorRow {
    Component row;  // Container::Horizontal — focus chain
    Component statusBtn, vsetInp, outputEnabledCyc, rampUpInp, rampDownInp, iLimitInp, clearFaultBtn, saveBtn;
    Component aliasInp;
};

// Build one row — creates all widgets, returns them in a MonitorRow.
inline MonitorRow makeMonitorRow(AppState& s, ConfigInputs& inputs, int ch,
                                 std::function<void(int, const std::string&)> saveAlias) {
    // Narrow, action-specific refreshes — each re-reads only the Modbus
    // block the corresponding write actually touched (merging in place via
    // the reference-taking client methods, never a wholesale struct
    // replace), instead of one catch-all readChannelConfig() re-read of
    // everything after every click. Status is re-read first in every case so
    // the button label updates immediately after a toggle — doPollScan skips
    // inactive channels (not in activeChMask), so without this a
    // just-disabled channel would stay showing "ON" until the next poll.
    auto refreshStatus = [&s, ch]() {
        s.client.readChannelStatus(ch, s.data.chInfo[ch].chCapFlags, s.data.chInfo[ch]);
    };
    auto refreshOutput = [&s, &inputs, ch]() {
        s.client.readChannelStatus(ch, s.data.chInfo[ch].chCapFlags, s.data.chInfo[ch]);
        s.client.readChannelOutputBlock(ch, s.data.chInfo[ch].chCapFlags, s.data.chCfg[ch]);
        syncDataToInputs(s.data, inputs);
    };
    auto refreshProtection = [&s, &inputs, ch]() {
        s.client.readChannelStatus(ch, s.data.chInfo[ch].chCapFlags, s.data.chInfo[ch]);
        s.client.readChannelProtectionBlock(ch, s.data.chInfo[ch].chCapFlags, s.data.chCfg[ch]);
        syncDataToInputs(s.data, inputs);
    };
    auto refreshOutputEnabled = [&s, &inputs, ch]() {
        s.client.readChannelStatus(ch, s.data.chInfo[ch].chCapFlags, s.data.chInfo[ch]);
        s.client.readChannelOutputEnabledBlock(ch, s.data.chInfo[ch].chCapFlags, s.data.chCfg[ch]);
        syncDataToInputs(s.data, inputs);
    };
    auto refreshFull = [&s, &inputs, ch]() {
        s.client.readChannelStatus(ch, s.data.chInfo[ch].chCapFlags, s.data.chInfo[ch]);
        s.client.readChannelConfig(ch, s.data.chInfo[ch].chCapFlags, s.data.chCfg[ch]);
        syncDataToInputs(s.data, inputs);
    };

    // ---- Vset Input (DAC channels — CH_CAP_RAW_OUTPUT_DRIVE) ----
    auto vsetInp = CommitInput(&inputs.targetV[ch], "+0.0", [&s, &inputs, ch, refreshOutput] {
        try {
            auto raw = reg::voltageFromV(std::stod(inputs.targetV[ch]));
            postWrite(s, inputs, "Target V",
                [&s, ch, raw] { return s.client.writeConfiguredTargetVoltage(ch, raw); },
                refreshOutput);
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid voltage"; }
    });

    // ---- Output Enabled cycler (fixed-voltage channels — CH_CAP_OUTPUT_ENABLE
    // without CH_CAP_RAW_OUTPUT_DRIVE). Occupies the same table slot as vsetInp;
    // the two are mutually exclusive per channel — see the render loop below. ----
    auto outputEnabledCyc = InlineCycler({"Off", "On"}, &inputs.outputEnabledIdx[ch],
        [&s, &inputs, ch, refreshOutputEnabled] {
            bool on = inputs.outputEnabledIdx[ch] != 0;
            postWrite(s, inputs, "Output Enabled",
                [&s, ch, on] { return s.client.writeOutputEnabled(ch, on); },
                refreshOutputEnabled);
        }, /*autoCommit=*/true);

    // ---- Status toggle button ----
    // "On" is capability-aware — see channelIsOn() in tui_policy.h. A plain
    // OUTPUT_ENABLE_ACTIVE or OUTPUT_DRIVE_NONZERO bit alone is wrong for at
    // least one channel shape each (fixed-voltage channels have no drive
    // concept; a DAC channel with an enable gate can be "enabled" while
    // legitimately driving 0).
    auto bopt = ButtonOption{};
    bopt.transform = [&s, ch](const EntryState& es) -> Element {
        uint16_t caps = s.data.valid ? s.data.chInfo[ch].chCapFlags : 0;
        uint16_t st   = s.data.valid ? s.data.chInfo[ch].status : 0;
        bool ramp = (st & ChStatus::RAMPING_ACTIVE) != 0;
        bool on   = channelIsOn((caps & CH_CAP_OUTPUT_ENABLE) != 0,
                                (caps & CH_CAP_RAW_OUTPUT_DRIVE) != 0,
                                (st & ChStatus::OUTPUT_ENABLE_ACTIVE) != 0,
                                (st & ChStatus::OUTPUT_DRIVE_NONZERO) != 0);
        Element e;
        if (ramp)      e = text("[ RAMP ]") | color(Color::Yellow) | bold;
        else if (on)   e = text("[  ON  ]") | color(Color::Green) | bold;
        else           e = text("[ OFF ]") | dim;
        if (es.focused) e = e | inverted;
        return e;
    };
    auto statusBtn = Button("", [&s, &inputs, ch, refreshStatus] {
        const uint16_t caps = s.data.chInfo[ch].chCapFlags;
        const uint16_t st   = s.data.chInfo[ch].status;
        const bool on = channelIsOn((caps & CH_CAP_OUTPUT_ENABLE) != 0,
                                    (caps & CH_CAP_RAW_OUTPUT_DRIVE) != 0,
                                    (st & ChStatus::OUTPUT_ENABLE_ACTIVE) != 0,
                                    (st & ChStatus::OUTPUT_DRIVE_NONZERO) != 0);
        const auto action = statusClickAction(
            s.data.valid,
            (st & ChStatus::RAMPING_ACTIVE) != 0,
            on);
        if (action == StatusClickAction::None) return;

        const bool disabling = action == StatusClickAction::DisableGraceful;
        const OutputAction outputAction = disabling
            ? OutputAction::DisableGraceful
            : OutputAction::Enable;
        postWrite(s, inputs, disabling ? "Dis-Grace" : "Enable",
            [&s, ch, outputAction] {
                return s.client.sendOutputAction(ch, outputAction);
            },
            refreshStatus);
    }, bopt);

    // ---- Ramp Up Input ----
    auto rampUpInp = CommitInput(&inputs.ruStep[ch], "0.0", [&s, &inputs, ch, refreshOutput] {
        try {
            auto stepRaw = reg::voltageFromV(std::stod(inputs.ruStep[ch]));
            postWrite(s, inputs, "Ramp Up",
                [&s, ch, stepRaw] { return s.client.writeRampUp(ch, stepRaw, s.data.chCfg[ch].rampUpInterval); },
                refreshOutput);
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid ramp-up value"; }
    });

    // ---- Ramp Down Input ----
    auto rampDownInp = CommitInput(&inputs.rdStep[ch], "0.0", [&s, &inputs, ch, refreshOutput] {
        try {
            auto stepRaw = reg::voltageFromV(std::stod(inputs.rdStep[ch]));
            postWrite(s, inputs, "Ramp Down",
                [&s, ch, stepRaw] { return s.client.writeRampDown(ch, stepRaw, s.data.chCfg[ch].rampDownInterval); },
                refreshOutput);
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid ramp-down value"; }
    });

    // ---- I-limit Input ----
    auto iLimitInp = CommitInput(&inputs.iThr[ch], "0", [&s, &inputs, ch, refreshProtection] {
        try {
            static constexpr OutputAction kIActVals[] = {
                OutputAction::None, OutputAction::DisableGraceful,
                OutputAction::DisableImmediate, OutputAction::ForceOutputZero
            };
            auto mode   = static_cast<ProtectionMode>(inputs.iModeIdx[ch]);
            int  iIdx   = inputs.iActIdx[ch];
            auto action = kIActVals[iIdx >= 0 && iIdx < 4 ? iIdx : 0];
            // Input is in the board's fixed display unit (currentUnitFor),
            // not plain amps — convert back before currentFromA, mirroring
            // the sync side in widgets.h.
            CurrentUnit iu  = currentUnitFor(s.data.sysInfo.currentUnitExp);
            auto raw    = reg::currentFromA(std::stod(inputs.iThr[ch]) / iu.scale,
                                             s.data.sysInfo.currentUnitExp);
            postWrite(s, inputs, "I Limit",
                [&s, ch, mode, action, raw] { return s.client.writeCurrentProtection(ch, mode, action, raw); },
                refreshProtection);
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid I-limit value"; }
    });

    // ---- Clear Fault button ----
    // FlagOnly mode surfaces fault *history* in the Fault column, so Clear
    // targets history there; ApplyOutputAction (and Disabled, which still
    // shows a dimmed active fault) target the active fault block instead.
    auto clearFaultBtn = ActionButton("Clear", [&s, &inputs, ch, refreshStatus] {
        auto mode = s.data.chCfg[ch].iProtMode;
        auto cmd = (mode == ProtectionMode::FlagOnly)
            ? ChannelFaultCommand::ClearFaultHistory
            : ChannelFaultCommand::ClearActiveFaultBlock;
        postWrite(s, inputs, "Clear Fault",
            [&s, ch, cmd] { return s.client.sendChannelFaultCommand(ch, cmd); },
            refreshStatus);
    });

    auto saveBtn = ActionButton("Save", [&s, &inputs, ch, refreshFull] {
        saveChannelConfig(s, inputs, ch, refreshFull);
    });

    // No hardware write — just a display name, so no CommitInput try/catch
    // body needed beyond forwarding the committed string straight to the
    // save closure. Placeholder is the canonical CHn name, per the
    // confirmed "empty means unset, never pre-filled" design.
    auto aliasInp = CommitInput(&inputs.chAlias[ch], "CH" + std::to_string(ch), [&inputs, ch, saveAlias] {
        saveAlias(ch, inputs.chAlias[ch]);
    });

    auto rowWidgets = Container::Horizontal({
        aliasInp, vsetInp, outputEnabledCyc, statusBtn, rampUpInp, rampDownInp, iLimitInp, clearFaultBtn, saveBtn,
    });

    return MonitorRow{
        CatchEvent(rowWidgets, [&s, ch](Event e) {
            if (e.is_mouse()) return false;  // let mouse events pass; parent checks bounds
            return !s.data.valid || ch >= s.data.numChannels();
        }),
        statusBtn, vsetInp, outputEnabledCyc, rampUpInp, rampDownInp, iLimitInp, clearFaultBtn, saveBtn,
        aliasInp,
    };
}

inline Component makeMonitorTab(AppState& s, ConfigInputs& inputs,
                                std::function<void(int, const std::string&)> saveAlias) {
    auto rows = std::make_shared<std::vector<MonitorRow>>();
    Components rowComps;
    for (int ch = 0; ch < MAX_CHANNELS; ++ch) {
        auto r = makeMonitorRow(s, inputs, ch, saveAlias);
        rowComps.push_back(r.row);
        rows->push_back(std::move(r));
    }
    auto tableContainer = Container::Vertical(rowComps);

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

        // Connect scan stages every channel and publishes the whole table
        // atomically (see doFullScan() in tui/main.cpp) — show one clear
        // progress message for the whole duration rather than revealing
        // rows one at a time, which read as a torn/inconsistent table
        // rather than an obviously-still-loading one.
        if (!s.data.allChannelsLoaded()) {
            return text(" Scanning channels... " + std::to_string(s.data.scanProgress) +
                        "/" + std::to_string(n) + " ") | dim | center;
        }

        // First-column label: capability is uniform per board in every real
        // case (jw_hvb: every channel has RAW_OUTPUT_DRIVE; jw_lvb: channels
        // are OUTPUT_ENABLE-only or neither, never RAW_OUTPUT_DRIVE), so one
        // scan over the loaded channels picks the right label for the whole
        // column instead of a static "Vset/En" that fits no board exactly.
        const char* vsetEnHeader = "Vset/En";
        {
            bool anyDrive = false, anyOut = false;
            for (int ch = 0; ch < n; ++ch) {
                uint16_t caps = s.data.chInfo[ch].chCapFlags;
                if (caps & CH_CAP_RAW_OUTPUT_DRIVE) anyDrive = true;
                if (caps & CH_CAP_OUTPUT_ENABLE)     anyOut   = true;
            }
            if (anyDrive)      vsetEnHeader = "Vset (V)";
            else if (anyOut)   vsetEnHeader = "En";
        }
        // Current unit is fixed per board (currentUnitFor), not per-value —
        // see fmtCurrentBare — so it's valid for the whole "I"/"Limit"
        // column and belongs in the header instead of every cell.
        CurrentUnit iu = currentUnitFor(s.data.sysInfo.currentUnitExp);
        const std::vector<std::string> headers = {
            "", "Alias", vsetEnHeader, "Status", "Vop", "V (V)",
            std::string("I (") + iu.label + ")",
            "Ru", "Rd", std::string("Limit (") + iu.label + ")",
            "Fault", "Clear", "Save",
        };

        std::vector<std::vector<Element>> grid;

        // Header row
        {
            std::vector<Element> hdr;
            for (const auto& h : headers)
                hdr.push_back(text(h) | bold | center);
            grid.push_back(std::move(hdr));
        }

        // Data rows
        for (int ch = 0; ch < n; ++ch) {
            char chLabel[8];
            snprintf(chLabel, sizeof(chLabel), "CH%-2d", ch);

            // Channel has failed enough consecutive status polls in a row
            // (see kChannelOfflineThreshold in tui/main.cpp) to be considered
            // genuinely unresponsive — show it as an error, not silently
            // continuing to display its last-known (now stale) values.
            if (s.data.chOffline[ch]) {
                std::vector<Element> cells;
                cells.push_back(text(chLabel) | center);
                cells.push_back(rows->at(ch).aliasInp->Render() | center);
                cells.push_back(text("OFFLINE") | color(Color::Red) | bold | center);
                for (size_t c = 3; c < headers.size(); ++c)
                    cells.push_back(text("--") | color(Color::Red) | dim | center);
                grid.push_back(std::move(cells));
                continue;
            }

            const auto& ci = s.data.chInfo[ch];
            const uint16_t caps = ci.chCapFlags;
            bool hasOut   = (caps & CH_CAP_OUTPUT_ENABLE) != 0;
            bool hasDrive = (caps & CH_CAP_RAW_OUTPUT_DRIVE) != 0;
            bool hasVolt  = (caps & CH_CAP_VOLTAGE_MEASUREMENT) != 0;
            bool hasCurr  = (caps & CH_CAP_CURRENT_MEASUREMENT) != 0;

            std::vector<Element> cells;
            cells.push_back(text(chLabel) | center);
            cells.push_back(rows->at(ch).aliasInp->Render() | center);
            // Vset slot: target-voltage input (DAC channels), on/off cycler
            // (fixed-voltage switchable channels), or "n/a" (locked always-on,
            // e.g. jw_lvb ch0 — no CH_CAP_OUTPUT_ENABLE at all).
            if (hasDrive)
                cells.push_back(rows->at(ch).vsetInp->Render() | center);
            else if (hasOut)
                cells.push_back(rows->at(ch).outputEnabledCyc->Render() | center);
            else
                cells.push_back(text(unsupportedMonitorCellLabel()) | center);
            // Status is meaningful whenever there's any output capability at
            // all — including a drive-only channel with no enable gate (see
            // channelIsOn() in tui_policy.h), not just hasOut alone.
            cells.push_back((hasOut || hasDrive) ? rows->at(ch).statusBtn->Render() | center
                                                  : text(unsupportedMonitorCellLabel()) | center);
            // Vop (operational/ramping target) is only meaningful on DAC
            // channels — fixed-voltage channels have no target-voltage concept
            // at all (readChannelInfo never populates it for them).
            cells.push_back(hasDrive ? text(fmtVoltage(ci.operationalTargetVoltageRaw)) | center
                                     : text(unsupportedMonitorCellLabel()) | center);
            // Bare (no unit suffix) — the "V (V)"/"I (<unit>)" headers above
            // already carry it.
            cells.push_back(hasVolt ? text(fmtVoltageBare(ci.voltageRaw)) | center
                                    : text(unsupportedMonitorCellLabel()) | center);
            cells.push_back(hasCurr ? text(fmtCurrentBare(ci.currentRaw, s.data.sysInfo.currentUnitExp)) | center
                                    : text(unsupportedMonitorCellLabel()) | center);
            cells.push_back(hasDrive ? rows->at(ch).rampUpInp->Render() | center
                                     : text(unsupportedMonitorCellLabel()) | center);
            cells.push_back(hasDrive ? rows->at(ch).rampDownInp->Render() | center
                                     : text(unsupportedMonitorCellLabel()) | center);
            cells.push_back(hasCurr ? rows->at(ch).iLimitInp->Render() | center
                                    : text(unsupportedMonitorCellLabel()) | center);
            // Fault column is mode-dependent: ApplyOutputAction shows the active
            // fault (protection took effect), FlagOnly shows fault history (the
            // only trace it leaves, since output is untouched), Disabled shows a
            // dimmed active fault (still meaningful for non-current faults).
            {
                ProtectionMode mode = s.data.chCfg[ch].iProtMode;
                Element faultEl;
                if (!hasCurr) {
                    faultEl = text(unsupportedMonitorCellLabel());
                } else if (mode == ProtectionMode::FlagOnly) {
                    faultEl = text(faultStr(ci.faultHistory));
                } else if (mode == ProtectionMode::ApplyOutputAction) {
                    faultEl = text(faultStr(ci.activeFault));
                } else {
                    faultEl = text(faultStr(ci.activeFault)) | dim;
                }
                cells.push_back(faultEl | center);
            }
            cells.push_back(hasCurr ? rows->at(ch).clearFaultBtn->Render() | center
                                    : text(unsupportedMonitorCellLabel()) | center);
            cells.push_back(rows->at(ch).saveBtn->Render() | center);

            grid.push_back(std::move(cells));
        }

        auto table = Table(std::move(grid));
        table.SelectAll().Separator(LIGHT);
        table.SelectRow(0).Decorate(bold);
        table.SelectRow(0).SeparatorVertical(LIGHT);
        table.SelectRow(0).Border(DOUBLE);

        for (size_t c = 0; c < headers.size(); ++c)
            table.SelectColumn(static_cast<int>(c)).Decorate(flex);

        return vbox({
            table.Render(),
            filler(),
        });
    });
}

} // namespace psb::tui
