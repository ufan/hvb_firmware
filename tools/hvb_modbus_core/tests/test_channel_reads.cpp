#include "hvb_modbus_client.h"
#include "types.h"

#include <catch2/catch_test_macros.hpp>

static void fillChannelDefaults(uint16_t* inputRegs, uint16_t* holdingRegs) {
    for (int ch = 0; ch < 2; ++ch) {
        uint16_t base = 40 + ch * 40;
        inputRegs[base + 9]  = 0x0007;  // CapFlags: OutEn + CurrMeas + AutoRec (CH_CAPABILITY_FLAGS)
        holdingRegs[base + 12] = 10;    // Current safe band pct (CH_CURRENT_SAFE_BAND_PCT)
        holdingRegs[base + 15] = 32767; // I limit threshold (CH_CURRENT_LIMIT_THRESHOLD)
        holdingRegs[base + 20] = 10000; // Out cal K (CH_OUTPUT_CAL_K)
        holdingRegs[base + 22] = 10000; // Meas V cal K (CH_MEASURED_V_CAL_K)
        holdingRegs[base + 24] = 10000; // Meas I cal K (CH_MEASURED_I_CAL_K)
    }
}

TEST_CASE("ChannelInfo — defaults both channels", "[channel-reads]") {
    uint16_t inputRegs[280] = {};
    uint16_t holdingRegs[280] = {};
    fillChannelDefaults(inputRegs, holdingRegs);

    hvb::HvbModbusClient client;
    client.attachTestArrays(inputRegs, holdingRegs, 280);

    for (int ch = 0; ch < 2; ++ch) {
        DYNAMIC_SECTION("Channel " << ch) {
            auto ci = client.readChannelInfo(ch);
            CHECK(ci.voltageRaw == 0);
            CHECK(ci.currentRaw == 0);
            CHECK(ci.status == 0);
            CHECK(ci.chCapFlags == 0x0007);
        }
    }
}

TEST_CASE("ChannelInfo — enabled channel has measurements", "[channel-reads]") {
    uint16_t inputRegs[280] = {};
    uint16_t holdingRegs[280] = {};
    fillChannelDefaults(inputRegs, holdingRegs);

    // CH0 enabled: Vmeas=5000, Imeas=1234, target=5000, status=0x0003
    inputRegs[40 + 10] = 5000;    // CH_MEASURED_VOLTAGE
    inputRegs[40 + 11] = 1234;    // CH_MEASURED_CURRENT
    inputRegs[40 + 8]  = 5000;    // CH_OPER_TARGET_VOLTAGE
    inputRegs[40 + 0]  = 0x0003;  // CH_STATUS_BITS

    hvb::HvbModbusClient client;
    client.attachTestArrays(inputRegs, holdingRegs, 280);

    auto ci = client.readChannelInfo(0);
    CHECK(ci.voltageRaw == 5000);
    CHECK(ci.currentRaw == 1234);
    CHECK(ci.operationalTargetVoltageRaw == 5000);
    CHECK(ci.status == 0x0003);
}

TEST_CASE("ChannelConfig — defaults", "[channel-reads]") {
    uint16_t inputRegs[280] = {};
    uint16_t holdingRegs[280] = {};
    fillChannelDefaults(inputRegs, holdingRegs);

    hvb::HvbModbusClient client;
    client.attachTestArrays(inputRegs, holdingRegs, 280);

    auto cfg = client.readChannelConfig(0);
    CHECK(cfg.configuredTargetVRaw == 0);
    CHECK(cfg.outputAction == hvb::OutputAction::None);
    CHECK(cfg.recoveryPolicyMode == hvb::RecoveryPolicy::ManualLatch);
    CHECK(cfg.currentSafeBandPct == 10);

    auto cal = client.readChannelCalConfig(0);
    CHECK(cal.outCalK == 10000);
    CHECK(cal.outCalB == 0);
}
