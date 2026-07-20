#pragma once
#include "ReportData.h"
#include <QString>

namespace psb::factory {

class ReportEngine {
public:
    static bool generatePdf(const ReportData& data, const QString& path, QString* err = nullptr);
    static bool generateMarkdown(const ReportData& data, const QString& path, QString* err = nullptr);

private:
    static QString buildHtml(const ReportData& data);
    static QString buildMarkdown(const ReportData& data);
    static QString verdictBadge(bool pass);
    static QString fitTable(const ChannelCalData& cal);
    static QString funcTable(const FuncTestResult& r);
    static QString stressTable(const StressTestResult& r);
};

} // namespace psb::factory
