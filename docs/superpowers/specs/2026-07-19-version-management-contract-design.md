# Version Management Contract — Design

**Status:** design approved, not yet implemented. This document defines the
contract; it does not itself change any code, register, Kconfig, or git tag.
Implementation is a separate, later plan.

## 1. Problem

This project has no coherent versioning story. Concretely, before this
document:

- Firmware version (`SYS_STATUS_FW_VERSION_HIGH/LOW` in `lib/sys_status/sys_uptime.c`)
  is a hardcoded stub (`0.1`), disconnected from git history or anything real.
- Board identity (`VC_VARIANT_ID`) is a single flat enum (1=HVB, 2=LVB) with
  no way to express a hardware revision, and no way to express "same
  architecture, different channel count" without conflating it with a real
  hardware change.
- Protocol version (`VC_PROTOCOL_MAJOR/MINOR`, currently 3.2) is the one
  dimension that already works — deliberately evolved, additive-only rules
  documented inline in `include/reg_store/modbus_view.def`.
- Host tools piggyback on the *whole repo's* git tag
  (`git describe --tags` in `tools/deploy_linux.sh`) — so one tag currently
  means "whatever commit the monorepo was at," conflating firmware,
  protocol, board definitions, and every host tool into a single number.

The root cause: firmware (a shared framework), board variants (physical
products built from that framework), the wire protocol (the contract
between firmware and host tools), and host tools (independently released
clients) are four things with genuinely different release cadences and
compatibility rules, currently sharing one undifferentiated version concept.

## 2. The five dimensions

| Dimension | What it identifies | Scheme | Source of truth | Bumped when |
|---|---|---|---|---|
| Board family | Organizational grouping ("HVB family" vs "LVB family") | Naming convention (`jw_*` prefix) — not a technical mechanism | Documentation / naming only | Never bumped — it's a label, not a version |
| Board variant | A distinct board architecture: driver set, bus protocol, which peripheral parts exist | Zephyr **board name** under `boards/jianwei/` | The board directory itself; reported via `VARIANT_ID` (existing register, reinterpreted — see §4) | A change that requires different DTS compatible-strings or Kconfig-selected drivers to compile |
| Hardware revision | A hardware iteration within one board variant that firmware/DTS topology can't see | Zephyr's native `board.yml` `revisions:` mechanism | `board.yml`, selected via `-b jw_hvb@B`; reported via new `BOARD_HW_REVISION` register (§4) | A change that only needs different revision-scoped `defconfig`/DTS values, with the identical compiled driver set |
| Firmware | The shared framework's own release, decoupled from which board(s) it's built for | SemVer `MAJOR.MINOR.PATCH` | git tag `firmware-vX.Y.Z`; embedded into `FW_VERSION` register at build time (§4, §6) | Any firmware code change reaches a release point |
| Protocol | The Modbus wire contract between firmware and host tools | `MAJOR.MINOR` (no patch — a wire contract has no bugfix tier) | `VC_PROTOCOL_MAJOR/MINOR` in `include/reg_store/reg_map.h` (already exists, unchanged) | MAJOR: breaking register/semantic change. MINOR: additive-only (new register in a previously-reserved slot) |
| Host tools | Each host-side deliverable, released on its own schedule | SemVer per tool/package | git tag `<tool-name>-vX.Y.Z` per deployable (§6) | That tool's own release point |

Two things are deliberately **not** version dimensions:

- **Channel count and capability flags** (`SUPPORTED_CHANNELS`,
  `CAPABILITY_FLAGS`, per-channel `CH_CAP_*`) are DTS-derived build data,
  already correctly modeled in `docs/guide/channel-capability-model.md`.
  They vary automatically and correctly whenever board variant or hardware
  revision changes (or, for channel count specifically, via a devicetree
  overlay within one variant — see §5). Giving them their own version
  number would be a duplicate, driftable source of truth.
- **On-device NVS config/calibration schema version** is a real, separate
  concern, already scoped as Tier 2 of
  `docs/superpowers/plans/2026-07-16-board-lifecycle-state-management.md`
  (deferred, designed but not built). It's a persistence-format detail
  nested inside "firmware," not a peer of the five dimensions above. This
  document cross-references it and does not re-design it.

## 3. Board identity: three levels

A single "hardware revision" scalar can't represent the real design space,
because hardware changes are not always sequential — some are parallel,
coexisting siblings (e.g. one variant using SPI ADC, another using I2C ADC,
both shipped, neither superseding the other), while others are strictly
linear iterations of one fixed design. Three levels, with a mechanical test
for which one a given hardware change belongs to:

**The test:** *Does this change require different DTS compatible-strings or
Kconfig-selected drivers to compile? → new board variant. Does it only
require different revision-scoped `defconfig`/DTS values with the identical
compiled driver set? → hardware revision. Does it only change a channel
count or capability flag with identical peripheral architecture? → neither;
it's build-time DTS data (§5), not an identity dimension.*

