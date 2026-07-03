# C API Reference — Voltage Control Domain Library

## Architecture

```
Host (Modbus / Shell / CAN)
        │
        ▼
  reg_read() / reg_write()          ← Register Catalog (public API)
        │
        ├── read: mutex-locked copy from vc_controller / vc_channel state
        └── write: builds vc_runtime_command → msgq → worker thread
                │
                ▼
  Runtime Worker Thread             ← vc_runtime.c (command queue + tick)
        │
        ▼
  vc_controller                     ← vc_controller.c (modes, channels, calibration)
        │
        ▼
  vc_channel[]                      ← vc_channel.c (SMF state machine per channel)
        │
        ▼
  vc_channel_api (vtable)           ← vc_channel_api.h (hardware abstraction)
        │
        ▼
  hvb_vc_channel / stub             ← driver implementations
```

**Data flow:**
- Writes: `reg_write(id, value, timeout)` → `vc_catalog_write()` → builds `vc_runtime_command` → `k_msgq_put` → worker thread → `vc_runtime_dispatch_command()` → controller → channel → API vtable → hardware. Caller blocks until processed.
- Reads: `reg_read(id, &value)` → `vc_catalog_read()` → `k_mutex_lock(&runtime->lock)` → copy from controller/channel state → non-blocking return.
- The register catalog (`lib/reg_store/reg_catalog.c`) maps protocol wire addresses to semantic register IDs and dispatches through owner vtables.

**Thread model:** Single worker thread owns all mutable state. Writes block until the worker thread processes the command. Reads copy state under a mutex — non-blocking.

## Facade API (`vc.h`)

The top-level VC API has three lifecycle functions. All domain interaction after init goes through the Register Catalog.

```c
#include "voltage_control/vc.h"
```

Required Kconfig: `CONFIG_VC_RUNTIME=y`

### Lifecycle

```c
#include "voltage_control/vc.h"

int main(void)
{
    /* 1. Init — builds singleton from DTS vc_controller node,
     *    auto-loads persisted config from NVS, starts worker thread. */
    struct vc_ctx *ctx = vc_init();
    if (!ctx) {
        return -1;
    }

    /* 2. Start hardware sampling (ADC loop on each channel) */
    vc_ctx_start(ctx);

    /* 3. Use register catalog for reads/writes */
    /* ... */

    /* 4. Optional: tear down */
    vc_destroy(ctx);
    return 0;
}
```

| Function | Description |
|----------|-------------|
| `struct vc_ctx *vc_init(void)` | Build singleton from DTS. Auto-loads persisted config from NVS. Returns NULL on failure. |
| `void vc_destroy(struct vc_ctx *ctx)` | Stop worker thread and release context. |
| `enum vc_status vc_ctx_start(struct vc_ctx *ctx)` | Start async hardware sampling on all channels. Call once after `vc_init()`. |

## Register Catalog API

The primary API for reading and writing domain state. Defined in `include/reg_store/reg_catalog.h`. All frontends (Modbus, shell, future CAN) use this.

### Core Functions

```c
#include "reg_store/reg_catalog.h"

/* Read a register by semantic ID. Non-blocking. */
enum reg_status reg_read(reg_id_t id, union reg_value *out);

/* Write a register by semantic ID. Blocks until worker thread processes. */
enum reg_status reg_write(reg_id_t id, union reg_value value,
                          k_timeout_t timeout);

/* Look up a register descriptor by semantic ID. */
const struct reg_descriptor *reg_describe(reg_id_t id);

/* Read/write via descriptor handle (avoids repeated lookups). */
enum reg_status reg_handle_read(reg_handle_t handle, union reg_value *out);
enum reg_status reg_handle_write(reg_handle_t handle, union reg_value value,
                                 k_timeout_t timeout);
```

### Semantic Register IDs

Register IDs encode module, channel instance, and field into a single `uint32_t`:

```c
#define REG_ID(module, instance, field) \
    ((((reg_id_t)(module) & 0xffU) << 24) |  \
     (((reg_id_t)(instance) & 0xffU) << 16) |  \
     ((reg_id_t)(field) & 0xffffU))

#define REG_ID_MODULE(id)   (((id) >> 24) & 0xffU)
#define REG_ID_INSTANCE(id) (((id) >> 16) & 0xffU)
#define REG_ID_FIELD(id)    ((id) & 0xffffU)
```

