#include <catch2/catch_test_macros.hpp>
#include "register_meta.h"
#include "register_map.h"

using namespace psb::meta;

namespace {
const RegDesc& channelInputDesc(const char* name) {
    for (const auto& d : CHANNEL_INPUT) {
        if (std::string(d.name) == name) return d;
    }
    FAIL("no CHANNEL_INPUT register named " << name);
    static RegDesc dummy;
    return dummy;
}
const RegDesc& channelHoldingDesc(const char* name) {
    for (const auto& d : CHANNEL_HOLDING) {
        if (std::string(d.name) == name) return d;
    }
    FAIL("no CHANNEL_HOLDING register named " << name);
    static RegDesc dummy;
    return dummy;
}
} // namespace

TEST_CASE("formatValue prefers enum labels over scaling") {
    const RegDesc& mode = SYSTEM_HOLDING[0]; // "Operating Mode"
    REQUIRE(std::string(mode.name) == "Operating Mode");
    CHECK(formatValue(0, mode) == "Normal");
    CHECK(formatValue(2, mode) == "Calibration");
}

TEST_CASE("formatValue leaves unscaled fields as bare raw") {
    const RegDesc& band = channelHoldingDesc("Current Safe Band %");
    CHECK(band.scaleTo == 1.0);
    CHECK(formatValue(50, band) == "50");
}

TEST_CASE("formatValue converts unsigned scaled fields") {
    const RegDesc& outK = channelHoldingDesc("Output Calibration K");
    CHECK(formatValue(10000, outK) == "10000 (1 x)");

    const RegDesc& measVK = channelHoldingDesc("Meas V Calibration K");
    CHECK(formatValue(2387, measVK) == "2387 (0.002387 x)");
}

TEST_CASE("formatValue converts signed scaled fields, including negative values") {
    const RegDesc& measuredV = channelInputDesc("Measured Voltage");
    REQUIRE(std::string(measuredV.type) == "int16");

    CHECK(formatValue(2000, measuredV) == "2000 (200 V)");

    // The leading number is always the raw uint16 wire value (as displayed
    // everywhere else in this codebase); the parenthesized value is the
    // sign-aware physical conversion. -1000 as uint16 (two's complement) must
    // decode to -100 V here, not wrap to a huge positive value — this is the
    // bug class the unit-mismatch audit was chasing (a sign-oblivious
    // conversion silently producing garbage).
    uint16_t negRaw = static_cast<uint16_t>(static_cast<int16_t>(-1000));
    CHECK(formatValue(negRaw, measuredV) == "64536 (-100 V)");
}

TEST_CASE("formatValue converts current fields using the shared 0.1 nA/LSB scale") {
    const RegDesc& measuredI = channelInputDesc("Measured Current");
    REQUIRE(std::string(measuredI.type) == "int16");
    CHECK(formatValue(500, measuredI) == "500 (50 nA)");
}

TEST_CASE("register_map.h calibration divisors match register_meta.cpp's scaleTo") {
    // Single source of truth check: the Cal K RegDesc entries must use the
    // exact same named constants as the write path (CalibrationBackend.cpp)
    // and the read/display path (register_map.h's scale namespace), not an
    // independently hand-copied literal that could drift.
    CHECK(channelHoldingDesc("Output Calibration K").scaleTo == psb::reg::scale::OUTPUT_CAL_DIVISOR);
    CHECK(channelHoldingDesc("Meas V Calibration K").scaleTo == psb::reg::scale::MEAS_CAL_DIVISOR);
    CHECK(channelHoldingDesc("Meas I Calibration K").scaleTo == psb::reg::scale::MEAS_CAL_DIVISOR);
}

TEST_CASE("findDesc resolves channel registers across channel indices") {
    for (int ch = 0; ch < 4; ++ch) {
        uint16_t addr = psb::reg::chAddr(ch, 10); // Measured Voltage offset
        const RegDesc* d = findDesc(addr, false);
        REQUIRE(d != nullptr);
        CHECK(std::string(d->name) == "Measured Voltage");
    }
}
