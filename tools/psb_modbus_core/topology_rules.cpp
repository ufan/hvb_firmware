#include "topology_rules.h"

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

} // namespace psb
