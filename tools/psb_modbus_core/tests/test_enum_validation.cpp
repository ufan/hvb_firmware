#include "psb_modbus_client.h"
#include "types.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("Validation — Enable rejected in protection context", "[validation]") {
    uint16_t inputRegs[280] = {};
    uint16_t holdingRegs[280] = {};

    psb::PsbModbusClient client;
    client.attachTestArrays(inputRegs, holdingRegs, 280);

    REQUIRE_FALSE(client.writeCurrentProtection(0, psb::ProtectionMode::ApplyOutputAction,
        psb::OutputAction::Enable, 1000));
    CHECK(client.lastError().find("invalid") != std::string::npos);
}

TEST_CASE("Validation — ForceZero rejected in host context", "[validation]") {
    uint16_t inputRegs[280] = {};
    uint16_t holdingRegs[280] = {};

    psb::PsbModbusClient client;
    client.attachTestArrays(inputRegs, holdingRegs, 280);

    REQUIRE_FALSE(client.sendOutputAction(0, psb::OutputAction::ForceOutputZero));
    CHECK(client.lastError().find("invalid") != std::string::npos);
}

TEST_CASE("Validation — ForceZero accepted in protection context", "[validation]") {
    uint16_t inputRegs[280] = {};
    uint16_t holdingRegs[280] = {};

    psb::PsbModbusClient client;
    client.attachTestArrays(inputRegs, holdingRegs, 280);

    CHECK(client.writeCurrentProtection(0, psb::ProtectionMode::ApplyOutputAction,
        psb::OutputAction::ForceOutputZero, 1000));
}

TEST_CASE("Validation — valid output actions pass", "[validation]") {
    uint16_t inputRegs[280] = {};
    uint16_t holdingRegs[280] = {};

    psb::PsbModbusClient client;
    client.attachTestArrays(inputRegs, holdingRegs, 280);

    CHECK(client.sendOutputAction(0, psb::OutputAction::Enable));
    CHECK(client.sendOutputAction(0, psb::OutputAction::DisableGraceful));
    CHECK(client.sendOutputAction(0, psb::OutputAction::DisableImmediate));
    CHECK(client.sendOutputAction(0, psb::OutputAction::None));

    CHECK(client.writeCurrentProtection(0, psb::ProtectionMode::ApplyOutputAction,
        psb::OutputAction::ForceOutputZero, 5000));
}
