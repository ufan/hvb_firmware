#include "psb_modbus_client.h"
#include "config_manager.h"
#include "register_map.h"
#include "register_meta.h"

#include <CLI/CLI.hpp>

#include <iostream>
#include <iomanip>
#include <sstream>
#include <thread>
#include <chrono>
#include <cmath>
#include <csignal>

// ============================================================================
//  Output helpers — raw LSB + estimated physical
// ============================================================================

static std::string formatVRaw(uint16_t raw) {
    auto v = psb::reg::voltageToV(raw);
    std::ostringstream ss;
    ss << raw << " LSB ≈ " << std::showpos << std::fixed << std::setprecision(1) << v << " V";
    return ss.str();
}
static std::string formatIRaw(uint16_t raw, int16_t currentUnitExp) {
    auto a = psb::reg::currentToA(static_cast<int16_t>(raw), currentUnitExp);
    std::ostringstream ss;
    ss << raw << " LSB ≈ " << psb::reg::formatAmpsAuto(a);
    return ss.str();
}

static void printSep(const std::string& left, const std::string& right) {
    std::cout << std::left << std::setw(24) << left << " " << right << "\n";
}

static void printStatusBits(uint16_t status) {
    auto yn = [status](uint16_t m) { return (status & m) ? "Yes" : "No"; };
    std::cout << "  Output Drive:     " << yn(psb::ChStatus::OUTPUT_DRIVE_NONZERO) << "\n";
    std::cout << "  Output Enable:    " << yn(psb::ChStatus::OUTPUT_ENABLE_ACTIVE) << "\n";
    std::cout << "  Ramping:          " << yn(psb::ChStatus::RAMPING_ACTIVE) << "\n";
    std::cout << "  Active Fault:     " << yn(psb::ChStatus::ACTIVE_FAULT) << "\n";
    std::cout << "  Fault History:    " << yn(psb::ChStatus::FAULT_HISTORY) << "\n";
    std::cout << "  Cooldown:         " << yn(psb::ChStatus::COOLDOWN_ACTIVE) << "\n";
    std::cout << "  Meas Stale:       " << yn(psb::ChStatus::MEASUREMENT_STALE) << "\n";
}

static void printFaultCause(uint16_t fault, const char* label) {
    std::cout << std::left << std::setw(22) << label;
    if (fault == 0) { std::cout << "None\n"; return; }
    bool first = true;
    auto p = [&](const char* s) { if (!first) std::cout << ","; std::cout << s; first = false; };
    if (fault & psb::FaultCause::CURRENT)        p("CL");
    if (fault & psb::FaultCause::MEASUREMENT)    p("MI");
    if (fault & psb::FaultCause::HARDWARE)       p("HW");
    if (fault & psb::FaultCause::INTERLOCK)      p("IL");
    if (fault & psb::FaultCause::RETRY_EXHAUST)  p("RE");
    if (fault & psb::FaultCause::CFG_INVALID)    p("CI");
    if (fault & psb::FaultCause::STALE)          p("ST");
    std::cout << "\n";
}

static std::string formatHex(uint16_t v) {
    std::ostringstream ss;
    ss << "0x" << std::hex << std::setw(4) << std::setfill('0') << v;
    return ss.str();
}

extern std::string renderMonitorTable(const psb::SystemInfo&,
                                      const std::vector<psb::ChannelInfo>&);

// ============================================================================
//  Globals
// ============================================================================

static psb::PsbModbusClient*    g_client  = nullptr;
static volatile sig_atomic_t    g_running = 1;
static void sigintHandler(int) { g_running = 0; }

// ============================================================================
//  Commands
// ============================================================================

int cmdListPorts() {
    auto ports = psb::PsbModbusClient::scanPorts();
    if (ports.empty()) { std::cout << "No serial ports found.\n"; return 0; }
    for (const auto& p : ports) std::cout << p << "\n";
    return 0;
}

int cmdListRegs() { std::cout << psb::meta::formatRegisterCatalog(); return 0; }

