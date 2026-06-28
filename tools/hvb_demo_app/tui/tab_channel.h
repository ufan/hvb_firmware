#pragma once
#include "widgets.h"
#include <memory>
#include <string>

namespace hvb::tui {

inline Component makeChannelTab(AppState& s, int ch) {
    static const std::vector<std::string> kProtModes  = {"Disabled","FlagOnly","Apply-Action"};
    static const std::vector<std::string> kIActNames  = {"None","Dis-Graceful","Dis-Immed","ForceZero"};
    static const std::vector<OutputAction> kIActVals  = {
        OutputAction::None, OutputAction::DisableGraceful,
        OutputAction::DisableImmediate, OutputAction::ForceOutputZero
    };
    static const std::vector<std::string> kRecovNames = {"ManualLatch","AutoRetry","AutoDerate","NeverRetry"};

    struct St {
        std::string targetV, iThr;
        std::string ruStep, ruInt, rdStep, rdInt, derateStep;
        std::string retryDelay, retryMax, retryWindow, iBand;
        int iModeIdx = 0, iActIdx = 0;
        int recovIdx = 0;
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
    auto onRecov = [&s, st, ch] {
        try {
            auto pol = static_cast<RecoveryPolicy>(st->recovIdx);
            int d = std::stoi(st->retryDelay), m = std::stoi(st->retryMax),
                w = std::stoi(st->retryWindow);
            writeAsync(s, "Recovery", [&s, ch, pol, d, m, w] {
                return s.client.writeChannelRecovery(ch, pol, d, m, w);
            });
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid recovery value"; }
    };
    auto onBand = [&s, st, ch] {
        try {
            uint16_t pct = (uint16_t)std::stoul(st->iBand);
            writeAsync(s, "SafeBand", [&s, ch, pct] { return s.client.writeChannelSafeBand(ch, pct); });
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid safe-band value"; }
    };

    auto tgtInp    = CommitInput(&st->targetV,   "+0.0",  onTarget);
    auto ruStepInp = CommitInput(&st->ruStep,    "0",     onRampUp);
    auto ruIntInp  = CommitInput(&st->ruInt,     "0",     onRampUp);
    auto rdStepInp = CommitInput(&st->rdStep,    "0",     onRampDown);
    auto rdIntInp  = CommitInput(&st->rdInt,     "0",     onRampDown);
    auto derInp    = CommitInput(&st->derateStep,"0",     onDerate);
    auto iModeC    = InlineCycler(kProtModes,  &st->iModeIdx, onIProt);
    auto iActC     = InlineCycler(kIActNames,  &st->iActIdx,  onIProt);
    auto iThrInp   = CommitInput(&st->iThr,    "0.000", onIProt);
    auto recovC    = InlineCycler(kRecovNames, &st->recovIdx, onRecov);
    auto delayInp  = CommitInput(&st->retryDelay,  "0",  onRecov);
    auto maxInp    = CommitInput(&st->retryMax,    "3",   onRecov);
    auto winInp    = CommitInput(&st->retryWindow, "60",  onRecov);
    auto iBandInp  = CommitInput(&st->iBand,       "10",  onBand);

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
        iModeC, iActC, iThrInp,
        recovC, delayInp, maxInp, winInp, iBandInp,
        bSave, bLoad, bFactory, bClrAct, bClrHist,
    });

    return Renderer(container, [=, &s, ch]() {
        // Channel beyond device's reported count — show placeholder.
        if (s.data.valid && ch >= s.data.numChannels())
            return text(" CH" + std::to_string(ch) + " not present on this device ") | dim | center;

        // Capability flags — all bits set when disconnected (show everything).
        const uint16_t caps = s.data.valid ? s.data.chInfo[ch].chCapFlags : 0xFFFFu;
        const bool hasOutEn = (caps & CH_CAP_OUTPUT_ENABLE) != 0;
        const bool hasVolts = (caps & CH_CAP_VOLTAGE_MEASUREMENT) != 0;
        const bool hasCurr  = (caps & CH_CAP_CURRENT_MEASUREMENT) != 0;

        Element liveBar = text(" Not connected ") | dim;
        if (s.data.valid) {
            const auto& ci = s.data.chInfo[ch];
            char lastFault[24] = "--";
            if (ci.lastFaultTimestamp > 0)
                snprintf(lastFault, sizeof(lastFault), "%u s ago", (unsigned)ci.lastFaultTimestamp);
            Elements liveParts;
            if (hasVolts) { liveParts.push_back(text("  Vmeas: ")); liveParts.push_back(text(fmtVoltage(ci.voltageRaw)) | bold); }
            if (hasCurr)  { liveParts.push_back(text("   Imeas: ")); liveParts.push_back(text(fmtCurrentUA(ci.currentRaw)) | bold); }
            liveParts.push_back(text("   Op Target: "));
            liveParts.push_back(text(fmtVoltage(ci.operationalTargetVoltageRaw)));
            liveParts.push_back(text("   Status: "));
            liveParts.push_back(text(statusBadge(ci.status)) | bold);
            liveParts.push_back(text("   Retries: "));
            liveParts.push_back(text(std::to_string(ci.retryCount)));
            liveBar = hbox(std::move(liveParts));
        }

        auto outputPanel = window(text(" Output "), vbox({
            hbox({ text("Target V : "), tgtInp->Render(), text(" V") }),
            hasOutEn
                ? hbox({ bEnable->Render(), text("  "), bDisImm->Render(), text("  "), bDisGra->Render() })
                : hbox({ text("  (output control not supported) ") | dim }),
        }));

        auto rampPanel = window(text(" Ramping "), vbox({
            hbox({ text("Ramp Up   : step "), ruStepInp->Render(), text(" LSB  int "), ruIntInp->Render(), text(" \xc3\x970.1s") }),
            hbox({ text("Ramp Down : step "), rdStepInp->Render(), text(" LSB  int "), rdIntInp->Render(), text(" \xc3\x970.1s") }),
            hbox({ text("Derate Step:      "), derInp->Render(),   text(" LSB") }),
        }));

        auto recovPanel = window(text(" Recovery "), vbox({
            hbox({ text("Policy    : "), recovC->Render() }),
            hbox({ text("Delay     : "), delayInp->Render(), text(" s"),
                   text("  Max: "), maxInp->Render(),
                   text("  Window: "), winInp->Render(), text(" s") }),
            hbox({ text("I Safe Band: "), iBandInp->Render(), text(" % (0-50)") }),
        }));

        auto persistPanel = window(text(" Persistence "), vbox({
            hbox({ bSave->Render(), text("  "), bLoad->Render(), text("  "), bFactory->Render() }),
            hbox({ bClrAct->Render(), text("  "), bClrHist->Render() }),
        }));

        Element calInfo = text(" No data ") | dim;
        if (s.data.valid) {
            const auto& cc = s.data.chCalCfg[ch];
            Elements calRows;
            calRows.push_back(hbox({ text("Output  K: "), text(std::to_string(cc.outCalK))  | bold, text("  B: "), text(std::to_string(cc.outCalB))  | bold }));
            if (hasVolts) calRows.push_back(hbox({ text("Meas V  K: "), text(std::to_string(cc.measVCalK)) | bold, text("  B: "), text(std::to_string(cc.measVCalB)) | bold }));
            if (hasCurr)  calRows.push_back(hbox({ text("Meas I  K: "), text(std::to_string(cc.measICalK)) | bold, text("  B: "), text(std::to_string(cc.measICalB)) | bold }));
            calInfo = vbox(std::move(calRows));
        }
        auto calPanel = window(text(" Calibration (read-only) "), calInfo);

        Elements rows;
        rows.push_back(window(text(" CH" + std::to_string(ch) + " Live "), liveBar));
        rows.push_back(hbox({ outputPanel, rampPanel }));
        rows.push_back(recovPanel);
        if (hasCurr) {
            rows.push_back(window(text(" Current Protection "), hbox({
                text("Mode : "), iModeC->Render(),
                text("   Action : "), iActC->Render(),
                text("   Threshold: "), iThrInp->Render(), text(" \xc2\xb5\x41"), // µA
            })));
        }
        rows.push_back(hbox({ calPanel, persistPanel }));
        return vbox(std::move(rows));
    });
}

} // namespace hvb::tui
