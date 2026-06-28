#pragma once
#include "widgets.h"
#include <algorithm>
#include <memory>
#include <string>

namespace hvb::tui {

inline Component makeMonitorTab(AppState& s) {
    static const std::vector<std::string> kProtModes = {"Disabled","FlagOnly","Apply-Action"};
    static const std::vector<std::string> kIActNames = {"None","Dis-Graceful","Dis-Immed","ForceZero"};
    static const std::vector<OutputAction> kIActVals = {
        OutputAction::None, OutputAction::DisableGraceful,
        OutputAction::DisableImmediate, OutputAction::ForceOutputZero
    };

    struct St {
        int selectedRow = 0;
        std::vector<std::string> rowLabels; // channel indices as strings {"0","1",...}
        std::string targetV[MAX_CHANNELS];
        std::string ruStep[MAX_CHANNELS], ruInt[MAX_CHANNELS];
        std::string rdStep[MAX_CHANNELS], rdInt[MAX_CHANNELS];
        std::string iThr[MAX_CHANNELS];
        int iModeIdx[MAX_CHANNELS]{}, iActIdx[MAX_CHANNELS]{};
    };
    auto st = std::make_shared<St>();

    // ---- Row selection menu: click a row to select it ----
    // EntryState.label holds the channel index as a decimal string.
    MenuOption rowOpt = MenuOption::Vertical();
    rowOpt.entries_option.transform = [&s](const EntryState& e) -> Element {
        int ch = -1;
        try { ch = std::stoi(e.label); } catch (...) {}
        if (ch < 0 || !s.data.valid || ch >= s.data.numChannels())
            return text("");

        const auto& ci = s.data.chInfo[ch];
        const auto& cc = s.data.chCfg[ch];

        // Fixed-width columns to align with the header row
        auto row = hbox({
            text(e.active ? " \xe2\x96\xb6 " : "   ") | size(WIDTH, EQUAL, 3),
            text(std::to_string(ch))                   | size(WIDTH, EQUAL, 4),
            text(fmtVoltage(ci.voltageRaw))            | size(WIDTH, EQUAL, 13),
            text(fmtCurrentUA(ci.currentRaw))          | size(WIDTH, EQUAL, 16),
            text(statusBadge(ci.status))               | size(WIDTH, EQUAL, 9),
            text(std::to_string(cc.rampUpStepRaw))     | size(WIDTH, EQUAL, 8),
            text(std::to_string(cc.rampDownStepRaw))   | size(WIDTH, EQUAL, 8),
            text(protCompact(cc.iProtMode, cc.iProtOutputAction)) | size(WIDTH, EQUAL, 14),
            text(fmtVoltage(cc.configuredTargetVRaw))  | size(WIDTH, EQUAL, 13),
            text(faultStr(ci.activeFault)),
        });
        if (e.active)               row = row | color(Color::Cyan) | bold;
        if (e.focused && !e.active) row = row | inverted;
        return row;
    };
    auto rowMenu = Menu(&st->rowLabels, &st->selectedRow, rowOpt);

    // ---- Per-channel action panels (all built upfront, panelTab switches active) ----
    auto makePanel = [&s, st, &kProtModes, &kIActNames, &kIActVals](int ch) -> Component {
        auto onTarget = [&s, st, ch] {
            try {
                auto raw = hvb::reg::voltageFromV(std::stod(st->targetV[ch]));
                writeAsync(s, "Target V", [&s, ch, raw] {
                    return s.client.writeConfiguredTargetVoltage(ch, raw);
                });
            } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid voltage"; }
        };
        auto onRampUp = [&s, st, ch] {
            try {
                auto step = (uint16_t)std::stoul(st->ruStep[ch]);
                auto iv   = (uint16_t)std::stoul(st->ruInt[ch]);
                writeAsync(s, "Ramp Up", [&s, ch, step, iv] {
                    return s.client.writeRampUp(ch, step, iv);
                });
            } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid ramp-up value"; }
        };
        auto onRampDown = [&s, st, ch] {
            try {
                auto step = (uint16_t)std::stoul(st->rdStep[ch]);
                auto iv   = (uint16_t)std::stoul(st->rdInt[ch]);
                writeAsync(s, "Ramp Down", [&s, ch, step, iv] {
                    return s.client.writeRampDown(ch, step, iv);
                });
            } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid ramp-down value"; }
        };
        auto onIProt = [&s, st, ch, &kIActVals] {
            try {
                auto mode   = static_cast<ProtectionMode>(st->iModeIdx[ch]);
                auto action = kIActVals.at(st->iActIdx[ch]);
                auto raw    = static_cast<int16_t>(std::stod(st->iThr[ch]) * 1000.0 + 0.5);
                writeAsync(s, "I Limit", [&s, ch, mode, action, raw] {
                    return s.client.writeCurrentProtection(ch, mode, action, raw);
                });
            } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid I-limit value"; }
        };

        auto tgtInp    = CommitInput(&st->targetV[ch],  "+0.0",  onTarget);
        auto ruStepInp = CommitInput(&st->ruStep[ch],   "0",     onRampUp);
        auto ruIntInp  = CommitInput(&st->ruInt[ch],    "0",     onRampUp);
        auto rdStepInp = CommitInput(&st->rdStep[ch],   "0",     onRampDown);
        auto rdIntInp  = CommitInput(&st->rdInt[ch],    "0",     onRampDown);
        auto iModeC    = InlineCycler(kProtModes, &st->iModeIdx[ch], onIProt);
        auto iActC     = InlineCycler(kIActNames, &st->iActIdx[ch],  onIProt);
        auto iThrInp   = CommitInput(&st->iThr[ch],     "0.000", onIProt);

        auto bEnable  = ActionButton("Enable",    [&s,ch]{ writeAsync(s,"Enable",    [&s,ch]{ return s.client.sendOutputAction(ch, OutputAction::Enable); }); });
        auto bDisImm  = ActionButton("Dis-Immed", [&s,ch]{ writeAsync(s,"Dis-Immed", [&s,ch]{ return s.client.sendOutputAction(ch, OutputAction::DisableImmediate); }); });
        auto bDisGra  = ActionButton("Dis-Grace", [&s,ch]{ writeAsync(s,"Dis-Grace", [&s,ch]{ return s.client.sendOutputAction(ch, OutputAction::DisableGraceful); }); });
        auto bClrAct  = ActionButton("ClrActive", [&s,ch]{ writeAsync(s,"ClrActive", [&s,ch]{ return s.client.sendChannelFaultCommand(ch, ChannelFaultCommand::ClearActiveFaultBlock); }); });
        auto bClrHist = ActionButton("ClrHist",   [&s,ch]{ writeAsync(s,"ClrHist",   [&s,ch]{ return s.client.sendChannelFaultCommand(ch, ChannelFaultCommand::ClearFaultHistory); }); });

        auto container = Container::Vertical({
            tgtInp, ruStepInp, ruIntInp, rdStepInp, rdIntInp,
            iModeC, iActC, iThrInp,
            bEnable, bDisImm, bDisGra, bClrAct, bClrHist,
        });

        return Renderer(container, [=, ch]() {
            return window(text(" CH" + std::to_string(ch) + " ") | bold, vbox({
                hbox({ text("  Target    : "), tgtInp->Render(), text(" V") }),
                hbox({ text("  Ramp Up   : step "), ruStepInp->Render(),
                       text(" LSB  interval "), ruIntInp->Render(), text(" \xc3\x970.1 s") }),
                hbox({ text("  Ramp Down : step "), rdStepInp->Render(),
                       text(" LSB  interval "), rdIntInp->Render(), text(" \xc3\x970.1 s") }),
                hbox({ text("  I Limit   : "), iModeC->Render(), text("  "), iActC->Render(),
                       text("  threshold "), iThrInp->Render(), text(" \xc2\xb5\x41") }),
                separator(),
                hbox({ text("  "), bEnable->Render(), text("  "), bDisImm->Render(),
                       text("  "), bDisGra->Render(), text("    "),
                       bClrAct->Render(), text("  "), bClrHist->Render() }),
            }));
        });
    };

    Components panels;
    for (int ch = 0; ch < MAX_CHANNELS; ++ch) panels.push_back(makePanel(ch));
    auto panelTab = Container::Tab(panels, &st->selectedRow);

    auto mainContainer = Container::Vertical({rowMenu, panelTab});

    return Renderer(mainContainer, [=, &s, st]() {
        // Keep rowLabels in sync with connected channel count.
        int n = s.data.numChannels();
        if ((int)st->rowLabels.size() != n) {
            st->rowLabels.clear();
            for (int i = 0; i < n; ++i)
                st->rowLabels.push_back(std::to_string(i));
            if (n > 0)
                st->selectedRow = std::min(st->selectedRow, n - 1);
        }

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
            text("  Temp: " + std::string(tmp) + " \xc2\xb0\x43"),
            text("  Humid: " + std::string(hum) + " %"),
            text("  Fault: " + faultStr(si.faultCause)),
        });

        if (n == 0)
            return vbox({ sysbar, text(" Discovering channels... ") | dim | center });

        // Column header — widths must match the row menu transform
        auto colHdr = hbox({
            text("   ") | size(WIDTH, EQUAL, 3),
            text(" CH ") | size(WIDTH, EQUAL, 4) | bold,
            text(" Vmeas      ") | size(WIDTH, EQUAL, 13) | bold,
            text(" Imeas         ") | size(WIDTH, EQUAL, 16) | bold,
            text(" Status  ") | size(WIDTH, EQUAL, 9) | bold,
            text(" Ramp\xe2\x86\x91 ") | size(WIDTH, EQUAL, 8) | bold,
            text(" Ramp\xe2\x86\x93 ") | size(WIDTH, EQUAL, 8) | bold,
            text(" I-Prot       ") | size(WIDTH, EQUAL, 14) | bold,
            text(" Target V   ") | size(WIDTH, EQUAL, 13) | bold,
            text(" Fault") | bold,
        });

        return vbox({
            sysbar,
            separator(),
            colHdr,
            separator(),
            rowMenu->Render(),
            separator(),
            panelTab->Render(),
        });
    });
}

} // namespace hvb::tui
