#include "CalibrationWorker.h"
#include "reg_store/reg_map.h"
#include <QMutexLocker>
#include <QThread>

namespace psb::factory {

CalibrationWorker::CalibrationWorker(QObject* parent) : QObject(parent) {}

void CalibrationWorker::setClient(PsbModbusClient* client, QMutex* mutex) {
    m_client = client;
    m_mutex  = mutex;
}

void CalibrationWorker::abort() { m_abort = true; }

// ---------------------------------------------------------------------------
// Quick operations — see the "Quick operations" comment in CalibrationWorker.h
// ---------------------------------------------------------------------------

void CalibrationWorker::doScanPorts() {
    QStringList ports;
    for (const auto& p : PsbModbusClient::scanPorts())
        ports.append(QString::fromStdString(p));
    emit portsScanned(ports);
}

void CalibrationWorker::doConnect(QString port, int baud, int slaveId) {
    bool ok;
    QString err;
    psb::SystemInfo info;
    QList<uint16_t> caps;
    {
        QMutexLocker lk(m_mutex);
        ok = m_client->connect(port.toStdString(), baud, slaveId);
        if (ok) {
            info = m_client->readSystemInfo();
            int nch = info.supportedChannels;
            caps.reserve(nch);
            for (int ch = 0; ch < nch; ++ch)
                caps.append(m_client->readChannelInfo(ch).chCapFlags);
        } else {
            err = QString::fromStdString(m_client->lastError());
        }
    }
    emit connectResult(ok, err, info, caps);
}

void CalibrationWorker::doDisconnect() {
    QMutexLocker lk(m_mutex);
    m_client->disconnect();
    emit disconnectDone();
}

void CalibrationWorker::doUnlockAndEnter(int numChannels) {
    bool ok;
    QString err;
    QList<uint16_t> caps;
    QList<psb::ChannelCalConfig> nvsCoeffs;
    {
        QMutexLocker lk(m_mutex);
        ok = m_client->unlockCalibrationStep(CAL_UNLOCK_STEP1)
          && m_client->unlockCalibrationStep(CAL_UNLOCK_STEP2)
          && m_client->enterCalibrationMode();
        if (ok) {
            caps.reserve(numChannels);
            nvsCoeffs.reserve(numChannels);
            for (int ch = 0; ch < numChannels; ++ch) {
                uint16_t c = m_client->readChannelInfo(ch).chCapFlags;
                caps.append(c);
                nvsCoeffs.append(m_client->readChannelCalConfig(ch, c));
            }
        } else {
            err = QString::fromStdString(m_client->lastError());
        }
    }
    emit unlockResult(ok, err, caps, nvsCoeffs);
}

void CalibrationWorker::doExitCalMode() {
    bool ok;
    {
        QMutexLocker lk(m_mutex);
        ok = m_client->exitCalibrationMode();
    }
    emit exitCalDone(ok);
}

void CalibrationWorker::doWriteCoefficients(int ch, bool hasOut, bool hasMeasV, bool hasMeasI,
                                             quint16 outK, qint16 outB,
                                             quint16 measVK, qint16 measVB,
                                             quint16 measIK, qint16 measIB) {
    bool ok = true;
    QString err;
    {
        QMutexLocker lk(m_mutex);
        if (hasOut)   ok = ok && m_client->writeCalibrationOutput(ch, outK, outB);
        if (hasMeasV) ok = ok && m_client->writeCalibrationMeasV(ch, measVK, measVB);
        if (hasMeasI) ok = ok && m_client->writeCalibrationMeasI(ch, measIK, measIB);
        if (!ok) err = QString::fromStdString(m_client->lastError());
    }
    emit writeCoeffsDone(ch, ok, err);
}

void CalibrationWorker::doCommitChannel(int ch) {
    bool ok;
    QString err;
    {
        QMutexLocker lk(m_mutex);
        ok = m_client->sendCalibrationCommitCommand(ch);
        if (!ok) err = QString::fromStdString(m_client->lastError());
    }
    emit commitDone(ch, ok, err);
}

void CalibrationWorker::doSafeAll(int numChannels) {
    {
        QMutexLocker lk(m_mutex);
        for (int ch = 0; ch < numChannels; ++ch) {
            m_client->writeConfiguredTargetVoltage(ch, 0);
            m_client->sendOutputAction(ch, OutputAction::DisableImmediate);
        }
    }
    emit safeAllDone();
}

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

} // namespace psb::factory
