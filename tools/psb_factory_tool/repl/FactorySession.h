#pragma once

#include "psb_modbus_client.h"
#include "config_manager.h"
#include "types.h"
#include <atomic>
#include <functional>
#include <mutex>
#include <string>

namespace psb::factory {

enum class WatchMode { Off, Adc, Measure, Status, All };

class FactorySession {
public:
    FactorySession();
    ~FactorySession();

    bool connect(const std::string& port, int baud = 115200, int slaveId = 1);
    void disconnect();
    bool isConnected() const;
    PsbModbusClient& client();

    int activeChannel() const;
    void setActiveChannel(int ch);

    // Blocking: prints periodic updates until any key is pressed or the
    // connection drops, then returns. Exclusively owns terminal input for
    // its duration — safe because the cli library deactivates its own
    // keyboard reader around command execution (see FactorySession.cpp).
    // No other command can run while this is in progress.
    void runWatch(WatchMode mode, int intervalMs, std::ostream& out);

    std::string lastError() const;
    std::mutex& clientMutex();

private:
    PsbModbusClient m_client;
    ConfigManager m_config;
    std::atomic<int> m_activeChannel{-1};
    std::mutex m_clientMutex;
};

} // namespace psb::factory
