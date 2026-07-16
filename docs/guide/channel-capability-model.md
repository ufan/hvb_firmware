# Channel Capability Model

This document is for anyone **extending this repo**: adding a new board
variant, a new capability-gated register, or a new host tool. It defines
what each `CH_CAP_*` bit means, exactly which behaviors it turns on or off,
and every place in the codebase that must independently agree on that
before a channel behaves correctly end to end.

It is not the wire-protocol reference — for the Modbus-exception-level view
aimed at third-party client implementers, see
[`modbus-reference.md` §12](modbus-reference.md#12-capability-flags). This
document goes one level deeper: the firmware-internal behavioral contract
that §12's register-guard table is generated from.

*(中文版本: none yet — see modbus-reference.zh.md for the wire-level
translation.)*

## Source of truth

The bits are defined in
[`include/dt-bindings/voltage_control/capabilities.h`](../../include/dt-bindings/voltage_control/capabilities.h).
The register-level gate table in this doc mirrors
`vc_catalog_supported()` in
[`lib/voltage_control/vc_runtime.c`](../../lib/voltage_control/vc_runtime.c)
verbatim. **If you change one, change the other in the same commit** — this
doc existing does not replace reading that function; it exists so you don't
have to reverse-engineer it from scratch every time, and so the five other
places listed in §4 have one page to check their behavior against instead
of independently re-deriving it (which is exactly how past bugs here have
happened — see §6).

## 1. The five capability bits

| Bit | Mask | Name | One-line meaning |
|---:|---:|---|---|
| 0 | `0x0001` | `CH_CAP_OUTPUT_ENABLE` | The channel's output can be turned on/off at all |
| 1 | `0x0002` | `CH_CAP_RAW_OUTPUT_DRIVE` | The channel has a DAC — output level is settable, not just on/off |
| 2 | `0x0004` | `CH_CAP_VOLTAGE_MEASUREMENT` | The channel has an ADC input for voltage sensing |
| 3 | `0x0008` | `CH_CAP_CURRENT_MEASUREMENT` | The channel has an ADC input for current sensing |
| 4 | `0x0010` | `CH_CAP_HARDWARE_STATUS` | Reserved — see §5 |

A channel's capability set is a DTS property (`capabilities = <(...)>`,
bitwise-OR'd flags) on its `jianwei,*-vc-channel` node, fixed at build time
per board. It never changes at runtime. See
`boards/jianwei/jw_hvb/jw_hvb.dts` and `boards/jianwei/jw_lvb/jw_lvb.dts`
for real examples, and §7 below for what each board actually declares.

### What each bit does *not* imply

These are the mistakes this model exists to prevent — each was a real bug
found this session before this doc existed:

- **`OUTPUT_ENABLE` is not "has a DAC."** A channel can be switchable
  on/off (`OUTPUT_ENABLE` set) with no way to set an output *level*
  (`RAW_OUTPUT_DRIVE` absent) — LVB's channels 1–9 are exactly this. Gating
  DAC-only UI (target voltage, ramp settings) on `OUTPUT_ENABLE` instead of
  `RAW_OUTPUT_DRIVE` is wrong and was a real bug (`tools/psb_demo_app/tui/tab_monitor.h`,
  fixed this session — invisible on HVB where both bits always coincide,
  wrong on LVB where they don't).
- **`RAW_OUTPUT_DRIVE` does not imply `OUTPUT_ENABLE`.** A DAC channel that
  lacks `OUTPUT_ENABLE` would be a DAC that's locked permanently on — not a
  combination any current board uses, but nothing in the firmware assumes
  otherwise, so don't assume it either.
- **`OUTPUT_ENABLE` absent — regardless of `RAW_OUTPUT_DRIVE` — means the
  channel is locked always-on.** Explicit `DISABLE_*` commands are refused
  outright (§3), and so is any config write that would change its startup
  enabled state. This is the mechanism behind LVB's channel 0 ("always on,
  cannot be disabled" — a literal product requirement, not a fixed GPIO
  with no software path).
- **`VOLTAGE_MEASUREMENT`/`CURRENT_MEASUREMENT` are independent of output
  control entirely.** A channel can measure without driving anything (pure
  sense channel) or drive without measuring. Don't assume a channel that
  can be enabled/disabled also reports current, or vice versa — always
  check the specific bit for the specific field you're reading.

## 2. Register-level gate table

This is the authoritative mapping from `vc_catalog_supported()` — the first
enforcement layer any Modbus/shell register access goes through. A register
whose guard capability is absent returns `REG_UNSUPPORTED`, which the
Modbus adapter turns into exception `0x02` (Illegal Data Address) and the
shell turns into an error line. It behaves exactly like an out-of-range
channel — not a "reads as zero" fallback.

| Register field(s) | Required capability |
|---|---|
| `MEASURED_VOLTAGE`, `RAW_ADC_VOLTAGE`, `MEASURED_V_CAL_K`, `MEASURED_V_CAL_B` | `VOLTAGE_MEASUREMENT` |
| `MEASURED_CURRENT`, `RAW_ADC_CURRENT`, `MEASURED_I_CAL_K`, `MEASURED_I_CAL_B`, `CURRENT_PROTECTION_MODE`, `CURRENT_PROT_OUT_ACTION`, `CURRENT_LIMIT_THRESHOLD` | `CURRENT_MEASUREMENT` |
| `CFG_TARGET_VOLTAGE`, `RAMP_UP_STEP`, `RAMP_UP_INTERVAL`, `RAMP_DOWN_STEP`, `RAMP_DOWN_INTERVAL`, `OUTPUT_CAL_K`, `OUTPUT_CAL_B`, `CAL_OUTPUT_ENABLE`, `CAL_DAC_CODE` | `RAW_OUTPUT_DRIVE` |
| `AUTO_DERATE_STEP` | `RAW_OUTPUT_DRIVE` **and** `VOLTAGE_MEASUREMENT` |
| `CFG_OUTPUT_ENABLED` | `RAW_OUTPUT_DRIVE` **absent** and `OUTPUT_ENABLE` present |
| `CAL_SAMPLE_CMD` | `VOLTAGE_MEASUREMENT` **or** `CURRENT_MEASUREMENT` |
| `CAL_COMMIT_CMD` | `RAW_OUTPUT_DRIVE` **or** `VOLTAGE_MEASUREMENT` **or** `CURRENT_MEASUREMENT` (any one) |
| Everything else (`STATUS_BITS`, `OUTPUT_ACTION`, `FAULT_CMD`, `PARAM_ACTION`, `RECOVERY_POLICY_MODE`, `AUTO_RETRY_*`, `CURRENT_SAFE_BAND_PCT`, fault/timestamp fields, etc.) | none — always accessible |

`CFG_OUTPUT_ENABLED` is the one entry worth pausing on: it requires
`RAW_OUTPUT_DRIVE` to be **absent**, not present. This is intentional and
mirrors §3's config-validation rule — a DAC channel expresses its
AUTOMATIC-mode startup intent through `CFG_TARGET_VOLTAGE` (zero = off,
nonzero = on-at-that-level); `CFG_OUTPUT_ENABLED` is the equivalent field
for channels that have no target to speak of. The two are mutually
exclusive by capability, on purpose — never both reachable on the same
channel.

## 3. Behavioral rules beyond register gating

Four more capability-driven rules exist that are *not* expressed as a
simple per-register table, because they change **what a write means**
rather than whether it's reachable at all:

1. **`validate_capability_config()`** (`vc_channel.c`) — a whole-config
   consistency check run whenever the full channel config is replaced
   (NVS load, factory-reset), independent of the individual per-field
   register-write checks in `vc_channel_set_field()`. It rejects:
   - Any nonzero `configured_target_voltage`, or a changed ramp step/interval,
     on a channel without `RAW_OUTPUT_DRIVE`.
   - Any changed `configured_output_enabled` on a channel *with*
     `RAW_OUTPUT_DRIVE` (that channel's intent is expressed via target
     voltage only — see §2).
   - Any changed `configured_output_enabled` on a channel without
     `OUTPUT_ENABLE` (locked-always-on channels can't have their *startup*
     state flipped off either, closing a loophole around the live-disable
     refusal in rule 2 below).
   - Any changed current-protection fields, or a nonzero protection mode,
     on a channel without `CURRENT_MEASUREMENT`.
   - Any changed `auto_derate_step` on a channel missing `RAW_OUTPUT_DRIVE`
     or `VOLTAGE_MEASUREMENT`.

2. **`vc_channel_output_action()`** (`vc_channel.c`) has two independent
   capability rules on top of the register gate:
   - Without `RAW_OUTPUT_DRIVE`, `DISABLE_GRACEFUL` and `DISABLE_IMMEDIATE`
     are silently coerced to `DISABLE_FORCE` — there's no ramp to perform
     when there's no DAC to ramp.
   - Without `OUTPUT_ENABLE`, **any** explicit disable command
     (`DISABLE_GRACEFUL`, `DISABLE_IMMEDIATE`, or `DISABLE_FORCE`) is
     rejected outright with `VC_ERR_UNSUPPORTED_CAPABILITY` — this is what
     "locked always-on" actually means at the command level. This only
     gates the user-facing command path; internal fault/overcurrent
     protection (`apply_protection_action()`) is untouched by this check,
     so hardware protection still works on a locked-always-on channel —
     "can't be turned off by a client" is not the same guarantee as "can't
     ever trip for safety."

3. **The AUTOMATIC-mode auto-enable invariant** (`vc_controller.c`, on
   every transition into `AUTOMATIC` and at boot) enables every non-faulted
   channel that "wants" output — but which config field expresses "wants"
   depends on capability: `configured_target_voltage != 0` for
   `RAW_OUTPUT_DRIVE` channels, `configured_output_enabled` for everything
   else. The two are mutually exclusive by construction (rule 1 above
   prevents both being independently meaningful on the same channel), so
   this branch doesn't change existing DAC-channel behavior — it only adds
   the second path for channels that never had a target voltage to check.

4. **Register catalog reads** go through the same `vc_catalog_supported()`
   gate as writes (§2) — it's checked once per access regardless of
   direction, so there's no read/write asymmetry to account for.

## 4. Everywhere this has to be re-derived correctly

The firmware enforces all of the above independently in five places
(`vc_runtime.c`'s catalog gate, `vc_channel_set_field()`'s inline checks,
`validate_capability_config()`, `vc_channel_output_action()`, and
`vc_controller.c`'s mode-transition handler) — and every host tool that
touches channel config has to *agree* with those five without being able
to ask the firmware "is this allowed" ahead of time for anything beyond a
single register probe. As of this writing, that's:

- `tools/psb_modbus_core` — `ChannelConfig`/`ChannelCalConfig` read batching
  (`psb_modbus_client.cpp`) has to request only capability-supported ranges
  or a single unsupported register in a batch fails the whole FC03/FC04
  transaction.
- `tools/psb_demo_app/cli` — capability pre-checks before `voltage`/
  `enable-cfg`, and the capability-branched display in `channel <n> config`.
- `tools/psb_demo_app/tui` — same branching, rendered as widget visibility
  instead of text.
- `tools/psb_factory_tool/repl` — capability pre-checks before
  `target`/`enable-cfg`/`cal enable`/`cal disable`/`cal dac`/`cal coeff`.
- `tools/board_test/board_test.sh` — asserts the expected accept/reject
  outcome of every write against a live board, per channel, from its
  capability flags.

**When you add a new capability-gated register or a new capability bit**:
update `vc_catalog_supported()` first, then update this doc's tables, then
walk the list above — every one of those places has its own copy of
"does this channel support X" logic that needs the same update, and none
of them will fail loudly if you miss one (a missed CLI check just means a
confusing raw Modbus error instead of a clear one; a missed TUI check
means a control renders where it shouldn't, harmlessly wrong until someone
clicks it).

## 5. `CH_CAP_HARDWARE_STATUS`

Defined but currently unused beyond a capability-summary display string in
`vc_shell.c`. No board sets this bit today, and no register or command is
gated on it. It's reserved for future hardware-fault-evidence reporting
(distinct from the fault/status bits already exposed unconditionally via
`STATUS_BITS`/`ACTIVE_FAULT_CAUSE`). Treat it as forward-declared, not yet
load-bearing.

## 6. Why this document exists

Every host tool listed in §4 had to independently re-derive the exact
capability semantics above by reading `vc_catalog_supported()` from
scratch. Several didn't get it exactly right the first time — not from
carelessness, but because the semantics were only ever written down once,
in a C `switch` statement, with no single place summarizing the *intent*
behind each branch. 

If you're adding variant #3, start from this doc's tables, update
them as part of your change, and this list should stay this short.

## 7. Worked examples — current variants

**HVB (`variant_id=1`), all channels**: `OUTPUT_ENABLE | RAW_OUTPUT_DRIVE |
VOLTAGE_MEASUREMENT | CURRENT_MEASUREMENT` (`0x000F`) — full-capability DAC
channels, every register and command in §2/§3 reachable.

**LVB (`variant_id=2`)**:
- Channel 0: `VOLTAGE_MEASUREMENT | CURRENT_MEASUREMENT` (`0x000C`) — no
  `OUTPUT_ENABLE`, no `RAW_OUTPUT_DRIVE`. Locked always-on (§3 rule 2),
  fixed-voltage with no way to configure a startup state at all (§2's
  `CFG_OUTPUT_ENABLED` row requires `OUTPUT_ENABLE`, which this channel
  lacks) — it measures, and nothing else is configurable about it.
- Channels 1–9: `OUTPUT_ENABLE | VOLTAGE_MEASUREMENT | CURRENT_MEASUREMENT`
  (`0x000D`) — switchable fixed-voltage channels. `CFG_OUTPUT_ENABLED` is
  their startup-policy register; `CFG_TARGET_VOLTAGE`/ramp/derate/output
  calibration are all correctly unreachable (no `RAW_OUTPUT_DRIVE`).

No current board sets `CH_CAP_HARDWARE_STATUS`.
