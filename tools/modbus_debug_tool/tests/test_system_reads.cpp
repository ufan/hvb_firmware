#include "hvb_modbus_client.h"
#include "types.h"

#include <catch2/catch_test_macros.hpp>

static void fillDefaultInputRegs(uint16_t* regs) {
    regs[0] = 2;    // Protocol Major
    regs[1] = 0;    // Protocol Minor
    regs[2] = 1;    // Variant ID
    regs[3] = 0x0003; // Capability flags (Auto + Env)
    regs[4] = 2;    // Supported channels
    regs[5] = 0x0003; // Active channel mask
    regs[6] = 254;  // Board temp (25.4C)
    regs[7] = 452;  // Board humidity (45.2%)
    regs[12] = 0;   // Operating mode = Normal
}

static void fillDefaultHoldingRegs(uint16_t* regs) {
    regs[0] = 0;    // OpMode = Normal
    regs[1] = 1;    // Slave address
    regs[3] = 0;    // Recovery = ManualLatch
    regs[7] = 10;   // V safe band
    regs[8] = 10;   // I safe band

    for (int ch = 0; ch < 2; ++ch) {
        uint16_t base = 40 + ch * 40;
        regs[base + 9] = 20000;  // V limit threshold
        regs[base + 12] = 32767; // I limit threshold
        regs[base + 15] = 10000; // Out cal K
        regs[base + 16] = 0;     // Out cal B
        regs[base + 17] = 10000; // Meas V cal K
        regs[base + 19] = 10000; // Meas I cal K
    }
}

TEST_CASE("SystemInfo — defaults", "[system-reads]") {
    uint16_t inputRegs[280] = {};
    uint16_t holdingRegs[280] = {};
    fillDefaultInputRegs(inputRegs);

    hvb::HvbModbusClient client;
    client.attachTestArrays(inputRegs, holdingRegs, 280);

    auto info = client.readSystemInfo();
    CHECK(info.protoMajor == 2);
    CHECK(info.protoMinor == 0);
    CHECK(info.variantId == 1);
    CHECK(info.supportedChannels == 2);
    CHECK(info.activeChMask == 0x0003);
    CHECK(info.boardTempRaw == 254);
    CHECK(info.boardHumidityRaw == 452);
    CHECK(info.activeOpMode == hvb::OpMode::Normal);
    CHECK(info.sysStatus == 0);
    CHECK(info.faultCause == 0);
}

TEST_CASE("SystemConfig — defaults", "[system-reads]") {
    uint16_t inputRegs[280] = {};
    uint16_t holdingRegs[280] = {};
    fillDefaultHoldingRegs(holdingRegs);

    hvb::HvbModbusClient client;
    client.attachTestArrays(inputRegs, holdingRegs, 280);

    auto cfg = client.readSystemConfig();
    CHECK(cfg.operatingMode == hvb::OpMode::Normal);
    CHECK(cfg.slaveAddr == 1);
    CHECK(cfg.baudRateCode == 0);
    CHECK(cfg.recoveryPolicy == hvb::RecoveryPolicy::ManualLatch);
    CHECK(cfg.voltageSafeBandPct == 10);
    CHECK(cfg.currentSafeBandPct == 10);
}

TEST_CASE("SystemInfo — capability flags decoded", "[system-reads]") {
    uint16_t inputRegs[280] = {};
    uint16_t holdingRegs[280] = {};
    fillDefaultInputRegs(inputRegs);

    hvb::HvbModbusClient client;
    client.attachTestArrays(inputRegs, holdingRegs, 280);

    auto info = client.readSystemInfo();
    CHECK((info.sysCapFlags & hvb::SysCap::AUTO_MODE_SUPPORTED) != 0);
    CHECK((info.sysCapFlags & hvb::SysCap::ENV_SENSOR_PRESENT) != 0);
}
