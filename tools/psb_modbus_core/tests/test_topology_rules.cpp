#include "topology_rules.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("TopologyRules - naming queries inspect topology identities", "[topology_rules]") {
    psb::TopologyConfig topo;
    psb::BusConfig bus;
    bus.name = "bus1";
    bus.port = "/dev/ttyUSB0";
    bus.baudRate = 115200;
    psb::BoardConfig left;
    left.nickname = "hvb-left";
    left.slaveId = 1;
    bus.boards.push_back(left);
    psb::BoardConfig right;
    right.nickname = "hvb-right";
    right.slaveId = 2;
    bus.boards.push_back(right);
    topo.buses.push_back(bus);

    psb::GroupConfig group;
    group.name = "detector";
    group.channels.push_back({"hvb-left", 0, "bias"});
    topo.groups.push_back(group);

    CHECK(psb::boardChannelId("hvb-left", 2) == "hvb-left/CH2");
    CHECK(psb::groupNameInUse(topo, "detector"));
    CHECK_FALSE(psb::groupNameInUse(topo, "supply"));
    CHECK(psb::boardNicknameInUse(topo, "hvb-left"));
    CHECK_FALSE(psb::boardNicknameInUse(topo, "missing"));
    CHECK(psb::slaveIdInUse(topo.buses[0], 1));
    CHECK_FALSE(psb::slaveIdInUse(topo.buses[0], 3));
    CHECK(psb::findGroupForBoardChannel(topo, "hvb-left", 0) == 0);
    CHECK(psb::findGroupForBoardChannel(topo, "hvb-left", 1) == -1);
    CHECK(psb::groupAliasInUse(topo.groups[0], "bias"));
    CHECK_FALSE(psb::groupAliasInUse(topo.groups[0], "guard"));
}
