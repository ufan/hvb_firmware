# High-Level Firmware Architecture Design

Date: 2026-06-15
Status: Approved for implementation planning

## Scope

This document defines the conceptual runtime architecture for Jianwei voltage-control board firmware. It covers the shared architecture for high-voltage and low-voltage boards, single-channel and multi-channel boards, and different precision or hardware revisions.

Detailed protocol-neutral behavior is specified in `docs/superpowers/specs/2026-06-15-voltage-control-domain-behavior.md`. Modbus register layout is specified in `ref/modbus_interface.md`.

Implementation terminology and Zephyr-native composition details are refined by `docs/superpowers/specs/2026-06-18-zephyr-native-production-runtime-architecture.md`. In case of terminology conflict, that later spec supersedes this document's older `Runtime Services` and `channel_service` wording.

The current HVB controller is the first concrete product variant using this architecture. The architecture must remain reusable across future Jianwei voltage-control boards whose DAC, ADC, sensing, channel count, voltage range, precision, and communication hardware may differ.

The shared architecture covers protocol access, product domain model, channel control, safety supervision, persistence, environmental telemetry, status indication, variant configuration, and hardware support boundaries.

OTA is intentionally treated as an extension point. This architecture should leave room for OTA, but it does not define bootloader, image management, packet flashing, or update-state-machine behavior.

## Architecture Summary

The firmware uses a Zephyr-native voltage-control domain runtime library surrounded by frontend adapters and board-variant virtual voltage channel providers.

```text
Frontend Adapters
  - Modbus and shell now
  - CAN/TCP/IP/local UI later
        |
        v
Zephyr-native Domain Runtime Library
  - product state and host-visible snapshots
  - command validation and operating modes
  - channel capability interpretation
  - ramping, protection, recovery, calibration policy
  - persistence semantics
        |
        v
Virtual Voltage Channel Providers
  - board-variant channel abstraction
  - on/off mandatory; raw drive, measurement, calibration, status capability-gated
        |
        v
Zephyr Devices / Drivers / Services
  - GPIO, DAC, ADC, sensor, SPI, I2C, Modbus, shell, settings/NVS, logging
```

The selected architecture is a Zephyr-native domain runtime library with virtual voltage channel providers. Zephyr provides standard operating-system, driver, protocol, build, and composition facilities wherever possible. Product-family code owns voltage-control behavior and frontend adaptation. Board-specific channel behavior is isolated behind virtual voltage channel providers described by devicetree and enabled by Kconfig.

## Product Family Model

The firmware architecture serves the Jianwei voltage-control board family, not only one high-voltage board. A board variant may differ by:

| Variant dimension | Examples |
|---|---|
| Voltage class | High-voltage or low-voltage output |
| Channel count | Single-channel or multi-channel |
| Precision class | High-precision or low-cost/low-precision measurement and output paths |
| Output hardware | Different DACs, PWM paths, analog front ends, or run/stop controls |
| Measurement hardware | Different ADCs, sensors, gains, dividers, shunts, or sample rates |
| Communication hardware | RS-485 today, Ethernet or Bluetooth later |
| Board revision | Pin, peripheral, and component changes between hardware versions |

The shared domain must model product capabilities rather than named chips. Board variants provide devicetree/Kconfig-backed Virtual Channel Providers and product capability/default descriptions that tell the shared application which channels exist, what ranges and precision they support, and which channel capabilities are available.

```text
Shared voltage-control firmware
  -> product-family domain API
  -> variant profile
       - channel count
       - voltage/current ranges
       - precision/scaling capabilities
       - safety capabilities
       - hardware bindings
  -> board-specific hardware support
```

The HVB variant can implement Virtual Channel Providers using AD5541, ADS1232, SHT31, HV run/stop GPIOs, and RS-485 Modbus. Another variant may use different parts while preserving the same Frontend Adapter, Domain Runtime Library, persistence, and virtual channel architecture.

## Design Principles

### Zephyr First

Use Zephyr-native APIs and subsystems wherever they fit. Do not reimplement transport, UART handling, GPIO, SPI, I2C, logging, kernel timers, threads, work queues, or storage without a verified gap.

Zephyr is the chosen RTOS because it fits the voltage-control board family in three important ways:

