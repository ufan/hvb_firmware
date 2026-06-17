# HVB Host Tools Architecture

Date: 2026-06-16
Status: Draft for implementation planning

## Scope

This document defines the architecture, directory layout, target decomposition, dependency strategy, and naming conventions for all HVB host-side Modbus tools. It replaces the current single `tools/modbus_debug_tool` monolith with a shared-core multi-application layout.

Domain behavior and protocol register definitions live in their existing authoritative documents:
- `docs/superpowers/specs/2026-06-15-voltage-control-domain-behavior.md`
- `ref/modbus_interface.md`
- `CONTEXT.md`
- `include/regmap/hvb_regs.h`

## Motivation

The current `tools/modbus_debug_tool` mixes three concerns in one directory:

1. A shared Modbus client core with protocol types, register metadata, and TOML config
2. An end-user/operator demo application (CLI + TUI + GUI)
3. Factory/service calibration commands (coefficient writes mixed into demo surfaces)

Calibration Mode (protocol 2.1, operating mode 2) is explicitly a factory/professional diagnostic surface. Placing raw DAC/ADC controls and coefficient writes inside the demo app blurs the product seam between end-user operator tools and factory calibration tools.

Additionally, the current naming (`modbus_debug_tool`, `hvbctrl`, `hvb_tui`, `hvb_modbus_tool`) no longer reflects the intended role of the demo app as an end-user operator console.

## Design Decisions

### Separated Applications Sharing One Core

```
tools/hvb_modbus_core/    shared static library
tools/hvb_demo_app/       end-user/operator console (CLI + TUI + GUI)
tools/hvb_factory_tool/   factory/service calibration console (REPL + GUI)
tools/shared_qml/         shared QML theme singleton used by both GUIs
```

The core library wraps Modbus transport, protocol types, register access, register metadata, TOML config, and monitor rendering. Both apps link against it and add no transport-layer dependencies of their own.

### Demo App is End-User Only

- Supports Normal and Automatic operating modes.
- Displays calibration coefficients as read-only system info.
- Displays protocol 2.1 and calibration capability bit.
- Removes writable calibration coefficient controls from CLI and TUI.
- Keeps raw FC03/FC04/FC06 debug commands for developer escape hatch only.

### Factory Tool is Calibration-Only

- Unlock sequence, mode entry/exit, raw DAC, raw ADC sample, coefficient write, commit.
- Provides both a Cisco-style REPL and a guided Qt/QML GUI.
- Does not duplicate normal operator controls (target voltage, ramping, protection, etc.) — the demo app owns those.

### Dependency Strategy

| Library | Integration | Used by |
|---------|------------|---------|
| ModbusLib v0.4.8 | FetchContent (existing) | core |
| CLI11 v2.4.2 | FetchContent (existing) | demo CLI |
| FTXUI v5.0.0 | FetchContent (existing) | demo TUI |
| toml++ v3.4.0 | FetchContent (existing) | core |
| daniele77/cli v2.2.0 | FetchContent (new) | factory REPL |
| Qt 6.8+ | System install | demo GUI, factory GUI |
| Catch2 | FetchContent (existing) | core tests |

No new C++ dependencies beyond daniele77/cli (header-only). Qt is already used by the current GUI and is not new.

### Binary Naming

| Old | New | Role |
|-----|-----|------|
| `hvbctrl` | `hvb_demo_cli` | Operator one-shot CLI |
| `hvb_tui` | `hvb_demo_tui` | Operator dashboard TUI |
| `hvb_gui` | `hvb_demo_gui` | Operator Qt GUI |
| — | `hvb_factory_tui` | Calibration Cisco-style REPL |
| — | `hvb_factory_gui` | Calibration guided Qt GUI |

Config file: `~/.hvb_modbus_tool.toml` → `~/.hvb_demo_app.toml`

### QML Theming

Both GUIs share a `tools/shared_qml/HvbTheme.qml` singleton providing a light and dark palette. The pattern is drawn from `QmlAppTemplate/ComponentLibrary/ThemeEngine.qml` (MIT), trimmed to only the needed semantic color tokens. The underlying QML style is `QtQuick.Controls.Material`, which ships with Qt 6 and requires zero extra dependencies.

