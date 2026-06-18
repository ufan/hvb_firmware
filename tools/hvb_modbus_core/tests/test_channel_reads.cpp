#include "hvb_modbus_client.h"
#include "types.h"

#include <catch2/catch_test_macros.hpp>

static void fillChannelDefaults(uint16_t* inputRegs, uint16_t* holdingRegs) {
    for (int ch = 0; ch < 2; ++ch) {
        uint16_t base = 40 + ch * 40;
        inputRegs[base + 11] = 0x0007;  // CapFlags: OutEn + CurrMeas + AutoRec
        holdingRegs[base + 9] = 20000;  // V limit threshold
        holdingRegs[base + 12] = 32767; // I limit threshold
        holdingRegs[base + 15] = 10000; // Out cal K
        holdingRegs[base + 17] = 10000; // Meas V cal K
        holdingRegs[base + 19] = 10000; // Meas I cal K
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
    inputRegs[40 + 0] = 5000;
    inputRegs[40 + 1] = 1234;
    inputRegs[40 + 2] = 5000;
    inputRegs[40 + 3] = 0x0003;

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
    CHECK(cfg.vProtMode == hvb::ProtectionMode::Disabled);
    CHECK(cfg.outCalK == 10000);
    CHECK(cfg.outCalB == 0);
}
