# Settings Persistence Design

## Problem

Channel configs and system configs are lost on power cycle. The domain already exposes `domain_system_param_action()` and `domain_channel_param_action()` with SAVE/LOAD/FACTORY_RESET/SOFTWARE_RESET actions, but they return `VC_ERR_STORAGE` (unimplemented).

## Decisions

- **Architecture:** separate `vc_storage_backend` interface. Domain calls the backend through function pointers — no direct Zephyr settings dependency in the domain layer. Real backend uses Zephyr Settings/NVS. Tests can pass a fake or NULL.
- **Auto-load at startup:** runtime init loads saved settings into the domain before accepting commands. If no saved data exists, factory defaults are kept.
- **Calibration is separate:** calibration fields (3 k/b pairs per channel) are persisted independently from operational channel config. FACTORY_RESET resets operational config but preserves calibration. Calibration save is triggered by `domain_calibration_commit()`, not by param_action commands.
- **SOFTWARE_RESET:** triggers `sys_reboot(SYS_REBOOT_COLD)`.
- **Storage backend:** Zephyr Settings subsystem backed by NVS on internal flash.

## Storage Interface

```c
struct vc_storage_backend {
    int (*save_system_config)(const struct vc_system_config *cfg);
    int (*load_system_config)(struct vc_system_config *cfg);
    int (*save_channel_config)(uint8_t ch, const struct vc_channel_config *cfg);
    int (*load_channel_config)(uint8_t ch, struct vc_channel_config *cfg);
    int (*save_channel_cal)(uint8_t ch, const struct vc_channel_config *cfg);
    int (*load_channel_cal)(uint8_t ch, struct vc_channel_config *cfg);
    int (*erase_all)(void);
};
```

- `save/load_channel_config`: reads/writes the 13 non-calibration fields of `vc_channel_config`.
- `save/load_channel_cal`: reads/writes only the 6 calibration fields (output_calib_k/b, measured_voltage_calib_k/b, measured_current_calib_k/b).
- `erase_all`: wipes all stored settings keys. Used by factory reset.
- Returns 0 on success, negative errno on failure.
- Domain holds `const struct vc_storage_backend *storage`. Injected via `domain_set_storage_backend()` after `domain_init()`. NULL means storage unavailable (returns `VC_ERR_STORAGE`).

## Settings Key Layout

Keys are constructed dynamically using the DTS-derived channel index. No hardcoded channel paths.

| Key pattern | Data | Size |
| --- | --- | --- |
| `vc/sys` | all 9 `vc_system_config` fields | ~28 bytes |
| `vc/ch<N>/cfg` | 13 non-calibration `vc_channel_config` fields | ~26 bytes |
| `vc/ch<N>/cal` | 6 calibration fields | ~12 bytes |

Where `<N>` is `0` to `VC_MAX_CHANNELS - 1`, derived from DTS at build time.

Keys are built at runtime:
```c
char key[16];
snprintk(key, sizeof(key), "vc/ch%u/cfg", channel);
settings_save_one(key, data, len);
```

## Action Behavior

### System param_action

| Action | Behavior |
| --- | --- |
| SAVE | Read `domain->sys_cfg`, write to `vc/sys` via backend |
| LOAD | Read `vc/sys` from backend, apply via `domain_set_system_config()` |
| FACTORY_RESET | Erase `vc/sys`, reinit domain system config with defaults |
| SOFTWARE_RESET | `sys_reboot(SYS_REBOOT_COLD)` |

### Channel param_action

| Action | Behavior |
| --- | --- |
| SAVE | Read `domain->channels[ch]`, write non-cal fields to `vc/ch<N>/cfg` via backend |
| LOAD | Read `vc/ch<N>/cfg` from backend, merge into domain channel config (preserving cal fields) |
| FACTORY_RESET | Erase `vc/ch<N>/cfg`, reinit channel non-cal fields with defaults. Calibration preserved. |
| SOFTWARE_RESET | `sys_reboot(SYS_REBOOT_COLD)` |

### Calibration persistence

Calibration save/load is triggered by `domain_calibration_commit()`, not by param_action commands. On commit, the backend saves the 6 calibration fields to `vc/ch<N>/cal`. At startup, calibration is loaded alongside channel config.

## Runtime Data Flow

The domain struct is the single runtime copy:
- `domain->sys_cfg` — system config
- `domain->channels[N]` — per-channel config (including cal fields)

| Operation | From | To |
| --- | --- | --- |
| Boot (auto-load) | NVS | `domain->sys_cfg`, `domain->channels[]` |
| Modbus write | command | domain RAM only (not persisted) |
| SAVE | domain RAM | NVS |
| LOAD | NVS | domain RAM (discards unsaved changes) |
| FACTORY_RESET | erase NVS, reinit defaults | both RAM and NVS reset |

No shadow buffer. The storage backend keeps no RAM copy.

## Startup Sequence

1. `domain_init()` sets factory defaults (existing behavior, unchanged).
2. Runtime init calls `storage->load_system_config()` → applies via `domain_set_system_config()`.
3. For each channel (0 to `VC_MAX_CHANNELS - 1`):
   - `storage->load_channel_config()` → merge non-cal fields into domain channel config.
   - `storage->load_channel_cal()` → merge cal fields into domain channel config.
4. If any load returns error (no saved data, first boot), keep defaults — not a failure.

## Flash Partition

Add a storage partition to `jw_hvb.dts`. STM32F429 has 2 MB flash. Reserve the last 128 KB (two 64 KB sectors) for NVS:

```dts
&flash0 {
    partitions {
        compatible = "fixed-partitions";
        #address-cells = <1>;
        #size-cells = <1>;

        storage_partition: partition@1e0000 {
            label = "storage";
            reg = <0x1e0000 0x20000>;
        };
    };
};
```

## Kconfig

```kconfig
config VC_SETTINGS_PERSISTENCE
    bool "VC settings persistence via NVS"
    depends on VC_RUNTIME
    select SETTINGS
    select NVS
    select FLASH
    select FLASH_MAP
    help
      Enable save/load/factory-reset of system and channel configs
      using Zephyr Settings backed by NVS on internal flash.
```

## Files

| File | Change |
| --- | --- |
| `include/voltage_control/vc_storage.h` | New: `vc_storage_backend` struct and API |
| `lib/voltage_control/vc_storage_settings.c` | New: Zephyr Settings/NVS backend implementation |
| `lib/voltage_control/domain_state.c` | Implement param_action functions using storage backend |
| `lib/voltage_control/domain_runtime.c` | Auto-load at startup, pass storage backend to domain |
| `lib/voltage_control/Kconfig` | Add `VC_SETTINGS_PERSISTENCE` |
| `lib/voltage_control/CMakeLists.txt` | Add `vc_storage_settings.c` conditionally |
| `boards/jianwei/jw_hvb/jw_hvb.dts` | Add flash storage partition |
| `applications/hvb_controller/prj.conf` | Enable `CONFIG_VC_SETTINGS_PERSISTENCE` |
| `tests/voltage_control/runtime/src/main.c` | Add tests with fake storage backend |

## Verification

1. All existing tests pass (domain, runtime, provider_bus, modbus_adapter) — storage backend is NULL in tests, param_action returns `VC_ERR_STORAGE` as before.
2. New runtime tests with a fake storage backend: save round-trip, load restores config, factory-reset returns defaults but preserves calibration, load with no saved data keeps defaults.
3. `jw_hvb` board build clean with `CONFIG_VC_SETTINGS_PERSISTENCE=y`.
