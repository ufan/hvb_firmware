#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "register_map.h"

namespace psb::meta {

struct RegDesc {
    uint16_t address = 0;
    const char* name = nullptr;
    const char* type = nullptr;      // "uint16"
    const char* unit = nullptr;      // physical unit AFTER dividing by scaleTo: "V", "nA", "s",
                                      // "x" (gain multiplier), "%", "dac", "lsb", "raw", "enum",
                                      // "bitmask", "bool", "count", ""
    const char* desc = nullptr;
    double scaleTo = 1.0;            // divisor for display: formatValue() prints raw/scaleTo `unit`.
                                      // 1.0 means raw has no fixed physical unit (calibration-
                                      // dependent raw ADC counts, or the value is inherently a
                                      // raw integer: enum/bitmask/bool/count/dac counts/etc).
    bool writable = false;
    bool selfClearing = false;
    int channelIndex = -1;           // -1 = system, 0+ = channel
    std::vector<const char*> enumLabels;
    psb::reg::PollCat pollCat = psb::reg::PollCat::Fixed;
};

extern const std::vector<RegDesc> SYSTEM_INPUT;
extern const std::vector<RegDesc> SYSTEM_HOLDING;
extern const std::vector<RegDesc> CHANNEL_INPUT;
extern const std::vector<RegDesc> CHANNEL_HOLDING;

const RegDesc* findDesc(uint16_t absAddr, bool holding);
std::string formatValue(uint16_t raw, const RegDesc& d);
uint16_t parseEnum(const std::string& s, const RegDesc& d);
std::string formatRegisterCatalog();

} // namespace psb::meta
