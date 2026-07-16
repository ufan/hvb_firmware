#pragma once

#include "register_map.h"
#include "types.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace psb::tui {

enum class StatusClickAction {
    None,
    Enable,
    DisableGraceful,
};

inline int selectedPortIndex(const std::vector<std::string>& ports,
                             const std::string& selected) {
    if (ports.empty()) return -1;

    const auto it = std::find(ports.begin(), ports.end(), selected);
    return it == ports.end() ? 0 : static_cast<int>(it - ports.begin());
}

// Capability-aware "is this channel's output on" — the single source of
// truth shared by the Status column's display and its click handler, so the
// two can never disagree with each other.
//   - OUTPUT_ENABLE + RAW_OUTPUT_DRIVE (e.g. jw_hvb): both the enable gate
//     and the drive value matter — enabled but driving 0 still reads "off".
//   - OUTPUT_ENABLE only (e.g. jw_lvb fixed-voltage channels): no drive
//     concept exists at all, so the enable gate alone decides.
//   - RAW_OUTPUT_DRIVE only (a DAC with no enable gate — locked always-on,
//     still drive-adjustable): on purely reflects the drive value.
//   - Neither: no output capability at all: never on (Status shows n/a).
inline bool channelIsOn(bool hasOutputEnable, bool hasRawOutputDrive,
                        bool enableActive, bool driveNonzero) {
    if (hasOutputEnable && hasRawOutputDrive) return enableActive && driveNonzero;
    if (hasOutputEnable) return enableActive;
    if (hasRawOutputDrive) return driveNonzero;
    return false;
}

// `on` should come from channelIsOn() above, not a raw status bit directly —
// see its comment for why a single bit isn't capability-agnostic-correct.
inline StatusClickAction statusClickAction(bool valid, bool ramping, bool on) {
    if (!valid || ramping) return StatusClickAction::None;
    return on ? StatusClickAction::DisableGraceful : StatusClickAction::Enable;
}

// Capability-aware status badge — single source of truth for the Channel
// tab's "Live" panel, so it can never disagree with the Monitor table's
// Status button. Same fault/cooldown/stale precedence as before, but "on" is
// decided via channelIsOn() instead of a bare OUTPUT_DRIVE_NONZERO check,
// which is meaningless on an output-enable-only channel with no drive
// concept at all (e.g. jw_lvb's fixed-voltage channels) — that bare check
// was why the Channel tab could show OFF while Monitor correctly showed ON.
inline std::string channelStatusBadge(uint16_t status, bool hasOutputEnable, bool hasRawOutputDrive) {
    using namespace psb::ChStatus;
    if (status & ACTIVE_FAULT)      return "FAULT";
    if (status & COOLDOWN_ACTIVE)   return "COOL";
    if (status & MEASUREMENT_STALE) return "STALE";
    bool on   = channelIsOn(hasOutputEnable, hasRawOutputDrive,
                            (status & OUTPUT_ENABLE_ACTIVE) != 0,
                            (status & OUTPUT_DRIVE_NONZERO) != 0);
    bool ramp = (status & RAMPING_ACTIVE) != 0;
    if (on && ramp) return "ON RAMP";
    if (on)         return "ON";
    if (ramp)       return "RAMP";
    return "OFF";
}

inline bool hasProtectionPolicy(uint16_t caps) {
    constexpr uint16_t required = CH_CAP_VOLTAGE_MEASUREMENT |
                                  CH_CAP_CURRENT_MEASUREMENT;
    return (caps & required) == required;
}

inline bool shouldPollChannel(int ch, int numChannels) {
    return ch >= 0 && ch < numChannels;
}

inline std::string unsupportedMonitorCellLabel() {
    return "n/a";
}

inline bool reconcileDisconnectedTabs(bool connected,
                                      std::vector<std::string>& titles,
                                      int& active) {
    if (connected) return false;
    if (titles.size() == 1 && titles.front() == "Monitor" && active == 0)
        return false;

    titles = {"Monitor"};
    active = 0;
    return true;
}

} // namespace psb::tui
