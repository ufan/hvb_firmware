#include "ReportEngine.h"
#include "register_map.h"
#include <QFile>
#include <QPageSize>
#include <QPrinter>
#include <QTextDocument>
#include <QTextStream>

namespace psb::factory {

namespace vscale = psb::reg::scale;

QString ReportEngine::verdictBadge(bool pass) {
    return pass
        ? "<span style='color:green;font-weight:bold'>PASS</span>"
        : "<span style='color:red;font-weight:bold'>FAIL</span>";
}

QString ReportEngine::fitTable(const ChannelCalData& cal) {
    if (!cal.needsCal) return "<p><i>No calibration required for this channel.</i></p>";
    QString s = "<table border='1' cellpadding='4' cellspacing='0' width='100%'>"
                "<tr><th>Axis</th><th>K (device register)</th><th>B</th><th>R²</th><th>Points</th></tr>";
    auto row = [&](const QString& axis, const FitResult& f, bool active, double divisor) {
        if (!active) return;
        s += QString("<tr><td>%1</td><td>%2</td><td>%3</td><td>%4</td><td>%5</td></tr>")
                 .arg(axis)
                 .arg(qRound(f.k * divisor))
                 .arg(qRound(f.b))
                 .arg(f.r2, 0, 'f', 6)
                 .arg(cal.points.size());
    };
    row("out (V→DAC), K ×10000",       cal.outFit,   cal.hasOut,   vscale::OUTPUT_CAL_DIVISOR);
    row("meas-V (ADC→V), K ×1000000", cal.measVFit, cal.hasMeasV, vscale::MEAS_CAL_DIVISOR);
    row("meas-I (ADC→I), K ×1000000", cal.measIFit, cal.hasMeasI, vscale::MEAS_CAL_DIVISOR);
    s += "</table>";
    return s;
}

QString ReportEngine::funcTable(const FuncTestResult& r) {
    QString s = "<table border='1' cellpadding='4' cellspacing='0' width='100%'>"
                "<tr><th>Ch</th><th>Target V</th><th>Tol %</th>"
                "<th>Measured V</th><th>Error %</th><th>Result</th></tr>";
    for (const auto& pt : r.points) {
        s += QString("<tr><td>CH%1</td><td>%2</td><td>±%3</td><td>%4</td><td>%5</td><td>%6</td></tr>")
                 .arg(pt.ch)
                 .arg(pt.targetV,   0, 'f', 3)
                 .arg(pt.tolerancePct, 0, 'f', 1)
                 .arg(pt.measuredV, 0, 'f', 3)
                 .arg(pt.errorPct,  0, 'f', 2)
                 .arg(pt.pass ? "PASS" : "FAIL");
    }
    s += "</table>";
    return s;
}

QString ReportEngine::stressTable(const StressTestResult& r) {
    QString s = "<table border='1' cellpadding='4' cellspacing='0' width='100%'>"
                "<tr><th>Ch</th><th>Target V</th><th>Duration</th>"
                "<th>Avg V</th><th>Min V</th><th>Max V</th><th>σ V</th>"
                "<th>Faults</th><th>Result</th></tr>";
    for (const auto& ch : r.channels) {
        s += QString("<tr><td>CH%1</td><td>%2</td><td>%3 s</td>"
                     "<td>%4</td><td>%5</td><td>%6</td><td>%7</td><td>%8</td><td>%9</td></tr>")
                 .arg(ch.ch)
                 .arg(ch.targetV,     0, 'f', 3)
                 .arg(ch.durationSec, 0, 'f', 0)
                 .arg(ch.avgV,        0, 'f', 4)
                 .arg(ch.minV,        0, 'f', 4)
                 .arg(ch.maxV,        0, 'f', 4)
                 .arg(ch.stddevV,     0, 'f', 4)
                 .arg(ch.faultCount)
                 .arg(ch.pass ? "PASS" : "FAIL");
    }
    s += "</table>";
    return s;
}

QString ReportEngine::buildHtml(const ReportData& data) {
    const bool overall = data.overallPass();
    QString h = R"(
<html><head><style>
body { font-family: Arial, sans-serif; font-size: 10pt; margin: 20px; }
h1   { font-size: 16pt; }
h2   { font-size: 12pt; border-bottom: 1px solid #aaa; margin-top: 20px; }
h3   { font-size: 10pt; }
table{ border-collapse: collapse; margin-bottom: 12px; }
th   { background: #e0e0e0; }
td,th{ padding: 4px 8px; }
.pass{ color: green; font-weight: bold; }
.fail{ color: red;   font-weight: bold; }
</style></head><body>
)";

    h += QString("<h1>HVB Factory Report — <span class='%1'>%2</span></h1>")
             .arg(overall ? "pass" : "fail")
             .arg(overall ? "PASS" : "FAIL");

    // Header info
    h += "<table>";
    auto kv = [&](const QString& k, const QString& v) {
        h += QString("<tr><td><b>%1</b></td><td>%2</td></tr>").arg(k, v);
    };
    kv("Board Serial", data.boardSerial.isEmpty() ? "(not set)" : data.boardSerial);
    kv("Operator",     data.operatorId.isEmpty()  ? "(not set)" : data.operatorId);
    kv("Timestamp",    data.timestamp);
    kv("Protocol",     QString("%1.%2").arg(data.deviceInfo.protoMajor).arg(data.deviceInfo.protoMinor));
    kv("Firmware",     QString("0x%1").arg(data.deviceInfo.fwVersion, 8, 16, QChar('0')));
    kv("Variant ID",   QString::number(data.deviceInfo.variantId));
    kv("Channels",     QString::number(data.deviceInfo.supportedChannels));
    h += "</table>";

    if (!data.notes.isEmpty())
        h += "<p><b>Notes:</b> " + data.notes.toHtmlEscaped() + "</p>";

    // Calibration
    h += "<h2>Calibration</h2>";
    if (!data.calRun) {
        h += "<p><i>Not run.</i></p>";
    } else {
        for (const auto& cal : data.calResults) {
            h += QString("<h3>CH%1 — %2</h3>")
                     .arg(cal.ch)
                     .arg(cal.committed ? "Committed" : "Not committed");
            h += fitTable(cal);
        }
    }

    // Functional test
    h += "<h2>Functional Test</h2>";
    if (!data.funcTestRun) {
        h += "<p><i>Not run.</i></p>";
    } else {
        h += QString("<p>Overall: %1 &nbsp; Timestamp: %2</p>")
                 .arg(verdictBadge(data.funcTest.pass))
                 .arg(data.funcTest.timestamp);
        h += funcTable(data.funcTest);
    }

    // Stress test
    h += "<h2>Stress Test</h2>";
    if (!data.stressTestRun) {
        h += "<p><i>Not run.</i></p>";
    } else {
        h += QString("<p>Overall: %1 &nbsp; Timestamp: %2</p>")
                 .arg(verdictBadge(data.stressTest.pass))
                 .arg(data.stressTest.timestamp);
        h += stressTable(data.stressTest);
    }

    // Signature
    h += "<h2>Sign-off</h2>"
         "<table><tr><td width='300'><b>Operator signature:</b><br/><br/>"
         "___________________________</td>"
         "<td width='200'><b>Date:</b><br/><br/>_______________</td></tr></table>";

    h += "</body></html>";
    return h;
}

bool ReportEngine::generatePdf(const ReportData& data, const QString& path, QString* /*err*/) {
    QPrinter printer(QPrinter::HighResolution);
    printer.setOutputFormat(QPrinter::PdfFormat);
    printer.setOutputFileName(path);
    printer.setPageSize(QPageSize(QPageSize::A4));

    QTextDocument doc;
    doc.setHtml(buildHtml(data));
    doc.print(&printer);
    return true;
}

bool ReportEngine::generateMarkdown(const ReportData& data, const QString& path, QString* err) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (err) *err = f.errorString();
        return false;
    }
    QTextStream ts(&f);
    ts << buildMarkdown(data);
    return true;
}

