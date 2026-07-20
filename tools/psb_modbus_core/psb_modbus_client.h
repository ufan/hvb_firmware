#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "types.h"

namespace psb {

class PsbModbusClient {
public:
    using FrameCallback = std::function<void(bool tx, const std::vector<uint8_t>& data)>;

    PsbModbusClient();
    ~PsbModbusClient();

    bool connect(const std::string& port, int baud = 115200, int slaveId = 1, int timeoutMs = 500);
    void disconnect();
    bool isConnected() const;
    std::string lastError() const;
    int slaveId() const;
    // Decimal exponent for MEASURED_CURRENT/CURRENT_LIMIT_THRESHOLD registers
    // (10^exp amperes/LSB), cached from the last successful readSystemInfo()
    // call — -10 (0.1nA/LSB) before any connection, matching every pre-v3.2
    // board. Formatting helpers (currentToA/currentFromA in register_map.h)
    // need this to interpret a raw current register correctly; reading it
    // fresh on every format call would mean an extra Modbus round-trip per
    // display, so it's cached here instead.
    int16_t currentUnitExp() const;

    // Direct test mode — inject register arrays (bypasses Modbus RTU)
    void attachTestArrays(uint16_t* inputRegs, uint16_t* holdingRegs, int maxAddr);
    void detachTestArrays();

    // High-level reads (raw LSB values)
    SystemInfo        readSystemInfo();
    ChannelInfo       readChannelInfo(int ch, uint16_t caps = 0);
    SystemConfig      readSystemConfig();
    ChannelConfig     readChannelConfig(int ch, uint16_t caps = 0);
    ChannelCalConfig  readChannelCalConfig(int ch, uint16_t caps = 0);

    // Lightweight poll helpers — read only dynamic registers, merging into a
    // cached struct populated by a prior full scan. Return whether the read
    // actually succeeded (false = transient failure — the cached struct is
    // left untouched), so a caller polling in a loop can track consecutive
    // failures per channel. `timeoutOverrideMs` (>= 0) temporarily replaces
    // the port's response timeout for just this call — a routine poll that
    // fails is expected to just retry next cycle, so it's worth failing fast
    // (a short override) rather than blocking the whole poll loop for
    // however long the port's normal timeout is; -1 keeps it unchanged.
    bool readSystemStatus(SystemInfo& info, int timeoutOverrideMs = -1);
    bool readChannelStatus(int ch, uint16_t caps, ChannelInfo& info, int timeoutOverrideMs = -1);

    // Single-register capability-flags probe — every other read/write in
    // this class takes `caps` as a cached, already-known input rather than
    // fetching it (by design, to avoid an extra transaction on every call).
    // This exists purely so a caller can re-probe a channel whose caps were
    // never captured correctly in the first place (e.g. a connect-time
    // transient failure left it at 0, the "unknown"/default value — no real
    // channel legitimately has zero capability bits).
    bool readChannelCapabilities(int ch, uint16_t& caps, int timeoutOverrideMs = -1);

    // Merge-on-success variants — like readChannelStatus() above, only the
    // fields of a successfully-read sub-block are written into `out`; a
    // transient failure on one sub-block leaves the caller's existing value
    // for that field untouched instead of resetting it to a default. Use
    // these (not the value-returning overloads above) for any caller that
    // keeps a long-lived cache and refreshes it repeatedly — e.g. the TUI.
    void readChannelConfig(int ch, uint16_t caps, ChannelConfig& out);
    void readChannelCalConfig(int ch, uint16_t caps, ChannelCalConfig& out);
    // readSystemInfo/readChannelInfo are the two other connect-time/on-demand
    // reads that share the value-returning family's reset-on-failure risk —
    // any caller that re-invokes these against a live cache (a GUI "Refresh"
    // action, not just the one-shot connect read) should use these instead.
    void readSystemInfo(SystemInfo& out);
    void readChannelInfo(int ch, uint16_t caps, ChannelInfo& out);

    // Per-block reads — the same Modbus transactions readChannelConfig()
    // performs internally, exposed individually so a caller that knows
    // exactly which block a write touched can re-read just that block
    // instead of the whole channel config. Each merges on success like the
    // reference-taking readChannelConfig() above. Caller must supply the
    // channel's already-known capability flags (unlike readChannelConfig(),
    // these don't fetch them internally — every narrow-refresh call site
    // already has them cached from a prior full scan).
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
    // Decimal exponent (k_exp): gain = k * 10^k_exp. Valid range [-9, 4].
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

    void setFrameCallback(FrameCallback cb);

    static std::vector<std::string> scanPorts();
    static std::vector<int> availableBaudRates();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    // timeoutOverrideMs >= 0 temporarily replaces the port's response timeout
    // for just this request (restored afterward); -1 keeps the port's current
    // timeout unchanged.
    bool readRegsInternal(bool holding, uint16_t addr, uint16_t count, uint16_t* out,
                          int timeoutOverrideMs = -1);
    bool writeRegsInternal(uint16_t addr, uint16_t count, const uint16_t* values,
                            int timeoutOverrideMs = -1);
    bool checkConnected();
};

} // namespace psb
