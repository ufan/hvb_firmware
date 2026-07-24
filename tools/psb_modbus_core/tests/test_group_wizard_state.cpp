#include "topology_rules.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("TopologyRules - addGroup rejects an empty name", "[topology_rules]") {
    psb::TopologyConfig topo;
    CHECK(psb::addGroup(topo, "") == "group name required");
    CHECK(topo.groups.empty());
}

TEST_CASE("TopologyRules - addGroup rejects a duplicate name", "[topology_rules]") {
    psb::TopologyConfig topo;
    REQUIRE(psb::addGroup(topo, "Battery Bank").empty());
    CHECK(psb::addGroup(topo, "Battery Bank") == "group name \"Battery Bank\" already in use");
    CHECK(topo.groups.size() == 1);
}

TEST_CASE("TopologyRules - removeGroup rejects an out-of-range index", "[topology_rules]") {
    psb::TopologyConfig topo;
    CHECK(psb::removeGroup(topo, 0) == "invalid group index");
}

TEST_CASE("TopologyRules - removeGroup drops the group", "[topology_rules]") {
    psb::TopologyConfig topo;
    psb::addGroup(topo, "Battery Bank");
    REQUIRE(psb::removeGroup(topo, 0).empty());
    CHECK(topo.groups.empty());
}

TEST_CASE("TopologyRules - group can be renamed", "[topology_rules]") {
    psb::TopologyConfig topo;
    REQUIRE(psb::addGroup(topo, "detector").empty());

    CHECK(psb::renameGroup(topo, 0, "bias-bank").empty());
    CHECK(topo.groups[0].name == "bias-bank");
}

TEST_CASE("TopologyRules - renameGroup rejects empty and invalid group names", "[topology_rules]") {
    psb::TopologyConfig topo;
    REQUIRE(psb::addGroup(topo, "detector").empty());

    CHECK(psb::renameGroup(topo, 0, "") == "group name required");
    CHECK(psb::renameGroup(topo, 1, "supply") == "invalid group index");
    CHECK(topo.groups[0].name == "detector");
}

TEST_CASE("TopologyRules - renameGroup rejects duplicate names", "[topology_rules]") {
    psb::TopologyConfig topo;
    REQUIRE(psb::addGroup(topo, "detector").empty());
    REQUIRE(psb::addGroup(topo, "supply").empty());

    CHECK(psb::renameGroup(topo, 1, "detector") == "group name \"detector\" already in use");
    CHECK(topo.groups[1].name == "supply");
}

TEST_CASE("TopologyRules - addChannelToGroup rejects an invalid group index", "[topology_rules]") {
    psb::TopologyConfig topo;
    CHECK(psb::addChannelToGroup(topo, 0, "hvb-bench", 0, "CH0") == "invalid group index");
}

TEST_CASE("TopologyRules - addChannelToGroup rejects an empty board nickname", "[topology_rules]") {
    psb::TopologyConfig topo;
    psb::addGroup(topo, "Battery Bank");
    CHECK(psb::addChannelToGroup(topo, 0, "", 0, "CH0") == "board required");
}

TEST_CASE("TopologyRules - addChannelToGroup rejects a channel already in the group", "[topology_rules]") {
    psb::TopologyConfig topo;
    psb::addGroup(topo, "Battery Bank");
    REQUIRE(psb::addChannelToGroup(topo, 0, "hvb-bench", 0, "CH0").empty());
    CHECK(psb::addChannelToGroup(topo, 0, "hvb-bench", 0, "CH0") ==
          "hvb-bench/CH0 already assigned to group Battery Bank");
    CHECK(topo.groups[0].channels.size() == 1);
}

TEST_CASE("TopologyRules - addChannelToGroup allows the same channel index on different boards", "[topology_rules]") {
    psb::TopologyConfig topo;
    psb::addGroup(topo, "Battery Bank");
    REQUIRE(psb::addChannelToGroup(topo, 0, "hvb-bench", 0, "CH0").empty());
    CHECK(psb::addChannelToGroup(topo, 0, "hvb-bench-2", 0, "CH1").empty());
    CHECK(topo.groups[0].channels.size() == 2);
}

