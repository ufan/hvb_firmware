# Ubiquitous Language

## Board family and variants

| Term | Definition | Aliases to avoid |
| --- | --- | --- |
| **Voltage-Control Board** | A Jianwei board that controls one or more voltage output channels and reports voltage/current telemetry. | HVB when referring to the whole product family |
| **HVB Variant** | The current Jianwei high-voltage board variant used as the first implementation of the voltage-control architecture. | Treating HVB as the whole architecture |
| **Variant Profile** | A board-specific description of channel count, ranges, precision, safety capabilities, and hardware bindings. | Board config, hardware config when discussing product capabilities |
| **Channel** | One independently controlled voltage output path with its own target, telemetry, protection settings, and runtime state. | Output, rail, module when independence matters |


## Runtime architecture

| Term | Definition | Aliases to avoid |
| --- | --- | --- |
| **Domain Runtime Library** | The Zephyr-native reusable product-policy library that owns voltage-control modes, validation, ramping, protection, recovery, calibration policy, persistence semantics, and host-visible snapshots. | Zephyr-free domain, protocol domain, app framework |
| **Virtual Voltage Channel** | A board-variant-provided abstraction for one controllable voltage channel that exposes channel capabilities and raw hardware evidence to the Domain Runtime Library. | Channel service when the intent is the channel abstraction, hardware support when product capabilities matter |
| **Virtual Channel Provider** | A Zephyr-style implementation of a Virtual Voltage Channel using devicetree-derived configuration, runtime data, and API callbacks. | Driver when discussing the product-facing channel boundary |
| **Channel Capability** | A static board-design ability of a Virtual Voltage Channel, such as on/off, raw output drive, voltage measurement, current measurement, or hardware status. Capabilities are derived from devicetree/Kconfig composition and are not dynamically negotiated at runtime. Calibration surfaces derive from Calibration Mode plus the relevant raw output or measurement capability. | Inferring capability from C function presence; treating capability as runtime configuration |
| **Frontend Adapter** | A user- or host-facing adapter that submits commands to and reads snapshots from the Domain Runtime Library. | Direct channel-service access |
| **Runtime Config Snapshot** | A complete versioned runtime intent published by the Domain Runtime Library for Virtual Channel Providers to apply. | Partial config update when crossing the domain/channel boundary |
| **Measurement Snapshot** | Raw hardware evidence published by a Virtual Channel Provider, including publication generation and measurement timestamp concepts. It is already-published evidence, not a blocking request to acquire new hardware data. | Domain snapshot when the data has not yet been interpreted by product policy |
| **Domain Snapshot** | The host-visible product read model produced by the Domain Runtime Library after applying calibration, freshness, protection, recovery, and status policy. | Raw measurement snapshot |

## Voltage control state

| Term | Definition | Aliases to avoid |
| --- | --- | --- |
| **Configured Target Voltage** | The host- or configuration-provided target voltage stored in channel settings. | Setpoint, output command |
| **Operational Target Voltage** | The voltage a channel is currently trying to reach. | Setpoint when the value may be derated |
| **Output Drive Level** | The immediate hardware drive value sent to output hardware, such as a DAC code or PWM duty cycle. | Output command, setpoint |
| **Output Enable** | The runtime gate that allows a channel output to be energized when the variant has such a control. | HW switch, run state |
| **Output Action** | A requested output-state transition such as enable, graceful disable, immediate disable, force output zero, or clamp. | Channel command when it mixes fault clearing, protection action when context matters |

## Protection and recovery

| Term | Definition | Aliases to avoid |
| --- | --- | --- |
| **Protection Mode** | Whether voltage or current protection is disabled, records Fault History only, or applies a Protection Output Action. | Limit mode |
| **Protection Output Action** | The Output Action applied when a protection event occurs and its Protection Mode applies an action. | Fault command |
| **Active Fault Block** | A current blocking condition that prevents enabling or automatic recovery until cleared according to domain rules. | Flag, latched flag |
| **Fault History** | A record that a fault event occurred and remains visible until explicitly cleared. | Latched fault when the condition is historical only |
| **Safe Band** | A configured hysteresis margin used before automatic recovery retries. | Tolerance when discussing retry eligibility |
| **Recovery Policy Mode** | The system-wide automatic recovery behavior selected for Automatic Mode. | Retry mode when it ignores Normal Mode behavior |

