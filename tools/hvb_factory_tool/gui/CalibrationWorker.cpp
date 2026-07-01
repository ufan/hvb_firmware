#include "CalibrationWorker.h"
#include <QMutexLocker>
#include <QThread>

namespace hvb::factory {

CalibrationWorker::CalibrationWorker(QObject* parent) : QObject(parent) {}

void CalibrationWorker::setClient(HvbModbusClient* client, QMutex* mutex) {
    m_client = client;
    m_mutex  = mutex;
}

void CalibrationWorker::abort() { m_abort = true; }

void CalibrationWorker::runSweep(int ch, SweepConfig cfg,
                                  bool hasOut, bool hasMeasV, bool hasMeasI) {
    m_abort = false;
    ChannelCalData data;
    data.ch       = ch;
    data.hasOut   = hasOut;
    data.hasMeasV = hasMeasV;
    data.hasMeasI = hasMeasI;
    data.needsCal = hasOut || hasMeasV || hasMeasI;

    auto safeOff = [&] {
        QMutexLocker lk(m_mutex);
        m_client->writeRawDacCode(ch, 0);
        m_client->writeCalibrationOutputEnable(ch, false);
    };

    // Enable cal output (only if channel has raw output drive capability)
    if (hasOut || hasMeasV || hasMeasI) {
        QMutexLocker lk(m_mutex);
        if (!m_client->writeCalibrationOutputEnable(ch, true)) {
            emit sweepFailed(QString::fromStdString(m_client->lastError()));
            return;
        }
    }

    const int total = (cfg.dacMax - cfg.dacMin) / cfg.stepSize + 1;
    int step = 0;
    bool ok = true;

    for (int dac = cfg.dacMin; dac <= cfg.dacMax && !m_abort && ok; dac += cfg.stepSize) {
        {
            QMutexLocker lk(m_mutex);
            if (!m_client->writeRawDacCode(ch, static_cast<uint16_t>(dac))) {
                emit sweepFailed(QString::fromStdString(m_client->lastError()));
                ok = false;
                break;
            }
        }

        QThread::msleep(cfg.settlementMs);
        if (m_abort) break;

        {
            QMutexLocker lk(m_mutex);
            if (!m_client->sendCalibrationSampleCommand(ch)) {
                emit sweepFailed(QString::fromStdString(m_client->lastError()));
                ok = false;
                break;
            }
        }

        QThread::msleep(cfg.cooldownMs);
        if (m_abort) break;

        CalibrationSnapshot snap;
        {
            QMutexLocker lk(m_mutex);
            snap = m_client->readCalibrationSnapshot(ch);
        }

        SweepPoint pt;
        pt.dacCode = dac;
        pt.adcV    = snap.rawAdcVoltage;
        pt.adcI    = snap.rawAdcCurrent;
        data.points.append(pt);

        emit stepComplete(step, total, dac, snap.rawAdcVoltage, snap.rawAdcCurrent);
        ++step;
    }

    safeOff();

    if (ok && !m_abort)
        emit sweepComplete(data);
    else if (m_abort)
        emit sweepAborted();
}

} // namespace hvb::factory
