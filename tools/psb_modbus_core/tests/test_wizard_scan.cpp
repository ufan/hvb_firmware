#include "../../psb_demo_app/tui/wizard_scan.h"
#include "register_map.h"

#include <catch2/catch_test_macros.hpp>

using namespace psb::tui;

TEST_CASE("scanBus — finds only the slave IDs that respond, in range order", "[wizard_scan]") {
    auto bus = std::make_shared<psb::PsbSerialBus>();

    uint16_t inputA[280] = {}, holdingA[280] = {};
    inputA[SYS_PROTOCOL_MAJOR] = VC_PROTOCOL_MAJOR;
    inputA[SYS_PROTOCOL_MINOR] = VC_PROTOCOL_MINOR;
    inputA[SYS_VARIANT_ID] = 1;  // jw_hvb
    bus->attachTestArrays(3, inputA, holdingA, 280);

    uint16_t inputB[280] = {}, holdingB[280] = {};
    inputB[SYS_PROTOCOL_MAJOR] = VC_PROTOCOL_MAJOR;
    inputB[SYS_PROTOCOL_MINOR] = VC_PROTOCOL_MINOR;
    inputB[SYS_VARIANT_ID] = 2;  // jw_lvb
    bus->attachTestArrays(7, inputB, holdingB, 280);
    // No test arrays attached for any other ID in [1, 10] — every other
    // candidate's verifyProtocol() fails, matching "nothing at this address"
    // on real hardware.

    auto found = scanBus(bus, 1, 10);

    REQUIRE(found.size() == 2);
    CHECK(found[0].slaveId == 3);
    CHECK(found[0].variantName == "jw_hvb");
    CHECK(found[1].slaveId == 7);
    CHECK(found[1].variantName == "jw_lvb");
}

TEST_CASE("scanBus — reports progress for every candidate in range, including non-responders", "[wizard_scan]") {
    auto bus = std::make_shared<psb::PsbSerialBus>();
    uint16_t input[280] = {}, holding[280] = {};
    input[SYS_PROTOCOL_MAJOR] = VC_PROTOCOL_MAJOR;
    input[SYS_PROTOCOL_MINOR] = VC_PROTOCOL_MINOR;
    bus->attachTestArrays(2, input, holding, 280);

    std::vector<int> probed;
    scanBus(bus, 1, 4, [&](int id) { probed.push_back(id); });

    CHECK(probed == std::vector<int>{1, 2, 3, 4});
}

TEST_CASE("scanBus — empty range with nothing attached finds nothing", "[wizard_scan]") {
    auto bus = std::make_shared<psb::PsbSerialBus>();
    CHECK(scanBus(bus, 1, 5).empty());
}