Convenience macros for VC domain (`include/reg_store/reg_map.h`):

```c
// Per-channel register:  REG_VC_ID(channel, REG_VC_FIELD_*)
// Global register:       REG_VC_GLOBAL_ID(REG_VC_GLOBAL_FIELD_*)
// Modbus adapter register: REG_MODBUS_ID(REG_MODBUS_FIELD_*)
```

### Register Value Union

```c
union reg_value {
    uint16_t u16;
    int16_t  s16;
    uint32_t u32;
    int32_t  s32;
    bool     boolean;
};
```

### Status Codes

```c
enum reg_status {
    REG_OK              =  0,
    REG_NOT_FOUND       = -1,  // No descriptor for this ID
    REG_INVALID_ARGUMENT = -2,
    REG_INVALID_VALUE   = -3,
    REG_READ_ONLY       = -4,
    REG_WRITE_ONLY      = -5,
    REG_UNSUPPORTED     = -6,  // Channel or capability not available
    REG_BUSY            = -7,
    REG_IO_ERROR        = -8,
};
```

### Example: Read Channel Voltage

```c
union reg_value val;
enum reg_status st = reg_read(REG_VC_ID(0, REG_VC_FIELD_MEASURED_VOLTAGE), &val);
if (st == REG_OK) {
    int16_t voltage_mV = val.s16;
}
```

### Example: Set Channel Target

```c
union reg_value val = { .u16 = 5000 };  // 5000 mV
enum reg_status st = reg_write(
    REG_VC_ID(0, REG_VC_FIELD_CFG_TARGET_VOLTAGE),
    val, K_SECONDS(1));
```

## Runtime Command API (Direct)

For internal use when the register catalog is not desired. Defined in `include/voltage_control/vc_runtime.h`.

```c
#include "voltage_control/vc_runtime.h"

/* Submit a raw command. Blocks until worker thread processes it. */
enum vc_status vc_runtime_submit_command(struct vc_runtime *runtime,
                                         const struct vc_runtime_command *cmd,
                                         k_timeout_t timeout);

/* Convenience wrappers: */
enum vc_status vc_runtime_set_operating_mode(struct vc_runtime *runtime,
                                             enum vc_operating_mode mode,
                                             k_timeout_t timeout);
enum vc_status vc_runtime_set_system_field(struct vc_runtime *runtime,
                                           enum vc_config_field field,
                                           uint16_t value, k_timeout_t timeout);
enum vc_status vc_runtime_set_channel_field(struct vc_runtime *runtime,
                                            uint8_t channel,
                                            enum vc_config_field field,
                                            uint16_t value, k_timeout_t timeout);
enum vc_status vc_runtime_set_channel_cal_field(struct vc_runtime *runtime,
                                                uint8_t channel,
                                                enum vc_cal_field field,
                                                uint16_t value, k_timeout_t timeout);
```

### Command Structure

```c
enum vc_runtime_command_type {
    VC_RUNTIME_CMD_SET_OPERATING_MODE = 0,
    VC_RUNTIME_CMD_OUTPUT_ACTION,
    VC_RUNTIME_CMD_FAULT_COMMAND,
    VC_RUNTIME_CMD_CALIBRATION_UNLOCK,
    VC_RUNTIME_CMD_CALIBRATION_OUTPUT_ENABLE,
    VC_RUNTIME_CMD_CALIBRATION_RAW_DAC,
    VC_RUNTIME_CMD_CALIBRATION_SAMPLE,
    VC_RUNTIME_CMD_CALIBRATION_COMMIT,
    VC_RUNTIME_CMD_CALIBRATION_MAX_RAW_DAC,
    VC_RUNTIME_CMD_CALIBRATION_EXIT,
    VC_RUNTIME_CMD_SYSTEM_PARAM_ACTION,
    VC_RUNTIME_CMD_CHANNEL_PARAM_ACTION,
    VC_RUNTIME_CMD_SET_SYSTEM_FIELD,
    VC_RUNTIME_CMD_SET_CHANNEL_FIELD,
    VC_RUNTIME_CMD_SET_CHANNEL_CAL_FIELD,
};

struct vc_runtime_command {
    enum vc_runtime_command_type type;
    uint8_t channel;
    union {
        enum vc_operating_mode        operating_mode;
        enum vc_output_action         output_action;
        enum vc_channel_fault_command fault_command;
        uint16_t                      calibration_unlock_value;
        bool                          calibration_output_enable;
        uint16_t                      calibration_raw_dac;
        uint16_t                      calibration_max_raw_dac;
        enum vc_param_action          param_action;
        struct vc_field_write         field_write;
        struct { enum vc_cal_field field; uint16_t value; } cal_field_write;
    } payload;
    struct k_sem *result_sem;     // caller-provided sem for blocking
    enum vc_status *result;       // output: command result
};
```

