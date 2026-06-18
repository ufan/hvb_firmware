#include <catch2/catch_test_macros.hpp>
#include "hvb_modbus_client.h"
#include "register_map.h"
#include "types.h"
#include <cstring>

using namespace hvb;

static constexpr int MAX_ADDR = 280;

static void initBoard(uint16_t* input, uint16_t* holding) {
    memset(input, 0, MAX_ADDR * sizeof(uint16_t));
    memset(holding, 0, MAX_ADDR * sizeof(uint16_t));
    input[SYS_PROTOCOL_MAJOR] = 2;
    input[SYS_PROTOCOL_MINOR] = 1;
    input[SYS_CAPABILITY_FLAGS] = 0x0007;
    input[SYS_SUPPORTED_CHANNELS] = 2;
    input[SYS_ACTIVE_CHANNEL_MASK] = 0x0003;
    for (int ch = 0; ch < 2; ++ch) {
        uint16_t base = reg::chAddr(ch, 0);
        holding[base + CH_OUTPUT_CAL_K] = 10000;
        holding[base + CH_MEASURED_V_CAL_K] = 10000;
        holding[base + CH_MEASURED_I_CAL_K] = 10000;
        holding[base + CH_CAL_MAX_RAW_DAC_LIMIT] = 4095;
    }
}

TEST_CASE("Protocol v2.1 system info", "[calibration]") {
    uint16_t input[MAX_ADDR], holding[MAX_ADDR];
    initBoard(input, holding);

    HvbModbusClient client;
    client.attachTestArrays(input, holding, MAX_ADDR);

    auto info = client.readSystemInfo();
    REQUIRE(info.protoMajor == 2);
    REQUIRE(info.protoMinor == 1);
    REQUIRE((info.sysCapFlags & SysCap::CALIBRATION_MODE) != 0);

    client.detachTestArrays();
}

TEST_CASE("CalibrationSampleStatus enum names", "[calibration]") {
    REQUIRE(std::string(calSampleStatusName(CalibrationSampleStatus::NoSample)) == "NoSample");
    REQUIRE(std::string(calSampleStatusName(CalibrationSampleStatus::Valid)) == "Valid");
    REQUIRE(std::string(calSampleStatusName(CalibrationSampleStatus::Busy)) == "Busy");
    REQUIRE(std::string(calSampleStatusName(CalibrationSampleStatus::Error)) == "Error");
}

TEST_CASE("OpMode Calibration name", "[calibration]") {
    REQUIRE(std::string(opModeName(OpMode::Calibration)) == "Calibration");
}

TEST_CASE("Raw ADC signed 32-bit decode", "[calibration]") {
    REQUIRE(reg::int32FromRegs(0x0000, 0x0001) == 1);
    REQUIRE(reg::int32FromRegs(0xFFFF, 0xFFFF) == -1);
    REQUIRE(reg::int32FromRegs(0x7FFF, 0xFFFF) == 2147483647);
    REQUIRE(reg::int32FromRegs(0x8000, 0x0000) == -2147483648);
}

TEST_CASE("Calibration unlock write", "[calibration]") {
    uint16_t input[MAX_ADDR], holding[MAX_ADDR];
    initBoard(input, holding);

    HvbModbusClient client;
    client.attachTestArrays(input, holding, MAX_ADDR);

    REQUIRE(client.unlockCalibrationStep(CAL_UNLOCK_STEP1));
    REQUIRE(holding[reg::extAddr(EXT_CAL_UNLOCK)] == 0xCA1B);

    REQUIRE(client.unlockCalibrationStep(CAL_UNLOCK_STEP2));
    REQUIRE(holding[reg::extAddr(EXT_CAL_UNLOCK)] == 0xA11B);

    client.detachTestArrays();
}

TEST_CASE("Calibration output enable and raw DAC code", "[calibration]") {
    uint16_t input[MAX_ADDR], holding[MAX_ADDR];
    initBoard(input, holding);

    HvbModbusClient client;
    client.attachTestArrays(input, holding, MAX_ADDR);

    REQUIRE(client.writeCalibrationOutputEnable(0, true));
    REQUIRE(holding[reg::chAddr(0, CH_CAL_OUTPUT_ENABLE)] == 1);

    REQUIRE(client.writeRawDacCode(0, 2048));
    REQUIRE(holding[reg::chAddr(0, CH_RAW_DAC_CODE)] == 2048);

    REQUIRE(client.writeCalibrationOutputEnable(0, false));
    REQUIRE(holding[reg::chAddr(0, CH_CAL_OUTPUT_ENABLE)] == 0);

    client.detachTestArrays();
}

TEST_CASE("Calibration sample and commit commands", "[calibration]") {
    uint16_t input[MAX_ADDR], holding[MAX_ADDR];
    initBoard(input, holding);

    HvbModbusClient client;
    client.attachTestArrays(input, holding, MAX_ADDR);

    REQUIRE(client.sendCalibrationSampleCommand(0));
    REQUIRE(holding[reg::chAddr(0, CH_CAL_SAMPLE_CMD)] == CAL_COMMAND_EXECUTE);

    REQUIRE(client.sendCalibrationCommitCommand(1));
    REQUIRE(holding[reg::chAddr(1, CH_CAL_COMMIT_CMD)] == CAL_COMMAND_EXECUTE);

    client.detachTestArrays();
}

