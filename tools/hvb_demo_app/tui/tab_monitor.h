#pragma once
#include "widgets.h"
#include <ftxui/dom/table.hpp>
#include <algorithm>
#include <memory>
#include <string>

namespace hvb::tui {

inline Component makeMonitorTab(AppState& s) {
    // ---- Dropdown tables ----
    static const std::vector<std::string> kProtModes  = {"Disabled","FlagOnly","Apply-Action"};
    static const std::vector<std::string> kVActNames  = {"None","Dis-Graceful","Dis-Immed","ForceZero","Clamp"};
    static const std::vector<OutputAction> kVActVals  = {
        OutputAction::None, OutputAction::DisableGraceful, OutputAction::DisableImmediate,
        OutputAction::ForceOutputZero, OutputAction::Clamp
    };
    static const std::vector<std::string> kIActNames  = {"None","Dis-Graceful","Dis-Immed","ForceZero"};
    static const std::vector<OutputAction> kIActVals  = {
        OutputAction::None, OutputAction::DisableGraceful,
        OutputAction::DisableImmediate, OutputAction::ForceOutputZero
    };

    // ---- Tab-local state ----
    struct St {
        int  selectedRow  = 0;
        bool panelFocused = false;
        std::string targetV[2];
        std::string ruStep[2], ruInt[2];
        std::string rdStep[2], rdInt[2];
        std::string vThr[2],   iThr[2];
        int vModeIdx[2]{}, vActIdx[2]{};
        int iModeIdx[2]{}, iActIdx[2]{};
    };
    auto st = std::make_shared<St>();

    // ---- Read-only table (called from Renderer every frame) ----
    auto drawTable = [&s, st]() -> Element {
        if (!s.data.valid)
            return text(" Not connected — press 'c' to connect ") | center | bold;

        const auto& si = s.data.sysInfo;
        char tmp[16], hum[16];
        snprintf(tmp, sizeof(tmp), "%.1f", si.boardTempRaw * 0.1);
        snprintf(hum, sizeof(hum), "%.1f", si.boardHumidityRaw * 0.1);
        auto sysbar = hbox({
            text("Proto: " + std::to_string(si.protoMajor) + "." + std::to_string(si.protoMinor)),
            text("  Variant: " + std::to_string(si.variantId)),
            text("  Uptime: " + std::to_string(si.uptimeSec) + " s"),
            text("  Mode: " + std::string(opModeName(si.activeOpMode))),
            text("  Temp: " + std::string(tmp) + " \xc2\xb0\x43"), // °C
            text("  Humid: " + std::string(hum) + " %"),
            text("  Fault: " + faultStr(si.faultCause)),
        });

        std::vector<std::vector<std::string>> rows;
        rows.push_back({"CH","Vmeas","Imeas","Status","Ramp\xe2\x86\x91","Ramp\xe2\x86\x93","I-Prot","Target V","Fault"});
        for (int ch = 0; ch < 2; ++ch) {
            const auto& ci = s.data.chInfo[ch];
            const auto& cc = s.data.chCfg[ch];
            std::string sel = (ch == st->selectedRow) ? "\xe2\x96\xb6" : " ";
            if (ci.status & ChStatus::UNSUPPORTED) {
                rows.push_back({sel + std::to_string(ch), "(unsupported)", "", "", "", "", "", "", ""});
                continue;
            }
            rows.push_back({
                sel + std::to_string(ch),
                fmtVoltage(ci.voltageRaw),
                fmtCurrentUA(ci.currentRaw),
                statusBadge(ci.status),
                std::to_string(cc.rampUpStepRaw),
                std::to_string(cc.rampDownStepRaw),
                protCompact(cc.iProtMode, cc.iProtOutputAction),
                fmtVoltage(cc.configuredTargetVRaw),
                faultStr(ci.activeFault),
            });
        }
        auto tbl = Table(rows);
        tbl.SelectAll().Border(LIGHT);
        tbl.SelectAll().Separator(LIGHT);
        tbl.SelectRows(0, 0).Decorate(bold);
        tbl.SelectRows(0, 0).Separator(HEAVY);
        if (s.data.valid && st->selectedRow < 2)
            tbl.SelectRows(st->selectedRow + 1, st->selectedRow + 1)
               .Decorate(bold | color(Color::Cyan));
        return vbox({ sysbar, separator(), tbl.Render() });
    };

    // ---- Action panel factory (one per channel, both built at startup) ----
    auto makePanel = [&s, st, &kProtModes, &kVActNames, &kVActVals,
                                           &kIActNames, &kIActVals](int ch) -> Component {
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
        auto onVProt = [&s, st, ch, &kVActVals] {
            try {
                auto mode   = static_cast<ProtectionMode>(st->vModeIdx[ch]);
                auto action = kVActVals.at(st->vActIdx[ch]);
                auto raw    = hvb::reg::voltageFromV(std::stod(st->vThr[ch]));
                writeAsync(s, "V Limit", [&s, ch, mode, action, raw] {
                    return s.client.writeVoltageProtection(ch, mode, action, raw);
                });
            } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid V-limit value"; }
        };
        auto onIProt = [&s, st, ch, &kIActVals] {
            try {
                auto mode   = static_cast<ProtectionMode>(st->iModeIdx[ch]);
                auto action = kIActVals.at(st->iActIdx[ch]);
                // User types µA; 1 LSB = 1 nA → multiply by 1000
                auto raw    = static_cast<int16_t>(std::stod(st->iThr[ch]) * 1000.0 + 0.5);
                writeAsync(s, "I Limit", [&s, ch, mode, action, raw] {
                    return s.client.writeCurrentProtection(ch, mode, action, raw);
                });
            } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid I-limit value"; }
        };

        auto tgtInp = CommitInput(&st->targetV[ch],  "+0.0",   onTarget);
        auto ruStepInp = CommitInput(&st->ruStep[ch], "0",     onRampUp);
        auto ruIntInp  = CommitInput(&st->ruInt[ch],  "0",     onRampUp);
        auto rdStepInp = CommitInput(&st->rdStep[ch], "0",     onRampDown);
        auto rdIntInp  = CommitInput(&st->rdInt[ch],  "0",     onRampDown);
        auto vModeC = InlineCycler(kProtModes, &st->vModeIdx[ch], onVProt);
        auto vActC  = InlineCycler(kVActNames, &st->vActIdx[ch],  onVProt);
        auto vThrInp = CommitInput(&st->vThr[ch],    "+0.0",   onVProt);
        auto iModeC = InlineCycler(kProtModes, &st->iModeIdx[ch], onIProt);
        auto iActC  = InlineCycler(kIActNames, &st->iActIdx[ch],  onIProt);
        auto iThrInp = CommitInput(&st->iThr[ch],    "0.000",  onIProt);

        auto bEnable  = ActionButton("Enable",    [&s,ch]{ writeAsync(s,"Enable",    [&s,ch]{ return s.client.sendOutputAction(ch, OutputAction::Enable); }); });
        auto bDisImm  = ActionButton("Dis-Immed", [&s,ch]{ writeAsync(s,"Dis-Immed", [&s,ch]{ return s.client.sendOutputAction(ch, OutputAction::DisableImmediate); }); });
        auto bDisGra  = ActionButton("Dis-Grace", [&s,ch]{ writeAsync(s,"Dis-Grace", [&s,ch]{ return s.client.sendOutputAction(ch, OutputAction::DisableGraceful); }); });
        auto bClrAct  = ActionButton("ClrActive", [&s,ch]{ writeAsync(s,"ClrActive", [&s,ch]{ return s.client.sendChannelFaultCommand(ch, ChannelFaultCommand::ClearActiveFaultBlock); }); });
        auto bClrHist = ActionButton("ClrHist",   [&s,ch]{ writeAsync(s,"ClrHist",   [&s,ch]{ return s.client.sendChannelFaultCommand(ch, ChannelFaultCommand::ClearFaultHistory); }); });

        // Flat Vertical container: Tab order matches visual top-to-bottom order
        auto container = Container::Vertical({
            tgtInp,
            ruStepInp, ruIntInp,
            rdStepInp, rdIntInp,
            vModeC, vActC, vThrInp,
            iModeC, iActC, iThrInp,
            bEnable, bDisImm, bDisGra, bClrAct, bClrHist,
        });

        // Custom renderer: lays out inputs with label text; Container handles focus/events
        return Renderer(container, [=, ch]() {
            std::string title = " CH" + std::to_string(ch) + " ";
            return window(text(title) | bold, vbox({
                hbox({ text("  Target    : "), tgtInp->Render(), text(" V") }),
                hbox({ text("  Ramp Up   : step "), ruStepInp->Render(),
                       text(" LSB  interval "), ruIntInp->Render(), text(" \xc3\x970.1 s") }),
                hbox({ text("  Ramp Down : step "), rdStepInp->Render(),
                       text(" LSB  interval "), rdIntInp->Render(), text(" \xc3\x970.1 s") }),
                hbox({ text("  V Limit   : "), vModeC->Render(), text("  "), vActC->Render(),
                       text("  threshold "), vThrInp->Render(), text(" V") }),
                hbox({ text("  I Limit   : "), iModeC->Render(), text("  "), iActC->Render(),
                       text("  threshold "), iThrInp->Render(), text(" \xc2\xb5\x41") }), // µA
                separator(),
                hbox({ text("  "), bEnable->Render(), text("  "), bDisImm->Render(),
                       text("  "), bDisGra->Render(), text("    "),
                       bClrAct->Render(), text("  "), bClrHist->Render() }),
            }));
        });
    };

    auto panel0 = makePanel(0);
    auto panel1 = makePanel(1);

    // Container::Tab switches the active (event-routing) panel when selectedRow changes
    auto panelTab = Container::Tab({panel0, panel1}, &st->selectedRow);

    // Full tab: render table + panel; route keyboard between them
    return Renderer(panelTab, [=, &s, st, drawTable]() {
        return vbox({ drawTable(), separator(), panelTab->Render() });
    }) | CatchEvent([st](Event e) {
        if (!st->panelFocused) {
            if (e == Event::ArrowUp)   { st->selectedRow = std::max(0, st->selectedRow - 1); return true; }
            if (e == Event::ArrowDown) { st->selectedRow = std::min(1, st->selectedRow + 1); return true; }
            if (e == Event::Tab)       { st->panelFocused = true; return true; }
            return false;
        }
        if (e == Event::Escape) { st->panelFocused = false; return true; }
        return false;
    });
}

} // namespace hvb::tui
