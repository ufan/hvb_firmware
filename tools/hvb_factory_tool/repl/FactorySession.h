#pragma once

#include "hvb_modbus_client.h"
#include "config_manager.h"
#include "types.h"
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace hvb::factory {

enum class WatchMode { Off, Adc, Measure, Status, All };

class FactorySession {
public:
    FactorySession();
    ~FactorySession();

    bool connect(const std::string& port, int baud = 115200, int slaveId = 1);
    void disconnect();
    bool isConnected() const;
    HvbModbusClient& client();

    int activeChannel() const;
    void setActiveChannel(int ch);

    WatchMode watchMode() const;
    void startWatch(WatchMode mode, int intervalMs, std::ostream& out);
    void stopWatch();

    std::string lastError() const;
    std::mutex& clientMutex();

private:
    HvbModbusClient m_client;
    ConfigManager m_config;
    std::atomic<int> m_activeChannel{-1};
    std::atomic<WatchMode> m_watchMode{WatchMode::Off};
    std::atomic<bool> m_watchRunning{false};
    std::thread m_watchThread;
    std::mutex m_clientMutex;

    void watchLoop(WatchMode mode, int intervalMs, std::ostream& out);
};

} // namespace hvb::factory
