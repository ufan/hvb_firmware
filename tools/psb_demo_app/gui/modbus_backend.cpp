#include "modbus_backend.h"
#include "modbus_worker.h"

ModbusBackend::ModbusBackend(QObject* parent)
    : QObject(parent)
{
    m_thread = new QThread(this);
    m_worker = new ModbusWorker();
    m_worker->moveToThread(m_thread);

    // Worker signals → Backend slots
    connect(m_worker, &ModbusWorker::connected, this, &ModbusBackend::onConnected);
    connect(m_worker, &ModbusWorker::disconnected, this, &ModbusBackend::onDisconnected);
    connect(m_worker, &ModbusWorker::systemInfoReady, this, &ModbusBackend::onSysInfoReady);
    connect(m_worker, &ModbusWorker::channelInfoReady, this, &ModbusBackend::onChInfoReady);
    connect(m_worker, &ModbusWorker::systemConfigReady, this, &ModbusBackend::onSysConfigReady);
    connect(m_worker, &ModbusWorker::channelConfigReady, this, &ModbusBackend::onChConfigReady);
    connect(m_worker, &ModbusWorker::operationComplete, this, &ModbusBackend::onOperationComplete);
    connect(m_worker, &ModbusWorker::portsScanned, this, &ModbusBackend::onPortsScanned);
    connect(m_worker, &ModbusWorker::rawFrameLog, this, &ModbusBackend::onRawFrameLog);
    connect(m_worker, &ModbusWorker::rawHexResult, this, &ModbusBackend::onRawHexResult);

    // Poll timer
    m_pollTimer.setInterval(m_pollInterval);
    connect(&m_pollTimer, &QTimer::timeout, this, &ModbusBackend::pollTick);

    // Status auto-clear timer (success messages only)
    m_statusClearTimer.setSingleShot(true);
    connect(&m_statusClearTimer, &QTimer::timeout,
            this, [this] { setStatus(QString()); });

    m_thread->start();

    // Initial port scan
    scanPorts();
}

ModbusBackend::~ModbusBackend()
{
    m_pollTimer.stop();
    m_thread->quit();
    m_thread->wait(2000);
    delete m_worker;
}

// ---------------------------------------------------------------------------
//  Properties
// ---------------------------------------------------------------------------

void ModbusBackend::setSelectedPort(const QString& p) { if (m_selectedPort != p) { m_selectedPort = p; emit selectedPortChanged(); } }
void ModbusBackend::setBaudRate(int b) { if (m_baud != b) { m_baud = b; emit baudRateChanged(); } }
void ModbusBackend::setSlaveId(int id) { if (m_slaveId != id) { m_slaveId = id; emit slaveIdChanged(); } }
void ModbusBackend::setPollIntervalMs(int ms) { if (m_pollInterval != ms) { m_pollInterval = ms; m_pollTimer.setInterval(ms); emit pollIntervalChanged(); } }

int ModbusBackend::channelCount() const
{
    if (!m_connected) return 0;
    int n = m_sysInfo.value("supportedChannels", 0).toInt();
    return (n >= 1 && n <= MAX_CHANNELS) ? n : 0;
}

QVariantList ModbusBackend::channelInfoList() const
{
    QVariantList list;
    int n = channelCount();
    for (int i = 0; i < n; ++i) list << m_chInfo[i];
    return list;
}

QVariantList ModbusBackend::channelConfigList() const
{
    QVariantList list;
    int n = channelCount();
    for (int i = 0; i < n; ++i) list << m_chConfig[i];
    return list;
}

// ---------------------------------------------------------------------------
//  Connection
// ---------------------------------------------------------------------------

void ModbusBackend::connectToDevice()
{
    setStatus("Connecting...");
    QMetaObject::invokeMethod(m_worker, "doConnect", Qt::QueuedConnection,
        Q_ARG(QString, m_selectedPort), Q_ARG(int, m_baud),
        Q_ARG(int, m_slaveId), Q_ARG(int, 3000));
}

void ModbusBackend::disconnectFromDevice()
{
    m_pollTimer.stop();
    QMetaObject::invokeMethod(m_worker, "doDisconnect", Qt::QueuedConnection);
}

void ModbusBackend::scanPorts()
{
    QMetaObject::invokeMethod(m_worker, "doScanPorts", Qt::QueuedConnection);
}

// ---------------------------------------------------------------------------
//  Refresh
// ---------------------------------------------------------------------------