## Architecture

### hvb_modbus_core

```
core/
  types.h                  protocol enums, bitflags, structs (SystemInfo, ChannelInfo, SystemConfig, ChannelConfig)
  calibration_types.h      CalibrationSampleStatus enum, CalibrationSnapshot struct
  register_map.h           includes firmware regmap/hvb_regs.h, scaling helpers
  register_meta.h/.cpp     human-readable register catalog for CLI list/describe
  config_manager.h/.cpp    TOML config (~/.hvb_demo_app.toml)
  hvb_modbus_client.h/.cpp ModbusLib wrapper + typed high-level read/write APIs
  monitor_render.cpp       ASCII table renderer for CLI monitor mode
  CMakeLists.txt
```

Core APIs for calibration (additions to `HvbModbusClient`):

```
// Calibration unlock
bool unlockCalibrationStep(uint16_t value);
bool enterCalibrationMode();
bool exitCalibrationMode(OpMode targetMode);

// Per-channel calibration
bool writeCalibrationOutputEnable(int ch, bool enable);
bool writeRawDacCode(int ch, uint16_t code);
bool sendCalibrationSampleCommand(int ch);
bool sendCalibrationCommitCommand(int ch);
CalibrationSnapshot readCalibrationSnapshot(int ch);
```

`CalibrationSnapshot` bundles raw ADC voltage (int32), raw ADC current (int32), sample status, raw DAC readback, output enable, raw DAC code, and max raw DAC limit into a single read so REPL watcher code does not need to know register batching.

### hvb_demo_app

```
cli/
  main.cpp                 CLI11 one-shot operator commands
tui/
  main.cpp                 FTXUI app entry, poll thread, connection modal
  tab_monitor.h            Monitor tab with table + action panel
  tab_system.h             System info (read-only) + config (writable)
  tab_channel.h            Channel live bar + output/ramp/protection/persistence panels
  widgets.h                Shared UI helpers (AppState, writeAsync, CommitInput, InlineCycler, ActionButton)
gui/
  main.cpp, modbus_backend.*, modbus_worker.*, qml/
  (existing structure, naming updated)
docs/, deploy scripts, README, CMakeLists.txt
```

Removals from demo:
- Calibration coefficient write commands from CLI (`cal-out`, `cal-meas-v`, `cal-meas-i`)
- Calibration coefficient write panel from TUI channel tab
- Calibration coefficient write fields from GUI channel tab

Additions to demo (read-only):
- Protocol 2.1 display in `info` / system tab
- Calibration capability bit in system capability flags
- Calibration Mode label in mode display helpers

### hvb_factory_tool

```
repl/
  main.cpp                 daniele77/cli REPL entry
  FactorySession.h/.cpp    connection, active channel, watch mode, scheduler
  FactoryCommands.h/.cpp   command handler implementations (dispatched from menu tree)
  FactoryMenu.h/.cpp       daniele77/cli menu tree construction
  CMakeLists.txt
gui/
  main.cpp                 Qt/QML app entry
  CalibrationBackend.h/.cpp QML-exposed C++ object (sits above HvbModbusClient)
  qml/
    MainWindow.qml          connection bar + workflow steps
    UnlockStep.qml          2-button unlock workflow
    EnterStep.qml           enter/exit mode with status
    ChannelControl.qml      enable/disable, raw DAC input, sample, auto-poll
    CoefficientsStep.qml    K/B fields for out/meas-V/meas-I
    CommitExitStep.qml      commit button, exit mode, safe all
  CMakeLists.txt
```

### shared_qml

```
HvbTheme.qml              pragma Singleton, LIGHT/DARK palettes
HvbControls.qml           optional: styled wrappers if needed beyond Material defaults
qmldir                    QML module registration
```

### Factory REPL Menu Tree

