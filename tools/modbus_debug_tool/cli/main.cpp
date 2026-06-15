#include "hvb_modbus_client.h"
#include "config_manager.h"
#include "register_map.h"
#include "register_meta.h"

#include <CLI/CLI.hpp>

#include <iostream>
#include <iomanip>
#include <sstream>
#include <thread>
#include <chrono>
#include <csignal>

// ============================================================================
//  Output helpers — raw LSB + estimated physical
// ============================================================================

static std::string formatVRaw(uint16_t raw) {
    auto v = hvb::reg::voltageToV(raw);
    std::ostringstream ss;
    ss << raw << " LSB ≈ " << std::showpos << std::fixed << std::setprecision(1) << v << " V";
    return ss.str();
}
static std::string formatIRaw(uint16_t raw) {
    auto a = hvb::reg::currentToA(raw);
    std::ostringstream ss;
    ss << raw << " LSB ≈ ";
    if (std::abs(a) >= 1.0) ss << std::fixed << std::setprecision(3) << a << " A";
    else if (std::abs(a) >= 1e-3) ss << (a * 1e3) << " mA";
    else ss << (a * 1e6) << " uA";
    return ss.str();
}

static void printSep(const std::string& left, const std::string& right) {
    std::cout << std::left << std::setw(24) << left << " " << right << "\n";
}

static void printStatusBits(uint16_t status) {
    auto yn = [status](uint16_t m) { return (status & m) ? "Yes" : "No"; };
    std::cout << "  Output Drive:     " << yn(hvb::ChStatus::OUTPUT_DRIVE_NONZERO) << "\n";
    std::cout << "  Output Enable:    " << yn(hvb::ChStatus::OUTPUT_ENABLE_ACTIVE) << "\n";
    std::cout << "  Ramping:          " << yn(hvb::ChStatus::RAMPING_ACTIVE) << "\n";
    std::cout << "  Active Fault:     " << yn(hvb::ChStatus::ACTIVE_FAULT) << "\n";
    std::cout << "  Fault History:    " << yn(hvb::ChStatus::FAULT_HISTORY) << "\n";
    std::cout << "  Cooldown:         " << yn(hvb::ChStatus::COOLDOWN_ACTIVE) << "\n";
    std::cout << "  Retry Exhausted:  " << yn(hvb::ChStatus::RETRY_EXHAUSTED) << "\n";
    std::cout << "  Unsupported:      " << yn(hvb::ChStatus::UNSUPPORTED) << "\n";
}

static void printFaultCause(uint16_t fault, const char* label) {
    std::cout << std::left << std::setw(22) << label;
    if (fault == 0) { std::cout << "None\n"; return; }
    bool first = true;
    auto p = [&](const char* s) { if (!first) std::cout << ","; std::cout << s; first = false; };
    if (fault & hvb::FaultCause::VOLTAGE_LIMIT)        p("VL");
    if (fault & hvb::FaultCause::CURRENT_LIMIT)        p("CL");
    if (fault & hvb::FaultCause::MEASUREMENT_INVALID)  p("MI");
    if (fault & hvb::FaultCause::OUTPUT_HW_FAULT)      p("HW");
    if (fault & hvb::FaultCause::VARIANT_INTERLOCK)    p("IL");
    if (fault & hvb::FaultCause::AUTO_RETRY_EXHAUSTED) p("RE");
    if (fault & hvb::FaultCause::CONFIG_INVALID_AUTO)  p("CI");
    std::cout << "\n";
}

static std::string formatHex(uint16_t v) {
    std::ostringstream ss;
    ss << "0x" << std::hex << std::setw(4) << std::setfill('0') << v;
    return ss.str();
}

// ============================================================================
//  Globals
// ============================================================================

static volatile sig_atomic_t g_running = 1;
static void sigintHandler(int) { g_running = 0; }
static hvb::HvbModbusClient* g_client = nullptr;

// ============================================================================
//  Commands
// ============================================================================

int cmdListPorts() {
    auto ports = hvb::HvbModbusClient::scanPorts();
    if (ports.empty()) { std::cout << "No serial ports found.\n"; return 0; }
    for (const auto& p : ports) std::cout << p << "\n";
    return 0;
}

int cmdListRegs() { std::cout << hvb::meta::formatRegisterCatalog(); return 0; }

