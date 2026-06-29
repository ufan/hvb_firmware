#include "FactorySession.h"
#include "register_map.h"
#include <chrono>
#include <iomanip>
#include <sstream>

namespace hvb::factory {

FactorySession::FactorySession() = default;

FactorySession::~FactorySession() {
    stopWatch();
    disconnect();
}

bool FactorySession::connect(const std::string& port, int baud, int slaveId) {
    return m_client.connect(port, baud, slaveId);
}

void FactorySession::disconnect() {
    stopWatch();
    m_client.disconnect();
    m_activeChannel = -1;
}

bool FactorySession::isConnected() const { return m_client.isConnected(); }
HvbModbusClient& FactorySession::client() { return m_client; }
int FactorySession::activeChannel() const { return m_activeChannel; }
void FactorySession::setActiveChannel(int ch) { m_activeChannel = ch; }
WatchMode FactorySession::watchMode() const { return m_watchMode.load(); }
std::string FactorySession::lastError() const { return m_client.lastError(); }
std::mutex& FactorySession::clientMutex() { return m_clientMutex; }

void FactorySession::startWatch(WatchMode mode, int intervalMs, std::ostream& out) {
    stopWatch();
    m_watchMode = mode;
    m_watchRunning = true;
    m_watchThread = std::thread(&FactorySession::watchLoop, this, mode, intervalMs, std::ref(out));
}

void FactorySession::stopWatch() {
    m_watchRunning = false;
    if (m_watchThread.joinable()) m_watchThread.join();
    m_watchMode = WatchMode::Off;
}

void FactorySession::watchLoop(WatchMode mode, int intervalMs, std::ostream& out) {
    int ch = m_activeChannel;
    while (m_watchRunning && m_client.isConnected()) {
        std::lock_guard<std::mutex> lk(m_clientMutex);
        std::ostringstream ss;

        if (mode == WatchMode::Adc || mode == WatchMode::All) {
            if (!m_client.sendCalibrationSampleCommand(ch)) break;
            auto snap = m_client.readCalibrationSnapshot(ch);
            ss << "  ADC V=" << snap.rawAdcVoltage
               << " I=" << snap.rawAdcCurrent
                << " DAC=" << snap.rawDacCode
               << " out=" << (snap.outputEnabled ? "ON" : "OFF");
        }
        if (mode == WatchMode::Measure || mode == WatchMode::All) {
            auto ci = m_client.readChannelInfo(ch);
            ss << "  V=" << std::fixed << std::setprecision(1)
               << reg::voltageToV(ci.voltageRaw) << "V"
               << " I=" << reg::currentToA(ci.currentRaw) * 1e6 << "uA";
        }
        if (mode == WatchMode::Status || mode == WatchMode::All) {
            auto si = m_client.readSystemInfo();
            ss << "  mode=" << opModeName(si.activeOpMode)
               << " uptime=" << si.uptimeSec << "s";
        }

        out << "\r" << ss.str() << std::flush;

        for (int i = 0; i < intervalMs / 100 && m_watchRunning; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    out << "\n";
}

} // namespace hvb::factory
