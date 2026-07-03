# HVB Demo App

Modbus RTU engineering/demo tools for the Jianwei voltage-control board:
a scriptable CLI, an interactive TUI dashboard, and an optional Qt GUI, all
built on the shared `hvb_modbus_core` client library. These are bring-up and
demo tools — for factory calibration, see `tools/hvb_factory_tool` and
[`docs/guide/calibration-guide.md`](../../docs/guide/calibration-guide.md).

## Build

From the `tools/` directory (not this subdirectory — `hvb_demo_app` is one
of several targets under the shared `tools/` CMake project):

```sh
cmake -S tools -B tools/build
cmake --build tools/build --target hvb_demo_cli hvb_tui -j
```

Binaries land in `tools/bin/`. The GUI is opt-in and requires Qt 6:

```sh
cmake -S tools -B tools/build -DBUILD_GUI=ON \
    -DCMAKE_PREFIX_PATH=/path/to/Qt/6.x/gcc_64
cmake --build tools/build --target hvb_demo_gui -j
```

| Target | Binary | Requires |
|---|---|---|
| `hvb_demo_cli` | `tools/bin/hvb_demo_cli` | — |
| `hvb_tui` | `tools/bin/hvb_tui` | — |
| `hvb_demo_gui` | `tools/bin/hvb_demo_gui` | Qt 6 (`-DBUILD_GUI=ON`) |

## hvb_tui — interactive dashboard

Live monitoring and control over a connected board: per-channel target
voltage, ramp, current protection, and recovery policy, plus system-level
config. Full walkthrough (screen layout, every panel, a typical workflow):
[`docs/guide/demo-tui-guide.md`](../../docs/guide/demo-tui-guide.md) /
[`.zh.md`](../../docs/guide/demo-tui-guide.zh.md).

```sh
tools/bin/hvb_tui -p /dev/ttyUSB0
```

## hvb_demo_cli — scriptable one-shot commands

```sh
# Discovery
hvb_demo_cli list ports
hvb_demo_cli list regs
hvb_demo_cli describe <hex-addr>

# Connect + read
hvb_demo_cli -p /dev/ttyUSB0 info
hvb_demo_cli -p /dev/ttyUSB0 status
hvb_demo_cli -p /dev/ttyUSB0 monitor [interval_s]

# Persist the connection for later commands
hvb_demo_cli -p /dev/ttyUSB0 --save info
hvb_demo_cli info                       # reuses ~/.hvb_demo_app.toml

# System
hvb_demo_cli system config
hvb_demo_cli system mode <NORMAL|AUTO>
hvb_demo_cli system startup-policy <0|1>        # 0=load-nvs, 1=factory-default
hvb_demo_cli system addr <0-247>
hvb_demo_cli system baud <0|1>                  # 0=115200, 1=9600
hvb_demo_cli system save|load|factory

# Channel (0 or 1 on the HVB variant)
hvb_demo_cli channel <ch> info
hvb_demo_cli channel <ch> config
hvb_demo_cli channel <ch> cal
hvb_demo_cli channel <ch> voltage <raw-lsb>
hvb_demo_cli channel <ch> output <NONE|ENABLE|DISABLE-GRACEFUL|DISABLE-IMMEDIATE>
hvb_demo_cli channel <ch> fault <CLEAR-ACTIVE|CLEAR-HISTORY>
hvb_demo_cli channel <ch> ramp-up <step-raw> <interval-x10s>
hvb_demo_cli channel <ch> ramp-down <step-raw> <interval-x10s>
hvb_demo_cli channel <ch> prot-i <DISABLED|FLAG-ONLY|APPLY-ACTION> <NONE|DISABLE-GRACEFUL|DISABLE-IMMEDIATE|FORCE-ZERO> <threshold-raw>
hvb_demo_cli channel <ch> recovery <MANUAL-LATCH|AUTO-RETRY|AUTO-DERATE|NEVER-RETRY> <delay-s> <max> <window-s>
hvb_demo_cli channel <ch> safe-band <0-50>
hvb_demo_cli channel <ch> derate <step-raw>
hvb_demo_cli channel <ch> save|load|factory

# Calibration session (started/unlocked via hvb_factory_tui; this only exits it)
hvb_demo_cli cal exit

# Raw Modbus debug
hvb_demo_cli raw fc04 <addr> [count]    # input registers, hex dump
hvb_demo_cli raw fc03 <addr> [count]    # holding registers, hex dump
hvb_demo_cli raw fc06 <addr> <value>    # single-register write

# Reset
hvb_demo_cli reset
```

`voltage`, `ramp-up`/`ramp-down`, and `prot-i`'s threshold all take **raw
register LSB values**, not physical units — the CLI does not do V/nA
conversion for writes (only for display in `info`/`status`/`config`). See
[`docs/guide/parameter-reference.md`](../../docs/guide/parameter-reference.md)
for the raw-to-physical formulas (0.1 V/LSB, 0.1 nA/LSB).

Connection settings (`-p`/`-b`/`-i`/`-t`) persist to `~/.hvb_demo_app.toml`
with `--save`; later invocations without `-p` reuse them. `hvb_tui` reads
the same file at startup but never writes it.

## Related documentation

- [`docs/guide/demo-tui-guide.md`](../../docs/guide/demo-tui-guide.md) — `hvb_tui` full user guide (EN/ZH)
- [`docs/guide/calibration-guide.md`](../../docs/guide/calibration-guide.md) — factory calibration procedure (`hvb_factory_tui`)
- [`docs/guide/operating-mode-guide.md`](../../docs/guide/operating-mode-guide.md) — Normal vs. Automatic mode, protection and recovery policy
- [`docs/guide/modbus-reference.md`](../../docs/guide/modbus-reference.md) — protocol register map
- [`docs/guide/parameter-reference.md`](../../docs/guide/parameter-reference.md) — field-by-field defaults, units, raw/physical scaling

## File structure

```
tools/hvb_demo_app/
├── cli/            hvb_demo_cli — CLI11 subcommands over HvbModbusClient
├── tui/             hvb_tui — FTXUI interactive dashboard
└── gui/             hvb_demo_gui — Qt Quick GUI (BUILD_GUI=ON only)
```

`hvb_modbus_core` (the shared Modbus client, register map, and config
manager) lives at `tools/hvb_modbus_core`, one level up — it's a sibling
target shared with `hvb_factory_tool`, not part of this directory.
