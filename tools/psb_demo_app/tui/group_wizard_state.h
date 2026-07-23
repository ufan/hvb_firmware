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

} // namespace psb::tui
