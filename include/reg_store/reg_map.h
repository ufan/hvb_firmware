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
 */

#ifndef REG_STORE_REG_MAP_H
#define REG_STORE_REG_MAP_H

#include "dt-bindings/voltage_control/capabilities.h"

#define VC_PROTOCOL_MAJOR             3
#define VC_PROTOCOL_MINOR             0
#define VC_PROTOCOL_MAX_CHANNELS      16

/* System capability flags (SYS_CAPABILITY_FLAGS input register) */
#define SYS_CAP_AUTOMATIC_MODE         0x0001
#define SYS_CAP_ENV_SENSOR             0x0002
#define SYS_CAP_CALIBRATION_MODE       0x0004

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
#define SYS_REG16(name, field, type, access, category, bank, offset) \
	SYS_##name = offset,
#define SYS_REG32(name, field, type, access, category, bank, offset) \
	SYS_##name##_HI = offset, SYS_##name##_LO = (offset) + 1,
#include "reg_store/system_regs.def"
#undef SYS_REG16
#undef SYS_REG32
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
#define VC_REG16(name, field, type, access, category, bank, offset) \
	CH_##name = offset,
#define VC_REG32(name, field, type, access, category, bank, offset) \
	CH_##name##_HI = offset, CH_##name##_LO = (offset) + 1,
#include "reg_store/vc_regs.def"
#undef VC_REG16
#undef VC_REG32
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
