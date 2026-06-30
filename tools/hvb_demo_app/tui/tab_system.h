#pragma once
#include "widgets.h"
#include <memory>
#include <string>

namespace hvb::tui {

inline Component makeSystemTab(AppState& s, ConfigInputs& inputs) {
    static const std::vector<std::string> kOpModes       = {"Normal", "Automatic"};
    static const std::vector<std::string> kBaudNames     = {"115200", "9600"};
    static const std::vector<std::string> kStartupPolicy = {"Load NVS Config", "Factory Default"};

    auto onOpMode  = [&s, &inputs] {
        postWrite(s, inputs, "OpMode",
            [&s, &inputs] { return s.client.writeOperatingMode(static_cast<OpMode>(inputs.opModeIdx)); },
            [&s, &inputs] {
                s.data.sysCfg = s.client.readSystemConfig();
                s.data.sysCfg.operatingMode = static_cast<OpMode>(inputs.opModeIdx);
            });
    };
    auto onBaud    = [&s, &inputs] {
        postWrite(s, inputs, "BaudRate",
            [&s, &inputs] { return s.client.writeBaudRateCode((uint16_t)inputs.baudIdx); },
            [&s, &inputs] { s.data.sysCfg = s.client.readSystemConfig(); });
    };
    auto onSlave   = [&s, &inputs] {
        try { uint16_t a = (uint16_t)std::stoul(inputs.slaveAddr);
              postWrite(s, inputs, "SlaveAddr",
                  [&s, a] { return s.client.writeSlaveAddress(a); },
                  [&s, &inputs] { s.data.sysCfg = s.client.readSystemConfig(); });
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid slave address"; }
    };
    auto onStartup = [&s, &inputs] {
        postWrite(s, inputs, "StartupPolicy",
            [&s, &inputs] { return s.client.writeStartupChannelPolicy((uint16_t)inputs.startupIdx); },
            [&s, &inputs] { s.data.sysCfg = s.client.readSystemConfig(); });
    };

    auto opModeC      = InlineCycler(kOpModes,       &inputs.opModeIdx,  onOpMode);
    auto baudC        = InlineCycler(kBaudNames,     &inputs.baudIdx,    onBaud);
    auto startupC     = InlineCycler(kStartupPolicy, &inputs.startupIdx, onStartup);
    auto slaveInp     = CommitInput(&inputs.slaveAddr, "1", onSlave);

    auto bSave    = ActionButton("Save",    [&s, &inputs]{
        postWrite(s, inputs, "Save", [&s]{ return s.client.sendParamAction(-1, ParamAction::Save); },
                  [&s, &inputs]{ s.data.sysCfg = s.client.readSystemConfig(); syncDataToInputs(s.data, inputs); });
    });
    auto bLoad    = ActionButton("Load",    [&s, &inputs]{
        postWrite(s, inputs, "Load", [&s]{ return s.client.sendParamAction(-1, ParamAction::Load); },
                  [&s, &inputs]{ s.data.sysCfg = s.client.readSystemConfig(); syncDataToInputs(s.data, inputs); });
    });
    auto bFactory = ActionButton("Factory", [&s, &inputs]{
        postWrite(s, inputs, "Factory", [&s]{ return s.client.sendParamAction(-1, ParamAction::FactoryReset); },
                  [&s, &inputs]{ s.data.sysCfg = s.client.readSystemConfig(); syncDataToInputs(s.data, inputs); });
    });

    auto cfgContainer = Container::Vertical({
        opModeC, baudC, slaveInp, startupC,
        bSave, bLoad, bFactory,
    });

    return Renderer(cfgContainer, [=, &s]() {
        if (!s.data.valid)
            return text(" Not connected \xe2\x80\x94 click [ Connect ] in the toolbar ") | center | border;

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
                   bFactory->Render() }),
        }) | border;

        (void)sc;
        return hbox({ leftPanel, rightPanel });
    });
}

} // namespace hvb::tui
