#pragma once

#include "board_session.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include <algorithm>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace psb::tui {

using namespace ftxui;

// The switcher's root Component (always Menu + Container::Tab, never just a
// bare dashboard) plus live-append hooks for both boards and groups. Building
// it this way — instead of Phase 2's "collapse to a bare dashboard when
// there's only one board" — means a board or group can be attached after
// construction without rebuilding or swapping the root: Container::Tab and
// Menu both already support dynamic children (ComponentBase::Add; Menu
// re-reads its backing vector's live size every render), so appending is
// enough. Pixel-identical single-board rendering (Phase 2's Global
// Constraint) is preserved by omitting the switcher bar *element* — not the
// underlying component — whenever fewer than two boards exist and no groups
// are defined.
struct BoardSwitcher {
    Component root;
    std::function<void(const std::string& nickname, Component dashboard)> attachBoard;
    std::function<void(const std::string& nickname)> detachBoard;
    std::function<void(const std::string& name, Component dashboard)> attachGroup;
    std::function<void(const std::string& name)> detachGroup;
    // Switches the shared selection to `nickname`'s board and sets that
    // board's own activeTab to channelIndex's Channel tab (1 + channelIndex,
    // since tabTitles[0] is always "Monitor" — see board_session.h's own
    // activeTab convention). A no-op if nickname isn't currently attached.
    std::function<void(const std::string& nickname, int channelIndex)> jumpToBoard;
};

