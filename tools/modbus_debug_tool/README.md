# HVB Modbus Debug Tool

Self-contained Modbus-RTU debug tool for the Jianwei High Voltage Board. Three layers: core library → CLI → GUI.

## Prerequisites

| Tool | Linux | Windows |
|------|-------|---------|
| C++17 compiler | GCC 11+ / Clang 14+ | MSVC 2022 / MinGW-w64 |
| CMake | 3.20+ | 3.20+ |
| Git | any | any |
| Qt 6.5+ (GUI only) | `qt6-base-dev qt6-declarative-dev` | [Qt Online Installer](https://www.qt.io/download) |
| Ninja | optional, recommended | optional |

No other dependencies — ModbusLib, CLI11, and toml++ are auto-fetched by CMake.

## Build

### CLI Only (no Qt required)

```sh
cd tools/modbus_debug_tool

# Configure
cmake --preset linux-release

# Build
cmake --build build/linux-release --target hvbctrl

# Binary at: build/linux-release/cli/hvbctrl
```

### CLI Only — Windows

```bat
cmake --preset win-release
cmake --build build/win-release --config Release --target hvbctrl
```

### With GUI

```sh
# Linux — set Qt path
cmake -B build/linux-gui -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_GUI=ON -DCMAKE_PREFIX_PATH=/path/to/Qt/6.x/gcc_64

cmake --build build/linux-gui --target hvb_modbus_gui
```

```bat
REM Windows — Qt path may vary
cmake -B build/win-gui -G "Visual Studio 17 2022" ^
    -DBUILD_GUI=ON -DCMAKE_PREFIX_PATH=C:/Qt/6.x/msvc2022_64

cmake --build build/win-gui --config Release --target hvb_modbus_gui
```

### Build Targets

| Target | Type | Dependencies |
|--------|------|-------------|
| `hvb_modbus_core` | Static library | ModbusLib, toml++ |
| `hvbctrl` | Executable | `hvb_modbus_core`, CLI11 |
| `hvb_modbus_gui` | Executable | `hvb_modbus_core`, Qt 6 (Core, Quick, QuickControls2) |

## Quick Start

```sh
# List serial ports
./hvbctrl list ports

# Connect and read system info
./hvbctrl -p /dev/ttyUSB0 info

# Channel 0 status
./hvbctrl -p /dev/ttyUSB0 status

# Enable channel 0 at 500 V
./hvbctrl -p /dev/ttyUSB0 channel 0 voltage 500
./hvbctrl -p /dev/ttyUSB0 channel 0 output ENABLE

# Live polling every 2 seconds
./hvbctrl -p /dev/ttyUSB0 monitor 2

# Save connection for next session
./hvbctrl -p /dev/ttyUSB0 --save info

# From now on, just:
./hvbctrl info
./hvbctrl status
```

## Configuration

Connection settings persist to `~/.hvb_modbus_tool.toml`:

```toml
[connection]
port = "/dev/ttyUSB0"
baud_rate = 115200
slave_id = 1
timeout_ms = 500

[display]
poll_interval_ms = 2000
```

Priority: CLI arguments (`-p`, `-b`, `-i`) > config file. Use `--save` to persist.

## CLI Reference

### Connection

```
-p, --port PORT       Serial port
-b, --baud RATE       Baud rate (9600, 115200, default: 115200)
-i, --id N            Slave ID (0-247, default: 1)
-t, --timeout MS      Timeout ms (default: 500)
--save                Persist to config file
```

### Discovery

```
list ports             List serial ports
list regs              Print register catalog from metadata
describe <ADDR>        Show register metadata (hex PDU address)
```

### Read-Only

```
info                   System info dump (protocol, variant, temp, uptime, opmode)
status                 CH0 + CH1 V/I/status summary
monitor [INTERVAL_S]   Live polling table, Ctrl-C to stop (default: 2s)
```

### System Config

```
system config          Read system configuration

system mode <NORMAL|AUTO>
system recovery <POLICY> <DELAY> <MAX> <WINDOW>
    POLICY: MANUAL-LATCH | AUTO-RETRY | AUTO-DERATE | NEVER-RETRY
system safe-bands <V-PCT> <I-PCT>    Range: 0-50
system addr <ADDR>                    Range: 0-247
system baud <0|1>                    0=115200, 1=9600
system save / load / factory
```

### Channel

```
channel <CH> info                          Measurements (V, I, status, faults)
channel <CH> config                        Full configuration
channel <CH> cal                           Calibration coefficients

channel <CH> voltage <V>                   Configured target voltage
channel <CH> output <ACTION>
    ACTION: NONE | ENABLE | DISABLE-GRACEFUL | DISABLE-IMMEDIATE
channel <CH> fault <CMD>
    CMD: CLEAR-ACTIVE | CLEAR-HISTORY

channel <CH> ramp-up <STEP_V> <INTERVAL_S>
channel <CH> ramp-down <STEP_V> <INTERVAL_S>

channel <CH> prot-v <MODE> <ACTION> <THRESHOLD_V>
    MODE:   DISABLED | FLAG-ONLY | APPLY-ACTION
    ACTION: NONE | DISABLE-GRACEFUL | DISABLE-IMMEDIATE | FORCE-ZERO | CLAMP
channel <CH> prot-i <MODE> <ACTION> <THRESHOLD_A>
    ACTION: NONE | DISABLE-GRACEFUL | DISABLE-IMMEDIATE | FORCE-ZERO

channel <CH> derate <STEP_V>
channel <CH> cal-out <K_UINT16> <B_INT16>
channel <CH> cal-meas-v <K_UINT16> <B_INT16>
channel <CH> cal-meas-i <K_UINT16> <B_INT16>

channel <CH> save / load / factory
```

### Debug

```
reset                   Software reset

raw fc04 <ADDR> <COUNT>     Raw FC04 read, hex dump
raw fc03 <ADDR> <COUNT>     Raw FC03 read, hex dump
raw fc06 <ADDR> <VALUE>     Raw FC06 write (uint16)
raw fc10 <ADDR> <HI> <LO>   Raw FC10 write 32-bit (big-endian)
```

## GUI

### Layout

```
┌──────────────────────────────────────────────────────┐
│ [Port ▼] [Baud ▼] [ID] [● Connect] [↻ 2s]           │
├──────────────────────────────────────────────────────┤
│ ┌ System Info ┬ System Config ┬ Channel 0 ┬ Ch 1 ┐  │
│ │             │               │           │       │  │
│ │ Protocol    │ OpMode        │ Vmeas V   │ same  │  │
│ │ Variant ID  │ Slave Addr    │ Imeas A   │       │  │
│ │ Board Temp  │ Baud Rate     │ Status    │       │  │
│ │ Uptime      │ Recovery      │ Faults    │       │  │
│ │ FW Version  │ Safe Bands    │ Target V  │       │  │
│ │ OpMode      │ [Save] [Load] │ Output    │       │  │
│ │ Cap Flags   │ [Factory]     │ Ramping   │       │  │
│ │             │ [Reset]       │ Protection│       │  │
│ │             │               │ Calibratn │       │  │
│ └─────────────┴───────────────┴───────────┴───────┘  │
├──────────────────────────────────────────────────────┤
│ [Status: Connected]      [Debug] [Refresh]           │
├──────────────────────────────────────────────────────┤
│ 14:32:01 Tx: 01 04 00 00 00 10 F1 CD                │
│ 14:32:01 Rx: 01 04 20 00 02 00 00 ...               │
└──────────────────────────────────────────────────────┘
```

### Tabs

| Tab | Content |
|-----|---------|
| System Info | Protocol version, variant, temp/humidity, uptime, FW version, operating mode, system status/fault, capability flags |
| System Config | Operating mode selector, slave address, baud rate, recovery policy + params (delay/max/window), voltage/current safe bands, save/load/factory/reset buttons |
| Channel 0/1 | Measurements (V/I/operational target), status bits, fault causes, capabilities; config: target voltage, output action, fault clear, ramping, voltage/current protection (mode + action + threshold), derate step, calibration (K/B per path), channel persistence |

### Debug Dialog

Raw Modbus access: FC03/FC04 read with hex output, FC06/FC10 write. Accessible via "Debug" button on status bar.

## Packaging

### CLI — Static Binary

ModbusLib is a shared library by default. For a portable CLI binary, build with static linking:

```sh
# CMake config with static ModbusLib
cmake -B build/static -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=OFF
cmake --build build/static --target hvbctrl

# Result: single binary, copy anywhere
cp build/static/cli/hvbctrl /usr/local/bin/
```

### GUI — Linux AppImage

```sh
# Requires: linuxdeployqt
# Build with install prefix in build tree
cmake -B build/appimage -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_GUI=ON -DCMAKE_PREFIX_PATH=/path/to/Qt/6.x/gcc_64 \
    -DCMAKE_INSTALL_PREFIX=/usr

cmake --build build/appimage --target hvb_modbus_gui

# Package
linuxdeployqt build/appimage/gui/hvb_modbus_gui \
    -qmldir=gui/resources/qml \
    -appimage

# Result: HVB_Modbus_Tool-*.AppImage
```

### GUI — Windows

```bat
REM Requires: windeployqt (ships with Qt)
cmake -B build/win-gui -G "Visual Studio 17 2022" ^
    -DBUILD_GUI=ON -DCMAKE_PREFIX_PATH=C:/Qt/6.x/msvc2022_64

cmake --build build/win-gui --config Release --target hvb_modbus_gui

REM Package
windeployqt build/win-gui/gui/Release/hvb_modbus_gui.exe ^
    --qmldir gui/resources/qml

REM Distribute the Release folder as a zip
```

## File Structure

```
tools/modbus_debug_tool/
├── CMakeLists.txt              # Top-level: FetchContent deps, targets
├── CMakePresets.json           # Build presets
├── README.md
├── core/                       # Layer 1: static library
│   ├── types.h                 # Enums, structs, scaling helpers
│   ├── register_map.h          # constexpr register addresses
│   ├── register_meta.h/.cpp    # RegDesc metadata tables
│   ├── hvb_modbus_client.h/.cpp# ModbusLib RTU client wrapper
│   └── config_manager.h/.cpp   # TOML config (~/.hvb_modbus_tool.toml)
├── cli/                        # Layer 2: CLI frontend
│   └── main.cpp                # CLI11 subcommands → HvbModbusClient
├── gui/                        # Layer 3: QML frontend
│   ├── main.cpp                # QML engine + backend registration
│   ├── modbus_backend.h/.cpp   # QML-exposed QObject
│   ├── modbus_worker.h/.cpp    # QThread: owns HvbModbusClient
│   └── resources/
│       ├── qml.qrc
│       └── qml/                # 10 QML files + components
└── scripts/                    # Deployment scripts
```

## Implementation Notes

- **Register map**: Follows `ref/modbus_interface.md` v2 (system block + channel blocks, FC03/04/06/10)
- **Scaling**: Voltage in Vx10, current in nA, intervals in seconds x10, calibration K as UINT16 x10000, B as INT16 x1000
- **Output action context**: The `OutputAction` enum has context-specific validity — `Enable` is host-only, `ForceOutputZero` is protection-only, `Clamp` is voltage-protection-only. CLI and GUI validate per context.
- **32-bit writes**: All INT32/UINT32 registers written atomically via FC10. Single-register writes to 32-bit fields are rejected at the protocol level.
- **Command registers**: Output Action, Fault Command, and Param Action registers are self-clearing (read back 0 after execution).
- **Thread safety**: GUI uses a QThread worker for all Modbus I/O. Blocking calls never block the QML render thread.
- **Config persistence**: Connection preferences saved to `~/.hvb_modbus_tool.toml` via toml++. Auto-created on first `--save`.

## Compliance

- [x] Protocol v2 register map (`ref/modbus_interface.md`)
- [x] Domain terminology (`UBIQUITOUS_LANGUAGE.md`, `CONTEXT.md`)
- [x] Output action context validation (§8.2)
- [x] Protection mode + action separation (§8.5)
- [x] System-wide recovery policy (§10.1)
- [x] Safe band percentages (§10.1)
- [x] UINT16 calibration K, INT16 calibration B (§5)
- [x] Big-endian 32-bit word order (§5)
- [x] Atomic FC10 writes for 32-bit values (§4)
- [x] Self-clearing command registers (§10.2)
- [x] Modbus exception mapping (§12)
