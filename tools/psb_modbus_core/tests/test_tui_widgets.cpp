#include <catch2/catch_test_macros.hpp>
#include "../../psb_demo_app/tui/channel_live_layout.h"
#include "../../psb_demo_app/tui/group_view_selection.h"
#include "../../psb_demo_app/tui/widgets.h"

using namespace psb::tui;
using namespace ftxui;

TEST_CASE("MouseOnlyActionButton consumes Return without firing action", "[tui_widgets]") {
    bool fired = false;
    auto button = MouseOnlyActionButton("Go", [&] { fired = true; });

    CHECK(button->OnEvent(Event::Return));
    CHECK_FALSE(fired);
}

TEST_CASE("Group dashboard replacement preserves current group view", "[tui_widgets]") {
    std::vector<std::string> oldGroups{"bias", "guard"};
    std::vector<std::string> newGroups{"bias", "guard"};
    int groupIdx = 0;
    bool showingGroup = true;
    int mainSelected = 3;
    int visibleContentIdx = 1;

    reconcileGroupViewAfterReplacement(oldGroups, newGroups, true, 0,
                                       groupIdx, showingGroup, mainSelected, visibleContentIdx);

    CHECK(groupIdx == 0);
    CHECK(showingGroup);
    CHECK(mainSelected == 3);
    CHECK(visibleContentIdx == 1);
}

TEST_CASE("Group dashboard replacement falls back to boards when groups disappear", "[tui_widgets]") {
    std::vector<std::string> oldGroups{"bias"};
    std::vector<std::string> newGroups;
    int groupIdx = 0;
    bool showingGroup = true;
    int mainSelected = 3;
    int visibleContentIdx = 1;

    reconcileGroupViewAfterReplacement(oldGroups, newGroups, true, 0,
                                       groupIdx, showingGroup, mainSelected, visibleContentIdx);

    CHECK_FALSE(showingGroup);
    CHECK(mainSelected == 3);
    CHECK(visibleContentIdx == 0);
}

TEST_CASE("Channel live row puts group alias controls at the end", "[tui_widgets]") {
    auto sections = channelLiveSections(true);

    REQUIRE(sections.size() == 5);
    CHECK(sections[0] == ChannelLiveSection::Title);
    CHECK(sections[1] == ChannelLiveSection::Telemetry);
    CHECK(sections[2] == ChannelLiveSection::ControlActions);
    CHECK(sections[3] == ChannelLiveSection::Spacer);
    CHECK(sections[4] == ChannelLiveSection::GroupNameControls);
}

TEST_CASE("Channel group alias label is named Name", "[tui_widgets]") {
    CHECK(channelGroupAliasLabel() == " Name: ");
}

TEST_CASE("Channel live control and startup policy labels", "[tui_widgets]") {
    CHECK(channelLiveControlLabel() == " Control ");
    CHECK(channelStartupPolicyPaneTitle() == " Startup Policy ");
}
