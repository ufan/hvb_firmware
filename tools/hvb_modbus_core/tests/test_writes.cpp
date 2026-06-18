#include "hvb_modbus_client.h"
#include "types.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("Write — target voltage", "[writes]") {
    uint16_t inputRegs[280] = {};
    uint16_t holdingRegs[280] = {};

    hvb::HvbModbusClient client;
    client.attachTestArrays(inputRegs, holdingRegs, 280);

    REQUIRE(client.writeConfiguredTargetVoltage(0, 5000));
    auto cfg = client.readChannelConfig(0);
    CHECK(cfg.configuredTargetVRaw == 5000);
}

TEST_CASE("Write — output action write succeeds", "[writes]") {
    uint16_t inputRegs[280] = {};
    uint16_t holdingRegs[280] = {};

    hvb::HvbModbusClient client;
    client.attachTestArrays(inputRegs, holdingRegs, 280);

    REQUIRE(client.writeConfiguredTargetVoltage(0, 5000));
    REQUIRE(client.sendOutputAction(0, hvb::OutputAction::Enable));

    // The write succeeded (client API returns true)
    // Self-clearing is real-board behavior, not simulated in test arrays
    CHECK(client.isConnected());
}

TEST_CASE("Write — fault command clear active", "[writes]") {
    uint16_t inputRegs[280] = {};
    uint16_t holdingRegs[280] = {};

    hvb::HvbModbusClient client;
    client.attachTestArrays(inputRegs, holdingRegs, 280);

    REQUIRE(client.sendChannelFaultCommand(0, hvb::ChannelFaultCommand::ClearActiveFaultBlock));
}

TEST_CASE("Write — calibration", "[writes]") {
    uint16_t inputRegs[280] = {};
    uint16_t holdingRegs[280] = {};

    hvb::HvbModbusClient client;
    client.attachTestArrays(inputRegs, holdingRegs, 280);

    REQUIRE(client.writeCalibrationOutput(0, 10123, -5));
    REQUIRE(client.writeCalibrationMeasV(1, 9999, 2));

    auto cfg0 = client.readChannelConfig(0);
    CHECK(cfg0.outCalK == 10123);
    CHECK(cfg0.outCalB == -5);

    auto cfg1 = client.readChannelConfig(1);
    CHECK(cfg1.measVCalK == 9999);
    CHECK(cfg1.measVCalB == 2);
}

TEST_CASE("Write — system recovery", "[writes]") {
    uint16_t inputRegs[280] = {};
    uint16_t holdingRegs[280] = {};

    hvb::HvbModbusClient client;
    client.attachTestArrays(inputRegs, holdingRegs, 280);

    REQUIRE(client.writeSystemRecoveryPolicy(hvb::RecoveryPolicy::AutoRetry, 30, 5, 600));

    auto cfg = client.readSystemConfig();
    CHECK(cfg.recoveryPolicy == hvb::RecoveryPolicy::AutoRetry);
    CHECK(cfg.retryDelay == 30);
    CHECK(cfg.retryMax == 5);
    CHECK(cfg.retryWindow == 600);
}

TEST_CASE("Write — safe bands", "[writes]") {
    uint16_t inputRegs[280] = {};
    uint16_t holdingRegs[280] = {};

    hvb::HvbModbusClient client;
    client.attachTestArrays(inputRegs, holdingRegs, 280);

    REQUIRE(client.writeSafeBands(25, 15));
    auto cfg = client.readSystemConfig();
    CHECK(cfg.voltageSafeBandPct == 25);
    CHECK(cfg.currentSafeBandPct == 15);
}

TEST_CASE("Write — read-back consistency", "[writes]") {
    uint16_t inputRegs[280] = {};
    uint16_t holdingRegs[280] = {};

    hvb::HvbModbusClient client;
    client.attachTestArrays(inputRegs, holdingRegs, 280);

    // Write then read back — verify round-trip
    REQUIRE(client.writeConfiguredTargetVoltage(0, 3000));
    REQUIRE(client.writeRampUp(0, 10, 5));
    REQUIRE(client.writeRampDown(0, 20, 3));
    REQUIRE(client.writeDerateStep(0, 50));
    REQUIRE(client.writeSaveTargetPolicy(0, true));

    auto cfg = client.readChannelConfig(0);
    CHECK(cfg.configuredTargetVRaw == 3000);
    CHECK(cfg.rampUpStepRaw == 10);
    CHECK(cfg.rampUpInterval == 5);
    CHECK(cfg.rampDownStepRaw == 20);
    CHECK(cfg.rampDownInterval == 3);
    CHECK(cfg.derateStepRaw == 50);
    CHECK(cfg.saveTargetPolicy == true);
}
