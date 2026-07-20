#pragma once
#include <QList>
#include <QMetaType>
#include <QString>

namespace psb::factory {

struct FuncTestPoint {
    int     ch           = 0;
    double  targetV      = 0.0;
    double  tolerancePct = 2.0;
    double  measuredV    = 0.0;
    double  errorPct     = 0.0;
    bool    pass         = false;
    QString detail;
};

struct FuncTestResult {
    QList<FuncTestPoint> points;
    bool    pass      = false;
    QString timestamp;
};

struct StressSample {
    double elapsedSec = 0.0;
    double voltageV   = 0.0;
    bool   fault      = false;
};

struct StressChannelResult {
    int     ch          = 0;
    double  targetV     = 0.0;
    double  durationSec = 0.0;
    double  avgV        = 0.0;
    double  minV        = 0.0;
    double  maxV        = 0.0;
    double  stddevV     = 0.0;
    int     faultCount  = 0;
    bool    pass        = false;
    QList<StressSample> samples;
};

struct StressTestResult {
    QList<StressChannelResult> channels;
    bool    pass      = false;
    QString timestamp;
};

} // namespace psb::factory

Q_DECLARE_METATYPE(psb::factory::FuncTestResult)
Q_DECLARE_METATYPE(psb::factory::StressTestResult)
