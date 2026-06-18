#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hvb::test {

// Full Modbus RTU server emulating HVB board register map (hvb_regs.h)
class VirtualBoard {
public:
    VirtualBoard();
    ~VirtualBoard();

    bool start(const std::string& port, int slaveId = 1);
    void stop();
    bool isRunning() const;

    // Register access helpers for tests
    uint16_t readInputReg(uint16_t addr);
    uint16_t readHoldingReg(uint16_t addr);
    void writeHoldingReg(uint16_t addr, uint16_t value);

    // Pre-populate for specific test scenarios
    void setVariantDefaults();
    void setChannelEnabled(int ch, bool enabled);

    friend class BoardDevice;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;

    uint16_t inputRegs[280] = {};
    uint16_t holdingRegs[280] = {};
};

} // namespace hvb::test