QString ReportEngine::buildMarkdown(const ReportData& data) {
    QString md;
    md += QString("# HVB Factory Report — %1\n\n").arg(data.overallPass() ? "PASS" : "FAIL");
    md += QString("| Key | Value |\n|---|---|\n");
    md += QString("| Board Serial | %1 |\n").arg(data.boardSerial);
    md += QString("| Operator | %1 |\n").arg(data.operatorId);
    md += QString("| Timestamp | %1 |\n").arg(data.timestamp);
    md += QString("| Protocol | %1.%2 |\n").arg(data.deviceInfo.protoMajor).arg(data.deviceInfo.protoMinor);
    md += "\n";

    // Calibration
    md += "## Calibration\n\n";
    if (!data.calRun) { md += "_Not run._\n\n"; }
    else {
        for (const auto& cal : data.calResults) {
            md += QString("### CH%1\n\n").arg(cal.ch);
            if (!cal.needsCal) { md += "_No calibration required._\n\n"; continue; }
            md += "| Axis | K (device register) | B | R² |\n|---|---|---|---|\n";
            auto row = [&](const QString& ax, const FitResult& f, bool active, double divisor) {
                if (!active) return;
                md += QString("| %1 | %2 | %3 | %4 |\n")
                          .arg(ax).arg(qRound(f.k * divisor)).arg(qRound(f.b))
                          .arg(f.r2, 0, 'f', 6);
            };
            row("out (V→DAC), K ×10000",       cal.outFit,   cal.hasOut,   vscale::OUTPUT_CAL_DIVISOR);
            row("meas-V (ADC→V), K ×1000000", cal.measVFit, cal.hasMeasV, vscale::MEAS_CAL_DIVISOR);
            row("meas-I (ADC→I), K ×1000000", cal.measIFit, cal.hasMeasI, vscale::MEAS_CAL_DIVISOR);
            md += "\n";
        }
    }

    // Functional test
    md += "## Functional Test\n\n";
    if (!data.funcTestRun) { md += "_Not run._\n\n"; }
    else {
        md += QString("**Overall: %1**  Timestamp: %2\n\n")
                  .arg(data.funcTest.pass ? "PASS" : "FAIL")
                  .arg(data.funcTest.timestamp);
        md += "| Ch | Target V | Tol% | Measured V | Error% | Result |\n|---|---|---|---|---|---|\n";
        for (const auto& pt : data.funcTest.points)
            md += QString("| CH%1 | %2 | ±%3 | %4 | %5 | %6 |\n")
                      .arg(pt.ch).arg(pt.targetV,0,'f',3)
                      .arg(pt.tolerancePct,0,'f',1)
                      .arg(pt.measuredV,0,'f',3)
                      .arg(pt.errorPct,0,'f',2)
                      .arg(pt.pass ? "PASS" : "FAIL");
        md += "\n";
    }

    // Stress test
    md += "## Stress Test\n\n";
    if (!data.stressTestRun) { md += "_Not run._\n\n"; }
    else {
        md += QString("**Overall: %1**  Timestamp: %2\n\n")
                  .arg(data.stressTest.pass ? "PASS" : "FAIL")
                  .arg(data.stressTest.timestamp);
        md += "| Ch | Target V | Duration | Avg V | Min V | Max V | σ V | Faults | Result |\n"
              "|---|---|---|---|---|---|---|---|---|\n";
        for (const auto& ch : data.stressTest.channels)
            md += QString("| CH%1 | %2 | %3 s | %4 | %5 | %6 | %7 | %8 | %9 |\n")
                      .arg(ch.ch).arg(ch.targetV,0,'f',3).arg(ch.durationSec,0,'f',0)
                      .arg(ch.avgV,0,'f',4).arg(ch.minV,0,'f',4).arg(ch.maxV,0,'f',4)
                      .arg(ch.stddevV,0,'f',4).arg(ch.faultCount)
                      .arg(ch.pass ? "PASS" : "FAIL");
    }
    return md;
}

} // namespace psb::factory
