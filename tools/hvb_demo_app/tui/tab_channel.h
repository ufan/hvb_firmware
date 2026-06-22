#pragma once
#include "widgets.h"
#include <memory>
#include <string>

namespace hvb::tui {

inline Component makeChannelTab(AppState& s, int ch) {
    static const std::vector<std::string> kProtModes = {"Disabled","FlagOnly","Apply-Action"};
    static const std::vector<std::string> kVActNames = {"None","Dis-Graceful","Dis-Immed","ForceZero","Clamp"};
    static const std::vector<OutputAction> kVActVals = {
        OutputAction::None, OutputAction::DisableGraceful, OutputAction::DisableImmediate,
        OutputAction::ForceOutputZero, OutputAction::Clamp
    };
    static const std::vector<std::string> kIActNames = {"None","Dis-Graceful","Dis-Immed","ForceZero"};
    static const std::vector<OutputAction> kIActVals = {
        OutputAction::None, OutputAction::DisableGraceful,
        OutputAction::DisableImmediate, OutputAction::ForceOutputZero
    };
    static const std::vector<std::string> kSaveTarget = {"No","Yes"};

    struct St {
        std::string targetV, vThr, iThr;
        std::string ruStep, ruInt, rdStep, rdInt, derateStep;
        int vModeIdx = 0, vActIdx = 0;
        int iModeIdx = 0, iActIdx = 0;
        int saveTargetIdx = 0;
    };
    auto st = std::make_shared<St>();

    auto onTarget = [&s, st, ch] {
        try {
            auto raw = hvb::reg::voltageFromV(std::stod(st->targetV));
            writeAsync(s, "Target V", [&s, ch, raw] {
                return s.client.writeConfiguredTargetVoltage(ch, raw);
            });
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid voltage"; }
    };
    auto onRampUp = [&s, st, ch] {
        try {
            auto step = (uint16_t)std::stoul(st->ruStep);
            auto iv   = (uint16_t)std::stoul(st->ruInt);
            writeAsync(s, "Ramp Up", [&s, ch, step, iv] { return s.client.writeRampUp(ch, step, iv); });
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid ramp value"; }
    };
    auto onRampDown = [&s, st, ch] {
        try {
            auto step = (uint16_t)std::stoul(st->rdStep);
            auto iv   = (uint16_t)std::stoul(st->rdInt);
            writeAsync(s, "Ramp Down", [&s, ch, step, iv] { return s.client.writeRampDown(ch, step, iv); });
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid ramp value"; }
    };
    auto onDerate = [&s, st, ch] {
        try {
            auto step = (uint16_t)std::stoul(st->derateStep);
            writeAsync(s, "Derate", [&s, ch, step] { return s.client.writeDerateStep(ch, step); });
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid derate step"; }
    };
    auto onVProt = [&s, st, ch, &kVActVals] {
        try {
            auto mode   = static_cast<ProtectionMode>(st->vModeIdx);
            auto action = kVActVals.at(st->vActIdx);
            auto raw    = hvb::reg::voltageFromV(std::stod(st->vThr));
            writeAsync(s, "V Limit", [&s, ch, mode, action, raw] {
                return s.client.writeVoltageProtection(ch, mode, action, raw);
            });
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid V-limit value"; }
    };
    auto onIProt = [&s, st, ch, &kIActVals] {
        try {
            auto mode   = static_cast<ProtectionMode>(st->iModeIdx);
            auto action = kIActVals.at(st->iActIdx);
            auto raw    = static_cast<int16_t>(std::stod(st->iThr) * 1000.0 + 0.5);
            writeAsync(s, "I Limit", [&s, ch, mode, action, raw] {
                return s.client.writeCurrentProtection(ch, mode, action, raw);
            });
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid I-limit value"; }
    };
    auto onSaveTarget = [&s, st, ch] {
        bool save = st->saveTargetIdx != 0;
        writeAsync(s, "SaveTarget", [&s, ch, save] { return s.client.writeSaveTargetPolicy(ch, save); });
    };

    auto tgtInp    = CommitInput(&st->targetV,  "+0.0",  onTarget);
    auto ruStepInp = CommitInput(&st->ruStep,   "0",     onRampUp);
    auto ruIntInp  = CommitInput(&st->ruInt,    "0",     onRampUp);
    auto rdStepInp = CommitInput(&st->rdStep,   "0",     onRampDown);
    auto rdIntInp  = CommitInput(&st->rdInt,    "0",     onRampDown);
    auto derInp    = CommitInput(&st->derateStep,"0",    onDerate);
    auto vModeC    = InlineCycler(kProtModes, &st->vModeIdx, onVProt);
    auto vActC     = InlineCycler(kVActNames, &st->vActIdx,  onVProt);
    auto vThrInp   = CommitInput(&st->vThr,    "+0.0",  onVProt);
    auto iModeC    = InlineCycler(kProtModes, &st->iModeIdx, onIProt);
    auto iActC     = InlineCycler(kIActNames, &st->iActIdx,  onIProt);
    auto iThrInp   = CommitInput(&st->iThr,    "0.000", onIProt);
    auto saveTgtC  = InlineCycler(kSaveTarget, &st->saveTargetIdx, onSaveTarget);

    auto bEnable  = ActionButton("Enable",    [&s,ch]{ writeAsync(s,"Enable",    [&s,ch]{ return s.client.sendOutputAction(ch, OutputAction::Enable); }); });
    auto bDisImm  = ActionButton("Dis-Immed", [&s,ch]{ writeAsync(s,"Dis-Immed", [&s,ch]{ return s.client.sendOutputAction(ch, OutputAction::DisableImmediate); }); });
    auto bDisGra  = ActionButton("Dis-Grace", [&s,ch]{ writeAsync(s,"Dis-Grace", [&s,ch]{ return s.client.sendOutputAction(ch, OutputAction::DisableGraceful); }); });
    auto bSave    = ActionButton("Save",      [&s,ch]{ writeAsync(s,"Save",      [&s,ch]{ return s.client.sendParamAction(ch, ParamAction::Save); }); });
    auto bLoad    = ActionButton("Load",      [&s,ch]{ writeAsync(s,"Load",      [&s,ch]{ return s.client.sendParamAction(ch, ParamAction::Load); }); });
    auto bFactory = ActionButton("Factory",   [&s,ch]{ writeAsync(s,"Factory",   [&s,ch]{ return s.client.sendParamAction(ch, ParamAction::FactoryReset); }); });
    auto bClrAct  = ActionButton("ClrActive", [&s,ch]{ writeAsync(s,"ClrActive", [&s,ch]{ return s.client.sendChannelFaultCommand(ch, ChannelFaultCommand::ClearActiveFaultBlock); }); });
    auto bClrHist = ActionButton("ClrHist",   [&s,ch]{ writeAsync(s,"ClrHist",   [&s,ch]{ return s.client.sendChannelFaultCommand(ch, ChannelFaultCommand::ClearFaultHistory); }); });

    auto container = Container::Vertical({
        tgtInp, bEnable, bDisImm, bDisGra,
        ruStepInp, ruIntInp, rdStepInp, rdIntInp, derInp,
        vModeC, vActC, vThrInp,
        iModeC, iActC, iThrInp,
        saveTgtC, bSave, bLoad, bFactory, bClrAct, bClrHist,
    });

    return Renderer(container, [=, &s, ch]() {
        // Live readings bar
        Element liveBar = text(" Not connected ") | dim;
        if (s.data.valid) {
            const auto& ci = s.data.chInfo[ch];
            {
                char lastFault[24] = "--";
                if (ci.lastFaultTimestamp > 0)
                    snprintf(lastFault, sizeof(lastFault), "%u s ago", (unsigned)ci.lastFaultTimestamp);
                liveBar = hbox({
                    text("  Vmeas: "),  text(fmtVoltage(ci.voltageRaw))  | bold,
                    text("   Imeas: "), text(fmtCurrentUA(ci.currentRaw)) | bold,
                    text("   Op Target: "), text(fmtVoltage(ci.operationalTargetVoltageRaw)),
                    text("   Status: "), text(statusBadge(ci.status)) | bold,
                    text("   Retries: "), text(std::to_string(ci.retryCount)),
                    text("\n  Active Fault: "), text(faultStr(ci.activeFault)),
                    text("   Fault History: "), text(faultStr(ci.faultHistory)),
                    text("   Cooldown: "), text(std::to_string(ci.cooldownSec) + " s"),
                    text("   Last Fault: "), text(lastFault),
                });
            }
        }

        auto outputPanel = window(text(" Output "), vbox({
            hbox({ text("Target V : "), tgtInp->Render(), text(" V") }),
            hbox({ bEnable->Render(), text("  "), bDisImm->Render(), text("  "), bDisGra->Render() }),
        }));

        auto rampPanel = window(text(" Ramping "), vbox({
            hbox({ text("Ramp Up   : step "), ruStepInp->Render(), text(" LSB  int "), ruIntInp->Render(), text(" \xc3\x970.1s") }),
            hbox({ text("Ramp Down : step "), rdStepInp->Render(), text(" LSB  int "), rdIntInp->Render(), text(" \xc3\x970.1s") }),
            hbox({ text("Derate Step:      "), derInp->Render(),   text(" LSB") }),
        }));

        auto vProtPanel = window(text(" Voltage Protection "), hbox({
            text("Mode : "), vModeC->Render(),
            text("   Action : "), vActC->Render(),
            text("   Threshold: "), vThrInp->Render(), text(" V"),
        }));

        auto iProtPanel = window(text(" Current Protection "), hbox({
            text("Mode : "), iModeC->Render(),
            text("   Action : "), iActC->Render(),
            text("   Threshold: "), iThrInp->Render(), text(" \xc2\xb5\x41"), // µA
        }));

        Element calInfo = text(" No data ") | dim;
        if (s.data.valid) {
            const auto& cc = s.data.chCfg[ch];
            calInfo = vbox({
                hbox({ text("Output  K: "), text(std::to_string(cc.outCalK)) | bold, text("  B: "), text(std::to_string(cc.outCalB)) | bold }),
                hbox({ text("Meas V  K: "), text(std::to_string(cc.measVCalK)) | bold, text("  B: "), text(std::to_string(cc.measVCalB)) | bold }),
                hbox({ text("Meas I  K: "), text(std::to_string(cc.measICalK)) | bold, text("  B: "), text(std::to_string(cc.measICalB)) | bold }),
            });
        }
        auto calPanel = window(text(" Calibration (read-only) "), calInfo);

        auto persistPanel = window(text(" Persistence "), vbox({
            hbox({ text("Save Target: "), saveTgtC->Render() }),
            separator(),
            hbox({ bSave->Render(), text("  "), bLoad->Render(), text("  "), bFactory->Render() }),
            hbox({ bClrAct->Render(), text("  "), bClrHist->Render() }),
        }));

        return vbox({
            window(text(" CH" + std::to_string(ch) + " Live "), liveBar),
            hbox({ outputPanel, rampPanel }),
            vProtPanel,
            iProtPanel,
            hbox({ calPanel, persistPanel }),
        });
    });
}

} // namespace hvb::tui
