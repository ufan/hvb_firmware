#include "psb_serial_bus.h"

#include <ModbusClientPort.h>
#include <ModbusPort.h>
#include <Modbus.h>

#include <chrono>
#include <cstring>
#include <thread>

namespace psb {

// See PsbModbusClient's historical version of this comment
// (git history / client-architecture-and-pitfalls.md §1.3) for the full
// story: ModbusClientPort::process() is a non-blocking state machine meant
// to be pumped from an external loop: a zero-delay spin pegs a CPU core at
// ~100% for the duration of every transaction. This sleep is required, not
// cosmetic.
static constexpr auto kNonBlockingPollInterval = std::chrono::milliseconds(1);

struct PsbSerialBus::Impl {
    ModbusClientPort* port = nullptr;
    bool connected = false;
    std::string errorText;
    FrameCallback frameCb;
    int lastSlaveId = 0;

    struct TestBoard {
        uint16_t* inputRegs;
        uint16_t* holdingRegs;
        int maxAddr;
    };
    std::map<int, TestBoard> testBoards;

    ~Impl() { disconnect(); }
    void disconnect() {
        if (port) { delete port; port = nullptr; }
        connected = false;
    }

    void onTx(const Modbus::Char*, const uint8_t* buff, uint16_t size) {
        if (frameCb) frameCb(lastSlaveId, true, std::vector<uint8_t>(buff, buff + size));
    }
    void onRx(const Modbus::Char*, const uint8_t* buff, uint16_t size) {
        if (frameCb) frameCb(lastSlaveId, false, std::vector<uint8_t>(buff, buff + size));
    }
};

PsbSerialBus::PsbSerialBus() : m_impl(std::make_unique<Impl>()) {}
PsbSerialBus::~PsbSerialBus() = default;

bool PsbSerialBus::connect(const std::string& portName, int baud, int timeoutMs) {
    m_impl->disconnect();

    Modbus::SerialSettings settings;
    settings.portName = portName.c_str();
    settings.baudRate = baud;
    settings.dataBits = 8;
    settings.parity = Modbus::NoParity;
    settings.stopBits = Modbus::OneStop;
    settings.flowControl = Modbus::NoFlowControl;
    settings.timeoutFirstByte = static_cast<uint32_t>(timeoutMs);
    // timeoutInterByte governs how long ModbusLib waits for each subsequent
    // byte within an already-started response before concluding the frame
    // is complete — a property of the serial link and adapter, unrelated to
    // timeoutFirstByte (how long to wait for a response to even begin) and
    // must NOT be derived from it. See
    // docs/guide/client-architecture-and-pitfalls.md §1.2 — a
    // timeoutMs/10-style formula here was a real, previously-fixed bug that
    // made every transaction (not just failures) ~6x slower than necessary.
    // 50ms has generous margin over both the Modbus RTU spec's
    // frame-silence threshold (~0.3ms at 115200 baud) and real USB-serial
    // adapter buffering behavior (e.g. CH340).
    settings.timeoutInterByte = 50;

    // Non-blocking mode: engages ModbusLib's inter-byte accumulation loop.
    // The blocking path's single ::read() treats a partial frame as
    // complete, truncating responses >~31 bytes over USB-serial. Our
    // read/write wrappers poll until the request stops returning
    // Status_Processing (see kNonBlockingPollInterval above).
    m_impl->port = Modbus::createClientPort(Modbus::RTU, &settings, false);
    if (!m_impl->port) {
        m_impl->errorText = "failed to create RTU client port";
        return false;
    }
    m_impl->port->connect(&ModbusClientPort::signalTx, m_impl.get(), &Impl::onTx);
    m_impl->port->connect(&ModbusClientPort::signalRx, m_impl.get(), &Impl::onRx);
    m_impl->connected = true;
    return true;
}

void PsbSerialBus::disconnect() { m_impl->disconnect(); }
bool PsbSerialBus::isConnected() const { return m_impl->connected || !m_impl->testBoards.empty(); }
std::string PsbSerialBus::lastError() const { return m_impl->errorText; }

void PsbSerialBus::attachTestArrays(int slaveId, uint16_t* inputRegs, uint16_t* holdingRegs, int maxAddr) {
    m_impl->testBoards[slaveId] = Impl::TestBoard{inputRegs, holdingRegs, maxAddr};
}
void PsbSerialBus::detachTestArrays(int slaveId) {
    m_impl->testBoards.erase(slaveId);
}

namespace {
// RAII helper: temporarily overrides a ModbusPort's response timeout for
// the duration of a single request, restoring the previous value on scope
// exit (including early returns). No-op if port is null or override is
// negative.
class ScopedPortTimeout {
public:
    ScopedPortTimeout(ModbusPort* port, int overrideMs) : m_port(nullptr) {
        if (port && overrideMs >= 0) {
            m_port = port;
            m_saved = m_port->timeout();
            m_port->setTimeout(static_cast<uint32_t>(overrideMs));
        }
    }
    ~ScopedPortTimeout() {
        if (m_port) m_port->setTimeout(m_saved);
    }
    ScopedPortTimeout(const ScopedPortTimeout&) = delete;
    ScopedPortTimeout& operator=(const ScopedPortTimeout&) = delete;

private:
    ModbusPort* m_port;
    uint32_t m_saved = 0;
};
} // namespace

bool PsbSerialBus::readRegsInternal(bool holding, int slaveId, uint16_t addr, uint16_t count,
                                    uint16_t* out, int timeoutOverrideMs) {
    auto it = m_impl->testBoards.find(slaveId);
    if (it != m_impl->testBoards.end()) {
        auto& tb = it->second;
        uint16_t* src = holding ? tb.holdingRegs : tb.inputRegs;
        if (!src || addr + count > static_cast<uint16_t>(tb.maxAddr)) {
            m_impl->errorText = "test: address out of range";
            return false;
        }
        memcpy(out, src + addr, count * sizeof(uint16_t));
        return true;
    }
    if (!m_impl->connected || !m_impl->port) {
        m_impl->errorText = "not connected";
        return false;
    }
    m_impl->lastSlaveId = slaveId;
    ScopedPortTimeout timeoutGuard(m_impl->port->port(), timeoutOverrideMs);
    Modbus::StatusCode s;
    auto unit = static_cast<uint8_t>(slaveId);
    do {
        if (holding)
            s = m_impl->port->readHoldingRegisters(unit, addr, count, out);
        else
            s = m_impl->port->readInputRegisters(unit, addr, count, out);
        if (Modbus::StatusIsProcessing(s)) std::this_thread::sleep_for(kNonBlockingPollInterval);
    } while (Modbus::StatusIsProcessing(s));
    if (!Modbus::StatusIsGood(s)) {
        m_impl->errorText = std::string("Modbus error: ") + m_impl->port->lastErrorText();
        return false;
    }
    return true;
}

bool PsbSerialBus::readInputRegs(int slaveId, uint16_t addr, uint16_t count, uint16_t* out,
                                 int timeoutOverrideMs) {
    return readRegsInternal(false, slaveId, addr, count, out, timeoutOverrideMs);
}
bool PsbSerialBus::readHoldingRegs(int slaveId, uint16_t addr, uint16_t count, uint16_t* out,
                                   int timeoutOverrideMs) {
    return readRegsInternal(true, slaveId, addr, count, out, timeoutOverrideMs);
}

bool PsbSerialBus::writeRegs(int slaveId, uint16_t addr, uint16_t count, const uint16_t* values,
                             int timeoutOverrideMs) {
    auto it = m_impl->testBoards.find(slaveId);
    if (it != m_impl->testBoards.end()) {
        auto& tb = it->second;
        if (!tb.holdingRegs || addr + count > static_cast<uint16_t>(tb.maxAddr)) {
            m_impl->errorText = "test: address out of range";
            return false;
        }
        memcpy(tb.holdingRegs + addr, values, count * sizeof(uint16_t));
        return true;
    }
    if (!m_impl->connected || !m_impl->port) {
        m_impl->errorText = "not connected";
        return false;
    }
    m_impl->lastSlaveId = slaveId;
    ScopedPortTimeout timeoutGuard(m_impl->port->port(), timeoutOverrideMs);
    auto unit = static_cast<uint8_t>(slaveId);
    for (uint16_t i = 0; i < count; i++) {
        Modbus::StatusCode s;
        do {
            s = m_impl->port->writeSingleRegister(unit, addr + i, values[i]);
            if (Modbus::StatusIsProcessing(s)) std::this_thread::sleep_for(kNonBlockingPollInterval);
        } while (Modbus::StatusIsProcessing(s));
        if (!Modbus::StatusIsGood(s)) {
            m_impl->errorText = std::string("Modbus error: ") + m_impl->port->lastErrorText();
            return false;
        }
    }
    return true;
}

void PsbSerialBus::setFrameCallback(FrameCallback cb) {
    m_impl->frameCb = std::move(cb);
}

std::vector<std::string> PsbSerialBus::scanPorts() {
    std::vector<std::string> result;
    for (const auto& p : Modbus::availableSerialPorts()) {
        std::string path(p);
#if defined(_WIN32)
        if (path.rfind("COM", 0) == 0)
            result.push_back(path);
#else
        // ttyUSB: USB-to-serial adapters (FTDI, CH340, etc.) using a
        // separate UART chip. ttyACM: USB CDC-ACM devices — boards with
        // native USB, and multi-port USB-serial adapters (e.g. WCH
        // "Quad_Serial") that enumerate as ACM rather than USB. Both are
        // real external serial links worth listing; ttyS* (onboard,
        // non-USB serial) stays excluded.
        if (path.rfind("/dev/ttyUSB", 0) == 0 || path.rfind("/dev/ttyACM", 0) == 0)
            result.push_back(path);
#endif
    }
    return result;
}

std::vector<int> PsbSerialBus::availableBaudRates() {
    std::vector<int> result;
    for (auto r : Modbus::availableBaudRate())
        result.push_back(static_cast<int>(r));
    return result;
}

} // namespace psb