int cmdDescribe(uint16_t addr) {
    auto* d = hvb::meta::findDesc(addr, true);
    if (d) { printSep("Holding:", std::string(d->name) + " " + d->desc); }
    d = hvb::meta::findDesc(addr, false);
    if (d) { printSep("Input:", std::string(d->name) + " " + d->desc); }
    if (!d) { std::cout << "No register at 0x" << std::hex << addr << std::dec << "\n"; }
    return 0;
}

// --- Reads ---

int cmdInfo() {
    auto info = g_client->readSystemInfo();
    if (!g_client->isConnected()) { std::cerr << "Error\n"; return 1; }
    std::cout << "=== System Info ===\n";
    printSep("Protocol:", std::to_string(info.protoMajor) + "." + std::to_string(info.protoMinor));
    printSep("Variant ID:", std::to_string(info.variantId));
    printSep("Cap Flags:", std::to_string(info.sysCapFlags) + " "
        + (info.sysCapFlags & hvb::SysCap::AUTO_MODE_SUPPORTED ? "[Auto]" : "")
        + (info.sysCapFlags & hvb::SysCap::ENV_SENSOR_PRESENT ? "[Env]" : ""));
    printSep("Channels:", std::to_string(info.supportedChannels) + " (mask " + formatHex(info.activeChMask) + ")");
    printSep("Board Temp:", std::to_string(info.boardTempRaw) + " raw");
    printSep("Humidity:", std::to_string(info.boardHumidityRaw) + " raw");
    printSep("Uptime:", std::to_string(info.uptimeSec) + " s");
    printSep("FW Version:", formatHex(info.fwVersion));
    printSep("Active OpMode:", hvb::opModeName(info.activeOpMode));
    printSep("Sys Status:", formatHex(info.sysStatus));
    printSep("Fault Cause:", formatHex(info.faultCause));
    return 0;
}

int cmdStatus() {
    for (int ch = 0; ch < 2; ++ch) {
        auto ci = g_client->readChannelInfo(ch);
        if (!g_client->isConnected()) { std::cerr << "Error ch" << ch << "\n"; return 1; }
        if (ci.status & hvb::ChStatus::UNSUPPORTED) { std::cout << "CH" << ch << ": UNSUPPORTED\n"; continue; }
        std::cout << "=== Channel " << ch << " ===\n";
        printSep("Measured Voltage:", formatVRaw(ci.voltageRaw));
        printSep("Measured Current:", formatIRaw(ci.currentRaw));
        printSep("Operational Target:", formatVRaw(ci.operationalTargetVoltageRaw));
        printSep("Status:", formatHex(ci.status));
        printStatusBits(ci.status);
        printFaultCause(ci.activeFault, "Active Fault:");
        printFaultCause(ci.faultHistory, "Fault History:");
        printSep("Last Prot Output:", hvb::outputActionName(static_cast<hvb::OutputAction>(ci.lastProtOutputAction)));
        printSep("Retries:", std::to_string(ci.retryCount));
        printSep("Cooldown:", std::to_string(ci.cooldownSec) + " s");
        printSep("Last Fault TS:", std::to_string(ci.lastFaultTimestamp) + " s");
        printSep("Cap Flags:", formatHex(ci.chCapFlags)
            + (ci.chCapFlags & hvb::ChCap::OUTPUT_ENABLE_CTRL ? " [OutEn]" : "")
            + (ci.chCapFlags & hvb::ChCap::CURRENT_MEAS ? " [CurrMeas]" : "")
            + (ci.chCapFlags & hvb::ChCap::AUTO_RECOVERY ? " [AutoRec]" : ""));
        if (ch == 0) std::cout << "\n";
    }
    return 0;
}

int cmdMonitor(int intervalSec) {
    if (!g_client->isConnected()) { std::cerr << "Not connected\n"; return 1; }
    ::signal(SIGINT, sigintHandler);
    while (g_running) {
        std::cout << "\033[2J\033[H";
        auto info = g_client->readSystemInfo();
        if (g_client->isConnected())
            std::cout << "=== Monitor [" << info.protoMajor << "." << info.protoMinor
                      << "] Uptime: " << info.uptimeSec
                      << "s  Mode: " << hvb::opModeName(info.activeOpMode) << " ===\n";
        for (int ch = 0; ch < 2; ++ch) {
            auto ci = g_client->readChannelInfo(ch);
            if (ci.status & hvb::ChStatus::UNSUPPORTED) continue;
            std::cout << "CH" << ch << ": V=" << formatVRaw(ci.voltageRaw)
                      << "  I=" << formatIRaw(ci.currentRaw)
                      << "  Target=" << formatVRaw(ci.operationalTargetVoltageRaw)
                      << "  Fault=" << (ci.activeFault ? "YES" : "no")
                      << "  Retry=" << ci.retryCount << "\n";
        }
        std::this_thread::sleep_for(std::chrono::seconds(intervalSec));
    }
    std::cout << "\n";
    return 0;
}

