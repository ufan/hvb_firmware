#include "FactoryCommands.h"
#include "register_map.h"
#include <iomanip>
#include <sstream>

namespace hvb::factory {

static void requireConnected(FactorySession& s, std::ostream& out, std::function<void()> fn) {
    if (!s.isConnected()) { out << "Error: not connected\n"; return; }
    fn();
}

static void requireCalChannel(FactorySession& s, std::ostream& out, std::function<void()> fn) {
    if (!s.isConnected()) { out << "Error: not connected\n"; return; }
    if (s.activeChannel() < 0) { out << "Error: no active channel (use 'ch 0' or 'ch 1')\n"; return; }
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
                out << "Protocol: " << info.protoMajor << "." << info.protoMinor << "\n"
                    << "Variant:  " << info.variantId << "\n"
                    << "Mode:     " << opModeName(info.activeOpMode) << "\n"
                    << "Uptime:   " << info.uptimeSec << " s\n"
                    << "Cap:      " << info.sysCapFlags
                    << (info.sysCapFlags & SysCap::CALIBRATION_MODE ? " [Cal]" : " [No Cal]") << "\n";
            });
        },
        "System info dump");

    // ---- Calibration submenu ----
    auto calMenu = std::make_unique<cli::Menu>("cal");

    calMenu->Insert("unlock",
        [&session](std::ostream& out) {
            requireConnected(session, out, [&] {
                static int step = 0;
                uint16_t val = (step == 0) ? CAL_UNLOCK_STEP1 : CAL_UNLOCK_STEP2;
                if (session.client().unlockCalibrationStep(val)) {
                    out << "Unlock step " << (step + 1) << " OK (0x"
                        << std::hex << val << std::dec << ")\n";
                    step = (step + 1) % 2;
                } else {
                    out << "Error: " << session.lastError() << "\n";
                    step = 0;
                }
            });
        },
        "Send unlock step (alternates step 1/2)");

    calMenu->Insert("enter",
        [&session](std::ostream& out) {
            requireConnected(session, out, [&] {
                if (session.client().enterCalibrationMode())
                    out << "Entered Calibration Mode\n";
                else
                    out << "Error: " << session.lastError() << "\n";
            });
        },
        "Enter Calibration Mode");

    calMenu->Insert("exit",
        [&session](std::ostream& out, const std::string& mode) {
            requireConnected(session, out, [&] {
                OpMode target = OpMode::Normal;
                if (mode == "auto" || mode == "automatic") target = OpMode::Automatic;
                session.stopWatch();
                if (session.client().exitCalibrationMode(target))
                    out << "Exited to " << opModeName(target) << "\n";
                else
                    out << "Error: " << session.lastError() << "\n";
            });
        },
        "Exit Calibration Mode", {"normal|auto"});

    calMenu->Insert("status",
        [&session](std::ostream& out) {
            requireConnected(session, out, [&] {
                auto si = session.client().readSystemInfo();
                out << "Mode: " << opModeName(si.activeOpMode) << "\n";
                for (int ch = 0; ch < 2; ++ch) {
                    auto snap = session.client().readCalibrationSnapshot(ch);
                    out << "  CH" << ch
                        << " out=" << (snap.outputEnabled ? "ON" : "OFF")
                        << " dac=" << snap.rawDacCode
                        << " readback=" << snap.rawDacReadback
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
                for (int ch = 0; ch < 2; ++ch) {
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
            if (ch < 0 || ch > 1) { out << "Error: channel must be 0 or 1\n"; return; }
            session.setActiveChannel(ch);
            out << "Active channel: " << ch << "\n";
        },
        "Select active channel", {"0|1"});

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
                if (session.client().sendCalibrationSampleCommand(ch))
                    out << "CH" << ch << " sample triggered\n";
                else out << "Error: " << session.lastError() << "\n";
            });
        },
        "Trigger raw ADC sample");

    calMenu->Insert("read",
        [&session](std::ostream& out) {
            requireCalChannel(session, out, [&] {
                int ch = session.activeChannel();
                auto snap = session.client().readCalibrationSnapshot(ch);
                out << "CH" << ch << " snapshot:\n"
                    << "  Output:    " << (snap.outputEnabled ? "ON" : "OFF") << "\n"
                    << "  DAC code:  " << snap.rawDacCode << "  readback: " << snap.rawDacReadback << "\n"
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
