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

inline std::string boardChannelId(const std::string& boardNickname, int channelIndex) {
    return boardNickname + "/" + psb::defaultChannelAlias(channelIndex);
}

inline int findGroupForBoardChannel(const psb::TopologyConfig& topo,
                                    const std::string& boardNickname,
                                    int channelIndex) {
    for (int gi = 0; gi < static_cast<int>(topo.groups.size()); ++gi) {
        for (const auto& c : topo.groups[gi].channels)
            if (c.boardNickname == boardNickname && c.channelIndex == channelIndex)
                return gi;
    }
    return -1;
}

inline bool groupAliasInUse(const psb::GroupConfig& group,
                            const std::string& alias,
                            int exceptChannelIdx = -1) {
    for (int i = 0; i < static_cast<int>(group.channels.size()); ++i)
        if (i != exceptChannelIdx && group.channels[i].alias == alias) return true;
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
                                     const std::string& boardNickname,
                                     int channelIndex,
                                     const std::string& alias) {
    if (groupIdx < 0 || groupIdx >= static_cast<int>(s.topo.groups.size()))
        return "invalid group index";
    if (boardNickname.empty()) return "board required";
    if (channelIndex < 0) return "invalid channel index";
    std::string finalAlias = alias.empty() ? psb::defaultChannelAlias(channelIndex) : alias;
    int assignedGroup = findGroupForBoardChannel(s.topo, boardNickname, channelIndex);
    if (assignedGroup >= 0)
        return boardChannelId(boardNickname, channelIndex) + " already assigned to group " +
               s.topo.groups[assignedGroup].name;
    auto& group = s.topo.groups[groupIdx];
    if (groupAliasInUse(group, finalAlias))
        return "alias \"" + finalAlias + "\" already in use in group " + group.name;

    psb::GroupChannelRef ref;
    ref.boardNickname = boardNickname;
    ref.channelIndex = channelIndex;
    ref.alias = finalAlias;
    group.channels.push_back(std::move(ref));
    s.dirty = true;
    return "";
}

inline std::string renameGroupChannelAlias(GroupWizardState& s, int groupIdx,
                                           int channelIdx,
                                           const std::string& alias) {
    if (groupIdx < 0 || groupIdx >= static_cast<int>(s.topo.groups.size()))
        return "invalid group index";
    auto& group = s.topo.groups[groupIdx];
    if (channelIdx < 0 || channelIdx >= static_cast<int>(group.channels.size()))
        return "invalid channel index";
    const auto& ref = group.channels[channelIdx];
    std::string finalAlias = alias.empty() ? psb::defaultChannelAlias(ref.channelIndex) : alias;
    if (groupAliasInUse(group, finalAlias, channelIdx))
        return "alias \"" + finalAlias + "\" already in use in group " + group.name;
    group.channels[channelIdx].alias = finalAlias;
    s.dirty = true;
    return "";
}

inline std::string renameGroupChannelAliasForBoardChannel(psb::TopologyConfig& topo,
                                                          const std::string& boardNickname,
                                                          int channelIndex,
                                                          const std::string& alias) {
    int groupIdx = findGroupForBoardChannel(topo, boardNickname, channelIndex);
    if (groupIdx < 0)
        return boardChannelId(boardNickname, channelIndex) + " is not assigned to a group";
    auto& group = topo.groups[groupIdx];
    for (int channelIdx = 0; channelIdx < static_cast<int>(group.channels.size()); ++channelIdx) {
        const auto& ref = group.channels[channelIdx];
        if (ref.boardNickname == boardNickname && ref.channelIndex == channelIndex) {
            std::string finalAlias = alias.empty() ? psb::defaultChannelAlias(channelIndex) : alias;
            if (groupAliasInUse(group, finalAlias, channelIdx))
                return "alias \"" + finalAlias + "\" already in use in group " + group.name;
            group.channels[channelIdx].alias = finalAlias;
            return "";
        }
    }
    return boardChannelId(boardNickname, channelIndex) + " is not assigned to a group";
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
