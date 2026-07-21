#pragma once

#include <optional>
#include <string>

namespace psb {

// App-level runtime preferences for psb_demo_tui — connection timeout and
// idle poll interval. Distinct from TopologyConfig (what to connect to, not
// how long to wait / how often to poll) and from TopologyConfig's own
// lastSingleConnectPath() (single-board quick-connect's separate
// preference) — three separate files under ~/.psb_demo_app/, none of them
// ever confused for one another.
struct AppPreferences {
    int timeoutMs = 3000;
    int pollIntervalS = 1;

    // Returns std::nullopt for both a missing file and a malformed one —
    // unlike TopologyConfig, callers never need to tell those apart: a bad
    // preferences file just means "use the hardcoded defaults above",
    // never a hard error.
    static std::optional<AppPreferences> load(const std::string& path);
    bool save(const std::string& path) const;
    static std::string defaultPath();  // ~/.psb_demo_app/preferences.toml
};

} // namespace psb
