#pragma once

#include "register_map.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace hvb::tui {

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

inline StatusClickAction statusClickAction(bool valid, bool ramping,
                                           int16_t configuredTarget,
                                           int16_t operationalTarget,
                                           bool driveOn) {
    if (!valid || ramping) return StatusClickAction::None;
    if (!driveOn && configuredTarget == 0 && operationalTarget == 0)
        return StatusClickAction::None;

    return driveOn && operationalTarget != 0
        ? StatusClickAction::DisableGraceful
        : StatusClickAction::Enable;
}

inline bool hasProtectionPolicy(uint16_t caps) {
    constexpr uint16_t required = CH_CAP_VOLTAGE_MEASUREMENT |
                                  CH_CAP_CURRENT_MEASUREMENT;
    return (caps & required) == required;
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

} // namespace hvb::tui
