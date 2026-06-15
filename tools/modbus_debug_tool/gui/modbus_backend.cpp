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

// ---------------------------------------------------------------------------
//  Connection
// ---------------------------------------------------------------------------

void ModbusBackend::connectToDevice()
{
    setStatus("Connecting...");
    QMetaObject::invokeMethod(m_worker, "doConnect", Qt::QueuedConnection,
        Q_ARG(QString, m_selectedPort), Q_ARG(int, m_baud),
        Q_ARG(int, m_slaveId), Q_ARG(int, 500));
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

void ModbusBackend::refreshAll()
{
    if (!m_connected) return;
    QMetaObject::invokeMethod(m_worker, "doRefreshSystemInfo", Qt::QueuedConnection);
    QMetaObject::invokeMethod(m_worker, "doRefreshChannelInfo", Qt::QueuedConnection, Q_ARG(int, 0));
    QMetaObject::invokeMethod(m_worker, "doRefreshChannelInfo", Qt::QueuedConnection, Q_ARG(int, 1));
}

void ModbusBackend::pollTick()
{
    refreshAll();
}

// ---------------------------------------------------------------------------
//  System writes
// ---------------------------------------------------------------------------

#define INVOKE(method, ...) \
    QMetaObject::invokeMethod(m_worker, method, Qt::QueuedConnection, ##__VA_ARGS__)

void ModbusBackend::writeOperatingMode(int mode)       { INVOKE("doWriteOperatingMode", Q_ARG(int, mode)); }
void ModbusBackend::writeSlaveAddress(int addr)         { INVOKE("doWriteSlaveAddress", Q_ARG(int, addr)); }
void ModbusBackend::writeBaudRate(int code)             { INVOKE("doWriteBaudRate", Q_ARG(int, code)); }
void ModbusBackend::writeRecoveryPolicy(int p, int d, int m, int w)
    { INVOKE("doWriteRecoveryPolicy", Q_ARG(int,p), Q_ARG(int,d), Q_ARG(int,m), Q_ARG(int,w)); }
void ModbusBackend::writeSafeBands(int v, int i)        { INVOKE("doWriteSafeBands", Q_ARG(int,v), Q_ARG(int,i)); }
void ModbusBackend::saveSystem()          { INVOKE("doSendParamAction", Q_ARG(int,-1), Q_ARG(int,1)); }
void ModbusBackend::loadSystem()          { INVOKE("doSendParamAction", Q_ARG(int,-1), Q_ARG(int,2)); }
void ModbusBackend::factoryResetSystem()  { INVOKE("doSendParamAction", Q_ARG(int,-1), Q_ARG(int,3)); }
void ModbusBackend::softwareReset()       { INVOKE("doSendParamAction", Q_ARG(int,-1), Q_ARG(int,255)); }

void ModbusBackend::sendOutputAction(int ch, int a)     { INVOKE("doSendOutputAction", Q_ARG(int,ch), Q_ARG(int,a)); }
void ModbusBackend::sendFaultCmd(int ch, int c)         { INVOKE("doSendFaultCmd", Q_ARG(int,ch), Q_ARG(int,c)); }
void ModbusBackend::writeTargetVoltage(int ch, int raw){ INVOKE("doWriteTargetVoltage", Q_ARG(int,ch), Q_ARG(int,raw)); }
void ModbusBackend::writeRampUp(int ch, int s, int i)  { INVOKE("doWriteRampUp", Q_ARG(int,ch), Q_ARG(int,s), Q_ARG(int,i)); }
void ModbusBackend::writeRampDown(int ch, int s, int i){ INVOKE("doWriteRampDown", Q_ARG(int,ch), Q_ARG(int,s), Q_ARG(int,i)); }
void ModbusBackend::writeVoltageProtection(int ch, int m, int a, int t)
    { INVOKE("doWriteVoltageProtection", Q_ARG(int,ch), Q_ARG(int,m), Q_ARG(int,a), Q_ARG(int,t)); }
void ModbusBackend::writeCurrentProtection(int ch, int m, int a, int t)
    { INVOKE("doWriteCurrentProtection", Q_ARG(int,ch), Q_ARG(int,m), Q_ARG(int,a), Q_ARG(int,t)); }
void ModbusBackend::writeDerateStep(int ch, int s)   { INVOKE("doWriteDerateStep", Q_ARG(int,ch), Q_ARG(int,s)); }
void ModbusBackend::writeCalOutput(int ch, int k, int b){ INVOKE("doWriteCalOutput", Q_ARG(int,ch), Q_ARG(int,k), Q_ARG(int,b)); }
void ModbusBackend::writeCalMeasV(int ch, int k, int b) { INVOKE("doWriteCalMeasV", Q_ARG(int,ch), Q_ARG(int,k), Q_ARG(int,b)); }
void ModbusBackend::writeCalMeasI(int ch, int k, int b) { INVOKE("doWriteCalMeasI", Q_ARG(int,ch), Q_ARG(int,k), Q_ARG(int,b)); }
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
        setStatus("Connected");
        m_pollTimer.start();
        refreshAll();
        QMetaObject::invokeMethod(m_worker, "doReadSystemConfig", Qt::QueuedConnection);
        QMetaObject::invokeMethod(m_worker, "doReadChannelConfig", Qt::QueuedConnection, Q_ARG(int, 0));
        QMetaObject::invokeMethod(m_worker, "doReadChannelConfig", Qt::QueuedConnection, Q_ARG(int, 1));
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
}

void ModbusBackend::onSysInfoReady(const QVariantMap& info)
{
    m_sysInfo = info;
    emit sysInfoChanged();
}

void ModbusBackend::onChInfoReady(int ch, const QVariantMap& info)
{
    if (ch == 0) { m_chInfo0 = info; emit ch0InfoChanged(); }
    else         { m_chInfo1 = info; emit ch1InfoChanged(); }
}

void ModbusBackend::onSysConfigReady(const QVariantMap& cfg)
{
    m_sysConfig = cfg;
    emit sysConfigChanged();
}

void ModbusBackend::onChConfigReady(int ch, const QVariantMap& cfg)
{
    if (ch == 0) { m_chConfig0 = cfg; emit ch0ConfigChanged(); }
    else         { m_chConfig1 = cfg; emit ch1ConfigChanged(); }
}

void ModbusBackend::onOperationComplete(bool ok, const QString& msg)
{
    setStatus(msg);
    if (!ok && msg.startsWith("Error")) return;
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