## Operating modes and protocols

| Term | Definition | Aliases to avoid |
| --- | --- | --- |
| **Normal Mode** | The operating mode where channel outputs are explicitly controlled by a remote frontend and protection disables require manual clear/re-enable. | Manual mode |
| **Automatic Mode** | The operating mode where configured channels may auto-start after boot and selected protection events may recover using bounded retry policy. | Auto mode when formal register names are needed |
| **Calibration Mode** | The volatile professional operating mode used during factory manufacturing and service debug. It bypasses the normal calibrated voltage-control loop and exposes raw DAC/ADC control surfaces while hard safety rails remain active. Calibration Mode is not an end-user mode and must not be persisted as a boot mode. | Factory mode when discussing the domain operating mode, raw debug mode when it hides calibration coefficient workflow |
| **Calibration Unlock** | The volatile two-step Modbus guard that must be completed before entering Calibration Mode. It prevents accidental entry by normal tools; it is not cryptographic authentication. | Password, login, authentication |
| **Calibration Output Enable** | The per-channel runtime gate that permits raw DAC output in Calibration Mode. Only one channel may have Calibration Output Enable active at a time. | Reusing Output Enable when the control path is raw calibration rather than normal product output |
| **Calibration Commit** | The per-channel action that persists approved calibration coefficients after factory or service tooling has calculated them externally. It saves calibration coefficients only, not raw debug state or temporary output values. | Channel save when the intent is specifically to persist calibration coefficients |
| **Protocol Adapter** | A frontend-specific translation layer that maps protocol requests to protocol-neutral domain commands and snapshots. | UI when discussing firmware module boundaries |
| **Modbus Adapter** | The Protocol Adapter that maps Modbus registers and exceptions to voltage-control domain operations. | Modbus domain, Modbus business logic |

## Relationships

- A **Voltage-Control Board** has one **Variant Profile**.
- A **Variant Profile** defines zero or more addressable **Channels**.
- A **Channel** has one **Configured Target Voltage** and one **Operational Target Voltage**.
- A **Channel** may have an **Output Enable** if the **Variant Profile** provides a separate enable control.
- A **Channel** may have a **Calibration Output Enable**, which is distinct from the normal **Output Enable** and only valid in **Calibration Mode**. Only one channel may have **Calibration Output Enable** active at a time.
- An **Output Action** changes runtime output state but does not change **Configured Target Voltage** unless the host writes that setting directly.
- **Output Actions** are rejected while in **Calibration Mode**; raw output is controlled via **Calibration Output Enable** instead.
- A **Protection Mode** determines whether a protection event records only **Fault History** or applies a **Protection Output Action**.
- An **Active Fault Block** blocks enable/retry; **Fault History** records that an event happened.
- **Automatic Mode** may retry selected future faults according to **Recovery Policy Mode**; **Normal Mode** always requires manual clear/re-enable after blocking protection events.
- **Calibration Mode** requires **Calibration Unlock** before entry. Hard safety rails remain active, but calibrated protection is bypassed while coefficients may be incomplete.
- A **Calibration Commit** persists calibration coefficients per channel; it does not save raw debug state, temporary output values, or the **Calibration Mode** operating state itself.
- A **Protocol Adapter** translates frontend-specific representations into domain operations without owning product behavior.
- A **Frontend Adapter** uses the **Domain Runtime Library** and must not call a **Virtual Channel Provider** directly.
- A **Virtual Voltage Channel** advertises **Channel Capabilities** to the **Domain Runtime Library**.
- A **Measurement Snapshot** is raw evidence from a **Virtual Channel Provider**; a **Domain Snapshot** is product state derived by the **Domain Runtime Library**.
- A **Runtime Config Snapshot** is published as a complete versioned intent, not as partial field updates.

## Example dialogue

> **Dev:** "If the host writes **Configured Target Voltage** to 0, is that the same as **Disable Graceful**?"

