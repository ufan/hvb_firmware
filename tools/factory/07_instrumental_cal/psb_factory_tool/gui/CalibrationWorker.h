#pragma once
#include <QMutex>
#include <QObject>
#include <atomic>
#include "SweepData.h"
#include "psb_modbus_client.h"

namespace psb::factory {

struct SweepConfig {
    int dacMin        = 0;
    int dacMax        = 4000;
    int stepSize      = 800;
    int settlementMs  = 200;
    int cooldownMs    = 100;
};

class CalibrationWorker : public QObject {
    Q_OBJECT
public:
    explicit CalibrationWorker(QObject* parent = nullptr);

    void setClient(PsbModbusClient* client, QMutex* mutex);
    void abort();

public slots:
    void runSweep(int ch, SweepConfig cfg, bool hasOut, bool hasMeasV, bool hasMeasI);

    // Quick (single/few-transaction) operations — moved here from
    // CalibrationBackend, which used to call PsbModbusClient directly on the
    // main/GUI thread (blocking the UI for the duration of every connect,
    // unlock, write, commit, and safe-all). This is the same class of bug
    // that caused demo_tui's original connect-freeze/button-latency issues;
    // demo_gui's ModbusWorker uses this exact worker/result-signal split.
    void doScanPorts();
    void doConnect(QString port, int baud, int slaveId);
    void doDisconnect();
    void doUnlockAndEnter(int numChannels);
    void doExitCalMode();
    void doWriteCoefficients(int ch, bool hasOut, bool hasMeasV, bool hasMeasI,
                              quint16 outK, qint16 outB,
                              quint16 measVK, qint16 measVB,
                              quint16 measIK, qint16 measIB);
    void doCommitChannel(int ch);
    void doSafeAll(int numChannels);

signals:
    void stepComplete(int step, int total, int dacCode, qint32 adcV, qint32 adcI);
    void sweepComplete(ChannelCalData data);
    void sweepFailed(QString message);
    void sweepAborted();

    void portsScanned(QStringList ports);
    void connectResult(bool ok, QString error, psb::SystemInfo sysInfo, QList<uint16_t> chCaps);
    void disconnectDone();
    void unlockResult(bool ok, QString error, QList<uint16_t> chCaps,
                       QList<psb::ChannelCalConfig> nvsCoeffs);
    void exitCalDone(bool ok);
    void writeCoeffsDone(int ch, bool ok, QString error);
    void commitDone(int ch, bool ok, QString error);
    void safeAllDone();

private:
    PsbModbusClient*   m_client = nullptr;
    QMutex*            m_mutex  = nullptr;
    std::atomic<bool>  m_abort{false};
};

} // namespace psb::factory

Q_DECLARE_METATYPE(psb::SystemInfo)
Q_DECLARE_METATYPE(psb::ChannelCalConfig)
