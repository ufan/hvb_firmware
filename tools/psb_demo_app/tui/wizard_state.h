#pragma once

#include "topology_config.h"

#include <string>

namespace psb::tui {

// In-progress topology edit, plus UI selection/status scratch — everything
// the wizard screen (wizard_screen.h) needs that isn't itself an FTXUI
// widget. Deliberately hardware- and FTXUI-free so the mutators below are
// unit-testable without a bus, a board, or a terminal (mirrors
// tui_policy.h's split between policy and rendering).
struct WizardState {
    psb::TopologyConfig topo;   // in-progress; may be unsaved
    std::string topologyPath;   // Save target
    int selectedBus = -1;       // index into topo.buses, -1 = none
    int selectedBoard = -1;     // index into topo.buses[selectedBus].boards, -1 = none
    std::string statusMsg;
    bool dirty = false;         // true after any mutation since the last successful save
};

inline bool nicknameInUse(const psb::TopologyConfig& topo, const std::string& nickname) {
    for (const auto& bus : topo.buses)
        for (const auto& board : bus.boards)
            if (board.nickname == nickname) return true;
    return false;
}

inline bool slaveIdInUse(const psb::BusConfig& bus, int slaveId) {
    for (const auto& board : bus.boards)
        if (board.slaveId == slaveId) return true;
    return false;
}

// Every mutator below: returns "" on success, a user-facing error message on
// failure; never throws; never touches hardware — Save/Connect Now/Apply
// (wizard_screen.h) decide separately when (if ever) to open a physical
// connection.

inline std::string addBus(WizardState& s, const std::string& name,
                          const std::string& port, int baud) {
    if (port.empty()) return "port required";
    for (const auto& b : s.topo.buses)
        if (b.port == port) return "port already in use by bus \"" + b.name + "\"";
    psb::BusConfig bus;
    bus.name = name.empty() ? ("bus" + std::to_string(s.topo.buses.size() + 1)) : name;
    bus.port = port;
    bus.baudRate = baud;
    s.topo.buses.push_back(std::move(bus));
    s.dirty = true;
    return "";
}

inline std::string removeBus(WizardState& s, int busIdx) {
    if (busIdx < 0 || busIdx >= static_cast<int>(s.topo.buses.size()))
        return "invalid bus index";
    s.topo.buses.erase(s.topo.buses.begin() + busIdx);
    if (s.selectedBus >= static_cast<int>(s.topo.buses.size()))
        s.selectedBus = static_cast<int>(s.topo.buses.size()) - 1;
    s.selectedBoard = -1;
    s.dirty = true;
    return "";
}

inline std::string addBoard(WizardState& s, int busIdx, const std::string& nickname, int slaveId) {
    if (busIdx < 0 || busIdx >= static_cast<int>(s.topo.buses.size()))
        return "invalid bus index";
    if (nickname.empty()) return "nickname required";
    if (slaveId < 0 || slaveId > 247) return "slave ID must be 0-247";
    if (nicknameInUse(s.topo, nickname)) return "nickname \"" + nickname + "\" already in use";
    if (slaveIdInUse(s.topo.buses[busIdx], slaveId))
        return "slave ID " + std::to_string(slaveId) + " already used on this bus";
    psb::BoardConfig board;
    board.nickname = nickname;
    board.slaveId = slaveId;
    s.topo.buses[busIdx].boards.push_back(std::move(board));
    s.dirty = true;
    return "";
}

inline std::string removeBoard(WizardState& s, int busIdx, int boardIdx) {
    if (busIdx < 0 || busIdx >= static_cast<int>(s.topo.buses.size()))
        return "invalid bus index";
    auto& boards = s.topo.buses[busIdx].boards;
    if (boardIdx < 0 || boardIdx >= static_cast<int>(boards.size()))
        return "invalid board index";
    boards.erase(boards.begin() + boardIdx);
    if (s.selectedBoard >= static_cast<int>(boards.size()))
        s.selectedBoard = static_cast<int>(boards.size()) - 1;
    s.dirty = true;
    return "";
}

} // namespace psb::tui
