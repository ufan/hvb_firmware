#pragma once
#include "SweepData.h"
#include "TestResult.h"
#include "types.h"
#include <QList>
#include <QString>

namespace psb::factory {

struct ReportData {
    QString    boardSerial;
    QString    operatorId;
    QString    notes;
    QString    timestamp;

    SystemInfo deviceInfo;

    QList<ChannelCalData> calResults;
    bool calRun = false;

    FuncTestResult  funcTest;
    bool funcTestRun = false;

    StressTestResult stressTest;
    bool stressTestRun = false;

    bool overallPass() const {
        if (funcTestRun && !funcTest.pass)    return false;
        if (stressTestRun && !stressTest.pass) return false;
        return true;
    }
};

} // namespace psb::factory
