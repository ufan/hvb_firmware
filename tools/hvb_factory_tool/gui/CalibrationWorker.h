#pragma once
#include <QMutex>
#include <QObject>
#include <atomic>
#include "SweepData.h"
#include "hvb_modbus_client.h"

namespace hvb::factory {

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

    void setClient(HvbModbusClient* client, QMutex* mutex);
    void abort();

public slots:
    void runSweep(int ch, SweepConfig cfg, bool hasOut, bool hasMeasV, bool hasMeasI);

signals:
    void stepComplete(int step, int total, int dacCode, qint32 adcV, qint32 adcI);
    void sweepComplete(ChannelCalData data);
    void sweepFailed(QString message);
    void sweepAborted();

private:
    HvbModbusClient*   m_client = nullptr;
    QMutex*            m_mutex  = nullptr;
    std::atomic<bool>  m_abort{false};
};

} // namespace hvb::factory
