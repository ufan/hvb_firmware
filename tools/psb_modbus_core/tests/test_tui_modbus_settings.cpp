// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Jianwei

#include <catch2/catch_test_macros.hpp>

#include "../../psb_demo_app/tui/modbus_settings.h"

#include <string>
#include <utility>
#include <vector>

using psb::tui::ModbusSettingsSaveResult;
using psb::tui::modbusSettingsStatusMessage;
using psb::tui::saveModbusSettings;

TEST_CASE("Modbus settings save result has a status-bar message") {
    CHECK(modbusSettingsStatusMessage(ModbusSettingsSaveResult::Success, "") ==
          "OK: Modbus config saved — takes effect after reset");
    CHECK(modbusSettingsStatusMessage(ModbusSettingsSaveResult::InvalidSlave, "") ==
          "Error: slave address must be 1-247");
    CHECK(modbusSettingsStatusMessage(ModbusSettingsSaveResult::InvalidBaud, "") ==
          "Error: invalid baud rate");
    CHECK(modbusSettingsStatusMessage(ModbusSettingsSaveResult::BaudWriteFailed,
                                      "device failure") ==
          "Error: device failure");
}

TEST_CASE("unchanged Modbus settings perform no writes") {
    psb::SystemConfig current{};
    current.slaveAddr = 1;
    current.baudRateCode = 0;
    int writes = 0;

    auto result = saveModbusSettings(
        "1", 0, current,
        [&](uint16_t) { ++writes; return true; },
        [&](uint16_t) { ++writes; return true; });

    CHECK(result == ModbusSettingsSaveResult::Success);
    CHECK(writes == 0);
}

TEST_CASE("changed Modbus fields are written in protocol order") {
    psb::SystemConfig current{};
    current.slaveAddr = 1;
    current.baudRateCode = 0;
    std::vector<std::pair<std::string, uint16_t>> calls;

    auto result = saveModbusSettings(
        "7", 1, current,
        [&](uint16_t value) { calls.emplace_back("slave", value); return true; },
        [&](uint16_t value) { calls.emplace_back("baud", value); return true; });

    CHECK(result == ModbusSettingsSaveResult::Success);
    CHECK(calls == std::vector<std::pair<std::string, uint16_t>>{
        {"slave", 7}, {"baud", 1}});
}

TEST_CASE("only changed Modbus settings are written") {
    psb::SystemConfig current{};
    current.slaveAddr = 3;
    current.baudRateCode = 1;
    std::vector<std::pair<std::string, uint16_t>> calls;

    SECTION("slave only") {
        auto result = saveModbusSettings(
            "4", 1, current,
            [&](uint16_t value) { calls.emplace_back("slave", value); return true; },
            [&](uint16_t value) { calls.emplace_back("baud", value); return true; });
        CHECK(result == ModbusSettingsSaveResult::Success);
        CHECK(calls == std::vector<std::pair<std::string, uint16_t>>{{"slave", 4}});
    }

    SECTION("baud only") {
        auto result = saveModbusSettings(
            "3", 0, current,
            [&](uint16_t value) { calls.emplace_back("slave", value); return true; },
            [&](uint16_t value) { calls.emplace_back("baud", value); return true; });
        CHECK(result == ModbusSettingsSaveResult::Success);
        CHECK(calls == std::vector<std::pair<std::string, uint16_t>>{{"baud", 0}});
    }
}

TEST_CASE("invalid Modbus settings perform no writes") {
    psb::SystemConfig current{};
    int writes = 0;
    auto write = [&](uint16_t) { ++writes; return true; };

    for (const char* value : {"", "abc", "1x", "0", "248", "-1"}) {
        CHECK(saveModbusSettings(value, 0, current, write, write) ==
              ModbusSettingsSaveResult::InvalidSlave);
    }
    CHECK(saveModbusSettings("1", 2, current, write, write) ==
          ModbusSettingsSaveResult::InvalidBaud);
    CHECK(writes == 0);
}

TEST_CASE("failed Modbus writes stop the save sequence") {
    psb::SystemConfig current{};
    current.slaveAddr = 1;
    current.baudRateCode = 0;
    int baudWrites = 0;

    CHECK(saveModbusSettings(
              "2", 1, current,
              [](uint16_t) { return false; },
              [&](uint16_t) { ++baudWrites; return true; }) ==
          ModbusSettingsSaveResult::SlaveWriteFailed);
    CHECK(baudWrites == 0);

    CHECK(saveModbusSettings(
              "1", 1, current,
              [](uint16_t) { return true; },
              [](uint16_t) { return false; }) ==
          ModbusSettingsSaveResult::BaudWriteFailed);
}
