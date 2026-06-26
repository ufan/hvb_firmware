#pragma once
#include "widgets.h"
#include <memory>
#include <string>

namespace hvb::tui {

inline Component makeSystemTab(AppState& s) {
    static const std::vector<std::string> kOpModes       = {"Normal", "Automatic"};
    static const std::vector<std::string> kBaudNames     = {"115200", "9600"};
    static const std::vector<std::string> kStartupPolicy = {"Load NVS Config", "Factory Default"};

    struct St {
        std::string slaveAddr;
        int opModeIdx = 0, baudIdx = 0, startupPolicyIdx = 0;
    };
    auto st = std::make_shared<St>();

    auto onOpMode  = [&s, st] { writeAsync(s, "OpMode",   [&s, st] { return s.client.writeOperatingMode(static_cast<OpMode>(st->opModeIdx)); }); };
    auto onBaud    = [&s, st] { writeAsync(s, "BaudRate", [&s, st] { return s.client.writeBaudRateCode((uint16_t)st->baudIdx); }); };
    auto onSlave   = [&s, st] {
        try { uint16_t a = (uint16_t)std::stoul(st->slaveAddr);
              writeAsync(s, "SlaveAddr", [&s, a] { return s.client.writeSlaveAddress(a); }); }
        catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid slave address"; }
    };
    auto onStartup = [&s, st] {
        writeAsync(s, "StartupPolicy", [&s, st] {
            return s.client.writeStartupChannelPolicy((uint16_t)st->startupPolicyIdx);
        });
    };

    auto opModeC      = InlineCycler(kOpModes,       &st->opModeIdx,       onOpMode);
    auto baudC        = InlineCycler(kBaudNames,      &st->baudIdx,         onBaud);
    auto startupC     = InlineCycler(kStartupPolicy,  &st->startupPolicyIdx, onStartup);
    auto slaveInp     = CommitInput(&st->slaveAddr,   "1", onSlave);

    auto bSave    = ActionButton("Save",    [&s]{ writeAsync(s,"Save",    [&s]{ return s.client.sendParamAction(-1, ParamAction::Save); }); });
    auto bLoad    = ActionButton("Load",    [&s]{ writeAsync(s,"Load",    [&s]{ return s.client.sendParamAction(-1, ParamAction::Load); }); });
    auto bFactory = ActionButton("Factory", [&s]{ writeAsync(s,"Factory", [&s]{ return s.client.sendParamAction(-1, ParamAction::FactoryReset); }); });
    auto bCalExit = ActionButton("Exit Cal", [&s]{ writeAsync(s,"Exit Cal", [&s]{ return s.client.exitCalibrationMode(); }); });
    auto bReset   = ActionButton("Reset",   [&s]{ writeAsync(s,"Reset",   [&s]{ return s.client.sendParamAction(-1, ParamAction::SoftwareReset); }); });

    auto cfgContainer = Container::Vertical({
        opModeC, baudC, slaveInp, startupC,
        bSave, bLoad, bFactory, bCalExit, bReset,
    });

    return Renderer(cfgContainer, [=, &s]() {
        if (!s.data.valid)
            return text(" Not connected ") | center | border;

        const auto& si = s.data.sysInfo;
        const auto& sc = s.data.sysCfg;

        char tmp[16], hum[16];
        snprintf(tmp, sizeof(tmp), "%.1f", si.boardTempRaw * 0.1);
        snprintf(hum, sizeof(hum), "%.1f", si.boardHumidityRaw * 0.1);
        char fw[16];
        snprintf(fw, sizeof(fw), "0x%04X", si.fwVersion);
        auto leftPanel = vbox({
            hbox({ text("Protocol  : "), text(std::to_string(si.protoMajor)+"."+std::to_string(si.protoMinor)) }),
            hbox({ text("Variant ID: "), text(std::to_string(si.variantId)) }),
            hbox({ text("FW Version: "), text(fw) }),
            hbox({ text("Channels  : "), text(std::to_string(si.supportedChannels)) }),
            hbox({ text("Uptime    : "), text(std::to_string(si.uptimeSec) + " s") }),
            hbox({ text("Board Temp: "), text(std::string(tmp) + " \xc2\xb0\x43") }),
            hbox({ text("Humidity  : "), text(std::string(hum) + " %RH") }),
            hbox({ text("Op Mode   : "), text(opModeName(si.activeOpMode)) }),
            hbox({ text("Fault     : "), text(faultStr(si.faultCause)) }),
        }) | border | size(WIDTH, GREATER_THAN, 38);

        auto rightPanel = vbox({
            hbox({ text("Op Mode      : "), opModeC->Render() }),
            hbox({ text("Slave Addr   : "), slaveInp->Render(), text("  (0-247)") }),
            hbox({ text("Baud Rate    : "), baudC->Render() }),
            hbox({ text("Startup Pol  : "), startupC->Render() }),
            separator(),
            hbox({ bSave->Render(), text("  "), bLoad->Render(), text("  "),
                   bFactory->Render(), text("  "), bCalExit->Render(), text("  "),
                   bReset->Render() }),
        }) | border;

        (void)sc;
        return hbox({ leftPanel, rightPanel });
    });
}

} // namespace hvb::tui