// Print one bank's metadata, plus a live decoded value (via psb::meta::formatValue)
// if a device is connected.
static void describeBank(const char* label, const psb::meta::RegDesc* d, uint16_t addr, bool holding) {
    if (!d) return;
    printSep(label, std::string(d->name) + " " + d->desc);
    printSep("Type:", std::string(d->type) + (d->unit[0] ? std::string(", ") + d->unit : ""));
    if (!g_client->isConnected()) return;
    uint16_t raw = 0;
    bool ok = holding ? g_client->readHoldingRegs(addr, 1, &raw)
                       : g_client->readInputRegs(addr, 1, &raw);
    if (ok) printSep("Value:", psb::meta::formatValue(raw, *d));
    else    std::cerr << "Read error: " << g_client->lastError() << "\n";
}

int cmdDescribe(uint16_t addr) {
    const auto* holdingDesc = psb::meta::findDesc(addr, true);
    const auto* inputDesc   = psb::meta::findDesc(addr, false);
    if (!holdingDesc && !inputDesc) {
        std::cout << "No register at 0x" << std::hex << addr << std::dec << "\n";
        return 1;
    }
    describeBank("Holding:", holdingDesc, addr, true);
    describeBank("Input:",   inputDesc,   addr, false);
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
        + (info.sysCapFlags & psb::SysCap::AUTOMATIC_MODE ? "[Auto]" : "")
        + (info.sysCapFlags & psb::SysCap::ENV_SENSOR ? "[Env]" : "")
        + (info.sysCapFlags & psb::SysCap::CALIBRATION_MODE ? "[Cal]" : ""));
    printSep("Channels:", std::to_string(info.supportedChannels) + " (mask " + formatHex(info.activeChMask) + ")");
    printSep("Board Temp:", std::to_string(info.boardTempRaw) + " raw");
    printSep("Humidity:", std::to_string(info.boardHumidityRaw) + " raw");
    printSep("Uptime:", std::to_string(info.uptimeSec) + " s");
    printSep("FW Version:", formatHex(info.fwVersion));
    printSep("Active OpMode:", psb::opModeName(info.activeOpMode));
    printSep("Sys Status:", formatHex(info.sysStatus));
    printSep("Fault Cause:", formatHex(info.faultCause));
    return 0;
}

int cmdStatus() {
    auto sysInfo = g_client->readSystemInfo();
    if (!g_client->isConnected()) { std::cerr << "Error\n"; return 1; }
    for (int ch = 0; ch < sysInfo.supportedChannels; ++ch) {
        auto ci = g_client->readChannelInfo(ch);
        if (!g_client->isConnected()) { std::cerr << "Error ch" << ch << "\n"; return 1; }
        if (!g_client->isConnected()) { std::cerr << "Error ch" << ch << "\n"; break; }
        std::cout << "=== Channel " << ch << " ===\n";
        printSep("Measured Voltage:", formatVRaw(ci.voltageRaw));
        printSep("Measured Current:", formatIRaw(ci.currentRaw, sysInfo.currentUnitExp));
        printSep("Operational Target:", formatVRaw(ci.operationalTargetVoltageRaw));
        printSep("Status:", formatHex(ci.status));
        printStatusBits(ci.status);
        printFaultCause(ci.activeFault, "Active Fault:");
        printFaultCause(ci.faultHistory, "Fault History:");
        printSep("Last Prot Output:", psb::outputActionName(static_cast<psb::OutputAction>(ci.lastProtOutputAction)));
        printSep("Retries:", std::to_string(ci.retryCount));
        printSep("Cooldown:", std::to_string(ci.cooldownSec) + " s");
        printSep("Last Fault TS:", std::to_string(ci.lastFaultTimestamp) + " s");
        printSep("Cap Flags:", formatHex(ci.chCapFlags)
            + (ci.chCapFlags & CH_CAP_OUTPUT_ENABLE ? " [OutEn]" : "")
            + (ci.chCapFlags & CH_CAP_RAW_OUTPUT_DRIVE ? " [RawDrive]" : "")
            + (ci.chCapFlags & CH_CAP_VOLTAGE_MEASUREMENT ? " [VMeas]" : "")
            + (ci.chCapFlags & CH_CAP_CURRENT_MEASUREMENT ? " [IMeas]" : ""));
        if (ch != sysInfo.supportedChannels - 1) std::cout << "\n";
    }
    return 0;
}