```
factory>                          root
  connect <port> [baud] [id]      establish Modbus connection
  info                            system info dump
  help                            list commands (built-in)
  quit                            exit REPL (built-in)

  factory(cal)>                   cal submenu (entered via unlock + mode enter)
    unlock                        send unlock step 1 (0xCA1B)
    unlock                        send unlock step 2 (0xA11B) — named same, advances state
    enter                         switch to Calibration Mode
    exit <normal|auto>            switch to target mode, force outputs off
    status                        show system + channel calibration state
    safe                          disable all cal outputs, zero raw DAC
    ch <0|1>                      enter channel submenu
    help

    factory(cal ch0)>             ch submenu
      enable                      enable calibration output on this channel
      disable                     disable calibration output, zero raw DAC
      dac <code>                  write raw DAC code (0..max limit)
      sample                      trigger raw ADC sample
      read                        read calibration snapshot (sample status + raw ADC + DAC readback)
      limit <max>                 write max raw DAC limit
      coeff out <k> <b>           write output calibration coefficients
      coeff meas-v <k> <b>        write voltage measurement calibration coefficients
      coeff meas-i <k> <b>        write current measurement calibration coefficients
      coeff show                  read back current coefficients
      commit                      commit coefficients for this channel
      watch adc [interval]        start periodic raw ADC sample + read (default 1s)
      watch measure [interval]    start periodic calibrated V/I read (default 1s)
      watch status [interval]     start periodic system/channel status refresh (default 1s)
      watch all [interval]        start all three combined (default 1s)
      watch off                   stop background watcher
      back                        return to cal submenu
      help
```

#### Watch Modes

| Command | Core API Used | Display |
|---------|-------------|---------|
| `watch adc` | `sendCalibrationSampleCommand(ch)` + `readCalibrationSnapshot(ch)` | raw ADC V/I, sample status, DAC readback, output state |
| `watch measure` | `readChannelInfo(ch)` | calibrated `voltageRaw`/`currentRaw` (V/I after coefficients applied) |
| `watch status` | `readSystemInfo()` + `readChannelInfo(all)` | system summary + channel status badges |
| `watch all` | all three combined | full status line: system + calibrated V/I + raw ADC + DAC |
| `watch off` | stops watcher thread | none |

`watch measure` does not trigger ADC samples — it reads the firmware's calibrated measurement registers directly. Useful for monitoring output stability after a DAC change without perturbing the ADC sample pipeline. Rejected outside Calibration Mode.

All watch modes require an active channel selected. Watcher stops on `exit`, `safe`, disconnect, or `quit`.

Interval format: `500ms`, `1s`, `2s`, `5s`. Default interval is `1s`.

Watch mode state machine:

```
WatchMode: Off | Adc | Measure | Status | All
  Off:       no background work
  Adc:       sendSampleCommand(ch), readCalibrationSnapshot(ch)
  Measure:   readChannelInfo(ch), display voltageRaw/currentRaw
  Status:    readSystemInfo(), readChannelInfo(all)
  All:       sendSampleCommand(ch), readCalibrationSnapshot(ch), readSystemInfo(), readChannelInfo(all)
```

#### GUI Auto-Poll Equivalent

In the factory GUI Channel Control step, the auto-sample dropdown:

```
Auto-sample: [Off ▼]
             Off
             ADC 500ms
             ADC 1s
             ADC 2s
             ADC 5s
             Measure 1s
             Measure 2s
             Status 1s
```

The Measure mode updates the calibrated V/I display fields without sending sample commands. The ADC mode sends samples and updates raw ADC fields. Status mode refreshes the system status bar.

Safety rules enforced by REPL handlers:
- `dac <nonzero>` rejected if channel not in Calibration Mode or output not enabled
- `commit` rejected if raw output is active or raw DAC is nonzero
- `safe` disables all calibration outputs and zeros raw DAC on all channels
- `watch adc` rejected outside Calibration Mode or without active channel
- Watcher stops on `exit`, `safe`, disconnect, or `quit`

### Factory GUI Workflow

Sequential steps, each gated on the previous:

1. **Unlock** — two buttons: Unlock Step 1 (0xCA1B), Unlock Step 2 (0xA11B). Status indicator.
2. **Enter Mode** — enter Calibration Mode button. Confirms outputs disabled.
3. **Channel Control** — channel selector (CH0/CH1), enable/disable toggle, raw DAC numeric input with set button and readback, sample button with status badge, auto-sample interval dropdown
4. **Coefficients** — K/B input fields for output, meas-V, meas-I. Write All button.
5. **Commit & Exit** — commit button (gated on outputs disabled), exit to Normal/Automatic, Safe All panic button

