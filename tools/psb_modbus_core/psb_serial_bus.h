#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace psb {

// Owns exactly one physical serial connection and serializes every
// transaction on it by slave ID — the correct model for RS-485 multi-drop
// (one wire, many boards addressed by slave ID) as well as the single-board
// case (one bus, one board). Not thread-safe: exactly one thread must drive
// a given bus at a time, mirroring PsbModbusClient's historical contract —
// see docs/guide/client-architecture-and-pitfalls.md §2.1. PsbBoardSession
// is the per-board handle that uses this.
class PsbSerialBus {
public:
    using FrameCallback = std::function<void(int slaveId, bool tx, const std::vector<uint8_t>& data)>;

    PsbSerialBus();
    ~PsbSerialBus();

    bool connect(const std::string& port, int baud = 115200, int timeoutMs = 500);
    void disconnect();
    bool isConnected() const;
    std::string lastError() const;

    // Test mode — direct array access per slave ID, bypasses Modbus RTU
    // entirely. Lets tests simulate multiple distinct boards sharing one
    // bus without a real serial port. Independent of connect()/disconnect().
    void attachTestArrays(int slaveId, uint16_t* inputRegs, uint16_t* holdingRegs, int maxAddr);
    void detachTestArrays(int slaveId);

    // Serialized transaction primitives — not thread-safe; caller (one
    // worker thread per bus) must serialize its own access. Every
    // PsbBoardSession attached to this bus routes through these.
    bool readInputRegs(int slaveId, uint16_t addr, uint16_t count, uint16_t* out,
                        int timeoutOverrideMs = -1);
    bool readHoldingRegs(int slaveId, uint16_t addr, uint16_t count, uint16_t* out,
                          int timeoutOverrideMs = -1);
    bool writeRegs(int slaveId, uint16_t addr, uint16_t count, const uint16_t* values,
                   int timeoutOverrideMs = -1);

    void setFrameCallback(FrameCallback cb);

    static std::vector<std::string> scanPorts();
    static std::vector<int> availableBaudRates();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;

    bool readRegsInternal(bool holding, int slaveId, uint16_t addr, uint16_t count, uint16_t* out,
                          int timeoutOverrideMs);
};

} // namespace psb
