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

} // namespace psb::tui
