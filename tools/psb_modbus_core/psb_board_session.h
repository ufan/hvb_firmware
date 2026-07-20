#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "types.h"

namespace psb {

class PsbSerialBus;

// One board (one slave ID) on a PsbSerialBus. Every high-level read/write
// method below is mechanically identical to PsbModbusClient's original,
// pre-split implementation — the only things that changed are how a
// transaction reaches the wire (now via m_bus, keyed by slave ID) and
// connection setup (now a constructor + verifyProtocol(), since the bus —
// not this class — owns the physical port).
class PsbBoardSession {
public:
    PsbBoardSession(std::shared_ptr<PsbSerialBus> bus, int slaveId);
    ~PsbBoardSession();

    // Reads PROTOCOL_MAJOR/MINOR (one transaction) and validates
    // compatibility — the same probe PsbModbusClient::connect() used to run
    // inline. Callers decide when to run it: PsbModbusClient's facade runs
    // it once right after construction (mirroring today's connect()
    // contract); a future topology scan can run it repeatedly against
    // different candidate slave IDs on an already-connected bus without
    // reconnecting anything.
    // timeoutOverrideMs: -1 uses the bus's normal timeout (today's
    // behavior, unchanged for every existing caller). A short override is
    // for the setup wizard's slave-ID scan (wizard_scan.h) — sweeping up to
    // ~32 candidate IDs at the bus's normal multi-second timeout would make
    // a scan take minutes; a non-responding ID is the overwhelmingly common
    // case during a sweep and must fail fast.
    bool verifyProtocol(int timeoutOverrideMs = -1);
    void disconnect();
    bool isConnected() const;
    std::string lastError() const;
    int slaveId() const;
    int16_t currentUnitExp() const;

    // Changes which slave ID this session addresses on its bus, in place —
    // same object, same address, so existing references to it (e.g.
    // AppState::client in psb_demo_tui) stay valid. Resets verified state:
    // the previous verifyProtocol() result was for the old slave ID and
    // doesn't apply to the new one. Used by the TUI's connection modal to
    // let the user correct a wrong slave ID and reconnect without
    // restarting the session that references this object.
    void rebind(int slaveId);

    // Direct test mode — inject register arrays (bypasses Modbus RTU),
    // scoped to this session's slave ID on its bus. Unlike verifyProtocol(),
    // this marks the session connected immediately without probing —
    // matches PsbModbusClient::attachTestArrays()'s existing contract.
    void attachTestArrays(uint16_t* inputRegs, uint16_t* holdingRegs, int maxAddr);
    void detachTestArrays();

    // High-level reads (raw LSB values)
    SystemInfo        readSystemInfo();
    ChannelInfo       readChannelInfo(int ch, uint16_t caps = 0);
    SystemConfig      readSystemConfig();
    ChannelConfig     readChannelConfig(int ch, uint16_t caps = 0);
    ChannelCalConfig  readChannelCalConfig(int ch, uint16_t caps = 0);

    bool readSystemStatus(SystemInfo& info, int timeoutOverrideMs = -1);
    bool readChannelStatus(int ch, uint16_t caps, ChannelInfo& info, int timeoutOverrideMs = -1);
    bool readChannelCapabilities(int ch, uint16_t& caps, int timeoutOverrideMs = -1);

    void readChannelConfig(int ch, uint16_t caps, ChannelConfig& out);
    void readChannelCalConfig(int ch, uint16_t caps, ChannelCalConfig& out);
    void readSystemInfo(SystemInfo& out);
    void readChannelInfo(int ch, uint16_t caps, ChannelInfo& out);

    void readChannelOutputBlock(int ch, uint16_t caps, ChannelConfig& out);
    void readChannelRecoveryBlock(int ch, ChannelConfig& out);
    void readChannelProtectionBlock(int ch, uint16_t caps, ChannelConfig& out);
    void readChannelDerateBlock(int ch, uint16_t caps, ChannelConfig& out);
    void readChannelOutputEnabledBlock(int ch, uint16_t caps, ChannelConfig& out);

    // High-level writes — system
    bool writeOperatingMode(OpMode mode);
    bool writeStartupChannelPolicy(uint16_t policy);
    bool writeSlaveAddress(uint16_t addr);
    bool writeBaudRateCode(uint16_t code);
    bool sendParamAction(int chScope, ParamAction action);

    // High-level writes — channel
    bool writeConfiguredTargetVoltage(int ch, int16_t raw);
    bool writeOutputEnabled(int ch, bool enabled);
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
    bool writeCalibrationOutputExp(int ch, int16_t exp);
    bool writeCalibrationMeasVExp(int ch, int16_t exp);
    bool writeCalibrationMeasIExp(int ch, int16_t exp);

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

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    bool readRegsInternal(bool holding, uint16_t addr, uint16_t count, uint16_t* out,
                          int timeoutOverrideMs = -1);
    bool writeRegsInternal(uint16_t addr, uint16_t count, const uint16_t* values,
                            int timeoutOverrideMs = -1);
    bool checkConnected();
};

} // namespace psb
