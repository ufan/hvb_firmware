#pragma once

#include "app_window_layout.h"
#include "board_session.h"
#include "group_view_selection.h"
#include "tool_version.h"
#include "tui_style.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include <algorithm>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
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
// enough. The top-level menu bar is always rendered here, even for a single
// board, so app-level actions never get visually folded into a board's own
// toolbar.
struct BoardSwitcher {
    Component root;
    std::function<void(const std::string& nickname, Component dashboard)> attachBoard;
    std::function<void(const std::string& nickname)> detachBoard;
    std::function<void(const std::string& name, Component dashboard)> attachGroup;
    std::function<void(const std::string& name)> detachGroup;
    std::function<void(std::vector<std::pair<std::string, Component>> groups)> replaceGroups;
    // Switches the shared selection to `nickname`'s board and sets that
    // board's own activeTab to channelIndex's Channel tab (1 + channelIndex,
    // since tabTitles[0] is always "Monitor" — see board_session.h's own
    // activeTab convention). A no-op if nickname isn't currently attached.
    std::function<void(const std::string& nickname, int channelIndex)> jumpToBoard;
    std::function<void(const std::string& groupName, int memberIndex)> jumpToGroup;
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
// (groups, boards), each with its own local Menu selector.
//
// Design note — why "which section is showing" is derived from mainSelected,
// not from Menu's own on_change: verified live (real hardware, a single
// group) that MenuBase::OnMouseEvent only calls OnChange() when the click
// changes *this Menu's own* selected index (`if (selected() != i)`) — with
// exactly one entry, its index is already 0 by default, so the very first
// click on it never fires on_change at all, and the group section silently
// never appeared. What *does* fire unconditionally on every click, mouse or
// keyboard, is ComponentBase::TakeFocus() (confirmed in the vendored
// source, component.cpp) walking every ancestor and calling
// SetActiveChild() — which is exactly what updates mainContainer's own
// mainSelected to the slot that was just interacted with. Deriving
// showingGroup from mainSelected at the top of the Renderer, every frame,
// means it's always correct regardless of whether a click happened to also
// change a Menu's own local index.
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

    // Which section currently owns the shared active selection — updated at
    // the top of root's Renderer every frame, derived from mainSelected (see
    // this file's own design note above). Starts on boards (false) since
    // groups are always attached after this constructor returns (main.cpp's
    // refreshGroupDashboards, called right after makeBoardSwitcher).
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
    // Dropping bare hover motion (Mouse::None) before it ever reaches the
    // Menu — MenuBase::OnMouseEvent (vendored source, menu.cpp) calls
    // TakeFocus() unconditionally for ANY mouse event landing on a row's
    // box, including a hover motion with no button held. Left un-filtered,
    // that TakeFocus() call propagates up to mainContainer's own
    // mainSelected (see this file's design note above), which the outer
    // Renderer's showingGroup derivation reads — so merely moving the
    // mouse over this list, with no click at all, silently switched which
    // section's dashboard was displayed. This app is mouse-driven, not
    // keyboard-driven (per user direction), so that switch must only ever
    // happen on an explicit click. A real click's Press/Released events
    // both carry button == Left, never None, so clicks pass through
    // untouched; only genuine no-button motion is dropped here.
    auto dropHoverMotion = [](Event e) {
        return e.is_mouse() && e.mouse().button == Mouse::None;
    };
    auto switcherBar = CatchEvent(Menu(boardNames.get(), boardLocalIdx.get(), switcherOpt), dropHoverMotion);

    MenuOption groupOpt = MenuOption::Vertical();
    groupOpt.entries_option.transform = [showingGroup](const EntryState& e) -> Element {
        bool active = e.active && *showingGroup;
        auto t = hbox({ text("  "), text(e.label + "  ") });
        if (active)                  t = t | bold | color(Color::Cyan);
        if (e.focused && !active)    t = t | inverted;
        if (!active && !e.focused)   t = t | dim;
        return t;
    };
    auto groupMenu = CatchEvent(Menu(groupNames.get(), groupLocalIdx.get(), groupOpt), dropHoverMotion);

    auto boardDashboardStack = Container::Tab({}, boardLocalIdx.get());
    for (auto& b : boards) boardDashboardStack->Add(b->dashboard);
    auto groupDashboardStack = Container::Tab({}, groupLocalIdx.get());
    auto visibleContentIdx = std::make_shared<int>(0);
    auto visibleContentStack = Container::Tab({boardDashboardStack, groupDashboardStack}, visibleContentIdx.get());

    // Order matches the visual/Tab order the Renderer below produces
    // (Setup, Group, Preferences, then Connect All/Disconnect All/Quit
    // pushed to the right corner) — Container children order drives
    // keyboard Tab traversal even though the actual visual arrangement is
    // decided entirely in the Renderer, so keeping the two in sync avoids
    // Tab jumping in an order that doesn't match what's on screen.
    auto globalMenuBar = Container::Horizontal(
        {globalSetup, globalGroup, globalPreferences, globalConnectAll, globalDisconnectAll, globalQuit});

