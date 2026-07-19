#include "CalibrationBackend.h"
#include "SweepData.h"
#include "register_map.h"
#include "board_catalog.h"

#include "dt-bindings/voltage_control/capabilities.h"
#include "reg_store/reg_map.h"

#include <QDateTime>
#include <QDir>
#include <QMutexLocker>
#include <QTimer>
#include <QtMath>

namespace psb::factory {

namespace vscale = psb::reg::scale;

// ---------------------------------------------------------------------------
// MetaType registration — required for cross-thread signal/slot with custom types
// ---------------------------------------------------------------------------
static void registerMetaTypes() {
    qRegisterMetaType<psb::factory::ChannelCalData>("psb::factory::ChannelCalData");
    qRegisterMetaType<psb::factory::FuncTestResult>("psb::factory::FuncTestResult");
    qRegisterMetaType<psb::factory::StressTestResult>("psb::factory::StressTestResult");
    qRegisterMetaType<psb::SystemInfo>("psb::SystemInfo");
    qRegisterMetaType<QList<uint16_t>>("QList<uint16_t>");
    qRegisterMetaType<psb::ChannelCalConfig>("psb::ChannelCalConfig");
    qRegisterMetaType<QList<psb::ChannelCalConfig>>("QList<psb::ChannelCalConfig>");
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

CalibrationBackend::CalibrationBackend(QObject* parent)
    : QObject(parent)
    , m_settings("PsbFactory", "psb_factory_tool")
{
    registerMetaTypes();

    // Sweep worker — lives on its own thread
    m_sweepWorker = new CalibrationWorker();
    m_sweepWorker->setClient(&m_client, &m_clientMutex);
    m_sweepWorker->moveToThread(&m_sweepThread);

    connect(&m_sweepThread, &QThread::finished, m_sweepWorker, &QObject::deleteLater);
    connect(m_sweepWorker, &CalibrationWorker::stepComplete,
            this, &CalibrationBackend::onSweepStep);
    connect(m_sweepWorker, &CalibrationWorker::sweepComplete,
            this, &CalibrationBackend::onSweepComplete);
    connect(m_sweepWorker, &CalibrationWorker::sweepFailed,
            this, &CalibrationBackend::onSweepFailed);
    connect(m_sweepWorker, &CalibrationWorker::sweepAborted, this, [this] {
        m_sweepRunning = false;
        emit sweepRunningChanged();
        setStatus("Sweep aborted");
    });
    connect(m_sweepWorker, &CalibrationWorker::portsScanned,
            this, &CalibrationBackend::onPortsScanned);
    connect(m_sweepWorker, &CalibrationWorker::connectResult,
            this, &CalibrationBackend::onConnectResult);
    connect(m_sweepWorker, &CalibrationWorker::disconnectDone,
            this, &CalibrationBackend::onDisconnectDone);
    connect(m_sweepWorker, &CalibrationWorker::unlockResult,
            this, &CalibrationBackend::onUnlockResult);
    connect(m_sweepWorker, &CalibrationWorker::exitCalDone,
            this, &CalibrationBackend::onExitCalDone);
    connect(m_sweepWorker, &CalibrationWorker::writeCoeffsDone,
            this, &CalibrationBackend::onWriteCoeffsDone);
    connect(m_sweepWorker, &CalibrationWorker::commitDone,
            this, &CalibrationBackend::onCommitDone);
    connect(m_sweepWorker, &CalibrationWorker::safeAllDone,
            this, &CalibrationBackend::onSafeAllDone);
    m_sweepThread.start();

    // Test engine — lives on its own thread
    m_testWorker = new TestEngine();
    m_testWorker->setClient(&m_client, &m_clientMutex);
    m_testWorker->moveToThread(&m_testThread);

    connect(&m_testThread, &QThread::finished, m_testWorker, &QObject::deleteLater);
    connect(m_testWorker, &TestEngine::funcTestProgress,
            this, &CalibrationBackend::onFuncTestProgress);
    connect(m_testWorker, &TestEngine::funcTestComplete,
            this, &CalibrationBackend::onFuncTestComplete);
    connect(m_testWorker, &TestEngine::stressProgress,
            this, &CalibrationBackend::onStressProgress);
    connect(m_testWorker, &TestEngine::stressComplete,
            this, &CalibrationBackend::onStressComplete);
    connect(m_testWorker, &TestEngine::testError, this, [this](const QString& msg) {
        m_testRunning = false;
        emit testRunningChanged();
        setStatus("Test error: " + msg);
    });
    connect(m_testWorker, &TestEngine::testAborted, this, [this] {
        m_testRunning = false;
        emit testRunningChanged();
        setStatus("Test aborted");
    });
    m_testThread.start();

    m_reportData.timestamp = QDateTime::currentDateTime().toString(Qt::ISODate);
}

CalibrationBackend::~CalibrationBackend() {
    m_sweepThread.quit();
    m_sweepThread.wait();
    m_testThread.quit();
    m_testThread.wait();
    QMutexLocker lk(&m_clientMutex);
    m_client.disconnect();
}

// ---------------------------------------------------------------------------
// Property readers
// ---------------------------------------------------------------------------

QStringList CalibrationBackend::ports()        const { return m_ports; }
bool        CalibrationBackend::connected()     const { return m_client.isConnected(); }
QString     CalibrationBackend::statusMessage() const { return m_statusMessage; }
bool        CalibrationBackend::calUnlocked()   const { return m_calUnlocked; }
bool        CalibrationBackend::calActive()     const { return m_calActive; }
int         CalibrationBackend::numChannels()   const { return m_sysInfo.supportedChannels; }
bool        CalibrationBackend::sweepRunning()  const { return m_sweepRunning; }
bool        CalibrationBackend::testRunning()   const { return m_testRunning; }

QString CalibrationBackend::reportBoardSerial() const { return m_reportData.boardSerial; }
QString CalibrationBackend::reportOperatorId()  const { return m_reportData.operatorId; }
QString CalibrationBackend::reportNotes()       const { return m_reportData.notes; }

bool CalibrationBackend::calDataAvailable()    const { return m_reportData.calRun; }
bool CalibrationBackend::funcTestAvailable()   const { return m_reportData.funcTestRun; }
bool CalibrationBackend::stressTestAvailable() const { return m_reportData.stressTestRun; }

void CalibrationBackend::setReportBoardSerial(const QString& v) {
    m_reportData.boardSerial = v; emit reportMetaChanged();
}
void CalibrationBackend::setReportOperatorId(const QString& v) {
    m_reportData.operatorId = v; emit reportMetaChanged();
}
void CalibrationBackend::setReportNotes(const QString& v) {
    m_reportData.notes = v; emit reportMetaChanged();
}

QVariantMap CalibrationBackend::deviceInfo() const {
    QVariantMap m;
    m["protoMajor"]        = m_sysInfo.protoMajor;
    m["protoMinor"]        = m_sysInfo.protoMinor;
    m["variantId"]         = m_sysInfo.variantId;
    m["variantName"]       = QString::fromStdString(psb::catalog::variantName(m_sysInfo.variantId));
    m["variantFamily"]     = QString::fromStdString(psb::catalog::variantFamily(m_sysInfo.variantId));
    m["boardHwRevision"]   = m_sysInfo.boardHwRevision;
    m["boardHwRevisionLabel"] = QString::fromStdString(psb::catalog::hwRevisionLabel(m_sysInfo.boardHwRevision));
    m["supportedChannels"] = m_sysInfo.supportedChannels;
    m["activeChMask"]      = (int)m_sysInfo.activeChMask;
    m["fwVersion"]         = QString::fromStdString(psb::reg::formatFwVersion(m_sysInfo.fwVersion));
    m["boardTemp"]         = m_sysInfo.boardTempRaw / 10.0;
    m["uptimeSec"]         = (int)m_sysInfo.uptimeSec;
    return m;
}

// ---------------------------------------------------------------------------
// Channel capability and NVS accessor
// ---------------------------------------------------------------------------

int CalibrationBackend::channelCaps(int ch) const {
    if (ch < 0 || ch >= m_chCaps.size()) return 0;
    return m_chCaps[ch];
}

QVariantMap CalibrationBackend::channelNvsCoeffs(int ch) const {
    QVariantMap m;
    if (ch < 0 || ch >= m_nvsCoeffs.size()) return m;
    const auto& c = m_nvsCoeffs[ch];
    m["outCalK"]   = c.outCalK;
    m["outCalB"]   = c.outCalB;
    m["measVCalK"] = c.measVCalK;
    m["measVCalB"] = c.measVCalB;
    m["measICalK"] = c.measICalK;
    m["measICalB"] = c.measICalB;
    return m;
}

QVariantMap CalibrationBackend::channelSweepFit(int ch) const {
    QVariantMap m;
    if (ch < 0 || ch >= m_calData.size()) return m;
    const auto& d = m_calData[ch];

    auto fitMap = [](const FitResult& f, bool active, double divisor) {
        QVariantMap fm;
        fm["active"]  = active;
        fm["valid"]   = f.valid;
        fm["k"]       = f.k;
        fm["b"]       = f.b;
        fm["r2"]      = f.r2;
        fm["kDevice"] = qRound(f.k * divisor);
        fm["bDevice"] = qRound(f.b);
        return fm;
    };
    m["hasOut"]        = d.hasOut;
    m["hasMeasV"]      = d.hasMeasV;
    m["hasMeasI"]      = d.hasMeasI;
    m["needsCal"]      = d.needsCal;
    m["outFit"]        = fitMap(d.outFit,   d.hasOut,   vscale::OUTPUT_CAL_DIVISOR);
    m["measVFit"]      = fitMap(d.measVFit, d.hasMeasV, vscale::MEAS_CAL_DIVISOR);
    m["measIFit"]      = fitMap(d.measIFit, d.hasMeasI, vscale::MEAS_CAL_DIVISOR);
    m["coeffsWritten"] = d.coeffsWritten;
    m["committed"]     = d.committed;
    return m;
}

// ---------------------------------------------------------------------------
// QSettings — sweep config
// ---------------------------------------------------------------------------

QVariantMap CalibrationBackend::sweepConfig() const {
    QVariantMap m;
    m["dacMin"]       = m_settings.value("sweep/dacMin",        0).toInt();
    m["dacMax"]       = m_settings.value("sweep/dacMax",     4095).toInt();
    m["stepSize"]     = m_settings.value("sweep/stepSize",    256).toInt();
    m["settlementMs"] = m_settings.value("sweep/settlementMs", 300).toInt();
    m["cooldownMs"]   = m_settings.value("sweep/cooldownMs",   100).toInt();
    return m;
}

void CalibrationBackend::setSweepConfig(const QVariantMap& cfg) {
    m_settings.setValue("sweep/dacMin",       cfg.value("dacMin",       0));
    m_settings.setValue("sweep/dacMax",       cfg.value("dacMax",    4095));
    m_settings.setValue("sweep/stepSize",     cfg.value("stepSize",   256));
    m_settings.setValue("sweep/settlementMs", cfg.value("settlementMs", 300));
    m_settings.setValue("sweep/cooldownMs",   cfg.value("cooldownMs",  100));
}

// ---------------------------------------------------------------------------
// QSettings — functional test config
// ---------------------------------------------------------------------------

QVariantMap CalibrationBackend::funcTestConfig() const {
    QVariantMap m;
    m["tolerancePct"]  = m_settings.value("funcTest/tolerancePct",  2.0).toDouble();
    m["settleMs"]      = m_settings.value("funcTest/settleMs",      500).toInt();
    m["retriesOnFault"]= m_settings.value("funcTest/retries",         2).toInt();
    m["targetVolts"]   = m_settings.value("funcTest/targetVolts",
                                          "10.0,20.0,30.0").toString();
    return m;
}

void CalibrationBackend::setFuncTestConfig(const QVariantMap& cfg) {
    m_settings.setValue("funcTest/tolerancePct", cfg.value("tolerancePct",    2.0));
    m_settings.setValue("funcTest/settleMs",     cfg.value("settleMs",        500));
    m_settings.setValue("funcTest/retries",      cfg.value("retriesOnFault",    2));
    m_settings.setValue("funcTest/targetVolts",  cfg.value("targetVolts", "10.0,20.0,30.0"));
}

// ---------------------------------------------------------------------------
// QSettings — stress test config
// ---------------------------------------------------------------------------

QVariantMap CalibrationBackend::stressConfig() const {
    QVariantMap m;
    m["durationSec"]    = m_settings.value("stress/durationSec",   120).toInt();
    m["pollMs"]         = m_settings.value("stress/pollMs",        1000).toInt();
    m["faultTolerance"] = m_settings.value("stress/faultTol",         0).toInt();
    m["tolerancePct"]   = m_settings.value("stress/tolerancePct",   5.0).toDouble();
    m["targetVolts"]    = m_settings.value("stress/targetVolts", "20.0").toString();
    return m;
}

void CalibrationBackend::setStressConfig(const QVariantMap& cfg) {
    m_settings.setValue("stress/durationSec",  cfg.value("durationSec",  120));
    m_settings.setValue("stress/pollMs",       cfg.value("pollMs",      1000));
    m_settings.setValue("stress/faultTol",     cfg.value("faultTolerance",  0));
    m_settings.setValue("stress/tolerancePct", cfg.value("tolerancePct", 5.0));
    m_settings.setValue("stress/targetVolts",  cfg.value("targetVolts","20.0"));
}

// ---------------------------------------------------------------------------
// QSettings — report config
// ---------------------------------------------------------------------------

QVariantMap CalibrationBackend::reportConfig() const {
    QVariantMap m;
    m["outputDir"] = m_settings.value("report/outputDir",
                                      QDir::homePath()).toString();
    m["format"]    = m_settings.value("report/format", "pdf").toString();
    return m;
}

void CalibrationBackend::setReportConfig(const QVariantMap& cfg) {
    m_settings.setValue("report/outputDir", cfg.value("outputDir", QDir::homePath()));
    m_settings.setValue("report/format",    cfg.value("format", "pdf"));
}

// ---------------------------------------------------------------------------
// Report summary accessors for ReportPage QML
// ---------------------------------------------------------------------------

QVariantList CalibrationBackend::calSummary() const {
    QVariantList list;
    for (const auto& d : m_reportData.calResults) {
        QVariantMap m;
        m["ch"]           = d.ch;
        m["needsCal"]     = d.needsCal;
        m["hasOut"]       = d.hasOut;
        m["hasMeasV"]     = d.hasMeasV;
        m["hasMeasI"]     = d.hasMeasI;
        m["committed"]    = d.committed;
        m["outR2"]        = d.outFit.r2;
        m["measVR2"]      = d.measVFit.r2;
        m["measIR2"]      = d.measIFit.r2;
        m["outKDevice"]   = qRound(d.outFit.k  * vscale::OUTPUT_CAL_DIVISOR);
        m["outBDevice"]   = qRound(d.outFit.b);
        m["measVKDevice"] = qRound(d.measVFit.k * vscale::MEAS_CAL_DIVISOR);
        m["measVBDevice"] = qRound(d.measVFit.b);
        m["measIKDevice"] = qRound(d.measIFit.k * vscale::MEAS_CAL_DIVISOR);
        m["measIBDevice"] = qRound(d.measIFit.b);
        list.append(m);
    }
    return list;
}

QVariantMap CalibrationBackend::funcTestSummary() const {
    if (!m_reportData.funcTestRun) return {};
    QVariantMap m;
    m["pass"]      = m_reportData.funcTest.pass;
    m["timestamp"] = m_reportData.funcTest.timestamp;
    QVariantList pts;
    for (const auto& pt : m_reportData.funcTest.points) {
        QVariantMap pm;
        pm["ch"]           = pt.ch;
        pm["targetV"]      = pt.targetV;
        pm["measuredV"]    = pt.measuredV;
        pm["errorPct"]     = pt.errorPct;
        pm["tolerancePct"] = pt.tolerancePct;
        pm["pass"]         = pt.pass;
        pm["detail"]       = pt.detail;
        pts.append(pm);
    }
    m["points"] = pts;
    return m;
}

QVariantMap CalibrationBackend::stressSummary() const {
    if (!m_reportData.stressTestRun) return {};
    QVariantMap m;
    m["pass"]      = m_reportData.stressTest.pass;
    m["timestamp"] = m_reportData.stressTest.timestamp;
    QVariantList chs;
    for (const auto& ch : m_reportData.stressTest.channels) {
        QVariantMap cm;
        cm["ch"]          = ch.ch;
        cm["targetV"]     = ch.targetV;
        cm["avgV"]        = ch.avgV;
        cm["minV"]        = ch.minV;
        cm["maxV"]        = ch.maxV;
        cm["stddevV"]     = ch.stddevV;
        cm["durationSec"] = ch.durationSec;
        cm["faultCount"]  = ch.faultCount;
        cm["pass"]        = ch.pass;
        chs.append(cm);
    }
    m["channels"] = chs;
    return m;
}

// ---------------------------------------------------------------------------
// Connection
// ---------------------------------------------------------------------------

void CalibrationBackend::scanPorts() {
    QTimer::singleShot(0, m_sweepWorker, [this] { m_sweepWorker->doScanPorts(); });
}

void CalibrationBackend::connectToDevice(const QString& port, int baud, int slaveId) {
    setStatus("Connecting...");
    QTimer::singleShot(0, m_sweepWorker, [this, port, baud, slaveId] {
        m_sweepWorker->doConnect(port, baud, slaveId);
    });
}

void CalibrationBackend::disconnectFromDevice() {
    if (m_calActive) exitCalMode();
    QTimer::singleShot(0, m_sweepWorker, [this] { m_sweepWorker->doDisconnect(); });
}

// ---------------------------------------------------------------------------
// Cal session
// ---------------------------------------------------------------------------

void CalibrationBackend::unlockAndEnter() {
    int nch = m_sysInfo.supportedChannels;
    QTimer::singleShot(0, m_sweepWorker, [this, nch] { m_sweepWorker->doUnlockAndEnter(nch); });
}

void CalibrationBackend::exitCalMode() {
    QTimer::singleShot(0, m_sweepWorker, [this] { m_sweepWorker->doExitCalMode(); });
}

// ---------------------------------------------------------------------------
// Roll-back
// ---------------------------------------------------------------------------

void CalibrationBackend::rollbackToConnect() {
    if (m_calActive) exitCalMode();
    m_calUnlocked = false;
    m_calData.clear();
    m_nvsCoeffs.clear();
    emit calStateChanged();
    setStatus("Rolled back to connected state");
}

void CalibrationBackend::rollbackToUnlock() {
    if (m_calActive) exitCalMode();
    m_calData.clear();
    m_nvsCoeffs.clear();
    emit calStateChanged();
    setStatus("Rolled back — ready to re-enter cal mode");
}

void CalibrationBackend::rollbackToCal() {
    setStatus("Back to calibration — select a channel");
}

// ---------------------------------------------------------------------------
// Sweep
// ---------------------------------------------------------------------------

void CalibrationBackend::startSweep(int ch) {
    if (m_sweepRunning) return;
    if (ch < 0 || ch >= m_calData.size()) {
        setStatus("Invalid channel for sweep");
        return;
    }
    const auto& d = m_calData[ch];
    if (!d.needsCal) {
        setStatus(QString("CH%1 has no calibration axes").arg(ch));
        return;
    }

    SweepConfig cfg;
    const QVariantMap sc = sweepConfig();
    cfg.dacMin       = sc.value("dacMin",       0).toInt();
    cfg.dacMax       = sc.value("dacMax",    4095).toInt();
    cfg.stepSize     = sc.value("stepSize",   256).toInt();
    cfg.settlementMs = sc.value("settlementMs", 300).toInt();
    cfg.cooldownMs   = sc.value("cooldownMs",  100).toInt();

    bool hasOut   = d.hasOut;
    bool hasMeasV = d.hasMeasV;
    bool hasMeasI = d.hasMeasI;

    m_sweepRunning = true;
    emit sweepRunningChanged();
    setStatus(QString("Sweeping CH%1…").arg(ch));

    QTimer::singleShot(0, m_sweepWorker, [=] {
        m_sweepWorker->runSweep(ch, cfg, hasOut, hasMeasV, hasMeasI);
    });
}

void CalibrationBackend::abortSweep() {
    m_sweepWorker->abort();
}

// ---------------------------------------------------------------------------
// Fit computation — called from QML after operator enters DMM readings
// ---------------------------------------------------------------------------

void CalibrationBackend::computeFit(int ch, const QVariantList& dmmPoints) {
    if (ch < 0 || ch >= m_calData.size()) return;
    auto& d = m_calData[ch];

    if (d.points.isEmpty() || dmmPoints.size() != d.points.size()) {
        setStatus("DMM point count mismatch — re-run sweep");
        return;
    }

    for (int i = 0; i < d.points.size(); ++i) {
        const QVariantMap dm = dmmPoints[i].toMap();
        d.points[i].dmmV    = dm.value("dmmV", 0.0).toDouble();
        d.points[i].dmmI    = dm.value("dmmI", 0.0).toDouble();
        d.points[i].dmmVSet = dm.contains("dmmV");
        d.points[i].dmmISet = dm.contains("dmmI");
    }

    // out (V→DAC): x = DMM voltage in mV, y = dacCode
    if (d.hasOut) {
        QList<double> x, y;
        for (const auto& pt : d.points) {
            if (!pt.dmmVSet) continue;
            x.append(pt.dmmV * 1000.0);
            y.append(pt.dacCode);
        }
        d.outFit = (x.size() >= 2) ? linearRegression(x, y) : FitResult{};
    }

    // measV (ADC→V): x = rawAdcVoltage, y = DMM voltage in mV
    if (d.hasMeasV) {
        QList<double> x, y;
        for (const auto& pt : d.points) {
            if (!pt.dmmVSet) continue;
            x.append(pt.adcV);
            y.append(pt.dmmV * 1000.0);
        }
        d.measVFit = (x.size() >= 2) ? linearRegression(x, y) : FitResult{};
    }

    // measI (ADC→I): x = rawAdcCurrent, y = DMM current in mA
    if (d.hasMeasI) {
        QList<double> x, y;
        for (const auto& pt : d.points) {
            if (!pt.dmmISet) continue;
            x.append(pt.adcI);
            y.append(pt.dmmI);
        }
        d.measIFit = (x.size() >= 2) ? linearRegression(x, y) : FitResult{};
    }

    auto fitMap = [](const FitResult& f, double divisor) {
        QVariantMap m;
        m["valid"]   = f.valid;
        m["k"]       = f.k;
        m["b"]       = f.b;
        m["r2"]      = f.r2;
        m["kDevice"] = qRound(f.k * divisor);
        m["bDevice"] = qRound(f.b);
        return m;
    };
    emit fitReady(ch, fitMap(d.outFit, vscale::OUTPUT_CAL_DIVISOR), fitMap(d.measVFit, vscale::MEAS_CAL_DIVISOR), fitMap(d.measIFit, vscale::MEAS_CAL_DIVISOR));
    setStatus(QString("CH%1 fit computed").arg(ch));
}

// ---------------------------------------------------------------------------
// Write computed coefficients to device
// ---------------------------------------------------------------------------

void CalibrationBackend::writeCoefficients(int ch,
                                           double outK,  double outB,
                                           double measVK, double measVB,
                                           double measIK, double measIB) {
    if (ch < 0 || ch >= m_calData.size()) return;
    const auto& d = m_calData[ch];

    // Persist the (possibly user-overridden) fit values immediately so the
    // report is accurate even before the write completes — matches the
    // previous synchronous behavior's ordering.
    m_calData[ch].outFit.k   = outK;   m_calData[ch].outFit.b   = outB;
    m_calData[ch].measVFit.k = measVK; m_calData[ch].measVFit.b = measVB;
    m_calData[ch].measIFit.k = measIK; m_calData[ch].measIFit.b = measIB;

    auto outK16  = static_cast<quint16>(qRound(outK  * vscale::OUTPUT_CAL_DIVISOR));
    auto outB16  = static_cast<qint16> (qRound(outB));
    auto vK16    = static_cast<quint16>(qRound(measVK * vscale::MEAS_CAL_DIVISOR));
    auto vB16    = static_cast<qint16> (qRound(measVB));
    auto iK16    = static_cast<quint16>(qRound(measIK * vscale::MEAS_CAL_DIVISOR));
    auto iB16    = static_cast<qint16> (qRound(measIB));
    bool hasOut = d.hasOut, hasMeasV = d.hasMeasV, hasMeasI = d.hasMeasI;

    QTimer::singleShot(0, m_sweepWorker, [this, ch, hasOut, hasMeasV, hasMeasI,
                                           outK16, outB16, vK16, vB16, iK16, iB16] {
        m_sweepWorker->doWriteCoefficients(ch, hasOut, hasMeasV, hasMeasI,
                                            outK16, outB16, vK16, vB16, iK16, iB16);
    });
}

// ---------------------------------------------------------------------------
// Commit
// ---------------------------------------------------------------------------

void CalibrationBackend::commitChannel(int ch) {
    if (ch < 0 || ch >= m_calData.size()) return;
    QTimer::singleShot(0, m_sweepWorker, [this, ch] { m_sweepWorker->doCommitChannel(ch); });
}

void CalibrationBackend::commitAll() {
    for (int ch = 0; ch < m_sysInfo.supportedChannels; ++ch) {
        if (ch < m_calData.size() && m_calData[ch].needsCal && m_calData[ch].coeffsWritten)
            commitChannel(ch);
    }
}

// ---------------------------------------------------------------------------
// Safe-all — B-3 fix: use supportedChannels, not hardcoded 2
// ---------------------------------------------------------------------------

void CalibrationBackend::safeAll() {
    int nch = m_sysInfo.supportedChannels;
    QTimer::singleShot(0, m_sweepWorker, [this, nch] { m_sweepWorker->doSafeAll(nch); });
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

namespace {
QList<double> parseVolts(const QString& s) {
    QList<double> r;
    for (const auto& tok : s.split(',', Qt::SkipEmptyParts))
        r.append(tok.trimmed().toDouble());
    return r;
}
} // anonymous namespace

void CalibrationBackend::startFunctionalTest(const QVariantMap& cfg) {
    if (m_testRunning) return;
    m_testRunning = true;
    emit testRunningChanged();

    FuncTestConfig tc;
    tc.ch             = cfg.value("ch", -1).toInt();
    tc.numChannels    = m_sysInfo.supportedChannels;
    tc.tolerancePct   = cfg.value("tolerancePct",
                            m_settings.value("funcTest/tolerancePct", 2.0)).toDouble();
    tc.settleMs       = cfg.value("settleMs",
                            m_settings.value("funcTest/settleMs", 500)).toInt();
    tc.retriesOnFault = cfg.value("retriesOnFault",
                            m_settings.value("funcTest/retries", 2)).toInt();
    tc.targetVolts    = parseVolts(cfg.value("targetVolts",
                            m_settings.value("funcTest/targetVolts",
                                             "10.0,20.0,30.0")).toString());

    setStatus("Functional test running…");
    QTimer::singleShot(0, m_testWorker, [=] {
        m_testWorker->runFunctionalTest(tc);
    });
}

void CalibrationBackend::startStressTest(const QVariantMap& cfg) {
    if (m_testRunning) return;
    m_testRunning = true;
    emit testRunningChanged();

    StressConfig sc;
    sc.ch             = cfg.value("ch", -1).toInt();
    sc.numChannels    = m_sysInfo.supportedChannels;
    sc.durationSec    = cfg.value("durationSec",
                            m_settings.value("stress/durationSec", 120)).toInt();
    sc.pollMs         = cfg.value("pollMs",
                            m_settings.value("stress/pollMs", 1000)).toInt();
    sc.faultTolerance = cfg.value("faultTolerance",
                            m_settings.value("stress/faultTol", 0)).toInt();
    sc.tolerancePct   = cfg.value("tolerancePct",
                            m_settings.value("stress/tolerancePct", 5.0)).toDouble();
    sc.targetVolts    = parseVolts(cfg.value("targetVolts",
                            m_settings.value("stress/targetVolts", "20.0")).toString());

    setStatus("Stress test running…");
    QTimer::singleShot(0, m_testWorker, [=] {
        m_testWorker->runStressTest(sc);
    });
}

void CalibrationBackend::abortTest() {
    m_testWorker->abort();
}

// ---------------------------------------------------------------------------
// Report generation
// ---------------------------------------------------------------------------

void CalibrationBackend::generatePdf(const QString& path) {
    m_reportData.timestamp  = QDateTime::currentDateTime().toString(Qt::ISODate);
    m_reportData.deviceInfo = m_sysInfo;
    QString err;
    bool ok = ReportEngine::generatePdf(m_reportData, path, &err);
    emit reportGenerated(ok, ok ? path : err);
}

void CalibrationBackend::generateMarkdown(const QString& path) {
    m_reportData.timestamp  = QDateTime::currentDateTime().toString(Qt::ISODate);
    m_reportData.deviceInfo = m_sysInfo;
    QString err;
    bool ok = ReportEngine::generateMarkdown(m_reportData, path, &err);
    emit reportGenerated(ok, ok ? path : err);
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

void CalibrationBackend::setStatus(const QString& msg) {
    m_statusMessage = msg;
    emit statusMessageChanged();
}

// ---------------------------------------------------------------------------
// Quick-operation result handlers — run on the main thread, so this is the
// only place m_sysInfo/m_chCaps/m_calUnlocked/m_calActive/m_calData are ever
// mutated. See the "Quick operations" comment in CalibrationWorker.h.
// ---------------------------------------------------------------------------

void CalibrationBackend::onPortsScanned(QStringList ports) {
    m_ports = ports;
    emit portsChanged();
}

void CalibrationBackend::onConnectResult(bool ok, QString error,
                                          psb::SystemInfo sysInfo, QList<uint16_t> chCaps) {
    if (ok) {
        m_sysInfo = sysInfo;
        m_chCaps  = chCaps;
        m_reportData.deviceInfo = m_sysInfo;
        emit deviceInfoChanged();
        setStatus("Connected");
    } else {
        setStatus("Connect failed: " + error);
    }
    emit connectedChanged();
}

void CalibrationBackend::onDisconnectDone() {
    m_calUnlocked = false;
    m_calActive   = false;
    m_sysInfo     = {};
    m_chCaps.clear();
    m_nvsCoeffs.clear();
    m_calData.clear();
    emit connectedChanged();
    emit calStateChanged();
    emit deviceInfoChanged();
    setStatus("Disconnected");
}

void CalibrationBackend::onUnlockResult(bool ok, QString error, QList<uint16_t> chCaps,
                                         QList<psb::ChannelCalConfig> nvsCoeffs) {
    if (!ok) {
        setStatus("Unlock failed: " + error);
        return;
    }
    m_calUnlocked = true;
    m_calActive   = true;
    m_chCaps      = chCaps;
    m_nvsCoeffs   = nvsCoeffs;

    int nch = m_sysInfo.supportedChannels;
    m_calData.resize(nch);
    for (int ch = 0; ch < nch; ++ch) {
        auto& d   = m_calData[ch];
        d.ch      = ch;
        uint16_t caps = m_chCaps.value(ch, 0);
        d.hasOut   = (caps & CH_CAP_RAW_OUTPUT_DRIVE)    != 0;
        d.hasMeasV = (caps & CH_CAP_VOLTAGE_MEASUREMENT) != 0;
        d.hasMeasI = (caps & CH_CAP_CURRENT_MEASUREMENT) != 0;
        d.needsCal = d.hasOut || d.hasMeasV || d.hasMeasI;
        d.points.clear();
        d.coeffsWritten = false;
        d.committed     = false;
    }

    emit calStateChanged();
    setStatus("Calibration mode entered — NVS coefficients saved");
}

void CalibrationBackend::onExitCalDone(bool /*ok*/) {
    m_calActive   = false;
    m_calUnlocked = false;
    emit calStateChanged();
    setStatus("Calibration mode exited");
}

void CalibrationBackend::onWriteCoeffsDone(int ch, bool ok, QString error) {
    if (ch < 0 || ch >= m_calData.size()) return;
    m_calData[ch].coeffsWritten = ok;
    emit writeCoeffsResult(ch, ok,
        ok ? QString("CH%1 coefficients written").arg(ch)
           : QString("CH%1 write failed: %2").arg(ch).arg(error));
    setStatus(ok ? QString("CH%1 coefficients written").arg(ch)
                 : QString("CH%1 write failed").arg(ch));
}

void CalibrationBackend::onCommitDone(int ch, bool ok, QString error) {
    if (ch < 0 || ch >= m_calData.size()) return;
    m_calData[ch].committed = ok;
    if (ok) {
        m_reportData.calResults = m_calData;
        m_reportData.calRun     = true;
        emit reportMetaChanged();
    }
    emit channelCommitted(ch, ok,
        ok ? QString("CH%1 committed to NVS").arg(ch)
           : QString("CH%1 commit failed: %2").arg(ch).arg(error));
    setStatus(ok ? QString("CH%1 committed").arg(ch)
                 : QString("CH%1 commit failed").arg(ch));
}

void CalibrationBackend::onSafeAllDone() {
    setStatus("All outputs safe-off");
}

// ---------------------------------------------------------------------------
// Sweep worker slots
// ---------------------------------------------------------------------------

void CalibrationBackend::onSweepStep(int step, int total, int dacCode, qint32 adcV, qint32 adcI) {
    emit sweepStep(step, total, dacCode, adcV, adcI);
}

void CalibrationBackend::onSweepComplete(ChannelCalData data) {
    int ch = data.ch;
    if (ch >= 0 && ch < m_calData.size())
        m_calData[ch] = data;

    m_sweepRunning = false;
    emit sweepRunningChanged();

    QVariantList pts;
    for (const auto& pt : data.points) {
        QVariantMap m;
        m["dacCode"] = pt.dacCode;
        m["adcV"]    = (int)pt.adcV;
        m["adcI"]    = (int)pt.adcI;
        pts.append(m);
    }
    emit sweepFinished(ch, pts);
    setStatus(QString("CH%1 sweep complete — %2 points").arg(ch).arg(data.points.size()));
}

void CalibrationBackend::onSweepFailed(QString msg) {
    m_sweepRunning = false;
    emit sweepRunningChanged();
    setStatus("Sweep failed: " + msg);
    emit sweepFailed(-1, msg);
}

// ---------------------------------------------------------------------------
// Test engine slots
// ---------------------------------------------------------------------------

void CalibrationBackend::onFuncTestProgress(int ch, int idx, int total) {
    emit funcTestProgress(ch, idx, total);
}

void CalibrationBackend::onFuncTestComplete(FuncTestResult result) {
    m_testRunning = false;
    emit testRunningChanged();
    m_reportData.funcTest    = result;
    m_reportData.funcTestRun = true;
    emit reportMetaChanged();

    QVariantList pts;
    for (const auto& pt : result.points) {
        QVariantMap m;
        m["ch"]           = pt.ch;
        m["targetV"]      = pt.targetV;
        m["measuredV"]    = pt.measuredV;
        m["errorPct"]     = pt.errorPct;
        m["tolerancePct"] = pt.tolerancePct;
        m["pass"]         = pt.pass;
        m["detail"]       = pt.detail;
        pts.append(m);
    }
    emit funcTestDone(result.pass, pts);
    setStatus(QString("Functional test %1").arg(result.pass ? "PASS" : "FAIL"));
}

void CalibrationBackend::onStressProgress(int ch, double elapsed, double total, double v) {
    emit stressProgress(ch, elapsed, total, v);
}

void CalibrationBackend::onStressComplete(StressTestResult result) {
    m_testRunning = false;
    emit testRunningChanged();
    m_reportData.stressTest    = result;
    m_reportData.stressTestRun = true;
    emit reportMetaChanged();

    QVariantList chs;
    for (const auto& ch : result.channels) {
        QVariantMap m;
        m["ch"]          = ch.ch;
        m["targetV"]     = ch.targetV;
        m["avgV"]        = ch.avgV;
        m["minV"]        = ch.minV;
        m["maxV"]        = ch.maxV;
        m["stddevV"]     = ch.stddevV;
        m["durationSec"] = ch.durationSec;
        m["faultCount"]  = ch.faultCount;
        m["pass"]        = ch.pass;
        chs.append(m);
    }
    emit stressDone(result.pass, chs);
    setStatus(QString("Stress test %1").arg(result.pass ? "PASS" : "FAIL"));
}

} // namespace psb::factory