void ModbusBackend::refreshChannels()
{
    int n = channelCount();
    for (int ch = 0; ch < n; ++ch)
        QMetaObject::invokeMethod(m_worker, "doRefreshChannelInfo", Qt::QueuedConnection, Q_ARG(int, ch));
}

void ModbusBackend::refreshAll()
{
    if (!m_connected) return;
    QMetaObject::invokeMethod(m_worker, "doRefreshSystemInfo", Qt::QueuedConnection);
    refreshChannels();
}

void ModbusBackend::pollTick()
{
    if (!m_connected) return;
    QMetaObject::invokeMethod(m_worker, "doPollStatus", Qt::QueuedConnection);
}

// ---------------------------------------------------------------------------
//  System writes
// ---------------------------------------------------------------------------

#define INVOKE(method, ...) \
    QMetaObject::invokeMethod(m_worker, method, Qt::QueuedConnection, ##__VA_ARGS__)

void ModbusBackend::writeOperatingMode(int mode)       { INVOKE("doWriteOperatingMode", Q_ARG(int, mode)); }
void ModbusBackend::writeStartupChannelPolicy(int p)   { INVOKE("doWriteStartupChannelPolicy", Q_ARG(int, p)); }
void ModbusBackend::writeSlaveAddress(int addr)         { INVOKE("doWriteSlaveAddress", Q_ARG(int, addr)); }
void ModbusBackend::writeBaudRate(int code)             { INVOKE("doWriteBaudRate", Q_ARG(int, code)); }
void ModbusBackend::saveSystem()          { INVOKE("doSendParamAction", Q_ARG(int,-1), Q_ARG(int,1)); }
void ModbusBackend::loadSystem()          { INVOKE("doSendParamAction", Q_ARG(int,-1), Q_ARG(int,2)); }
void ModbusBackend::factoryResetSystem()  { INVOKE("doSendParamAction", Q_ARG(int,-1), Q_ARG(int,3)); }
void ModbusBackend::softwareReset()       { INVOKE("doSendParamAction", Q_ARG(int,-1), Q_ARG(int,255)); }

void ModbusBackend::sendOutputAction(int ch, int a)     { INVOKE("doSendOutputAction", Q_ARG(int,ch), Q_ARG(int,a)); }
void ModbusBackend::sendFaultCmd(int ch, int c)         { INVOKE("doSendFaultCmd", Q_ARG(int,ch), Q_ARG(int,c)); }
void ModbusBackend::writeTargetVoltage(int ch, int raw){ INVOKE("doWriteTargetVoltage", Q_ARG(int,ch), Q_ARG(int,raw)); }
void ModbusBackend::writeRampUp(int ch, int s, int i)  { INVOKE("doWriteRampUp", Q_ARG(int,ch), Q_ARG(int,s), Q_ARG(int,i)); }
void ModbusBackend::writeRampDown(int ch, int s, int i){ INVOKE("doWriteRampDown", Q_ARG(int,ch), Q_ARG(int,s), Q_ARG(int,i)); }
void ModbusBackend::writeChannelRecovery(int ch, int p, int d, int m, int w)
    { INVOKE("doWriteChannelRecovery", Q_ARG(int,ch), Q_ARG(int,p), Q_ARG(int,d), Q_ARG(int,m), Q_ARG(int,w)); }
void ModbusBackend::writeChannelSafeBand(int ch, int pct)
    { INVOKE("doWriteChannelSafeBand", Q_ARG(int,ch), Q_ARG(int,pct)); }
void ModbusBackend::writeCurrentProtection(int ch, int m, int a, int t)
    { INVOKE("doWriteCurrentProtection", Q_ARG(int,ch), Q_ARG(int,m), Q_ARG(int,a), Q_ARG(int,t)); }
void ModbusBackend::writeDerateStep(int ch, int s)   { INVOKE("doWriteDerateStep", Q_ARG(int,ch), Q_ARG(int,s)); }
void ModbusBackend::writeCalOutput(int ch, int k, int b){ INVOKE("doWriteCalOutput", Q_ARG(int,ch), Q_ARG(int,k), Q_ARG(int,b)); }
void ModbusBackend::writeCalMeasV(int ch, int k, int b) { INVOKE("doWriteCalMeasV", Q_ARG(int,ch), Q_ARG(int,k), Q_ARG(int,b)); }
void ModbusBackend::writeCalMeasI(int ch, int k, int b) { INVOKE("doWriteCalMeasI", Q_ARG(int,ch), Q_ARG(int,k), Q_ARG(int,b)); }
void ModbusBackend::exitCalibrationMode() { INVOKE("doExitCalibrationMode"); }
void ModbusBackend::saveChannel(int ch)      { INVOKE("doSendParamAction", Q_ARG(int,ch), Q_ARG(int,1)); }
void ModbusBackend::loadChannel(int ch)      { INVOKE("doSendParamAction", Q_ARG(int,ch), Q_ARG(int,2)); }
void ModbusBackend::factoryResetChannel(int ch) { INVOKE("doSendParamAction", Q_ARG(int,ch), Q_ARG(int,3)); }

void ModbusBackend::rawReadFc04(int addr, int count)   { INVOKE("doRawReadFc04", Q_ARG(int,addr), Q_ARG(int,count)); }
void ModbusBackend::rawReadFc03(int addr, int count)   { INVOKE("doRawReadFc03", Q_ARG(int,addr), Q_ARG(int,count)); }
void ModbusBackend::rawWriteFc06(int addr, int v)      { INVOKE("doRawWriteFc06", Q_ARG(int,addr), Q_ARG(int,v)); }

#undef INVOKE

// ---------------------------------------------------------------------------
//  Worker callbacks (on main thread)
// ---------------------------------------------------------------------------

void ModbusBackend::onConnected(bool ok, const QString& error)
{
    m_connected = ok;
    if (ok) {
        // Read sysInfo first — channelCount() is derived from it.
        // Channel reads fire in onSysInfoReady once the count is known.
        QMetaObject::invokeMethod(m_worker, "doRefreshSystemInfo", Qt::QueuedConnection);
        QMetaObject::invokeMethod(m_worker, "doReadSystemConfig", Qt::QueuedConnection);
        setStatus("Connected — discovering channels...");
        m_pollTimer.start();
    } else {
        setStatus("Error: " + error);
    }
    emit connectedChanged();
}

void ModbusBackend::onDisconnected()
{
    m_connected = false;
    setStatus("Disconnected");
    emit connectedChanged();
    emit channelDataChanged();
}

void ModbusBackend::onSysInfoReady(const QVariantMap& info)
{
    m_sysInfo = info;
    emit sysInfoChanged();

    // Now that supportedChannels is known, kick off per-channel reads.
    int n = channelCount();
    for (int ch = 0; ch < n; ++ch) {
        QMetaObject::invokeMethod(m_worker, "doRefreshChannelInfo", Qt::QueuedConnection, Q_ARG(int, ch));
        QMetaObject::invokeMethod(m_worker, "doReadChannelConfig",  Qt::QueuedConnection, Q_ARG(int, ch));
    }
    if (n > 0)
        setStatus(QString("Connected — %1 channel%2").arg(n).arg(n > 1 ? "s" : ""));
    emit channelDataChanged();
}

void ModbusBackend::onChInfoReady(int ch, const QVariantMap& info)
{
    if (ch < 0 || ch >= MAX_CHANNELS) return;
    m_chInfo[ch] = info;
    emit channelDataChanged();
}

void ModbusBackend::onSysConfigReady(const QVariantMap& cfg)
{
    m_sysConfig = cfg;
    emit sysConfigChanged();
}

void ModbusBackend::onChConfigReady(int ch, const QVariantMap& cfg)
{
    if (ch < 0 || ch >= MAX_CHANNELS) return;
    m_chConfig[ch] = cfg;
    emit channelDataChanged();
    emit channelConfigUpdated(ch);
}

void ModbusBackend::onOperationComplete(bool ok, const QString& msg)
{
    if (ok) {
        setStatus(QString("✓ %1").arg(msg));
        m_statusClearTimer.start(4000);
    } else {
        m_statusClearTimer.stop();
        setStatus(QString("✗ %1").arg(msg));
    }
}

void ModbusBackend::onPortsScanned(const QStringList& ports)
{
    m_ports = ports;
    if (!ports.isEmpty() && m_selectedPort.isEmpty())
        m_selectedPort = ports.first();
    emit portsChanged();
    if (!ports.isEmpty() && m_selectedPort.isEmpty())
        emit selectedPortChanged();
}

void ModbusBackend::onRawFrameLog(const QString& msg)
{
    m_rawLogLines.append(msg);
    if (m_rawLogLines.size() > 500) m_rawLogLines.removeFirst();
    emit rawLogChanged();
}

void ModbusBackend::onRawHexResult(const QString& hex)
{
    emit rawHexReady(hex);
}

void ModbusBackend::setStatus(const QString& msg)
{
    m_statusMessage = msg;
    emit statusMessageChanged();
}
