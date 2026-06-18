#pragma once

#include <cstdint>

// Shared register map from firmware — single UINT16 registers, raw LSB values
#include "regmap/hvb_regs.h"

namespace hvb::reg {

inline constexpr uint16_t MAX_CHANNELS    = 4;

// channel absolute = SYS_BLOCK_BASE + offset, or CH_BLOCK_BASE(ch) + localOffset
inline constexpr uint16_t sysAddr(uint16_t off) {
    return SYS_BLOCK_BASE + off;
}
inline constexpr uint16_t chAddr(int ch, uint16_t off) {
    return static_cast<uint16_t>(CH_BLOCK_BASE(ch)) + off;
}

// Extension block address helper
inline constexpr uint16_t extAddr(uint16_t off) {
    return EXT_BLOCK_BASE + off;
}

// UINT32 packing for uptime/timestamps (HI at lower address, LO at higher)
inline uint32_t uint32FromRegs(uint16_t hi, uint16_t lo) {
    return (static_cast<uint32_t>(hi) << 16) | lo;
}

// INT32 signed packing for raw ADC values
inline int32_t int32FromRegs(uint16_t hi, uint16_t lo) {
    uint32_t u = (static_cast<uint32_t>(hi) << 16) | lo;
    return static_cast<int32_t>(u);
}

// UINT16 scaling model — variant profile provides compile-time scales (§5)
// HVB variant (id=1): voltage_scale = 100 mV/LSB, current_scale = 1 nA/LSB
//   raw 20000 = 2000 V, raw 5000 = 5 uA

namespace scale {
    inline constexpr double VOLTAGE_LSB_TO_V = 0.1;   // 100 mV/LSB → V
    inline constexpr double CURRENT_LSB_TO_A = 1e-9;  // 1 nA/LSB → A
}

// INT16 values — negative voltage/current are possible
inline double voltageToV(int16_t raw) {
    return static_cast<double>(raw) * scale::VOLTAGE_LSB_TO_V;
}
inline int16_t voltageFromV(double v) {
    return static_cast<int16_t>(v / scale::VOLTAGE_LSB_TO_V + 0.5);
}
inline double currentToA(int16_t raw) {
    return static_cast<double>(raw) * scale::CURRENT_LSB_TO_A;
}
inline int16_t currentFromA(double a) {
    return static_cast<int16_t>(a / scale::CURRENT_LSB_TO_A + 0.5);
}

// Time — single-register UINT16 seconds
inline double intervalToS(uint16_t raw) {
    return static_cast<double>(raw) * 0.1;  // seconds x10
}
inline uint16_t intervalFromS(double s) {
    return static_cast<uint16_t>(s * 10.0 + 0.5);
}

} // namespace hvb::reg