The GUI reuses `ModbusBackend`/`ModbusWorker` pattern from demo GUI. `CalibrationBackend` exposes calibration-specific QML properties: `calUnlocked`, `calActive`, `activeChannel`, `calOutputEnabled`, `rawDacCode`, `rawDacReadback`, `maxRawDacLimit`, `rawAdcVoltage`, `rawAdcCurrent`, `sampleStatus`, `sampleValid`, `sampleError`, `outCalK`, `outCalB`, `measVCalK`, `measVCalB`, `measICalK`, `measICalB`.

## Protocol 2.1 Core Changes

### types.h Additions

```cpp
enum class OpMode : uint16_t {
    Normal      = 0,
    Automatic   = 1,
    Calibration = 2,
};

enum class CalibrationSampleStatus : uint16_t {
    NoSample    = 0,
    Valid       = 1,
    Busy        = 2,
    Error       = 3,
};

// SysCap additions
namespace SysCap {
    inline constexpr uint16_t CALIBRATION_MODE = 0x0004;
}
```

### ChannelInfo Additions

```cpp
// v2.1 calibration fields
int32_t rawAdcVoltage = 0;    // signed 32-bit from HI/LO
int32_t rawAdcCurrent = 0;
CalibrationSampleStatus sampleStatus = CalibrationSampleStatus::NoSample;
uint16_t rawDacReadback = 0;
```

### ChannelConfig Additions

```cpp
bool calOutputEnabled = false;
uint16_t rawDacCode = 0;
uint16_t maxRawDacLimit = 4095;
```

### CalibrationSnapshot

```cpp
struct CalibrationSnapshot {
    bool outputEnabled = false;
    uint16_t rawDacCode = 0;
    uint16_t maxRawDacLimit = 0;
    uint16_t rawDacReadback = 0;
    CalibrationSampleStatus sampleStatus = CalibrationSampleStatus::NoSample;
    int32_t rawAdcVoltage = 0;
    int32_t rawAdcCurrent = 0;
};
```

### register_meta.cpp Additions

- System input: mode labels include "Calibration"
- System holding: mode labels include "Calibration"
- Channel input: add offsets 12–17 (raw ADC HI/LO, sample status, raw DAC readback)
- Channel holding: add offsets 21–25 (cal output enable, raw DAC code, sample cmd, commit cmd, max raw DAC limit)
- Extension holding: add offset 200 (CAL_UNLOCK)
- Update catalog headings to reflect v2.1 offsets

### hvb_modbus_client.cpp Additions

- `readChannelInfo()` reads 18 registers (offsets 0..17)
- `readChannelConfig()` reads 26 registers (offsets 0..25)
- New typed APIs for all calibration operations
- `readCalibrationSnapshot(ch)` bundles channel input offsets 12–17 + holding offsets 21–25

## Tests

### Core Tests

- Protocol 2.1 system info (minor=1, capability bit)
- Raw ADC signed 32-bit HI/LO decode
- Calibration unlock register read/write
- Calibration output enable, raw DAC code, sample command, commit command
- CalibrationSnapshot round-trip through test arrays
- Mode enum formatting for Calibration

### Demo App Tests

- Calibration coefficient writes removed from CLI/TUI
- Calibration capability displayed read-only in system info
- Protocol 2.1 displayed correctly

### Factory Tool Tests

- REPL menu navigation
- Watch command parsing and safety gates
- REPL command handler dispatch

### Test Fixture

- Virtual board defaults updated to protocol minor 1, calibration capability bit
- Virtual board supports calibration extension block writes
- CLI shell tests updated for `hvb_demo_cli` binary name

## Out of Scope

- Cryptographic unlock authentication
- Multi-board calibration orchestration
- Calibration report generation
- Automated coefficient fitting in host tools
- Remote/telnet access to calibration REPL (though daniele77/cli supports it, it is not in the initial slice)
- Mobile (Android/iOS) build targets for factory GUI
- Migration/backward compatibility with the old `modbus_debug_tool` layout
- Updating the existing Windows deploy script (deferred to a later slice)
