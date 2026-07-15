#include "psb_modbus_client.h"
#include "types.h"
#include "register_map.h"

#include <catch2/catch_test_macros.hpp>

static void fillDefaultInputRegs(uint16_t* regs) {
    regs[0] = 3;    // Protocol Major
    regs[1] = 0;    // Protocol Minor
    regs[2] = 1;    // Variant ID
    regs[3] = SYS_CAP_AUTOMATIC_MODE | SYS_CAP_ENV_SENSOR; // Capability flags (Auto + Env)
    regs[4] = 2;    // Supported channels
    regs[5] = 0x0003; // Active channel mask
    regs[6] = 254;  // Board temp (25.4C)
    regs[7] = 452;  // Board humidity (45.2%)
    regs[12] = 0;   // Operating mode = Normal
}

static void fillDefaultHoldingRegs(uint16_t* regs) {
    regs[0] = 0;    // OpMode = Normal
    regs[1] = 0;    // StartupChannelPolicy = 0
    regs[2] = 1;    // Slave address
    regs[3] = 0;    // BaudRateCode = 0

    for (int ch = 0; ch < 2; ++ch) {
        uint16_t base = 40 + ch * 40;
        regs[base + 15] = 32767; // I limit threshold (CH_CURRENT_LIMIT_THRESHOLD)
        regs[base + 20] = 10000; // Out cal K (CH_OUTPUT_CAL_K)
        regs[base + 22] = 10000; // Meas V cal K (CH_MEASURED_V_CAL_K)
        regs[base + 24] = 10000; // Meas I cal K (CH_MEASURED_I_CAL_K)
    }
}

TEST_CASE("SystemInfo — defaults", "[system-reads]") {
    uint16_t inputRegs[280] = {};
    uint16_t holdingRegs[280] = {};
    fillDefaultInputRegs(inputRegs);

    psb::PsbModbusClient client;
    client.attachTestArrays(inputRegs, holdingRegs, 280);

    auto info = client.readSystemInfo();
    CHECK(info.protoMajor == 3);
    CHECK(info.protoMinor == 0);
    CHECK(info.variantId == 1);
    CHECK(info.supportedChannels == 2);
    CHECK(info.activeChMask == 0x0003);
    CHECK(info.boardTempRaw == 254);
    CHECK(info.boardHumidityRaw == 452);
    CHECK(info.activeOpMode == psb::OpMode::Normal);
    CHECK(info.sysStatus == 0);
    CHECK(info.faultCause == 0);
}

TEST_CASE("SystemConfig — defaults", "[system-reads]") {
    uint16_t inputRegs[280] = {};
    uint16_t holdingRegs[280] = {};
    fillDefaultHoldingRegs(holdingRegs);

    psb::PsbModbusClient client;
    client.attachTestArrays(inputRegs, holdingRegs, 280);

    auto cfg = client.readSystemConfig();
    CHECK(cfg.operatingMode == psb::OpMode::Normal);
    CHECK(cfg.startupChannelPolicy == 0);
    CHECK(cfg.slaveAddr == 1);
    CHECK(cfg.baudRateCode == 0);
}

TEST_CASE("SystemInfo — capability flags decoded", "[system-reads]") {
    uint16_t inputRegs[280] = {};
    uint16_t holdingRegs[280] = {};
    fillDefaultInputRegs(inputRegs);

    psb::PsbModbusClient client;
    client.attachTestArrays(inputRegs, holdingRegs, 280);

    auto info = client.readSystemInfo();
    CHECK((info.sysCapFlags & psb::SysCap::AUTOMATIC_MODE) != 0);
    CHECK((info.sysCapFlags & psb::SysCap::ENV_SENSOR) != 0);
}