    // 4-slot scheme: 0=globalMenuBar, 1=groupMenu, 2=switcherBar,
    // 3=visibleContentStack. The board and group dashboard stacks sit behind
    // one visible-content Tab, not directly beside each other in
    // mainContainer. FTXUI keeps reflected mouse boxes on components after
    // they stop rendering; keeping both dashboard stacks as mainContainer
    // siblings lets a previously rendered board dashboard steal clicks from
    // the currently visible group dashboard. The extra Tab makes event
    // routing match the visible dashboard exactly. Groups are always
    // attached after this
    // constructor returns (main.cpp's refreshGroupDashboards, called right
    // after this function) — groupNames is provably empty here, so
    // defaulting focus on it would always be dead code. Default mirrors
    // Phase 2's own reasoning instead (a Menu with very few/zero entries
    // unconditionally swallows Event::Return and doesn't meaningfully
    // handle Up/Down — see MenuBase::OnEvent in the vendored FTXUI source):
    // the boards Menu if there's more than one board, else straight to the
    // board dashboard so keys reach it immediately. A user who already has
    // groups defined can still reach them with one Tab/Shift-Tab press.
    auto mainSelected = std::make_shared<int>(boardNames->size() > 1 ? 2 : 3);

    auto mainContainer = Container::Vertical(
        {globalMenuBar, groupMenu, switcherBar, visibleContentStack}, mainSelected.get());
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
    // globalMenuBar is the only render site for app-level actions. Keep it
    // outside the board dashboard in every mode so Topology/Group/
    // Preferences/Connect All/Disconnect All/Quit never appear as board
    // controls and never duplicate when a single board also has groups.
    auto root = Renderer(mainContainer, [switcherBar, groupMenu, visibleContentStack,
                                         globalSetup, globalGroup, globalPreferences,
                                         globalConnectAll, globalDisconnectAll, globalQuit,
                                         boardNames, groupNames, showingGroup, mainSelected, visibleContentIdx] {
        // Derive showingGroup from mainSelected every frame — see this
        // file's own design note near the top for why this, not Menu's own
        // on_change, is the source of truth. mainSelected == 0 (the global
        // menu bar) intentionally leaves showingGroup untouched: clicking
        // Topology/Group/Preferences/Quit says nothing about which section
        // should be displayed underneath.
        if (*mainSelected == 1) *showingGroup = true;
        else if (*mainSelected == 2) *showingGroup = false;
        else if (*mainSelected == 3) *showingGroup = *visibleContentIdx == 1;

        bool showSwitcherSection = !groupNames->empty() || boardNames->size() > 1;
        bool showAggregateConnectionActions = boardNames->size() > 1;
        *visibleContentIdx = *showingGroup ? 1 : 0;
        auto appTitleEl = appTitleChrome(text(std::string(" PSB Demo TUI (") + TOOL_VERSION_STRING + ") ")) | center;
        Elements globalMenuButtons = {
            globalSetup->Render(), text(" "), globalGroup->Render(), text(" "), globalPreferences->Render(),
            filler(),
        };
        if (showAggregateConnectionActions) {
            globalMenuButtons.push_back(globalConnectAll->Render());
            globalMenuButtons.push_back(text(" "));
            globalMenuButtons.push_back(globalDisconnectAll->Render());
            globalMenuButtons.push_back(text(" "));
        }
        globalMenuButtons.push_back(globalQuit->Render());
        auto globalMenuButtonsEl = appMenuChrome(hbox(std::move(globalMenuButtons)));
        auto globalMenuBarEl = dbox({globalMenuButtonsEl, appTitleEl});
        if (!showSwitcherSection) {
            return vbox({
                globalMenuBarEl,
                separatorDouble(),
                visibleContentStack->Render() | flex,
            }) | size(WIDTH, GREATER_THAN, appWindowMinWidthColumns());
        }
        // Title bold (not dim — a section title should stand out, the
        // opposite of Menu's own dim-when-unselected row styling) plus a
        // separator() between title and list, both spatially and visually
        // distinguishing the title from its items without introducing a
        // decorative convention (surrounding symbols, etc.) not used
        // anywhere else in this codebase — mirrors the existing "Buses"/
        // "Boards" pane-title convention in wizard_screen.h. Each section's
        // own list is wrapped in `frame` (scrollable once it overflows its
        // box, the same idiom wizard_screen.h's own Buses/Boards panes
        // already use) and the section's whole vbox is marked `flex` so the
        // two sections share the sidebar's available height equally, each
        // scrolling independently rather than one crowding out the other.
        Elements sideParts;
        if (!groupNames->empty()) {
            sideParts.push_back(vbox({
                sidebarTitleChrome(text(" Groups ")),
                separator(),
                groupMenu->Render() | frame | flex,
            }) | flex);
            sideParts.push_back(separator());
        }
        sideParts.push_back(vbox({
            sidebarTitleChrome(text(" Boards ")),
            separator(),
            switcherBar->Render() | frame | flex,
        }) | flex);
        return hbox({
            sidebarChrome(vbox({
                text(""),
                separatorDouble(),
                vbox(std::move(sideParts)) | flex,
            })),
            separator(),
            vbox({
                globalMenuBarEl,
                separatorDouble(),
                visibleContentStack->Render() | flex,
            }) | flex,
        }) | flex | size(WIDTH, GREATER_THAN, appWindowMinWidthColumns());
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
    // section. Falls back to the boards section (mainSelected = 2) if the
    // group being removed was the one currently showing, or if this was the
    // last group — mirrors detachBoard's own "force a sane visible section"
    // precedent (Phase 2's mainSelected-forcing for the boards side) rather
    // than leaving the view pointed at a section that's about to render
    // empty. Setting mainSelected (not showingGroup directly) keeps
    // mainSelected as the single source of truth the Renderer's own
    // derivation reads from.
    auto detachGroup = [groupNames, groupDashboardStack, groupLocalIdx, showingGroup, mainSelected]
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
            if (wasShowingThis || groupNames->empty()) *mainSelected = 2;
            return;
        }
    };

