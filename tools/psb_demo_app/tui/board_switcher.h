#pragma once

#include "board_session.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include <memory>
#include <string>
#include <vector>

namespace psb::tui {

using namespace ftxui;

// With exactly one board, returns its dashboard directly — pixel-identical
// to today's single-board layout, no switcher visible (Global Constraints).
// With more than one, adds a board-switcher bar above the active board's
// dashboard, reusing each board's already-built Component from
// makeBoardDashboard() unchanged — nothing about a board's own UI is aware
// a switcher exists around it. Mirrors the existing Monitor/CHx tab pattern
// (Container::Tab + a Menu selecting the index) one level up.
inline Component makeBoardSwitcher(std::vector<std::unique_ptr<BoardSession>>& boards,
                                    ScreenInteractive& screen) {
    (void)screen;  // not needed directly here — each dashboard already captured it
    if (boards.size() == 1) return boards.front()->dashboard;

    auto boardNames = std::make_shared<std::vector<std::string>>();
    for (auto& b : boards) boardNames->push_back(b->nickname);
    auto activeBoard = std::make_shared<int>(0);

    MenuOption switcherOpt = MenuOption::Horizontal();
    switcherOpt.entries_option.transform = [](const EntryState& e) -> Element {
        auto t = text("  " + e.label + "  ");
        if (e.active)                t = t | bold | color(Color::Cyan);
        if (e.focused && !e.active)  t = t | inverted;
        if (!e.active && !e.focused) t = t | dim;
        return t;
    };
    auto switcherBar = Menu(boardNames.get(), activeBoard.get(), switcherOpt);

    Components dashboards;
    for (auto& b : boards) dashboards.push_back(b->dashboard);
    auto dashboardStack = Container::Tab(dashboards, activeBoard.get());

    auto mainContainer = Container::Vertical({switcherBar, dashboardStack});
    return Renderer(mainContainer, [switcherBar, dashboardStack] {
        return vbox({
            text(" Boards ") | bold | dim,
            switcherBar->Render(),
            separator(),
            dashboardStack->Render() | flex,
        });
    });
}

} // namespace psb::tui
