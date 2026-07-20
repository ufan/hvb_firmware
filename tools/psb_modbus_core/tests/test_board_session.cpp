#include "psb_board_session.h"
#include "psb_serial_bus.h"
#include "register_map.h"

#include <catch2/catch_test_macros.hpp>
#include <memory>

TEST_CASE("PsbBoardSession — verifyProtocol succeeds against a compatible protocol version", "[board_session]") {
    auto bus = std::make_shared<psb::PsbSerialBus>();
    uint16_t inputRegs[8] = {}, holdingRegs[8] = {};
    inputRegs[0] = VC_PROTOCOL_MAJOR;
    inputRegs[1] = VC_PROTOCOL_MINOR;
    bus->attachTestArrays(1, inputRegs, holdingRegs, 8);

    psb::PsbBoardSession session(bus, 1);
    REQUIRE(session.verifyProtocol());
    CHECK(session.isConnected());
}

TEST_CASE("PsbBoardSession — verifyProtocol fails on no response", "[board_session]") {
    auto bus = std::make_shared<psb::PsbSerialBus>();
    // No test arrays attached for slave 1, bus never connected — every read fails.
    psb::PsbBoardSession session(bus, 1);
    REQUIRE_FALSE(session.verifyProtocol());
    CHECK(session.lastError().find("no response from board") != std::string::npos);
    CHECK_FALSE(session.isConnected());
}

TEST_CASE("PsbBoardSession — verifyProtocol fails on out-of-range protocol major", "[board_session]") {
    auto bus = std::make_shared<psb::PsbSerialBus>();
    uint16_t inputRegs[8] = {}, holdingRegs[8] = {};
    inputRegs[0] = 0;  // protoMajor 0 is invalid (< 1)
    bus->attachTestArrays(1, inputRegs, holdingRegs, 8);

    psb::PsbBoardSession session(bus, 1);
    REQUIRE_FALSE(session.verifyProtocol());
    CHECK(session.lastError().find("unexpected protocol version") != std::string::npos);
}

TEST_CASE("PsbBoardSession — two sessions on one bus don't share slave-ID-scoped state", "[board_session]") {
    auto bus = std::make_shared<psb::PsbSerialBus>();
    uint16_t inputA[8] = {}, holdingA[8] = {};
    uint16_t inputB[8] = {}, holdingB[8] = {};
    inputA[0] = VC_PROTOCOL_MAJOR; inputA[1] = VC_PROTOCOL_MINOR;
    inputB[0] = 0;  // deliberately invalid, to prove session B's failure doesn't affect session A
    bus->attachTestArrays(1, inputA, holdingA, 8);
    bus->attachTestArrays(2, inputB, holdingB, 8);

    psb::PsbBoardSession sessionA(bus, 1);
    psb::PsbBoardSession sessionB(bus, 2);
    REQUIRE(sessionA.verifyProtocol());
    REQUIRE_FALSE(sessionB.verifyProtocol());
    CHECK(sessionA.isConnected());
    CHECK_FALSE(sessionB.isConnected());
}

TEST_CASE("PsbBoardSession — attachTestArrays bypasses verifyProtocol, matching PsbModbusClient's existing contract", "[board_session]") {
    auto bus = std::make_shared<psb::PsbSerialBus>();
    uint16_t inputRegs[280] = {}, holdingRegs[280] = {};
    psb::PsbBoardSession session(bus, 1);
    session.attachTestArrays(inputRegs, holdingRegs, 280);
    CHECK(session.isConnected());

    session.detachTestArrays();
    CHECK_FALSE(session.isConnected());
}

TEST_CASE("PsbBoardSession — rebind() changes slave ID without changing object identity", "[board_session]") {
    auto bus = std::make_shared<psb::PsbSerialBus>();
    uint16_t inputA[8] = {}, holdingA[8] = {};
    uint16_t inputB[8] = {}, holdingB[8] = {};
    inputA[0] = VC_PROTOCOL_MAJOR; inputA[1] = VC_PROTOCOL_MINOR;
    inputB[0] = VC_PROTOCOL_MAJOR; inputB[1] = VC_PROTOCOL_MINOR;
    bus->attachTestArrays(1, inputA, holdingA, 8);
    bus->attachTestArrays(2, inputB, holdingB, 8);

    psb::PsbBoardSession session(bus, 1);
    REQUIRE(session.verifyProtocol());
    CHECK(session.slaveId() == 1);

    session.rebind(2);
    CHECK(session.slaveId() == 2);
    // rebind() resets verified state — the previous verifyProtocol() result
    // was for slave 1, not slave 2, even though this happens to also be a
    // valid slave here.
    CHECK_FALSE(session.isConnected());
    REQUIRE(session.verifyProtocol());
    CHECK(session.isConnected());
}

TEST_CASE("PsbBoardSession — verifyProtocol accepts a timeout override without changing behavior", "[board_session]") {
    auto bus = std::make_shared<psb::PsbSerialBus>();
    uint16_t inputRegs[8] = {}, holdingRegs[8] = {};
    inputRegs[0] = VC_PROTOCOL_MAJOR;
    inputRegs[1] = VC_PROTOCOL_MINOR;
    bus->attachTestArrays(1, inputRegs, holdingRegs, 8);

    psb::PsbBoardSession session(bus, 1);
    // Test-mode arrays respond instantly regardless of the timeout value —
    // this only proves the overload compiles and still succeeds, matching
    // the existing no-argument call's behavior exactly.
    REQUIRE(session.verifyProtocol(50));
    CHECK(session.isConnected());
}
