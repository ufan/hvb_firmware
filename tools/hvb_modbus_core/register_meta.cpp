#include "register_meta.h"
#include "register_map.h"
#include <sstream>
#include <iomanip>
#include <stdexcept>

namespace hvb::meta {

using Cat = hvb::reg::PollCat;

const std::vector<RegDesc> SYSTEM_INPUT = {
    {0,  "Protocol Major",        "uint16", "",        "Protocol version major", 1.0, false, false, -1, {}, Cat::Fixed},
    {1,  "Protocol Minor",        "uint16", "",        "Protocol version minor", 1.0, false, false, -1, {}, Cat::Fixed},
    {2,  "Variant ID",            "uint16", "enum",    "Board/product variant identifier", 1.0, false, false, -1, {}, Cat::Fixed},
    {3,  "System Capability Flags","uint16","bitmask", "System capability flags", 1.0, false, false, -1, {}, Cat::Fixed},
    {4,  "Supported Channel Count","uint16","count",   "Number of addressable channels", 1.0, false, false, -1, {}, Cat::Fixed},
    {5,  "Active Channel Mask",   "uint16", "bitmask", "Bit N = channel N addressable", 1.0, false, false, -1, {}, Cat::Realtime},
    {6,  "Board Temperature",     "uint16", "raw",     "Board temperature (raw LSB)", 1.0, false, false, -1, {}, Cat::Realtime},
    {7,  "Board Humidity",        "uint16", "raw",     "Board humidity (raw LSB)", 1.0, false, false, -1, {}, Cat::Realtime},
    {8,  "Uptime HI",             "uint16", "seconds", "Seconds since boot (high word)", 1.0, false, false, -1, {}, Cat::Realtime},
    {9,  "Uptime LO",             "uint16", "seconds", "Seconds since boot (low word)", 1.0, false, false, -1, {}, Cat::Realtime},
    {10, "Firmware Version HI",   "uint16", "packed",  "FW version encoding, high word", 1.0, false, false, -1, {}, Cat::Fixed},
    {11, "Firmware Version LO",   "uint16", "packed",  "FW version encoding, low word", 1.0, false, false, -1, {}, Cat::Fixed},
    {12, "Active Operating Mode", "uint16", "enum",    "Current domain operating mode", 1.0, false, false, -1,
        {"Normal", "Automatic", "Calibration"}, Cat::Realtime},
    {13, "System Status",         "uint16", "bitmask", "Global status flags", 1.0, false, false, -1, {}, Cat::Realtime},
    {14, "System Fault Cause",    "uint16", "bitmask", "Global fault summary", 1.0, false, false, -1, {}, Cat::Realtime},
};

const std::vector<RegDesc> SYSTEM_HOLDING = {
    {0,  "Operating Mode",           "uint16", "enum",    "Normal, Automatic, or Calibration", 1.0, true, false, -1,
        {"Normal", "Automatic", "Calibration"}, Cat::Config},
    {1,  "Startup Channel Policy",   "uint16", "enum",    "0=load NVS op-config, 1=factory reset op-config", 1.0, true, false, -1,
        {"LoadNVS", "FactoryDefault"}, Cat::Config},
    {2,  "Slave Address",            "uint16", "",        "Modbus slave address (0-247)", 1.0, true, false, -1, {}, Cat::Config},
    {3,  "Baud Rate Code",           "uint16", "enum",    "0=115200, 1=9600", 1.0, true, false, -1,
        {"115200", "9600"}, Cat::Config},
    /* 4..38 reserved */
    {39, "System Param Action",      "uint16", "enum",    "Save/Load/Factory/Reset", 1.0, true, true, -1,
        {"None", "Save", "Load", "FactoryReset"}, Cat::Command},
};

const std::vector<RegDesc> CHANNEL_INPUT = {
    {0,  "Channel Status Bits",    "uint16", "bitmask", "Channel status bits", 1.0, false, false, -1, {}, Cat::Realtime},
    {1,  "Active Fault Cause",     "uint16", "bitmask", "Fault bits blocking operation", 1.0, false, false, -1, {}, Cat::Realtime},
    {2,  "Fault History Cause",    "uint16", "bitmask", "Fault bits since last clear", 1.0, false, false, -1, {}, Cat::Realtime},
    {3,  "Last Prot Output Action","uint16", "enum",    "Last applied protection output action", 1.0, false, false, -1, {}, Cat::Realtime},
    {4,  "Auto Retry Count",       "uint16", "count",   "Retries in current window", 1.0, false, false, -1, {}, Cat::Realtime},
    {5,  "Auto Cooldown Remaining","uint16", "seconds", "Seconds until retry allowed", 1.0, false, false, -1, {}, Cat::Realtime},
    {6,  "Last Fault TS HI",       "uint16", "seconds", "Uptime of last fault event (high)", 1.0, false, false, -1, {}, Cat::Realtime},
    {7,  "Last Fault TS LO",       "uint16", "seconds", "Uptime of last fault event (low)", 1.0, false, false, -1, {}, Cat::Realtime},
    {8,  "Oper Target Voltage",    "uint16", "lsb",     "Current runtime target (raw LSB)", 1.0, false, false, -1, {}, Cat::Realtime},
    {9,  "Channel Capability Flags","uint16","bitmask", "Channel capability flags", 1.0, false, false, -1, {}, Cat::Fixed},
    {10, "Measured Voltage",       "uint16", "lsb",     "Calibrated output voltage (raw LSB)", 1.0, false, false, -1, {}, Cat::Realtime},
    {11, "Measured Current",       "uint16", "lsb",     "Calibrated output current (raw LSB)", 1.0, false, false, -1, {}, Cat::Realtime},
    {12, "Raw ADC Voltage HI",     "int32_hi","lsb",    "Calibration Mode — raw ADC voltage (high)", 1.0, false, false, -1, {}, Cat::Realtime},
    {13, "Raw ADC Voltage LO",     "int32_lo","lsb",    "Calibration Mode — raw ADC voltage (low)", 1.0, false, false, -1, {}, Cat::Realtime},
    {14, "Raw ADC Current HI",     "int32_hi","lsb",    "Calibration Mode — raw ADC current (high)", 1.0, false, false, -1, {}, Cat::Realtime},
    {15, "Raw ADC Current LO",     "int32_lo","lsb",    "Calibration Mode — raw ADC current (low)", 1.0, false, false, -1, {}, Cat::Realtime},
    /* 16..39 reserved — CH_CAL_SAMPLE_STATUS and CH_RAW_DAC_READBACK removed in v3 */
};

const std::vector<RegDesc> CHANNEL_HOLDING = {
    /* Commands */
    {0,  "Channel Output Action", "uint16", "enum",    "Host output action", 1.0, true, true, -1,
        {"None", "Enable", "DisableGraceful", "DisableImmediate"}, Cat::Command},
    {1,  "Channel Fault Command", "uint16", "enum",    "Fault clear command", 1.0, true, true, -1,
        {"None", "ClearActive", "ClearHistory"}, Cat::Command},
    {2,  "Channel Param Action",  "uint16", "enum",    "Save/Load/Factory/Reset per channel", 1.0, true, true, -1,
        {"None", "Save", "Load", "FactoryReset"}, Cat::Command},
    /* Operational config */
    {3,  "Configured Target V",   "uint16", "lsb",     "Host target voltage (raw LSB)", 1.0, true, false, -1, {}, Cat::Config},
    {4,  "Ramp Up Step",          "uint16", "lsb",     "Step size per ramp-up", 1.0, true, false, -1, {}, Cat::Config},
    {5,  "Ramp Up Interval",      "uint16", "seconds_x10","Delay per ramp-up step", 10.0, true, false, -1, {}, Cat::Config},
    {6,  "Ramp Down Step",        "uint16", "lsb",     "Step size per ramp-down", 1.0, true, false, -1, {}, Cat::Config},
    {7,  "Ramp Down Interval",    "uint16", "seconds_x10","Delay per ramp-down step", 10.0, true, false, -1, {}, Cat::Config},
    /* Recovery (moved from system in v3) */
    {8,  "Recovery Policy Mode",  "uint16", "enum",    "Per-channel recovery policy", 1.0, true, false, -1,
        {"ManualLatch", "AutoRetry", "AutoDerate", "NeverRetry"}, Cat::Config},
    {9,  "Auto Retry Delay",      "uint16", "seconds", "Cooldown before retry", 1.0, true, false, -1, {}, Cat::Config},
    {10, "Auto Retry Max Count",  "uint16", "count",   "Max retries in sliding window", 1.0, true, false, -1, {}, Cat::Config},
    {11, "Auto Retry Window",     "uint16", "seconds", "Sliding window for retry counting", 1.0, true, false, -1, {}, Cat::Config},
    {12, "Current Safe Band %",   "uint16", "%",       "I band below limit to trigger retry", 1.0, true, false, -1, {}, Cat::Config},
    /* Current protection */
    {13, "Current Protection Mode","uint16","enum",    "Current protection mode", 1.0, true, false, -1,
        {"Disabled", "FlagOnly", "ApplyAction"}, Cat::Config},
    {14, "I Protection Out Action","uint16","enum",    "Action applied on I fault", 1.0, true, false, -1,
        {"None", "DisableGraceful", "DisableImmediate", "ForceZero"}, Cat::Config},
    {15, "Current Limit Threshold","uint16","lsb",     "Current limit (raw LSB)", 1.0, true, false, -1, {}, Cat::Config},
    {16, "Auto Derate Step",      "uint16", "lsb",     "Target reduction per derate retry", 1.0, true, false, -1, {}, Cat::Config},
    /* 17..19 reserved — CH_SAVE_TARGET_POLICY removed in v3 */
    /* Cal config — readable any mode, writable in cal mode only */
    {20, "Output Calibration K",  "uint16", "x10000",  "Output path slope", 1.0, true, false, -1, {}, Cat::Config},
    {21, "Output Calibration B",  "int16",  "dac",     "Output path offset (DAC counts)", 1.0, true, false, -1, {}, Cat::Config},
    {22, "Meas V Calibration K",  "uint16", "x1000000","Voltage measurement slope (unity not representable)", 1.0, true, false, -1, {}, Cat::Config},
    {23, "Meas V Calibration B",  "int16",  "x100mV",  "Voltage measurement offset", 1.0, true, false, -1, {}, Cat::Config},
    {24, "Meas I Calibration K",  "uint16", "x1000000","Current measurement slope (unity not representable)", 1.0, true, false, -1, {}, Cat::Config},
    {25, "Meas I Calibration B",  "int16",  "x0.1nA",  "Current measurement offset", 1.0, true, false, -1, {}, Cat::Config},
    /* 26..29 reserved */
    /* Cal session commands — cal mode only */
    {30, "Cal Output Enable",     "uint16", "bool",    "Calibration Mode — raw output gate", 1.0, true, false, -1, {}, Cat::Command},
    {31, "Raw DAC Code",          "uint16", "lsb",     "Calibration Mode — native DAC code", 1.0, true, false, -1, {}, Cat::Command},
    {32, "Cal Sample Command",    "uint16", "enum",    "Calibration Mode — write 1 to capture ADC", 1.0, true, true, -1,
        {"None", "Execute"}, Cat::Command},
    {33, "Cal Commit Command",    "uint16", "enum",    "Calibration Mode — write 1 to persist cal coefficients", 1.0, true, true, -1,
        {"None", "Execute"}, Cat::Command},
    /* 34..39 reserved */
};

const RegDesc* findDesc(uint16_t absAddr, bool holding) {
    const auto& sysVec = holding ? SYSTEM_HOLDING : SYSTEM_INPUT;
    for (const auto& d : sysVec) {
        if (d.address == absAddr - hvb::reg::sysAddr(0)) {
            return &d;
        }
    }
    const auto& chVec = holding ? CHANNEL_HOLDING : CHANNEL_INPUT;
    for (const auto& d : chVec) {
        for (int ch = 0; ch < 4; ++ch) {
            if (d.address == absAddr - hvb::reg::chAddr(ch, 0)) {
                return &d;
            }
        }
    }
    return nullptr;
}

std::string formatValue(uint16_t raw, const RegDesc& d) {
    if (!d.enumLabels.empty()) {
        if (raw < d.enumLabels.size() && d.enumLabels[raw] && d.enumLabels[raw][0] != '\0') {
            return d.enumLabels[raw];
        }
    }
    std::ostringstream ss;
    ss << raw;
    return ss.str();
}

uint16_t parseEnum(const std::string& s, const RegDesc& d) {
    for (size_t i = 0; i < d.enumLabels.size(); ++i) {
        if (d.enumLabels[i] && s == d.enumLabels[i]) return static_cast<uint16_t>(i);
    }
    throw std::runtime_error("unknown enum value: " + s);
}

std::string formatRegisterCatalog() {
    std::ostringstream ss;
    ss << "=== System Input Registers (FC04, addr 0..14) ===\n";
    for (const auto& d : SYSTEM_INPUT) {
        ss << "  0x" << std::hex << std::setw(2) << std::setfill('0') << d.address
           << std::dec << "  " << d.name << " (" << d.type
           << (d.unit[0] ? ", " : "") << d.unit << ")\n";
    }
    ss << "\n=== System Holding Registers (FC03/06/10, addr 0..39) ===\n";
    for (const auto& d : SYSTEM_HOLDING) {
        ss << "  0x" << std::hex << std::setw(2) << std::setfill('0') << d.address
           << std::dec << "  " << d.name << " (" << d.type
           << (d.unit[0] ? ", " : "") << d.unit
           << (d.writable ? " [RW]" : " [R]")
           << (d.selfClearing ? " [CLR]" : "") << ")\n";
    }
    ss << "\n=== Channel Input Registers (FC04, per-channel offsets 0..15) ===\n";
    for (const auto& d : CHANNEL_INPUT) {
        ss << "  0x" << std::hex << std::setw(2) << std::setfill('0') << d.address
           << std::dec << "  " << d.name << " (" << d.type
           << (d.unit[0] ? ", " : "") << d.unit << ")\n";
    }
    ss << "\n=== Channel Holding Registers (FC03/06, per-channel offsets 0..33) ===\n";
    for (const auto& d : CHANNEL_HOLDING) {
        ss << "  0x" << std::hex << std::setw(2) << std::setfill('0') << d.address
           << std::dec << "  " << d.name << " (" << d.type
           << (d.unit[0] ? ", " : "") << d.unit
           << (d.writable ? " [RW]" : " [R]")
           << (d.selfClearing ? " [CLR]" : "") << ")\n";
    }
    return ss.str();
}

} // namespace hvb::meta
