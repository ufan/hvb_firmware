#include <catch2/catch_test_macros.hpp>
#include "../../psb_demo_app/tui/tui_format.h"

using namespace psb::tui;
using namespace psb;

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

TEST_CASE("fmtCurrentAuto is variant-aware via the unitExp parameter") {
    // jw_hvb-style (unitExp=-10, 0.1nA/LSB): auto-selects uA/nA by magnitude.
    CHECK(fmtCurrentAuto(32767, -10) == "+3.277 uA");
    CHECK(fmtCurrentAuto(0, -10)     == "+0.000 nA");
    CHECK(fmtCurrentAuto(1000, -10)  == "+100.000 nA");
    CHECK(fmtCurrentAuto(-1000, -10) == "-100.000 nA");

    // jw_lvb-style (unitExp=-1, 0.1A/LSB): the same raw magnitudes that read
    // as nA/uA above now correctly read as whole amps/milliamps — this is
    // exactly the case a hardcoded "nA" label got wrong before.
    CHECK(fmtCurrentAuto(500, -1) == "+50.000 A");
    CHECK(fmtCurrentAuto(5, -1)   == "+500.000 mA");
}

TEST_CASE("fmtInterval") {
    CHECK(fmtInterval(0)   == "0.0 s");
    CHECK(fmtInterval(10)  == "1.0 s");
    CHECK(fmtInterval(100) == "10.0 s");
}
