#pragma once

#include "board_session.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace psb::tui {

using namespace ftxui;

// The switcher's root Component (always Menu + Container::Tab, never just a
// bare dashboard) plus a live-append hook. Building it this way — instead of
// Phase 2's "collapse to a bare dashboard when there's only one board" —
// means a board can be attached after construction (Task 7's mid-session
// wizard) without rebuilding or swapping the root: Container::Tab and Menu
// both already support dynamic children (ComponentBase::Add; Menu re-reads
// its backing vector's live size every render), so appending is enough.
// Pixel-identical single-board rendering (Phase 2's Global Constraint) is
// preserved by omitting the switcher bar *element* — not the underlying
// component — whenever fewer than two boards exist.
struct BoardSwitcher {
    Component root;
    std::function<void(const std::string& nickname, Component dashboard)> attachBoard;
};

inline BoardSwitcher makeBoardSwitcher(std::vector<std::unique_ptr<BoardSession>>& boards) {
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

    auto dashboardStack = Container::Tab({}, activeBoard.get());
    for (auto& b : boards) dashboardStack->Add(b->dashboard);

    auto mainContainer = Container::Vertical({switcherBar, dashboardStack});
    // Capture boardNames/activeBoard, not just switcherBar/dashboardStack —
    // Menu()/Container::Tab() only hold raw pointers into them (FTXUI's
    // non-owning-pointer widget convention); losing the owning shared_ptrs
    // here was a real use-after-free, found live via gdb during Phase 2
    // (std::length_error inside MenuBase::Clamp() reading a corrupted
    // vector size from freed memory) — see that fix's commit for the full
    // diagnosis. The lesson generalizes: anything Menu/Tab is given a raw
    // pointer into must outlive this returned Component, which this closure
    // is what accomplishes.
    auto root = Renderer(mainContainer, [switcherBar, dashboardStack, boardNames, activeBoard] {
        bool showBar = boardNames->size() > 1;
        Elements top;
        if (showBar) {
            top.push_back(text(" Boards ") | bold | dim);
            top.push_back(switcherBar->Render());
            top.push_back(separator());
        }
        top.push_back(dashboardStack->Render() | flex);
        return vbox(std::move(top));
    });

    auto attachBoard = [boardNames, dashboardStack](const std::string& nickname, Component dashboard) {
        boardNames->push_back(nickname);
        dashboardStack->Add(std::move(dashboard));
    };

    return BoardSwitcher{root, attachBoard};
}

} // namespace psb::tui
