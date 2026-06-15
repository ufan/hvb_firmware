#pragma once

#include <string>

namespace hvb {

class ConfigManager {
public:
    bool load();
    bool save() const;
    bool exists() const;
    static std::string defaultPath();

    std::string port;
    int baudRate = 115200;
    int slaveId = 1;
    int timeoutMs = 500;
    int pollIntervalMs = 2000;

    bool hasConnectionSettings() const;
    void setFromArgs(const std::string& p, int baud, int id, int timeout);
};

} // namespace hvb
