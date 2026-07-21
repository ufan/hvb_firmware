#pragma once

#include "psb_serial_bus.h"
#include "psb_board_session.h"
#include "board_catalog.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace psb::tui {

struct DiscoveredBoard {
    int slaveId;
    std::string variantName;
    uint32_t fwVersion;
};

// Short per-candidate timeout for a scan sweep — a non-responding ID is the
// overwhelmingly common case (a 1-32 sweep against a 2-board bus probes 30
// dead addresses), and each one must fail fast or a full-range sweep takes
// minutes instead of seconds. Confirmed via Task 1's PsbBoardSession::
// verifyProtocol(int) override.
inline constexpr int kScanProbeTimeoutMs = 200;

// Sweeps [startId, endId] (inclusive) on a bus the caller has already
// connect()ed — this function never opens or closes a port. Each candidate
// gets exactly the probe every board's real connect already runs
// (verifyProtocol), just with a short timeout; a candidate that answers gets
// one follow-up readSystemInfo() (default timeout — this call only ever
// targets an ID that just proved it's alive, so it's expected to succeed
// promptly) to report its variant for the picker. `onProgress(candidateId)`
// fires before every probe (responder or not) so a caller can drive a
// progress indicator; default no-op.
//
// Blocking — runs entirely on the calling thread. Callers driving this from
// an interactive UI must run it on a dedicated thread and marshal results
// back to the UI thread themselves (see wizard_screen.h), the same pattern
// board_session.h's doFullScan/doPollScan already establish for routine
// polling.
inline std::vector<DiscoveredBoard> scanBus(std::shared_ptr<PsbSerialBus> bus,
                                             int startId, int endId,
                                             const std::function<void(int)>& onProgress = {}) {
    std::vector<DiscoveredBoard> found;
    for (int id = startId; id <= endId; ++id) {
        if (onProgress) onProgress(id);
        PsbBoardSession probe(bus, id);
        if (!probe.verifyProtocol(kScanProbeTimeoutMs)) continue;
        SystemInfo info = probe.readSystemInfo();
        found.push_back({id, catalog::variantName(info.variantId), info.fwVersion});
    }
    return found;
}

} // namespace psb::tui
