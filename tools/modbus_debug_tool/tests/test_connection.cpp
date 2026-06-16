#include "hvb_modbus_client.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("Connection — scanPorts returns only USB", "[connection]") {
    auto ports = hvb::HvbModbusClient::scanPorts();
    for (const auto& p : ports) {
        INFO("Port: " << p);
        CHECK(p.find("/dev/ttyUSB") == 0);
    }
}

TEST_CASE("Connection — attach/detach test arrays", "[connection]") {
    hvb::HvbModbusClient client;
    uint16_t inputRegs[280] = {};
    uint16_t holdingRegs[280] = {};

    client.attachTestArrays(inputRegs, holdingRegs, 280);
    REQUIRE(client.isConnected());

    client.detachTestArrays();
    REQUIRE_FALSE(client.isConnected());
}
