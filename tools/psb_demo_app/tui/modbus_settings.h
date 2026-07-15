// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Jianwei

#pragma once

#include "types.h"

#include <charconv>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace psb::tui {

enum class ModbusSettingsSaveResult {
    Success,
    InvalidSlave,
    InvalidBaud,
    SlaveWriteFailed,
    BaudWriteFailed,
};

inline std::string modbusSettingsStatusMessage(ModbusSettingsSaveResult result,
                                                std::string_view clientError) {
    switch (result) {
    case ModbusSettingsSaveResult::Success:
        return "OK: Modbus config saved — takes effect after reset";
    case ModbusSettingsSaveResult::InvalidSlave:
        return "Error: slave address must be 1-247";
    case ModbusSettingsSaveResult::InvalidBaud:
        return "Error: invalid baud rate";
    case ModbusSettingsSaveResult::SlaveWriteFailed:
    case ModbusSettingsSaveResult::BaudWriteFailed:
        return "Error: " + std::string(clientError);
    }
    return "Error: invalid Modbus save result";
}

inline bool parseModbusSlaveAddress(std::string_view text, uint16_t& address) {
    if (text.empty()) return false;

    uint16_t parsed = 0;
    const char* begin = text.data();
    const char* end = begin + text.size();
    auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end || parsed < 1 || parsed > 247) {
        return false;
    }

    address = parsed;
    return true;
}

inline ModbusSettingsSaveResult saveModbusSettings(
    std::string_view slaveText,
    int baudCode,
    const SystemConfig& current,
    const std::function<bool(uint16_t)>& writeSlave,
    const std::function<bool(uint16_t)>& writeBaud) {
    uint16_t slaveAddress = 0;
    if (!parseModbusSlaveAddress(slaveText, slaveAddress)) {
        return ModbusSettingsSaveResult::InvalidSlave;
    }
    if (baudCode < 0 || baudCode > 1) {
        return ModbusSettingsSaveResult::InvalidBaud;
    }

    if (slaveAddress != current.slaveAddr && !writeSlave(slaveAddress)) {
        return ModbusSettingsSaveResult::SlaveWriteFailed;
    }
    if (static_cast<uint16_t>(baudCode) != current.baudRateCode &&
        !writeBaud(static_cast<uint16_t>(baudCode))) {
        return ModbusSettingsSaveResult::BaudWriteFailed;
    }

    return ModbusSettingsSaveResult::Success;
}

} // namespace psb::tui