| Fit | Architectural value |
|---|---|
| Repository and board management | West, out-of-tree boards, devicetree, Kconfig, and modules support a family of related board variants without copying firmware architecture per board |
| Driver and sensor sharing | Standard driver, sensor, bus, storage, logging, and protocol subsystems reduce custom code and make hardware variants easier to add |
| MCU separation from core logic | Devicetree and Zephyr device APIs keep STM32-specific pin/peripheral details outside the product domain, allowing the voltage-control core to stay mostly MCU-independent |

This reinforces the main boundary rule: product behavior belongs in the Domain Runtime Library; board-specific channel behavior belongs in Virtual Voltage Channel Providers; MCU, board, and chip details belong in Zephyr board definitions, devicetree, drivers, and provider implementations.

Preferred ownership:

| Area | Preferred owner |
|---|---|
| Modbus RTU framing, UART, CRC, server plumbing | Zephyr Modbus serial support |
| RS-485 DE GPIO | Zephyr Modbus serial/devicetree support via `de-gpios` |
| GPIO access | Zephyr GPIO API and devicetree specs |
| SPI DAC transfer | Zephyr SPI API |
| Environmental sensor access | Zephyr sensor driver if available; otherwise a small adapter over Zephyr I2C/SPI/GPIO as needed |
| NVM/settings | Zephyr settings/NVS if suitable |
| Timers, threads, work | Zephyr kernel primitives |
| Logging | Zephyr logging |
| Product state and safety policy | Domain Runtime Library |
| Board-specific channel abstraction | Virtual Voltage Channel Providers over Zephyr APIs |
| Variant-specific chip behavior | Custom support over Zephyr APIs when no suitable Zephyr driver exists |

### Protocol Adapters Are UI Boundaries

Modbus is a remote-control UI/protocol adapter, not the product model. The domain core must not be shaped around Modbus register blocks, function codes, register packing, exception codes, broadcast behavior, or RS-485 timing.

The domain exposes protocol-neutral and board-family-neutral commands and queries, such as:

| Domain concept | Description |
|---|---|
| Set channel target voltage | Request a target voltage in product units |
| Request channel enable/disable | Request runtime enable state without exposing Modbus switch registers |
| Set ramp parameters | Configure ramp step and interval semantics |
| Set limit policy | Configure current/voltage limit modes and thresholds |
| Read channel telemetry | Query measured voltage, current, status, and fault state |
| Save/load/factory reset parameters | Execute persistence actions through product semantics |

Future CAN, TCP/IP, Bluetooth, shell, or local UI frontends must translate frontend-specific requests into the same Domain Runtime Library facade used by the Modbus adapter.

```text
RS-485 Modbus -> modbus_adapter   \
Shell         -> shell_adapter     -> Domain Runtime Library facade
CAN/TCP/IP    -> future adapters  /
```

### Hardware Support Is Policy-Free

Low-level hardware support must remain hardware-shaped. DAC, ADC, GPIO, sensor, and board-mode support should not know about protocol registers, calibration persistence, ramping policy, or safety policy. Board-specific Virtual Channel Providers compose those hardware pieces into product-facing channel capabilities without owning product policy.

Examples:

| Hardware support | Owns | Must not own |
|---|---|---|
| Virtual Channel Provider | Channel on/off, raw drive, raw measurement, raw calibration/status capabilities over Zephyr APIs | Product policy, frontend behavior, calibrated unit semantics |
| Output hardware support | DAC/PWM/output write behavior over Zephyr APIs | Voltage ramping or limit behavior |
| Measurement hardware support | ADC/sample acquisition, raw measurement capture | Calibration policy or fault limits |
| Run/stop GPIO support | Board output-enable access where available | Deciding when output is allowed |
| Board mode support | Future boot-time SYS_MOD DIP sampling | Initial channel availability policy |
| Status support | LED output access | Product health policy beyond requested indication |

## Conceptual Components

| Component | Responsibility |
|---|---|
| `voltage_control_app` | Boot orchestration, safe startup, variant selection, service startup, top-level lifecycle |
| Zephyr Modbus server | RTU transport, UART, CRC, frame handling, RS-485 DE handling via devicetree |
| `modbus_adapter` | Register map, access rules, exception mapping, translation between Modbus and domain commands/snapshots |
| Domain Runtime Library facade | Zephyr-native command/query boundary for all present and future frontend adapters |
| Domain Runtime Library internals | Product state, operating mode, channel capability model, command validation, ramping, protection, recovery, calibration policy, persistence semantics, host-visible snapshots |
| Virtual Voltage Channel Provider | Board-specific channel abstraction: on/off mandatory; raw drive, raw measurement, raw calibration/status capability-gated |
| `variant_profile` | Product-family defaults and limits that may be derived from devicetree/Kconfig and channel capabilities |
| `param_store` | Save/load/factory reset using Zephyr storage/settings where suitable |
| `env_service` | Board temperature/humidity sampling through Zephyr sensor/I2C support |
| `status_service` | SYS_RUN LED/status indication through Zephyr GPIO/LED APIs |
| Output hardware support | Board/chip-specific output driver or adapter over Zephyr APIs |
| Measurement hardware support | Board/chip-specific ADC/sample driver or adapter over Zephyr APIs |
| `board_mode` | Future SYS_MOD DIP read support through Zephyr GPIO; not part of the initial implementation |

