#include "hvb_modbus_client.h"
#include "config_manager.h"
#include "register_map.h"
#include "types.h"

#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/table.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include <iostream>
#include <thread>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <atomic>

using namespace ftxui;

static hvb::HvbModbusClient g_client;
static hvb::ConfigManager   g_cfg;
static std::atomic<bool>    g_connected{false};
static int g_pollInterval = 2;

// ============================================================================
//  Helpers
// ============================================================================

static std::string fmtV(uint16_t raw) {
    double v = hvb::reg::voltageToV(raw);
    char buf[32];
    snprintf(buf, sizeof(buf), "%+.1f V", v);
    return buf;
}

static std::string fmtI(uint16_t raw) {
    double a = hvb::reg::currentToA(raw) * 1e6;
    char buf[32];
    snprintf(buf, sizeof(buf), "%+.1f uA", a);
    return buf;
}

struct ScannedData {
    hvb::SystemInfo   sysInfo{};
    hvb::ChannelInfo  chInfo[2]{};
    hvb::SystemConfig sysCfg{};
    hvb::ChannelConfig chCfg[2]{};
    bool valid = false;
};

static ScannedData pollData() {
    ScannedData d;
    if (!g_client.isConnected()) return d;
    d.sysInfo = g_client.readSystemInfo();
    for (int ch = 0; ch < 2; ++ch) d.chInfo[ch] = g_client.readChannelInfo(ch);
    d.sysCfg  = g_client.readSystemConfig();
    for (int ch = 0; ch < 2; ++ch) d.chCfg[ch]  = g_client.readChannelConfig(ch);
    d.valid = g_client.isConnected();
    return d;
}

// ============================================================================
//  Tab renderers
// ============================================================================

static Element renderMonitor(const ScannedData& d) {
    if (!d.valid) return text(" Not connected — press 'c' to connect ") | center | border;

    std::vector<std::vector<std::string>> rows;
    rows.push_back({"CH", "Vmeas", "Imeas", "Target", "Status", "Fault", "Retry"});
    for (int ch = 0; ch < 2; ++ch) {
        const auto& ci = d.chInfo[ch];
        if (ci.status & hvb::ChStatus::UNSUPPORTED) {
            rows.push_back({std::to_string(ch), "(unsupported)", "", "", "", "", ""});
            continue;
        }
        bool on    = ci.status & hvb::ChStatus::OUTPUT_DRIVE_NONZERO;
        bool fault = ci.activeFault != 0;
        rows.push_back({
            std::to_string(ch),
            fmtV(ci.voltageRaw), fmtI(ci.currentRaw),
            fmtV(ci.operationalTargetVoltageRaw),
            on ? "ON" : "OFF",
            fault ? "YES" : "--",
            std::to_string(ci.retryCount)
        });
    }

    auto table = Table(rows);
    table.SelectAll().Border(LIGHT);
    table.SelectAll().Separator(LIGHT);
    table.SelectRows(0, 0).Decorate(bold);
    table.SelectRows(0, 0).Separator(HEAVY);

    std::string title = " HVB Monitor [" + std::to_string(d.sysInfo.protoMajor) + "." +
        std::to_string(d.sysInfo.protoMinor) + "]  Uptime: " +
        std::to_string(d.sysInfo.uptimeSec) + "s  Mode: " +
        hvb::opModeName(d.sysInfo.activeOpMode) + " ";

    return vbox({
        text(title) | bold | center,
        separator(),
        table.Render() | center,
    }) | border;
}

static Element renderSystemInfo(const ScannedData& d) {
    if (!d.valid) return text(" Not connected ") | center | border;
    const auto& s = d.sysInfo;
    std::vector<std::vector<std::string>> rows;
    auto add = [&](const std::string& k, const std::string& v) { rows.push_back({k, v}); };

    add("Protocol",    std::to_string(s.protoMajor) + "." + std::to_string(s.protoMinor));
    add("Variant ID",  std::to_string(s.variantId));
    add("Channels",    std::to_string(s.supportedChannels) +
        " (mask 0x" + (std::ostringstream{} << std::hex << s.activeChMask).str() + ")");
    add("Op Mode",     hvb::opModeName(s.activeOpMode));
    add("Temp Raw",    std::to_string(s.boardTempRaw));
    add("Humidity Raw",std::to_string(s.boardHumidityRaw));
    add("Uptime",      std::to_string(s.uptimeSec) + " s");
    add("FW Version",  "0x" + (std::ostringstream{} << std::hex << s.fwVersion).str());
    add("Sys Status",  "0x" + (std::ostringstream{} << std::hex << s.sysStatus).str());
    add("Fault Cause", "0x" + (std::ostringstream{} << std::hex << s.faultCause).str());
    rows.push_back({"Cap Flags",
        (s.sysCapFlags & hvb::SysCap::AUTO_MODE_SUPPORTED ? "[Auto] " : "") +
        std::string(s.sysCapFlags & hvb::SysCap::ENV_SENSOR_PRESENT ? "[Env]" : "")});

    auto table = Table(rows);
    table.SelectAll().Border(LIGHT);
    table.SelectColumn(0).Separator(LIGHT);
    return vbox({
        text(" System Info ") | bold | center, separator(), table.Render(),
    }) | border;
}

