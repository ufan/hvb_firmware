#include "virtual_board.h"
#include "register_map.h"

#include <ModbusServerPort.h>
#include <Modbus.h>
#include <cstring>
#include <thread>
#include <chrono>

namespace hvb::test {

struct VirtualBoard::Impl {
    ModbusServerPort* server = nullptr;
    std::string portName;
    volatile bool running = false;
};

VirtualBoard::VirtualBoard() : m_impl(std::make_unique<Impl>()) {
    setVariantDefaults();
}

VirtualBoard::~VirtualBoard() { stop(); }

void VirtualBoard::setVariantDefaults() {
    memset(inputRegs, 0, sizeof(inputRegs));
    memset(holdingRegs, 0, sizeof(holdingRegs));

    // System input (v3 protocol)
    inputRegs[reg::sysAddr(0) + SYS_PROTOCOL_MAJOR] = 3;
    inputRegs[reg::sysAddr(0) + SYS_PROTOCOL_MINOR] = 0;
    inputRegs[reg::sysAddr(0) + SYS_VARIANT_ID] = 1;
    inputRegs[reg::sysAddr(0) + SYS_CAPABILITY_FLAGS] = SYS_CAP_AUTOMATIC_MODE | SYS_CAP_ENV_SENSOR | SYS_CAP_CALIBRATION_MODE;
    inputRegs[reg::sysAddr(0) + SYS_SUPPORTED_CHANNELS] = 2;
    inputRegs[reg::sysAddr(0) + SYS_ACTIVE_CHANNEL_MASK] = 0x0003;
    inputRegs[reg::sysAddr(0) + SYS_BOARD_TEMPERATURE] = 254;  // 25.4 °C
    inputRegs[reg::sysAddr(0) + SYS_BOARD_HUMIDITY] = 452;     // 45.2 %
    inputRegs[reg::sysAddr(0) + SYS_ACTIVE_OPERATING_MODE] = 0;

    // System holding
    holdingRegs[reg::sysAddr(SYS_OPERATING_MODE)] = 0;
    holdingRegs[reg::sysAddr(SYS_STARTUP_CHANNEL_POLICY)] = 0;
    holdingRegs[reg::sysAddr(SYS_SLAVE_ADDRESS)] = 1;
    /* v3: recovery policy / safe-band fields moved to per-channel holding registers */

    // Channel defaults (CH0 and CH1)
    for (int ch = 0; ch < 2; ++ch) {
        uint16_t base = reg::chAddr(ch, 0);
        holdingRegs[base + CH_CFG_TARGET_VOLTAGE] = 0;
        holdingRegs[base + CH_OUTPUT_ACTION] = 0;
        holdingRegs[base + CH_FAULT_CMD] = 0;
        holdingRegs[base + CH_CURRENT_LIMIT_THRESHOLD] = 32767;
        holdingRegs[base + CH_OUTPUT_CAL_K] = 10000;
        holdingRegs[base + CH_OUTPUT_CAL_B] = 0;
        holdingRegs[base + CH_MEASURED_V_CAL_K] = 10000;
        holdingRegs[base + CH_MEASURED_V_CAL_B] = 0;
        holdingRegs[base + CH_MEASURED_I_CAL_K] = 10000;
        holdingRegs[base + CH_MEASURED_I_CAL_B] = 0;
        holdingRegs[base + CH_CAL_MAX_RAW_DAC_LIMIT] = 4095;

        inputRegs[base + CH_CAPABILITY_FLAGS] = CH_CAP_OUTPUT_ENABLE | CH_CAP_RAW_OUTPUT_DRIVE | CH_CAP_VOLTAGE_MEASUREMENT;
    }
}

void VirtualBoard::setChannelEnabled(int ch, bool enabled) {
    uint16_t base = reg::chAddr(ch, 0);
    if (enabled) {
        inputRegs[base + CH_MEASURED_VOLTAGE] = static_cast<int16_t>(5000);
        inputRegs[base + CH_MEASURED_CURRENT] = static_cast<int16_t>(1234);
        inputRegs[base + CH_OPER_TARGET_VOLTAGE] = static_cast<int16_t>(5000);
        inputRegs[base + CH_STATUS_BITS] = 0x0003; // OutputDrive + OutputEnable
    } else {
        inputRegs[base + CH_MEASURED_VOLTAGE] = 0;
        inputRegs[base + CH_MEASURED_CURRENT] = 0;
        inputRegs[base + CH_OPER_TARGET_VOLTAGE] = 0;
        inputRegs[base + CH_STATUS_BITS] = 0;
    }
}

// ============================================================================
//  ModbusInterface implementation
// ============================================================================

class BoardDevice : public ModbusInterface {
    VirtualBoard* board;
public:
    BoardDevice(VirtualBoard* b) : board(b) {}

