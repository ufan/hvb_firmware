#pragma once

#include <string>

namespace psb {

// Shared by TopologyConfig and AppPreferences (both persist under
// homeDir() + "/.psb_demo_app/") — needed by more than one .cpp file
// within psb_modbus_core, so it isn't static/internal-linkage like it
// used to be when only topology_config.cpp needed it.
std::string homeDir();

} // namespace psb