int cmdMonitor(int intervalSec) {
    if (!g_client->isConnected()) { std::cerr << "Not connected\n"; return 1; }
    g_running = 1;
    ::signal(SIGINT, sigintHandler);
    while (g_running) {
        auto info = g_client->readSystemInfo();
        std::vector<psb::ChannelInfo> channels;
        for (int ch = 0; ch < info.supportedChannels; ++ch)
            channels.push_back(g_client->readChannelInfo(ch));
        std::cout << "\033[2J\033[H" << renderMonitorTable(info, channels) << std::flush;
        for (int i = 0; i < intervalSec && g_running; ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    std::cout << "\n";
    return 0;
}

int cmdSystemConfig() {
    auto cfg = g_client->readSystemConfig();
    if (!g_client->isConnected()) return 1;
    std::cout << "=== System Configuration ===\n";
    printSep("Operating Mode:", psb::opModeName(cfg.operatingMode));
    printSep("Startup Ch Policy:", std::to_string(cfg.startupChannelPolicy));
    printSep("Slave Address:", std::to_string(cfg.slaveAddr));
    printSep("Baud Rate:", cfg.baudRateCode == 0 ? "115200" : "9600");
    return 0;
}

int cmdChannelConfig(int ch) {
    uint16_t caps = g_client->readChannelInfo(ch).chCapFlags;
    auto cfg = g_client->readChannelConfig(ch, caps);
    if (!g_client->isConnected()) return 1;
    int16_t currentUnitExp = g_client->readSystemInfo().currentUnitExp;
    std::cout << "=== Channel " << ch << " Configuration ===\n";
    if (caps & CH_CAP_RAW_OUTPUT_DRIVE) {
        printSep("Configured Target:", formatVRaw(cfg.configuredTargetVRaw));
    } else if (caps & CH_CAP_OUTPUT_ENABLE) {
        printSep("Output Enabled (cfg):", cfg.outputEnabledCfg ? "1 (on)" : "0 (off)");
    } else {
        printSep("Output Enabled (cfg):", "n/a — channel is locked always-on");
    }
    printSep("Output Action:", psb::outputActionName(cfg.outputAction));
    printSep("Fault Command:", psb::faultCommandName(cfg.faultCommand));
    if (caps & CH_CAP_RAW_OUTPUT_DRIVE) {
        printSep("Ramp Up:", std::to_string(cfg.rampUpStepRaw) + " raw / " + std::to_string(cfg.rampUpInterval) + " x10s");
        printSep("Ramp Down:", std::to_string(cfg.rampDownStepRaw) + " raw / " + std::to_string(cfg.rampDownInterval) + " x10s");
    } else {
        printSep("Ramp Up/Down:", "n/a — no DAC");
    }
    printSep("Recovery Policy:", psb::recoveryPolicyName(cfg.recoveryPolicyMode));
    printSep("Auto Retry:", "delay=" + std::to_string(cfg.autoRetryDelay) + "s  max="
             + std::to_string(cfg.autoRetryMaxCount) + "  window=" + std::to_string(cfg.autoRetryWindow) + "s");
    printSep("I Safe Band:", std::to_string(cfg.currentSafeBandPct) + "%");
    if (caps & CH_CAP_CURRENT_MEASUREMENT) {
        printSep("I Protection:", std::string(psb::protectionModeName(cfg.iProtMode)) + " / "
                 + psb::outputActionName(cfg.iProtOutputAction) + " / " + formatIRaw(cfg.iLimitThresholdRaw, currentUnitExp));
    } else {
        printSep("I Protection:", "n/a — no current measurement");
    }
    if ((caps & CH_CAP_RAW_OUTPUT_DRIVE) && (caps & CH_CAP_VOLTAGE_MEASUREMENT)) {
        printSep("Derate Step:", formatVRaw(cfg.derateStepRaw));
    } else {
        printSep("Derate Step:", "n/a — needs DAC + voltage measurement");
    }
    return 0;
}

int cmdChannelCal(int ch) {
    auto cal = g_client->readChannelCalConfig(ch);
    if (!g_client->isConnected()) return 1;
    std::cout << "=== Channel " << ch << " Calibration ===\n";
    // Gain is k * 10^k_exp (decimal floating-point, v3.1+) - not a fixed
    // divisor, so display the live exponent rather than a hardcoded label.
    auto expTag = [](int16_t exp) {
        return " (x10^" + std::to_string(exp) + ")";
    };
    printSep("Output:", "K=" + std::to_string(cal.outCalK) + expTag(cal.outCalKExp)
             + "  B=" + std::to_string(cal.outCalB) + " (x1000)");
    printSep("Meas V:", "K=" + std::to_string(cal.measVCalK) + expTag(cal.measVCalKExp)
             + "  B=" + std::to_string(cal.measVCalB) + " (x1000)");
    printSep("Meas I:", "K=" + std::to_string(cal.measICalK) + expTag(cal.measICalKExp)
             + "  B=" + std::to_string(cal.measICalB) + " (x1000)");
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
    CLI::App app{"PSB Demo App"};
    psb::ConfigManager cfgMgr;
    cfgMgr.load();

    // ===================================================================
    //  ALL option variables MUST be declared here — nested {} scopes
    //  would cause stack-use-after-scope when CLI11 writes back values.
    // ===================================================================

    // Global
    std::string port;
    int baud = 115200, slaveId = 1, timeout = 500;
    bool save = false;
    app.add_option("-p,--port", port, "Serial port");
    app.add_option("-b,--baud", baud, "Baud rate")->check(CLI::IsMember({9600, 115200}));
    app.add_option("-i,--id", slaveId, "Slave ID")->check(CLI::Range(0, 247));
    app.add_option("-t,--timeout", timeout, "Timeout ms");
    app.add_flag("--save", save, "Save connection to config");

    // Describe
    uint32_t hexAddr = 0;
    auto* describeCmd = app.add_subcommand("describe", "Show register metadata");
    describeCmd->add_option("addr", hexAddr, "PDU address (hex)")->required();
    describeCmd->callback([&]() { cmdDescribe(static_cast<uint16_t>(hexAddr)); });

    // Monitor
    int interval = 2;
    auto* monitorCmd = app.add_subcommand("monitor", "Live polling");
    monitorCmd->add_option("interval", interval, "Poll interval (s)");
    monitorCmd->callback([&]() { cmdMonitor(interval); });

    // System subcommand option variables
    std::string sys_mode;
    uint16_t sys_addr_val=1;
    uint16_t sys_baud_code=0;

    // Channel subcommand option variables
    std::string ch_output_action;
    std::string ch_fault_cmd;
    uint16_t ch_voltage_raw = 0;
    uint16_t ch_ramp_up_step=0, ch_ramp_up_interval=0;
    uint16_t ch_ramp_dn_step=0, ch_ramp_dn_interval=0;
    std::string ch_recovery_policy; int ch_recovery_d=0, ch_recovery_mx=0, ch_recovery_w=0;
    uint16_t ch_safe_band_pct=10;
    std::string ch_prot_i_mode, ch_prot_i_action; uint16_t ch_prot_i_thresh=0;
    uint16_t ch_derate_step=0;

    // Raw subcommand option variables
    uint16_t raw_fc04_addr=0, raw_fc04_count=16;
    uint16_t raw_fc03_addr=0, raw_fc03_count=16;
    uint32_t raw_fc06_addr=0, raw_fc06_value=0;

    // ===================================================================
    //  Subcommand definitions
    // ===================================================================

    // Discovery
    auto* listCmd = app.add_subcommand("list", "Discovery")->require_subcommand(1);
    listCmd->add_subcommand("ports", "List serial ports")->callback([&]() { cmdListPorts(); });
    listCmd->add_subcommand("regs", "List register catalog")->callback([&]() { cmdListRegs(); });

    // Top-level reads
    app.add_subcommand("info", "System info dump")->callback([&]() { cmdInfo(); });
    app.add_subcommand("status", "Channel summary")->callback([&]() { cmdStatus(); });

    // System subcommands
    auto* sysCmd = app.add_subcommand("system", "System config read/write")->require_subcommand(1);
    sysCmd->add_subcommand("config", "Read system config")->callback([&]() { cmdSystemConfig(); });

    auto* sysMode = sysCmd->add_subcommand("mode", "Set operating mode");
    sysMode->add_option("mode", sys_mode, "NORMAL|AUTO")->required();
    sysMode->callback([&]() { std::cout << (g_client->writeOperatingMode(sys_mode=="AUTO"?psb::OpMode::Automatic:psb::OpMode::Normal)?"OK\n":g_client->lastError()+"\n"); });

    auto* sysStartupPol = sysCmd->add_subcommand("startup-policy", "Set startup channel policy");
    uint16_t sys_startup_policy = 0;
    sysStartupPol->add_option("policy", sys_startup_policy, "0=load-nvs, 1=factory-default")->required()->check(CLI::Range(0u,1u));
    sysStartupPol->callback([&]() { std::cout << (g_client->writeStartupChannelPolicy(sys_startup_policy)?"OK\n":g_client->lastError()+"\n"); });

    auto* sysAddr = sysCmd->add_subcommand("addr", "Set slave address");
    sysAddr->add_option("addr", sys_addr_val, "0-247")->required()->check(CLI::Range(0u,247u));
    sysAddr->callback([&]() { std::cout << (g_client->writeSlaveAddress(sys_addr_val)?"OK\n":g_client->lastError()+"\n"); });

    auto* sysBaud = sysCmd->add_subcommand("baud", "Set baud rate");
    sysBaud->add_option("code", sys_baud_code, "0=115200,1=9600")->required()->check(CLI::Range(0u,1u));
    sysBaud->callback([&]() { std::cout << (g_client->writeBaudRateCode(sys_baud_code)?"OK\n":g_client->lastError()+"\n"); });

    sysCmd->add_subcommand("save", "Save system params")->callback([&]() { std::cout << (g_client->sendParamAction(-1,psb::ParamAction::Save)?"OK\n":g_client->lastError()+"\n"); });
    sysCmd->add_subcommand("load", "Load system params")->callback([&]() { std::cout << (g_client->sendParamAction(-1,psb::ParamAction::Load)?"OK\n":g_client->lastError()+"\n"); });
    sysCmd->add_subcommand("factory", "Factory reset system")->callback([&]() { std::cout << (g_client->sendParamAction(-1,psb::ParamAction::FactoryReset)?"OK\n":g_client->lastError()+"\n"); });

    // Calibration session commands
    auto* calCmd = app.add_subcommand("cal", "Calibration session commands")->require_subcommand(1);
    calCmd->add_subcommand("exit", "Exit calibration mode and restore the pre-calibration operating mode")
        ->callback([&]() {
            std::cout << (g_client->exitCalibrationMode() ? "OK\n" : g_client->lastError() + "\n");
        });

    // Channel subcommands
    auto* chCmd = app.add_subcommand("channel", "Channel operations");
    int ch = 0;
    // Upper bound is the protocol maximum (psb::reg::MAX_CHANNELS), not the
    // connected board's actual channel count — that isn't known until after
    // connecting, which happens later than CLI11's option parsing/validation.
    // An out-of-range channel for the connected board is rejected by the
    // device itself (Modbus exception), same as any other unsupported access.
    chCmd->add_option("channel", ch, "0.." + std::to_string(psb::reg::MAX_CHANNELS - 1))
        ->required()->check(CLI::Range(0, static_cast<int>(psb::reg::MAX_CHANNELS) - 1));
    chCmd->require_subcommand(1);

    chCmd->add_subcommand("info", "Measurements")->callback([&]() {
        auto ci = g_client->readChannelInfo(ch);
        if (!g_client->isConnected()) return;
        int16_t currentUnitExp = g_client->readSystemInfo().currentUnitExp;
        std::cout << "=== Channel " << ch << " ===\n";
        printSep("Measured V:", formatVRaw(ci.voltageRaw));
        printSep("Measured I:", formatIRaw(ci.currentRaw, currentUnitExp));
        printSep("Operational Target:", formatVRaw(ci.operationalTargetVoltageRaw));
        printSep("Status:", formatHex(ci.status)); printStatusBits(ci.status);
        printFaultCause(ci.activeFault, "Active Fault:");
        printFaultCause(ci.faultHistory, "Fault History:");
        printSep("Last Prot:", psb::outputActionName(static_cast<psb::OutputAction>(ci.lastProtOutputAction)));
        printSep("Retries:", std::to_string(ci.retryCount));
        printSep("Cooldown:", std::to_string(ci.cooldownSec)+"s");
    });
    chCmd->add_subcommand("config", "Configuration")->callback([&]() { cmdChannelConfig(ch); });
    chCmd->add_subcommand("cal", "Calibration")->callback([&]() { cmdChannelCal(ch); });

    auto* chOutput = chCmd->add_subcommand("output", "Set output action");
    chOutput->add_option("action", ch_output_action, "NONE|ENABLE|DISABLE-GRACEFUL|DISABLE-IMMEDIATE")->required();
    chOutput->callback([&]() {
        psb::OutputAction oa = psb::OutputAction::None;
        if (ch_output_action=="ENABLE") oa=psb::OutputAction::Enable;
        else if (ch_output_action=="DISABLE-GRACEFUL") oa=psb::OutputAction::DisableGraceful;
        else if (ch_output_action=="DISABLE-IMMEDIATE") oa=psb::OutputAction::DisableImmediate;
        bool isDisable = (oa == psb::OutputAction::DisableGraceful || oa == psb::OutputAction::DisableImmediate);
        if (isDisable) {
            uint16_t caps = g_client->readChannelInfo(ch).chCapFlags;
            if (!(caps & CH_CAP_OUTPUT_ENABLE)) {
                std::cerr << "ch" << ch << " is locked always-on (CH_CAP_OUTPUT_ENABLE absent) — "
                          << "disable will be rejected by the device\n";
            }
        }
        std::cout << (g_client->sendOutputAction(ch,oa)?"OK\n":g_client->lastError()+"\n"); });

    auto* chFault = chCmd->add_subcommand("fault", "Fault command");
    chFault->add_option("cmd", ch_fault_cmd, "CLEAR-ACTIVE|CLEAR-HISTORY")->required();
    chFault->callback([&]() {
        psb::ChannelFaultCommand fc = psb::ChannelFaultCommand::None;
        if (ch_fault_cmd=="CLEAR-ACTIVE") fc=psb::ChannelFaultCommand::ClearActiveFaultBlock;
        else if (ch_fault_cmd=="CLEAR-HISTORY") fc=psb::ChannelFaultCommand::ClearFaultHistory;
        std::cout << (g_client->sendChannelFaultCommand(ch,fc)?"OK\n":g_client->lastError()+"\n"); });

    auto* chVoltage = chCmd->add_subcommand("voltage", "Set configured target voltage (raw LSB) — DAC channels only");
    chVoltage->add_option("raw", ch_voltage_raw, "Raw LSB value")->required();
    chVoltage->callback([&]() {
        uint16_t caps = g_client->readChannelInfo(ch).chCapFlags;
        if (!(caps & CH_CAP_RAW_OUTPUT_DRIVE)) {
            std::cerr << "ch" << ch << " has no DAC (CH_CAP_RAW_OUTPUT_DRIVE absent) — "
                      << "use 'channel " << ch << " enable-cfg' for a fixed-voltage channel instead\n";
            return;
        }
        std::cout << (g_client->writeConfiguredTargetVoltage(ch,ch_voltage_raw)?"OK\n":g_client->lastError()+"\n");
    });

    uint16_t ch_enable_cfg = 0;
    auto* chEnableCfg = chCmd->add_subcommand("enable-cfg",
        "Set startup/AUTOMATIC-mode desired on/off state (0|1) — fixed-voltage channels only");
    chEnableCfg->add_option("value", ch_enable_cfg, "0=off, 1=on")->required()->check(CLI::Range(0u,1u));
    chEnableCfg->callback([&]() {
        uint16_t caps = g_client->readChannelInfo(ch).chCapFlags;
        if (caps & CH_CAP_RAW_OUTPUT_DRIVE) {
            std::cerr << "ch" << ch << " has a DAC (CH_CAP_RAW_OUTPUT_DRIVE) — "
                      << "use 'channel " << ch << " voltage' instead\n";
            return;
        }
        if (!(caps & CH_CAP_OUTPUT_ENABLE)) {
            std::cerr << "ch" << ch << " is locked always-on (CH_CAP_OUTPUT_ENABLE absent) — "
                      << "startup state can't be changed\n";
            return;
        }
        std::cout << (g_client->writeOutputEnabled(ch, ch_enable_cfg != 0)?"OK\n":g_client->lastError()+"\n");
    });

    auto* chRampUp = chCmd->add_subcommand("ramp-up", "Set ramp up (raw step, interval x10s)");
    chRampUp->add_option("step", ch_ramp_up_step, "Raw LSB")->required();
    chRampUp->add_option("interval", ch_ramp_up_interval, "Interval x10s")->required();
    chRampUp->callback([&]() { std::cout << (g_client->writeRampUp(ch,ch_ramp_up_step,ch_ramp_up_interval)?"OK\n":g_client->lastError()+"\n"); });

    auto* chRampDn = chCmd->add_subcommand("ramp-down", "Set ramp down (raw step, interval x10s)");
    chRampDn->add_option("step", ch_ramp_dn_step, "Raw LSB")->required();
    chRampDn->add_option("interval", ch_ramp_dn_interval, "Interval x10s")->required();
    chRampDn->callback([&]() { std::cout << (g_client->writeRampDown(ch,ch_ramp_dn_step,ch_ramp_dn_interval)?"OK\n":g_client->lastError()+"\n"); });

    auto* chRecovery = chCmd->add_subcommand("recovery", "Set channel recovery policy");
    chRecovery->add_option("policy", ch_recovery_policy, "MANUAL-LATCH|AUTO-RETRY|AUTO-DERATE|NEVER-RETRY")->required();
    chRecovery->add_option("delay", ch_recovery_d, "Cooldown seconds")->required();
    chRecovery->add_option("max", ch_recovery_mx, "Max retries")->required();
    chRecovery->add_option("window", ch_recovery_w, "Retry window seconds")->required();
    chRecovery->callback([&]() {
        psb::RecoveryPolicy rp = psb::RecoveryPolicy::ManualLatch;
        if (ch_recovery_policy=="AUTO-RETRY") rp=psb::RecoveryPolicy::AutoRetry;
        else if (ch_recovery_policy=="AUTO-DERATE") rp=psb::RecoveryPolicy::AutoDerateRetry;
        else if (ch_recovery_policy=="NEVER-RETRY") rp=psb::RecoveryPolicy::NeverRetry;
        std::cout << (g_client->writeChannelRecovery(ch,rp,ch_recovery_d,ch_recovery_mx,ch_recovery_w)?"OK\n":g_client->lastError()+"\n");
    });

    auto* chSafeBand = chCmd->add_subcommand("safe-band", "Set I safe band %");
    chSafeBand->add_option("pct", ch_safe_band_pct, "0-50")->required()->check(CLI::Range(0u,50u));
    chSafeBand->callback([&]() { std::cout << (g_client->writeChannelSafeBand(ch,ch_safe_band_pct)?"OK\n":g_client->lastError()+"\n"); });

    auto* chProtI = chCmd->add_subcommand("prot-i", "Set current protection");
    chProtI->add_option("mode", ch_prot_i_mode, "DISABLED|FLAG-ONLY|APPLY-ACTION")->required();
    chProtI->add_option("action", ch_prot_i_action, "NONE|DISABLE-GRACEFUL|DISABLE-IMMEDIATE|FORCE-ZERO")->required();
    chProtI->add_option("threshold", ch_prot_i_thresh, "Raw LSB")->required();
    chProtI->callback([&]() {
        psb::ProtectionMode pm = psb::ProtectionMode::Disabled;
        if (ch_prot_i_mode=="FLAG-ONLY") pm=psb::ProtectionMode::FlagOnly;
        else if (ch_prot_i_mode=="APPLY-ACTION") pm=psb::ProtectionMode::ApplyOutputAction;
        psb::OutputAction oa = psb::OutputAction::None;
        if (ch_prot_i_action=="DISABLE-GRACEFUL") oa=psb::OutputAction::DisableGraceful;
        else if (ch_prot_i_action=="DISABLE-IMMEDIATE") oa=psb::OutputAction::DisableImmediate;
        else if (ch_prot_i_action=="FORCE-ZERO") oa=psb::OutputAction::ForceOutputZero;
        std::cout << (g_client->writeCurrentProtection(ch,pm,oa,ch_prot_i_thresh)?"OK\n":g_client->lastError()+"\n"); });

    auto* chDerate = chCmd->add_subcommand("derate", "Set derate step (raw LSB)");
    chDerate->add_option("step", ch_derate_step, "Raw LSB")->required();
    chDerate->callback([&]() { std::cout << (g_client->writeDerateStep(ch,ch_derate_step)?"OK\n":g_client->lastError()+"\n"); });

    chCmd->add_subcommand("save", "Save channel params")->callback([&]() { std::cout << (g_client->sendParamAction(ch,psb::ParamAction::Save)?"OK\n":g_client->lastError()+"\n"); });
    chCmd->add_subcommand("load", "Load channel params")->callback([&]() { std::cout << (g_client->sendParamAction(ch,psb::ParamAction::Load)?"OK\n":g_client->lastError()+"\n"); });
    chCmd->add_subcommand("factory", "Factory reset channel")->callback([&]() { std::cout << (g_client->sendParamAction(ch,psb::ParamAction::FactoryReset)?"OK\n":g_client->lastError()+"\n"); });

    // Reset
    app.add_subcommand("reset", "Software reset")->callback([&]() { std::cout << (g_client->sendParamAction(-1,psb::ParamAction::SoftwareReset)?"OK\n":g_client->lastError()+"\n"); });

    // Raw subcommands
    auto* rawCmd = app.add_subcommand("raw", "Raw Modbus")->require_subcommand(1);
    auto* rawFc04 = rawCmd->add_subcommand("fc04", "Raw FC04 read");
    rawFc04->add_option("addr", raw_fc04_addr)->required();
    rawFc04->add_option("count", raw_fc04_count);
    rawFc04->callback([&]() { cmdRawFc04(raw_fc04_addr, raw_fc04_count); });

    auto* rawFc03 = rawCmd->add_subcommand("fc03", "Raw FC03 read");
    rawFc03->add_option("addr", raw_fc03_addr)->required();
    rawFc03->add_option("count", raw_fc03_count);
    rawFc03->callback([&]() { cmdRawFc03(raw_fc03_addr, raw_fc03_count); });

    auto* rawFc06 = rawCmd->add_subcommand("fc06", "Raw FC06 write");
    rawFc06->add_option("addr", raw_fc06_addr)->required();
    rawFc06->add_option("value", raw_fc06_value)->required();
    rawFc06->callback([&]() { cmdRawFc06(static_cast<uint16_t>(raw_fc06_addr), static_cast<uint16_t>(raw_fc06_value)); });

    psb::PsbModbusClient client;
    g_client = &client;

    // Resolve port from config early (CLI args override below)
    if (port.empty() && cfgMgr.hasConnectionSettings()) {
        port = cfgMgr.port; baud = cfgMgr.baudRate; slaveId = cfgMgr.slaveId; timeout = cfgMgr.timeoutMs;
    }

    // Connect after parsing but before subcommand callbacks fire
    app.parse_complete_callback([&]() {
        if (!port.empty()) {
            if (!client.connect(port, baud, slaveId, timeout)) {
                std::cerr << "Connection error: " << client.lastError() << "\n";
                std::exit(1);
            }
        }
    });

    CLI11_PARSE(app, argc, argv);

    if (!port.empty()) {
        cfgMgr.setFromArgs(port, baud, slaveId, timeout);
        if (save) cfgMgr.save();
    }

    return 0;
}