## Data Model

The firmware uses a layered data model with distinct representations for hardware raw values, calibrated product state, persisted settings, and protocol-visible registers.

### Layers (bottom-up)

```
Layer 4:  Register Store          reg_catalog.c       ISR-safe descriptor-based
        (semantic register                              register catalog with owner
         address space)                                 vtables for read/write dispatch

Layer 3:  Domain State            vc_controller.c     Operating mode, cal state,
        (authoritative runtime    vc_channel.c         calibrated V/I, targets,
         product policy)                                ramping, faults, SMF states

Layer 2:  Measurement Snapshot    vc_channel_buffer    Raw ADC voltage/current + timestamps,
        (hardware evidence)       (linker section)      populated by HW driver (async)
```

Note: The register catalog in the current codebase is a descriptor-based catalog with owner vtables (`lib/reg_store/reg_catalog.c`). Register values are read and written through `reg_read()`/`reg_write()` which dispatch to the owning module's vtable. There is no separate `_input[]`/`_holding[]` flat array layer; the catalog maps semantic IDs directly to domain state via owner callbacks.

### Layer 2: Measurement Snapshot

Defined by `struct vc_channel_buffer` in `include/voltage_control/vc_channel_api.h:29`:

```c
struct vc_channel_buffer {
    struct k_spinlock lock;
    uint8_t  channel_id;
    int32_t  raw_voltage;           /* raw ADC counts */
    uint32_t voltage_timestamp_ms;  /* k_uptime_get_32() when sampled */
    int32_t  raw_current;           /* raw ADC counts */
    uint32_t current_timestamp_ms;
};
```

- **Who writes:** Hardware driver (`hvb_vc_channel`) via async ADC sampling work loop, using `vc_channel_buffer_publish_voltage()` / `vc_channel_buffer_publish_current()` inline helpers.
- **Who reads:** The domain runtime (`vc_channel_run()`) via `vc_channel_buffer_read()`. Timestamp comparison detects fresh vs. stale data.
- **Memory:** Iterable linker section — one buffer per channel, statically allocated by DTS macros. Accessed via `VC_CHANNEL_BUFFER_EXTERN()` / `VC_CHANNEL_BUFFER_PTR()`.
- **Concurrency:** Spinlock-protected for ISR-safe producer access.

### Layer 3: Domain State

The authoritative mutable state owned by the runtime worker thread. Consists of:

| Component | Struct | Location | Contents |
|-----------|--------|----------|----------|
| System config | `vc_system_config` | `vc_controller.sys_cfg` | Operating mode, startup policy |
| System runtime | — | `vc_controller` | `cal_unlocked`, `cal_unlock_step`, `pre_cal_mode`, `cal_watchdog_ms` |
| Channel config | `vc_channel_config` | `vc_channel.config` | Target, ramp, recovery, protection, derate |
| Cal config | `vc_channel_cal_config` | `vc_channel.cal_config` | 6 calibration k/b coefficients |
| Channel runtime | — | `vc_channel` | `measured_voltage`, `measured_current`, `raw_adc_voltage`, `raw_adc_current`, `operational_target_voltage`, `raw_dac_readback`, `output_enabled`, `cal_output_enabled`, `active_fault_cause`, `fault_history_cause`, `status_bits`, SMF state, ramp accumulator, retry state |

