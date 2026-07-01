#pragma once

#include <QMutex>
#include <QObject>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqml.h>

#include "CalibrationWorker.h"
#include "ReportData.h"
#include "ReportEngine.h"
#include "SweepData.h"
#include "TestEngine.h"
#include "hvb_modbus_client.h"

namespace hvb::factory {

class CalibrationBackend : public QObject {
    Q_OBJECT
    QML_ELEMENT

    // ---- Connection --------------------------------------------------------
    Q_PROPERTY(QStringList ports         READ ports          NOTIFY portsChanged)
    Q_PROPERTY(bool        connected     READ connected      NOTIFY connectedChanged)
    Q_PROPERTY(QString     statusMessage READ statusMessage  NOTIFY statusMessageChanged)
    Q_PROPERTY(QVariantMap deviceInfo    READ deviceInfo     NOTIFY deviceInfoChanged)

    // ---- Cal session state -------------------------------------------------
    Q_PROPERTY(bool calUnlocked READ calUnlocked NOTIFY calStateChanged)
    Q_PROPERTY(bool calActive   READ calActive   NOTIFY calStateChanged)
    Q_PROPERTY(int  numChannels READ numChannels NOTIFY deviceInfoChanged)

    // ---- Sweep progress ----------------------------------------------------
    Q_PROPERTY(bool sweepRunning READ sweepRunning NOTIFY sweepRunningChanged)

    // ---- Test progress -----------------------------------------------------
    Q_PROPERTY(bool testRunning READ testRunning NOTIFY testRunningChanged)

    // ---- Report session data -----------------------------------------------
    Q_PROPERTY(QString reportBoardSerial READ reportBoardSerial WRITE setReportBoardSerial NOTIFY reportMetaChanged)
    Q_PROPERTY(QString reportOperatorId  READ reportOperatorId  WRITE setReportOperatorId  NOTIFY reportMetaChanged)
    Q_PROPERTY(QString reportNotes       READ reportNotes       WRITE setReportNotes       NOTIFY reportMetaChanged)
    Q_PROPERTY(bool    calDataAvailable   READ calDataAvailable   NOTIFY reportMetaChanged)
    Q_PROPERTY(bool    funcTestAvailable  READ funcTestAvailable  NOTIFY reportMetaChanged)
    Q_PROPERTY(bool    stressTestAvailable READ stressTestAvailable NOTIFY reportMetaChanged)

public:
    explicit CalibrationBackend(QObject* parent = nullptr);
    ~CalibrationBackend() override;

    // Property readers
    QStringList  ports()         const;
    bool         connected()     const;
    QString      statusMessage() const;
    QVariantMap  deviceInfo()    const;
    bool         calUnlocked()   const;
    bool         calActive()     const;
    int          numChannels()   const;
    bool         sweepRunning()  const;
    bool         testRunning()   const;

    QString reportBoardSerial() const;
    QString reportOperatorId()  const;
    QString reportNotes()       const;
    bool calDataAvailable()    const;
    bool funcTestAvailable()   const;
    bool stressTestAvailable() const;

    void setReportBoardSerial(const QString& v);
    void setReportOperatorId(const QString& v);
    void setReportNotes(const QString& v);

    // QSettings sweep config accessors (for Settings dialog)
    Q_INVOKABLE QVariantMap sweepConfig() const;
    Q_INVOKABLE void        setSweepConfig(const QVariantMap& cfg);

    // QSettings test config accessors
    Q_INVOKABLE QVariantMap funcTestConfig()   const;
    Q_INVOKABLE void        setFuncTestConfig(const QVariantMap& cfg);
    Q_INVOKABLE QVariantMap stressConfig()     const;
    Q_INVOKABLE void        setStressConfig(const QVariantMap& cfg);

    // QSettings report config
    Q_INVOKABLE QVariantMap reportConfig() const;
    Q_INVOKABLE void        setReportConfig(const QVariantMap& cfg);

    // Channel capabilities (bit flags from CH_CAP_*)
    Q_INVOKABLE int channelCaps(int ch) const;

    // Sweep data accessor for CalCommitPage (old NVS K/B read at session start)
    Q_INVOKABLE QVariantMap channelNvsCoeffs(int ch) const;
    Q_INVOKABLE QVariantMap channelSweepFit(int ch)  const;

