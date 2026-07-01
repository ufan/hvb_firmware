#pragma once
#include <QList>
#include <QMetaType>
#include <cstdint>

namespace hvb::factory {

struct SweepPoint {
    int   dacCode = 0;
    int32_t adcV  = 0;
    int32_t adcI  = 0;
    double  dmmV  = 0.0;
    double  dmmI  = 0.0;
    bool    dmmVSet = false;
    bool    dmmISet = false;
};

struct FitResult {
    double  k     = 1.0;     // raw K value (multiply by 10000 for uint16 register)
    double  b     = 0.0;     // raw B value (cast to int16 for register)
    double  r2    = 0.0;
    bool    valid = false;
};

struct ChannelCalData {
    int  ch       = 0;
    bool hasOut   = false;   // CH_CAP_RAW_OUTPUT_DRIVE
    bool hasMeasV = false;   // CH_CAP_VOLTAGE_MEASUREMENT
    bool hasMeasI = false;   // CH_CAP_CURRENT_MEASUREMENT
    bool needsCal = false;   // any of the above

    QList<SweepPoint> points;
    FitResult outFit;        // V  -> DAC
    FitResult measVFit;      // ADC -> V
    FitResult measIFit;      // ADC -> I

    bool coeffsWritten = false;
    bool committed     = false;
};

// Simple least-squares linear regression: y = slope*x + intercept
// Returns {k=slope, b=intercept, r2, valid}; caller multiplies k by 10000 for device register
FitResult linearRegression(const QList<double>& x, const QList<double>& y);

} // namespace hvb::factory

Q_DECLARE_METATYPE(hvb::factory::ChannelCalData)
