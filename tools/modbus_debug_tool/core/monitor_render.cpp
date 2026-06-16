#include "hvb_modbus_client.h"
#include "register_map.h"
#include "types.h"

#include <sstream>
#include <iomanip>
#include <vector>

std::string renderMonitorTable(const hvb::SystemInfo& sys,
                               const std::vector<hvb::ChannelInfo>& channels)
{
    std::ostringstream ss;

    // Header
    ss << "=== HVB Monitor [" << sys.protoMajor << "." << sys.protoMinor
       << "] Uptime: " << sys.uptimeSec << "s"
       << "  Mode: " << hvb::opModeName(sys.activeOpMode) << " ===\n";

    // Column headers
    ss << " CH │ Vmeas        │ Imeas        │ Trg(V)      │ Status  │ Fault │ Retry\n";
    ss << "────┼──────────────┼──────────────┼─────────────┼─────────┼───────┼──────\n";

    // Data rows
    for (size_t i = 0; i < channels.size() && i < 2; ++i) {
        const auto& ci = channels[i];

        if (ci.status & hvb::ChStatus::UNSUPPORTED) {
            ss << " CH" << i << " │ (unsupported)\n";
            continue;
        }

        auto v = hvb::reg::voltageToV(ci.voltageRaw);
        auto a = hvb::reg::currentToA(ci.currentRaw);
        auto tv = hvb::reg::voltageToV(ci.operationalTargetVoltageRaw);
        bool fault = ci.activeFault != 0;
        bool outDrive = ci.status & hvb::ChStatus::OUTPUT_DRIVE_NONZERO;

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
