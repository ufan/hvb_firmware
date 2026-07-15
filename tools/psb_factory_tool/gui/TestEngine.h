#pragma once
#include <QList>
#include <QMutex>
#include <QObject>
#include <atomic>
#include "TestResult.h"
#include "psb_modbus_client.h"

namespace psb::factory {

struct FuncTestConfig {
    int             ch            = -1;   // -1 = all
    int             numChannels   = 1;
    QList<double>   targetVolts;          // shared target list (V, engineering)
    bool            perChannel    = false;
    QList<QList<double>> chTargets;       // per-channel targets when perChannel=true
    double          tolerancePct  = 2.0;
    int             settleMs      = 500;
    int             retriesOnFault = 2;
};

struct StressConfig {
    int           ch            = -1;   // -1 = all
    int           numChannels   = 1;
    QList<double> targetVolts;          // per-channel target (V)
    int           durationSec   = 120;
    int           pollMs        = 1000;
    int           faultTolerance = 0;
    double        tolerancePct  = 5.0;
};

class TestEngine : public QObject {
    Q_OBJECT
public:
    explicit TestEngine(QObject* parent = nullptr);

    void setClient(PsbModbusClient* client, QMutex* mutex);
    void abort();

public slots:
    void runFunctionalTest(FuncTestConfig cfg);
    void runStressTest(StressConfig cfg);

signals:
    void funcTestProgress(int ch, int pointIdx, int total);
    void funcTestComplete(FuncTestResult result);
    void stressProgress(int ch, double elapsed, double total, double voltageV);
    void stressComplete(StressTestResult result);
    void testError(QString message);
    void testAborted();

private:
    PsbModbusClient*  m_client = nullptr;
    QMutex*           m_mutex  = nullptr;
    std::atomic<bool> m_abort{false};

    bool runFuncChannel(int ch, const QList<double>& targets,
                        double tol, int settleMs, int retries,
                        QList<FuncTestPoint>& out);
    bool runStressChannel(int ch, double targetV, const StressConfig& cfg,
                          StressChannelResult& out);

    int16_t voltsToRaw(double v) const;
    double  rawToVolts(int16_t raw) const;
};

} // namespace psb::factory