Worked examples:

1. **More channels, same architecture** (e.g. jw_hvb shipped as 1ch/2ch/4ch
   SKUs, identical DAC/ADC/MCU) — neither variant nor revision. A devicetree
   overlay selects how many identical channel nodes are enabled. See §5.
2. **A connected component changes** — DAC part, ADC part, the MCU itself,
   or any other peripheral wired to the MCU — **new board variant** (new
   board name). It changes which driver/Kconfig symbols must compile, so it
   needs its own DTS and defconfig, released independently, parallel to
   sibling variants.
3. **The analog measurement path changes, but the DAC/ADC parts themselves
   don't** (e.g. a divider network or isolation amplifier swap that only
   shifts a calibration-nominal constant) — **hardware revision** within the
   same variant. Firmware compiles identically; only revision-scoped
   `defconfig` values change (e.g. a new `VC_DEFAULT_MEASURED_V_CAL_K`).
4. **The bus protocol to a channel peripheral changes** (SPI→I2C→UART) —
   **new board variant**. Different compatible-string, different driver,
   firmware code path differs.

**Terminology note:** Zephyr's own build CLI has a *different*, narrower
`variant` qualifier (`board@revision/soc/variant`, e.g. `native_sim/native/64`)
used for build flavors of identical hardware. This document's "board
variant" is realized as a distinct Zephyr **board name** (what `jw_hvb` and
`jw_lvb` already are), not that CLI slot — don't confuse the two. "Board
variant" here matches `UBIQUITOUS_LANGUAGE.md`'s existing "Variant Profile"
term: *"a board-specific description of channel count, ranges, precision,
safety capabilities, and hardware bindings."*

