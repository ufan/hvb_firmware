#pragma once

#include <string>
#include <vector>

namespace psb::tui {

enum class ChannelLiveSection {
    Title,
    Telemetry,
    ControlActions,
    Spacer,
    GroupNameControls,
};

inline std::string channelGroupAliasLabel() {
    return " Name: ";
}

inline std::string channelLiveControlLabel() {
    return " Control ";
}

inline std::string channelStartupPolicyPaneTitle() {
    return " Startup Policy ";
}

inline std::vector<ChannelLiveSection> channelLiveSections(bool grouped) {
    std::vector<ChannelLiveSection> sections{
        ChannelLiveSection::Title,
        ChannelLiveSection::Telemetry,
        ChannelLiveSection::Spacer,
        ChannelLiveSection::ControlActions,
    };
    if (grouped)
        sections.push_back(ChannelLiveSection::Spacer);
    if (grouped)
        sections.push_back(ChannelLiveSection::GroupNameControls);
    return sections;
}

} // namespace psb::tui
