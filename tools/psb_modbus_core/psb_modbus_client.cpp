#include "psb_modbus_client.h"
#include "psb_serial_bus.h"
#include "psb_board_session.h"

namespace psb {

struct PsbModbusClient::Impl {
    std::shared_ptr<PsbSerialBus> bus;
    std::unique_ptr<PsbBoardSession> session;
};

PsbModbusClient::PsbModbusClient() : m_impl(std::make_unique<Impl>()) {
    m_impl->bus = std::make_shared<PsbSerialBus>();
    // Created eagerly, not lazily in connect() — every high-level method
    // below forwards to m_impl->session unconditionally, so it must never
    // be null. A freshly-constructed, never-connected client behaves
    // exactly like today's: every read/write fails cleanly via
    // PsbBoardSession::checkConnected() rather than crashing.
    m_impl->session = std::make_unique<PsbBoardSession>(m_impl->bus, 1);
}
PsbModbusClient::~PsbModbusClient() = default;

// ============================================================================
//  Connection
// ============================================================================

bool PsbModbusClient::connect(const std::string& portName, int baud, int slaveId, int timeoutMs) {
    m_impl->session->disconnect();
    if (!m_impl->bus->connect(portName, baud, timeoutMs)) {
        // Bus-level failure (e.g. "failed to create RTU client port") — no
        // probe was ever attempted, so replace the session (picking up the
        // requested slaveId for a subsequent retry) but don't touch its
        // verified state; lastError() below surfaces the bus's error since
        // the bus itself isn't connected.
        m_impl->session = std::make_unique<PsbBoardSession>(m_impl->bus, slaveId);
        return false;
    }
    m_impl->session = std::make_unique<PsbBoardSession>(m_impl->bus, slaveId);
    if (!m_impl->session->verifyProtocol()) {
        m_impl->bus->disconnect();
        return false;
    }
    return true;
}

void PsbModbusClient::disconnect() {
    m_impl->bus->disconnect();
    m_impl->session->disconnect();
}
bool PsbModbusClient::isConnected() const { return m_impl->session->isConnected(); }
std::string PsbModbusClient::lastError() const {
    // A bus-open failure (before any session ever existed to record its own
    // error) must surface here too, not just a session-level verify
    // failure — see connect()'s early-return path above.
    if (!m_impl->bus->isConnected()) {
        auto busErr = m_impl->bus->lastError();
        if (!busErr.empty()) return busErr;
    }
    return m_impl->session->lastError();
}
int PsbModbusClient::slaveId() const { return m_impl->session->slaveId(); }
int16_t PsbModbusClient::currentUnitExp() const { return m_impl->session->currentUnitExp(); }

void PsbModbusClient::attachTestArrays(uint16_t* inputRegs, uint16_t* holdingRegs, int maxAddr) {
    m_impl->session->attachTestArrays(inputRegs, holdingRegs, maxAddr);
}
void PsbModbusClient::detachTestArrays() {
    m_impl->session->detachTestArrays();
}

// ============================================================================
//  High-level reads — forwarded to the active board session
// ============================================================================

SystemInfo PsbModbusClient::readSystemInfo() { return m_impl->session->readSystemInfo(); }
void PsbModbusClient::readSystemInfo(SystemInfo& out) { m_impl->session->readSystemInfo(out); }

ChannelInfo PsbModbusClient::readChannelInfo(int ch, uint16_t caps) { return m_impl->session->readChannelInfo(ch, caps); }
void PsbModbusClient::readChannelInfo(int ch, uint16_t caps, ChannelInfo& out) { m_impl->session->readChannelInfo(ch, caps, out); }

SystemConfig PsbModbusClient::readSystemConfig() { return m_impl->session->readSystemConfig(); }

ChannelConfig PsbModbusClient::readChannelConfig(int ch, uint16_t caps) { return m_impl->session->readChannelConfig(ch, caps); }
void PsbModbusClient::readChannelConfig(int ch, uint16_t caps, ChannelConfig& out) { m_impl->session->readChannelConfig(ch, caps, out); }

ChannelCalConfig PsbModbusClient::readChannelCalConfig(int ch, uint16_t caps) { return m_impl->session->readChannelCalConfig(ch, caps); }
void PsbModbusClient::readChannelCalConfig(int ch, uint16_t caps, ChannelCalConfig& out) { m_impl->session->readChannelCalConfig(ch, caps, out); }

bool PsbModbusClient::readSystemStatus(SystemInfo& info, int timeoutOverrideMs) { return m_impl->session->readSystemStatus(info, timeoutOverrideMs); }
bool PsbModbusClient::readChannelStatus(int ch, uint16_t caps, ChannelInfo& info, int timeoutOverrideMs) { return m_impl->session->readChannelStatus(ch, caps, info, timeoutOverrideMs); }
bool PsbModbusClient::readChannelCapabilities(int ch, uint16_t& caps, int timeoutOverrideMs) { return m_impl->session->readChannelCapabilities(ch, caps, timeoutOverrideMs); }

void PsbModbusClient::readChannelOutputBlock(int ch, uint16_t caps, ChannelConfig& out) { m_impl->session->readChannelOutputBlock(ch, caps, out); }
void PsbModbusClient::readChannelRecoveryBlock(int ch, ChannelConfig& out) { m_impl->session->readChannelRecoveryBlock(ch, out); }
void PsbModbusClient::readChannelProtectionBlock(int ch, uint16_t caps, ChannelConfig& out) { m_impl->session->readChannelProtectionBlock(ch, caps, out); }
void PsbModbusClient::readChannelDerateBlock(int ch, uint16_t caps, ChannelConfig& out) { m_impl->session->readChannelDerateBlock(ch, caps, out); }
void PsbModbusClient::readChannelOutputEnabledBlock(int ch, uint16_t caps, ChannelConfig& out) { m_impl->session->readChannelOutputEnabledBlock(ch, caps, out); }

// ============================================================================
//  High-level writes — forwarded to the active board session
// ============================================================================

bool PsbModbusClient::writeOperatingMode(OpMode mode) { return m_impl->session->writeOperatingMode(mode); }
bool PsbModbusClient::writeStartupChannelPolicy(uint16_t policy) { return m_impl->session->writeStartupChannelPolicy(policy); }
bool PsbModbusClient::writeSlaveAddress(uint16_t addr) { return m_impl->session->writeSlaveAddress(addr); }
bool PsbModbusClient::writeBaudRateCode(uint16_t code) { return m_impl->session->writeBaudRateCode(code); }
bool PsbModbusClient::sendParamAction(int chScope, ParamAction action) { return m_impl->session->sendParamAction(chScope, action); }

bool PsbModbusClient::writeConfiguredTargetVoltage(int ch, int16_t raw) { return m_impl->session->writeConfiguredTargetVoltage(ch, raw); }
bool PsbModbusClient::writeOutputEnabled(int ch, bool enabled) { return m_impl->session->writeOutputEnabled(ch, enabled); }
bool PsbModbusClient::sendOutputAction(int ch, OutputAction action) { return m_impl->session->sendOutputAction(ch, action); }
bool PsbModbusClient::sendChannelFaultCommand(int ch, ChannelFaultCommand cmd) { return m_impl->session->sendChannelFaultCommand(ch, cmd); }
bool PsbModbusClient::writeRampUp(int ch, uint16_t stepRaw, uint16_t interval) { return m_impl->session->writeRampUp(ch, stepRaw, interval); }
bool PsbModbusClient::writeRampDown(int ch, uint16_t stepRaw, uint16_t interval) { return m_impl->session->writeRampDown(ch, stepRaw, interval); }
bool PsbModbusClient::writeChannelRecovery(int ch, RecoveryPolicy policy, int delay, int max, int window) { return m_impl->session->writeChannelRecovery(ch, policy, delay, max, window); }
bool PsbModbusClient::writeChannelSafeBand(int ch, uint16_t pct) { return m_impl->session->writeChannelSafeBand(ch, pct); }
bool PsbModbusClient::writeCurrentProtection(int ch, ProtectionMode mode, OutputAction action, int16_t thresholdRaw) { return m_impl->session->writeCurrentProtection(ch, mode, action, thresholdRaw); }
bool PsbModbusClient::writeDerateStep(int ch, uint16_t stepRaw) { return m_impl->session->writeDerateStep(ch, stepRaw); }
bool PsbModbusClient::writeCalibrationOutput(int ch, uint16_t k, int16_t b) { return m_impl->session->writeCalibrationOutput(ch, k, b); }
bool PsbModbusClient::writeCalibrationMeasV(int ch, uint16_t k, int16_t b) { return m_impl->session->writeCalibrationMeasV(ch, k, b); }
bool PsbModbusClient::writeCalibrationMeasI(int ch, uint16_t k, int16_t b) { return m_impl->session->writeCalibrationMeasI(ch, k, b); }
bool PsbModbusClient::writeCalibrationOutputExp(int ch, int16_t exp) { return m_impl->session->writeCalibrationOutputExp(ch, exp); }
bool PsbModbusClient::writeCalibrationMeasVExp(int ch, int16_t exp) { return m_impl->session->writeCalibrationMeasVExp(ch, exp); }
bool PsbModbusClient::writeCalibrationMeasIExp(int ch, int16_t exp) { return m_impl->session->writeCalibrationMeasIExp(ch, exp); }

// ============================================================================
//  Calibration Mode operations (v2.1)
// ============================================================================

bool PsbModbusClient::unlockCalibrationStep(uint16_t value) { return m_impl->session->unlockCalibrationStep(value); }
bool PsbModbusClient::enterCalibrationMode() { return m_impl->session->enterCalibrationMode(); }
bool PsbModbusClient::exitCalibrationMode() { return m_impl->session->exitCalibrationMode(); }
bool PsbModbusClient::writeCalibrationOutputEnable(int ch, bool enable) { return m_impl->session->writeCalibrationOutputEnable(ch, enable); }
bool PsbModbusClient::writeRawDacCode(int ch, uint16_t code) { return m_impl->session->writeRawDacCode(ch, code); }
bool PsbModbusClient::sendCalibrationSampleCommand(int ch) { return m_impl->session->sendCalibrationSampleCommand(ch); }
bool PsbModbusClient::sendCalibrationCommitCommand(int ch) { return m_impl->session->sendCalibrationCommitCommand(ch); }
CalibrationSnapshot PsbModbusClient::readCalibrationSnapshot(int ch) { return m_impl->session->readCalibrationSnapshot(ch); }

// ============================================================================
//  Low-level / misc
// ============================================================================

bool PsbModbusClient::readInputRegs(uint16_t addr, uint16_t count, uint16_t* out) { return m_impl->session->readInputRegs(addr, count, out); }
bool PsbModbusClient::readHoldingRegs(uint16_t addr, uint16_t count, uint16_t* out) { return m_impl->session->readHoldingRegs(addr, count, out); }
bool PsbModbusClient::writeReg16(uint16_t addr, uint16_t value) { return m_impl->session->writeReg16(addr, value); }

void PsbModbusClient::setFrameCallback(FrameCallback cb) {
    // The bus's callback is tagged with slaveId (multiple boards can share
    // one bus); PsbModbusClient's single-board API predates that and never
    // needed it, so this adapter just drops the tag.
    m_impl->bus->setFrameCallback([cb](int /*slaveId*/, bool tx, const std::vector<uint8_t>& data) {
        if (cb) cb(tx, data);
    });
}

std::vector<std::string> PsbModbusClient::scanPorts() { return PsbSerialBus::scanPorts(); }
std::vector<int> PsbModbusClient::availableBaudRates() { return PsbSerialBus::availableBaudRates(); }

} // namespace psb