**Lifecycle:**
- **System + channel config** loaded from NVS on boot (3-phase auto-load: sys config → channel configs → apply mode). Persisted on `VC_PARAM_ACTION_SAVE`.
- **Cal config** loaded from NVS per-channel on boot or load. Persisted on `VC_PARAM_ACTION_SAVE` or `vc cal commit`.
- **Channel runtime** derived from config + hardware input at every tick (`vc_channel_run()` in `vc_channel.c:335`):
  1. Consume fresh raw ADC data from measurement buffer (Layer 2) via timestamp comparison
  2. Apply calibration: `measured = raw × k/1000000 + b` (measurement axes; output axis uses ÷10000)
  3. Run current protection check (`tick_current_protection()` at `vc_channel.c:239`)
  4. Advance ramping state machine (`vc_channel_tick_ramp()` at `vc_channel.c:619`)
  5. Apply output to hardware via `apply_hw()` (`vc_channel.c:87`)
- In **calibration mode**, the tick skips normal processing. Raw DAC writes go direct to hardware through `vc_channel_cal_set_raw_dac()` → `apply_hw()`.

**Key distinction — Config vs. Runtime:**
- `configured_target_voltage` (config) — host-provided target, persisted.
- `operational_target_voltage` (runtime) — what the channel is *currently* driving toward. Can differ from config during ramping, auto-derate, or protection.

### Persistent Storage

Config and cal config persist to NVS flash via Zephyr Settings (`vc_storage_settings.c`):

| NVS Key | Contents | Loaded on | Saved on |
|---------|----------|-----------|----------|
| `vc/sys` | `vc_system_config` | Boot (phase 1) | `VC_PARAM_ACTION_SAVE` (sys) |
| `vc/chN/cfg` | `vc_channel_config` | Boot (phase 2) | `VC_PARAM_ACTION_SAVE` (ch) |
| `vc/chN/cal` | `vc_channel_cal_config` | Boot (phase 2), Load | `VC_PARAM_ACTION_SAVE` (ch), `vc cal commit` |
| `mb/cfg` | `mb_adapter_config` | Boot | `mb save` |

**Startup auto-load sequence:**
1. **Phase 1:** Load system config (for `startup_channel_policy`). Do not apply yet.
2. **Phase 2:** Load channel op-config + cal-config per startup policy (`0` = load config from NVS, `1` = factory reset op-config then load cal from NVS).
3. **Phase 3:** Apply system config — if mode is AUTO, non-faulted channels with non-zero targets are auto-enabled.

## VC Channel Hardware API (`vc_channel_api.h`)

The vtable that hardware channel drivers must implement. See `include/voltage_control/vc_channel_api.h:19`.

```c
typedef void (*vc_meas_ready_cb_t)(uint8_t channel, void *user_data);

struct vc_channel_api {
    int (*set_output)(const struct device *dev, uint16_t code);
    int (*set_enable)(const struct device *dev, bool enable);
    int (*start_sampling)(const struct device *dev);
    int (*stop_sampling)(const struct device *dev);
    uint16_t (*get_capabilities)(const struct device *dev);
    int (*set_meas_callback)(const struct device *dev,
                             vc_meas_ready_cb_t cb, void *user_data);
};
```

For a complete guide on writing a new hardware channel driver, see `docs/guide/vc-runtime-execution.md` Section 4.

## Snapshot API

Read-only snapshots of system and channel state are available through the controller.

```c
// System snapshot
void vc_controller_get_system_snapshot(const struct vc_controller *ctrl,
                                       struct vc_system_snapshot *snap);
// Per-channel snapshot
enum vc_status vc_controller_get_channel_snapshot(const struct vc_controller *ctrl,
                                                  uint8_t ch,
                                                  struct vc_channel_snapshot *snap);
// Channel config
enum vc_status vc_controller_get_channel_config(const struct vc_controller *ctrl,
                                                uint8_t ch,
                                                struct vc_channel_config *cfg);
// Channel cal config
enum vc_status vc_controller_get_channel_cal_config(const struct vc_controller *ctrl,
                                                    uint8_t ch,
                                                    struct vc_channel_cal_config *cal);
```

These are also accessible via `reg_read()` with the appropriate semantic register IDs, which is the preferred path for protocol frontends.

## Type Reference

### vc_status (error codes)