static Element renderChannelDetail(const ScannedData& d, int ch) {
    if (!d.valid) return text(" Not connected ") | center | border;
    const auto& ci = d.chInfo[ch];
    const auto& cc = d.chCfg[ch];
    if (ci.status & hvb::ChStatus::UNSUPPORTED)
        return text(" Channel " + std::to_string(ch) + " unsupported ") | center | border;

    std::vector<std::vector<std::string>> rows;
    auto add = [&](const std::string& k, const std::string& v) { rows.push_back({k, v}); };
    auto yn  = [](bool b) { return b ? "YES" : "no"; };

    add("Measured V",    fmtV(ci.voltageRaw));
    add("Measured I",    fmtI(ci.currentRaw));
    add("Op Target V",   fmtV(ci.operationalTargetVoltageRaw));
    add("Output Drive",  yn(ci.status & hvb::ChStatus::OUTPUT_DRIVE_NONZERO));
    add("Output Enable", yn(ci.status & hvb::ChStatus::OUTPUT_ENABLE_ACTIVE));
    add("Ramping",       yn(ci.status & hvb::ChStatus::RAMPING_ACTIVE));
    add("Active Fault",  yn(ci.status & hvb::ChStatus::ACTIVE_FAULT));
    add("Fault History", yn(ci.status & hvb::ChStatus::FAULT_HISTORY));
    add("Cooldown",      yn(ci.status & hvb::ChStatus::COOLDOWN_ACTIVE));
    add("Retry Exhausted", yn(ci.status & hvb::ChStatus::RETRY_EXHAUSTED));

    if (ci.activeFault) {
        std::string faults;
        if (ci.activeFault & hvb::FaultCause::VOLTAGE_LIMIT)       faults += "VL ";
        if (ci.activeFault & hvb::FaultCause::CURRENT_LIMIT)       faults += "CL ";
        if (ci.activeFault & hvb::FaultCause::MEASUREMENT_INVALID) faults += "MI ";
        if (ci.activeFault & hvb::FaultCause::OUTPUT_HW_FAULT)     faults += "HW ";
        add("Fault Causes", faults);
    }

    add("", "");
    add("Target V (raw)",  std::to_string(cc.configuredTargetVRaw));
    add("V Limit (raw)",   std::to_string(cc.vLimitThresholdRaw));
    add("I Limit (raw)",   std::to_string(cc.iLimitThresholdRaw));
    add("Ramp Up",  std::to_string(cc.rampUpStepRaw) + " / " + std::to_string(cc.rampUpInterval));
    add("Ramp Down",std::to_string(cc.rampDownStepRaw) + " / " + std::to_string(cc.rampDownInterval));
    add("Derate Step", std::to_string(cc.derateStepRaw));
    add("", "");
    add("Output Cal", "K=" + std::to_string(cc.outCalK) + " B=" + std::to_string(cc.outCalB));
    add("Meas V Cal", "K=" + std::to_string(cc.measVCalK) + " B=" + std::to_string(cc.measVCalB));
    add("Meas I Cal", "K=" + std::to_string(cc.measICalK) + " B=" + std::to_string(cc.measICalB));

    auto table = Table(rows);
    table.SelectAll().Border(LIGHT);
    table.SelectColumn(0).Separator(LIGHT);
    return vbox({
        text(" Channel " + std::to_string(ch) + " ") | bold | center,
        separator(),
        table.Render(),
    }) | border;
}

// ============================================================================
//  Main
// ============================================================================

