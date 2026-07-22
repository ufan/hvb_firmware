#pragma once

#include "group_wizard_state.h"
#include "widgets.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace psb::tui {

using namespace ftxui;

// A currently-connected board, as the group wizard's "Add Channel" picker
// needs to see it — never sourced from topo.buses (which lists every board
// ever defined, connected or not); see makeGroupWizardScreen's GetLiveBoards
// parameter. aliases[ch] is "" when that channel has no alias set.
struct LiveBoardInfo {
    std::string nickname;
    int numChannels = 0;
    std::vector<std::string> aliases;
};
using LiveBoards = std::vector<LiveBoardInfo>;
using GetLiveBoards = std::function<LiveBoards()>;

// Falls back to "CHn" when no alias is set or the alias lookup can't find
// this channel (offline board) — the same display-fallback rule the Monitor/
// Channel tabs already use for tab titles (rebuildChannelTitles).
inline std::string groupChannelDisplayName(const GroupChannelRef& ref, const LiveBoards& live) {
    for (const auto& lb : live) {
        if (lb.nickname != ref.boardNickname) continue;
        if (ref.channelIndex >= 0 && ref.channelIndex < static_cast<int>(lb.aliases.size())
            && !lb.aliases[ref.channelIndex].empty())
            return lb.aliases[ref.channelIndex];
        break;
    }
    return "CH" + std::to_string(ref.channelIndex);
}

