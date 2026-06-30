#pragma once
#include "widgets.h"
#include <memory>
#include <string>

namespace hvb::tui {

inline Component makeChannelTab(AppState& s, ConfigInputs& inputs, int ch) {
    static const std::vector<std::string> kProtModes  = {"Disabled","FlagOnly","Apply-Action"};
    static const std::vector<std::string> kIActNames  = {"None","Dis-Graceful","Dis-Immed","ForceZero"};
    static const std::vector<OutputAction> kIActVals  = {
        OutputAction::None, OutputAction::DisableGraceful,
        OutputAction::DisableImmediate, OutputAction::ForceOutputZero
    };
    static const std::vector<std::string> kRecovNames = {"ManualLatch","AutoRetry","AutoDerate","NeverRetry"};

    auto refreshCh = [&s, &inputs, ch]() {
        s.data.chCfg[ch] = s.client.readChannelConfig(ch, s.data.chInfo[ch].chCapFlags);
        s.data.chCalCfg[ch] = s.client.readChannelCalConfig(ch, s.data.chInfo[ch].chCapFlags);
        syncDataToInputs(s.data, inputs);
    };

    auto onTarget = [&s, &inputs, refreshCh, ch] {
        try {
            auto raw = reg::voltageFromV(std::stod(inputs.targetV[ch]));
            writeSync(s, inputs, "Target V",
                [&s, ch, raw] { return s.client.writeConfiguredTargetVoltage(ch, raw); },
                refreshCh);
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid voltage"; }
    };
    auto onRampUp = [&s, &inputs, refreshCh, ch] {
        try {
            auto stepRaw = reg::voltageFromV(std::stod(inputs.ruStep[ch]));
            auto iv = s.data.chCfg[ch].rampUpInterval;
            writeSync(s, inputs, "Ramp Up",
                [&s, ch, stepRaw, iv] { return s.client.writeRampUp(ch, stepRaw, iv); },
                refreshCh);
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid ramp value"; }
    };
    auto onRampDown = [&s, &inputs, refreshCh, ch] {
        try {
            auto stepRaw = reg::voltageFromV(std::stod(inputs.rdStep[ch]));
            auto iv = s.data.chCfg[ch].rampDownInterval;
            writeSync(s, inputs, "Ramp Down",
                [&s, ch, stepRaw, iv] { return s.client.writeRampDown(ch, stepRaw, iv); },
                refreshCh);
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid ramp value"; }
    };
    auto onDerate = [&s, &inputs, refreshCh, ch] {
        try {
            auto step = (uint16_t)std::stoul(inputs.derateStep[ch]);
            writeSync(s, inputs, "Derate",
                [&s, ch, step] { return s.client.writeDerateStep(ch, step); },
                refreshCh);
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid derate step"; }
    };
    auto onIProt = [&s, &inputs, refreshCh, ch] {
        try {
            auto mode   = static_cast<ProtectionMode>(inputs.iModeIdx[ch]);
            auto action = kIActVals.at(inputs.iActIdx[ch]);
            auto raw    = static_cast<int16_t>(std::stod(inputs.iThr[ch]) + 0.5);
            writeSync(s, inputs, "I Limit",
                [&s, ch, mode, action, raw] { return s.client.writeCurrentProtection(ch, mode, action, raw); },
                refreshCh);
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid I-limit value"; }
    };
    auto onRecov = [&s, &inputs, refreshCh, ch] {
        try {
            auto pol = static_cast<RecoveryPolicy>(inputs.recovIdx[ch]);
            int d = std::stoi(inputs.retryDelay[ch]), m = std::stoi(inputs.retryMax[ch]),
                w = std::stoi(inputs.retryWindow[ch]);
            writeSync(s, inputs, "Recovery",
                [&s, ch, pol, d, m, w] { return s.client.writeChannelRecovery(ch, pol, d, m, w); },
                refreshCh);
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid recovery value"; }
    };
    auto onBand = [&s, &inputs, refreshCh, ch] {
        try {
            uint16_t pct = (uint16_t)std::stoul(inputs.iBand[ch]);
            writeSync(s, inputs, "SafeBand",
                [&s, ch, pct] { return s.client.writeChannelSafeBand(ch, pct); },
                refreshCh);
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid safe-band value"; }
    };

    auto tgtInp    = CommitInput(&inputs.targetV[ch],   "+0.0",  onTarget);
    auto ruStepInp = CommitInput(&inputs.ruStep[ch],    "0.0",    onRampUp);
    auto rdStepInp = CommitInput(&inputs.rdStep[ch],    "0.0",    onRampDown);
    auto derInp    = CommitInput(&inputs.derateStep[ch],"0",     onDerate);
    auto iModeC    = InlineCycler(kProtModes,  &inputs.iModeIdx[ch], onIProt);
    auto iActC     = InlineCycler(kIActNames,  &inputs.iActIdx[ch],  onIProt);
    auto iThrInp   = CommitInput(&inputs.iThr[ch],    "0",    onIProt);
    auto recovC    = InlineCycler(kRecovNames, &inputs.recovIdx[ch], onRecov);
    auto delayInp  = CommitInput(&inputs.retryDelay[ch],  "0",  onRecov);
    auto maxInp    = CommitInput(&inputs.retryMax[ch],    "3",  onRecov);
    auto winInp    = CommitInput(&inputs.retryWindow[ch], "60", onRecov);
    auto iBandInp  = CommitInput(&inputs.iBand[ch],       "10", onBand);

    auto bEnable  = ActionButton("Enable",    [&s, &inputs, refreshCh, ch]{
        writeSync(s, inputs, "Enable", [&s, ch]{ return s.client.sendOutputAction(ch, OutputAction::Enable); }, refreshCh); });
    auto bDisImm  = ActionButton("Dis-Immed", [&s, &inputs, refreshCh, ch]{
        writeSync(s, inputs, "Dis-Immed", [&s, ch]{ return s.client.sendOutputAction(ch, OutputAction::DisableImmediate); }, refreshCh); });
    auto bDisGra  = ActionButton("Dis-Grace", [&s, &inputs, refreshCh, ch]{
        writeSync(s, inputs, "Dis-Grace", [&s, ch]{ return s.client.sendOutputAction(ch, OutputAction::DisableGraceful); }, refreshCh); });
    auto bSave    = ActionButton("Save",      [&s, &inputs, refreshCh, ch]{
        writeSync(s, inputs, "Save", [&s, ch]{ return s.client.sendParamAction(ch, ParamAction::Save); }, refreshCh); });
    auto bLoad    = ActionButton("Load",      [&s, &inputs, refreshCh, ch]{
        writeSync(s, inputs, "Load", [&s, ch]{ return s.client.sendParamAction(ch, ParamAction::Load); }, refreshCh); });
    auto bFactory = ActionButton("Factory",   [&s, &inputs, refreshCh, ch]{
        writeSync(s, inputs, "Factory", [&s, ch]{ return s.client.sendParamAction(ch, ParamAction::FactoryReset); }, refreshCh); });
    auto bClrAct  = ActionButton("ClrActive", [&s, &inputs, refreshCh, ch]{
        writeSync(s, inputs, "ClrActive", [&s, ch]{ return s.client.sendChannelFaultCommand(ch, ChannelFaultCommand::ClearActiveFaultBlock); }, refreshCh); });
    auto bClrHist = ActionButton("ClrHist",   [&s, &inputs, refreshCh, ch]{
        writeSync(s, inputs, "ClrHist", [&s, ch]{ return s.client.sendChannelFaultCommand(ch, ChannelFaultCommand::ClearFaultHistory); }, refreshCh); });

    auto container = Container::Vertical({
        tgtInp, bEnable, bDisImm, bDisGra,
        ruStepInp, rdStepInp,
        iModeC, iActC, iThrInp,
        recovC, delayInp, maxInp, winInp, iBandInp, derInp,
        bClrAct, bClrHist,
        bSave, bLoad, bFactory,
    });

    return Renderer(container, [=, &s]() {
        if (s.data.valid && ch >= s.data.numChannels())
            return text(" CH" + std::to_string(ch) + " not present on this device ") | dim | center;

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
            if (hasCurr)  { liveParts.push_back(text("   Imeas: ")); liveParts.push_back(text(fmtCurrentNA(ci.currentRaw)) | bold); }
            liveParts.push_back(text("   Vt: "));
            liveParts.push_back(text(fmtVoltage(ci.operationalTargetVoltageRaw)));
            liveParts.push_back(text("   Status: "));
            liveParts.push_back(text(statusBadge(ci.status)) | bold);
            liveParts.push_back(text("   Retries: "));
            liveParts.push_back(text(std::to_string(ci.retryCount)));
            liveBar = hbox(std::move(liveParts));
        }

        auto outputPanel = window(text(" Output "), vbox({
            hbox({ text("Vset : "), tgtInp->Render(), text(" V") }),
            hasOutEn
                ? hbox({ bEnable->Render(), text("  "), bDisImm->Render(), text("  "), bDisGra->Render() })
                : hbox({ text("  (output control not supported) ") | dim }),
        }));

        auto rampPanel = window(text(" Ramping "), vbox({
            hbox({ text("Ramp Up   : "), ruStepInp->Render(), text(" V") }),
            hbox({ text("Ramp Down : "), rdStepInp->Render(), text(" V") }),
        }));

        auto recovPanel = window(text(" Recovery "), vbox({
            hbox({ text("Policy    : "), recovC->Render() }),
            hbox({ text("Delay     : "), delayInp->Render(), text(" s"),
                   text("  Max: "), maxInp->Render(),
                   text("  Window: "), winInp->Render(), text(" s") }),
            hbox({ text("Derate    : "), derInp->Render(),   text(" LSB") }),
            hbox({ text("I Safe Band: "), iBandInp->Render(), text(" % (0-50)") }),
            hbox({ bClrAct->Render(), text("  "), bClrHist->Render() }),
        }));

        auto persistPanel = window(text(" Persistence "), vbox({
            hbox({ bSave->Render(), text("  "), bLoad->Render(), text("  "), bFactory->Render() }),
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
                text("   Threshold: "), iThrInp->Render(), text(" nA"),
            })));
        }
        rows.push_back(hbox({ calPanel, persistPanel }));
        return vbox(std::move(rows));
    });
}

} // namespace hvb::tui
