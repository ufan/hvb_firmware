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

} // namespace psb
