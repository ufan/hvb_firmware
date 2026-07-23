#include <catch2/catch_test_macros.hpp>
#include "../../psb_demo_app/tui/widgets.h"

using namespace psb::tui;
using namespace ftxui;

TEST_CASE("MouseOnlyActionButton consumes Return without firing action", "[tui_widgets]") {
    bool fired = false;
    auto button = MouseOnlyActionButton("Go", [&] { fired = true; });

    CHECK(button->OnEvent(Event::Return));
    CHECK_FALSE(fired);
}
