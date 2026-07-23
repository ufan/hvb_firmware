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

TEST_CASE("TopologyRules - availableGroupChannels omits assigned channels", "[topology_rules]") {
    psb::TopologyConfig topo;
    psb::GroupConfig group;
    group.name = "detector";
    group.channels.push_back({"hvb-left", 0, "bias"});
    topo.groups.push_back(group);

    std::vector<psb::LiveBoardInfo> live{{"hvb-left", 2}, {"hvb-right", 1}};
    auto available = psb::availableGroupChannels(topo, live);

    REQUIRE(available.size() == 2);
    CHECK(available[0].boardNickname == "hvb-left");
    CHECK(available[0].channelIndex == 1);
    CHECK(available[0].alias == "CH1");
    CHECK(available[1].boardNickname == "hvb-right");
    CHECK(available[1].channelIndex == 0);
    CHECK(available[1].alias == "CH0");
}

TEST_CASE("TopologyRules - resolveBoardEndpoint selects the only board without a nickname", "[topology_rules]") {
    psb::TopologyConfig topo;
    psb::BusConfig bus;
    bus.name = "bus1";
    bus.port = "/dev/ttyUSB0";
    bus.baudRate = 9600;
    psb::BoardConfig board;
    board.nickname = "hvb-left";
    board.slaveId = 7;
    bus.boards.push_back(board);
    topo.buses.push_back(bus);

    psb::BoardEndpoint endpoint;
    CHECK(psb::resolveBoardEndpoint(topo, "", endpoint).empty());
    CHECK(endpoint.port == "/dev/ttyUSB0");
    CHECK(endpoint.baudRate == 9600);
    CHECK(endpoint.slaveId == 7);
    CHECK(endpoint.boardNickname == "hvb-left");
}

TEST_CASE("TopologyRules - resolveBoardEndpoint requires nickname for multi-board topology", "[topology_rules]") {
    psb::TopologyConfig topo;
    psb::BusConfig bus;
    bus.name = "bus1";
    bus.port = "/dev/ttyUSB0";
    bus.boards.push_back({"hvb-left", 1, {}});
    bus.boards.push_back({"hvb-right", 2, {}});
    topo.buses.push_back(bus);

    psb::BoardEndpoint endpoint;
    CHECK(psb::resolveBoardEndpoint(topo, "", endpoint) ==
          "topology has 2 boards; specify one with --board <nickname>");
    CHECK(psb::resolveBoardEndpoint(topo, "hvb-right", endpoint).empty());
    CHECK(endpoint.slaveId == 2);
}

TEST_CASE("TopologyRules - resolveBoardEndpoint reports missing boards", "[topology_rules]") {
    psb::TopologyConfig topo;
    psb::BusConfig bus;
    bus.name = "bus1";
    bus.port = "/dev/ttyUSB0";
    bus.boards.push_back({"hvb-left", 1, {}});
    topo.buses.push_back(bus);

    psb::BoardEndpoint endpoint;
    CHECK(psb::resolveBoardEndpoint(topo, "missing", endpoint) == "no board named 'missing'");
}
