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
 * particular build has configured.  The firmware's reg_store maps these wire
 * addresses to a compact in-RAM layout; callers always use the wire addresses
 * defined here.
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
#define EXT_BLOCK_BASE    680  /* 40 + 16 * 40; fixed by protocol regardless of channel count */
#define EXT_BLOCK_SIZE    80

/* ------------------------------------------------------------------ */
/* System input block  (FC04, offsets 0..39)                           */
/* ------------------------------------------------------------------ */

#define SYS_PROTOCOL_MAJOR            0
#define SYS_PROTOCOL_MINOR            1
#define SYS_VARIANT_ID                2
#define SYS_CAPABILITY_FLAGS          3
#define SYS_SUPPORTED_CHANNELS        4
#define SYS_ACTIVE_CHANNEL_MASK       5
#define SYS_BOARD_TEMPERATURE         6
#define SYS_BOARD_HUMIDITY            7
#define SYS_UPTIME_HI                 8
#define SYS_UPTIME_LO                 9
#define SYS_FW_VERSION_HI             10
#define SYS_FW_VERSION_LO             11
#define SYS_ACTIVE_OPERATING_MODE     12
#define SYS_STATUS                    13
#define SYS_FAULT_CAUSE               14
/* 15..39 reserved */

/* ------------------------------------------------------------------ */
/* System holding block  (FC03 / FC06, offsets 0..39)                  */
/* ------------------------------------------------------------------ */

#define SYS_OPERATING_MODE            0
#define SYS_STARTUP_CHANNEL_POLICY    1   /* 0=load NVS op-config, 1=factory reset op-config */
#define SYS_SLAVE_ADDRESS             2
#define SYS_BAUD_RATE_CODE            3
/* 4..38 reserved */
#define SYS_PARAM_ACTION              39
#define SYS_PARAM_ACTION_SOFTWARE_RESET 255U

/* ------------------------------------------------------------------ */
/* Channel input block  (FC04, per-channel offsets 0..39)              */
/* ------------------------------------------------------------------ */

#define CH_STATUS_BITS                0
#define CH_ACTIVE_FAULT_CAUSE         1
#define CH_FAULT_HISTORY_CAUSE        2
#define CH_LAST_PROT_OUT_ACTION       3
#define CH_AUTO_RETRY_COUNT           4
#define CH_AUTO_COOLDOWN_REMAINING    5
#define CH_LAST_FAULT_TIMESTAMP_HI    6
#define CH_LAST_FAULT_TIMESTAMP_LO    7
#define CH_OPER_TARGET_VOLTAGE        8
#define CH_CAPABILITY_FLAGS           9
#define CH_MEASURED_VOLTAGE           10
#define CH_MEASURED_CURRENT           11
#define CH_RAW_ADC_VOLTAGE_HI         12
#define CH_RAW_ADC_VOLTAGE_LO         13
#define CH_RAW_ADC_CURRENT_HI         14
#define CH_RAW_ADC_CURRENT_LO         15
/* 16..39 reserved */

/* ------------------------------------------------------------------ */
/* Channel holding block  (FC03 / FC06, per-channel offsets 0..39)     */
/* ------------------------------------------------------------------ */

/* Commands */
#define CH_OUTPUT_ACTION              0
#define CH_FAULT_CMD                  1
#define CH_PARAM_ACTION               2

/* Operational config */
#define CH_CFG_TARGET_VOLTAGE         3
#define CH_RAMP_UP_STEP               4
#define CH_RAMP_UP_INTERVAL           5
#define CH_RAMP_DOWN_STEP             6
#define CH_RAMP_DOWN_INTERVAL         7
#define CH_RECOVERY_POLICY_MODE       8
#define CH_AUTO_RETRY_DELAY           9
#define CH_AUTO_RETRY_MAX_COUNT       10
#define CH_AUTO_RETRY_WINDOW          11
#define CH_CURRENT_SAFE_BAND_PCT      12
#define CH_CURRENT_PROTECTION_MODE    13
#define CH_CURRENT_PROT_OUT_ACTION    14
#define CH_CURRENT_LIMIT_THRESHOLD    15
#define CH_AUTO_DERATE_STEP           16
/* 17..19 reserved */

/* Cal config — readable in any mode, writable in cal mode only */
#define CH_OUTPUT_CAL_K               20
#define CH_OUTPUT_CAL_B               21
#define CH_MEASURED_V_CAL_K           22
#define CH_MEASURED_V_CAL_B           23
#define CH_MEASURED_I_CAL_K           24
#define CH_MEASURED_I_CAL_B           25
/* 26..29 reserved */

/* Cal session commands — cal mode only */
#define CH_CAL_OUTPUT_ENABLE          30
#define CH_CAL_DAC_CODE               31
#define CH_CAL_SAMPLE_CMD             32
#define CH_CAL_COMMIT_CMD             33
#define CH_CAL_MAX_RAW_DAC_LIMIT      34
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