    // Report data for ReportPage
    Q_INVOKABLE QVariantList calSummary()     const;
    Q_INVOKABLE QVariantMap  funcTestSummary() const;
    Q_INVOKABLE QVariantMap  stressSummary()   const;

public slots:
    // --- Connection ---
    void scanPorts();
    void connectToDevice(const QString& port, int baud, int slaveId);
    void disconnectFromDevice();

    // --- Cal session ---
    void unlockAndEnter();
    void exitCalMode();

    // --- Roll-back ---
    void rollbackToConnect();     // step 3/4 → step 1
    void rollbackToUnlock();      // step 3/4 → step 2
    void rollbackToCal();         // step 4   → step 3

    // --- Sweep ---
    void startSweep(int ch);
    void abortSweep();

    // DMM values submitted from QML after sweep completes:
    // dmmPoints is a list of {dmmV, dmmI} maps, parallel to sweep points
    void computeFit(int ch, const QVariantList& dmmPoints);

    // Write computed (or user-overridden) coefficients
    void writeCoefficients(int ch,
                           double outK,  double outB,
                           double measVK, double measVB,
                           double measIK, double measIB);

    // --- Commit ---
    void commitChannel(int ch);
    void commitAll();

    // --- Tests ---
    void startFunctionalTest(const QVariantMap& cfg);
    void startStressTest(const QVariantMap& cfg);
    void abortTest();

    // --- Report ---
    void generatePdf(const QString& path);
    void generateMarkdown(const QString& path);

    // --- Safe all (emergency) ---
    void safeAll();

signals:
    void portsChanged();
    void connectedChanged();
    void statusMessageChanged();
    void deviceInfoChanged();
    void calStateChanged();
    void sweepRunningChanged();
    void testRunningChanged();
    void reportMetaChanged();

    // Sweep step-by-step progress
    void sweepStep(int step, int total, int dacCode, qint32 adcV, qint32 adcI);
    // Sweep finished (ch, points as VariantList of {dacCode,adcV,adcI})
    void sweepFinished(int ch, QVariantList points);
    void sweepFailed(int ch, QString message);

    // Fit result after computeFit()
    void fitReady(int ch, QVariantMap outFit, QVariantMap measVFit, QVariantMap measIFit);

    // Write coefficients result
    void writeCoeffsResult(int ch, bool ok, QString message);

    // Commit
    void channelCommitted(int ch, bool ok, QString message);

    // Tests
    void funcTestProgress(int ch, int pointIdx, int total);
    void funcTestDone(bool pass, QVariantList points);
    void stressProgress(int ch, double elapsed, double total, double voltageV);
    void stressDone(bool pass, QVariantList channels);

    // Report
    void reportGenerated(bool ok, QString pathOrError);

private slots:
    void onSweepStep(int step, int total, int dacCode, qint32 adcV, qint32 adcI);
    void onSweepComplete(ChannelCalData data);
    void onSweepFailed(QString msg);

    void onFuncTestComplete(FuncTestResult result);
    void onFuncTestProgress(int ch, int idx, int total);
    void onStressProgress(int ch, double elapsed, double total, double v);
    void onStressComplete(StressTestResult result);

private:
    void setStatus(const QString& msg);
    void readDeviceInfo();
    void readChannelCaps();

    HvbModbusClient  m_client;
    QMutex           m_clientMutex;
    QSettings        m_settings;

    // Device state
    SystemInfo       m_sysInfo;
    QList<uint16_t>  m_chCaps;          // per-channel cap flags
    QList<ChannelCalConfig> m_nvsCoeffs; // saved at session start

    // Cal session
    bool             m_calUnlocked = false;
    bool             m_calActive   = false;

    // Sweep worker
    QThread          m_sweepThread;
    CalibrationWorker* m_sweepWorker = nullptr;
    bool             m_sweepRunning = false;
    QList<ChannelCalData> m_calData; // indexed by ch

    // Test engine
    QThread          m_testThread;
    TestEngine*      m_testWorker = nullptr;
    bool             m_testRunning = false;

    // Report data
    ReportData       m_reportData;

    QString          m_statusMessage;
    QStringList      m_ports;
};

} // namespace hvb::factory
