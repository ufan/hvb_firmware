#pragma once

#include <optional>
#include <string>
#include <vector>

namespace psb {

struct BoardConfig {
    std::string nickname;
    int slaveId = 1;
    // Index = channel number; "" means no alias set for that channel. Only
    // as long as the highest-set channel needs it — never pre-sized to
    // MAX_CHANNELS on disk.
    std::vector<std::string> channelAliases;
};

struct BusConfig {
    std::string name;
    std::string port;
    int baudRate = 115200;
    std::vector<BoardConfig> boards;
};

// References a channel by the board's nickname + channel index rather than
// bus/port — nicknames are already the codebase's de facto unique board
// identifier (used the same way by detachBoard in board_switcher.h), so a
// group's membership survives a board moving to a different port.
struct GroupChannelRef {
    std::string boardNickname;
    int channelIndex = 0;
    std::string alias;
};

struct GroupConfig {
    std::string name;
    std::vector<GroupChannelRef> channels;
};

inline std::string defaultChannelAlias(int channelIndex) {
    return "CH" + std::to_string(channelIndex);
}

// Supersedes ConfigManager (~/.psb_demo_app.toml, single board/bus only).
// See docs/superpowers/specs/2026-07-20-multi-board-topology-design.md.
struct TopologyConfig {
    std::vector<BusConfig> buses;
    std::vector<GroupConfig> groups;  // user-defined channel groups, sibling to buses

    // Returns std::nullopt if the file doesn't exist, or fails to parse
    // (malformed TOML) — use exists() first if the caller needs to tell
    // those two cases apart (e.g. to decide whether to offer to create one
    // vs. report a parse error).
    static std::optional<TopologyConfig> load(const std::string& path);
    bool save(const std::string& path) const;
    static bool exists(const std::string& path);
    static TopologyConfig singleBoard(const std::string& port, int baud,
                                       int slaveId, const std::string& nickname = "board1");
    static std::string defaultPath();  // ~/.psb_demo_app/topology.toml
    // Deliberately separate from defaultPath() — single-board mode's quick-
    // connect form (mode_select.h) pre-fills from and saves to this path,
    // never the real multi-board topology file, so the two can never be
    // confused for one another.
    static std::string lastSingleConnectPath();  // ~/.psb_demo_app/last_single.toml
    int totalBoardCount() const;       // sum of boards across all buses
};

} // namespace psb
