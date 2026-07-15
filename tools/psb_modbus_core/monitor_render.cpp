#include "psb_modbus_client.h"
#include "register_map.h"
#include "types.h"

#include <sstream>
#include <iomanip>
#include <vector>

std::string renderMonitorTable(const psb::SystemInfo& sys,
                               const std::vector<psb::ChannelInfo>& channels)
{
    std::ostringstream ss;

    // Header
    ss << "=== PSB Monitor [" << sys.protoMajor << "." << sys.protoMinor
       << "] Uptime: " << sys.uptimeSec << "s"
       << "  Mode: " << psb::opModeName(sys.activeOpMode) << " ===\n";

    // Column headers
    ss << " CH │ Vmeas        │ Imeas        │ Trg(V)      │ Status  │ Fault │ Retry\n";
    ss << "────┼──────────────┼──────────────┼─────────────┼─────────┼───────┼──────\n";

    // Data rows — channels.size() already reflects the connected board's
    // actual SUPPORTED_CHANNELS (see cmdMonitor()), not a fixed count.
    for (size_t i = 0; i < channels.size(); ++i) {
        const auto& ci = channels[i];

        auto v = psb::reg::voltageToV(ci.voltageRaw);
        auto a = psb::reg::currentToA(ci.currentRaw);
        auto tv = psb::reg::voltageToV(ci.operationalTargetVoltageRaw);
        bool fault = ci.activeFault != 0;
        bool outDrive = ci.status & psb::ChStatus::OUTPUT_DRIVE_NONZERO;

        ss << " CH" << i << " │ "
           << std::showpos << std::fixed << std::setprecision(1) << std::setw(8) << v << " V │ "
           << std::showpos << std::fixed << std::setprecision(1) << std::setw(8) << (a * 1e6) << " uA │ "
           << std::showpos << std::fixed << std::setprecision(1) << std::setw(8) << tv << "  │ "
           << (outDrive ? " ON " : " OFF") << " │ "
           << (fault ? " YES " : "  -- ") << " │ "
           << std::setw(4) << ci.retryCount << "\n";
    }

    return ss.str();
}
