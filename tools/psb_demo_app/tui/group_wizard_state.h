#pragma once

#include "topology_config.h"

#include <string>

namespace psb::tui {

// In-progress group edits, plus UI selection/status scratch — mirrors
// WizardState (wizard_state.h) exactly: hardware- and FTXUI-free so the
// mutators below are unit-testable without a bus, a board, or a terminal.
// `topo` is always seeded from the *full* live topology (buses + boards +
// aliases + groups), never a groups-only copy — group_wizard_screen.h's own
// Save writes this whole struct back to disk, and a groups-only copy would
// silently truncate the saved file's buses/boards, exactly the class of bug
// Phase 1's currentTopologyPath fix exists to prevent.
struct GroupWizardState {
    psb::TopologyConfig topo;
    std::string topologyPath;
    int selectedGroup = -1;    // index into topo.groups, -1 = none
    int selectedChannel = -1;  // index into topo.groups[selectedGroup].channels, -1 = none
    std::string statusMsg;
    bool dirty = false;
};

inline bool groupNameInUse(const psb::TopologyConfig& topo, const std::string& name) {
    for (const auto& g : topo.groups)
        if (g.name == name) return true;
    return false;
}

// Every mutator below: returns "" on success, a user-facing error message on
// failure; never throws; never touches hardware.

inline std::string addGroup(GroupWizardState& s, const std::string& name) {
    if (name.empty()) return "group name required";
    if (groupNameInUse(s.topo, name)) return "group name \"" + name + "\" already in use";
    psb::GroupConfig g;
    g.name = name;
    s.topo.groups.push_back(std::move(g));
    s.dirty = true;
    return "";
}

// Same known limitation as WizardState's removeBus/removeBoard: only clamps
// selectedGroup when it falls past the end after the erase, does not shift
// it when idx is strictly before the current selection. Harmless here too —
// group_wizard_screen.h only ever calls removeGroup(s, s.selectedGroup).
inline std::string removeGroup(GroupWizardState& s, int idx) {
    if (idx < 0 || idx >= static_cast<int>(s.topo.groups.size()))
        return "invalid group index";
    s.topo.groups.erase(s.topo.groups.begin() + idx);
    if (s.selectedGroup >= static_cast<int>(s.topo.groups.size()))
        s.selectedGroup = static_cast<int>(s.topo.groups.size()) - 1;
    s.selectedChannel = -1;
    s.dirty = true;
    return "";
}

inline std::string addChannelToGroup(GroupWizardState& s, int groupIdx,
                                     const std::string& boardNickname, int channelIndex) {
    if (groupIdx < 0 || groupIdx >= static_cast<int>(s.topo.groups.size()))
        return "invalid group index";
    if (boardNickname.empty()) return "board required";
    if (channelIndex < 0) return "invalid channel index";
    auto& channels = s.topo.groups[groupIdx].channels;
    for (const auto& c : channels)
        if (c.boardNickname == boardNickname && c.channelIndex == channelIndex)
            return "channel already in group";
    psb::GroupChannelRef ref;
    ref.boardNickname = boardNickname;
    ref.channelIndex = channelIndex;
    channels.push_back(std::move(ref));
    s.dirty = true;
    return "";
}

// Same known limitation as removeGroup above regarding selectedChannel shift.
inline std::string removeChannelFromGroup(GroupWizardState& s, int groupIdx, int channelIdx) {
    if (groupIdx < 0 || groupIdx >= static_cast<int>(s.topo.groups.size()))
        return "invalid group index";
    auto& channels = s.topo.groups[groupIdx].channels;
    if (channelIdx < 0 || channelIdx >= static_cast<int>(channels.size()))
        return "invalid channel index";
    channels.erase(channels.begin() + channelIdx);
    if (s.selectedChannel >= static_cast<int>(channels.size()))
        s.selectedChannel = static_cast<int>(channels.size()) - 1;
    s.dirty = true;
    return "";
}

} // namespace psb::tui
