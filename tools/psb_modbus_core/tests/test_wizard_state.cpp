#include "topology_rules.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("TopologyRules - addBus rejects an empty port", "[topology_rules]") {
    psb::TopologyConfig topo;
    CHECK(psb::addBus(topo, "bus1", "", 115200) == "port required");
    CHECK(topo.buses.empty());
}

TEST_CASE("TopologyRules - addBus rejects a port already in use by another bus", "[topology_rules]") {
    psb::TopologyConfig topo;
    REQUIRE(psb::addBus(topo, "bus1", "/dev/ttyUSB0", 115200).empty());
    CHECK(psb::addBus(topo, "bus2", "/dev/ttyUSB0", 9600) ==
          "port already in use by bus \"bus1\"");
    CHECK(topo.buses.size() == 1);
}

TEST_CASE("TopologyRules - addBus defaults an empty name to busN", "[topology_rules]") {
    psb::TopologyConfig topo;
    REQUIRE(psb::addBus(topo, "", "/dev/ttyUSB0", 115200).empty());
    CHECK(topo.buses[0].name == "bus1");
}

TEST_CASE("TopologyRules - removeBus rejects an out-of-range index", "[topology_rules]") {
    psb::TopologyConfig topo;
    CHECK(psb::removeBus(topo, 0) == "invalid bus index");
}

TEST_CASE("TopologyRules - removeBus drops the bus", "[topology_rules]") {
    psb::TopologyConfig topo;
    psb::addBus(topo, "bus1", "/dev/ttyUSB0", 115200);
    REQUIRE(psb::removeBus(topo, 0).empty());
    CHECK(topo.buses.empty());
}

TEST_CASE("TopologyRules - addBoard rejects an empty nickname", "[topology_rules]") {
    psb::TopologyConfig topo;
    psb::addBus(topo, "bus1", "/dev/ttyUSB0", 115200);
    CHECK(psb::addBoard(topo, 0, "", 1) == "nickname required");
}

TEST_CASE("TopologyRules - addBoard rejects an out-of-range slave ID", "[topology_rules]") {
    psb::TopologyConfig topo;
    psb::addBus(topo, "bus1", "/dev/ttyUSB0", 115200);
    CHECK(psb::addBoard(topo, 0, "board1", 300) == "slave ID must be 0-247");
}

TEST_CASE("TopologyRules - addBoard rejects a duplicate nickname across the whole topology", "[topology_rules]") {
    psb::TopologyConfig topo;
    psb::addBus(topo, "bus1", "/dev/ttyUSB0", 115200);
    psb::addBus(topo, "bus2", "/dev/ttyUSB1", 115200);
    REQUIRE(psb::addBoard(topo, 0, "hvb-bench", 1).empty());
    CHECK(psb::addBoard(topo, 1, "hvb-bench", 1) == "nickname \"hvb-bench\" already in use");
}

TEST_CASE("TopologyRules - addBoard rejects a duplicate slave ID on the same bus", "[topology_rules]") {
    psb::TopologyConfig topo;
    psb::addBus(topo, "bus1", "/dev/ttyUSB0", 115200);
    REQUIRE(psb::addBoard(topo, 0, "board1", 1).empty());
    CHECK(psb::addBoard(topo, 0, "board2", 1) == "slave ID 1 already used on this bus");
}

TEST_CASE("TopologyRules - addBoard allows the same slave ID on different buses", "[topology_rules]") {
    psb::TopologyConfig topo;
    psb::addBus(topo, "bus1", "/dev/ttyUSB0", 115200);
    psb::addBus(topo, "bus2", "/dev/ttyUSB1", 115200);
    REQUIRE(psb::addBoard(topo, 0, "board1", 1).empty());
    CHECK(psb::addBoard(topo, 1, "board2", 1).empty());
}

TEST_CASE("TopologyRules - removeBoard drops the board", "[topology_rules]") {
    psb::TopologyConfig topo;
    psb::addBus(topo, "bus1", "/dev/ttyUSB0", 115200);
    psb::addBoard(topo, 0, "board1", 1);
    REQUIRE(psb::removeBoard(topo, 0, 0).empty());
    CHECK(topo.buses[0].boards.empty());
}
