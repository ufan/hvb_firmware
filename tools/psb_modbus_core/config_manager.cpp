#include "config_manager.h"

#include <toml++/toml.hpp>

#include <cstdlib>
#include <fstream>
#include <iostream>

#if defined(_WIN32)
#include <cstdlib>
#else
#include <unistd.h>
#include <pwd.h>
#endif

namespace psb {

static std::string homeDir() {
#if defined(_WIN32)
    const char* hd = std::getenv("USERPROFILE");
    return hd ? hd : ".";
#else
    const char* hd = std::getenv("HOME");
    if (hd) return hd;
    struct passwd* pw = getpwuid(getuid());
    return pw && pw->pw_dir ? pw->pw_dir : ".";
#endif
}

std::string ConfigManager::defaultPath() {
    return homeDir() + "/.psb_demo_app.toml";
}

bool ConfigManager::load() {
    auto path = defaultPath();
    if (!exists()) return false;
    try {
        auto tbl = toml::parse_file(path);
        auto conn = tbl["connection"];
        port = conn["port"].value_or("");
        baudRate = static_cast<int>(conn["baud_rate"].value_or(115200));
        slaveId = static_cast<int>(conn["slave_id"].value_or(1));
        timeoutMs = static_cast<int>(conn["timeout_ms"].value_or(500));
        auto disp = tbl["display"];
        pollIntervalMs = static_cast<int>(disp["poll_interval_ms"].value_or(2000));
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Config load warning: " << e.what() << "\n";
        return false;
    }
}

bool ConfigManager::save() const {
    auto path = defaultPath();
    try {
        toml::table tbl;
        tbl["connection"].as_table()->insert_or_assign("port", port);
        tbl["connection"].as_table()->insert_or_assign("baud_rate", baudRate);
        tbl["connection"].as_table()->insert_or_assign("slave_id", slaveId);
        tbl["connection"].as_table()->insert_or_assign("timeout_ms", timeoutMs);
        tbl["display"].as_table()->insert_or_assign("poll_interval_ms", pollIntervalMs);

        std::ofstream ofs(path);
        if (!ofs) return false;
        ofs << tbl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Config save error: " << e.what() << "\n";
        return false;
    }
}

bool ConfigManager::exists() const {
    std::ifstream ifs(defaultPath());
    return ifs.good();
}

bool ConfigManager::hasConnectionSettings() const {
    return !port.empty();
}

void ConfigManager::setFromArgs(const std::string& p, int baud, int id, int timeout) {
    if (!p.empty()) port = p;
    baudRate = baud;
    slaveId = id;
    timeoutMs = timeout;
}

} // namespace psb