> **Domain expert:** "No. Writing **Configured Target Voltage** to 0 asks the channel to ramp toward 0 V, but it does not explicitly clear **Output Enable**. **Disable Graceful** ramps down and then disables **Output Enable**."

> **Dev:** "During **Automatic Mode** recovery, why do we also expose **Operational Target Voltage**?"

> **Domain expert:** "Because auto-derating may lower the **Operational Target Voltage** while the **Configured Target Voltage** remains unchanged for future enables."

> **Dev:** "If protection is **Flag Only**, does it create an **Active Fault Block**?"

> **Domain expert:** "No. It records **Fault History** only. An **Active Fault Block** is reserved for conditions that block enable or retry."

> **Dev:** "Can the **Modbus Adapter** clear unsafe hardware directly?"

> **Domain expert:** "No. The **Modbus Adapter** translates register writes into domain operations; the **Domain Runtime Library** protection policy decides whether a fault clear or output action is allowed."

> **Dev:** "Can a field tech change calibration coefficients while in **Normal Mode**?"

> **Domain expert:** "No. Calibration coefficient writes are rejected unless the domain is in **Calibration Mode**. The host must complete the two-step **Calibration Unlock**, enter **Calibration Mode**, write the coefficients, and then issue a **Calibration Commit** to persist them."

> **Dev:** "Does **Calibration Output Enable** mean a channel's normal output is active?"

> **Domain expert:** "No. **Calibration Output Enable** is a separate raw control gate that only exists in **Calibration Mode**. Normal **Output Actions** are rejected while in **Calibration Mode**, and **Calibration Output Enable** is cleared on exit."

> **Dev:** "Can the embedded shell sample a **Virtual Voltage Channel** directly for debugging?"

> **Domain expert:** "No. The shell is a **Frontend Adapter**. It reads a **Domain Snapshot** or submits a domain command; only the **Domain Runtime Library** talks to **Virtual Channel Providers**."

> **Dev:** "Why do we keep both **Measurement Snapshot** and **Domain Snapshot**?"

> **Domain expert:** "A **Measurement Snapshot** is raw evidence with generation and timestamp. A **Domain Snapshot** is calibrated product state after freshness, protection, and recovery policy have been applied."

## Flagged ambiguities

- "Setpoint" was used for several concepts; use **Configured Target Voltage**, **Operational Target Voltage**, or **Output Drive Level** depending on whether the value is stored configuration, runtime target, or immediate hardware drive.
- "Output command" was ambiguous; use **Output Action** for transitions and **Output Drive Level** for the immediate hardware drive value.
- "HW Switch" and "SW Switch" came from the old Modbus model; use **Output Enable** for the runtime gate and **Output Action** for enable/disable requests.
- "Flag" was ambiguous between historical telemetry and blocking state; use **Fault History** for recorded events and **Active Fault Block** for blocking conditions.
- "Limit mode" hid two decisions; use **Protection Mode** for detection behavior and **Protection Output Action** for the output-state response.
- "Active channel" and "unsupported channel" should not be separate protocol concepts in the initial implementation; unsupported access returns Modbus illegal address.
- "Factory mode" was used for **Calibration Mode**; use **Calibration Mode** for the domain operating mode to distinguish it from any higher-level factory test orchestrator.
- "Password" or "login" were used for the unlock sequence; use **Calibration Unlock** since the guard is not cryptographic authentication but a two-step accidental-entry barrier. The unlock is volatile and self-clearing.
- "Channel save" was used for persisting coefficients; use **Calibration Commit** when the intent is specifically to persist calibration coefficients, not general channel configuration.
- "Channel service" was overloaded between product policy and board hardware abstraction; use **Domain Runtime Library** for policy and **Virtual Voltage Channel** or **Virtual Channel Provider** for the board-implemented channel boundary.
- "Snapshot" was ambiguous; use **Measurement Snapshot** for raw channel evidence and **Domain Snapshot** for host-visible product state.
- "Frontend" should refer to user/host interaction surfaces; use **Frontend Adapter** for Modbus, shell, future CAN/TCP/IP, or local display/buttons.
