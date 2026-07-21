#pragma once

#include "board_session.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include <algorithm>
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
    std::function<void(const std::string& nickname)> detachBoard;
};

inline BoardSwitcher makeBoardSwitcher(std::vector<std::unique_ptr<BoardSession>>& boards,
                                       Component globalQuit, Component globalSetup,
                                       Component globalPreferences) {
    auto boardNames = std::make_shared<std::vector<std::string>>();
    for (auto& b : boards) boardNames->push_back(b->nickname);
    auto activeBoard = std::make_shared<int>(0);
    // Starting focus for the switcher's own outer container. Same
    // reasoning as before Sub-project B, just re-indexed for the new
    // 3-slot scheme (0=globalMenuBar, 1=switcherBar, 2=dashboardStack):
    // with a single board the switcher bar is a Menu with one entry, which
    // unconditionally swallows Event::Return and doesn't handle Up/Down
    // at all (see MenuBase::OnEvent in the vendored FTXUI source) — if it
    // held initial focus, keys typed immediately at launch would be
    // silently dropped until the user happened to Tab/Down into
    // dashboardStack. So focus starts on dashboardStack (now index 2) for
    // a single board, restoring Phase 2's "keys reach the dashboard
    // immediately" behavior. With 2+ boards, focus starts on switcherBar
    // (now index 1) as already established and verified in Phase 2 —
    // arrow keys need to reach the switcher first so the user can move
    // between boards. globalMenuBar (index 0) is never the default in
    // either case — Quit/Setup are reachable via explicit Tab/Shift-Tab,
    // not the first thing a keypress lands on.
    auto mainSelected = std::make_shared<int>(boards.size() > 1 ? 1 : 2);

    MenuOption switcherOpt = MenuOption::Vertical();
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

    auto globalMenuBar = Container::Horizontal({globalQuit, globalSetup, globalPreferences});

    auto mainContainer = Container::Vertical({globalMenuBar, switcherBar, dashboardStack}, mainSelected.get());
    // Capture boardNames/activeBoard, not just switcherBar/dashboardStack —
    // Menu()/Container::Tab() only hold raw pointers into them (FTXUI's
    // non-owning-pointer widget convention); losing the owning shared_ptrs
    // here was a real use-after-free, found live via gdb during Phase 2
    // (std::length_error inside MenuBase::Clamp() reading a corrupted
    // vector size from freed memory) — see that fix's commit for the full
    // diagnosis. The lesson generalizes: anything Menu/Tab is given a raw
    // pointer into must outlive this returned Component, which this closure
    // is what accomplishes. mainSelected is captured for the identical
    // reason: Container::Vertical's selector overload holds a raw int*
    // into it.
    // globalMenuBar renders here as its own row only when 2+ boards exist
    // — with exactly one board, board_dashboard.h's own Renderer instead
    // calls Render() on these same globalQuit/globalSetup Components,
    // folding their output into its own menu row (a second render call
    // site for the same Components — safe, since rendering is stateless
    // per call; only *parenting*, unchanged here, is singular). This is
    // the visual-only single-row merge described in
    // docs/superpowers/specs/2026-07-21-mode-architecture-design.md.
    //
    // switcherBar renders as an unlabeled vertical sidebar (nicknames
    // only) to the left of the active board's dashboard — deliberately
    // not marked `| flex`, so it sizes to its own content width instead
    // of splitting the terminal 50/50 with dashboardStack, which is the
    // one that gets the remaining space.
    auto root = Renderer(mainContainer, [switcherBar, dashboardStack, globalMenuBar, boardNames, activeBoard, mainSelected] {
        bool showBar = boardNames->size() > 1;
        if (!showBar) {
            return vbox({ dashboardStack->Render() | flex });
        }
        return vbox({
            globalMenuBar->Render(),
            separator(),
            hbox({
                switcherBar->Render(),
                separator(),
                dashboardStack->Render() | flex,
            }) | flex,
        });
    });

    auto attachBoard = [boardNames, dashboardStack](const std::string& nickname, Component dashboard) {
        boardNames->push_back(nickname);
        dashboardStack->Add(std::move(dashboard));
    };

    // Symmetric to attachBoard. FTXUI's Container::Tab indexes its active
    // child via `*selector_ % children_.size()` (confirmed in the vendored
    // source, container.cpp) — not a clamp, a modulo — so after an erase,
    // activeBoard needs explicit adjustment or it can silently jump to an
    // unrelated board rather than either staying on the same one or landing
    // on a sensible neighbor. The two adjustments below cover every case:
    // if the removed tab was before the active one, decrement to keep
    // tracking the same logical board (which just shifted down one index);
    // then clamp into the new valid range (handles the removed tab being
    // the active one, especially if it was also the last slot).
    //
    // mainSelected also needs attention: shrinking to <=1 board makes the
    // switcher bar invisible again (see root's Renderer below), and if
    // mainSelected were still pointing at switcherBar (index 0), keyboard
    // input would land on an invisible single-entry Menu — the exact class
    // of bug already found and fixed for single-board startup (see
    // mainSelected's own comment above). Forcing it back to dashboardStack
    // (index 1) whenever the board count drops to <=1 prevents recreating
    // that regression via removal instead of via startup.
    auto detachBoard = [boardNames, dashboardStack, activeBoard, mainSelected](const std::string& nickname) {
        for (size_t i = 0; i < boardNames->size(); ++i) {
            if ((*boardNames)[i] != nickname) continue;
            dashboardStack->ChildAt(i)->Detach();
            boardNames->erase(boardNames->begin() + i);
            int removedIdx = static_cast<int>(i);
            if (*activeBoard > removedIdx) --*activeBoard;
            if (*activeBoard >= static_cast<int>(boardNames->size()))
                *activeBoard = std::max(0, static_cast<int>(boardNames->size()) - 1);
            if (boardNames->size() <= 1) *mainSelected = 2;
            return;
        }
    };

    return BoardSwitcher{root, attachBoard, detachBoard};
}

} // namespace psb::tui
