#include "hvb_modbus_client.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("Error — read fails when not connected", "[errors]") {
    hvb::HvbModbusClient client;
    client.detachTestArrays();

    auto info = client.readSystemInfo();
    CHECK_FALSE(client.isConnected());
    CHECK(info.protoMajor == 0);
}

TEST_CASE("Error — read fails after detach", "[errors]") {
    uint16_t inputRegs[280] = {};
    uint16_t holdingRegs[280] = {};
    inputRegs[0] = 2;

    hvb::HvbModbusClient client;
    client.attachTestArrays(inputRegs, holdingRegs, 280);

    auto info1 = client.readSystemInfo();
    CHECK(info1.protoMajor == 2);

    client.detachTestArrays();
    auto info2 = client.readSystemInfo();
    CHECK(info2.protoMajor == 0);
}

TEST_CASE("Error — write to out-of-range address", "[errors]") {
    uint16_t inputRegs[280] = {};
    uint16_t holdingRegs[280] = {};

    hvb::HvbModbusClient client;
    client.attachTestArrays(inputRegs, holdingRegs, 280);

    REQUIRE_FALSE(client.readInputRegs(300, 1, nullptr));
    CHECK(client.lastError().find("out of range") != std::string::npos);
}
