#include "SweepData.h"
#include <cmath>

namespace hvb::factory {

FitResult linearRegression(const QList<double>& x, const QList<double>& y) {
    FitResult r;
    int n = x.size();
    if (n < 2 || n != y.size()) return r;

    double sumX = 0, sumY = 0, sumXY = 0, sumX2 = 0;
    for (int i = 0; i < n; ++i) {
        sumX  += x[i];
        sumY  += y[i];
        sumXY += x[i] * y[i];
        sumX2 += x[i] * x[i];
    }
    double denom = n * sumX2 - sumX * sumX;
    if (std::abs(denom) < 1e-12) return r;

    double slope     = (n * sumXY - sumX * sumY) / denom;
    double intercept = (sumY - slope * sumX) / n;

    // R²
    double meanY = sumY / n;
    double ssTot = 0, ssRes = 0;
    for (int i = 0; i < n; ++i) {
        double diff = y[i] - meanY;
        ssTot += diff * diff;
        double res = y[i] - (slope * x[i] + intercept);
        ssRes += res * res;
    }
    r.r2    = (ssTot < 1e-12) ? 1.0 : 1.0 - ssRes / ssTot;
    r.k     = slope;       // caller multiplies by 10000 to get device register value
    r.b     = intercept;
    r.valid = true;
    return r;
}

} // namespace hvb::factory
