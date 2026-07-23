#include <catch2/catch_test_macros.hpp>
#include "../../psb_demo_app/tui/group_wizard_state.h"

using namespace psb::tui;

TEST_CASE("GroupWizardState — addGroup rejects an empty name", "[group_wizard_state]") {
    GroupWizardState s;
    CHECK(addGroup(s, "") == "group name required");
    CHECK(s.topo.groups.empty());
}

TEST_CASE("GroupWizardState — addGroup rejects a duplicate name", "[group_wizard_state]") {
    GroupWizardState s;
    REQUIRE(addGroup(s, "Battery Bank").empty());
    CHECK(addGroup(s, "Battery Bank") == "group name \"Battery Bank\" already in use");
    CHECK(s.topo.groups.size() == 1);
}

TEST_CASE("GroupWizardState — successful addGroup marks the state dirty", "[group_wizard_state]") {
    GroupWizardState s;
    CHECK_FALSE(s.dirty);
    addGroup(s, "Battery Bank");
    CHECK(s.dirty);
}

TEST_CASE("GroupWizardState — removeGroup rejects an out-of-range index", "[group_wizard_state]") {
    GroupWizardState s;
    CHECK(removeGroup(s, 0) == "invalid group index");
}

TEST_CASE("GroupWizardState — removeGroup drops the group and clears selection past the end", "[group_wizard_state]") {
    GroupWizardState s;
    addGroup(s, "Battery Bank");
    s.selectedGroup = 0;
    REQUIRE(removeGroup(s, 0).empty());
    CHECK(s.topo.groups.empty());
    CHECK(s.selectedGroup == -1);
}

TEST_CASE("GroupWizardState — addChannelToGroup rejects an invalid group index", "[group_wizard_state]") {
    GroupWizardState s;
    CHECK(addChannelToGroup(s, 0, "hvb-bench", 0, "CH0") == "invalid group index");
}

TEST_CASE("GroupWizardState — addChannelToGroup rejects an empty board nickname", "[group_wizard_state]") {
    GroupWizardState s;
    addGroup(s, "Battery Bank");
    CHECK(addChannelToGroup(s, 0, "", 0, "CH0") == "board required");
}

TEST_CASE("GroupWizardState — addChannelToGroup rejects a channel already in the group", "[group_wizard_state]") {
    GroupWizardState s;
    addGroup(s, "Battery Bank");
    REQUIRE(addChannelToGroup(s, 0, "hvb-bench", 0, "CH0").empty());
    CHECK(addChannelToGroup(s, 0, "hvb-bench", 0, "CH0") ==
          "hvb-bench/CH0 already assigned to group Battery Bank");
    CHECK(s.topo.groups[0].channels.size() == 1);
}

TEST_CASE("GroupWizardState — addChannelToGroup allows the same channel index on different boards", "[group_wizard_state]") {
    GroupWizardState s;
    addGroup(s, "Battery Bank");
    REQUIRE(addChannelToGroup(s, 0, "hvb-bench", 0, "CH0").empty());
    CHECK(addChannelToGroup(s, 0, "hvb-bench-2", 0, "CH1").empty());
    CHECK(s.topo.groups[0].channels.size() == 2);
}

TEST_CASE("GroupWizardState - aliases are unique only inside one group", "[group_wizard_state]") {
    GroupWizardState s;
    REQUIRE(addGroup(s, "detector").empty());
    REQUIRE(addGroup(s, "supply").empty());

    CHECK(addChannelToGroup(s, 0, "hvb-left", 0, "bias").empty());
    CHECK(addChannelToGroup(s, 0, "hvb-left", 1, "bias") ==
          "alias \"bias\" already in use in group detector");
    CHECK(addChannelToGroup(s, 1, "hvb-right", 0, "bias").empty());
}

TEST_CASE("GroupWizardState - board channel can be assigned to only one group", "[group_wizard_state]") {
    GroupWizardState s;
    REQUIRE(addGroup(s, "detector").empty());
    REQUIRE(addGroup(s, "supply").empty());

    CHECK(addChannelToGroup(s, 0, "hvb-left", 0, "bias").empty());
    CHECK(addChannelToGroup(s, 1, "hvb-left", 0, "bias") ==
          "hvb-left/CH0 already assigned to group detector");
}

TEST_CASE("GroupWizardState - group channel alias can be renamed", "[group_wizard_state]") {
    GroupWizardState s;
    REQUIRE(addGroup(s, "detector").empty());
    REQUIRE(addChannelToGroup(s, 0, "hvb-left", 0, "CH0").empty());

    CHECK(renameGroupChannelAlias(s, 0, 0, "bias").empty());
    CHECK(s.topo.groups[0].channels[0].alias == "bias");
    CHECK(s.dirty);
}

TEST_CASE("GroupWizardState - group channel alias rename rejects duplicate in the same group", "[group_wizard_state]") {
    GroupWizardState s;
    REQUIRE(addGroup(s, "detector").empty());
    REQUIRE(addChannelToGroup(s, 0, "hvb-left", 0, "bias").empty());
    REQUIRE(addChannelToGroup(s, 0, "hvb-left", 1, "guard").empty());

    CHECK(renameGroupChannelAlias(s, 0, 1, "bias") ==
          "alias \"bias\" already in use in group detector");
    CHECK(s.topo.groups[0].channels[1].alias == "guard");
}

TEST_CASE("GroupWizardState - board channel alias rename reports duplicate and leaves alias unchanged", "[group_wizard_state]") {
    GroupWizardState s;
    REQUIRE(addGroup(s, "detector").empty());
    REQUIRE(addChannelToGroup(s, 0, "hvb-left", 0, "bias").empty());
    REQUIRE(addChannelToGroup(s, 0, "hvb-left", 1, "guard").empty());

    CHECK(renameGroupChannelAliasForBoardChannel(s.topo, "hvb-left", 1, "bias") ==
          "alias \"bias\" already in use in group detector");
    CHECK(s.topo.groups[0].channels[1].alias == "guard");
}

TEST_CASE("GroupWizardState — removeChannelFromGroup rejects an out-of-range channel index", "[group_wizard_state]") {
    GroupWizardState s;
    addGroup(s, "Battery Bank");
    CHECK(removeChannelFromGroup(s, 0, 0) == "invalid channel index");
}

TEST_CASE("GroupWizardState — removeChannelFromGroup drops the member channel", "[group_wizard_state]") {
    GroupWizardState s;
    addGroup(s, "Battery Bank");
    addChannelToGroup(s, 0, "hvb-bench", 0, "CH0");
    REQUIRE(removeChannelFromGroup(s, 0, 0).empty());
    CHECK(s.topo.groups[0].channels.empty());
}
