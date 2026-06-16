#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "types.h"

namespace hvb {

class HvbModbusClient {
public:
    using FrameCallback = std::function<void(bool tx, const std::vector<uint8_t>& data)>;

    HvbModbusClient();
    ~HvbModbusClient();

    bool connect(const std::string& port, int baud = 115200, int slaveId = 1, int timeoutMs = 500);
    void disconnect();
    bool isConnected() const;
    std::string lastError() const;
    int slaveId() const;

    // Direct test mode — inject register arrays (bypasses Modbus RTU)
    void attachTestArrays(uint16_t* inputRegs, uint16_t* holdingRegs, int maxAddr);
    void detachTestArrays();

    // High-level reads (raw LSB values)
    SystemInfo     readSystemInfo();
    ChannelInfo    readChannelInfo(int ch);
    SystemConfig   readSystemConfig();
    ChannelConfig  readChannelConfig(int ch);

    // High-level writes
    bool writeOperatingMode(OpMode mode);
    bool writeSlaveAddress(uint16_t addr);
    bool writeBaudRateCode(uint16_t code);
    bool writeSystemRecoveryPolicy(RecoveryPolicy policy, int delay, int max, int window);
    bool writeSafeBands(uint16_t voltagePct, uint16_t currentPct);
    bool sendParamAction(int chScope, ParamAction action);

    bool writeConfiguredTargetVoltage(int ch, int16_t raw);
    bool sendOutputAction(int ch, OutputAction action);
    bool sendChannelFaultCommand(int ch, ChannelFaultCommand cmd);
    bool writeRampUp(int ch, uint16_t stepRaw, uint16_t interval);
    bool writeRampDown(int ch, uint16_t stepRaw, uint16_t interval);
    bool writeVoltageProtection(int ch, ProtectionMode mode, OutputAction action, int16_t thresholdRaw);
    bool writeCurrentProtection(int ch, ProtectionMode mode, OutputAction action, int16_t thresholdRaw);
    bool writeDerateStep(int ch, uint16_t stepRaw);
    bool writeSaveTargetPolicy(int ch, bool saveTarget);
    bool writeCalibrationOutput(int ch, uint16_t k, int16_t b);
    bool writeCalibrationMeasV(int ch, uint16_t k, int16_t b);
    bool writeCalibrationMeasI(int ch, uint16_t k, int16_t b);

    // Low-level
    bool readInputRegs(uint16_t addr, uint16_t count, uint16_t* out);
    bool readHoldingRegs(uint16_t addr, uint16_t count, uint16_t* out);
    bool writeReg16(uint16_t addr, uint16_t value);

    void setFrameCallback(FrameCallback cb);

    static std::vector<std::string> scanPorts();
    static std::vector<int> availableBaudRates();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    bool readRegsInternal(bool holding, uint16_t addr, uint16_t count, uint16_t* out);
    bool writeRegsInternal(uint16_t addr, uint16_t count, const uint16_t* values);
    bool checkConnected();
};

} // namespace hvb
