#pragma once

#include <vector>

namespace psb::tui {

enum class ChannelLiveSection {
    Title,
    Telemetry,
    GroupAliasControls,
};

inline std::vector<ChannelLiveSection> channelLiveSections(bool grouped) {
    std::vector<ChannelLiveSection> sections{
        ChannelLiveSection::Title,
        ChannelLiveSection::Telemetry,
    };
    if (grouped)
        sections.push_back(ChannelLiveSection::GroupAliasControls);
    return sections;
}

} // namespace psb::tui
