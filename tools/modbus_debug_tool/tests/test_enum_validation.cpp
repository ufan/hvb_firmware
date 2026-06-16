#include "hvb_modbus_client.h"
#include "types.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("Validation — Enable rejected in protection context", "[validation]") {
    uint16_t inputRegs[280] = {};
    uint16_t holdingRegs[280] = {};

    hvb::HvbModbusClient client;
    client.attachTestArrays(inputRegs, holdingRegs, 280);

    REQUIRE_FALSE(client.writeVoltageProtection(0, hvb::ProtectionMode::ApplyOutputAction,
        hvb::OutputAction::Enable, 1000));
    CHECK(client.lastError().find("invalid") != std::string::npos);
}

TEST_CASE("Validation — Clamp rejected in host context", "[validation]") {
    uint16_t inputRegs[280] = {};
    uint16_t holdingRegs[280] = {};

    hvb::HvbModbusClient client;
    client.attachTestArrays(inputRegs, holdingRegs, 280);

    REQUIRE_FALSE(client.sendOutputAction(0, hvb::OutputAction::Clamp));
    CHECK(client.lastError().find("invalid") != std::string::npos);
}

TEST_CASE("Validation — Clamp rejected in current prot context", "[validation]") {
    uint16_t inputRegs[280] = {};
    uint16_t holdingRegs[280] = {};

    hvb::HvbModbusClient client;
    client.attachTestArrays(inputRegs, holdingRegs, 280);

    REQUIRE_FALSE(client.writeCurrentProtection(0, hvb::ProtectionMode::ApplyOutputAction,
        hvb::OutputAction::Clamp, 1000));
    CHECK(client.lastError().find("invalid") != std::string::npos);
}

TEST_CASE("Validation — valid output actions pass", "[validation]") {
    uint16_t inputRegs[280] = {};
    uint16_t holdingRegs[280] = {};

    hvb::HvbModbusClient client;
    client.attachTestArrays(inputRegs, holdingRegs, 280);

    CHECK(client.sendOutputAction(0, hvb::OutputAction::Enable));
    CHECK(client.sendOutputAction(0, hvb::OutputAction::DisableGraceful));
    CHECK(client.sendOutputAction(0, hvb::OutputAction::DisableImmediate));
    CHECK(client.sendOutputAction(0, hvb::OutputAction::None));

    CHECK(client.writeVoltageProtection(0, hvb::ProtectionMode::ApplyOutputAction,
        hvb::OutputAction::Clamp, 2000));
    CHECK(client.writeCurrentProtection(0, hvb::ProtectionMode::ApplyOutputAction,
        hvb::OutputAction::ForceOutputZero, 5000));
}
