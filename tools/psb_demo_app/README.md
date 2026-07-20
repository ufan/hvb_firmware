# PSB Demo App

Modbus RTU engineering/demo tools for Jianwei voltage-control boards:
a scriptable CLI, an interactive TUI dashboard, and an optional Qt GUI, all
built on the shared `psb_modbus_core` client library. These are bring-up and
demo tools — for factory calibration, see `tools/factory/07_instrumental_cal/psb_factory_tool` and
[`docs/guide/calibration-guide.md`](../../docs/guide/calibration-guide.md).

## Build

From the `tools/` directory (not this subdirectory — `psb_demo_app` is one
of several targets under the shared `tools/` CMake project):

```sh
cmake -S tools -B tools/build
cmake --build tools/build --target psb_demo_cli psb_demo_tui -j
```

Binaries land in `tools/bin/`. The GUI is opt-in and requires Qt 6:

```sh
cmake -S tools -B tools/build -DBUILD_GUI=ON \
    -DCMAKE_PREFIX_PATH=/path/to/Qt/6.x/gcc_64
cmake --build tools/build --target psb_demo_gui -j
```

| Target | Binary | Requires |
|---|---|---|
| `psb_demo_cli` | `tools/bin/psb_demo_cli` | — |
| `psb_demo_tui` | `tools/bin/psb_demo_tui` | — |
| `psb_demo_gui` | `tools/bin/psb_demo_gui` | Qt 6 (`-DBUILD_GUI=ON`) |

## psb_demo_tui — interactive dashboard

Live monitoring and control over a connected board: per-channel target
voltage (DAC-capable channels) or on/off state (fixed-voltage channels),
ramp, current protection, and recovery policy, plus system-level config.
Full walkthrough (screen layout, every panel, a typical workflow):
[`docs/guide/demo-tui-guide.md`](../../docs/guide/demo-tui-guide.md) /
[`.zh.md`](../../docs/guide/demo-tui-guide.zh.md).

```sh
tools/bin/psb_demo_tui -p /dev/ttyUSB0
```

## psb_demo_cli — scriptable one-shot commands

```sh
# Discovery
psb_demo_cli list ports

# Connect + read
psb_demo_cli -p /dev/ttyUSB0 info
psb_demo_cli -p /dev/ttyUSB0 status
psb_demo_cli -p /dev/ttyUSB0 monitor [interval_s]

# Persist the connection for later commands
psb_demo_cli -p /dev/ttyUSB0 --save info
psb_demo_cli info                       # reuses ~/.psb_demo_app.toml

# System
psb_demo_cli system config
psb_demo_cli system mode <NORMAL|AUTO>
psb_demo_cli system startup-policy <0|1>        # 0=load-nvs, 1=factory-default
psb_demo_cli system addr <0-247>
psb_demo_cli system baud <0|1>                  # 0=115200, 1=9600
psb_demo_cli system save|load|factory

# Channel (range depends on the connected board — see `system config`/`info`
# for SUPPORTED_CHANNELS; 2 on jw_hvb, 10 on jw_lvb)
psb_demo_cli channel <ch> info
psb_demo_cli channel <ch> config
psb_demo_cli channel <ch> cal
psb_demo_cli channel <ch> voltage <raw-lsb>              # DAC channels only (CH_CAP_RAW_OUTPUT_DRIVE)
psb_demo_cli channel <ch> enable-cfg <0|1>                # fixed-voltage channels only (CH_CAP_OUTPUT_ENABLE, no DAC)
psb_demo_cli channel <ch> output <NONE|ENABLE|DISABLE-GRACEFUL|DISABLE-IMMEDIATE>
psb_demo_cli channel <ch> fault <CLEAR-ACTIVE|CLEAR-HISTORY>
psb_demo_cli channel <ch> ramp-up <step-raw> <interval-x10s>
psb_demo_cli channel <ch> ramp-down <step-raw> <interval-x10s>
psb_demo_cli channel <ch> prot-i <DISABLED|FLAG-ONLY|APPLY-ACTION> <NONE|DISABLE-GRACEFUL|DISABLE-IMMEDIATE|FORCE-ZERO> <threshold-raw>
psb_demo_cli channel <ch> recovery <MANUAL-LATCH|AUTO-RETRY|AUTO-DERATE|NEVER-RETRY> <delay-s> <max> <window-s>
psb_demo_cli channel <ch> safe-band <0-50>
psb_demo_cli channel <ch> derate <step-raw>
psb_demo_cli channel <ch> save|load|factory

# Calibration session (started/unlocked via psb_factory_tui; this only exits it)
psb_demo_cli cal exit

# Raw Modbus debug
psb_demo_cli raw fc04 <addr> [count]    # input registers, hex dump
psb_demo_cli raw fc03 <addr> [count]    # holding registers, hex dump
psb_demo_cli raw fc06 <addr> <value>    # single-register write

# Reset
psb_demo_cli reset
```

`voltage`, `ramp-up`/`ramp-down`, and `prot-i`'s threshold all take **raw
register LSB values**, not physical units — the CLI does not do V/nA
conversion for writes (only for display in `info`/`status`/`config`). See
[`docs/guide/parameter-reference.md`](../../docs/guide/parameter-reference.md)
for the raw-to-physical formulas (0.1 V/LSB, 0.1 nA/LSB).

Connection settings (`-p`/`-b`/`-i`/`-t`) persist to `~/.psb_demo_app.toml`
with `--save`; later invocations without `-p` reuse them. `psb_demo_tui` reads
the same file at startup but never writes it.

## Related documentation

- [`docs/guide/demo-tui-guide.md`](../../docs/guide/demo-tui-guide.md) — `psb_demo_tui` full user guide (EN/ZH)
- [`docs/guide/client-architecture-and-pitfalls.md`](../../docs/guide/client-architecture-and-pitfalls.md) — threading/polling architecture and every non-obvious bug found hardening `psb_demo_tui` against real hardware; read this before touching the GUI, the TUI, or writing your own client
- [`docs/guide/calibration-guide.md`](../../docs/guide/calibration-guide.md) — factory calibration procedure (`psb_factory_tui`)
- [`docs/guide/operating-mode-guide.md`](../../docs/guide/operating-mode-guide.md) — Normal vs. Automatic mode, protection and recovery policy
- [`docs/guide/modbus-reference.md`](../../docs/guide/modbus-reference.md) — protocol register map
- [`docs/guide/parameter-reference.md`](../../docs/guide/parameter-reference.md) — field-by-field defaults, units, raw/physical scaling

## File structure

```
tools/psb_demo_app/
├── cli/            psb_demo_cli — CLI11 subcommands over PsbModbusClient
├── tui/             psb_demo_tui — FTXUI interactive dashboard
└── gui/             psb_demo_gui — Qt Quick GUI (BUILD_GUI=ON only)
```

`psb_modbus_core` (the shared Modbus client, register map, and config
manager) lives at `tools/psb_modbus_core`, one level up — it's a sibling
target shared with `psb_factory_tool`, not part of this directory.