// Design note — why two independent Container::Tabs, not one shared index:
// the design spec's own architecture sketch describes "one flat index space,
// groups-first-then-boards" driving a single Container::Tab. Implementing
// that literally — passing the *same* int* to both the groups Menu and the
// boards Menu — does not work with FTXUI's actual Menu semantics:
// MenuBase::Render() calls Clamp() on every render, which unconditionally
// clamps the shared selector into [0, thisMenu'sOwnSize() - 1] (confirmed in
// the vendored source, menu.cpp). With a shared index space, whichever Menu
// renders *last* on a given frame would silently clamp the shared value back
// into its own section's range even while the other section is genuinely
// showing — corrupting the "one shared active selection" behavior the
// design actually calls for. Instead: two independent Container::Tabs
// (groups, boards), each with its own local Menu selector, plus one
// showingGroup flag deciding which stack's Render() the outer view actually
// shows. Each Menu's on_change (confirmed in the vendored source to fire
// only from OnEvent() — user-driven arrow/click — never from
// Render()/Clamp(), so this is safe) updates showingGroup when the user
// picks an entry in that section. This still delivers the spec's functional
// requirement — one shared active selection, whichever was clicked last
// wins — with substantially less risk than fighting Menu's per-widget
// clamping.
inline BoardSwitcher makeBoardSwitcher(std::vector<std::unique_ptr<BoardSession>>& boards,
                                       ScreenInteractive& screen,
                                       Component globalQuit, Component globalSetup, Component globalGroup,
                                       Component globalPreferences,
                                       Component globalConnectAll, Component globalDisconnectAll) {
    // ---- Boards section (bottom) ----
    auto boardNames = std::make_shared<std::vector<std::string>>();
    for (auto& b : boards) boardNames->push_back(b->nickname);
    auto boardLocalIdx = std::make_shared<int>(0);

    // ---- Groups section (top) ----
    auto groupNames = std::make_shared<std::vector<std::string>>();
    auto groupLocalIdx = std::make_shared<int>(0);

    // Which section currently owns the shared active selection — see this
    // file's own design note above for why this replaces a literal shared
    // index. Starts on boards (false) since groups are always attached after
    // this constructor returns (main.cpp's refreshGroupDashboards, called
    // right after makeBoardSwitcher) — a user who already has groups defined
    // can still reach them with one Tab/Shift-Tab press.
    auto showingGroup = std::make_shared<bool>(false);

    MenuOption switcherOpt = MenuOption::Vertical();
    // Looks up e.label (the nickname) in `boards` to read that board's live
    // connection state — Menu only ever hands the transform a label/active/
    // focused triple, never an index or the BoardSession itself, so a
    // by-nickname scan is the only way in. Boards are few in practice
    // (single digits), and this mirrors detachBoard's own by-nickname
    // lookup below. `boards` is `rt.boards` from main.cpp, captured by
    // reference the same way main.cpp's own Connect All/Disconnect All
    // buttons already capture it — safe without an extra lock: mutations to
    // the vector itself (add/remove) are, like this transform's reads, only
    // ever issued from the single UI thread, so they can't interleave with
    // this scan.
    //
    // Five states, reusing the exact fields/thresholds the per-board
    // dashboard's own status dot (board_dashboard.h) already computes, so
    // the two never disagree about what "connected" or "stale" means:
    //   green  ● : connected, polling normal
    //   red    ■ : connected, but data gone stale (polling failed)
    //   yellow ◐ : connecting (static glyph — no redraw ticker exists
    //              during an in-flight connect attempt for this to animate)
    //   red    ■ : not connected/connecting, last status was an error
    //              (connection failed)
    //   gray   ○ : not connected/connecting, no error (idle / never
    //              connected) — the loop's starting value below
    //
    // e.active reflects only this Menu's own local selector (boardLocalIdx)
    // — overridden to false whenever the groups section currently owns the
    // shared selection, so at most one row across both sections ever shows
    // as "the" active one.
    switcherOpt.entries_option.transform = [&boards, showingGroup](const EntryState& e) -> Element {
        Element dot = text("\xe2\x97\x8b ") | color(Color::GrayDark);  // ○
        for (auto& b : boards) {
            if (b->nickname != e.label) continue;
            bool connected = b->connected.load();
            bool connecting = b->connecting.load();
            bool stale = connected && b->data.sysStale(kSysStaleThreshold);
            if (connected && !stale) {
                dot = text("\xe2\x97\x8f ") | color(Color::Green);   // ●
            } else if (connected && stale) {
                dot = text("\xe2\x96\xa0 ") | color(Color::Red);     // ■
            } else if (connecting) {
                dot = text("\xe2\x97\x90 ") | color(Color::Yellow);  // ◐
            } else {
                std::lock_guard<std::mutex> lk(b->statusMutex);
                if (b->statusMsg.find("Error") != std::string::npos)
                    dot = text("\xe2\x96\xa0 ") | color(Color::Red); // ■
            }
            break;
        }
        bool active = e.active && !*showingGroup;
        auto t = hbox({ text("  "), dot, text(e.label + "  ") });
        if (active)                  t = t | bold | color(Color::Cyan);
        if (e.focused && !active)    t = t | inverted;
        if (!active && !e.focused)   t = t | dim;
        return t;
    };
    switcherOpt.on_change = [showingGroup] { *showingGroup = false; };
    auto switcherBar = Menu(boardNames.get(), boardLocalIdx.get(), switcherOpt);

    MenuOption groupOpt = MenuOption::Vertical();
    groupOpt.entries_option.transform = [showingGroup](const EntryState& e) -> Element {
        bool active = e.active && *showingGroup;
        auto t = hbox({ text("  "), text(e.label + "  ") });
        if (active)                  t = t | bold | color(Color::Cyan);
        if (e.focused && !active)    t = t | inverted;
        if (!active && !e.focused)   t = t | dim;
        return t;
    };
    groupOpt.on_change = [showingGroup] { *showingGroup = true; };
    auto groupMenu = Menu(groupNames.get(), groupLocalIdx.get(), groupOpt);

    auto boardDashboardStack = Container::Tab({}, boardLocalIdx.get());
    for (auto& b : boards) boardDashboardStack->Add(b->dashboard);
    auto groupDashboardStack = Container::Tab({}, groupLocalIdx.get());

    // Order matches the visual/Tab order the Renderer below produces
    // (Setup, Group, Preferences, then Connect All/Disconnect All/Quit
    // pushed to the right corner) — Container children order drives
    // keyboard Tab traversal even though the actual visual arrangement is
    // decided entirely in the Renderer, so keeping the two in sync avoids
    // Tab jumping in an order that doesn't match what's on screen.
    auto globalMenuBar = Container::Horizontal(
        {globalSetup, globalGroup, globalPreferences, globalConnectAll, globalDisconnectAll, globalQuit});

    // 4-slot scheme: 0=globalMenuBar, 1=groupMenu, 2=switcherBar,
    // 3=dashboardStack (whichever of boardDashboardStack/groupDashboardStack
    // is currently showing, decided by showingGroup — see the Renderer
    // below). Groups are always attached after this constructor returns
    // (main.cpp's refreshGroupDashboards, called right after this function)
    // — groupNames is provably empty here, so defaulting focus on it would
    // always be dead code. Default mirrors Phase 2's own reasoning instead
    // (a Menu with very few/zero entries unconditionally swallows
    // Event::Return and doesn't meaningfully handle Up/Down — see
    // MenuBase::OnEvent in the vendored FTXUI source): the boards Menu if
    // there's more than one board, else straight to the dashboard so keys
    // reach it immediately. A user who already has groups defined can
    // still reach them with one Tab/Shift-Tab press.
    auto mainSelected = std::make_shared<int>(boardNames->size() > 1 ? 2 : 3);

    auto mainContainer = Container::Vertical(
        {globalMenuBar, groupMenu, switcherBar, boardDashboardStack}, mainSelected.get());
    // Capture boardNames/groupNames/boardLocalIdx/groupLocalIdx, not just
    // switcherBar/groupMenu/*DashboardStack — Menu()/Container::Tab() only
    // hold raw pointers into them (FTXUI's non-owning-pointer widget
    // convention); losing the owning shared_ptrs here was a real
    // use-after-free, found live via gdb during Phase 2 (std::length_error
    // inside MenuBase::Clamp() reading a corrupted vector size from freed
    // memory) — see that fix's commit for the full diagnosis. The lesson
    // generalizes: anything Menu/Tab is given a raw pointer into must
    // outlive this returned Component, which this closure is what
    // accomplishes. mainSelected is captured for the identical reason:
    // Container::Vertical's selector overload holds a raw int* into it.
    //
    // globalMenuBar renders here as its own row only when the switcher
    // section is shown at all (2+ boards, or any groups) — with exactly one
    // board and no groups, board_dashboard.h's own Renderer instead calls
    // Render() on these same globalQuit/globalSetup Components, folding
    // their output into its own menu row (a second render call site for the
    // same Components — safe, since rendering is stateless per call; only
    // *parenting*, unchanged here, is singular). This is the visual-only
    // single-row merge described in
    // docs/superpowers/specs/2026-07-21-mode-architecture-design.md.
    auto root = Renderer(mainContainer, [switcherBar, groupMenu, boardDashboardStack, groupDashboardStack,
                                         globalSetup, globalGroup, globalPreferences,
                                         globalConnectAll, globalDisconnectAll, globalQuit,
                                         boardNames, groupNames, showingGroup, mainSelected] {
        bool showSwitcherSection = !groupNames->empty() || boardNames->size() > 1;
        Component activeStack = *showingGroup ? groupDashboardStack : boardDashboardStack;
        if (!showSwitcherSection) {
            return vbox({ activeStack->Render() | flex });
        }
        Elements sideParts;
        if (!groupNames->empty()) {
            sideParts.push_back(text(" Groups ") | dim);
            sideParts.push_back(groupMenu->Render());
            sideParts.push_back(separator());
        }
        sideParts.push_back(text(" Boards ") | dim);
        sideParts.push_back(switcherBar->Render());
        return vbox({
            hbox({
                globalSetup->Render(), text(" "), globalGroup->Render(), text(" "), globalPreferences->Render(),
                filler(),
                globalConnectAll->Render(), text(" "), globalDisconnectAll->Render(), text(" "),
                globalQuit->Render(),
            }),
            separator(),
            hbox({
                vbox(std::move(sideParts)),
                separator(),
                activeStack->Render() | flex,
            }) | flex,
        });
    });

    auto attachBoard = [boardNames, boardDashboardStack](const std::string& nickname, Component dashboard) {
        boardNames->push_back(nickname);
        boardDashboardStack->Add(std::move(dashboard));
    };

    // Symmetric to attachBoard. FTXUI's Container::Tab indexes its active
    // child via `*selector_ % children_.size()` (confirmed in the vendored
    // source, container.cpp) — not a clamp, a modulo — so after an erase,
    // boardLocalIdx needs explicit adjustment or it can silently jump to an
    // unrelated board rather than either staying on the same one or landing
    // on a sensible neighbor. The two adjustments below cover every case:
    // if the removed tab was before the active one, decrement to keep
    // tracking the same logical board (which just shifted down one index);
    // then clamp into the new valid range (handles the removed tab being
    // the active one, especially if it was also the last slot).
    auto detachBoard = [boardNames, boardDashboardStack, boardLocalIdx](const std::string& nickname) {
        for (size_t i = 0; i < boardNames->size(); ++i) {
            if ((*boardNames)[i] != nickname) continue;
            boardDashboardStack->ChildAt(i)->Detach();
            boardNames->erase(boardNames->begin() + i);
            int removedIdx = static_cast<int>(i);
            if (*boardLocalIdx > removedIdx) --*boardLocalIdx;
            if (*boardLocalIdx >= static_cast<int>(boardNames->size()))
                *boardLocalIdx = std::max(0, static_cast<int>(boardNames->size()) - 1);
            return;
        }
    };

    auto attachGroup = [groupNames, groupDashboardStack](const std::string& name, Component dashboard) {
        groupNames->push_back(name);
        groupDashboardStack->Add(std::move(dashboard));
    };

    // Same index-shift reasoning as detachBoard, retargeted at the groups
    // section. Falls back to the boards section (showingGroup = false) if
    // the group being removed was the one currently showing, or if this
    // was the last group — mirrors detachBoard's own "force a sane visible
    // section" adjustment (Phase 2's mainSelected-forcing precedent) rather
    // than leaving the view pointed at a section that's about to render
    // empty.
    auto detachGroup = [groupNames, groupDashboardStack, groupLocalIdx, showingGroup]
                       (const std::string& name) {
        for (size_t i = 0; i < groupNames->size(); ++i) {
            if ((*groupNames)[i] != name) continue;
            bool wasShowingThis = *showingGroup && (*groupLocalIdx == static_cast<int>(i));
            groupDashboardStack->ChildAt(i)->Detach();
            groupNames->erase(groupNames->begin() + i);
            int removedIdx = static_cast<int>(i);
            if (*groupLocalIdx > removedIdx) --*groupLocalIdx;
            if (*groupLocalIdx >= static_cast<int>(groupNames->size()))
                *groupLocalIdx = std::max(0, static_cast<int>(groupNames->size()) - 1);
            if (wasShowingThis || groupNames->empty()) *showingGroup = false;
            return;
        }
    };

    auto jumpToBoard = [&boards, boardNames, boardLocalIdx, showingGroup, mainSelected, &screen]
                       (const std::string& nickname, int channelIndex) {
        for (size_t i = 0; i < boardNames->size(); ++i) {
            if ((*boardNames)[i] != nickname) continue;
            *boardLocalIdx = static_cast<int>(i);
            *showingGroup = false;
            break;
        }
        for (auto& b : boards) {
            if (b->nickname != nickname) continue;
            int maxTab = static_cast<int>(b->tabTitles.size()) - 1;
            b->activeTab = std::min(1 + channelIndex, std::max(0, maxTab));
            break;
        }
        *mainSelected = 3;  // land keyboard focus on the dashboard itself
        screen.PostEvent(Event::Custom);
    };

    return BoardSwitcher{root, attachBoard, detachBoard, attachGroup, detachGroup, jumpToBoard};
}

} // namespace psb::tui
