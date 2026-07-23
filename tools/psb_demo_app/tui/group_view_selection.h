#pragma once

#include <algorithm>
#include <string>
#include <vector>

namespace psb::tui {

inline void reconcileGroupViewAfterReplacement(const std::vector<std::string>& oldGroups,
                                               const std::vector<std::string>& newGroups,
                                               bool wasShowingGroup,
                                               int oldGroupIdx,
                                               int& groupIdx,
                                               bool& showingGroup,
                                               int& mainSelected,
                                               int& visibleContentIdx) {
    if (newGroups.empty()) {
        groupIdx = 0;
        showingGroup = false;
        visibleContentIdx = 0;
        mainSelected = 3;
        return;
    }

    if (!wasShowingGroup) {
        groupIdx = std::min(groupIdx, static_cast<int>(newGroups.size()) - 1);
        showingGroup = false;
        visibleContentIdx = 0;
        return;
    }

    std::string oldName;
    if (oldGroupIdx >= 0 && oldGroupIdx < static_cast<int>(oldGroups.size()))
        oldName = oldGroups[oldGroupIdx];

    auto it = std::find(newGroups.begin(), newGroups.end(), oldName);
    if (it != newGroups.end())
        groupIdx = static_cast<int>(std::distance(newGroups.begin(), it));
    else
        groupIdx = std::min(std::max(0, oldGroupIdx), static_cast<int>(newGroups.size()) - 1);

    showingGroup = true;
    visibleContentIdx = 1;
    mainSelected = 3;
}

} // namespace psb::tui