Sequential vs. parallel is the key distinguishing property: board variants
are **parallel** (siblings coexist, none supersedes another — exactly what
Zephyr board names already support, no new mechanism needed). Hardware
revisions are **sequential** within one variant (default = latest — exactly
what `board.yml`'s `revisions:` mechanism is designed for).

## 4. Wire-level introspection registers

For host tools to auto-recognize what they're connected to without a
lookup step outside the protocol itself, the system input register block
(`include/reg_store/modbus_view.def`, offsets 0-15 used, 16-39 reserved)
needs one addition and one semantic correction:

| Register | Offset | Change |
|---|---|---|
| `PROTOCOL_MAJOR` / `PROTOCOL_MINOR` | 0, 1 | unchanged |
| `VARIANT_ID` | 2 | **semantic note only, no wire change**: this is the board *variant* (board name) identifier under this model — 1=jw_hvb, 2=jw_lvb. Board *family* stays wire-invisible (§2) |
| `FW_VERSION` | 10-11 | **repack**: currently an unused stub (`SYS_STATUS_FW_VERSION_HIGH/LOW = 0/1`); redefine to carry the real firmware SemVer (major.minor.patch). Exact bit-packing (e.g. 8/8/16) is an implementation decision, not fixed by this contract |
| `BOARD_HW_REVISION` | new, in the 16-39 reserved block | **new register**: integer index of the `board.yml` revision this image was built for (0=default/revA, 1=revB, …) |

Adding `BOARD_HW_REVISION` to a currently-reserved offset is a protocol
**MINOR** bump under the existing additive-only rule already documented at
the top of `modbus_view.def` — an ordinary application of the existing
protocol-versioning rule, not a special case.

Board variant *name* strings ("jw_hvb", human-readable hardware-revision
labels like "rev B") are **not** put on the wire. Register budget stays
numeric-only, consistent with how capability flags already work. Instead,
host tools carry their own small `variant_id → name` / `hw_revision → label`
lookup table, versioned and released alongside the host tools themselves
(§6) — extended whenever a new board variant or revision ships. A host tool
can then render, e.g.:

```
Connected: jw_hvb rev B  (HVB family)
Firmware:  v1.2.0        Protocol: v3.3
```

## 5. Channel count as a build parameter, not an identity dimension

`boards/jianwei/jw_hvb/jw_hvb.dts` declares 2 channel nodes (HV1, HV2),
each backed by real, documented hardware (`ref/jw_hvb/pin_map.md`,
`ref/jw_hvb/board_design.md`): a dedicated SPI bus + AD5541 DAC and a
dedicated 7-GPIO ADS1232 bit-bang ADC per channel. `VC_MAX_CHANNELS` is
`DT_CHILD_NUM_STATUS_OKAY(vc_controller)` — a direct count of enabled
channel nodes at build time. The base DTS stays the 2-channel (fully
populated) description; a 1-channel BOM population is expressed as an
overlay that disables HV2's DAC/ADC/logical channel:

```
boards/jianwei/jw_hvb/
    jw_hvb.dts              # base: HV1 + HV2, both status="okay" (today's
                             # 2ch board, fully populated)
    jw_hvb_1ch.overlay       # disables ads1232_hv2, dac_hv2, vc_ch1
```

Build — only the DTC overlay flag changes:

```sh
west build -b jw_hvb applications/psb_controller                                                # 2ch (base)
west build -b jw_hvb applications/psb_controller -- -DEXTRA_DTC_OVERLAY_FILE=jw_hvb_1ch.overlay  # 1ch
```

A 4-channel overlay is **not** included here — see the 2026-07-19
implementation note below.

Flashing is identical in both cases — `west flash` doesn't care about
channel count; only the build step differs. This is why channel count isn't
part of the identity hierarchy in §3: it's an ordinary, additive,
non-sequential DTS build parameter, the same idiomatic mechanism Zephyr uses
for any optional-hardware-population case.

(Writing the overlay file(s) is an implementation task — out of scope for
this contract per §1, but now has a concrete target shape.)

**2026-07-19 implementation note**: `jw_hvb_1ch.overlay` was implemented
(disables HV2's DAC/ADC/logical channel — full pin data was already
documented for HV1/HV2 in `ref/jw_hvb/pin_map.md` and
`ref/jw_hvb/board_design.md`). A `jw_hvb_4ch.overlay` was **not**
implemented: `board_design.md` §10 documents a real `SYS_MOD0`/`SYS_MOD1`
DIP switch with a 4-channel mode entry, but no HV3/HV4 SPI bus, ADS1232
GPIO set, or isolator part numbers are documented anywhere in this repo.
Writing `channel@2`/`channel@3` DTS nodes without that schematic data
would mean fabricating pin assignments. Blocked on real hardware
documentation for HV3/HV4 before a 4ch overlay can be written correctly.

## 6. Compatibility rules

**Host tool ↔ firmware, at connect time:** a host tool checks
`PROTOCOL_MAJOR == <the major it was built against>` and
`PROTOCOL_MINOR >= <the minor it was built against>`, and refuses to operate
(not just warn) on a mismatch. Standard SemVer client/server rule: an exact
major mismatch means the tool cannot correctly speak the wire format at all;
a firmware minor at or above what the tool expects means every register the
tool knows about is guaranteed present, with new unknown registers simply
unused. **Firmware version and hardware revision are read and displayed for
diagnostics only — they never gate host-tool behavior.** This is the direct
consequence of decoupling "firmware release" from "protocol contract":
tools depend on the contract, not on which framework release happens to be
running.

**Firmware ↔ board variant/revision:** one firmware version covers every
board variant and revision buildable from that source tree — there is no
per-variant firmware version. A `firmware-vX.Y.Z` tag identifies an exact
source commit; any board variant/revision can be built from it, and the
embedded `FW_VERSION` register is the same value regardless of which board
it was built for.

**Protocol ↔ firmware:** protocol version is not independently tagged (§7)
— it's a firmware-embedded constant, so its history is the firmware's
commit history. A firmware release may or may not bump the protocol version;
most won't.

## 7. Git tag conventions

Two tag families only. Board variants/revisions are permanent tree
artifacts (like Zephyr board ports) — never independently tagged. Protocol
version is not separately tagged (§6).

```
firmware-vX.Y.Z            # whole framework tree at a release point;
                            # covers every board variant/revision buildable
                            # from that commit
<tool-name>-vX.Y.Z          # one per host-tools deliverable:
psb_demo_tui-vX.Y.Z
psb_demo_cli-vX.Y.Z
psb_factory_tool-vX.Y.Z
psb_modbus_core-vX.Y.Z
```

Tags are annotated (`git tag -a`); the tag message serves as that
component's release notes (matches the pattern already used for the
existing `v0.91` tag) — no separate `CHANGELOG.md` file, to avoid a second,
driftable copy of the same information.

`tools/deploy_linux.sh` currently resolves version via
`git describe --tags --always` (repo-wide, picks up whatever tag is most
recent regardless of relevance). Under this scheme it must resolve
per-component instead, e.g. `git describe --tags --match 'psb_demo_tui-v*'`
when packaging that tool — an implementation follow-up, not done by this
document.

The existing `v0.90`/`v0.91` tags predate this scheme and are left as
historical markers, not retagged. The first tags under the new scheme are
fresh starts (e.g. `firmware-v1.0.0`), cut whenever a real release point is
next wanted — this document does not cut them.

## 8. Non-goals

- No code, Kconfig, register, or git tag changes are made by this document.
  §4 and §5 describe target shapes for a later implementation plan.
- NVS/config schema versioning is not designed here — see
  `docs/superpowers/plans/2026-07-16-board-lifecycle-state-management.md`
  Tier 2.
- Persisted per-unit provisioning/lifecycle state (unprovisioned →
  calibrated → production-locked) is not designed here — see the same
  plan's Tier 3 sketch. That's a question of *where a physical unit is in
  its life*, orthogonal to which version of firmware/protocol/board it runs.
- Exact `FW_VERSION` bit-packing and the precise `BOARD_HW_REVISION`
  Kconfig plumbing are left to the implementation plan.
