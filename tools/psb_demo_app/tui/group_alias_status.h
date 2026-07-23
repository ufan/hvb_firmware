#pragma once

#include <string>

namespace psb::tui {

inline std::string groupAliasSaveStatus(const std::string& err) {
    return err.empty() ? "OK: group alias saved" : "Error: " + err;
}

inline void syncGroupAliasInput(std::string& inputAlias,
                                std::string& lastMembershipKey,
                                bool grouped,
                                const std::string& membershipKey,
                                const std::string& membershipAlias,
                                bool inputFocused) {
    if (!grouped) {
        lastMembershipKey.clear();
        inputAlias.clear();
        return;
    }

    if (lastMembershipKey != membershipKey || inputAlias.empty() ||
        (!inputFocused && inputAlias != membershipAlias)) {
        inputAlias = membershipAlias;
        lastMembershipKey = membershipKey;
    }
}

} // namespace psb::tui
