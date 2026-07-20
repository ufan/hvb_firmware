#include <catch2/catch_test_macros.hpp>
#include "../../psb_demo_app/tui/wizard_state.h"

using namespace psb::tui;

TEST_CASE("WizardState — addBus rejects an empty port", "[wizard_state]") {
    WizardState s;
    CHECK(addBus(s, "bus1", "", 115200) == "port required");
    CHECK(s.topo.buses.empty());
}

TEST_CASE("WizardState — addBus rejects a port already in use by another bus", "[wizard_state]") {
    WizardState s;
    REQUIRE(addBus(s, "bus1", "/dev/ttyUSB0", 115200).empty());
    CHECK(addBus(s, "bus2", "/dev/ttyUSB0", 9600) == "port already in use by bus \"bus1\"");
    CHECK(s.topo.buses.size() == 1);
}

TEST_CASE("WizardState — addBus defaults an empty name to busN", "[wizard_state]") {
    WizardState s;
    REQUIRE(addBus(s, "", "/dev/ttyUSB0", 115200).empty());
    CHECK(s.topo.buses[0].name == "bus1");
}

TEST_CASE("WizardState — removeBus rejects an out-of-range index", "[wizard_state]") {
    WizardState s;
    CHECK(removeBus(s, 0) == "invalid bus index");
}

TEST_CASE("WizardState — removeBus drops the bus and clears selection past the end", "[wizard_state]") {
    WizardState s;
    addBus(s, "bus1", "/dev/ttyUSB0", 115200);
    s.selectedBus = 0;
    REQUIRE(removeBus(s, 0).empty());
    CHECK(s.topo.buses.empty());
    CHECK(s.selectedBus == -1);
}

TEST_CASE("WizardState — addBoard rejects an empty nickname", "[wizard_state]") {
    WizardState s;
    addBus(s, "bus1", "/dev/ttyUSB0", 115200);
    CHECK(addBoard(s, 0, "", 1) == "nickname required");
}

TEST_CASE("WizardState — addBoard rejects an out-of-range slave ID", "[wizard_state]") {
    WizardState s;
    addBus(s, "bus1", "/dev/ttyUSB0", 115200);
    CHECK(addBoard(s, 0, "board1", 300) == "slave ID must be 0-247");
}

TEST_CASE("WizardState — addBoard rejects a duplicate nickname across the whole topology", "[wizard_state]") {
    WizardState s;
    addBus(s, "bus1", "/dev/ttyUSB0", 115200);
    addBus(s, "bus2", "/dev/ttyUSB1", 115200);
    REQUIRE(addBoard(s, 0, "hvb-bench", 1).empty());
    CHECK(addBoard(s, 1, "hvb-bench", 1) == "nickname \"hvb-bench\" already in use");
}

TEST_CASE("WizardState — addBoard rejects a duplicate slave ID on the same bus", "[wizard_state]") {
    WizardState s;
    addBus(s, "bus1", "/dev/ttyUSB0", 115200);
    REQUIRE(addBoard(s, 0, "board1", 1).empty());
    CHECK(addBoard(s, 0, "board2", 1) == "slave ID 1 already used on this bus");
}

TEST_CASE("WizardState — addBoard allows the same slave ID on different buses", "[wizard_state]") {
    WizardState s;
    addBus(s, "bus1", "/dev/ttyUSB0", 115200);
    addBus(s, "bus2", "/dev/ttyUSB1", 115200);
    REQUIRE(addBoard(s, 0, "board1", 1).empty());
    CHECK(addBoard(s, 1, "board2", 1).empty());
}

TEST_CASE("WizardState — removeBoard drops the board and clears selection past the end", "[wizard_state]") {
    WizardState s;
    addBus(s, "bus1", "/dev/ttyUSB0", 115200);
    addBoard(s, 0, "board1", 1);
    s.selectedBus = 0; s.selectedBoard = 0;
    REQUIRE(removeBoard(s, 0, 0).empty());
    CHECK(s.topo.buses[0].boards.empty());
    CHECK(s.selectedBoard == -1);
}

TEST_CASE("WizardState — successful mutations mark the state dirty", "[wizard_state]") {
    WizardState s;
    CHECK_FALSE(s.dirty);
    addBus(s, "bus1", "/dev/ttyUSB0", 115200);
    CHECK(s.dirty);
}
