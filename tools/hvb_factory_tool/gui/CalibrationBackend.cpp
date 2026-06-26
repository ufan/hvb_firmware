#include "CalibrationBackend.h"
#include "types.h"
#include "register_map.h"

namespace hvb::factory {

CalibrationBackend::CalibrationBackend(QObject* parent) : QObject(parent) {}
CalibrationBackend::~CalibrationBackend() = default;

bool CalibrationBackend::connected() const { return m_client.isConnected(); }
bool CalibrationBackend::calUnlocked() const { return m_calUnlocked; }
bool CalibrationBackend::calActive() const { return m_calActive; }
int CalibrationBackend::activeChannel() const { return m_activeChannel; }
QStringList CalibrationBackend::ports() const { return m_ports; }
QString CalibrationBackend::statusMessage() const { return m_statusMessage; }

void CalibrationBackend::setActiveChannel(int ch) {
    if (m_activeChannel != ch) { m_activeChannel = ch; emit activeChannelChanged(); }
}

void CalibrationBackend::scanPorts() {
    m_ports.clear();
    for (const auto& p : HvbModbusClient::scanPorts())
        m_ports.append(QString::fromStdString(p));
    emit portsChanged();
}

void CalibrationBackend::connectToDevice(const QString& port, int baud, int slaveId) {
    if (m_client.connect(port.toStdString(), baud, slaveId)) {
        m_statusMessage = "Connected";
    } else {
        m_statusMessage = QString::fromStdString(m_client.lastError());
    }
    emit connectedChanged();
    emit statusMessageChanged();
}

void CalibrationBackend::disconnectFromDevice() {
    m_client.disconnect();
    m_calUnlocked = false;
    m_calActive = false;
    emit connectedChanged();
    emit calStateChanged();
}

void CalibrationBackend::unlockStep1() {
    if (m_client.unlockCalibrationStep(CAL_UNLOCK_STEP1))
        m_statusMessage = "Unlock step 1 OK";
    else
        m_statusMessage = QString::fromStdString(m_client.lastError());
    emit statusMessageChanged();
}

void CalibrationBackend::unlockStep2() {
    if (m_client.unlockCalibrationStep(CAL_UNLOCK_STEP2)) {
        m_statusMessage = "Unlock step 2 OK";
        m_calUnlocked = true;
        emit calStateChanged();
    } else {
        m_statusMessage = QString::fromStdString(m_client.lastError());
    }
    emit statusMessageChanged();
}

void CalibrationBackend::enterCalibrationMode() {
    if (m_client.enterCalibrationMode()) {
        m_calActive = true;
        m_statusMessage = "Calibration Mode active";
        emit calStateChanged();
    } else {
        m_statusMessage = QString::fromStdString(m_client.lastError());
    }
    emit statusMessageChanged();
}

void CalibrationBackend::exitCalibrationMode() {
    if (m_client.exitCalibrationMode()) {
        m_calActive = false;
        m_calUnlocked = false;
        m_statusMessage = "Exited calibration mode";
        emit calStateChanged();
    } else {
        m_statusMessage = QString::fromStdString(m_client.lastError());
    }
    emit statusMessageChanged();
}

void CalibrationBackend::enableOutput(bool enable) {
    if (m_client.writeCalibrationOutputEnable(m_activeChannel, enable))
        m_statusMessage = enable ? "Output enabled" : "Output disabled";
    else
        m_statusMessage = QString::fromStdString(m_client.lastError());
    emit statusMessageChanged();
}

void CalibrationBackend::writeRawDac(int code) {
    if (m_client.writeRawDacCode(m_activeChannel, static_cast<uint16_t>(code)))
        m_statusMessage = "DAC = " + QString::number(code);
    else
        m_statusMessage = QString::fromStdString(m_client.lastError());
    emit statusMessageChanged();
}

void CalibrationBackend::triggerSample() {
    if (m_client.sendCalibrationSampleCommand(m_activeChannel))
        m_statusMessage = "Sample triggered";
    else
        m_statusMessage = QString::fromStdString(m_client.lastError());
    emit statusMessageChanged();
}

void CalibrationBackend::commitChannel() {
    if (m_client.sendCalibrationCommitCommand(m_activeChannel))
        m_statusMessage = "Committed CH" + QString::number(m_activeChannel);
    else
        m_statusMessage = QString::fromStdString(m_client.lastError());
    emit statusMessageChanged();
}

void CalibrationBackend::safeAll() {
    for (int ch = 0; ch < 2; ++ch) {
        m_client.writeRawDacCode(ch, 0);
        m_client.writeCalibrationOutputEnable(ch, false);
    }
    m_statusMessage = "All outputs safe";
    emit statusMessageChanged();
}

void CalibrationBackend::writeCoefficients(const QString& type, double k, double b) {
    auto kRaw = static_cast<uint16_t>(k);
    auto bRaw = static_cast<int16_t>(b);
    bool ok = false;
    if (type == "out") ok = m_client.writeCalibrationOutput(m_activeChannel, kRaw, bRaw);
    else if (type == "meas-v") ok = m_client.writeCalibrationMeasV(m_activeChannel, kRaw, bRaw);
    else if (type == "meas-i") ok = m_client.writeCalibrationMeasI(m_activeChannel, kRaw, bRaw);
    m_statusMessage = ok ? "Coefficients written" : QString::fromStdString(m_client.lastError());
    emit statusMessageChanged();
}

void CalibrationBackend::refreshSnapshot() {
    auto snap = m_client.readCalibrationSnapshot(m_activeChannel);
    QVariantMap map;
    map["outputEnabled"] = snap.outputEnabled;
    map["rawDacCode"] = snap.rawDacCode;
    map["maxRawDacLimit"] = snap.maxRawDacLimit;
    map["rawDacReadback"] = snap.rawDacReadback;
    map["rawAdcVoltage"] = snap.rawAdcVoltage;
    map["rawAdcCurrent"] = snap.rawAdcCurrent;
    emit snapshotUpdated(map);
}

} // namespace hvb::factory