int cmdSystemConfig() {
    auto cfg = g_client->readSystemConfig();
    if (!g_client->isConnected()) return 1;
    std::cout << "=== System Configuration ===\n";
    printSep("Operating Mode:", hvb::opModeName(cfg.operatingMode));
    printSep("Slave Address:", std::to_string(cfg.slaveAddr));
    printSep("Baud Rate:", cfg.baudRateCode == 0 ? "115200" : "9600");
    printSep("Recovery Policy:", hvb::recoveryPolicyName(cfg.recoveryPolicy));
    printSep("Retry:", "delay=" + std::to_string(cfg.retryDelay) + "s  max="
             + std::to_string(cfg.retryMax) + "  window=" + std::to_string(cfg.retryWindow) + "s");
    printSep("Safe Bands:", "V=" + std::to_string(cfg.voltageSafeBandPct) + "%  I="
             + std::to_string(cfg.currentSafeBandPct) + "%");
    return 0;
}

int cmdChannelConfig(int ch) {
    auto cfg = g_client->readChannelConfig(ch);
    if (!g_client->isConnected()) return 1;
    std::cout << "=== Channel " << ch << " Configuration ===\n";
    printSep("Configured Target:", formatVRaw(cfg.configuredTargetVRaw));
    printSep("Output Action:", hvb::outputActionName(cfg.outputAction));
    printSep("Fault Command:", hvb::faultCommandName(cfg.faultCommand));
    printSep("Ramp Up:", std::to_string(cfg.rampUpStepRaw) + " raw / " + std::to_string(cfg.rampUpInterval) + " x10s");
    printSep("Ramp Down:", std::to_string(cfg.rampDownStepRaw) + " raw / " + std::to_string(cfg.rampDownInterval) + " x10s");
    printSep("V Protection:", std::string(hvb::protectionModeName(cfg.vProtMode)) + " / "
             + hvb::outputActionName(cfg.vProtOutputAction) + " / " + formatVRaw(cfg.vLimitThresholdRaw));
    printSep("I Protection:", std::string(hvb::protectionModeName(cfg.iProtMode)) + " / "
             + hvb::outputActionName(cfg.iProtOutputAction) + " / " + formatIRaw(cfg.iLimitThresholdRaw));
    printSep("Derate Step:", formatVRaw(cfg.derateStepRaw));
    printSep("Save Target:", cfg.saveTargetPolicy ? "Yes" : "No");
    printSep("Calibration:", "");
    printSep("  Output:", "K=" + std::to_string(cfg.outCalK) + "  B=" + std::to_string(cfg.outCalB));
    printSep("  Meas V:", "K=" + std::to_string(cfg.measVCalK) + "  B=" + std::to_string(cfg.measVCalB));
    printSep("  Meas I:", "K=" + std::to_string(cfg.measICalK) + "  B=" + std::to_string(cfg.measICalB));
    return 0;
}

int cmdChannelCal(int ch) {
    auto cfg = g_client->readChannelConfig(ch);
    if (!g_client->isConnected()) return 1;
    std::cout << "=== Channel " << ch << " Calibration ===\n";
    printSep("Output:", "K=" + std::to_string(cfg.outCalK) + " (x10000)  B=" + std::to_string(cfg.outCalB) + " (x1000)");
    printSep("Meas V:", "K=" + std::to_string(cfg.measVCalK) + " (x10000)  B=" + std::to_string(cfg.measVCalB) + " (x1000)");
    printSep("Meas I:", "K=" + std::to_string(cfg.measICalK) + " (x10000)  B=" + std::to_string(cfg.measICalB) + " (x1000)");
    return 0;
}

// Raw debug
int cmdRawFc04(uint16_t addr, uint16_t count) {
    std::vector<uint16_t> buf(count);
    if (!g_client->readInputRegs(addr, count, buf.data())) { std::cerr << g_client->lastError() << "\n"; return 1; }
    std::cout << std::hex << std::uppercase;
    for (uint16_t i = 0; i < count; ++i)
        std::cout << std::setw(4) << std::setfill('0') << buf[i] << (i % 8 == 7 ? "\n" : " ");
    std::cout << std::dec << "\n";
    return 0;
}

