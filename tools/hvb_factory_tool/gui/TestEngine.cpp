#include "TestEngine.h"
#include "register_map.h"
#include <QDateTime>
#include <QMutexLocker>
#include <QThread>
#include <cmath>

namespace hvb::factory {

// 1 LSB = 1 mV (matches firmware reg::voltageToV / voltageFromV scaling)
int16_t TestEngine::voltsToRaw(double v) const { return static_cast<int16_t>(v * 1000.0); }
double  TestEngine::rawToVolts(int16_t raw)   const { return raw / 1000.0; }

TestEngine::TestEngine(QObject* parent) : QObject(parent) {}

void TestEngine::setClient(HvbModbusClient* client, QMutex* mutex) {
    m_client = client;
    m_mutex  = mutex;
}

void TestEngine::abort() { m_abort = true; }

// ---------------------------------------------------------------------------
// Functional test
// ---------------------------------------------------------------------------

bool TestEngine::runFuncChannel(int ch, const QList<double>& targets,
                                 double tol, int settleMs, int retries,
                                 QList<FuncTestPoint>& out) {
    for (int i = 0; i < targets.size() && !m_abort; ++i) {
        double target = targets[i];

        bool pointOk = false;
        int  attempt = 0;
        double measured = 0.0;
        QString detail;

        while (attempt <= retries && !m_abort) {
            // Set target + enable
            {
                QMutexLocker lk(m_mutex);
                m_client->writeConfiguredTargetVoltage(ch, voltsToRaw(target));
                m_client->sendOutputAction(ch, OutputAction::Enable);
            }
            QThread::msleep(settleMs);

            // Read measurement
            ChannelInfo ci;
            {
                QMutexLocker lk(m_mutex);
                ci = m_client->readChannelInfo(ch);
            }

            // Disable output
            {
                QMutexLocker lk(m_mutex);
                m_client->sendOutputAction(ch, OutputAction::DisableGraceful);
            }

            if (ci.activeFault) {
                detail = QString("Fault on attempt %1").arg(attempt + 1);
                ++attempt;
                QThread::msleep(200);
                continue;
            }

            measured = rawToVolts(ci.voltageRaw);
            double errPct = (target > 0.001)
                ? (measured - target) / target * 100.0
                : 0.0;
            if (std::abs(errPct) <= tol) {
                pointOk = true;
                detail = QString("attempt %1").arg(attempt + 1);
                break;
            }
            detail = QString("attempt %1: err %2%").arg(attempt + 1).arg(errPct, 0, 'f', 2);
            ++attempt;
        }

        double errPct = (target > 0.001) ? (measured - target) / target * 100.0 : 0.0;
        FuncTestPoint pt;
        pt.ch           = ch;
        pt.targetV      = target;
        pt.tolerancePct = tol;
        pt.measuredV    = measured;
        pt.errorPct     = errPct;
        pt.pass         = pointOk;
        pt.detail       = detail;
        out.append(pt);
    }
    return true;
}

void TestEngine::runFunctionalTest(FuncTestConfig cfg) {
    m_abort = false;
    FuncTestResult result;
    result.timestamp = QDateTime::currentDateTime().toString(Qt::ISODate);
    result.pass = true;

    auto channels = (cfg.ch == -1)
        ? [&]{ QList<int> l; for (int i = 0; i < cfg.numChannels; ++i) l << i; return l; }()
        : QList<int>{cfg.ch};

    int total = 0;
    for (int ch : channels)
        total += cfg.perChannel ? cfg.chTargets[ch].size() : cfg.targetVolts.size();

    int done = 0;
    for (int ch : channels) {
        if (m_abort) break;
        const auto& targets = cfg.perChannel ? cfg.chTargets[ch] : cfg.targetVolts;

        QList<FuncTestPoint> pts;
        if (!runFuncChannel(ch, targets, cfg.tolerancePct, cfg.settleMs,
                             cfg.retriesOnFault, pts)) {
            emit testError("Error during functional test");
            return;
        }
        for (auto& pt : pts) {
            if (!pt.pass) result.pass = false;
            result.points.append(pt);
            emit funcTestProgress(ch, done, total);
            ++done;
        }
    }

    emit funcTestComplete(result);
}

// ---------------------------------------------------------------------------
// Stress test
// ---------------------------------------------------------------------------

bool TestEngine::runStressChannel(int ch, double targetV, const StressConfig& cfg,
                                   StressChannelResult& out) {
    out.ch          = ch;
    out.targetV     = targetV;
    out.durationSec = cfg.durationSec;
    out.minV        = 1e9;
    out.maxV        = -1e9;

    double sum = 0, sum2 = 0;
    int    n   = 0;

    // Enable output
    {
        QMutexLocker lk(m_mutex);
        m_client->writeConfiguredTargetVoltage(ch, voltsToRaw(targetV));
        m_client->sendOutputAction(ch, OutputAction::Enable);
    }

    QThread::msleep(500);  // initial settle

    double elapsed = 0.0;
    while (elapsed < cfg.durationSec && !m_abort && out.faultCount <= cfg.faultTolerance) {
        QThread::msleep(cfg.pollMs);
        elapsed += cfg.pollMs / 1000.0;

        ChannelInfo ci;
        {
            QMutexLocker lk(m_mutex);
            ci = m_client->readChannelInfo(ch);
        }

        bool fault = ci.activeFault != 0;
        double v   = rawToVolts(ci.voltageRaw);

        StressSample s;
        s.elapsedSec = elapsed;
        s.voltageV   = v;
        s.fault      = fault;
        out.samples.append(s);

        if (fault) {
            ++out.faultCount;
            if (out.faultCount > cfg.faultTolerance) break;
        }

        if (v < out.minV) out.minV = v;
        if (v > out.maxV) out.maxV = v;
        sum  += v;
        sum2 += v * v;
        ++n;

        emit stressProgress(ch, elapsed, cfg.durationSec, v);
    }

    // Disable output
    {
        QMutexLocker lk(m_mutex);
        m_client->sendOutputAction(ch, OutputAction::DisableGraceful);
    }

    if (n > 0) {
        out.avgV    = sum / n;
        out.stddevV = std::sqrt(sum2 / n - out.avgV * out.avgV);
    }

    double errPct = (targetV > 0.001)
        ? std::abs(out.avgV - targetV) / targetV * 100.0 : 0.0;
    out.pass = (out.faultCount <= cfg.faultTolerance) && (errPct <= cfg.tolerancePct);
    return true;
}

void TestEngine::runStressTest(StressConfig cfg) {
    m_abort = false;
    StressTestResult result;
    result.timestamp = QDateTime::currentDateTime().toString(Qt::ISODate);
    result.pass = true;

    auto channels = (cfg.ch == -1)
        ? [&]{ QList<int> l; for (int i = 0; i < cfg.numChannels; ++i) l << i; return l; }()
        : QList<int>{cfg.ch};

    for (int i = 0; i < channels.size() && !m_abort; ++i) {
        int    ch      = channels[i];
        double target  = (i < cfg.targetVolts.size()) ? cfg.targetVolts[i] : 0.0;

        StressChannelResult chResult;
        if (!runStressChannel(ch, target, cfg, chResult)) {
            emit testError("Error during stress test");
            return;
        }
        result.channels.append(chResult);
        if (!chResult.pass) result.pass = false;
    }

    if (m_abort)
        emit testAborted();
    else
        emit stressComplete(result);
}

} // namespace hvb::factory
