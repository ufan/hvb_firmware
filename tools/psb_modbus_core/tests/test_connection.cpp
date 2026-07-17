#include "psb_modbus_client.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("Connection — scanPorts returns only USB serial (ttyUSB/ttyACM)", "[connection]") {
    auto ports = psb::PsbModbusClient::scanPorts();
    for (const auto& p : ports) {
        INFO("Port: " << p);
        CHECK((p.rfind("/dev/ttyUSB", 0) == 0 || p.rfind("/dev/ttyACM", 0) == 0));
    }
}

TEST_CASE("Connection — attach/detach test arrays", "[connection]") {
    psb::PsbModbusClient client;
    uint16_t inputRegs[280] = {};
    uint16_t holdingRegs[280] = {};

    client.attachTestArrays(inputRegs, holdingRegs, 280);
    REQUIRE(client.isConnected());

    client.detachTestArrays();
    REQUIRE_FALSE(client.isConnected());
}
