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
#include <cmath>
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

extern std::string renderMonitorTable(const hvb::SystemInfo&,
                                      const std::vector<hvb::ChannelInfo>&);

// ============================================================================
//  Globals
// ============================================================================

static hvb::HvbModbusClient*    g_client  = nullptr;
static volatile sig_atomic_t    g_running = 1;
static void sigintHandler(int) { g_running = 0; }

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
    g_running = 1;
    ::signal(SIGINT, sigintHandler);
    while (g_running) {
        auto info = g_client->readSystemInfo();
        std::vector<hvb::ChannelInfo> channels;
        for (int ch = 0; ch < 2; ++ch)
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

    // Monitor
    int interval = 2;
    auto* monitorCmd = app.add_subcommand("monitor", "Live polling");
    monitorCmd->add_option("interval", interval, "Poll interval (s)");
    monitorCmd->callback([&]() { cmdMonitor(interval); });

    // System subcommand option variables
    std::string sys_mode;
    std::string sys_recovery_policy; int sys_recovery_d=0, sys_recovery_mx=0, sys_recovery_w=0;
    uint16_t sys_safe_band_v=10, sys_safe_band_i=10;
    uint16_t sys_addr_val=1;
    uint16_t sys_baud_code=0;

    // Channel subcommand option variables
    std::string ch_output_action;
    std::string ch_fault_cmd;
    uint16_t ch_voltage_raw = 0;
    uint16_t ch_ramp_up_step=0, ch_ramp_up_interval=0;
    uint16_t ch_ramp_dn_step=0, ch_ramp_dn_interval=0;
    std::string ch_prot_v_mode, ch_prot_v_action; uint16_t ch_prot_v_thresh=0;
    std::string ch_prot_i_mode, ch_prot_i_action; uint16_t ch_prot_i_thresh=0;
    uint16_t ch_derate_step=0;
    uint16_t ch_cal_out_k=10000; uint16_t ch_cal_out_b=0;
    uint16_t ch_cal_mv_k=10000; uint16_t ch_cal_mv_b=0;
    uint16_t ch_cal_mi_k=10000; uint16_t ch_cal_mi_b=0;

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
    sysMode->callback([&]() { std::cout << (g_client->writeOperatingMode(sys_mode=="AUTO"?hvb::OpMode::Automatic:hvb::OpMode::Normal)?"OK\n":g_client->lastError()+"\n"); });

    auto* sysRecovery = sysCmd->add_subcommand("recovery", "Set recovery policy");
    sysRecovery->add_option("policy", sys_recovery_policy, "MANUAL-LATCH|AUTO-RETRY|AUTO-DERATE|NEVER-RETRY")->required();
    sysRecovery->add_option("delay", sys_recovery_d, "Cooldown seconds")->required();
    sysRecovery->add_option("max", sys_recovery_mx, "Max retries")->required();
    sysRecovery->add_option("window", sys_recovery_w, "Retry window seconds")->required();
    sysRecovery->callback([&]() {
        hvb::RecoveryPolicy rp = hvb::RecoveryPolicy::ManualLatch;
        if (sys_recovery_policy=="AUTO-RETRY") rp=hvb::RecoveryPolicy::AutoRetry;
        else if (sys_recovery_policy=="AUTO-DERATE") rp=hvb::RecoveryPolicy::AutoDerateRetry;
        else if (sys_recovery_policy=="NEVER-RETRY") rp=hvb::RecoveryPolicy::NeverRetry;
        std::cout << (g_client->writeSystemRecoveryPolicy(rp,sys_recovery_d,sys_recovery_mx,sys_recovery_w)?"OK\n":g_client->lastError()+"\n");
    });

    auto* sysSafeBands = sysCmd->add_subcommand("safe-bands", "Set safe bands");
    sysSafeBands->add_option("v-pct", sys_safe_band_v, "0-50")->required()->check(CLI::Range(0u,50u));
    sysSafeBands->add_option("i-pct", sys_safe_band_i, "0-50")->required()->check(CLI::Range(0u,50u));
    sysSafeBands->callback([&]() { std::cout << (g_client->writeSafeBands(sys_safe_band_v,sys_safe_band_i)?"OK\n":g_client->lastError()+"\n"); });

    auto* sysAddr = sysCmd->add_subcommand("addr", "Set slave address");
    sysAddr->add_option("addr", sys_addr_val, "0-247")->required()->check(CLI::Range(0u,247u));
    sysAddr->callback([&]() { std::cout << (g_client->writeSlaveAddress(sys_addr_val)?"OK\n":g_client->lastError()+"\n"); });

    auto* sysBaud = sysCmd->add_subcommand("baud", "Set baud rate");
    sysBaud->add_option("code", sys_baud_code, "0=115200,1=9600")->required()->check(CLI::Range(0u,1u));
    sysBaud->callback([&]() { std::cout << (g_client->writeBaudRateCode(sys_baud_code)?"OK\n":g_client->lastError()+"\n"); });

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

    auto* chOutput = chCmd->add_subcommand("output", "Set output action");
    chOutput->add_option("action", ch_output_action, "NONE|ENABLE|DISABLE-GRACEFUL|DISABLE-IMMEDIATE")->required();
    chOutput->callback([&]() {
        hvb::OutputAction oa = hvb::OutputAction::None;
        if (ch_output_action=="ENABLE") oa=hvb::OutputAction::Enable;
        else if (ch_output_action=="DISABLE-GRACEFUL") oa=hvb::OutputAction::DisableGraceful;
        else if (ch_output_action=="DISABLE-IMMEDIATE") oa=hvb::OutputAction::DisableImmediate;
        std::cout << (g_client->sendOutputAction(ch,oa)?"OK\n":g_client->lastError()+"\n"); });

    auto* chFault = chCmd->add_subcommand("fault", "Fault command");
    chFault->add_option("cmd", ch_fault_cmd, "CLEAR-ACTIVE|CLEAR-HISTORY")->required();
    chFault->callback([&]() {
        hvb::ChannelFaultCommand fc = hvb::ChannelFaultCommand::None;
        if (ch_fault_cmd=="CLEAR-ACTIVE") fc=hvb::ChannelFaultCommand::ClearActiveFaultBlock;
        else if (ch_fault_cmd=="CLEAR-HISTORY") fc=hvb::ChannelFaultCommand::ClearFaultHistory;
        std::cout << (g_client->sendChannelFaultCommand(ch,fc)?"OK\n":g_client->lastError()+"\n"); });

    auto* chVoltage = chCmd->add_subcommand("voltage", "Set configured target voltage (raw LSB)");
    chVoltage->add_option("raw", ch_voltage_raw, "Raw LSB value")->required();
    chVoltage->callback([&]() { std::cout << (g_client->writeConfiguredTargetVoltage(ch,ch_voltage_raw)?"OK\n":g_client->lastError()+"\n"); });

    auto* chRampUp = chCmd->add_subcommand("ramp-up", "Set ramp up (raw step, interval x10s)");
    chRampUp->add_option("step", ch_ramp_up_step, "Raw LSB")->required();
    chRampUp->add_option("interval", ch_ramp_up_interval, "Interval x10s")->required();
    chRampUp->callback([&]() { std::cout << (g_client->writeRampUp(ch,ch_ramp_up_step,ch_ramp_up_interval)?"OK\n":g_client->lastError()+"\n"); });

    auto* chRampDn = chCmd->add_subcommand("ramp-down", "Set ramp down (raw step, interval x10s)");
    chRampDn->add_option("step", ch_ramp_dn_step, "Raw LSB")->required();
    chRampDn->add_option("interval", ch_ramp_dn_interval, "Interval x10s")->required();
    chRampDn->callback([&]() { std::cout << (g_client->writeRampDown(ch,ch_ramp_dn_step,ch_ramp_dn_interval)?"OK\n":g_client->lastError()+"\n"); });

    auto* chProtV = chCmd->add_subcommand("prot-v", "Set voltage protection");
    chProtV->add_option("mode", ch_prot_v_mode, "DISABLED|FLAG-ONLY|APPLY-ACTION")->required();
    chProtV->add_option("action", ch_prot_v_action, "NONE|DISABLE-GRACEFUL|DISABLE-IMMEDIATE|FORCE-ZERO|CLAMP")->required();
    chProtV->add_option("threshold", ch_prot_v_thresh, "Raw LSB")->required();
    chProtV->callback([&]() {
        hvb::ProtectionMode pm = hvb::ProtectionMode::Disabled;
        if (ch_prot_v_mode=="FLAG-ONLY") pm=hvb::ProtectionMode::FlagOnly;
        else if (ch_prot_v_mode=="APPLY-ACTION") pm=hvb::ProtectionMode::ApplyOutputAction;
        hvb::OutputAction oa = hvb::OutputAction::None;
        if (ch_prot_v_action=="DISABLE-GRACEFUL") oa=hvb::OutputAction::DisableGraceful;
        else if (ch_prot_v_action=="DISABLE-IMMEDIATE") oa=hvb::OutputAction::DisableImmediate;
        else if (ch_prot_v_action=="FORCE-ZERO") oa=hvb::OutputAction::ForceOutputZero;
        else if (ch_prot_v_action=="CLAMP") oa=hvb::OutputAction::Clamp;
        std::cout << (g_client->writeVoltageProtection(ch,pm,oa,ch_prot_v_thresh)?"OK\n":g_client->lastError()+"\n"); });

    auto* chProtI = chCmd->add_subcommand("prot-i", "Set current protection");
    chProtI->add_option("mode", ch_prot_i_mode, "DISABLED|FLAG-ONLY|APPLY-ACTION")->required();
    chProtI->add_option("action", ch_prot_i_action, "NONE|DISABLE-GRACEFUL|DISABLE-IMMEDIATE|FORCE-ZERO")->required();
    chProtI->add_option("threshold", ch_prot_i_thresh, "Raw LSB")->required();
    chProtI->callback([&]() {
        hvb::ProtectionMode pm = hvb::ProtectionMode::Disabled;
        if (ch_prot_i_mode=="FLAG-ONLY") pm=hvb::ProtectionMode::FlagOnly;
        else if (ch_prot_i_mode=="APPLY-ACTION") pm=hvb::ProtectionMode::ApplyOutputAction;
        hvb::OutputAction oa = hvb::OutputAction::None;
        if (ch_prot_i_action=="DISABLE-GRACEFUL") oa=hvb::OutputAction::DisableGraceful;
        else if (ch_prot_i_action=="DISABLE-IMMEDIATE") oa=hvb::OutputAction::DisableImmediate;
        else if (ch_prot_i_action=="FORCE-ZERO") oa=hvb::OutputAction::ForceOutputZero;
        std::cout << (g_client->writeCurrentProtection(ch,pm,oa,ch_prot_i_thresh)?"OK\n":g_client->lastError()+"\n"); });

    auto* chDerate = chCmd->add_subcommand("derate", "Set derate step (raw LSB)");
    chDerate->add_option("step", ch_derate_step, "Raw LSB")->required();
    chDerate->callback([&]() { std::cout << (g_client->writeDerateStep(ch,ch_derate_step)?"OK\n":g_client->lastError()+"\n"); });

    auto* chCalOut = chCmd->add_subcommand("cal-out", "Set output calibration");
    chCalOut->add_option("k", ch_cal_out_k)->required();
    chCalOut->add_option("b", ch_cal_out_b)->required();
    chCalOut->callback([&]() { std::cout << (g_client->writeCalibrationOutput(ch,ch_cal_out_k,ch_cal_out_b)?"OK\n":g_client->lastError()+"\n"); });

    auto* chCalMeasV = chCmd->add_subcommand("cal-meas-v", "Set meas V calibration");
    chCalMeasV->add_option("k", ch_cal_mv_k)->required();
    chCalMeasV->add_option("b", ch_cal_mv_b)->required();
    chCalMeasV->callback([&]() { std::cout << (g_client->writeCalibrationMeasV(ch,ch_cal_mv_k,ch_cal_mv_b)?"OK\n":g_client->lastError()+"\n"); });

    auto* chCalMeasI = chCmd->add_subcommand("cal-meas-i", "Set meas I calibration");
    chCalMeasI->add_option("k", ch_cal_mi_k)->required();
    chCalMeasI->add_option("b", ch_cal_mi_b)->required();
    chCalMeasI->callback([&]() { std::cout << (g_client->writeCalibrationMeasI(ch,ch_cal_mi_k,ch_cal_mi_b)?"OK\n":g_client->lastError()+"\n"); });

    chCmd->add_subcommand("save", "Save channel params")->callback([&]() { std::cout << (g_client->sendParamAction(ch,hvb::ParamAction::Save)?"OK\n":g_client->lastError()+"\n"); });
    chCmd->add_subcommand("load", "Load channel params")->callback([&]() { std::cout << (g_client->sendParamAction(ch,hvb::ParamAction::Load)?"OK\n":g_client->lastError()+"\n"); });
    chCmd->add_subcommand("factory", "Factory reset channel")->callback([&]() { std::cout << (g_client->sendParamAction(ch,hvb::ParamAction::FactoryReset)?"OK\n":g_client->lastError()+"\n"); });

    // Reset
    app.add_subcommand("reset", "Software reset")->callback([&]() { std::cout << (g_client->sendParamAction(-1,hvb::ParamAction::SoftwareReset)?"OK\n":g_client->lastError()+"\n"); });

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

    hvb::HvbModbusClient client;
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
