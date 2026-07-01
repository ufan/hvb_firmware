#include <catch2/catch_test_macros.hpp>
#include "../../hvb_demo_app/tui/tui_policy.h"

using namespace hvb::tui;

TEST_CASE("port selection preserves an available port") {
    CHECK(selectedPortIndex({"A", "B"}, "B") == 1);
}

TEST_CASE("port selection falls back to the first available port") {
    CHECK(selectedPortIndex({"A", "B"}, "missing") == 0);
    CHECK(selectedPortIndex({}, "missing") == -1);
}

TEST_CASE("status click has no action for invalid, ramping, or zero-target off channels") {
    CHECK(statusClickAction(false, false, 10, 0, false) == StatusClickAction::None);
    CHECK(statusClickAction(true, true, 10, 0, false) == StatusClickAction::None);
    CHECK(statusClickAction(true, false, 0, 0, false) == StatusClickAction::None);
}

TEST_CASE("status click enables a target and gracefully disables an active output") {
    CHECK(statusClickAction(true, false, 10, 0, false) == StatusClickAction::Enable);
    CHECK(statusClickAction(true, false, 10, 10, true) == StatusClickAction::DisableGraceful);
}

TEST_CASE("protection requires both measurement capabilities") {
    CHECK_FALSE(hasProtectionPolicy(0));
    CHECK_FALSE(hasProtectionPolicy(CH_CAP_VOLTAGE_MEASUREMENT));
    CHECK_FALSE(hasProtectionPolicy(CH_CAP_CURRENT_MEASUREMENT));
    CHECK(hasProtectionPolicy(CH_CAP_VOLTAGE_MEASUREMENT |
                              CH_CAP_CURRENT_MEASUREMENT));
}
