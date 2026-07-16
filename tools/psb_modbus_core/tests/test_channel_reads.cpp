#include "psb_modbus_client.h"
#include "types.h"

#include <catch2/catch_test_macros.hpp>

static void fillChannelDefaults(uint16_t* inputRegs, uint16_t* holdingRegs) {
    for (int ch = 0; ch < 2; ++ch) {
        uint16_t base = 40 + ch * 40;
        inputRegs[base + 9]  = 0x000F;  // CapFlags: OutEn + RawDrive + VMeas + IMeas (CH_CAPABILITY_FLAGS)
        holdingRegs[base + 12] = 10;    // Current safe band pct (CH_CURRENT_SAFE_BAND_PCT)
        holdingRegs[base + 15] = 32767; // I limit threshold (CH_CURRENT_LIMIT_THRESHOLD)
        holdingRegs[base + 20] = 10000; // Out cal K (CH_OUTPUT_CAL_K)
        holdingRegs[base + 22] = 10000; // Meas V cal K (CH_MEASURED_V_CAL_K)
        holdingRegs[base + 24] = 10000; // Meas I cal K (CH_MEASURED_I_CAL_K)
    }
}

TEST_CASE("readChannelCapabilities reads the capability-flags register directly", "[channel-reads]") {
    uint16_t inputRegs[280] = {};
    uint16_t holdingRegs[280] = {};
    fillChannelDefaults(inputRegs, holdingRegs);

    psb::PsbModbusClient client;
    client.attachTestArrays(inputRegs, holdingRegs, 280);

    uint16_t caps = 0;
    REQUIRE(client.readChannelCapabilities(0, caps));
    CHECK(caps == 0x000F);
}

TEST_CASE("readChannelCapabilities fails without clobbering caller's value", "[channel-reads]") {
    uint16_t inputRegs[280] = {};
    uint16_t holdingRegs[280] = {};
    fillChannelDefaults(inputRegs, holdingRegs);

    psb::PsbModbusClient client;
    // Window too small to reach CH_CAPABILITY_FLAGS (offset 9) for ch0 (base 40).
    client.attachTestArrays(inputRegs, holdingRegs, 40 + 9);

    uint16_t caps = 0x1234;
    CHECK_FALSE(client.readChannelCapabilities(0, caps));
    CHECK(caps == 0x1234);
}