Defined in `include/voltage_control/vc_types.h:25`:

```c
enum vc_status {
    VC_OK                        =  0,
    VC_ERR_UNSUPPORTED_CHANNEL   = -1,
    VC_ERR_INVALID_VALUE         = -2,
    VC_ERR_INVALID_COMMAND       = -3,
    VC_ERR_UNSAFE_STATE          = -4,
    VC_ERR_STORAGE               = -5,
    VC_ERR_UNSUPPORTED_CAPABILITY = -6,
};
```

### vc_operating_mode

```c
enum vc_operating_mode {
    VC_OPERATING_MODE_NORMAL       = 0,
    VC_OPERATING_MODE_AUTOMATIC    = 1,
    VC_OPERATING_MODE_CALIBRATION  = 2,
};
```

### vc_output_action

```c
enum vc_output_action {
    VC_OUTPUT_ACTION_NONE              = 0,
    VC_OUTPUT_ACTION_ENABLE            = 1,
    VC_OUTPUT_ACTION_DISABLE_GRACEFUL  = 2,
    VC_OUTPUT_ACTION_DISABLE_IMMEDIATE = 3,
    VC_OUTPUT_ACTION_FORCE_OUTPUT_ZERO = 4,  /* protection only */
};
```

### vc_channel_fault_command

```c
enum vc_channel_fault_command {
    VC_CHANNEL_FAULT_COMMAND_NONE          = 0,
    VC_CHANNEL_FAULT_COMMAND_CLEAR_ACTIVE  = 1,
    VC_CHANNEL_FAULT_COMMAND_CLEAR_HISTORY = 2,
};
```

### vc_protection_mode

```c
enum vc_protection_mode {
    VC_PROTECTION_MODE_DISABLED             = 0,
    VC_PROTECTION_MODE_FLAG_ONLY            = 1,
    VC_PROTECTION_MODE_APPLY_OUTPUT_ACTION  = 2,
};
```

### vc_recovery_policy_mode

```c
enum vc_recovery_policy_mode {
    VC_RECOVERY_MANUAL_LATCH       = 0,
    VC_RECOVERY_AUTO_RETRY         = 1,
    VC_RECOVERY_AUTO_DERATE_RETRY  = 2,
    VC_RECOVERY_NEVER_RETRY        = 3,
};
```

### vc_param_action

```c
enum vc_param_action {
    VC_PARAM_ACTION_NONE          = 0,
    VC_PARAM_ACTION_SAVE          = 1,
    VC_PARAM_ACTION_LOAD          = 2,
    VC_PARAM_ACTION_FACTORY_RESET = 3,
};
```

### Fault Cause Bitmask

```c
#define VC_FAULT_CURRENT       0x0002
#define VC_FAULT_MEASUREMENT   0x0004
#define VC_FAULT_HARDWARE      0x0008
#define VC_FAULT_INTERLOCK     0x0010
#define VC_FAULT_RETRY_EXHAUST 0x0020
#define VC_FAULT_CFG_INVALID   0x0040
#define VC_FAULT_STALE         0x0080
```

### vc_config_field

```c
enum vc_config_field {
    /* System fields */
    VC_FIELD_OPERATING_MODE,
    VC_FIELD_STARTUP_CHANNEL_POLICY,

    /* Channel fields */
    VC_FIELD_CONFIGURED_TARGET_VOLTAGE,
    VC_FIELD_RAMP_UP_STEP,
    VC_FIELD_RAMP_UP_INTERVAL,
    VC_FIELD_RAMP_DOWN_STEP,
    VC_FIELD_RAMP_DOWN_INTERVAL,
    VC_FIELD_RECOVERY_POLICY_MODE,
    VC_FIELD_AUTO_RETRY_DELAY,
    VC_FIELD_AUTO_RETRY_MAX_COUNT,
    VC_FIELD_AUTO_RETRY_WINDOW,
    VC_FIELD_CURRENT_SAFE_BAND_PCT,
    VC_FIELD_CURRENT_PROTECTION_MODE,
    VC_FIELD_CURRENT_PROT_OUT_ACTION,
    VC_FIELD_CURRENT_LIMIT_THRESHOLD,
    VC_FIELD_AUTO_DERATE_STEP,
};
```