int main(int argc, char** argv) {
    g_cfg.load();

    std::string portArg;
    int baudArg = 115200, slaveArg = 1, timeoutArg = 500;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "-p" && i+1 < argc) portArg    = argv[++i];
        else if (a == "-b" && i+1 < argc) baudArg    = std::stoi(argv[++i]);
        else if (a == "-i" && i+1 < argc) slaveArg   = std::stoi(argv[++i]);
        else if (a == "-t" && i+1 < argc) timeoutArg = std::stoi(argv[++i]);
        else if (a == "-s" && i+1 < argc) g_pollInterval = std::stoi(argv[++i]);
    }

    // Seed modal inputs from args > config
    std::string modalPort    = portArg.empty()  ? (g_cfg.port.empty() ? "/dev/ttyUSB0" : g_cfg.port) : portArg;
    std::string modalBaud    = std::to_string(baudArg != 115200 ? baudArg : g_cfg.baudRate);
    std::string modalSlaveId = std::to_string(slaveArg != 1     ? slaveArg : g_cfg.slaveId);

    auto screen = ScreenInteractive::Fullscreen();
    int  activeTab    = 0;
    bool showModal    = false;
    bool connecting   = false;
    std::string statusMsg;
    ScannedData data;
    std::atomic<bool> running{true};

    // ----------------------------------------------------------------
    //  Poll thread: runs every g_pollInterval seconds while connected
    // ----------------------------------------------------------------
    std::thread pollThread([&] {
        while (running) {
            if (g_connected) {
                data = pollData();
                if (running) screen.PostEvent(Event::Custom);
            }
            for (int i = 0; i < g_pollInterval * 10 && running; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });

    // ----------------------------------------------------------------
    //  Connection modal
    // ----------------------------------------------------------------
    auto portInput    = Input(&modalPort,    "e.g. /dev/ttyUSB0");
    auto baudInput    = Input(&modalBaud,    "115200");
    auto slaveInput   = Input(&modalSlaveId, "1");

    auto doConnect = [&] {
        if (modalPort.empty() || connecting) return;
        connecting  = true;
        statusMsg   = "Connecting to " + modalPort + "...";
        screen.PostEvent(Event::Custom);

        // Connect on a background thread so the render loop stays alive
        std::thread([&] {
            int baud  = 115200, slave = 1, tmo = timeoutArg;
            try { baud  = std::stoi(modalBaud);    } catch (...) {}
            try { slave = std::stoi(modalSlaveId); } catch (...) {}

            bool ok = g_client.connect(modalPort, baud, slave, tmo);
            g_connected = ok;
            statusMsg   = ok ? "Connected " + modalPort
                             : "Connect failed: " + g_client.lastError();
            connecting  = false;
            showModal   = false;

            if (ok) {
                // Immediate poll so data appears right away
                data = pollData();
            }
            screen.PostEvent(Event::Custom);
        }).detach();
    };

    auto modalButtons = Container::Horizontal({
        Button("Connect", doConnect),
        Button("Cancel",  [&] { showModal = false; }),
    });

    auto modalForm = Container::Vertical({
        portInput, baudInput, slaveInput, modalButtons,
    });

    auto modalRenderer = Renderer(modalForm, [&] {
        return vbox({
            text(" Connect to HVB ") | bold | center,
            separator(),
            hbox({ text("Port    : "), portInput->Render()  }),
            hbox({ text("Baud    : "), baudInput->Render()  }),
            hbox({ text("Slave ID: "), slaveInput->Render() }),
            separator(),
            modalButtons->Render() | center,
            text(connecting ? "Connecting..." : "") | dim | center,
        }) | border | size(WIDTH, EQUAL, 50);
    });

    // ----------------------------------------------------------------
    //  Main layout
    // ----------------------------------------------------------------
    std::vector<std::string> tabTitles = {"Monitor", "System", "CH0", "CH1"};
    auto tabSelector = Toggle(&tabTitles, &activeTab);

    auto tabContent = Container::Tab({
        Renderer([&] { return renderMonitor(data);           }),
        Renderer([&] { return renderSystemInfo(data);        }),
        Renderer([&] { return renderChannelDetail(data, 0);  }),
        Renderer([&] { return renderChannelDetail(data, 1);  }),
    }, &activeTab);

    auto topBar = Renderer([&] {
        std::string connStatus = g_connected
            ? ("Connected " + modalPort) : (statusMsg.empty() ? "Disconnected" : statusMsg);
        return hbox({
            text(" HVB TUI ") | bold,
            filler(),
            text(connStatus) | (g_connected ? color(Color::Green) : color(Color::Red)),
            filler(),
            tabSelector->Render(),
            filler(),
            text(" q:quit  r:refresh  c:connect  d:disconnect ") | dim,
        });
    });

    auto mainContainer = Container::Vertical({topBar, tabSelector, tabContent});

    auto root = mainContainer
        | Modal(modalRenderer, &showModal)
        | CatchEvent([&](Event e) {
            // When the modal is open, let its Input components handle all
            // character events — otherwise 'd', 'r', 'q', etc. are stolen
            // before reaching the focused text field.
            if (showModal) {
                if (e == Event::Escape) { showModal = false; return true; }
                return false;
            }
            if (e == Event::Character('q')) {
                running = false;
                screen.ExitLoopClosure()();
                return true;
            }
            if (e == Event::Character('r')) {
                if (g_connected) { data = pollData(); }
                return true;
            }
            if (e == Event::Character('c')) {
                if (!g_connected && !connecting) showModal = true;
                return true;
            }
            if (e == Event::Character('d')) {
                g_client.disconnect();
                g_connected = false;
                statusMsg   = "Disconnected";
                return true;
            }
            return false;
        });

    // Auto-connect if port was supplied on command line
    if (!portArg.empty()) {
        showModal = false;
        std::thread([&] {
            bool ok = g_client.connect(portArg, baudArg, slaveArg, timeoutArg);
            g_connected = ok;
            statusMsg   = ok ? "" : "Connect failed: " + g_client.lastError();
            if (ok) data = pollData();
            screen.PostEvent(Event::Custom);
        }).detach();
    }

    screen.Loop(root);
    running = false;
    if (pollThread.joinable()) pollThread.join();
    g_client.disconnect();
    return 0;
}
