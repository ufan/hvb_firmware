#pragma once
#include "tui_policy.h"
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
        syncDataToInputs(s.data, inputs);
    };

    // ---- Output / Control ----
    auto onTarget = [&s, &inputs, refreshCh, ch] {
        try {
            auto raw = reg::voltageFromV(std::stod(inputs.targetV[ch]));
            postWrite(s, inputs, "Target V",
                [&s, ch, raw] { return s.client.writeConfiguredTargetVoltage(ch, raw); }, refreshCh);
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid voltage"; }
    };
    auto onRampUp = [&s, &inputs, refreshCh, ch] {
        try {
            auto stepRaw = reg::voltageFromV(std::stod(inputs.ruStep[ch]));
            auto iv = s.data.chCfg[ch].rampUpInterval;
            postWrite(s, inputs, "Ramp Up",
                [&s, ch, stepRaw, iv] { return s.client.writeRampUp(ch, stepRaw, iv); }, refreshCh);
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid ramp value"; }
    };
    auto onRampDown = [&s, &inputs, refreshCh, ch] {
        try {
            auto stepRaw = reg::voltageFromV(std::stod(inputs.rdStep[ch]));
            auto iv = s.data.chCfg[ch].rampDownInterval;
            postWrite(s, inputs, "Ramp Down",
                [&s, ch, stepRaw, iv] { return s.client.writeRampDown(ch, stepRaw, iv); }, refreshCh);
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid ramp value"; }
    };

    auto tgtInp    = CommitInput(&inputs.targetV[ch],   "+0.0", onTarget);
    auto ruStepInp = CommitInput(&inputs.ruStep[ch],    "0.0",  onRampUp);
    auto rdStepInp = CommitInput(&inputs.rdStep[ch],    "0.0",  onRampDown);

    auto bEnable  = ActionButton("Enable",    [&s, &inputs, refreshCh, ch]{
        postWrite(s, inputs, "Enable", [&s, ch]{ return s.client.sendOutputAction(ch, OutputAction::Enable); }, refreshCh); });
    auto bDisGra  = ActionButton("Disable",   [&s, &inputs, refreshCh, ch]{
        postWrite(s, inputs, "Disable", [&s, ch]{ return s.client.sendOutputAction(ch, OutputAction::DisableGraceful); }, refreshCh); });
    auto bKill    = ActionButton("Kill",      [&s, &inputs, refreshCh, ch]{
        postWrite(s, inputs, "Kill", [&s, ch]{ return s.client.sendOutputAction(ch, OutputAction::DisableImmediate); }, refreshCh); });

    // ---- Protection ----
    auto onIProt = [&s, &inputs, refreshCh, ch] {
        try {
            auto mode   = static_cast<ProtectionMode>(inputs.iModeIdx[ch]);
            auto action = kIActVals.at(inputs.iActIdx[ch]);
            auto raw    = static_cast<int16_t>(std::stod(inputs.iThr[ch]) + 0.5);
            postWrite(s, inputs, "I Limit",
                [&s, ch, mode, action, raw] { return s.client.writeCurrentProtection(ch, mode, action, raw); }, refreshCh);
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid I-limit value"; }
    };
    auto iModeC  = InlineCycler(kProtModes, &inputs.iModeIdx[ch], onIProt);
    auto iActC   = InlineCycler(kIActNames, &inputs.iActIdx[ch], onIProt);
    auto iThrInp = CommitInput(&inputs.iThr[ch], "0", onIProt);

    // ---- Recovery ----
    auto onRecov = [&s, &inputs, refreshCh, ch] {
        try {
            auto pol = static_cast<RecoveryPolicy>(inputs.recovIdx[ch]);
            int d = std::stoi(inputs.retryDelay[ch]), m = std::stoi(inputs.retryMax[ch]),
                w = std::stoi(inputs.retryWindow[ch]);
            postWrite(s, inputs, "Recovery",
                [&s, ch, pol, d, m, w] { return s.client.writeChannelRecovery(ch, pol, d, m, w); }, refreshCh);
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid recovery value"; }
    };
    auto onDerate = [&s, &inputs, refreshCh, ch] {
        try {
            auto step = (uint16_t)std::stoul(inputs.derateStep[ch]);
            postWrite(s, inputs, "Derate",
                [&s, ch, step] { return s.client.writeDerateStep(ch, step); }, refreshCh);
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid derate step"; }
    };
    auto onBand = [&s, &inputs, refreshCh, ch] {
        try {
            uint16_t pct = (uint16_t)std::stoul(inputs.iBand[ch]);
            postWrite(s, inputs, "SafeBand",
                [&s, ch, pct] { return s.client.writeChannelSafeBand(ch, pct); }, refreshCh);
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid safe-band value"; }
    };
    auto recovC   = InlineCycler(kRecovNames, &inputs.recovIdx[ch], onRecov);
    auto delayInp = CommitInput(&inputs.retryDelay[ch],  "0",  onRecov);
    auto maxInp   = CommitInput(&inputs.retryMax[ch],    "3",  onRecov);
    auto winInp   = CommitInput(&inputs.retryWindow[ch], "60", onRecov);
    auto derInp   = CommitInput(&inputs.derateStep[ch],  "0",  onDerate);
    auto iBandInp = CommitInput(&inputs.iBand[ch],       "10", onBand);

    auto bClrAct  = ActionButton("ClrActive", [&s, &inputs, refreshCh, ch]{
        postWrite(s, inputs, "ClrActive", [&s, ch]{ return s.client.sendChannelFaultCommand(ch, ChannelFaultCommand::ClearActiveFaultBlock); }, refreshCh); });
    auto bClrHist = ActionButton("ClrHist",   [&s, &inputs, refreshCh, ch]{
        postWrite(s, inputs, "ClrHist", [&s, ch]{ return s.client.sendChannelFaultCommand(ch, ChannelFaultCommand::ClearFaultHistory); }, refreshCh); });

    // ---- Persistence ----
    auto bSave    = ActionButton("Save",    [&s, &inputs, refreshCh, ch]{
        postWrite(s, inputs, "Save", [&s, ch]{ return s.client.sendParamAction(ch, ParamAction::Save); }, refreshCh); });
    auto bLoad    = ActionButton("Load",    [&s, &inputs, refreshCh, ch]{
        postWrite(s, inputs, "Load", [&s, ch]{ return s.client.sendParamAction(ch, ParamAction::Load); }, refreshCh); });
    auto bFactory = ActionButton("Factory", [&s, &inputs, refreshCh, ch]{
        postWrite(s, inputs, "Factory", [&s, ch]{ return s.client.sendParamAction(ch, ParamAction::FactoryReset); }, refreshCh); });

    auto hasOutput = [&s, ch] {
        return !s.data.valid ||
               (s.data.chInfo[ch].chCapFlags & CH_CAP_OUTPUT_ENABLE) != 0;
    };
    auto hasProtection = [&s, ch] {
        return !s.data.valid ||
               hasProtectionPolicy(s.data.chInfo[ch].chCapFlags);
    };

    auto outputControls = Container::Horizontal({bEnable, bDisGra, bKill});
    auto visibleOutputControls = Maybe(outputControls, hasOutput);
    auto protectionControls = Container::Vertical({
        iModeC, iActC, iThrInp, bClrAct, bClrHist,
    });
    auto visibleProtectionControls = Maybe(protectionControls, hasProtection);

    auto container = Container::Vertical({
        visibleOutputControls, tgtInp,
        ruStepInp, rdStepInp,
        visibleProtectionControls,
        recovC, delayInp, maxInp, winInp, derInp, iBandInp,
        bSave, bLoad, bFactory,
    });

    return Renderer(container, [=, &s]() {
        if (s.data.valid && ch >= s.data.numChannels())
            return text(" CH" + std::to_string(ch) + " not present on this device ") | dim | center;

        const uint16_t caps = s.data.valid ? s.data.chInfo[ch].chCapFlags : 0xFFFFu;
        const bool hasVolts = (caps & CH_CAP_VOLTAGE_MEASUREMENT) != 0;
        const bool hasCurr  = (caps & CH_CAP_CURRENT_MEASUREMENT) != 0;

        // LiveStatus panel (single row)
        Element liveBar = text(" Not connected ") | dim;
        if (s.data.valid) {
            const auto& ci = s.data.chInfo[ch];
            Elements liveParts;
            if (hasVolts) { liveParts.push_back(text("  Vop: ")); liveParts.push_back(text(fmtVoltage(ci.operationalTargetVoltageRaw)) | bold); }
            if (hasVolts) { liveParts.push_back(text("   V: "));  liveParts.push_back(text(fmtVoltage(ci.voltageRaw)) | bold); }
            if (hasCurr)  { liveParts.push_back(text("   I: "));  liveParts.push_back(text(fmtCurrentNA(ci.currentRaw)) | bold); }
            liveParts.push_back(text("   Status: "));
            liveParts.push_back(text(statusBadge(ci.status)) | bold);
            liveParts.push_back(text("   Retries: "));
            liveParts.push_back(text(std::to_string(ci.retryCount)));
            liveBar = hbox(std::move(liveParts));
        }
        auto livePanel = hbox({
            text(" Live ") | bold | color(Color::Cyan),
            separator(),
            liveBar,
        });

        // Control panel
        auto controlPanel = window(text(" Control "), vbox({
            emptyElement(),
            hasOutput()
                ? hbox({ bEnable->Render(), text(" "), bDisGra->Render(),
                         text(" "), bKill->Render() })
                : text(" output control not supported ") | dim,
            hbox({ text("Vset    : "), tgtInp->Render() | flex, text(" V") }),
            hbox({ text("Ramp up : "), ruStepInp->Render() | flex, text(" V/s") }),
            hbox({ text("Ramp dn : "), rdStepInp->Render() | flex, text(" V/s") }),
            filler(),
        }));

        // Protection panel
        Element protPanel = emptyElement();
        if (hasProtection()) {
            protPanel = window(text(" Protection Policy "), vbox({
                emptyElement(),
                hbox({ text("Limit  : "), iThrInp->Render() | flex, text(" nA") }),
                hbox({ text("Mode   : "), iModeC->Render() | flex }),
                hbox({ text("Action : "), iActC->Render() | flex }),
                hbox({ bClrAct->Render(), text("  "), bClrHist->Render() }),
                filler(),
            }));
        }

        // Recovery panel
        auto recovPanel = window(text(" Recovery Policy "), vbox({
            emptyElement(),
            hbox({ text("Policy : "), recovC->Render() | flex }),
            hbox({ text("Max    : "), maxInp->Render() | flex,
                   text("  Win: "), winInp->Render() | flex, text(" s") }),
            hbox({ text("Delay  : "), delayInp->Render() | flex, text(" s") }),
            hbox({ text("Derate : "), derInp->Render() | flex, text(" LSB") }),
            hbox({ text("Band   : "), iBandInp->Render() | flex, text(" %") }),
            filler(),
        }));

        // Persistence / Setting panel
        auto persistPanel = window(text(" Setting "), vbox({
            emptyElement(),
            hbox({ bSave->Render(), text("  "), bLoad->Render(), text("  "), bFactory->Render() }),
            filler(),
        }));

        auto leftColumn = vbox({
            controlPanel | flex,
            persistPanel,
        });
        auto rightColumn = vbox({
            protPanel,
            recovPanel | flex,
        });

        return vbox({
            emptyElement() | size(HEIGHT, EQUAL, 1),
            livePanel,
            emptyElement() | size(HEIGHT, EQUAL, 1),
            hbox({ leftColumn | flex, rightColumn | flex }) | flex,
            filler(),
        });
    });
}

} // namespace hvb::tui
