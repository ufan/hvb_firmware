#pragma once

#include "topology_config.h"

#include <string>

namespace psb {

bool boardNicknameInUse(const TopologyConfig& topo, const std::string& nickname);
bool slaveIdInUse(const BusConfig& bus, int slaveId);
bool groupNameInUse(const TopologyConfig& topo, const std::string& name);
std::string boardChannelId(const std::string& boardNickname, int channelIndex);
int findGroupForBoardChannel(const TopologyConfig& topo,
                             const std::string& boardNickname,
                             int channelIndex);
bool groupAliasInUse(const GroupConfig& group,
                     const std::string& alias,
                     int exceptChannelIdx = -1);

} // namespace psb
