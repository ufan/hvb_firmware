#pragma once

#include <cstdint>

// Shared register map from firmware — single UINT16 registers, raw LSB values
#include "reg_store/reg_map.h"

namespace hvb::reg {

inline constexpr uint16_t MAX_CHANNELS = VC_PROTOCOL_MAX_CHANNELS;

// Poll category for each register — drives host-tool polling strategy.
enum class PollCat : uint8_t {
    Realtime = VC_POLL_REALTIME, // live measurements, status, faults — poll fast (~100 ms)
    Config   = VC_POLL_CONFIG,   // operational parameters — poll slow or on demand
    Fixed    = VC_POLL_FIXED,    // version, capabilities — read once at connect
    Command  = VC_POLL_COMMAND,  // write-only triggers — do not poll
};

// Per-register poll category constants: SYS_<NAME>_POLL and CH_<NAME>_POLL.
// Cast to PollCat for typed comparisons, e.g.:
//   static_cast<PollCat>(CH_MEASURED_VOLTAGE_POLL) == PollCat::Realtime
enum {
#define MODBUS_SYS16(name, bank, offset, poll_cat) SYS_##name##_POLL = VC_POLL_##poll_cat,
#define MODBUS_SYS32(name, bank, offset, poll_cat) SYS_##name##_POLL = VC_POLL_##poll_cat,
#define MODBUS_VC16(name, bank, offset, poll_cat)
#define MODBUS_VC32(name, bank, offset, poll_cat)
#include "reg_store/modbus_view.def"
#undef MODBUS_SYS16
#undef MODBUS_SYS32
#undef MODBUS_VC16
#undef MODBUS_VC32
};
enum {
#define MODBUS_SYS16(name, bank, offset, poll_cat)
#define MODBUS_SYS32(name, bank, offset, poll_cat)
#define MODBUS_VC16(name, bank, offset, poll_cat) CH_##name##_POLL = VC_POLL_##poll_cat,
#define MODBUS_VC32(name, bank, offset, poll_cat) CH_##name##_POLL = VC_POLL_##poll_cat,
#include "reg_store/modbus_view.def"
#undef MODBUS_SYS16
#undef MODBUS_SYS32
#undef MODBUS_VC16
#undef MODBUS_VC32
};

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
// HVB variant (id=1): voltage_scale = 100 mV/LSB, current_scale = 0.1 nA/LSB
//   raw 20000 = 2000 V, raw 5000 = 0.5 uA
// Both axes are calibration-dependent (measured_voltage_calib_k /
// measured_current_calib_k, see lib/voltage_control/vc_channel.c): these
// constants describe the scale a *calibrated* board is expected to produce,
// not a fixed firmware guarantee.

namespace scale {
    inline constexpr double VOLTAGE_LSB_TO_V = 0.1;    // 100 mV/LSB → V
    inline constexpr double CURRENT_LSB_TO_A = 1e-10;  // 0.1 nA/LSB → A
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
