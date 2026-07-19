/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Protocol register address dictionary for Jianwei voltage-control boards.
 * No Zephyr or firmware dependencies — usable by host tools and firmware alike.
 *
 * This file defines the WIRE address space: the full 16-channel protocol layout
 * that every board variant and host tool agrees on regardless of what any
 * particular build has configured. Firmware adapters map these wire addresses
 * to protocol-neutral Semantic Register IDs; callers always use the wire
 * addresses defined here.
 *
 * Protocol v3 wire address layout:
 *   0      System block           input + holding, 40 registers
 *   40     Channel 0 block        input + holding, 40 registers
 *   80     Channel 1 block        input + holding, 40 registers
 *   ...
 *   640    Channel 15 block       input + holding, 40 registers
 *   680    Extension block        holding only, 80 registers
 *
 * Unused channel slots (beyond SYS_SUPPORTED_CHANNELS) return protocol
 * exceptions; module-absent registers read as zero and are identified via
 * SYS_CAPABILITY_FLAGS.
 *
 * Breaking changes from v2:
 *   - SYS_STARTUP_CHANNEL_POLICY added at holding offset 1
 *   - SYS_SLAVE_ADDRESS/BAUD_RATE_CODE shifted to 2/3
 *   - Recovery fields removed from system holding (moved to channel)
 *   - CH_SAVE_TARGET_POLICY removed
 *   - CH_CAL_SAMPLE_STATUS (FC04 in:16) and CH_RAW_DAC_READBACK (FC04 in:17) removed
 *   - Recovery fields added to channel holding at 8-12
 *   - Protection fields shifted to 13-16
 *   - Cal config (k/b) moved to channel holding 20-25
 *   - Cal session commands moved to channel holding 30-34
 *
 * Additive changes in v3.1 (non-breaking):
 *   - CH_OUTPUT_CAL_K_EXP / CH_MEASURED_V_CAL_K_EXP / CH_MEASURED_I_CAL_K_EXP
 *     added at channel holding 26-28 (previously reserved). Calibration gain
 *     is now k * 10^k_exp rather than a fixed-divisor k alone; k_exp defaults
 *     reproduce the old fixed divisors exactly, so existing k/b values are
 *     unaffected unless k_exp is deliberately changed.
 *
 * Additive changes in v3.2 (non-breaking):
 *   - SYS_CURRENT_UNIT_EXP added at system input offset 15 (previously
 *     reserved). Declares the decimal exponent for MEASURED_CURRENT /
 *     CURRENT_LIMIT_THRESHOLD: those registers are in units of
 *     10^CURRENT_UNIT_EXP amperes per LSB, board-specific (a single fixed
 *     unit can't span both nA-scale sense currents and amp-scale load
 *     currents in an int16). Defaults to -10 (0.1 nA/LSB), the value every
 *     board used before this register existed, so a client that only knows
 *     v3.1 and assumes 0.1 nA/LSB is unaffected unless it's actually talking
 *     to a variant that overrides it (e.g. jw_lvb, -1 = 0.1 A/LSB). Voltage
 *     has no equivalent register — every variant uses a fixed 0.1 V/LSB.
 *
 * Additive changes in v3.3 (non-breaking):
 *   - SYS_BOARD_HW_REVISION added at system input offset 16 (previously
 *     reserved). Reports the Zephyr board.yml hardware-revision index
 *     within this board variant (0 = default/revA), distinct from
 *     SYS_VARIANT_ID (board type). See docs/superpowers/specs/
 *     2026-07-19-version-management-contract-design.md sections 3-4.
 */

#ifndef REG_STORE_REG_MAP_H
#define REG_STORE_REG_MAP_H

#include "dt-bindings/voltage_control/capabilities.h"

#define VC_PROTOCOL_MAJOR             3
#define VC_PROTOCOL_MINOR             3
#define VC_PROTOCOL_MAX_CHANNELS      16

/* System capability flags (SYS_CAPABILITY_FLAGS input register) */
#define SYS_CAP_AUTOMATIC_MODE         0x0001
#define SYS_CAP_ENV_SENSOR             0x0002
#define SYS_CAP_CALIBRATION_MODE       0x0004

