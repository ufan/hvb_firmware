#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "register_map.h"

namespace hvb::meta {

struct RegDesc {
    uint16_t address = 0;
    const char* name = nullptr;
    const char* type = nullptr;      // "uint16"
    const char* unit = nullptr;      // "raw", "lsb", "seconds", "seconds_x10", "x10000", "x1000000", "x1000", "x100mV", "x0.1nA", "%", "enum", "bitmask", "bool", "count", ""
    const char* desc = nullptr;
    double scaleTo = 1.0;            // divisor for display
    bool writable = false;
    bool selfClearing = false;
    int channelIndex = -1;           // -1 = system, 0+ = channel
    std::vector<const char*> enumLabels;
    hvb::reg::PollCat pollCat = hvb::reg::PollCat::Fixed;
};

extern const std::vector<RegDesc> SYSTEM_INPUT;
extern const std::vector<RegDesc> SYSTEM_HOLDING;
extern const std::vector<RegDesc> CHANNEL_INPUT;
extern const std::vector<RegDesc> CHANNEL_HOLDING;

const RegDesc* findDesc(uint16_t absAddr, bool holding);
std::string formatValue(uint16_t raw, const RegDesc& d);
uint16_t parseEnum(const std::string& s, const RegDesc& d);
std::string formatRegisterCatalog();

} // namespace hvb::meta
