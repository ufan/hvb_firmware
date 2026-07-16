#include <catch2/catch_test_macros.hpp>
#include "../../psb_demo_app/tui/tui_policy.h"

using namespace psb::tui;

TEST_CASE("port selection preserves an available port") {
    CHECK(selectedPortIndex({"A", "B"}, "B") == 1);
}

TEST_CASE("port selection falls back to the first available port") {
    CHECK(selectedPortIndex({"A", "B"}, "missing") == 0);
    CHECK(selectedPortIndex({}, "missing") == -1);
}

TEST_CASE("status click has no action when invalid or ramping") {
    CHECK(statusClickAction(false, false, false) == StatusClickAction::None);
    CHECK(statusClickAction(true, true, false) == StatusClickAction::None);
    CHECK(statusClickAction(true, true, true) == StatusClickAction::None);
}

TEST_CASE("status click enables a disabled channel and gracefully disables an enabled one") {
    CHECK(statusClickAction(true, false, false) == StatusClickAction::Enable);
    CHECK(statusClickAction(true, false, true) == StatusClickAction::DisableGraceful);
}

TEST_CASE("channelIsOn is capability-aware") {
    // Both OUTPUT_ENABLE and RAW_OUTPUT_DRIVE (e.g. jw_hvb): enabled AND driving.
    CHECK(channelIsOn(true, true, true, true));
    CHECK_FALSE(channelIsOn(true, true, true, false));   // enabled but driving 0 -> off
    CHECK_FALSE(channelIsOn(true, true, false, true));
    CHECK_FALSE(channelIsOn(true, true, false, false));

    // OUTPUT_ENABLE only (e.g. jw_lvb fixed-voltage channels): enable gate alone.
    CHECK(channelIsOn(true, false, true, false));
    CHECK(channelIsOn(true, false, true, true));
    CHECK_FALSE(channelIsOn(true, false, false, false));
    CHECK_FALSE(channelIsOn(true, false, false, true));

    // RAW_OUTPUT_DRIVE only (a DAC with no enable gate): drive value alone.
    CHECK(channelIsOn(false, true, false, true));
    CHECK(channelIsOn(false, true, true, true));
    CHECK_FALSE(channelIsOn(false, true, false, false));
    CHECK_FALSE(channelIsOn(false, true, true, false));

    // Neither capability: never on.
    CHECK_FALSE(channelIsOn(false, false, true, true));
}

TEST_CASE("protection requires both measurement capabilities") {
    CHECK_FALSE(hasProtectionPolicy(0));
    CHECK_FALSE(hasProtectionPolicy(CH_CAP_VOLTAGE_MEASUREMENT));
    CHECK_FALSE(hasProtectionPolicy(CH_CAP_CURRENT_MEASUREMENT));
    CHECK(hasProtectionPolicy(CH_CAP_VOLTAGE_MEASUREMENT |
                              CH_CAP_CURRENT_MEASUREMENT));
}

TEST_CASE("TUI poll refreshes every discovered channel, not only active outputs") {
    CHECK(shouldPollChannel(0, 10));
    CHECK(shouldPollChannel(9, 10));
    CHECK_FALSE(shouldPollChannel(10, 10));
    CHECK_FALSE(shouldPollChannel(-1, 10));
}

TEST_CASE("unsupported monitor cells use a visible neutral placeholder") {
    CHECK(unsupportedMonitorCellLabel() == std::string("n/a"));
}

TEST_CASE("disconnected UI retains only Monitor") {
    std::vector<std::string> titles = {"Monitor", "CH0", "CH1"};
    int active = 2;

    CHECK(reconcileDisconnectedTabs(false, titles, active));
    CHECK(titles == std::vector<std::string>{"Monitor"});
    CHECK(active == 0);
    CHECK_FALSE(reconcileDisconnectedTabs(true, titles, active));
}
