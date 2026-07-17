#pragma once
#include "tui_policy.h"
#include "widgets.h"
#include <memory>
#include <string>

namespace psb::tui {

inline Component makeChannelTab(AppState& s, ConfigInputs& inputs, int ch) {
    static const std::vector<std::string> kProtModes  = {"Disabled","FlagOnly","Apply-Action"};
    static const std::vector<std::string> kIActNames  = {"None","Dis-Graceful","Dis-Immed","ForceZero"};
    static const std::vector<OutputAction> kIActVals  = {
        OutputAction::None, OutputAction::DisableGraceful,
        OutputAction::DisableImmediate, OutputAction::ForceOutputZero
    };
    static const std::vector<std::string> kRecovNames = {"ManualLatch","AutoRetry","AutoDerate","NeverRetry"};

    // Narrow, action-specific refreshes — each re-reads only the Modbus
    // block the corresponding write actually touched (merging in place via
    // the reference-taking client methods, never a wholesale struct
    // replace), instead of one catch-all readChannelConfig() re-read of
    // everything after every click. See the identical pattern (with more
    // detail) in tab_monitor.h.
    auto refreshStatus = [&s, ch]() {
        s.client.readChannelStatus(ch, s.data.chInfo[ch].chCapFlags, s.data.chInfo[ch]);
    };
    auto refreshOutput = [&s, &inputs, ch]() {
        s.client.readChannelStatus(ch, s.data.chInfo[ch].chCapFlags, s.data.chInfo[ch]);
        s.client.readChannelOutputBlock(ch, s.data.chInfo[ch].chCapFlags, s.data.chCfg[ch]);
        syncDataToInputs(s.data, inputs);
    };
    auto refreshOutputEnabled = [&s, &inputs, ch]() {
        s.client.readChannelStatus(ch, s.data.chInfo[ch].chCapFlags, s.data.chInfo[ch]);
        s.client.readChannelOutputEnabledBlock(ch, s.data.chInfo[ch].chCapFlags, s.data.chCfg[ch]);
        syncDataToInputs(s.data, inputs);
    };
    auto refreshProtection = [&s, &inputs, ch]() {
        s.client.readChannelStatus(ch, s.data.chInfo[ch].chCapFlags, s.data.chInfo[ch]);
        s.client.readChannelProtectionBlock(ch, s.data.chInfo[ch].chCapFlags, s.data.chCfg[ch]);
        syncDataToInputs(s.data, inputs);
    };
    auto refreshRecovery = [&s, &inputs, ch]() {
        s.client.readChannelStatus(ch, s.data.chInfo[ch].chCapFlags, s.data.chInfo[ch]);
        s.client.readChannelRecoveryBlock(ch, s.data.chCfg[ch]);
        syncDataToInputs(s.data, inputs);
    };
    auto refreshDerate = [&s, &inputs, ch]() {
        s.client.readChannelStatus(ch, s.data.chInfo[ch].chCapFlags, s.data.chInfo[ch]);
        s.client.readChannelDerateBlock(ch, s.data.chInfo[ch].chCapFlags, s.data.chCfg[ch]);
        syncDataToInputs(s.data, inputs);
    };
    auto refreshFull = [&s, &inputs, ch]() {
        s.client.readChannelStatus(ch, s.data.chInfo[ch].chCapFlags, s.data.chInfo[ch]);
        s.client.readChannelConfig(ch, s.data.chInfo[ch].chCapFlags, s.data.chCfg[ch]);
        syncDataToInputs(s.data, inputs);
    };

    // ---- Output / Control ----
    auto onTarget = [&s, &inputs, refreshOutput, ch] {
        try {
            auto raw = reg::voltageFromV(std::stod(inputs.targetV[ch]));
            postWrite(s, inputs, "Target V",
                [&s, ch, raw] { return s.client.writeConfiguredTargetVoltage(ch, raw); }, refreshOutput);
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid voltage"; }
    };
    auto onRampUp = [&s, &inputs, refreshOutput, ch] {
        try {
            auto stepRaw = reg::voltageFromV(std::stod(inputs.ruStep[ch]));
            auto iv = s.data.chCfg[ch].rampUpInterval;
            postWrite(s, inputs, "Ramp Up",
                [&s, ch, stepRaw, iv] { return s.client.writeRampUp(ch, stepRaw, iv); }, refreshOutput);
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid ramp value"; }
    };
    auto onRampDown = [&s, &inputs, refreshOutput, ch] {
        try {
            auto stepRaw = reg::voltageFromV(std::stod(inputs.rdStep[ch]));
            auto iv = s.data.chCfg[ch].rampDownInterval;
            postWrite(s, inputs, "Ramp Down",
                [&s, ch, stepRaw, iv] { return s.client.writeRampDown(ch, stepRaw, iv); }, refreshOutput);
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid ramp value"; }
    };

    auto tgtInp    = CommitInput(&inputs.targetV[ch],   "+0.0", onTarget);
    auto ruStepInp = CommitInput(&inputs.ruStep[ch],    "0.0",  onRampUp);
    auto rdStepInp = CommitInput(&inputs.rdStep[ch],    "0.0",  onRampDown);

    // Fixed-voltage channel counterpart to tgtInp (CH_CAP_OUTPUT_ENABLE
    // without CH_CAP_RAW_OUTPUT_DRIVE — no DAC, no ramp, just on/off).
    auto onOutputEnabledCfg = [&s, &inputs, refreshOutputEnabled, ch] {
        bool on = inputs.outputEnabledIdx[ch] != 0;
        postWrite(s, inputs, "Output Enabled",
            [&s, ch, on] { return s.client.writeOutputEnabled(ch, on); }, refreshOutputEnabled);
    };
    auto outputEnabledCyc = InlineCycler({"Off", "On"}, &inputs.outputEnabledIdx[ch],
        onOutputEnabledCfg, /*autoCommit=*/true);

    auto bEnable  = ActionButton("Enable",    [&s, &inputs, refreshStatus, ch]{
        postWrite(s, inputs, "Enable", [&s, ch]{ return s.client.sendOutputAction(ch, OutputAction::Enable); }, refreshStatus); });
    auto bDisGra  = ActionButton("Disable",   [&s, &inputs, refreshStatus, ch]{
        postWrite(s, inputs, "Disable", [&s, ch]{ return s.client.sendOutputAction(ch, OutputAction::DisableGraceful); }, refreshStatus); });
    auto bKill    = ActionButton("Kill",      [&s, &inputs, refreshStatus, ch]{
        postWrite(s, inputs, "Kill", [&s, ch]{ return s.client.sendOutputAction(ch, OutputAction::DisableImmediate); }, refreshStatus); });

    // ---- Protection ----
    auto onIProt = [&s, &inputs, refreshProtection, ch] {
        try {
            auto mode   = static_cast<ProtectionMode>(inputs.iModeIdx[ch]);
            auto action = kIActVals.at(inputs.iActIdx[ch]);
            // Input is in the board's fixed display unit (currentUnitFor),
            // not plain amps — convert back before currentFromA, mirroring
            // the sync side in widgets.h.
            CurrentUnit iu = currentUnitFor(s.data.sysInfo.currentUnitExp);
            auto raw    = reg::currentFromA(std::stod(inputs.iThr[ch]) / iu.scale,
                                             s.data.sysInfo.currentUnitExp);
            postWrite(s, inputs, "I Limit",
                [&s, ch, mode, action, raw] { return s.client.writeCurrentProtection(ch, mode, action, raw); }, refreshProtection);
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid I-limit value"; }
    };
    auto iModeC  = InlineCycler(kProtModes, &inputs.iModeIdx[ch], onIProt);
    auto iActC   = InlineCycler(kIActNames, &inputs.iActIdx[ch], onIProt);
    auto iThrInp = CommitInput(&inputs.iThr[ch], "0", onIProt);

    // ---- Recovery ----
    auto onRecov = [&s, &inputs, refreshRecovery, ch] {
        try {
            auto pol = static_cast<RecoveryPolicy>(inputs.recovIdx[ch]);
            int d = std::stoi(inputs.retryDelay[ch]), m = std::stoi(inputs.retryMax[ch]),
                w = std::stoi(inputs.retryWindow[ch]);
            postWrite(s, inputs, "Recovery",
                [&s, ch, pol, d, m, w] { return s.client.writeChannelRecovery(ch, pol, d, m, w); }, refreshRecovery);
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid recovery value"; }
    };
    auto onDerate = [&s, &inputs, refreshDerate, ch] {
        try {
            auto step = (uint16_t)std::stoul(inputs.derateStep[ch]);
            postWrite(s, inputs, "Derate",
                [&s, ch, step] { return s.client.writeDerateStep(ch, step); }, refreshDerate);
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid derate step"; }
    };
    auto onBand = [&s, &inputs, refreshRecovery, ch] {
        try {
            uint16_t pct = (uint16_t)std::stoul(inputs.iBand[ch]);
            postWrite(s, inputs, "SafeBand",
                [&s, ch, pct] { return s.client.writeChannelSafeBand(ch, pct); }, refreshRecovery);
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid safe-band value"; }
    };
    auto recovC   = InlineCycler(kRecovNames, &inputs.recovIdx[ch], onRecov);
    auto delayInp = CommitInput(&inputs.retryDelay[ch],  "0",  onRecov);
    auto maxInp   = CommitInput(&inputs.retryMax[ch],    "3",  onRecov);
    auto winInp   = CommitInput(&inputs.retryWindow[ch], "60", onRecov);
    auto derInp   = CommitInput(&inputs.derateStep[ch],  "0",  onDerate);
    auto iBandInp = CommitInput(&inputs.iBand[ch],       "10", onBand);

    auto bClrAct  = ActionButton("ClrActive", [&s, &inputs, refreshStatus, ch]{
        postWrite(s, inputs, "ClrActive", [&s, ch]{ return s.client.sendChannelFaultCommand(ch, ChannelFaultCommand::ClearActiveFaultBlock); }, refreshStatus); });
    auto bClrHist = ActionButton("ClrHist",   [&s, &inputs, refreshStatus, ch]{
        postWrite(s, inputs, "ClrHist", [&s, ch]{ return s.client.sendChannelFaultCommand(ch, ChannelFaultCommand::ClearFaultHistory); }, refreshStatus); });

    // ---- Persistence ----
    auto bSave    = ActionButton("Save",    [&s, &inputs, refreshFull, ch]{
        saveChannelConfig(s, inputs, ch, refreshFull); });
    auto bLoad    = ActionButton("Load",    [&s, &inputs, refreshFull, ch]{
        postWrite(s, inputs, "Load", [&s, ch]{ return s.client.sendParamAction(ch, ParamAction::Load); }, refreshFull); });
    auto bFactory = ActionButton("Factory", [&s, &inputs, refreshFull, ch]{
        postWrite(s, inputs, "Factory", [&s, ch]{ return s.client.sendParamAction(ch, ParamAction::FactoryReset); }, refreshFull); });

    auto hasOutput = [&s, ch] {
        return !s.data.valid ||
               (s.data.chInfo[ch].chCapFlags & CH_CAP_OUTPUT_ENABLE) != 0;
    };
    auto hasDrive = [&s, ch] {
        return !s.data.valid ||
               (s.data.chInfo[ch].chCapFlags & CH_CAP_RAW_OUTPUT_DRIVE) != 0;
    };
    auto hasFixedOutputCfg = [&s, ch] {
        // CH_CAP_OUTPUT_ENABLE without CH_CAP_RAW_OUTPUT_DRIVE — a
        // switchable channel with no DAC (jw_lvb ch1-9), as opposed to a
        // locked always-on channel (jw_lvb ch0, neither capability).
        if (!s.data.valid) return false;
        uint16_t caps = s.data.chInfo[ch].chCapFlags;
        return (caps & CH_CAP_OUTPUT_ENABLE) != 0 && (caps & CH_CAP_RAW_OUTPUT_DRIVE) == 0;
    };
    auto hasProtection = [&s, ch] {
        return !s.data.valid ||
               hasProtectionPolicy(s.data.chInfo[ch].chCapFlags);
    };

    auto outputControls = Container::Horizontal({bEnable, bDisGra, bKill});
    auto visibleOutputControls = Maybe(outputControls, hasOutput);
    auto visibleTgtInp = Maybe(tgtInp, hasDrive);
    auto visibleRuStepInp = Maybe(ruStepInp, hasDrive);
    auto visibleRdStepInp = Maybe(rdStepInp, hasDrive);
    auto visibleOutputEnabledCyc = Maybe(outputEnabledCyc, hasFixedOutputCfg);
    auto protectionControls = Container::Vertical({
        iModeC, iActC, iThrInp, bClrAct, bClrHist,
    });
    auto visibleProtectionControls = Maybe(protectionControls, hasProtection);

    auto container = Container::Vertical({
        visibleOutputControls, visibleTgtInp,
        visibleRuStepInp, visibleRdStepInp, visibleOutputEnabledCyc,
        visibleProtectionControls,
        recovC, delayInp, maxInp, winInp, derInp, iBandInp,
        bSave, bLoad, bFactory,
    });

    return Renderer(container, [=, &s, &inputs]() {
        if (s.data.valid && ch >= s.data.numChannels())
            return text(" CH" + std::to_string(ch) + " not present on this device ") | dim | center;

        // Recovery-policy and derate-step fields are deliberately left out
        // of the connect scan (doFullScan() in tui/main.cpp only fetches
        // what Monitor displays) — fetch them here, once, the first time
        // this channel's tab is actually opened. FTXUI's Container::Tab
        // only renders the active tab, so this fires at most once per
        // channel per session, never for tabs the user never visits.
        if (s.data.valid && ch < s.data.numChannels() && !s.data.chDetailLoaded[ch]) {
            s.data.chDetailLoaded[ch] = true;
            std::function<void()> item = [&s, &inputs, ch] {
                uint16_t caps = s.data.chInfo[ch].chCapFlags;
                s.client.readChannelRecoveryBlock(ch, s.data.chCfg[ch]);
                s.client.readChannelDerateBlock(ch, caps, s.data.chCfg[ch]);
                syncDataToInputs(s.data, inputs);
                s.screen.PostEvent(Event::Custom);
            };
            { std::lock_guard<std::mutex> lk(s.workMutex); s.workQueue.push(std::move(item)); }
            s.workCv.notify_one();
        }

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
            if (hasCurr)  {
                // Fixed per-board unit (not fmtCurrentAuto's per-value
                // auto-ranging) so this always matches the Limit row's unit
                // below and Monitor's "I" column.
                CurrentUnit iu = currentUnitFor(s.data.sysInfo.currentUnitExp);
                liveParts.push_back(text("   I: "));
                liveParts.push_back(text(fmtCurrentBare(ci.currentRaw, s.data.sysInfo.currentUnitExp) +
                                          " " + iu.label) | bold);
            }
            liveParts.push_back(text("   Status: "));
            liveParts.push_back(text(channelStatusBadge(ci.status,
                (caps & CH_CAP_OUTPUT_ENABLE) != 0,
                (caps & CH_CAP_RAW_OUTPUT_DRIVE) != 0)) | bold);
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
        Elements controlRows;
        controlRows.push_back(emptyElement());
        controlRows.push_back(
            hasOutput()
                ? hbox({ bEnable->Render(), text(" "), bDisGra->Render(),
                         text(" "), bKill->Render() })
                : text(" output control not supported ") | dim);
        if (hasDrive()) {
            controlRows.push_back(hbox({ text("Vset    : "), tgtInp->Render() | flex, text(" V") }));
            controlRows.push_back(hbox({ text("Ramp up : "), ruStepInp->Render() | flex, text(" V/s") }));
            controlRows.push_back(hbox({ text("Ramp dn : "), rdStepInp->Render() | flex, text(" V/s") }));
        } else if (hasFixedOutputCfg()) {
            controlRows.push_back(hbox({ text("Startup : "), outputEnabledCyc->Render() | flex,
                                         text(" (AUTOMATIC-mode desired state)") }));
        } else {
            controlRows.push_back(text(" fixed output, always on — no configurable setpoint ") | dim);
        }
        controlRows.push_back(filler());
        auto controlPanel = window(text(" Control "), vbox(std::move(controlRows)));

        // Protection panel
        Element protPanel = emptyElement();
        if (hasProtection()) {
            CurrentUnit iu = currentUnitFor(s.data.sysInfo.currentUnitExp);
            protPanel = window(text(" Protection Policy "), vbox({
                emptyElement(),
                hbox({ text("Limit  : "), iThrInp->Render() | flex, text(std::string(" ") + iu.label) }),
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

} // namespace psb::tui
