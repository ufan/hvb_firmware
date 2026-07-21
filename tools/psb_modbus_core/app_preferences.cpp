#include "app_preferences.h"
#include "platform_paths.h"

#include <toml++/toml.hpp>

#include <filesystem>
#include <fstream>

namespace psb {

std::string AppPreferences::defaultPath() {
    return homeDir() + "/.psb_demo_app/preferences.toml";
}

std::optional<AppPreferences> AppPreferences::load(const std::string& path) {
    std::ifstream check(path);
    if (!check.good()) return std::nullopt;
    try {
        auto tbl = toml::parse_file(path);
        AppPreferences prefs;
        prefs.timeoutMs = static_cast<int>(tbl["timeout_ms"].value_or(3000));
        prefs.pollIntervalS = static_cast<int>(tbl["poll_interval_s"].value_or(1));
        return prefs;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

bool AppPreferences::save(const std::string& path) const {
    try {
        toml::table root;
        root.insert_or_assign("timeout_ms", timeoutMs);
        root.insert_or_assign("poll_interval_s", pollIntervalS);

        std::filesystem::path fsPath(path);
        if (fsPath.has_parent_path())
            std::filesystem::create_directories(fsPath.parent_path());

        std::ofstream ofs(path);
        if (!ofs) return false;
        ofs << root;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

} // namespace psb
