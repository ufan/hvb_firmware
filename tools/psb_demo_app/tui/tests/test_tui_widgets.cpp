#include <catch2/catch_test_macros.hpp>
#include "app_window_layout.h"
#include "board_dashboard.h"
#include "channel_live_layout.h"
#include "group_monitor.h"
#include "group_view_selection.h"
#include "widgets.h"

using namespace psb::tui;
using namespace ftxui;

TEST_CASE("MouseOnlyActionButton consumes Return without firing action", "[tui_widgets]") {
    bool fired = false;
    auto button = MouseOnlyActionButton("Go", [&] { fired = true; });

    CHECK(button->OnEvent(Event::Return));
    CHECK_FALSE(fired);
}

TEST_CASE("App window has a minimum width for dense dashboard rows", "[tui_widgets]") {
    CHECK(appWindowMinWidthColumns() >= 132);
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

    REQUIRE(sections.size() == 6);
    CHECK(sections[0] == ChannelLiveSection::Title);
    CHECK(sections[1] == ChannelLiveSection::Telemetry);
    CHECK(sections[2] == ChannelLiveSection::Spacer);
    CHECK(sections[3] == ChannelLiveSection::ControlActions);
    CHECK(sections[4] == ChannelLiveSection::Spacer);
    CHECK(sections[5] == ChannelLiveSection::GroupNameControls);
}

TEST_CASE("Channel group alias label is named Name", "[tui_widgets]") {
    CHECK(channelGroupAliasLabel() == " Name: ");
}

TEST_CASE("Channel live control and startup policy labels", "[tui_widgets]") {
    CHECK(channelLiveControlLabel() == " Control ");
    CHECK(channelStartupPolicyPaneTitle() == " Startup Policy ");
}

TEST_CASE("Channel policy boxes are arranged by requested columns", "[tui_widgets]") {
    auto layout = channelPolicyBoxLayout();

    REQUIRE(layout.size() == 2);
    REQUIRE(layout[0].size() == 2);
    REQUIRE(layout[1].size() == 2);
    CHECK(layout[0][0] == ChannelPolicyBox::StartupPolicy);
    CHECK(layout[0][1] == ChannelPolicyBox::ProtectionPolicy);
    CHECK(layout[1][0] == ChannelPolicyBox::RecoveryPolicy);
    CHECK(layout[1][1] == ChannelPolicyBox::Setting);
}

TEST_CASE("Channel policy box content heights align protection and setting bottoms", "[tui_widgets]") {
    CHECK(channelPolicyBoxContentRows(ChannelPolicyBox::StartupPolicy) +
          channelPolicyBoxContentRows(ChannelPolicyBox::ProtectionPolicy) ==
          channelPolicyBoxContentRows(ChannelPolicyBox::RecoveryPolicy) +
          channelPolicyBoxContentRows(ChannelPolicyBox::Setting));
    CHECK(channelProtectionLeftColumnRows() == 3);
    CHECK(channelProtectionButtonColumnRows() == 2);
}

TEST_CASE("Board menu identity shows nickname and connected channel variant summary", "[tui_widgets]") {
    BoardSession board;
    board.nickname = "rack-left";

    auto offline = boardMenuIdentityLabels(board);
    CHECK(offline.boardName == "rack-left");
    CHECK(offline.channelVariant == "-- --");

    board.data.valid = true;
    board.connected = true;
    board.data.sysInfo.supportedChannels = 10;
    board.data.sysInfo.activeChMask = 0x03ff;
    board.data.sysInfo.variantId = 1;

    auto online = boardMenuIdentityLabels(board);
    CHECK(online.boardName == "rack-left");
    CHECK(online.channelVariant == "10 Ch @ jw_hvb");
}

TEST_CASE("Board menu actions omit system Save", "[tui_widgets]") {
    auto singleBoard = boardMenuActionSlots(1);
    REQUIRE(singleBoard.size() == 1);
    CHECK(singleBoard[0] == BoardMenuActionSlot::ConnectToggle);

    auto multiBoard = boardMenuActionSlots(2);
    REQUIRE(multiBoard.size() == 2);
    CHECK(multiBoard[0] == BoardMenuActionSlot::ConnectToggle);
    CHECK(multiBoard[1] == BoardMenuActionSlot::Remove);
}

TEST_CASE("Group name save status mirrors alias save feedback", "[tui_widgets]") {
    CHECK(groupNameSaveStatus("") == "OK: group renamed");
    CHECK(groupNameSaveStatus("group name \"bias\" already in use") ==
          "Error: group name \"bias\" already in use");
}
