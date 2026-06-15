# Ubiquitous Language

## Board family and variants

| Term | Definition | Aliases to avoid |
| --- | --- | --- |
| **Voltage-Control Board** | A Jianwei board that controls one or more voltage output channels and reports voltage/current telemetry. | HVB when referring to the whole product family |
| **HVB Variant** | The current Jianwei high-voltage board variant used as the first implementation of the voltage-control architecture. | Treating HVB as the whole architecture |
| **Variant Profile** | A board-specific description of channel count, ranges, precision, safety capabilities, and hardware bindings. | Board config, hardware config when discussing product capabilities |
| **Channel** | One independently controlled voltage output path with its own target, telemetry, protection settings, and runtime state. | Output, rail, module when independence matters |

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
| **Protocol Adapter** | A frontend-specific translation layer that maps protocol requests to protocol-neutral domain commands and snapshots. | UI when discussing firmware module boundaries |
| **Modbus Adapter** | The Protocol Adapter that maps Modbus registers and exceptions to voltage-control domain operations. | Modbus domain, Modbus business logic |

## Relationships

- A **Voltage-Control Board** has one **Variant Profile**.
- A **Variant Profile** defines zero or more addressable **Channels**.
- A **Channel** has one **Configured Target Voltage** and one **Operational Target Voltage**.
- A **Channel** may have an **Output Enable** if the **Variant Profile** provides a separate enable control.
- An **Output Action** changes runtime output state but does not change **Configured Target Voltage** unless the host writes that setting directly.
- A **Protection Mode** determines whether a protection event records only **Fault History** or applies a **Protection Output Action**.
- An **Active Fault Block** blocks enable/retry; **Fault History** records that an event happened.
- **Automatic Mode** may retry selected future faults according to **Recovery Policy Mode**; **Normal Mode** always requires manual clear/re-enable after blocking protection events.
- A **Protocol Adapter** translates frontend-specific representations into domain operations without owning product behavior.

## Example dialogue

> **Dev:** "If the host writes **Configured Target Voltage** to 0, is that the same as **Disable Graceful**?"

> **Domain expert:** "No. Writing **Configured Target Voltage** to 0 asks the channel to ramp toward 0 V, but it does not explicitly clear **Output Enable**. **Disable Graceful** ramps down and then disables **Output Enable**."

> **Dev:** "During **Automatic Mode** recovery, why do we also expose **Operational Target Voltage**?"

> **Domain expert:** "Because auto-derating may lower the **Operational Target Voltage** while the **Configured Target Voltage** remains unchanged for future enables."

> **Dev:** "If protection is **Flag Only**, does it create an **Active Fault Block**?"

> **Domain expert:** "No. It records **Fault History** only. An **Active Fault Block** is reserved for conditions that block enable or retry."

> **Dev:** "Can the **Modbus Adapter** clear unsafe hardware directly?"

> **Domain expert:** "No. The **Modbus Adapter** translates register writes into domain operations; the domain and safety supervisor decide whether a fault clear or output action is allowed."

## Flagged ambiguities

- "Setpoint" was used for several concepts; use **Configured Target Voltage**, **Operational Target Voltage**, or **Output Drive Level** depending on whether the value is stored configuration, runtime target, or immediate hardware drive.
- "Output command" was ambiguous; use **Output Action** for transitions and **Output Drive Level** for the immediate hardware drive value.
- "HW Switch" and "SW Switch" came from the old Modbus model; use **Output Enable** for the runtime gate and **Output Action** for enable/disable requests.
- "Flag" was ambiguous between historical telemetry and blocking state; use **Fault History** for recorded events and **Active Fault Block** for blocking conditions.
- "Limit mode" hid two decisions; use **Protection Mode** for detection behavior and **Protection Output Action** for the output-state response.
- "Active channel" and "unsupported channel" should not be separate protocol concepts in the initial implementation; unsupported access returns Modbus illegal address.