int cmdRawFc03(uint16_t addr, uint16_t count) {
    std::vector<uint16_t> buf(count);
    if (!g_client->readHoldingRegs(addr, count, buf.data())) { std::cerr << g_client->lastError() << "\n"; return 1; }
    std::cout << std::hex << std::uppercase;
    for (uint16_t i = 0; i < count; ++i)
        std::cout << std::setw(4) << std::setfill('0') << buf[i] << (i % 8 == 7 ? "\n" : " ");
    std::cout << std::dec << "\n";
    return 0;
}

int cmdRawFc06(uint16_t addr, uint16_t value) {
    if (!g_client->writeReg16(addr, value)) { std::cerr << g_client->lastError() << "\n"; return 1; }
    std::cout << "OK\n";
    return 0;
}

// ============================================================================
//  Main
// ============================================================================

int main(int argc, char** argv) {
    CLI::App app{"HVB Modbus Debug Tool"};
    hvb::ConfigManager cfgMgr;
    cfgMgr.load();

    // Global options
    std::string port;
    int baud = 115200, slaveId = 1, timeout = 500;
    bool save = false;
    app.add_option("-p,--port", port, "Serial port");
    app.add_option("-b,--baud", baud, "Baud rate")->check(CLI::IsMember({9600, 115200}));
    app.add_option("-i,--id", slaveId, "Slave ID")->check(CLI::Range(0, 247));
    app.add_option("-t,--timeout", timeout, "Timeout ms");
    app.add_flag("--save", save, "Save connection to config");

    // Discovery
    auto* listCmd = app.add_subcommand("list", "Discovery")->require_subcommand(1);
    listCmd->add_subcommand("ports", "List serial ports")->callback([&]() { cmdListPorts(); });
    listCmd->add_subcommand("regs", "List register catalog")->callback([&]() { cmdListRegs(); });

    auto* describeCmd = app.add_subcommand("describe", "Show register metadata");
    uint32_t hexAddr = 0;
    describeCmd->add_option("addr", hexAddr, "PDU address (hex)")->required();

    // Top-level reads
    app.add_subcommand("info", "System info dump")->callback([&]() { cmdInfo(); });
    app.add_subcommand("status", "Channel summary")->callback([&]() { cmdStatus(); });
    auto* monitorCmd = app.add_subcommand("monitor", "Live polling");
    int interval = 2;
    monitorCmd->add_option("interval", interval, "Poll interval (s)");

    // System subcommands
    auto* sysCmd = app.add_subcommand("system", "System config read/write")->require_subcommand(1);
    sysCmd->add_subcommand("config", "Read system config")->callback([&]() { cmdSystemConfig(); });
    {
        auto* sc = sysCmd->add_subcommand("mode", "Set operating mode"); std::string m;
        sc->add_option("mode", m, "NORMAL|AUTO")->required();
        sc->callback([&]() { std::cout << (g_client->writeOperatingMode(m=="AUTO"?hvb::OpMode::Automatic:hvb::OpMode::Normal)?"OK\n":g_client->lastError()+"\n"); });
    }
    {
        auto* sc = sysCmd->add_subcommand("recovery", "Set recovery policy");
        std::string p; int d=0,mx=0,w=0;
        sc->add_option("policy", p, "MANUAL-LATCH|AUTO-RETRY|AUTO-DERATE|NEVER-RETRY")->required();
        sc->add_option("delay", d, "Cooldown seconds")->required();
        sc->add_option("max", mx, "Max retries")->required();
        sc->add_option("window", w, "Retry window seconds")->required();
        sc->callback([&]() {
            hvb::RecoveryPolicy rp = hvb::RecoveryPolicy::ManualLatch;
            if (p=="AUTO-RETRY") rp=hvb::RecoveryPolicy::AutoRetry;
            else if (p=="AUTO-DERATE") rp=hvb::RecoveryPolicy::AutoDerateRetry;
            else if (p=="NEVER-RETRY") rp=hvb::RecoveryPolicy::NeverRetry;
            std::cout << (g_client->writeSystemRecoveryPolicy(rp,d,mx,w)?"OK\n":g_client->lastError()+"\n"); });
    }
    {
        auto* sc = sysCmd->add_subcommand("safe-bands", "Set safe bands");
        uint16_t v=10, i=10;
        sc->add_option("v-pct", v, "0-50")->required()->check(CLI::Range(0u,50u));
        sc->add_option("i-pct", i, "0-50")->required()->check(CLI::Range(0u,50u));
        sc->callback([&]() { std::cout << (g_client->writeSafeBands(v,i)?"OK\n":g_client->lastError()+"\n"); });
    }
    {
        auto* sc = sysCmd->add_subcommand("addr", "Set slave address"); uint16_t a=1;
        sc->add_option("addr", a, "0-247")->required()->check(CLI::Range(0u,247u));
        sc->callback([&]() { std::cout << (g_client->writeSlaveAddress(a)?"OK\n":g_client->lastError()+"\n"); });
    }
    {
        auto* sc = sysCmd->add_subcommand("baud", "Set baud rate"); uint16_t c=0;
        sc->add_option("code", c, "0=115200,1=9600")->required()->check(CLI::Range(0u,1u));
        sc->callback([&]() { std::cout << (g_client->writeBaudRateCode(c)?"OK\n":g_client->lastError()+"\n"); });
    }
    sysCmd->add_subcommand("save", "Save system params")->callback([&]() { std::cout << (g_client->sendParamAction(-1,hvb::ParamAction::Save)?"OK\n":g_client->lastError()+"\n"); });
    sysCmd->add_subcommand("load", "Load system params")->callback([&]() { std::cout << (g_client->sendParamAction(-1,hvb::ParamAction::Load)?"OK\n":g_client->lastError()+"\n"); });
    sysCmd->add_subcommand("factory", "Factory reset system")->callback([&]() { std::cout << (g_client->sendParamAction(-1,hvb::ParamAction::FactoryReset)?"OK\n":g_client->lastError()+"\n"); });

    // Channel subcommands
    auto* chCmd = app.add_subcommand("channel", "Channel operations");
    int ch = 0;
    chCmd->add_option("channel", ch, "0 or 1")->required()->check(CLI::Range(0,1));
    chCmd->require_subcommand(1);

    chCmd->add_subcommand("info", "Measurements")->callback([&]() {
        auto ci = g_client->readChannelInfo(ch);
        if (!g_client->isConnected()) return;
        std::cout << "=== Channel " << ch << " ===\n";
        printSep("Measured V:", formatVRaw(ci.voltageRaw));
        printSep("Measured I:", formatIRaw(ci.currentRaw));
        printSep("Operational Target:", formatVRaw(ci.operationalTargetVoltageRaw));
        printSep("Status:", formatHex(ci.status)); printStatusBits(ci.status);
        printFaultCause(ci.activeFault, "Active Fault:");
        printFaultCause(ci.faultHistory, "Fault History:");
        printSep("Last Prot:", hvb::outputActionName(static_cast<hvb::OutputAction>(ci.lastProtOutputAction)));
        printSep("Retries:", std::to_string(ci.retryCount));
        printSep("Cooldown:", std::to_string(ci.cooldownSec)+"s");
    });
    chCmd->add_subcommand("config", "Configuration")->callback([&]() { cmdChannelConfig(ch); });
    chCmd->add_subcommand("cal", "Calibration")->callback([&]() { cmdChannelCal(ch); });

    {
        auto* sc = chCmd->add_subcommand("output", "Set output action"); std::string a;
        sc->add_option("action", a, "NONE|ENABLE|DISABLE-GRACEFUL|DISABLE-IMMEDIATE")->required();
        sc->callback([&]() {
            hvb::OutputAction oa = hvb::OutputAction::None;
            if (a=="ENABLE") oa=hvb::OutputAction::Enable;
            else if (a=="DISABLE-GRACEFUL") oa=hvb::OutputAction::DisableGraceful;
            else if (a=="DISABLE-IMMEDIATE") oa=hvb::OutputAction::DisableImmediate;
            std::cout << (g_client->sendOutputAction(ch,oa)?"OK\n":g_client->lastError()+"\n"); });
    }
    {
        auto* sc = chCmd->add_subcommand("fault", "Fault command"); std::string c;
        sc->add_option("cmd", c, "CLEAR-ACTIVE|CLEAR-HISTORY")->required();
        sc->callback([&]() {
            hvb::ChannelFaultCommand fc = hvb::ChannelFaultCommand::None;
            if (c=="CLEAR-ACTIVE") fc=hvb::ChannelFaultCommand::ClearActiveFaultBlock;
            else if (c=="CLEAR-HISTORY") fc=hvb::ChannelFaultCommand::ClearFaultHistory;
            std::cout << (g_client->sendChannelFaultCommand(ch,fc)?"OK\n":g_client->lastError()+"\n"); });
    }
    {
        auto* sc = chCmd->add_subcommand("voltage", "Set configured target voltage (raw LSB)");
        uint16_t raw = 0;
        sc->add_option("raw", raw, "Raw LSB value")->required();
        sc->callback([&]() { std::cout << (g_client->writeConfiguredTargetVoltage(ch,raw)?"OK\n":g_client->lastError()+"\n"); });
    }
    {
        auto* sc = chCmd->add_subcommand("ramp-up", "Set ramp up (raw step, interval x10s)");
        uint16_t step=0, interval=0;
        sc->add_option("step", step, "Raw LSB")->required();
        sc->add_option("interval", interval, "Interval x10s")->required();
        sc->callback([&]() { std::cout << (g_client->writeRampUp(ch,step,interval)?"OK\n":g_client->lastError()+"\n"); });
    }
    {
        auto* sc = chCmd->add_subcommand("ramp-down", "Set ramp down (raw step, interval x10s)");
        uint16_t step=0, interval=0;
        sc->add_option("step", step, "Raw LSB")->required();
        sc->add_option("interval", interval, "Interval x10s")->required();
        sc->callback([&]() { std::cout << (g_client->writeRampDown(ch,step,interval)?"OK\n":g_client->lastError()+"\n"); });
    }
    {
        auto* sc = chCmd->add_subcommand("prot-v", "Set voltage protection"); std::string m,a; uint16_t t=0;
        sc->add_option("mode", m, "DISABLED|FLAG-ONLY|APPLY-ACTION")->required();
        sc->add_option("action", a, "NONE|DISABLE-GRACEFUL|DISABLE-IMMEDIATE|FORCE-ZERO|CLAMP")->required();
        sc->add_option("threshold", t, "Raw LSB")->required();
        sc->callback([&]() {
            hvb::ProtectionMode pm = hvb::ProtectionMode::Disabled;
            if (m=="FLAG-ONLY") pm=hvb::ProtectionMode::FlagOnly;
            else if (m=="APPLY-ACTION") pm=hvb::ProtectionMode::ApplyOutputAction;
            hvb::OutputAction oa = hvb::OutputAction::None;
            if (a=="DISABLE-GRACEFUL") oa=hvb::OutputAction::DisableGraceful;
            else if (a=="DISABLE-IMMEDIATE") oa=hvb::OutputAction::DisableImmediate;
            else if (a=="FORCE-ZERO") oa=hvb::OutputAction::ForceOutputZero;
            else if (a=="CLAMP") oa=hvb::OutputAction::Clamp;
            std::cout << (g_client->writeVoltageProtection(ch,pm,oa,t)?"OK\n":g_client->lastError()+"\n"); });
    }
    {
        auto* sc = chCmd->add_subcommand("prot-i", "Set current protection"); std::string m,a; uint16_t t=0;
        sc->add_option("mode", m, "DISABLED|FLAG-ONLY|APPLY-ACTION")->required();
        sc->add_option("action", a, "NONE|DISABLE-GRACEFUL|DISABLE-IMMEDIATE|FORCE-ZERO")->required();
        sc->add_option("threshold", t, "Raw LSB")->required();
        sc->callback([&]() {
            hvb::ProtectionMode pm = hvb::ProtectionMode::Disabled;
            if (m=="FLAG-ONLY") pm=hvb::ProtectionMode::FlagOnly;
            else if (m=="APPLY-ACTION") pm=hvb::ProtectionMode::ApplyOutputAction;
            hvb::OutputAction oa = hvb::OutputAction::None;
            if (a=="DISABLE-GRACEFUL") oa=hvb::OutputAction::DisableGraceful;
            else if (a=="DISABLE-IMMEDIATE") oa=hvb::OutputAction::DisableImmediate;
            else if (a=="FORCE-ZERO") oa=hvb::OutputAction::ForceOutputZero;
            std::cout << (g_client->writeCurrentProtection(ch,pm,oa,t)?"OK\n":g_client->lastError()+"\n"); });
    }
    {
        auto* sc = chCmd->add_subcommand("derate", "Set derate step (raw LSB)");
        uint16_t s=0; sc->add_option("step", s, "Raw LSB")->required();
        sc->callback([&]() { std::cout << (g_client->writeDerateStep(ch,s)?"OK\n":g_client->lastError()+"\n"); });
    }
    {
        auto* sc = chCmd->add_subcommand("cal-out", "Set output calibration"); uint16_t k=10000, b=0;
        sc->add_option("k", k)->required(); sc->add_option("b", b)->required();
        sc->callback([&]() { std::cout << (g_client->writeCalibrationOutput(ch,k,b)?"OK\n":g_client->lastError()+"\n"); });
    }
    {
        auto* sc = chCmd->add_subcommand("cal-meas-v", "Set meas V calibration"); uint16_t k=10000, b=0;
        sc->add_option("k", k)->required(); sc->add_option("b", b)->required();
        sc->callback([&]() { std::cout << (g_client->writeCalibrationMeasV(ch,k,b)?"OK\n":g_client->lastError()+"\n"); });
    }
    {
        auto* sc = chCmd->add_subcommand("cal-meas-i", "Set meas I calibration"); uint16_t k=10000, b=0;
        sc->add_option("k", k)->required(); sc->add_option("b", b)->required();
        sc->callback([&]() { std::cout << (g_client->writeCalibrationMeasI(ch,k,b)?"OK\n":g_client->lastError()+"\n"); });
    }

    chCmd->add_subcommand("save", "Save channel params")->callback([&]() { std::cout << (g_client->sendParamAction(ch,hvb::ParamAction::Save)?"OK\n":g_client->lastError()+"\n"); });
    chCmd->add_subcommand("load", "Load channel params")->callback([&]() { std::cout << (g_client->sendParamAction(ch,hvb::ParamAction::Load)?"OK\n":g_client->lastError()+"\n"); });
    chCmd->add_subcommand("factory", "Factory reset channel")->callback([&]() { std::cout << (g_client->sendParamAction(ch,hvb::ParamAction::FactoryReset)?"OK\n":g_client->lastError()+"\n"); });

    // Reset
    app.add_subcommand("reset", "Software reset")->callback([&]() { std::cout << (g_client->sendParamAction(-1,hvb::ParamAction::SoftwareReset)?"OK\n":g_client->lastError()+"\n"); });

    // Raw
    auto* rawCmd = app.add_subcommand("raw", "Raw Modbus")->require_subcommand(1);
    {
        auto* sc = rawCmd->add_subcommand("fc04", "Raw FC04 read"); uint16_t a=0,c=16;
        sc->add_option("addr", a)->required(); sc->add_option("count", c);
        sc->callback([&]() { cmdRawFc04(a,c); });
    }
    {
        auto* sc = rawCmd->add_subcommand("fc03", "Raw FC03 read"); uint16_t a=0,c=16;
        sc->add_option("addr", a)->required(); sc->add_option("count", c);
        sc->callback([&]() { cmdRawFc03(a,c); });
    }
    {
        auto* sc = rawCmd->add_subcommand("fc06", "Raw FC06 write"); uint32_t a=0,v=0;
        sc->add_option("addr", a)->required(); sc->add_option("value", v)->required();
        sc->callback([&]() { cmdRawFc06(static_cast<uint16_t>(a),static_cast<uint16_t>(v)); });
    }

    CLI11_PARSE(app, argc, argv);

    // Connection resolution
    if (port.empty() && cfgMgr.hasConnectionSettings()) {
        port = cfgMgr.port; baud = cfgMgr.baudRate; slaveId = cfgMgr.slaveId; timeout = cfgMgr.timeoutMs;
    }
    if (!port.empty()) {
        cfgMgr.setFromArgs(port, baud, slaveId, timeout);
        if (save) cfgMgr.save();
    }

    hvb::HvbModbusClient client;
    g_client = &client;

    bool hasSub = false;
    for (auto* sc : app.get_subcommands()) { if (sc->parsed()) { hasSub = true; break; } }

    if (!port.empty() && hasSub) {
        if (!client.connect(port, baud, slaveId, timeout)) {
            std::cerr << "Connection error: " << client.lastError() << "\n";
            return 1;
        }
    }

    if (describeCmd->parsed()) cmdDescribe(static_cast<uint16_t>(hexAddr));
    if (monitorCmd->parsed()) cmdMonitor(interval);

    client.disconnect();
    return 0;
}
