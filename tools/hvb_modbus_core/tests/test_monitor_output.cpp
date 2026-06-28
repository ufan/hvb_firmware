#include "hvb_modbus_client.h"
#include "types.h"

#include <catch2/catch_test_macros.hpp>
#include <string>

extern std::string renderMonitorTable(const hvb::SystemInfo& sys,
                                      const std::vector<hvb::ChannelInfo>& channels);

TEST_CASE("Monitor — table includes protocol version and uptime", "[monitor]") {
    hvb::SystemInfo sys;
    sys.protoMajor = 3;
    sys.protoMinor = 0;
    sys.uptimeSec = 3600;
    sys.activeOpMode = hvb::OpMode::Normal;

    std::vector<hvb::ChannelInfo> channels(2);
    channels[0].voltageRaw = 5000;
    channels[0].status = 0x0003;

    auto output = renderMonitorTable(sys, channels);
    CHECK(output.find("3.0") != std::string::npos);
    CHECK(output.find("3600s") != std::string::npos);
}

TEST_CASE("Monitor — table includes channel data", "[monitor]") {
    hvb::SystemInfo sys;
    sys.protoMajor = 3;
    sys.uptimeSec = 100;
    sys.activeOpMode = hvb::OpMode::Normal;

    std::vector<hvb::ChannelInfo> channels(2);
    channels[0].voltageRaw = 5000;   // 500.0 V
    channels[0].currentRaw = 1234;   // 1.234 uA
    channels[0].operationalTargetVoltageRaw = 5000;
    channels[0].status = 0x0003;
    channels[0].activeFault = 0;

    channels[1].voltageRaw = 0;
    channels[1].status = 0;

    auto output = renderMonitorTable(sys, channels);
    CHECK(output.find("CH0") != std::string::npos);
    CHECK(output.find("CH1") != std::string::npos);
}

TEST_CASE("Monitor — shows fault status", "[monitor]") {
    hvb::SystemInfo sys;
    sys.protoMajor = 3;
    sys.uptimeSec = 60;
    sys.activeOpMode = hvb::OpMode::Automatic;

    std::vector<hvb::ChannelInfo> channels(1);
    channels[0].voltageRaw = 6000;
    channels[0].activeFault = hvb::FaultCause::CURRENT;
    channels[0].status = 0x000B; // OutputDrive + ActiveFault

    auto output = renderMonitorTable(sys, channels);
    CHECK(output.find("Fault") != std::string::npos);
    CHECK(output.find("Automatic") != std::string::npos);
}