Dependency direction:

```text
Frontend Adapters -> Domain Runtime Library facade -> Domain Runtime Library
Domain Runtime Library -> Virtual Voltage Channel Providers and Zephyr services
Virtual Voltage Channel Providers -> Zephyr APIs/devicetree/device model
```

The Domain Runtime Library may use Zephyr-native facilities such as devicetree-derived configuration, Kconfig, device handles, workqueues, settings, and logging. It should not depend on specific GPIO pins, SPI/I2C peripheral numbers, DAC/ADC part numbers, Zephyr Modbus callback details, or board-private timing assumptions. Those belong in board devicetree, drivers, and Virtual Channel Providers.

## Runtime Data Flow

### Modbus Write

```text
Modbus write
  -> Zephyr Modbus server receives RTU request
  -> modbus_adapter validates register/function shape
  -> adapter translates register write to protocol-neutral domain command
  -> voltage_control_domain validates product meaning against variant capabilities and range
  -> domain state changes or returns an error
  -> adapter maps domain result to Modbus success or exception
```

### Modbus Read

```text
Modbus read
  -> Zephyr Modbus server receives RTU request
  -> modbus_adapter queries protocol-neutral Domain Snapshot
  -> adapter packs product state into Modbus register values
  -> Zephyr Modbus server returns RTU response
```

### Future Protocol Write

```text
Ethernet/Bluetooth request
  -> protocol adapter validates protocol shape
  -> adapter translates request to the same domain command
  -> voltage_control_domain handles command without knowing the source protocol
```

### Channel Control Tick

```text
virtual channel measurement/update
  -> Virtual Channel Provider publishes raw Measurement Snapshot
  -> Domain Runtime Library applies calibration, freshness, protection, recovery, and ramping policy
  -> Domain Runtime Library publishes host-visible Domain Snapshot
  -> Domain Runtime Library emits raw channel commands/config snapshots back to Virtual Channel Providers
```

Raw Modbus register integers live at the Modbus adapter boundary. Product state is represented with named domain fields, variant capabilities, and product units, not register offsets.

## Operating Modes

Detailed operating-mode behavior is specified in `docs/superpowers/specs/2026-06-15-voltage-control-domain-behavior.md`.

The voltage-control domain supports operating modes as product behavior, not as Modbus behavior. Protocol adapters expose and configure modes, but the meaning of each mode belongs in the domain and services.

Initial modes:

| Mode | Behavior |
|---|---|
| Normal | Outputs are explicitly controlled by a remote frontend. After a protective disable, the user must explicitly clear the blocking condition and re-activate the channel according to the product rules. |
| Automatic | After safe startup and configuration load, the domain may automatically request channel enable and ramp to a nonzero Configured Target Voltage if configuration and safety state allow it. |

Automatic mode must use the same domain commands, validation, ramping path, and safety supervision as remote control. It must not write hardware directly or bypass normal product rules.

```text
boot
  -> safe outputs
  -> load persisted configuration
  -> initialize domain and supervisor
  -> evaluate operating mode

normal mode
  -> wait for remote frontend commands

automatic mode
  -> validate automatic-start configuration per channel
  -> request channel enable through domain command path
  -> Domain Runtime Library applies configured ramp policy and emits raw channel commands through Virtual Channel Providers
  -> Domain Runtime Library protection policy remains final authority
```

Operating mode is part of the shared voltage-control model. Runtime mode changes do not disrupt currently running channels. Switching to Automatic does not retry existing Active Fault Blocks. Switching from Automatic to Normal cancels pending retries and converts them to manual Active Fault Blocks. Different board variants may restrict or disable automatic mode if the hardware cannot support safe automatic recovery.

## Safety Model

