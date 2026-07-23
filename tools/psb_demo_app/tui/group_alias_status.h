#pragma once

#include <string>

namespace psb::tui {

inline std::string groupAliasSaveStatus(const std::string& err) {
    return err.empty() ? "OK: group alias saved" : "Error: " + err;
}

} // namespace psb::tui
