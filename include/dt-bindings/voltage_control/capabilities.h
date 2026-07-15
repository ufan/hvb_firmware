/* Copyright (c) 2026 Jianwei
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef DT_BINDINGS_VOLTAGE_CONTROL_CAPABILITIES_H
#define DT_BINDINGS_VOLTAGE_CONTROL_CAPABILITIES_H

/* Indicates that the channel can be enabled for output operation, i.e. on/off. */
#define CH_CAP_OUTPUT_ENABLE           0x0001
/* Indicates that the channel can drive a raw output signal, i.e. DAC output */
#define CH_CAP_RAW_OUTPUT_DRIVE        0x0002
/* Indicates that the channel supports voltage measurement, i.e. ADC input for voltage sensing. */
#define CH_CAP_VOLTAGE_MEASUREMENT     0x0004
/* Indicates that the channel supports current measurement, i.e. ADC input for current sensing. */
#define CH_CAP_CURRENT_MEASUREMENT     0x0008
/* Indicates that the channel exposes hardware status information. */
#define CH_CAP_HARDWARE_STATUS         0x0010

#endif /* DT_BINDINGS_VOLTAGE_CONTROL_CAPABILITIES_H */
