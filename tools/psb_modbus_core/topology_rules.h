#pragma once

#include "topology_config.h"

#include <string>
#include <vector>

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

std::string addBus(TopologyConfig& topo,
                   const std::string& name,
                   const std::string& port,
                   int baud);
std::string removeBus(TopologyConfig& topo, int busIdx);
std::string addBoard(TopologyConfig& topo,
                     int busIdx,
                     const std::string& nickname,
                     int slaveId);
std::string removeBoard(TopologyConfig& topo, int busIdx, int boardIdx);
std::string addGroup(TopologyConfig& topo, const std::string& name);
std::string removeGroup(TopologyConfig& topo, int groupIdx);
std::string addChannelToGroup(TopologyConfig& topo,
                              int groupIdx,
                              const std::string& boardNickname,
                              int channelIndex,
                              const std::string& alias);
std::string renameGroupChannelAlias(TopologyConfig& topo,
                                    int groupIdx,
                                    int channelIdx,
                                    const std::string& alias);
std::string renameGroupChannelAliasForBoardChannel(TopologyConfig& topo,
                                                   const std::string& boardNickname,
                                                   int channelIndex,
                                                   const std::string& alias);
std::string removeChannelFromGroup(TopologyConfig& topo, int groupIdx, int channelIdx);

} // namespace psb