Detailed protection, fault, safe-band, and automatic-recovery behavior is specified in `docs/superpowers/specs/2026-06-15-voltage-control-domain-behavior.md`.

Safety is split between per-channel detection, domain recovery policy, and final supervisor authority.

```text
Channel service
  detects measured current/voltage limit crossings
  records local fault evidence
  applies immediate per-channel action when unambiguous

Domain recovery policy
  classifies whether a fault is retryable or latched
  applies operating-mode-specific recovery rules
  bounds automatic retry behavior

Safety supervisor
  consumes channel fault evidence
  latches product-level fault state
  enforces final safe-state output
  prevents stale frontend state from re-enabling unsafe output
```

Virtual Channel Providers are closest to measurement and hardware timing, so they publish Measurement Snapshots as quickly as their hardware permits and apply raw channel commands from the Domain Runtime Library. The Domain Runtime Library consumes those snapshots, detects threshold crossings, records fault evidence, applies protection/recovery policy, and emits safe-state raw commands such as force output zero or disable Output Enable.

Fault evidence, protection action, and recovery policy are separate concepts:

| Concept | Meaning |
|---|---|
| Fault evidence | A measured value crossed a configured threshold or another hardware condition was observed |
| Protection mode | Whether a protection is disabled, records Fault History only, or applies a Protection Output Action |
| Protection output action | Immediate output-state action to reduce risk, such as clamp, ramp down, force output zero, or disable output |
| Recovery policy | Whether firmware may retry automatically, and under what bounds |
| Active Fault Block | A current blocking condition that requires explicit clear or bounded automatic recovery |
| Fault History | A record that a fault event occurred; it remains until explicitly cleared |

Normal mode and automatic mode may use different recovery policies for the same detected limit event. In normal mode, a protective disable should normally latch the channel and require explicit user clear/re-enable. In automatic mode, selected faults may enter a bounded cooldown-and-retry flow.

```text
normal mode limit event
  -> record fault evidence
  -> execute protection output action if Protection Mode applies one
  -> create Active Fault Block unless Protection Mode is Flag Only
  -> require remote clear/re-enable

automatic mode retryable limit event
  -> record fault evidence and Fault History
  -> execute protection output action if Protection Mode applies one
  -> enter cooldown
  -> retry only if retry policy allows and measurements return to a safe band
  -> create Active Fault Block if severe fault, unsafe clear condition, derate floor reached, or retry budget is exhausted
```

Automatic recovery should be controlled by explicit policy fields rather than hidden behavior:

| Policy field | Purpose |
|---|---|
| Recovery Policy Mode | System-wide manual latch, auto retry, auto derate retry, or never retry |
| Auto Retry Delay | Cooldown time before retry |
| Auto Retry Max Count | Maximum retries in the sliding retry window |
| Auto Retry Window | Sliding window used to count retries |
| Voltage Safe Band % | Voltage band around target required before retry |
| Current Safe Band % | Current below-limit margin required before retry |
| Auto Derate Step | Per-channel target reduction after each retry when derate retry is used |

Automatic retry must be conservative. In Normal mode, runtime behavior is always manual latch even if auto-retry settings are configured. Auto-retry applies only to faults detected while Automatic mode is already active. Current is evaluated before voltage; if both current and voltage faults occur in one cycle, current determines the protection output action. Protection uses calibrated measured voltage/current values, not raw ADC values.

Domain Runtime Library protection policy is the final authority. If a fault requires safe state, it wins over frontend intent. For example, even if a frontend requests channel enable, an Active Fault Block forces the variant's safe output state, such as run/stop low and Output Drive Level zero on HVB, until the defined clear/recovery path occurs.

Safety invariants:

| Invariant | Meaning |
|---|---|
| Boot safe | Variant output-enable controls are in their safe state before any service can enable outputs |
| Output drive safe | Output Drive Level is zero or otherwise safe at startup and during immediate safe-state actions |
| Supervisor wins | No service may energize or enable an output against a supervisor safe-state command |
| Active fault blocks are explicit | Blocking fault state is not silently cleared after measurements recover |
| Fault history persists in RAM | Automatic retry may clear an active retry block, but it must not erase Fault History |
| Current first | Current limit is evaluated before voltage limit |
| Unsupported channels rejected | Channels not supported by the current firmware/hardware configuration return Modbus exception `0x02` in the Modbus adapter |
| Volatile enables | Runtime enable requests do not persist across boot |