/* ------------------------------------------------------------------ */
/* Poll category tags  (4th field in modbus_view.def entries)          */
/* ------------------------------------------------------------------ */

/* REALTIME: live measurements, status, faults — poll fast (~100 ms)  */
#define VC_POLL_REALTIME  0
/* CONFIG: operational parameters — poll slow or on demand             */
#define VC_POLL_CONFIG    1
/* FIXED: version, variant, capabilities — read once at connect        */
#define VC_POLL_FIXED     2
/* COMMAND: write-only triggers — do not poll                          */
#define VC_POLL_COMMAND   3

/* ------------------------------------------------------------------ */
/* Block base addresses  (wire addresses, protocol-fixed)              */
/* ------------------------------------------------------------------ */

#define SYS_BLOCK_BASE    0
#define CH_BLOCK_BASE(c)  (40 + (c) * 40)
#define CH_BLOCK_SIZE     40
#define EXT_BLOCK_BASE    (40 + VC_PROTOCOL_MAX_CHANNELS * CH_BLOCK_SIZE)
#define EXT_BLOCK_SIZE    80

/* ------------------------------------------------------------------ */
/* System input block  (FC04, offsets 0..39)                           */
/* ------------------------------------------------------------------ */

enum {
#define MODBUS_SYS16(name, bank, offset, poll_cat) \
	SYS_##name = offset,
#define MODBUS_SYS32(name, bank, offset, poll_cat) \
	SYS_##name##_HI = offset, SYS_##name##_LO = (offset) + 1,
#define MODBUS_VC16(name, bank, offset, poll_cat)
#define MODBUS_VC32(name, bank, offset, poll_cat)
#include "reg_store/modbus_view.def"
#undef MODBUS_SYS16
#undef MODBUS_SYS32
#undef MODBUS_VC16
#undef MODBUS_VC32
};
/* 15..39 reserved */

/* ------------------------------------------------------------------ */
/* System holding block  (FC03 / FC06, offsets 0..39)                  */
/* ------------------------------------------------------------------ */

/* 4..38 reserved */
#define SYS_PARAM_ACTION_SOFTWARE_RESET 255U

/* ------------------------------------------------------------------ */
/* Channel input block  (FC04, per-channel offsets 0..39)              */
/* ------------------------------------------------------------------ */

enum {
#define MODBUS_SYS16(name, bank, offset, poll_cat)
#define MODBUS_SYS32(name, bank, offset, poll_cat)
#define MODBUS_VC16(name, bank, offset, poll_cat) \
	CH_##name = offset,
#define MODBUS_VC32(name, bank, offset, poll_cat) \
	CH_##name##_HI = offset, CH_##name##_LO = (offset) + 1,
#include "reg_store/modbus_view.def"
#undef MODBUS_SYS16
#undef MODBUS_SYS32
#undef MODBUS_VC16
#undef MODBUS_VC32
};
/* 16..39 reserved */

/* ------------------------------------------------------------------ */
/* Channel holding block  (FC03 / FC06, per-channel offsets 0..39)     */
/* ------------------------------------------------------------------ */

/* 17..19 reserved */
/* 26..29 reserved */
/* 35..39 reserved */

/* ------------------------------------------------------------------ */
/* Extension holding block  (FC03 / FC06, offsets 0..79)               */
/* ------------------------------------------------------------------ */

#define EXT_CAL_UNLOCK                0
#define EXT_CAL_UNLOCK_ABS            (EXT_BLOCK_BASE + EXT_CAL_UNLOCK)

#define EXT_CAL_EXIT                  1
#define EXT_CAL_EXIT_ABS              (EXT_BLOCK_BASE + EXT_CAL_EXIT)

#define CAL_UNLOCK_STEP1              0xCA1B
#define CAL_UNLOCK_STEP2              0xA11B
#define CAL_COMMAND_NONE              0
#define CAL_COMMAND_EXECUTE           1

#endif /* REG_STORE_REG_MAP_H */
