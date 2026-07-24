#include "topology_rules.h"

#include <utility>

namespace psb {

bool boardNicknameInUse(const TopologyConfig& topo, const std::string& nickname) {
    for (const auto& bus : topo.buses)
        for (const auto& board : bus.boards)
            if (board.nickname == nickname) return true;
    return false;
}

bool slaveIdInUse(const BusConfig& bus, int slaveId) {
    for (const auto& board : bus.boards)
        if (board.slaveId == slaveId) return true;
    return false;
}

bool groupNameInUse(const TopologyConfig& topo, const std::string& name) {
    for (const auto& group : topo.groups)
        if (group.name == name) return true;
    return false;
}

std::string boardChannelId(const std::string& boardNickname, int channelIndex) {
    return boardNickname + "/" + defaultChannelAlias(channelIndex);
}

int findGroupForBoardChannel(const TopologyConfig& topo,
                             const std::string& boardNickname,
                             int channelIndex) {
    for (int groupIdx = 0; groupIdx < static_cast<int>(topo.groups.size()); ++groupIdx) {
        for (const auto& channel : topo.groups[groupIdx].channels) {
            if (channel.boardNickname == boardNickname && channel.channelIndex == channelIndex)
                return groupIdx;
        }
    }
    return -1;
}

bool groupAliasInUse(const GroupConfig& group,
                     const std::string& alias,
                     int exceptChannelIdx) {
    for (int channelIdx = 0; channelIdx < static_cast<int>(group.channels.size()); ++channelIdx) {
        if (channelIdx != exceptChannelIdx && group.channels[channelIdx].alias == alias)
            return true;
    }
    return false;
}

std::string addBus(TopologyConfig& topo,
                   const std::string& name,
                   const std::string& port,
                   int baud) {
    if (port.empty()) return "port required";
    for (const auto& bus : topo.buses)
        if (bus.port == port) return "port already in use by bus \"" + bus.name + "\"";

    BusConfig bus;
    bus.name = name.empty() ? ("bus" + std::to_string(topo.buses.size() + 1)) : name;
    bus.port = port;
    bus.baudRate = baud;
    topo.buses.push_back(std::move(bus));
    return "";
}

std::string removeBus(TopologyConfig& topo, int busIdx) {
    if (busIdx < 0 || busIdx >= static_cast<int>(topo.buses.size()))
        return "invalid bus index";
    topo.buses.erase(topo.buses.begin() + busIdx);
    return "";
}

std::string addBoard(TopologyConfig& topo,
                     int busIdx,
                     const std::string& nickname,
                     int slaveId) {
    if (busIdx < 0 || busIdx >= static_cast<int>(topo.buses.size()))
        return "invalid bus index";
    if (nickname.empty()) return "nickname required";
    if (slaveId < 0 || slaveId > 247) return "slave ID must be 0-247";
    if (boardNicknameInUse(topo, nickname)) return "nickname \"" + nickname + "\" already in use";
    if (slaveIdInUse(topo.buses[busIdx], slaveId))
        return "slave ID " + std::to_string(slaveId) + " already used on this bus";

    BoardConfig board;
    board.nickname = nickname;
    board.slaveId = slaveId;
    topo.buses[busIdx].boards.push_back(std::move(board));
    return "";
}

std::string removeBoard(TopologyConfig& topo, int busIdx, int boardIdx) {
    if (busIdx < 0 || busIdx >= static_cast<int>(topo.buses.size()))
        return "invalid bus index";
    auto& boards = topo.buses[busIdx].boards;
    if (boardIdx < 0 || boardIdx >= static_cast<int>(boards.size()))
        return "invalid board index";
    boards.erase(boards.begin() + boardIdx);
    return "";
}

std::string addGroup(TopologyConfig& topo, const std::string& name) {
    if (name.empty()) return "group name required";
    if (groupNameInUse(topo, name)) return "group name \"" + name + "\" already in use";

    GroupConfig group;
    group.name = name;
    topo.groups.push_back(std::move(group));
    return "";
}

std::string renameGroup(TopologyConfig& topo, int groupIdx, const std::string& name) {
    if (groupIdx < 0 || groupIdx >= static_cast<int>(topo.groups.size()))
        return "invalid group index";
    if (name.empty()) return "group name required";
    for (int i = 0; i < static_cast<int>(topo.groups.size()); ++i) {
        if (i != groupIdx && topo.groups[i].name == name)
            return "group name \"" + name + "\" already in use";
    }

    topo.groups[groupIdx].name = name;
    return "";
}

std::string removeGroup(TopologyConfig& topo, int groupIdx) {
    if (groupIdx < 0 || groupIdx >= static_cast<int>(topo.groups.size()))
        return "invalid group index";
    topo.groups.erase(topo.groups.begin() + groupIdx);
    return "";
}

std::string addChannelToGroup(TopologyConfig& topo,
                              int groupIdx,
                              const std::string& boardNickname,
                              int channelIndex,
                              const std::string& alias) {
    if (groupIdx < 0 || groupIdx >= static_cast<int>(topo.groups.size()))
        return "invalid group index";
    if (boardNickname.empty()) return "board required";
    if (channelIndex < 0) return "invalid channel index";

    std::string finalAlias = alias.empty() ? defaultChannelAlias(channelIndex) : alias;
    int assignedGroup = findGroupForBoardChannel(topo, boardNickname, channelIndex);
    if (assignedGroup >= 0)
        return boardChannelId(boardNickname, channelIndex) + " already assigned to group " +
               topo.groups[assignedGroup].name;

    auto& group = topo.groups[groupIdx];
    if (groupAliasInUse(group, finalAlias))
        return "alias \"" + finalAlias + "\" already in use in group " + group.name;

    GroupChannelRef ref;
    ref.boardNickname = boardNickname;
    ref.channelIndex = channelIndex;
    ref.alias = finalAlias;
    group.channels.push_back(std::move(ref));
    return "";
}

std::string renameGroupChannelAlias(TopologyConfig& topo,
                                    int groupIdx,
                                    int channelIdx,
                                    const std::string& alias) {
    if (groupIdx < 0 || groupIdx >= static_cast<int>(topo.groups.size()))
        return "invalid group index";

    auto& group = topo.groups[groupIdx];
    if (channelIdx < 0 || channelIdx >= static_cast<int>(group.channels.size()))
        return "invalid channel index";

    const auto& ref = group.channels[channelIdx];
    std::string finalAlias = alias.empty() ? defaultChannelAlias(ref.channelIndex) : alias;
    if (groupAliasInUse(group, finalAlias, channelIdx))
        return "alias \"" + finalAlias + "\" already in use in group " + group.name;

    group.channels[channelIdx].alias = finalAlias;
    return "";
}

std::string renameGroupChannelAliasForBoardChannel(TopologyConfig& topo,
                                                   const std::string& boardNickname,
                                                   int channelIndex,
                                                   const std::string& alias) {
    int groupIdx = findGroupForBoardChannel(topo, boardNickname, channelIndex);
    if (groupIdx < 0)
        return boardChannelId(boardNickname, channelIndex) + " is not assigned to a group";

    auto& group = topo.groups[groupIdx];
    for (int channelIdx = 0; channelIdx < static_cast<int>(group.channels.size()); ++channelIdx) {
        const auto& ref = group.channels[channelIdx];
        if (ref.boardNickname == boardNickname && ref.channelIndex == channelIndex)
            return renameGroupChannelAlias(topo, groupIdx, channelIdx, alias);
    }
    return boardChannelId(boardNickname, channelIndex) + " is not assigned to a group";
}

std::string removeChannelFromGroup(TopologyConfig& topo, int groupIdx, int channelIdx) {
    if (groupIdx < 0 || groupIdx >= static_cast<int>(topo.groups.size()))
        return "invalid group index";
    auto& channels = topo.groups[groupIdx].channels;
    if (channelIdx < 0 || channelIdx >= static_cast<int>(channels.size()))
        return "invalid channel index";
    channels.erase(channels.begin() + channelIdx);
    return "";
}

std::vector<GroupChannelRef> availableGroupChannels(const TopologyConfig& topo,
                                                    const std::vector<LiveBoardInfo>& liveBoards) {
    std::vector<GroupChannelRef> available;
    for (const auto& board : liveBoards) {
        for (int channelIndex = 0; channelIndex < board.numChannels; ++channelIndex) {
            if (findGroupForBoardChannel(topo, board.nickname, channelIndex) >= 0)
                continue;
            available.push_back({board.nickname, channelIndex, defaultChannelAlias(channelIndex)});
        }
    }
    return available;
}

std::string resolveBoardEndpoint(const TopologyConfig& topo,
                                 const std::string& boardNickname,
                                 BoardEndpoint& out) {
    const BoardConfig* foundBoard = nullptr;
    const BusConfig* foundBus = nullptr;

    if (!boardNickname.empty()) {
        for (const auto& bus : topo.buses) {
            for (const auto& board : bus.boards) {
                if (board.nickname == boardNickname) {
                    foundBoard = &board;
                    foundBus = &bus;
                    break;
                }
            }
            if (foundBoard) break;
        }
        if (!foundBoard)
            return "no board named '" + boardNickname + "'";
    } else if (topo.totalBoardCount() == 1) {
        for (const auto& bus : topo.buses) {
            if (!bus.boards.empty()) {
                foundBus = &bus;
                foundBoard = &bus.boards.front();
                break;
            }
        }
    } else if (topo.totalBoardCount() > 1) {
        return "topology has " + std::to_string(topo.totalBoardCount()) +
               " boards; specify one with --board <nickname>";
    } else {
        return "topology has no boards configured";
    }

    out.port = foundBus->port;
    out.baudRate = foundBus->baudRate;
    out.slaveId = foundBoard->slaveId;
    out.boardNickname = foundBoard->nickname;
    return "";
}

} // namespace psb