### vc_cal_field

```c
enum vc_cal_field {
    VC_CAL_FIELD_OUTPUT_K,
    VC_CAL_FIELD_OUTPUT_B,
    VC_CAL_FIELD_MEASURED_V_K,
    VC_CAL_FIELD_MEASURED_V_B,
    VC_CAL_FIELD_MEASURED_I_K,
    VC_CAL_FIELD_MEASURED_I_B,
};
```

### vc_system_snapshot

```c
struct vc_system_snapshot {
    uint16_t protocol_major;          /* Protocol major version */
    uint16_t protocol_minor;          /* Protocol minor version */
    uint16_t variant_id;              /* Board variant identifier */
    uint16_t system_capability_flags; /* SYS_CAP_* bitmask */
    uint16_t supported_channel_count; /* Number of channels */
    uint16_t active_channel_mask;     /* Present channel bitmask */
    enum vc_operating_mode active_operating_mode;
    uint16_t system_status;           /* System status bitmask */
    uint16_t system_fault_cause;      /* System fault bitmask */
};
```

### vc_channel_snapshot

```c
struct vc_channel_snapshot {
    int16_t measured_voltage;              /* Calibrated voltage (mV) */
    int16_t measured_current;              /* Calibrated current (raw ADC) */
    int16_t operational_target_voltage;    /* Current ramp target (mV) */
    uint16_t status_bits;                  /* Status bitmask */
    uint16_t active_fault_cause;           /* Active blocking faults */
    uint16_t fault_history_cause;          /* Recorded fault history */
    enum vc_output_action last_protection_output_action;
    uint16_t auto_retry_count;
    uint16_t auto_cooldown_remaining;      /* Seconds */
    uint32_t last_fault_timestamp;
    uint16_t channel_capability_flags;     /* CH_CAP_* bitmask */
    int32_t raw_adc_voltage;               /* Raw ADC voltage reading */
    int32_t raw_adc_current;               /* Raw ADC current reading */
    uint16_t raw_dac_readback;             /* Last DAC code written */
    uint16_t cal_output_enabled;           /* Calibration output active */
    uint16_t cal_max_raw_dac_limit;        /* Calibration DAC ceiling */
};
```

### vc_system_config

```c
struct vc_system_config {
    enum vc_operating_mode operating_mode;
    uint16_t startup_channel_policy;       /* 0=load, 1=factory reset */
};
```

### vc_channel_config

```c
struct vc_channel_config {
    int16_t configured_target_voltage;            /* mV */
    uint16_t ramp_up_step;                        /* mV per interval */
    uint16_t ramp_up_interval;                    /* ×100 ms */
    uint16_t ramp_down_step;                      /* mV per interval */
    uint16_t ramp_down_interval;                  /* ×100 ms */
    enum vc_recovery_policy_mode recovery_policy_mode;
    uint16_t auto_retry_delay;                    /* seconds */
    uint16_t auto_retry_max_count;
    uint16_t auto_retry_window;                   /* seconds */
    uint16_t current_safe_band_pct;               /* 0–100 */
    enum vc_protection_mode current_protection_mode;
    enum vc_output_action current_protection_output_action;
    int16_t current_limit_threshold;              /* raw ADC counts */
    uint16_t auto_derate_step;                    /* mV per derate */
};
```

### vc_channel_cal_config

```c
struct vc_channel_cal_config {
    uint16_t output_calib_k;              /* DAC gain (÷10000, default 10000) */
    int16_t  output_calib_b;              /* DAC offset (default 0) */
    uint16_t measured_voltage_calib_k;    /* Voltage meas gain (÷1000000) */
    int16_t  measured_voltage_calib_b;    /* Voltage meas offset */
    uint16_t measured_current_calib_k;    /* Current meas gain (÷1000000) */
    int16_t  measured_current_calib_b;    /* Current meas offset */
};
```

## Calibration Formula

```
calibrated = raw × k / D + b
```

Three independent axes, each with its own divisor `D`:
- **Output**: `raw_dac = target_voltage × output_calib_k / 10000 + output_calib_b`.
  Identity: `k = 10000`, `b = 0`.