    auto replaceGroups = [groupNames, groupDashboardStack, groupLocalIdx, showingGroup,
                          mainSelected, visibleContentIdx]
                         (std::vector<std::pair<std::string, Component>> groups) {
        std::vector<std::string> oldGroups = *groupNames;
        bool wasShowingGroup = *showingGroup;
        int oldGroupIdx = *groupLocalIdx;

        while (!groupNames->empty()) {
            groupDashboardStack->ChildAt(0)->Detach();
            groupNames->erase(groupNames->begin());
        }

        std::vector<std::string> newGroups;
        for (auto& group : groups) {
            newGroups.push_back(group.first);
            groupNames->push_back(std::move(group.first));
            groupDashboardStack->Add(std::move(group.second));
        }

        reconcileGroupViewAfterReplacement(oldGroups, newGroups, wasShowingGroup, oldGroupIdx,
                                           *groupLocalIdx, *showingGroup,
                                           *mainSelected, *visibleContentIdx);
    };

    auto jumpToBoard = [&boards, boardNames, boardLocalIdx, showingGroup, mainSelected, visibleContentIdx, &screen]
                       (const std::string& nickname, int channelIndex) {
        for (size_t i = 0; i < boardNames->size(); ++i) {
            if ((*boardNames)[i] != nickname) continue;
            *boardLocalIdx = static_cast<int>(i);
            break;
        }
        for (auto& b : boards) {
            if (b->nickname != nickname) continue;
            // Deliberately NOT clamped against b->tabTitles.size() here — a
            // board's own tabTitles only gets rebuilt to its real,
            // fully-scanned size inside makeBoardDashboard's own Renderer
            // (consuming pendingSync), which only runs while that specific
            // board's dashboard is the one actually being displayed. A
            // board that finished connecting while the user was viewing a
            // *different* board or a group table the whole time still has
            // tabTitles stuck at its initial {"Monitor"} (size 1) even
            // though its ScannedData is already fully populated by
            // background polling — clamping against that stale size here
            // would silently force this jump back to Monitor instead of the
            // requested channel. Bounding against MAX_CHANNELS instead is a
            // static, always-correct safety bound; the target board's own
            // Renderer rebuilds tabTitles to match this activeTab (or clamps
            // it down only if it's genuinely out of range) the moment it
            // next runs — which happens on the very next frame, since this
            // jump is about to make that board the active one.
            b->activeTab = std::max(0, std::min(1 + channelIndex, MAX_CHANNELS));
            break;
        }
        *showingGroup = false;
        *visibleContentIdx = 0;
        *mainSelected = 3;  // land keyboard focus on the board dashboard itself
        screen.PostEvent(Event::Custom);
    };

    auto jumpToGroup = [groupNames, groupLocalIdx, showingGroup, mainSelected, visibleContentIdx, &screen]
                       (const std::string& groupName, int /*memberIndex*/) {
        for (size_t i = 0; i < groupNames->size(); ++i) {
            if ((*groupNames)[i] != groupName) continue;
            *groupLocalIdx = static_cast<int>(i);
            break;
        }
        *showingGroup = true;
        *visibleContentIdx = 1;
        *mainSelected = 3;
        screen.PostEvent(Event::Custom);
    };

    return BoardSwitcher{root, attachBoard, detachBoard, attachGroup, detachGroup, replaceGroups,
                         jumpToBoard, jumpToGroup};
}

} // namespace psb::tui
