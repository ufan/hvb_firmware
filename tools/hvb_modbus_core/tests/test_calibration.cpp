#include <catch2/catch_test_macros.hpp>
#include "hvb_modbus_client.h"
#include "register_map.h"
#include "types.h"
#include <cstring>

using namespace hvb;

static constexpr int MAX_ADDR = EXT_BLOCK_BASE + EXT_BLOCK_SIZE;

static void initBoard(uint16_t* input, uint16_t* holding) {
    memset(input, 0, MAX_ADDR * sizeof(uint16_t));
    memset(holding, 0, MAX_ADDR * sizeof(uint16_t));
    input[SYS_PROTOCOL_MAJOR] = 3;
    input[SYS_PROTOCOL_MINOR] = 0;
    input[SYS_CAPABILITY_FLAGS] = SYS_CAP_AUTOMATIC_MODE | SYS_CAP_ENV_SENSOR | SYS_CAP_CALIBRATION_MODE;
    input[SYS_SUPPORTED_CHANNELS] = 2;
    input[SYS_ACTIVE_CHANNEL_MASK] = 0x0003;
    for (int ch = 0; ch < 2; ++ch) {
        uint16_t base = reg::chAddr(ch, 0);
        input[base + CH_CAPABILITY_FLAGS] = CH_CAP_OUTPUT_ENABLE | CH_CAP_RAW_OUTPUT_DRIVE |
                                            CH_CAP_VOLTAGE_MEASUREMENT | CH_CAP_CURRENT_MEASUREMENT;
        holding[base + CH_OUTPUT_CAL_K] = 10000;
        holding[base + CH_MEASURED_V_CAL_K] = 10000;
        holding[base + CH_MEASURED_I_CAL_K] = 10000;
    }
}

TEST_CASE("Protocol v3 system info", "[calibration]") {
    uint16_t input[MAX_ADDR], holding[MAX_ADDR];
    initBoard(input, holding);

    HvbModbusClient client;
    client.attachTestArrays(input, holding, MAX_ADDR);

    auto info = client.readSystemInfo();
    REQUIRE(info.protoMajor == 3);
    REQUIRE(info.protoMinor == 0);
    REQUIRE((info.sysCapFlags & SysCap::CALIBRATION_MODE) != 0);

    client.detachTestArrays();
}

/* v3: CalibrationSampleStatus enum removed — CH_CAL_SAMPLE_STATUS register deleted */

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
    REQUIRE(holding[reg::chAddr(0, CH_CAL_DAC_CODE)] == 2048);

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
    /* v3: CH_CAL_SAMPLE_STATUS and CH_RAW_DAC_READBACK removed from FC04 input regs */
    holding[base_hold + CH_CAL_OUTPUT_ENABLE] = 1;
    holding[base_hold + CH_CAL_DAC_CODE] = 2048;

    HvbModbusClient client;
    client.attachTestArrays(input, holding, MAX_ADDR);

    auto snap = client.readCalibrationSnapshot(0);
    REQUIRE(snap.rawAdcVoltage == 0x00012345);
    REQUIRE(snap.rawAdcCurrent == -512);
    /* v3: sampleStatus removed from FC04 input regs; not available via Modbus */
    REQUIRE(snap.outputEnabled == true);
    REQUIRE(snap.rawDacCode == 2048);
    /* v3: rawDacReadback is the same FC03 register (CAL_DAC_CODE), no separate field */

    client.detachTestArrays();
}

TEST_CASE("readCalibrationSnapshot reads raw ADC input registers", "[calibration]") {
    uint16_t input[MAX_ADDR], holding[MAX_ADDR];
    initBoard(input, holding);

    uint16_t base = reg::chAddr(0, 0);
    input[base + CH_RAW_ADC_VOLTAGE_HI] = 0x0000;
    input[base + CH_RAW_ADC_VOLTAGE_LO] = 0x1234;
    input[base + CH_RAW_ADC_CURRENT_HI] = 0x0000;
    input[base + CH_RAW_ADC_CURRENT_LO] = 0x5678;
    /* v3: raw ADC reads belong in readCalibrationSnapshot, not readChannelInfo */

    HvbModbusClient client;
    client.attachTestArrays(input, holding, MAX_ADDR);

    auto snap = client.readCalibrationSnapshot(0);
    REQUIRE(snap.rawAdcVoltage == 0x1234);
    REQUIRE(snap.rawAdcCurrent == 0x5678);

    client.detachTestArrays();
}

TEST_CASE("readCalibrationSnapshot includes cal session fields", "[calibration]") {
    uint16_t input[MAX_ADDR], holding[MAX_ADDR];
    initBoard(input, holding);

    uint16_t base = reg::chAddr(0, 0);
    holding[base + CH_CAL_OUTPUT_ENABLE] = 1;
    holding[base + CH_CAL_DAC_CODE] = 3000;

    HvbModbusClient client;
    client.attachTestArrays(input, holding, MAX_ADDR);

    auto snap = client.readCalibrationSnapshot(0);
    REQUIRE(snap.outputEnabled == true);
    REQUIRE(snap.rawDacCode == 3000);

    client.detachTestArrays();
}

TEST_CASE("readChannelCalConfig reads cal coefficients", "[calibration]") {
    uint16_t input[MAX_ADDR], holding[MAX_ADDR];
    initBoard(input, holding);

    uint16_t base = reg::chAddr(0, 0);
    holding[base + CH_OUTPUT_CAL_K] = 9900;
    holding[base + CH_OUTPUT_CAL_B] = static_cast<uint16_t>(static_cast<int16_t>(-5));
    holding[base + CH_MEASURED_V_CAL_K] = 10050;
    holding[base + CH_MEASURED_V_CAL_B] = 3;
    holding[base + CH_MEASURED_I_CAL_K] = 9980;
    holding[base + CH_MEASURED_I_CAL_B] = static_cast<uint16_t>(static_cast<int16_t>(-2));

    HvbModbusClient client;
    client.attachTestArrays(input, holding, MAX_ADDR);

    auto cal = client.readChannelCalConfig(0);
    REQUIRE(cal.outCalK == 9900);
    REQUIRE(cal.outCalB == -5);
    REQUIRE(cal.measVCalK == 10050);
    REQUIRE(cal.measVCalB == 3);
    REQUIRE(cal.measICalK == 9980);
    REQUIRE(cal.measICalB == -2);

    client.detachTestArrays();
}

TEST_CASE("exitCalibrationMode writes EXT_CAL_EXIT", "[calibration]") {
    uint16_t input[MAX_ADDR], holding[MAX_ADDR];
    initBoard(input, holding);

    HvbModbusClient client;
    client.attachTestArrays(input, holding, MAX_ADDR);

    REQUIRE(client.exitCalibrationMode());
    REQUIRE(holding[reg::extAddr(EXT_CAL_EXIT)] == 1);

    client.detachTestArrays();
}

