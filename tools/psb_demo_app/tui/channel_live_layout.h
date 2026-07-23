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

enum class ChannelPolicyBox {
    StartupPolicy,
    ProtectionPolicy,
    RecoveryPolicy,
    Setting,
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

inline std::vector<std::vector<ChannelPolicyBox>> channelPolicyBoxLayout() {
    return {
        {ChannelPolicyBox::StartupPolicy, ChannelPolicyBox::ProtectionPolicy},
        {ChannelPolicyBox::RecoveryPolicy, ChannelPolicyBox::Setting},
    };
}

inline int channelProtectionLeftColumnRows() {
    return 3;
}

inline int channelProtectionButtonColumnRows() {
    return 2;
}

inline int channelPolicyBoxContentRows(ChannelPolicyBox box) {
    switch (box) {
    case ChannelPolicyBox::StartupPolicy:
        return 3;
    case ChannelPolicyBox::ProtectionPolicy:
        return channelProtectionLeftColumnRows();
    case ChannelPolicyBox::RecoveryPolicy:
        return 5;
    case ChannelPolicyBox::Setting:
        return 1;
    }
    return 0;
}

} // namespace psb::tui