    Modbus::StatusCode readInputRegisters(uint8_t /*unit*/, uint16_t offset, uint16_t count, uint16_t* values) override {
        if (offset + count > 280) return Modbus::Status_BadIllegalDataAddress;
        for (uint16_t i = 0; i < count; ++i)
            values[i] = board->inputRegs[offset + i];
        return Modbus::Status_Good;
    }

    Modbus::StatusCode readHoldingRegisters(uint8_t /*unit*/, uint16_t offset, uint16_t count, uint16_t* values) override {
        if (offset + count > 280) return Modbus::Status_BadIllegalDataAddress;
        // Emulate 12-reg read limit
        if (count > 12) return Modbus::Status_BadIllegalDataAddress;
        for (uint16_t i = 0; i < count; ++i)
            values[i] = board->holdingRegs[offset + i];
        return Modbus::Status_Good;
    }

    Modbus::StatusCode writeSingleRegister(uint8_t /*unit*/, uint16_t offset, uint16_t value) override {
        if (offset >= 280) return Modbus::Status_BadIllegalDataAddress;
        // Reserved checks
        if ((offset >= 9 && offset <= 38) && (offset & 1)) return Modbus::Status_BadIllegalDataAddress; // simplify
        board->holdingRegs[offset] = value;
        return Modbus::Status_Good;
    }

    Modbus::StatusCode writeMultipleRegisters(uint8_t /*unit*/, uint16_t offset, uint16_t count, const uint16_t* values) override {
        if (offset + count > 280) return Modbus::Status_BadIllegalDataAddress;
        if (count > 12) return Modbus::Status_BadIllegalDataAddress;
        for (uint16_t i = 0; i < count; ++i)
            board->holdingRegs[offset + i] = values[i];
        return Modbus::Status_Good;
    }
};

// ============================================================================
//  Server start/stop
// ============================================================================

bool VirtualBoard::start(const std::string& port, int slaveId) {
    (void)slaveId;
    m_impl->portName = port;

    Modbus::SerialSettings settings;
    settings.portName = port.c_str();
    settings.baudRate = 115200;
    settings.dataBits = 8;
    settings.parity = Modbus::NoParity;
    settings.stopBits = Modbus::OneStop;
    settings.flowControl = Modbus::NoFlowControl;
    settings.timeoutFirstByte = 500;
    settings.timeoutInterByte = 50;

    auto* device = new BoardDevice(this);

    // ModbusLib server — RTU, non-blocking
    m_impl->server = Modbus::createServerPort(device, Modbus::RTU, &settings, false);
    if (!m_impl->server) {
        delete device;
        return false;
    }

    m_impl->running = true;
    return true;
}

void VirtualBoard::stop() {
    m_impl->running = false;
    if (m_impl->server) {
        delete m_impl->server;
        m_impl->server = nullptr;
    }
}

bool VirtualBoard::isRunning() const {
    return m_impl->running;
}

uint16_t VirtualBoard::readInputReg(uint16_t addr) {
    if (addr >= 280) return 0;
    return inputRegs[addr];
}

uint16_t VirtualBoard::readHoldingReg(uint16_t addr) {
    if (addr >= 280) return 0;
    return holdingRegs[addr];
}

void VirtualBoard::writeHoldingReg(uint16_t addr, uint16_t value) {
    if (addr >= 280) return;
    holdingRegs[addr] = value;
}

} // namespace hvb::test
