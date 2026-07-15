#include "psb_modbus_client.h"
#include "types.h"
#include "register_map.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("Write — target voltage", "[writes]") {
    uint16_t inputRegs[280] = {};
    uint16_t holdingRegs[280] = {};
    inputRegs[psb::reg::chAddr(0, CH_CAPABILITY_FLAGS)] = CH_CAP_OUTPUT_ENABLE | CH_CAP_RAW_OUTPUT_DRIVE;

    psb::PsbModbusClient client;
    client.attachTestArrays(inputRegs, holdingRegs, 280);

    REQUIRE(client.writeConfiguredTargetVoltage(0, 5000));
    auto cfg = client.readChannelConfig(0);
    CHECK(cfg.configuredTargetVRaw == 5000);
}

TEST_CASE("Write — output action write succeeds", "[writes]") {
    uint16_t inputRegs[280] = {};
    uint16_t holdingRegs[280] = {};

    psb::PsbModbusClient client;
    client.attachTestArrays(inputRegs, holdingRegs, 280);

    REQUIRE(client.writeConfiguredTargetVoltage(0, 5000));
    REQUIRE(client.sendOutputAction(0, psb::OutputAction::Enable));

    // The write succeeded (client API returns true)
    // Self-clearing is real-board behavior, not simulated in test arrays
    CHECK(client.isConnected());
}

TEST_CASE("Write — fault command clear active", "[writes]") {
    uint16_t inputRegs[280] = {};
    uint16_t holdingRegs[280] = {};

    psb::PsbModbusClient client;
    client.attachTestArrays(inputRegs, holdingRegs, 280);

    REQUIRE(client.sendChannelFaultCommand(0, psb::ChannelFaultCommand::ClearActiveFaultBlock));
}

TEST_CASE("Write — calibration", "[writes]") {
    uint16_t inputRegs[280] = {};
    uint16_t holdingRegs[280] = {};
    inputRegs[psb::reg::chAddr(0, CH_CAPABILITY_FLAGS)] = CH_CAP_RAW_OUTPUT_DRIVE;
    inputRegs[psb::reg::chAddr(1, CH_CAPABILITY_FLAGS)] = CH_CAP_VOLTAGE_MEASUREMENT;

    psb::PsbModbusClient client;
    client.attachTestArrays(inputRegs, holdingRegs, 280);

    REQUIRE(client.writeCalibrationOutput(0, 10123, -5));
    REQUIRE(client.writeCalibrationMeasV(1, 9999, 2));

    auto cal0 = client.readChannelCalConfig(0);
    CHECK(cal0.outCalK == 10123);
    CHECK(cal0.outCalB == -5);

    auto cal1 = client.readChannelCalConfig(1);
    CHECK(cal1.measVCalK == 9999);
    CHECK(cal1.measVCalB == 2);
}

TEST_CASE("Write — channel recovery", "[writes]") {
    uint16_t inputRegs[280] = {};
    uint16_t holdingRegs[280] = {};

    psb::PsbModbusClient client;
    client.attachTestArrays(inputRegs, holdingRegs, 280);

    REQUIRE(client.writeChannelRecovery(0, psb::RecoveryPolicy::AutoRetry, 30, 5, 600));

    auto cfg = client.readChannelConfig(0);
    CHECK(cfg.recoveryPolicyMode == psb::RecoveryPolicy::AutoRetry);
    CHECK(cfg.autoRetryDelay == 30);
    CHECK(cfg.autoRetryMaxCount == 5);
    CHECK(cfg.autoRetryWindow == 600);
}

TEST_CASE("Write — channel safe band", "[writes]") {
    uint16_t inputRegs[280] = {};
    uint16_t holdingRegs[280] = {};

    psb::PsbModbusClient client;
    client.attachTestArrays(inputRegs, holdingRegs, 280);

    REQUIRE(client.writeChannelSafeBand(0, 25));
    auto cfg = client.readChannelConfig(0);
    CHECK(cfg.currentSafeBandPct == 25);
}

TEST_CASE("Write — read-back consistency", "[writes]") {
    uint16_t inputRegs[280] = {};
    uint16_t holdingRegs[280] = {};
    inputRegs[psb::reg::chAddr(0, CH_CAPABILITY_FLAGS)] = CH_CAP_RAW_OUTPUT_DRIVE | CH_CAP_VOLTAGE_MEASUREMENT;

    psb::PsbModbusClient client;
    client.attachTestArrays(inputRegs, holdingRegs, 280);

    // Write then read back — verify round-trip
    REQUIRE(client.writeConfiguredTargetVoltage(0, 3000));
    REQUIRE(client.writeRampUp(0, 10, 5));
    REQUIRE(client.writeRampDown(0, 20, 3));
    REQUIRE(client.writeDerateStep(0, 50));

    auto cfg = client.readChannelConfig(0);
    CHECK(cfg.configuredTargetVRaw == 3000);
    CHECK(cfg.rampUpStepRaw == 10);
    CHECK(cfg.rampUpInterval == 5);
    CHECK(cfg.rampDownStepRaw == 20);
    CHECK(cfg.rampDownInterval == 3);
    CHECK(cfg.derateStepRaw == 50);
}

TEST_CASE("Write — parameter actions preserve system and channel scope", "[writes]") {
    uint16_t inputRegs[280] = {};
    uint16_t holdingRegs[280] = {};

    psb::PsbModbusClient client;
    client.attachTestArrays(inputRegs, holdingRegs, 280);

    REQUIRE(client.sendParamAction(-1, psb::ParamAction::Save));
    CHECK(holdingRegs[psb::reg::sysAddr(SYS_PARAM_ACTION)] ==
          static_cast<uint16_t>(psb::ParamAction::Save));

    REQUIRE(client.sendParamAction(1, psb::ParamAction::Save));
    CHECK(holdingRegs[psb::reg::chAddr(1, CH_PARAM_ACTION)] ==
          static_cast<uint16_t>(psb::ParamAction::Save));
}