- **Voltage measurement**: `measured_v = raw_adc_voltage × measured_voltage_calib_k / 1000000 + measured_voltage_calib_b`
- **Current measurement**: `measured_i = raw_adc_current × measured_current_calib_k / 1000000 + measured_current_calib_b`

The measurement axes use a finer ÷1000000 scale (vs. output's ÷10000) because
they convert a small, attenuated raw ADC gain (~0.001–0.01) rather than a
near-unity output gain; see `docs/guide/parameter-reference.md` for the
derivation. Unity gain is not representable on the measurement axes — the
maximum gain a `uint16_t` k can express there is 65535/1000000 ≈ 0.0655.

## Channel Capability Bitmask

Defined in `include/dt-bindings/voltage_control/capabilities.h`:

```c
#define CH_CAP_OUTPUT_ENABLE           0x0001  /* Has enable GPIO */
#define CH_CAP_RAW_OUTPUT_DRIVE        0x0002  /* Has DAC */
#define CH_CAP_VOLTAGE_MEASUREMENT     0x0004  /* Has ADC voltage channel */
#define CH_CAP_CURRENT_MEASUREMENT     0x0008  /* Has ADC current channel */
#define CH_CAP_HARDWARE_STATUS         0x0010  /* Has hardware status register */
```

## Modbus Adapter C API

```c
#include "modbus_adapter/modbus_adapter.h"

/* Init the Modbus RTU server (USART6) */
int modbus_adapter_init(struct vc_ctx *ctx);

/* Config accessors */
void modbus_adapter_get_config(struct mb_adapter_config *cfg);
int  modbus_adapter_set_slave_address(uint16_t addr);    /* 1–247 */
int  modbus_adapter_set_baud_rate_code(uint16_t code);   /* 0=115200, 1=9600 */

/* Persistence */
int  modbus_adapter_config_save(void);
int  modbus_adapter_config_load(void);
void modbus_adapter_config_factory(void);

/* Apply pending config changes (restart server) */
void modbus_adapter_apply_config(void);
```

```c
struct mb_adapter_config {
    uint16_t slave_address;
    uint16_t baud_rate_code;
};

enum vc_baud_rate_code {
    VC_BAUD_RATE_115200 = 0,
    VC_BAUD_RATE_9600   = 1,
};
```

## System Status C API

```c
#include "sys_status/sys_status.h"

struct sys_status_snapshot sys_status_get(void);
```

```c
struct sys_status_snapshot {
    int16_t board_temperature;     /* deci-°C */
    uint16_t board_humidity;       /* deci-%RH */
    uint32_t uptime;               /* seconds */
    uint16_t fw_version_high;
    uint16_t fw_version_low;
};
```

## Thread Safety

| Operation | Thread Model |
|-----------|-------------|
| `reg_read()` | Non-blocking. Copies from controller/channel state under `runtime->lock` mutex. |
| `reg_write()` | Blocks until worker thread processes. Internally: builds `vc_runtime_command` → `k_msgq_put` → `k_sem_take` on result semaphore. |
| `vc_runtime_submit_command()` | Same blocking pattern. Result semaphore and status pointer are caller-provided. |
| `vc_controller_start_sampling()` | Called once from main thread before worker starts. |
| `sys_status_get()` | Atomic reads of sensor values; `k_uptime_get()` is thread-safe. |
| `modbus_adapter_*` | Called from shell or Modbus ISR context. `modbus_disable`/`modbus_server_start` are Zephyr Modbus API calls. |

## Related Documentation

- **VC Runtime & Channel Execution**: `docs/guide/vc-runtime-execution.md` — worker thread, SMF state machine, mode logic, writing HW drivers
- **Shell Reference**: `docs/guide/shell-reference.md` — complete `vc`, `mb`, `ss` command tree
- **Modbus Register Map**: `docs/guide/modbus-reference.md` — protocol v3.0 register layout
- **Calibration Guide**: `docs/guide/calibration-guide.md` — factory calibration procedure
- **Demo TUI Guide**: `docs/guide/demo-tui-guide.md` — interactive monitoring/control dashboard user guide
- **Operating Mode Guide**: `docs/guide/operating-mode-guide.md` — Normal vs. Automatic mode, protection and recovery policy
