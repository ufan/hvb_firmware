/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared Modbus register offset definitions for Jianwei voltage-control
 * boards.  No Zephyr dependencies.  Usable by both firmware and host
 * application.
 *
 * Protocol major version 2, register-block layout:
 *   0      System block           input + holding, 40 registers
 *   40     Channel 0 block        input + holding, 40 registers
 *   80     Channel 1 block        input + holding, 40 registers
 *   120    Channel 2 block        reserved for future variants
 *   160    Channel 3 block        reserved for future variants
 *   200    Reserved extension     holding only, 80 registers
 *
 * All registers are 16-bit (UINT16 or INT16).  Voltage and current
 * values are in raw LSBs; the variant profile provides compile-time
 * scale factors (voltage_scale in mV/LSB, current_scale in uA/LSB).
 * 32-bit uptime and timestamps are split across two consecutive
 * 16-bit registers with HI/LO suffixes.
 */

#ifndef REGMAP_HVB_REGS_H
#define REGMAP_HVB_REGS_H

/* ------------------------------------------------------------------ */
/* Block base addresses                                                */
/* ------------------------------------------------------------------ */

#define SYS_BLOCK_BASE    0
#define CH_BLOCK_BASE(c)  (40 + (c) * 40)
#define CH_BLOCK_SIZE     40
#define EXT_BLOCK_BASE    200

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
#define SYS_SLAVE_ADDRESS             1
#define SYS_BAUD_RATE_CODE            2
#define SYS_RECOVERY_POLICY_MODE      3
#define SYS_AUTO_RETRY_DELAY          4
#define SYS_AUTO_RETRY_MAX_COUNT      5
#define SYS_AUTO_RETRY_WINDOW         6
#define SYS_VOLTAGE_SAFE_BAND_PCT     7
#define SYS_CURRENT_SAFE_BAND_PCT     8
/* 9..38 reserved */
#define SYS_PARAM_ACTION              39

/* ------------------------------------------------------------------ */
/* Channel input block  (FC04, per-channel offsets 0..39)              */
/* ------------------------------------------------------------------ */

#define CH_MEASURED_VOLTAGE           0
#define CH_MEASURED_CURRENT           1
#define CH_OPER_TARGET_VOLTAGE        2
#define CH_STATUS_BITS                3
#define CH_ACTIVE_FAULT_CAUSE         4
#define CH_FAULT_HISTORY_CAUSE        5
#define CH_LAST_PROT_OUT_ACTION       6
#define CH_AUTO_RETRY_COUNT           7
#define CH_AUTO_COOLDOWN_REMAINING    8
#define CH_LAST_FAULT_TIMESTAMP_HI    9
#define CH_LAST_FAULT_TIMESTAMP_LO    10
#define CH_CAPABILITY_FLAGS           11
/* 12..39 reserved */

/* ------------------------------------------------------------------ */
/* Channel holding block  (FC03 / FC06, per-channel offsets 0..39)     */
/* ------------------------------------------------------------------ */

#define CH_CFG_TARGET_VOLTAGE         0
#define CH_OUTPUT_ACTION              1
#define CH_FAULT_CMD                  2
#define CH_RAMP_UP_STEP               3
#define CH_RAMP_UP_INTERVAL           4
#define CH_RAMP_DOWN_STEP             5
#define CH_RAMP_DOWN_INTERVAL         6
#define CH_VOLTAGE_PROTECTION_MODE    7
#define CH_VOLTAGE_PROT_OUT_ACTION    8
#define CH_VOLTAGE_LIMIT_THRESHOLD    9
#define CH_CURRENT_PROTECTION_MODE    10
#define CH_CURRENT_PROT_OUT_ACTION    11
#define CH_CURRENT_LIMIT_THRESHOLD    12
#define CH_AUTO_DERATE_STEP           13
#define CH_SAVE_TARGET_POLICY         14
#define CH_OUTPUT_CAL_K              15
#define CH_OUTPUT_CAL_B              16
#define CH_MEASURED_V_CAL_K          17
#define CH_MEASURED_V_CAL_B          18
#define CH_MEASURED_I_CAL_K          19
#define CH_MEASURED_I_CAL_B          20
/* 21..38 reserved */
#define CH_PARAM_ACTION               39

#endif
