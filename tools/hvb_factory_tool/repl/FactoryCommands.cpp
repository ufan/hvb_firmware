#include "FactoryCommands.h"
#include "register_map.h"
#include <chrono>
#include <iomanip>
#include <sstream>
#include <thread>

namespace hvb::factory {

static void requireConnected(FactorySession& s, std::ostream& out, std::function<void()> fn) {
    if (!s.isConnected()) { out << "Error: not connected\n"; return; }
    fn();
}

static void requireCalChannel(FactorySession& s, std::ostream& out, std::function<void()> fn) {
    if (!s.isConnected()) { out << "Error: not connected\n"; return; }
    if (s.activeChannel() < 0) { out << "Error: no active channel (use 'cal ch <n>')\n"; return; }
    fn();
}

static int parseIntervalMs(const std::string& str) {
    if (str.empty()) return 1000;
    if (str.size() > 2 && str.substr(str.size() - 2) == "ms") {
        return std::stoi(str.substr(0, str.size() - 2));
    }
    if (str.back() == 's') {
        return static_cast<int>(std::stod(str.substr(0, str.size() - 1)) * 1000);
    }
    return static_cast<int>(std::stod(str) * 1000);
}

std::unique_ptr<cli::Menu> buildRootMenu(FactorySession& session) {
    auto root = std::make_unique<cli::Menu>("factory");

    root->Insert("connect",
        [&session](std::ostream& out, const std::string& port) {
            if (session.connect(port)) out << "Connected to " << port << "\n";
            else out << "Error: " << session.lastError() << "\n";
        },
        "Connect to device", {"port"});

    root->Insert("connect",
        [&session](std::ostream& out, const std::string& port, int baud) {
            if (session.connect(port, baud)) out << "Connected to " << port << " @ " << baud << "\n";
            else out << "Error: " << session.lastError() << "\n";
        },
        "Connect with baud", {"port", "baud"});

    root->Insert("connect",
        [&session](std::ostream& out, const std::string& port, int baud, int id) {
            if (session.connect(port, baud, id)) out << "Connected to " << port << " @ " << baud << " id=" << id << "\n";
            else out << "Error: " << session.lastError() << "\n";
        },
        "Connect with baud and slave id", {"port", "baud", "id"});

    root->Insert("disconnect",
        [&session](std::ostream& out) {
            session.disconnect();
            out << "Disconnected\n";
        },
        "Disconnect from device");

    root->Insert("info",
        [&session](std::ostream& out) {
            requireConnected(session, out, [&] {
                auto info = session.client().readSystemInfo();
                char fw[12];
                std::snprintf(fw, sizeof(fw), "0x%04X", info.fwVersion);
                out << "Protocol: " << info.protoMajor << "." << info.protoMinor << "\n"
                    << "Variant:  " << info.variantId << "\n"
                    << "FW:       " << fw << "\n"
                    << "Channels: " << info.supportedChannels << "\n"
                    << "Mode:     " << opModeName(info.activeOpMode) << "\n"
                    << "Uptime:   " << info.uptimeSec << " s\n"
                    << "Temp:     " << (info.boardTempRaw * 0.1) << " C\n"
                    << "Cap:      0x" << std::hex << info.sysCapFlags << std::dec
                    << (info.sysCapFlags & SysCap::CALIBRATION_MODE ? " [Cal]" : " [No Cal]") << "\n";
            });
        },
        "Device info");

    // ---- Active-channel selection (shared by cal and verification commands) ----
    root->Insert("ch",
        [&session](std::ostream& out, int ch) {
            if (ch < 0 || ch >= static_cast<int>(VC_PROTOCOL_MAX_CHANNELS)) {
                out << "Error: channel must be 0-" << (VC_PROTOCOL_MAX_CHANNELS - 1) << "\n"; return;
            }
            session.setActiveChannel(ch);
            out << "Active channel: " << ch << "\n";
        },
        "Select active channel", {"ch"});

    // ---- Post-calibration verification commands ----
    root->Insert("target",
        [&session](std::ostream& out, double v) {
            requireCalChannel(session, out, [&] {
                int ch = session.activeChannel();
                auto raw = reg::voltageFromV(v);
                if (session.client().writeConfiguredTargetVoltage(ch, raw))
                    out << "CH" << ch << " target = " << v << " V  (raw=" << raw << ")\n";
                else out << "Error: " << session.lastError() << "\n";
            });
        },
        "Set target voltage on active channel (V)", {"V"});

    root->Insert("output",
        [&session](std::ostream& out, const std::string& action) {
            requireCalChannel(session, out, [&] {
                int ch = session.activeChannel();
                OutputAction act;
                if      (action == "on"    || action == "enable")  act = OutputAction::Enable;
                else if (action == "off"   || action == "disable") act = OutputAction::DisableGraceful;
                else if (action == "immed")                        act = OutputAction::DisableImmediate;
                else if (action == "zero")                         act = OutputAction::ForceOutputZero;
                else { out << "Error: action must be on|off|immed|zero\n"; return; }
                if (session.client().sendOutputAction(ch, act))
                    out << "CH" << ch << " output " << action << "\n";
                else out << "Error: " << session.lastError() << "\n";
            });
        },
        "Output action on active channel", {"on|off|immed|zero"});

    root->Insert("measure",
        [&session](std::ostream& out) {
            requireCalChannel(session, out, [&] {
                int ch = session.activeChannel();
                auto ci = session.client().readChannelInfo(ch);
                out << "CH" << ch << ":\n"
                    << "  Vmeas:  " << std::fixed << std::setprecision(1)
                    << reg::voltageToV(ci.voltageRaw) << " V  (raw=" << ci.voltageRaw << ")\n"
                    << "  Imeas:  " << std::fixed << std::setprecision(3)
                    << (reg::currentToA(ci.currentRaw) * 1e6) << " uA  (raw=" << ci.currentRaw << ")\n"
                    << "  Target: " << reg::voltageToV(ci.operationalTargetVoltageRaw) << " V\n"
                    << "  Status: 0x" << std::hex << ci.status << std::dec
                    << ((ci.status & 0x0002) ? " [ON]" : " [OFF]")
                    << (ci.activeFault ? " FAULT" : "") << "\n";
            });
        },
        "Read measurements from active channel");

    // ---- System submenu ----
    auto sysMenu = std::make_unique<cli::Menu>("sys");

    sysMenu->Insert("status",
        [&session](std::ostream& out) {
            requireConnected(session, out, [&] {
                auto si = session.client().readSystemInfo();
                out << "Mode: " << opModeName(si.activeOpMode)
                    << "  Channels: " << si.supportedChannels
                    << "  Uptime: " << si.uptimeSec << " s\n";
                for (int ch = 0; ch < si.supportedChannels; ++ch) {
                    auto ci = session.client().readChannelInfo(ch);
                    out << "  CH" << ch
                        << "  V=" << std::fixed << std::setprecision(1)
                        << reg::voltageToV(ci.voltageRaw) << "V"
                        << "  I=" << std::fixed << std::setprecision(3)
                        << (reg::currentToA(ci.currentRaw) * 1e6) << "uA"
                        << "  tgt=" << reg::voltageToV(ci.operationalTargetVoltageRaw) << "V"
                        << "  status=0x" << std::hex << ci.status << std::dec
                        << (ci.activeFault ? " FAULT" : "") << "\n";
                }
            });
        },
        "System + per-channel status overview");

    sysMenu->Insert("mode",
        [&session](std::ostream& out, const std::string& m) {
            requireConnected(session, out, [&] {
                OpMode mode;
                if      (m == "normal") mode = OpMode::Normal;
                else if (m == "auto")   mode = OpMode::Automatic;
                else { out << "Error: mode must be normal|auto\n"; return; }
                if (session.client().writeOperatingMode(mode))
                    out << "Mode -> " << m << "\n";
                else out << "Error: " << session.lastError() << "\n";
            });
        },
        "Set operating mode", {"normal|auto"});

    sysMenu->Insert("save",
        [&session](std::ostream& out) {
            requireConnected(session, out, [&] {
                if (session.client().sendParamAction(-1, ParamAction::Save))
                    out << "Configuration saved to NVS\n";
                else out << "Error: " << session.lastError() << "\n";
            });
        },
        "Save all configuration to NVS");

    sysMenu->Insert("load",
        [&session](std::ostream& out) {
            requireConnected(session, out, [&] {
                if (session.client().sendParamAction(-1, ParamAction::Load))
                    out << "Configuration loaded from NVS\n";
                else out << "Error: " << session.lastError() << "\n";
            });
        },
        "Load configuration from NVS");

    sysMenu->Insert("factory",
        [&session](std::ostream& out) {
            requireConnected(session, out, [&] {
                if (session.client().sendParamAction(-1, ParamAction::FactoryReset))
                    out << "Factory reset applied\n";
                else out << "Error: " << session.lastError() << "\n";
            });
        },
        "Apply factory defaults");

    sysMenu->Insert("reset",
        [&session](std::ostream& out) {
            requireConnected(session, out, [&] {
                out << "Sending software reset...\n";
                session.client().sendParamAction(-1, ParamAction::SoftwareReset);
                session.disconnect();
                out << "Disconnected (reconnect after device restarts)\n";
            });
        },
        "Software reset and disconnect");

    root->Insert(std::move(sysMenu));

    // ---- Calibration submenu ----
    auto calMenu = std::make_unique<cli::Menu>("cal");

    calMenu->Insert("unlock",
        [&session](std::ostream& out) {
            requireConnected(session, out, [&] {
                if (!session.client().unlockCalibrationStep(CAL_UNLOCK_STEP1)) {
                    out << "Error (step 1): " << session.lastError() << "\n"; return;
                }
                if (!session.client().unlockCalibrationStep(CAL_UNLOCK_STEP2)) {
                    out << "Error (step 2): " << session.lastError() << "\n"; return;
                }
                if (!session.client().enterCalibrationMode()) {
                    out << "Error (enter): " << session.lastError() << "\n"; return;
                }
                out << "Calibration session started\n"
                    << "  ch <n>          select active channel\n"
                    << "  limit <max>     set DAC safety cap first\n"
                    << "  enable          enable cal output\n"
                    << "  dac <code>      set raw DAC code\n"
                    << "  sample          sample ADC (reads + displays result)\n"
                    << "  coeff out|meas-v|meas-i <k> <b>  set coefficients\n"
                    << "  commit          save coefficients to NVS\n"
                    << "  exit-cal        end session\n";
            });
        },
        "Unlock + enter calibration mode (atomic)");

    calMenu->Insert("exit-cal",
        [&session](std::ostream& out) {
            requireConnected(session, out, [&] {
                session.stopWatch();
                if (session.client().exitCalibrationMode())
                    out << "Exited calibration mode\n";
                else
                    out << "Error: " << session.lastError() << "\n";
            });
        },
        "Exit Calibration Mode");

    calMenu->Insert("status",
        [&session](std::ostream& out) {
            requireConnected(session, out, [&] {
                auto si = session.client().readSystemInfo();
                out << "Mode: " << opModeName(si.activeOpMode)
                    << "  Channels: " << si.supportedChannels << "\n";
                for (int ch = 0; ch < si.supportedChannels; ++ch) {
                    auto snap = session.client().readCalibrationSnapshot(ch);
                    out << "  CH" << ch
                        << " out=" << (snap.outputEnabled ? "ON" : "OFF")
                        << " dac=" << snap.rawDacCode
                        << " adc_v=" << snap.rawAdcVoltage
                        << " adc_i=" << snap.rawAdcCurrent
                        << " limit=" << snap.maxRawDacLimit << "\n";
                }
            });
        },
        "Show calibration status");

    calMenu->Insert("safe",
        [&session](std::ostream& out) {
            requireConnected(session, out, [&] {
                auto si = session.client().readSystemInfo();
                for (int ch = 0; ch < si.supportedChannels; ++ch) {
                    session.client().writeRawDacCode(ch, 0);
                    session.client().writeCalibrationOutputEnable(ch, false);
                }
                session.stopWatch();
                out << "All calibration outputs disabled, DAC zeroed\n";
            });
        },
        "Disable all cal outputs and zero DAC");

    calMenu->Insert("ch",
        [&session](std::ostream& out, int ch) {
            if (ch < 0 || ch >= static_cast<int>(VC_PROTOCOL_MAX_CHANNELS)) {
                out << "Error: channel must be 0-" << (VC_PROTOCOL_MAX_CHANNELS - 1) << "\n"; return;
            }
            session.setActiveChannel(ch);
            out << "Active channel: " << ch << "\n";
        },
        "Select active channel", {"ch"});

    calMenu->Insert("enable",
        [&session](std::ostream& out) {
            requireCalChannel(session, out, [&] {
                int ch = session.activeChannel();
                if (session.client().writeCalibrationOutputEnable(ch, true))
                    out << "CH" << ch << " calibration output enabled\n";
                else out << "Error: " << session.lastError() << "\n";
            });
        },
        "Enable calibration output on active channel");

    calMenu->Insert("disable",
        [&session](std::ostream& out) {
            requireCalChannel(session, out, [&] {
                int ch = session.activeChannel();
                session.client().writeRawDacCode(ch, 0);
                session.client().writeCalibrationOutputEnable(ch, false);
                out << "CH" << ch << " calibration output disabled, DAC zeroed\n";
            });
        },
        "Disable calibration output, zero DAC");

    calMenu->Insert("dac",
        [&session](std::ostream& out, int code) {
            requireCalChannel(session, out, [&] {
                int ch = session.activeChannel();
                if (code < 0) { out << "Error: DAC code must be >= 0\n"; return; }
                if (session.client().writeRawDacCode(ch, static_cast<uint16_t>(code)))
                    out << "CH" << ch << " DAC = " << code << "\n";
                else out << "Error: " << session.lastError() << "\n";
            });
        },
        "Write raw DAC code", {"code"});

    calMenu->Insert("sample",
        [&session](std::ostream& out) {
            requireCalChannel(session, out, [&] {
                int ch = session.activeChannel();
                if (!session.client().sendCalibrationSampleCommand(ch)) {
                    out << "Error: " << session.lastError() << "\n"; return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                auto snap = session.client().readCalibrationSnapshot(ch);
                out << "CH" << ch << " sample:\n"
                    << "  dac=" << snap.rawDacCode << "\n"
                    << "  adc_v=" << snap.rawAdcVoltage << "\n"
                    << "  adc_i=" << snap.rawAdcCurrent << "\n";
            });
        },
        "Sample ADC and display raw result");

    calMenu->Insert("read",
        [&session](std::ostream& out) {
            requireCalChannel(session, out, [&] {
                int ch = session.activeChannel();
                auto snap = session.client().readCalibrationSnapshot(ch);
                out << "CH" << ch << " snapshot:\n"
                    << "  Output:    " << (snap.outputEnabled ? "ON" : "OFF") << "\n"
                    << "  DAC code:  " << snap.rawDacCode << "\n"
                    << "  Max limit: " << snap.maxRawDacLimit << "\n"
                    << "  ADC V:     " << snap.rawAdcVoltage << "\n"
                    << "  ADC I:     " << snap.rawAdcCurrent << "\n";
            });
        },
        "Read calibration snapshot");

    calMenu->Insert("limit",
        [&session](std::ostream& out, int max) {
            requireCalChannel(session, out, [&] {
                int ch = session.activeChannel();
                if (session.client().writeCalibrationMaxDacLimit(ch, static_cast<uint16_t>(max)))
                    out << "CH" << ch << " max DAC limit = " << max << "\n";
                else out << "Error: " << session.lastError() << "\n";
            });
        },
        "Set max raw DAC limit", {"max"});

    calMenu->Insert("commit",
        [&session](std::ostream& out) {
            requireCalChannel(session, out, [&] {
                int ch = session.activeChannel();
                if (session.client().sendCalibrationCommitCommand(ch))
                    out << "CH" << ch << " coefficients committed\n";
                else out << "Error: " << session.lastError() << "\n";
            });
        },
        "Commit calibration coefficients for active channel");

    calMenu->Insert("coeff",
        [&session](std::ostream& out, const std::string& type, double k, double b) {
            requireCalChannel(session, out, [&] {
                int ch = session.activeChannel();
                auto kRaw = static_cast<uint16_t>(k);
                auto bRaw = static_cast<int16_t>(b);
                bool ok = false;
                if (type == "out")
                    ok = session.client().writeCalibrationOutput(ch, kRaw, bRaw);
                else if (type == "meas-v")
                    ok = session.client().writeCalibrationMeasV(ch, kRaw, bRaw);
                else if (type == "meas-i")
                    ok = session.client().writeCalibrationMeasI(ch, kRaw, bRaw);
                else { out << "Error: type must be out|meas-v|meas-i\n"; return; }
                if (ok) out << "CH" << ch << " " << type << " K=" << kRaw << " B=" << bRaw << "\n";
                else out << "Error: " << session.lastError() << "\n";
            });
        },
        "Write calibration coefficients", {"out|meas-v|meas-i", "k", "b"});

    calMenu->Insert("coeff",
        [&session](std::ostream& out, const std::string& subcmd) {
            if (subcmd != "show") { out << "Usage: coeff show | coeff <type> <k> <b>\n"; return; }
            requireCalChannel(session, out, [&] {
                int ch = session.activeChannel();
                auto cal = session.client().readChannelCalConfig(ch);
                out << "CH" << ch << " coefficients:\n"
                    << "  Output:  K=" << cal.outCalK << " B=" << cal.outCalB << "\n"
                    << "  Meas V:  K=" << cal.measVCalK << " B=" << cal.measVCalB << "\n"
                    << "  Meas I:  K=" << cal.measICalK << " B=" << cal.measICalB << "\n";
            });
        },
        "Show current coefficients", {"show"});

    calMenu->Insert("watch",
        [&session](std::ostream& out, const std::string& mode) {
            if (mode == "off") { session.stopWatch(); out << "Watch stopped\n"; return; }
            requireCalChannel(session, out, [&] {
                WatchMode wm = WatchMode::Off;
                if (mode == "adc") wm = WatchMode::Adc;
                else if (mode == "measure") wm = WatchMode::Measure;
                else if (mode == "status") wm = WatchMode::Status;
                else if (mode == "all") wm = WatchMode::All;
                else { out << "Error: mode must be adc|measure|status|all|off\n"; return; }
                session.startWatch(wm, 1000, out);
                out << "Watch " << mode << " started (1s)\n";
            });
        },
        "Start/stop periodic watch", {"adc|measure|status|all|off"});

    calMenu->Insert("watch",
        [&session](std::ostream& out, const std::string& mode, const std::string& interval) {
            if (mode == "off") { session.stopWatch(); out << "Watch stopped\n"; return; }
            requireCalChannel(session, out, [&] {
                WatchMode wm = WatchMode::Off;
                if (mode == "adc") wm = WatchMode::Adc;
                else if (mode == "measure") wm = WatchMode::Measure;
                else if (mode == "status") wm = WatchMode::Status;
                else if (mode == "all") wm = WatchMode::All;
                else { out << "Error: mode must be adc|measure|status|all|off\n"; return; }
                int ms = parseIntervalMs(interval);
                if (ms <= 0) { out << "Error: invalid interval\n"; return; }
                session.startWatch(wm, ms, out);
                out << "Watch " << mode << " started (" << interval << ")\n";
            });
        },
        "Start periodic watch with interval", {"adc|measure|status|all|off", "interval"});

    root->Insert(std::move(calMenu));
    return root;
}

} // namespace hvb::factory