TEST_CASE("ChannelInfo — defaults both channels", "[channel-reads]") {
    uint16_t inputRegs[280] = {};
    uint16_t holdingRegs[280] = {};
    fillChannelDefaults(inputRegs, holdingRegs);

    psb::PsbModbusClient client;
    client.attachTestArrays(inputRegs, holdingRegs, 280);

    for (int ch = 0; ch < 2; ++ch) {
        DYNAMIC_SECTION("Channel " << ch) {
            auto ci = client.readChannelInfo(ch);
            CHECK(ci.voltageRaw == 0);
            CHECK(ci.currentRaw == 0);
            CHECK(ci.status == 0);
            CHECK(ci.chCapFlags == 0x000F);
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

    psb::PsbModbusClient client;
    client.attachTestArrays(inputRegs, holdingRegs, 280);

    auto ci = client.readChannelInfo(0);
    CHECK(ci.voltageRaw == 5000);
    CHECK(ci.currentRaw == 1234);
    CHECK(ci.operationalTargetVoltageRaw == 5000);
    CHECK(ci.status == 0x0003);
}

TEST_CASE("readChannelStatus/readSystemStatus accept an explicit poll timeout override", "[channel-reads]") {
    // Test-mode reads bypass the real port entirely, so an explicit
    // timeoutOverrideMs is a no-op here — this just confirms the added
    // parameter doesn't disturb existing behavior for callers that pass it.
    uint16_t inputRegs[280] = {};
    uint16_t holdingRegs[280] = {};
    fillChannelDefaults(inputRegs, holdingRegs);
    inputRegs[40 + 10] = 5000;
    inputRegs[40 + 11] = 1234;

    psb::PsbModbusClient client;
    client.attachTestArrays(inputRegs, holdingRegs, 280);

    psb::ChannelInfo ci;
    REQUIRE(client.readChannelStatus(0, 0x000F, ci, /*timeoutOverrideMs=*/300));
    CHECK(ci.voltageRaw == 5000);
    CHECK(ci.currentRaw == 1234);

    psb::SystemInfo si;
    CHECK(client.readSystemStatus(si, /*timeoutOverrideMs=*/300));

    uint16_t caps = 0;
    REQUIRE(client.readChannelCapabilities(0, caps, /*timeoutOverrideMs=*/300));
    CHECK(caps == 0x000F);
}

TEST_CASE("ChannelConfig — defaults", "[channel-reads]") {
    uint16_t inputRegs[280] = {};
    uint16_t holdingRegs[280] = {};
    fillChannelDefaults(inputRegs, holdingRegs);

    psb::PsbModbusClient client;
    client.attachTestArrays(inputRegs, holdingRegs, 280);

    auto cfg = client.readChannelConfig(0);
    CHECK(cfg.configuredTargetVRaw == 0);
    CHECK(cfg.outputAction == psb::OutputAction::None);
    CHECK(cfg.recoveryPolicyMode == psb::RecoveryPolicy::ManualLatch);
    CHECK(cfg.currentSafeBandPct == 10);

    auto cal = client.readChannelCalConfig(0);
    CHECK(cal.outCalK == 10000);
    CHECK(cal.outCalB == 0);
}

TEST_CASE("readChannelConfig merge-on-success — failed sub-block preserves prior value", "[channel-reads][merge]") {
    uint16_t inputRegs[280] = {};
    uint16_t holdingRegs[280] = {};
    fillChannelDefaults(inputRegs, holdingRegs);

    psb::PsbModbusClient client;
    client.attachTestArrays(inputRegs, holdingRegs, 280);

    psb::ChannelConfig cfg;
    client.readChannelConfig(0, 0x000F, cfg);
    REQUIRE(cfg.iLimitThresholdRaw == 32767);
    REQUIRE(cfg.currentSafeBandPct == 10);

    // Shrink the addressable window so the protection block (holding
    // offsets 13-15, CH_CURRENT_PROTECTION_MODE onward) fails while the
    // output (0-7) and recovery (8-12) blocks before it still succeed.
    client.attachTestArrays(inputRegs, holdingRegs, 40 + 13);
    client.readChannelConfig(0, 0x000F, cfg);

    // Protection block's read failed this time — a merge-on-success read
    // must retain its previous good value instead of resetting to the
    // zero default a fresh struct would have.
    CHECK(cfg.iLimitThresholdRaw == 32767);
    // Recovery block succeeded both times — still reflects the device.
    CHECK(cfg.currentSafeBandPct == 10);
}

TEST_CASE("readChannelConfig per-block methods combined match the monolithic read", "[channel-reads][merge]") {
    uint16_t inputRegs[280] = {};
    uint16_t holdingRegs[280] = {};
    fillChannelDefaults(inputRegs, holdingRegs);

    psb::PsbModbusClient client;
    client.attachTestArrays(inputRegs, holdingRegs, 280);

    psb::ChannelConfig viaMonolithic;
    client.readChannelConfig(0, 0x000F, viaMonolithic);

    psb::ChannelConfig viaBlocks;
    client.readChannelOutputBlock(0, 0x000F, viaBlocks);
    client.readChannelRecoveryBlock(0, viaBlocks);
    client.readChannelProtectionBlock(0, 0x000F, viaBlocks);
    client.readChannelDerateBlock(0, 0x000F, viaBlocks);
    client.readChannelOutputEnabledBlock(0, 0x000F, viaBlocks);

    CHECK(viaBlocks.outputAction == viaMonolithic.outputAction);
    CHECK(viaBlocks.currentSafeBandPct == viaMonolithic.currentSafeBandPct);
    CHECK(viaBlocks.iLimitThresholdRaw == viaMonolithic.iLimitThresholdRaw);
    CHECK(viaBlocks.recoveryPolicyMode == viaMonolithic.recoveryPolicyMode);
}

TEST_CASE("readChannelCalConfig merge-on-success — failed exponent read preserves prior value", "[channel-reads][merge]") {
    uint16_t inputRegs[280] = {};
    uint16_t holdingRegs[280] = {};
    fillChannelDefaults(inputRegs, holdingRegs);
    holdingRegs[40 + 26] = 3;  // Out cal K exponent (CH_OUTPUT_CAL_K_EXP)
    holdingRegs[40 + 27] = 4;  // Meas V cal K exponent (CH_MEASURED_V_CAL_K_EXP)

    psb::PsbModbusClient client;
    client.attachTestArrays(inputRegs, holdingRegs, 280);

    // Drive + Volt only (no Current) — takes the per-axis path rather than
    // the combined-9-register fast path, so each axis's K/B pair and
    // exponent are independent Modbus transactions.
    const uint16_t caps = 0x0002 /* RAW_OUTPUT_DRIVE */ | 0x0004 /* VOLTAGE_MEASUREMENT */;

    psb::ChannelCalConfig cal;
    client.readChannelCalConfig(0, caps, cal);
    REQUIRE(cal.outCalK == 10000);
    REQUIRE(cal.outCalKExp == 3);

    // Shrink the window so the output axis's K/B pair (offsets 20-21)
    // still succeeds but its exponent (offset 26) now fails.
    client.attachTestArrays(inputRegs, holdingRegs, 40 + 22);
    client.readChannelCalConfig(0, caps, cal);

    CHECK(cal.outCalK == 10000);  // re-read succeeded, unchanged
    CHECK(cal.outCalKExp == 3);   // exponent read failed — retains prior value
}