Active Fault Block clear is rejected while the measured condition remains unsafe. Fault History is cleared only by an explicit per-channel fault command.

## Startup Model

Startup follows a safe-first sequence:

1. Initialize Zephyr and board devices.
2. Configure variant output-enable controls to the safe disabled state.
3. Write safe Output Drive Level values, such as DAC code zero for HVB, to all physically present outputs before any enable path is available.
4. Establish the initial supported channel set from firmware/hardware configuration; do not sample SYS_MOD in the initial implementation.
5. Load persistent parameters, excluding volatile runtime enable state.
6. Initialize domain state, protection policy state, and provider links.
7. Evaluate operating mode.
8. Start channel, environment, status, and protocol services.
9. In Automatic mode only, request automatic channel start through the normal domain command path for channels with nonzero Configured Target Voltage after safety and configuration checks pass. Startup sequencing is variant-defined; HVB defaults to parallel startup unless hardware constraints require sequencing.

This sequence ensures protocol access cannot energize or enable outputs before safe output defaults, channel availability configuration, and domain state are established.

## Persistence Model

Detailed persistence scope and parameter-action behavior is specified in `docs/superpowers/specs/2026-06-15-voltage-control-domain-behavior.md`.

Persistence is a product-domain concern implemented through Zephyr storage facilities where suitable. The domain decides what values are persistent, volatile, defaulted, or effective only after restart. The storage layer only saves and loads data records.

Rules from the Modbus reference that must remain product semantics:

| Parameter class | Persistence rule |
|---|---|
| Runtime enable requests | Never persisted; initialize disabled at boot |
| Configured Target Voltage | Persist only when the save-target policy says to do so; otherwise save the variant safe target default |
| Ramp settings | Persist on save action |
| Protection modes, Protection Output Actions, and thresholds | Persist on save action |
| Operating mode | Persist on save action if the product variant supports automatic mode |
| System recovery policy and safe-band settings | Persist on system save action if automatic recovery is supported |
| Per-channel Auto Derate Step | Persist on channel save action if automatic recovery is supported |
| Calibration coefficients | Persist on channel save action; K is UINT16 and B is INT16 |
| Slave address and baud rate | Persist on save action and become effective after restart |
| Active Fault Blocks and Fault History | Runtime only unless a future spec defines otherwise |

Protocol command registers, such as Modbus parameter-action and channel action registers, are adapter representations of product commands. Their self-clearing readback behavior belongs in the protocol adapter. System Param Action is system-only; Channel Param Action is channel-only.

## Extension Points

### Additional Protocols

CAN, TCP/IP, Bluetooth, shell, display, or button frontends can be added as Frontend Adapters. They must call the same Domain Runtime Library facade as the Modbus adapter. They must not bypass the domain to call Virtual Channel Providers or hardware directly.

### Board Variants

New Jianwei voltage-control boards should be added as variants rather than new architectures. A new variant supplies board capabilities, hardware bindings, calibration defaults, supported protocols, channel count, range/precision limits, and any unavailable safety actions.

The shared domain and services should not assume high voltage, two channels, AD5541 output, ADS1232 measurement, SHT31 environment sensing, or RS-485-only communication. Those are properties of the current HVB variant.

### OTA

OTA remains an extension point. The current architecture reserves space for future update services and protocol commands, but does not define bootloader boundaries, image layout, packet handling, CRC behavior, or target selection.

### Channel Count Selection

The initial implementation does not use the SYS_MOD DIP pins. Channel availability is fixed by the firmware/hardware configuration for the first working version. The domain should still avoid hard-coding Modbus register layout into product behavior, so a later iteration can add SYS_MOD-based channel count selection without reshaping protocol-neutral commands.

When SYS_MOD support is added, it should be implemented as a boot-time board-mode input that establishes the addressable channel set before protocol services start. Access to channels outside that set should be rejected at the protocol adapter boundary according to the domain channel-availability model.

## Open Decisions

The architecture depends on these behavioral decisions from the reference documents, but does not resolve them here:

| Decision | Reason it remains open |
|---|---|
| Retryable versus non-retryable fault classes | Requires product and hardware safety decisions per board variant |
| Firmware version encoding | The reference does not define the 32-bit version format |
| OTA CRC and packet byte order | OTA is out of scope for this architecture pass |
| Exact Modbus timeout tolerance | Zephyr support should be used, but final tolerance may require verification |

These should be resolved in targeted specs before implementation work that depends on them.
