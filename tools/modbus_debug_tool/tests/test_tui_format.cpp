#include <catch2/catch_test_macros.hpp>
#include "../tui/tui_format.h"

using namespace hvb::tui;
using namespace hvb;

TEST_CASE("statusBadge") {
    CHECK(statusBadge(0x0000) == "OFF");
    CHECK(statusBadge(ChStatus::OUTPUT_DRIVE_NONZERO) == "ON");
    CHECK(statusBadge(ChStatus::RAMPING_ACTIVE) == "RAMP");
    CHECK(statusBadge(ChStatus::OUTPUT_DRIVE_NONZERO | ChStatus::RAMPING_ACTIVE) == "ON RAMP");
    CHECK(statusBadge(ChStatus::ACTIVE_FAULT) == "FAULT");
    CHECK(statusBadge(ChStatus::COOLDOWN_ACTIVE) == "COOL");
    CHECK(statusBadge(ChStatus::UNSUPPORTED) == "UNSUP");
    CHECK(statusBadge(ChStatus::RETRY_EXHAUSTED) == "RETRY-X");
}

TEST_CASE("faultStr") {
    CHECK(faultStr(0) == "--");
    CHECK(faultStr(FaultCause::VOLTAGE_LIMIT) == "VL");
    CHECK(faultStr(FaultCause::VOLTAGE_LIMIT | FaultCause::CURRENT_LIMIT) == "VL CL");
    CHECK(faultStr(FaultCause::OUTPUT_HW_FAULT) == "HW");
}

TEST_CASE("protCompact") {
    CHECK(protCompact(ProtectionMode::Disabled,          OutputAction::None)             == "Disabled");
    CHECK(protCompact(ProtectionMode::FlagOnly,          OutputAction::None)             == "FlagOnly");
    CHECK(protCompact(ProtectionMode::ApplyOutputAction, OutputAction::DisableGraceful)  == "Apply/Dis-Gr");
    CHECK(protCompact(ProtectionMode::ApplyOutputAction, OutputAction::DisableImmediate) == "Apply/Dis-Im");
    CHECK(protCompact(ProtectionMode::ApplyOutputAction, OutputAction::ForceOutputZero)  == "Apply/Force0");
    CHECK(protCompact(ProtectionMode::ApplyOutputAction, OutputAction::Clamp)            == "Apply/Clamp");
}

TEST_CASE("fmtVoltage") {
    CHECK(fmtVoltage(5000)  == "+500.0 V");
    CHECK(fmtVoltage(0)     == "+0.0 V");
    CHECK(fmtVoltage(-1000) == "-100.0 V");
}

TEST_CASE("fmtCurrentUA") {
    // 1 LSB = 1 nA; 32767 LSB = 32.767 µA
    CHECK(fmtCurrentUA(32767) == "+32.767 uA");
    CHECK(fmtCurrentUA(0)     == "+0.000 uA");
    CHECK(fmtCurrentUA(-1000) == "-1.000 uA");
}

TEST_CASE("fmtInterval") {
    CHECK(fmtInterval(0)   == "0.0 s");
    CHECK(fmtInterval(10)  == "1.0 s");
    CHECK(fmtInterval(100) == "10.0 s");
}