TEST_CASE("CalibrationSnapshot round-trip", "[calibration]") {
    uint16_t input[MAX_ADDR], holding[MAX_ADDR];
    initBoard(input, holding);

    uint16_t base_in = reg::chAddr(0, 0);
    uint16_t base_hold = reg::chAddr(0, 0);
    input[base_in + CH_RAW_ADC_VOLTAGE_HI] = 0x0001;
    input[base_in + CH_RAW_ADC_VOLTAGE_LO] = 0x2345;
    input[base_in + CH_RAW_ADC_CURRENT_HI] = 0xFFFF;
    input[base_in + CH_RAW_ADC_CURRENT_LO] = 0xFE00;
    input[base_in + CH_CAL_SAMPLE_STATUS] = 1;
    input[base_in + CH_RAW_DAC_READBACK] = 2048;
    holding[base_hold + CH_CAL_OUTPUT_ENABLE] = 1;
    holding[base_hold + CH_RAW_DAC_CODE] = 2048;
    holding[base_hold + CH_CAL_MAX_RAW_DAC_LIMIT] = 3000;

    HvbModbusClient client;
    client.attachTestArrays(input, holding, MAX_ADDR);

    auto snap = client.readCalibrationSnapshot(0);
    REQUIRE(snap.rawAdcVoltage == 0x00012345);
    REQUIRE(snap.rawAdcCurrent == -512);
    REQUIRE(snap.sampleStatus == CalibrationSampleStatus::Valid);
    REQUIRE(snap.rawDacReadback == 2048);
    REQUIRE(snap.outputEnabled == true);
    REQUIRE(snap.rawDacCode == 2048);
    REQUIRE(snap.maxRawDacLimit == 3000);

    client.detachTestArrays();
}

TEST_CASE("readChannelInfo includes v2.1 calibration fields", "[calibration]") {
    uint16_t input[MAX_ADDR], holding[MAX_ADDR];
    initBoard(input, holding);

    uint16_t base = reg::chAddr(0, 0);
    input[base + CH_RAW_ADC_VOLTAGE_HI] = 0x0000;
    input[base + CH_RAW_ADC_VOLTAGE_LO] = 0x1234;
    input[base + CH_RAW_ADC_CURRENT_HI] = 0x0000;
    input[base + CH_RAW_ADC_CURRENT_LO] = 0x5678;
    input[base + CH_CAL_SAMPLE_STATUS] = 2;
    input[base + CH_RAW_DAC_READBACK] = 1024;

    HvbModbusClient client;
    client.attachTestArrays(input, holding, MAX_ADDR);

    auto info = client.readChannelInfo(0);
    REQUIRE(info.rawAdcVoltage == 0x1234);
    REQUIRE(info.rawAdcCurrent == 0x5678);
    REQUIRE(info.sampleStatus == CalibrationSampleStatus::Busy);
    REQUIRE(info.rawDacReadback == 1024);

    client.detachTestArrays();
}

TEST_CASE("readChannelConfig includes v2.1 calibration fields", "[calibration]") {
    uint16_t input[MAX_ADDR], holding[MAX_ADDR];
    initBoard(input, holding);

    uint16_t base = reg::chAddr(0, 0);
    holding[base + CH_CAL_OUTPUT_ENABLE] = 1;
    holding[base + CH_RAW_DAC_CODE] = 3000;
    holding[base + CH_CAL_MAX_RAW_DAC_LIMIT] = 3500;

    HvbModbusClient client;
    client.attachTestArrays(input, holding, MAX_ADDR);

    auto cfg = client.readChannelConfig(0);
    REQUIRE(cfg.calOutputEnabled == true);
    REQUIRE(cfg.rawDacCode == 3000);
    REQUIRE(cfg.maxRawDacLimit == 3500);

    client.detachTestArrays();
}

TEST_CASE("exitCalibrationMode rejects Calibration target", "[calibration]") {
    uint16_t input[MAX_ADDR], holding[MAX_ADDR];
    initBoard(input, holding);

    HvbModbusClient client;
    client.attachTestArrays(input, holding, MAX_ADDR);

    REQUIRE_FALSE(client.exitCalibrationMode(OpMode::Calibration));
    REQUIRE(client.exitCalibrationMode(OpMode::Normal));
    REQUIRE(holding[reg::sysAddr(SYS_OPERATING_MODE)] == 0);

    client.detachTestArrays();
}

TEST_CASE("writeCalibrationMaxDacLimit", "[calibration]") {
    uint16_t input[MAX_ADDR], holding[MAX_ADDR];
    initBoard(input, holding);

    HvbModbusClient client;
    client.attachTestArrays(input, holding, MAX_ADDR);

    REQUIRE(client.writeCalibrationMaxDacLimit(1, 2000));
    REQUIRE(holding[reg::chAddr(1, CH_CAL_MAX_RAW_DAC_LIMIT)] == 2000);

    client.detachTestArrays();
}
