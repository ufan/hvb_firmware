#include "psb_serial_bus.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("PsbSerialBus — scanPorts returns only USB serial (ttyUSB/ttyACM)", "[serial_bus]") {
    auto ports = psb::PsbSerialBus::scanPorts();
    for (const auto& p : ports) {
        INFO("Port: " << p);
        CHECK((p.rfind("/dev/ttyUSB", 0) == 0 || p.rfind("/dev/ttyACM", 0) == 0));
    }
}

TEST_CASE("PsbSerialBus — read fails when not connected and no test arrays", "[serial_bus]") {
    psb::PsbSerialBus bus;
    uint16_t out[2] = {};
    CHECK_FALSE(bus.readInputRegs(1, 0, 2, out));
    CHECK(bus.lastError() == "not connected");
    CHECK_FALSE(bus.isConnected());
}

TEST_CASE("PsbSerialBus — two slave IDs on one bus read/write independently, no cross-contamination", "[serial_bus]") {
    psb::PsbSerialBus bus;
    uint16_t inputA[8] = {}, holdingA[8] = {};
    uint16_t inputB[8] = {}, holdingB[8] = {};
    inputA[0] = 111;
    inputB[0] = 222;

    bus.attachTestArrays(1, inputA, holdingA, 8);
    bus.attachTestArrays(2, inputB, holdingB, 8);
    REQUIRE(bus.isConnected());

    uint16_t outA = 0, outB = 0;
    REQUIRE(bus.readInputRegs(1, 0, 1, &outA));
    REQUIRE(bus.readInputRegs(2, 0, 1, &outB));
    CHECK(outA == 111);
    CHECK(outB == 222);

    uint16_t writeVal = 42;
    REQUIRE(bus.writeRegs(1, 0, 1, &writeVal));
    CHECK(holdingA[0] == 42);
    CHECK(holdingB[0] == 0);  // slave 2's holding regs untouched by slave 1's write

    bus.detachTestArrays(1);
    uint16_t outAfterDetach = 0;
    CHECK_FALSE(bus.readInputRegs(1, 0, 1, &outAfterDetach));  // no port connected, no test arrays for slave 1 anymore
    REQUIRE(bus.readInputRegs(2, 0, 1, &outB));  // slave 2 still works
}

TEST_CASE("PsbSerialBus — out-of-range test address fails clearly", "[serial_bus]") {
    psb::PsbSerialBus bus;
    uint16_t inputRegs[8] = {}, holdingRegs[8] = {};
    bus.attachTestArrays(1, inputRegs, holdingRegs, 8);

    uint16_t out[1] = {};
    REQUIRE_FALSE(bus.readInputRegs(1, 100, 1, out));
    CHECK(bus.lastError().find("out of range") != std::string::npos);
}
