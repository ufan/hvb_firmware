#include "FactorySession.h"
#include "register_map.h"
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <thread>

#if defined(_WIN32)
#include <conio.h>
#else
#include <termios.h>
#include <unistd.h>
#include <sys/select.h>
#endif

namespace psb::factory {

namespace {

// Detects a single keypress on the controlling terminal without requiring
// Enter and without echoing it, so "watch" can treat any key as "stop".
//
// The cli library calls kb.DeactivateInput() before invoking a command
// handler and kb.ActivateInput() after it returns (see
// cli/detail/commandprocessor.h), which is what makes it safe to touch
// stdin from here at all. But DeactivateInput() only stops its background
// keyboard-reader thread from *starting* another blocking read — it can't
// cancel one already in flight. Concretely: that thread reads one key at a
// time and immediately loops back to wait for the next one; if it has
// already re-entered that wait for "whatever comes after Enter" before
// DeactivateInput() runs (a real, observed race, not hypothetical), it
// stays blocked there for as long as this object's read loop runs too —
// both sides are then watching stdin at once. Whichever side loses that
// race for the *stop* keystroke has it delivered later, once
// ActivateInput() reawakens the reader, as if newly typed — surfacing as a
// stray leading character glued onto whatever command is typed next. There
// is no way to preempt the library's already-blocked read from here, so
// this can't be closed completely; tcflush() below only discards bytes that
// arrived and were never read by anyone, which narrows but doesn't
// eliminate the window.
class KeyWatcher {
public:
    KeyWatcher() {
#if !defined(_WIN32)
        m_isTty = ::isatty(STDIN_FILENO) != 0;
        if (m_isTty) {
            ::tcgetattr(STDIN_FILENO, &m_old);
            termios raw = m_old;
            raw.c_lflag &= ~(ICANON | ECHO);
            raw.c_cc[VMIN] = 0;
            raw.c_cc[VTIME] = 0;
            ::tcsetattr(STDIN_FILENO, TCSANOW, &raw);
            ::tcflush(STDIN_FILENO, TCIFLUSH);
        }
#endif
    }

    ~KeyWatcher() {
#if !defined(_WIN32)
        if (m_isTty) ::tcsetattr(STDIN_FILENO, TCSANOW, &m_old);
#endif
    }

    bool isTty() const {
#if defined(_WIN32)
        return true;
#else
        return m_isTty;
#endif
    }

    // Waits up to timeoutMs for a keypress, consuming it. Returns true if a
    // key arrived, false on timeout.
    bool waitKey(int timeoutMs) {
#if defined(_WIN32)
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline) {
            if (_kbhit()) {
                (void)_getch();
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
#else
        if (!m_isTty) {
            // No real keyboard to watch — just wait out the interval.
            std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs));
            return false;
        }
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
        int r = ::select(STDIN_FILENO + 1, &rfds, nullptr, nullptr, &tv);
        if (r > 0 && FD_ISSET(STDIN_FILENO, &rfds)) {
            char c;
            (void)::read(STDIN_FILENO, &c, 1);
            return true;
        }
        return false;
#endif
    }

private:
#if !defined(_WIN32)
    bool m_isTty = false;
    termios m_old{};
#endif
};

} // namespace

FactorySession::FactorySession() = default;

FactorySession::~FactorySession() {
    disconnect();
}

bool FactorySession::connect(const std::string& port, int baud, int slaveId) {
    // The default response timeout is fine for every command except
    // "cal sample", which needs longer headroom for the firmware's internal
    // wait — that's scoped directly in PsbModbusClient::sendCalibrationSampleCommand
    // so routine commands stay fast.
    return m_client.connect(port, baud, slaveId);
}

void FactorySession::disconnect() {
    m_client.disconnect();
    m_activeChannel = -1;
}

bool FactorySession::isConnected() const { return m_client.isConnected(); }
PsbModbusClient& FactorySession::client() { return m_client; }
int FactorySession::activeChannel() const { return m_activeChannel; }
void FactorySession::setActiveChannel(int ch) { m_activeChannel = ch; }
std::string FactorySession::lastError() const { return m_client.lastError(); }
std::mutex& FactorySession::clientMutex() { return m_clientMutex; }

void FactorySession::runWatch(WatchMode mode, int intervalMs, std::ostream& out) {
    KeyWatcher keys;
    if (!keys.isTty()) {
        out << "Error: watch requires an interactive terminal\n";
        return;
    }

    int ch = m_activeChannel;
    out << "(any key stops)\n";
    // No lock here: this runs synchronously inside the calling command's
    // requireCalChannel(), which already holds clientMutex() for the whole
    // call — and since the cli library deactivates its keyboard reader and
    // blocks command dispatch while we run (see runWatch's declaration),
    // nothing else can touch m_client concurrently anyway.
    while (m_client.isConnected()) {
        std::ostringstream ss;
        if (mode == WatchMode::Adc || mode == WatchMode::All) {
            if (!m_client.sendCalibrationSampleCommand(ch)) {
                out << "\r\nWatch stopped: " << m_client.lastError() << "\n";
                return;
            }
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

        for (int slept = 0; slept < intervalMs; ) {
            int step = std::min(100, intervalMs - slept);
            if (keys.waitKey(step)) {
                out << "\nWatch stopped\n";
                return;
            }
            slept += step;
        }
    }
    out << "\nWatch stopped: device disconnected\n";
}

} // namespace psb::factory
