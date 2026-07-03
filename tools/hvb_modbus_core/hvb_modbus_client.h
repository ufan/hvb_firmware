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
    SystemInfo        readSystemInfo();
    ChannelInfo       readChannelInfo(int ch, uint16_t caps = 0);
    SystemConfig      readSystemConfig();
    ChannelConfig     readChannelConfig(int ch, uint16_t caps = 0);
    ChannelCalConfig  readChannelCalConfig(int ch, uint16_t caps = 0);

    // Lightweight poll helpers — read only dynamic registers,
    // merging into a cached struct populated by a prior full scan.
    void readSystemStatus(SystemInfo& info);
    void readChannelStatus(int ch, uint16_t caps, ChannelInfo& info);

    // High-level writes — system
    bool writeOperatingMode(OpMode mode);
    bool writeStartupChannelPolicy(uint16_t policy);
    bool writeSlaveAddress(uint16_t addr);
    bool writeBaudRateCode(uint16_t code);
    bool sendParamAction(int chScope, ParamAction action);

    // High-level writes — channel
    bool writeConfiguredTargetVoltage(int ch, int16_t raw);
    bool sendOutputAction(int ch, OutputAction action);
    bool sendChannelFaultCommand(int ch, ChannelFaultCommand cmd);
    bool writeRampUp(int ch, uint16_t stepRaw, uint16_t interval);
    bool writeRampDown(int ch, uint16_t stepRaw, uint16_t interval);
    bool writeChannelRecovery(int ch, RecoveryPolicy policy, int delay, int max, int window);
    bool writeChannelSafeBand(int ch, uint16_t pct);
    bool writeCurrentProtection(int ch, ProtectionMode mode, OutputAction action, int16_t thresholdRaw);
    bool writeDerateStep(int ch, uint16_t stepRaw);
    bool writeCalibrationOutput(int ch, uint16_t k, int16_t b);
    bool writeCalibrationMeasV(int ch, uint16_t k, int16_t b);
    bool writeCalibrationMeasI(int ch, uint16_t k, int16_t b);

    // Calibration Mode operations (v2.1)
    bool unlockCalibrationStep(uint16_t value);
    bool enterCalibrationMode();
    bool exitCalibrationMode();
    bool writeCalibrationOutputEnable(int ch, bool enable);
    bool writeRawDacCode(int ch, uint16_t code);
    bool sendCalibrationSampleCommand(int ch);
    bool sendCalibrationCommitCommand(int ch);
    CalibrationSnapshot readCalibrationSnapshot(int ch);

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
    // timeoutOverrideMs >= 0 temporarily replaces the port's response timeout
    // for just this request (restored afterward); -1 keeps the port's current
    // timeout unchanged.
    bool writeRegsInternal(uint16_t addr, uint16_t count, const uint16_t* values,
                            int timeoutOverrideMs = -1);
    bool checkConnected();
};

} // namespace hvb