TEST_CASE("TopologyRules - aliases are unique only inside one group", "[topology_rules]") {
    psb::TopologyConfig topo;
    REQUIRE(psb::addGroup(topo, "detector").empty());
    REQUIRE(psb::addGroup(topo, "supply").empty());

    CHECK(psb::addChannelToGroup(topo, 0, "hvb-left", 0, "bias").empty());
    CHECK(psb::addChannelToGroup(topo, 0, "hvb-left", 1, "bias") ==
          "alias \"bias\" already in use in group detector");
    CHECK(psb::addChannelToGroup(topo, 1, "hvb-right", 0, "bias").empty());
}

TEST_CASE("TopologyRules - board channel can be assigned to only one group", "[topology_rules]") {
    psb::TopologyConfig topo;
    REQUIRE(psb::addGroup(topo, "detector").empty());
    REQUIRE(psb::addGroup(topo, "supply").empty());

    CHECK(psb::addChannelToGroup(topo, 0, "hvb-left", 0, "bias").empty());
    CHECK(psb::addChannelToGroup(topo, 1, "hvb-left", 0, "bias") ==
          "hvb-left/CH0 already assigned to group detector");
}

TEST_CASE("TopologyRules - group channel alias can be renamed", "[topology_rules]") {
    psb::TopologyConfig topo;
    REQUIRE(psb::addGroup(topo, "detector").empty());
    REQUIRE(psb::addChannelToGroup(topo, 0, "hvb-left", 0, "CH0").empty());

    CHECK(psb::renameGroupChannelAlias(topo, 0, 0, "bias").empty());
    CHECK(topo.groups[0].channels[0].alias == "bias");
}

TEST_CASE("TopologyRules - group channel alias rename rejects duplicate in the same group", "[topology_rules]") {
    psb::TopologyConfig topo;
    REQUIRE(psb::addGroup(topo, "detector").empty());
    REQUIRE(psb::addChannelToGroup(topo, 0, "hvb-left", 0, "bias").empty());
    REQUIRE(psb::addChannelToGroup(topo, 0, "hvb-left", 1, "guard").empty());

    CHECK(psb::renameGroupChannelAlias(topo, 0, 1, "bias") ==
          "alias \"bias\" already in use in group detector");
    CHECK(topo.groups[0].channels[1].alias == "guard");
}

TEST_CASE("TopologyRules - board channel alias rename reports duplicate and leaves alias unchanged", "[topology_rules]") {
    psb::TopologyConfig topo;
    REQUIRE(psb::addGroup(topo, "detector").empty());
    REQUIRE(psb::addChannelToGroup(topo, 0, "hvb-left", 0, "bias").empty());
    REQUIRE(psb::addChannelToGroup(topo, 0, "hvb-left", 1, "guard").empty());

    CHECK(psb::renameGroupChannelAliasForBoardChannel(topo, "hvb-left", 1, "bias") ==
          "alias \"bias\" already in use in group detector");
    CHECK(topo.groups[0].channels[1].alias == "guard");
}

TEST_CASE("TopologyRules - removeChannelFromGroup rejects an out-of-range channel index", "[topology_rules]") {
    psb::TopologyConfig topo;
    psb::addGroup(topo, "Battery Bank");
    CHECK(psb::removeChannelFromGroup(topo, 0, 0) == "invalid channel index");
}

TEST_CASE("TopologyRules - removeChannelFromGroup drops the member channel", "[topology_rules]") {
    psb::TopologyConfig topo;
    psb::addGroup(topo, "Battery Bank");
    psb::addChannelToGroup(topo, 0, "hvb-bench", 0, "CH0");
    REQUIRE(psb::removeChannelFromGroup(topo, 0, 0).empty());
    CHECK(topo.groups[0].channels.empty());
}
