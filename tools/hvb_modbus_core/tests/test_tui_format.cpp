#include <catch2/catch_test_macros.hpp>
#include "../../hvb_demo_app/tui/tui_format.h"

using namespace hvb::tui;
using namespace hvb;

TEST_CASE("statusBadge") {
    CHECK(statusBadge(0x0000) == "OFF");
    CHECK(statusBadge(ChStatus::OUTPUT_DRIVE_NONZERO) == "ON");
    CHECK(statusBadge(ChStatus::RAMPING_ACTIVE) == "RAMP");
    CHECK(statusBadge(ChStatus::OUTPUT_DRIVE_NONZERO | ChStatus::RAMPING_ACTIVE) == "ON RAMP");
    CHECK(statusBadge(ChStatus::ACTIVE_FAULT) == "FAULT");
    CHECK(statusBadge(ChStatus::COOLDOWN_ACTIVE) == "COOL");
    CHECK(statusBadge(ChStatus::MEASUREMENT_STALE) == "STALE");
}

TEST_CASE("faultStr") {
    CHECK(faultStr(0) == "--");
    CHECK(faultStr(FaultCause::CURRENT) == "CL");
    CHECK(faultStr(FaultCause::CURRENT | FaultCause::MEASUREMENT) == "CL MI");
    CHECK(faultStr(FaultCause::HARDWARE) == "HW");
}

TEST_CASE("protCompact") {
    CHECK(protCompact(ProtectionMode::Disabled,          OutputAction::None)             == "Disabled");
    CHECK(protCompact(ProtectionMode::FlagOnly,          OutputAction::None)             == "FlagOnly");
    CHECK(protCompact(ProtectionMode::ApplyOutputAction, OutputAction::DisableGraceful)  == "Apply/Dis-Gr");
    CHECK(protCompact(ProtectionMode::ApplyOutputAction, OutputAction::DisableImmediate) == "Apply/Dis-Im");
    CHECK(protCompact(ProtectionMode::ApplyOutputAction, OutputAction::ForceOutputZero)  == "Apply/Force0");
    CHECK(protCompact(ProtectionMode::ApplyOutputAction, OutputAction::ForceOutputZero)  == "Apply/Force0");
}

TEST_CASE("fmtVoltage") {
    CHECK(fmtVoltage(5000)  == "+500.0 V");
    CHECK(fmtVoltage(0)     == "+0.0 V");
    CHECK(fmtVoltage(-1000) == "-100.0 V");
}

TEST_CASE("fmtCurrentUA") {
    // 1 LSB = 0.1 nA; 32767 LSB = 3.2767 µA
    CHECK(fmtCurrentUA(32767) == "+3.277 uA");
    CHECK(fmtCurrentUA(0)     == "+0.000 uA");
    CHECK(fmtCurrentUA(-1000) == "-0.100 uA");
}

TEST_CASE("fmtCurrentNA") {
    // Monitor table cell — one decimal place so sub-nA readings aren't
    // rounded away to an indistinguishable integer.
    CHECK(fmtCurrentNA(32767) == "+3276.7 nA");
    CHECK(fmtCurrentNA(0)     == "+0.0 nA");
    CHECK(fmtCurrentNA(-1000) == "-100.0 nA");
}

TEST_CASE("fmtInterval") {
    CHECK(fmtInterval(0)   == "0.0 s");
    CHECK(fmtInterval(10)  == "1.0 s");
    CHECK(fmtInterval(100) == "10.0 s");
}