// Builds the Group wizard's Component — a group name list plus Add/Remove
// Group, a selected group's member-channel list plus Add/Remove Channel, and
// Save/Exit. Deliberately simpler than makeWizardScreen (wizard_screen.h):
// no scan sub-flow, and no ConnectNow/Back outcomes — this is always a
// mid-session-only modal (there is no pre-dashboard "launch flow" for
// groups, since a group needs boards to already be defined), so onFinish
// takes no arguments; it always means "close this modal."
//
// getLiveBoards is called fresh every time the Add Channel modal opens
// (mirroring the Topology wizard's own doScanPorts-on-open pattern), never
// cached across renders — a board can connect/disconnect at any time while
// this modal sits open.
inline Component makeGroupWizardScreen(GroupWizardState& s, ScreenInteractive& screen,
                                       std::function<void()> onFinish,
                                       GetLiveBoards getLiveBoards) {
    // ---- Group list (left pane) ----
    auto groupNames = std::make_shared<std::vector<std::string>>();
    auto rebuildGroupNames = [&s, groupNames] {
        groupNames->clear();
        for (const auto& g : s.topo.groups) groupNames->push_back(g.name);
    };
    rebuildGroupNames();
    auto groupMenu = Menu(groupNames.get(), &s.selectedGroup);

    // ---- Member channel list (right pane) ----
    // Rebuilt every render, not just on add/remove — a member's board can
    // connect/disconnect independently of any edit made here, and the
    // "(offline)" marker (Global Constraints) must reflect that live.
    auto channelLabels = std::make_shared<std::vector<std::string>>();
    auto rebuildChannelLabels = [&s, channelLabels, getLiveBoards] {
        channelLabels->clear();
        if (s.selectedGroup < 0 || s.selectedGroup >= static_cast<int>(s.topo.groups.size())) return;
        LiveBoards live = getLiveBoards();
        for (const auto& ref : s.topo.groups[s.selectedGroup].channels) {
            bool online = false;
            for (const auto& lb : live) if (lb.nickname == ref.boardNickname) { online = true; break; }
            std::string label = ref.boardNickname + " " + groupChannelDisplayName(ref, live);
            if (!online) label += " (offline)";
            channelLabels->push_back(label);
        }
    };
    rebuildChannelLabels();
    auto channelMenu = Menu(channelLabels.get(), &s.selectedChannel);

    // ---- Add Group modal ----
    auto newGroupName = std::make_shared<std::string>();
    auto showAddGroupPtr = std::make_shared<bool>(false);
    auto groupNameInp = Input(newGroupName.get(), "group name");
    auto bAddGroupConfirm = ActionButton("Add", [&s, newGroupName, rebuildGroupNames, showAddGroupPtr, &screen] {
        std::string err = addGroup(s, *newGroupName);
        s.statusMsg = err.empty() ? "" : "Error: " + err;
        if (err.empty()) {
            rebuildGroupNames();
            *showAddGroupPtr = false;
            newGroupName->clear();
        }
        screen.PostEvent(Event::Custom);
    });
    auto bAddGroupCancel = ActionButton("Cancel", [showAddGroupPtr, &screen] {
        *showAddGroupPtr = false; screen.PostEvent(Event::Custom);
    });
    auto addGroupForm = Container::Vertical({groupNameInp, bAddGroupConfirm, bAddGroupCancel});
    auto addGroupPopup = Renderer(addGroupForm, [groupNameInp, bAddGroupConfirm, bAddGroupCancel] {
        return vbox({
            text(" Add Group ") | bold | center, separator(),
            hbox({ text("Name : "), groupNameInp->Render() }),
            separator(),
            hbox({ bAddGroupConfirm->Render(), text("  "), bAddGroupCancel->Render() }) | center,
        }) | border | size(WIDTH, EQUAL, 40);
    });

    // ---- Add Channel modal (picker scoped to currently-connected boards,
    //      excluding channels already in the selected group — Global
    //      Constraints) ----
    auto channelPickerLabels = std::make_shared<std::vector<std::string>>();
    auto channelPickerRefs = std::make_shared<std::vector<GroupChannelRef>>();
    auto channelPickerIdx = std::make_shared<int>(-1);
    auto rebuildChannelPicker = [&s, channelPickerLabels, channelPickerRefs, channelPickerIdx, getLiveBoards] {
        channelPickerLabels->clear();
        channelPickerRefs->clear();
        if (s.selectedGroup < 0 || s.selectedGroup >= static_cast<int>(s.topo.groups.size())) return;
        const auto& existing = s.topo.groups[s.selectedGroup].channels;
        LiveBoards live = getLiveBoards();
        for (const auto& lb : live) {
            for (int ch = 0; ch < lb.numChannels; ++ch) {
                bool already = false;
                for (const auto& ref : existing)
                    if (ref.boardNickname == lb.nickname && ref.channelIndex == ch) { already = true; break; }
                if (already) continue;
                std::string label = lb.nickname + " CH" + std::to_string(ch);
                if (ch < static_cast<int>(lb.aliases.size()) && !lb.aliases[ch].empty())
                    label += " (" + lb.aliases[ch] + ")";
                channelPickerLabels->push_back(label);
                GroupChannelRef ref;
                ref.boardNickname = lb.nickname;
                ref.channelIndex = ch;
                channelPickerRefs->push_back(ref);
            }
        }
        if (*channelPickerIdx >= static_cast<int>(channelPickerLabels->size()))
            *channelPickerIdx = channelPickerLabels->empty() ? -1 : 0;
    };
    auto channelPickerMenu = Menu(channelPickerLabels.get(), channelPickerIdx.get());
    auto showAddChannelPtr = std::make_shared<bool>(false);

    auto bAddChannelConfirm = ActionButton("Add", [&s, channelPickerRefs, channelPickerIdx,
                                                    rebuildChannelPicker, rebuildChannelLabels, &screen] {
        int i = *channelPickerIdx;
        if (i < 0 || i >= static_cast<int>(channelPickerRefs->size())) return;
        const auto& ref = (*channelPickerRefs)[i];
        std::string err = addChannelToGroup(s, s.selectedGroup, ref.boardNickname, ref.channelIndex);
        s.statusMsg = err.empty() ? "" : "Error: " + err;
        if (err.empty()) {
            rebuildChannelPicker();
            rebuildChannelLabels();
        }
        screen.PostEvent(Event::Custom);
    });
    auto bAddChannelCancel = ActionButton("Cancel", [showAddChannelPtr, &screen] {
        *showAddChannelPtr = false; screen.PostEvent(Event::Custom);
    });
    auto addChannelForm = Container::Vertical({channelPickerMenu, bAddChannelConfirm, bAddChannelCancel});
    auto addChannelPopup = Renderer(addChannelForm, [channelPickerMenu, channelPickerLabels,
                                                      bAddChannelConfirm, bAddChannelCancel] {
        Element list = channelPickerLabels->empty()
            ? text("(no channels available — connect a board)") | dim
            : channelPickerMenu->Render() | frame | size(HEIGHT, LESS_THAN, 10);
        return vbox({
            text(" Add Channel ") | bold | center, separator(),
            list,
            separator(),
            hbox({ bAddChannelConfirm->Render(), text("  "), bAddChannelCancel->Render() }) | center,
        }) | border | size(WIDTH, EQUAL, 46);
    });

    // ---- List actions ----
    auto bAddGroup = ActionButton("Add Group", [showAddGroupPtr, &screen] {
        *showAddGroupPtr = true; screen.PostEvent(Event::Custom);
    });
    auto bRemoveGroup = ActionButton("Remove Group", [&s, rebuildGroupNames, &screen] {
        if (s.selectedGroup < 0) return;
        s.statusMsg = removeGroup(s, s.selectedGroup);
        rebuildGroupNames();
        screen.PostEvent(Event::Custom);
    });
    // Same "check range, not >= 0" reasoning as wizard_screen.h's
    // busInRange/boardInRange — MenuBase::Clamp() promotes an empty list's
    // -1 sentinel to 0 on every Render().
    auto groupInRange = [&s] { return s.selectedGroup >= 0 && s.selectedGroup < static_cast<int>(s.topo.groups.size()); };
    auto groupSelectable = Maybe(bRemoveGroup, groupInRange);

    auto bAddChannel = ActionButton("Add Channel", [showAddChannelPtr, rebuildChannelPicker, &screen] {
        rebuildChannelPicker();
        *showAddChannelPtr = true; screen.PostEvent(Event::Custom);
    });
    auto addChannelEnabled = Maybe(bAddChannel, groupInRange);
    auto bRemoveChannel = ActionButton("Remove Channel", [&s, rebuildChannelLabels, &screen] {
        if (s.selectedGroup < 0 || s.selectedChannel < 0) return;
        s.statusMsg = removeChannelFromGroup(s, s.selectedGroup, s.selectedChannel);
        rebuildChannelLabels();
        screen.PostEvent(Event::Custom);
    });
    auto channelInRange = [&s, groupInRange] {
        return groupInRange() && s.selectedChannel >= 0
            && s.selectedChannel < static_cast<int>(s.topo.groups[s.selectedGroup].channels.size());
    };
    auto channelSelectable = Maybe(bRemoveChannel, channelInRange);

    // ---- Save / Exit ----
    auto bSave = ActionButton("Save", [&s, &screen] {
        if (s.topo.save(s.topologyPath)) {
            s.dirty = false;
            s.statusMsg = "Saved to " + s.topologyPath;
        } else {
            s.statusMsg = "Error: could not save to " + s.topologyPath;
        }
        screen.PostEvent(Event::Custom);
    });
    auto bExit = ActionButton("Exit", [onFinish] { onFinish(); });

    Components mainChildren = {
        groupMenu, bAddGroup, groupSelectable,
        channelMenu, addChannelEnabled, channelSelectable,
        bSave, bExit,
    };
    auto mainContainer = Container::Vertical(mainChildren);

    auto root = Renderer(mainContainer, [&s, groupMenu, bAddGroup, groupSelectable,
                                         channelMenu, addChannelEnabled, channelSelectable,
                                         bSave, bExit, rebuildGroupNames, rebuildChannelLabels] {
        // Re-derive both lists from s.topo on every render — s.topo can be
        // reseeded from outside this file (main.cpp's openGroupSetup) at any
        // time, same reasoning as wizard_screen.h's own rebuildBusNames/
        // rebuildBoardNames calls at the top of its Renderer.
        rebuildGroupNames();
        rebuildChannelLabels();
        return vbox({
            text(" Group Wizard " + std::string(s.dirty ? "*" : "") + " ") | bold | center,
            separator(),
            hbox({
                vbox({ text("Groups") | bold, groupMenu->Render() | frame | flex,
                       hbox({ bAddGroup->Render(), text(" "), groupSelectable->Render() }) }) | flex | border,
                vbox({ text("Channels") | bold, channelMenu->Render() | frame | flex,
                       hbox({ addChannelEnabled->Render(), text(" "), channelSelectable->Render() }) }) | flex | border,
            }) | flex,
            separator(),
            text(" " + s.statusMsg + " ") | (s.statusMsg.rfind("Error", 0) == 0 ? color(Color::Red) : color(Color::Green)),
            separator(),
            hbox({ bSave->Render(), text("  "), bExit->Render() }) | center,
        }) | border | size(WIDTH, GREATER_THAN, 80) | size(HEIGHT, GREATER_THAN, 24);
    }) | Modal(addGroupPopup, showAddGroupPtr.get())
       | Modal(addChannelPopup, showAddChannelPtr.get());

    return root;
}

} // namespace psb::tui
