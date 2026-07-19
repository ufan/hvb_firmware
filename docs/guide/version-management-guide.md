# Version & Release Management Guide — Jianwei Voltage-Control Boards

This is the practical, day-to-day companion to
[`docs/superpowers/specs/2026-07-19-version-management-contract-design.md`](../superpowers/specs/2026-07-19-version-management-contract-design.md)
(the design doc — read that for the *why*; this doc is the *how*). It's for
anyone who needs to answer "what version is this board/tool running?", cut
a release tag, or add a new board variant to the lookup table.

*(中文版本: none yet.)*

Related: [`modbus-reference.md`](modbus-reference.md) §1/§6 (the
`Protocol`/`FW Version`/`Variant ID`/`Board HW Revision` wire registers
this doc explains the meaning of), [`channel-capability-model.md`](channel-capability-model.md)
(capability flags — a related but distinct concept from board identity).

---

## 1. The five dimensions, at a glance

| Dimension | Scheme | Where it lives | Tagged in git? |
|---|---|---|---|
| Board family | Label only (`jw_*` naming) | Documentation/naming convention | No |
| Board variant | Zephyr board name | `boards/jianwei/<name>/` | No |
| Hardware revision | Zephyr `board.yml` `revisions:` | `board.yml`, `BOARD_HW_REVISION` register | No |
| Firmware | SemVer `MAJOR.MINOR.PATCH` | `FW_VERSION` register, embedded via git tag at build time | **Yes** — `firmware-vX.Y.Z` |
| Protocol | `MAJOR.MINOR` | `VC_PROTOCOL_MAJOR/MINOR` in `include/reg_store/reg_map.h` | No (embedded in firmware, versioned via firmware's own history) |
| Host tools | SemVer per tool | Each tool's own build | **Yes** — `<tool-name>-vX.Y.Z` |

Only **firmware** and **host tools** get git tags. Board identity (family/
variant/revision) and protocol version are reported facts, not release
artifacts — see §3.

---

## 2. Checking what's currently running

**On a connected board** (illustrative — CLI's `info` command, TUI's
System tab, and the factory REPL's `info` command each show the same three
facts with slightly different labels):

```
Protocol:    3.3
Variant:     jw_hvb (HVB family, rev A)
FW Version:  v0.92.0
```

Under the hood this comes from four system input registers
(`modbus-reference.md` §6, offsets 0/1/2/10-11/16), decoded host-side by
`tools/psb_modbus_core/board_catalog.h` (`variantName`, `variantFamily`,
`hwRevisionLabel`) and `register_map.h`'s `formatFwVersion`. The wire
protocol itself stays numeric-only — the human-readable names are a
host-tool-side lookup, not something the firmware sends as a string.

**In the repo, for the currently checked-out commit:**

```sh
# Firmware version this commit would build
git describe --tags --match "firmware-v*" --always --dirty

# A specific host tool's version this commit would build
git describe --tags --match "psb_demo_tui-v*" --always --dirty
git describe --tags --match "psb_demo_cli-v*" --always --dirty
git describe --tags --match "psb_factory_tui-v*" --always --dirty
```

Each resolves independently — cutting a tag for one tool doesn't move the
others. If no matching tag is reachable, you get an abbreviated commit hash
(firmware) or `dev` (deploy scripts) as the fallback — never a build
failure.

**All tags in the repo, with their release notes:**

```sh
git tag -l -n99 | sort -V
```

---

## 3. Board identity: family, variant, revision

Three levels — full rationale and the mechanical test for classifying a
hardware change lives in the design doc §3; the short version:

| Level | Example | Mechanism | Sequential or parallel? |
|---|---|---|---|
| Family | "HVB family" | Naming convention only | N/A |
| Variant | `jw_hvb` (`VARIANT_ID=1`), `jw_lvb` (`VARIANT_ID=2`) | Distinct Zephyr board name | Parallel — siblings, neither supersedes the other |
| Hardware revision | `rev A` (index 0) | `board.yml` `revisions:` → `BOARD_HW_REVISION` register | Sequential — default is always the latest |

**Adding a new board variant**: update `tools/psb_modbus_core/board_catalog.h`'s
`variantName()`/`variantFamily()` switch statements — that's the one place
host tools need to learn the new name. The firmware-side `VC_VARIANT_ID`
Kconfig (documented in `lib/voltage_control/Kconfig`) is the numeric source
of truth; `board_catalog.h` must stay in sync with it (same discipline as
`channel-capability-model.md`'s capability-flag table).

**Adding a real hardware revision**: no board in this tree declares
`board.yml` `revisions:` yet — every board defaults to
`CONFIG_VC_BOARD_HW_REVISION=0`. When one is added, extend
`board_catalog.h`'s `hwRevisionLabel()` if the generic `0=rev A, 1=rev B, ...`
scheme isn't descriptive enough for that variant.

**Channel count is not part of this hierarchy** — it's a devicetree build
parameter (e.g. `jw_hvb_1ch.overlay`), not a board variant or revision. See
design doc §5.

---

## 4. Board / hardware compatibility table

Which firmware a board variant needs, and why. **Keep this table current**:
add a row (or extend an existing one) every time a new board variant or a
real `board.yml` hardware revision is added — see §3 for the mechanism. A
stale compatibility table is worse than none, since it actively misleads
whoever checks it next.

| Board variant | `VARIANT_ID` | HW revision | Min. protocol | Min. firmware tag | Notes |
|---|---|---|---|---|---|
| `jw_hvb` | 1 | rev A (0) — only revision that exists | 3.0+ | `firmware-v0.92.0` (first tagged release; untagged commits back to protocol 3.0 also work) | Original variant. 2-channel by default; 1-channel BOM population via `jw_hvb_1ch.overlay` (§3, design doc §5) |
| `jw_lvb` | 2 | rev A (0) — only revision that exists | **3.2+** — a client that only knows protocol ≤3.1 has no way to read `CURRENT_UNIT_EXP` and will misinterpret `MEASURED_CURRENT`/`CURRENT_LIMIT_THRESHOLD` by ~9 orders of magnitude (jw_lvb's amp-scale load currents vs. the pre-3.2 universal 0.1 nA/LSB assumption) — see `modbus-reference.md` §3, §6 | `firmware-v0.92.0` (first tagged release) | 10-channel, fixed-voltage (no DAC) |

No board in this tree declares a real `board.yml` hardware revision yet —
every row above is "rev A (0)" by construction (`CONFIG_VC_BOARD_HW_REVISION`
default). When a board gets a second revision, add a row per revision here,
since different revisions can have different minimum-firmware requirements
(e.g. a revision-scoped `defconfig` value a host tool needs to know how to
interpret).

---

## 5. Host-tool compatibility behavior

Every host tool (CLI, TUI, factory REPL, both GUIs) connects through the
shared `PsbModbusClient::connect()`, which enforces:

- **Protocol major must match exactly.** A firmware reporting a different
  major version speaks an incompatible wire format — the tool refuses to
  connect, it does not attempt to proceed with a warning.
- **Protocol minor must be `>=`** what the tool was built against. A newer
  firmware may expose registers the tool doesn't know about, which is
  harmless; an older firmware might be missing something the tool needs.

Example refusal message (firmware protocol 3.1, tool built against 3.3):

```
firmware protocol v3.1 is incompatible with this tool (requires v3.3 or newer, same major version)
```

**Firmware version and hardware revision never gate this check** — they're
read and displayed for diagnostics only (§2). This is deliberate: a tool
shouldn't refuse to talk to a board just because it's running an older
*firmware release* that still speaks the same *protocol*.

---

## 6. Cutting a release tag

### Firmware

```sh
git tag -a firmware-vX.Y.Z -m "$(cat <<'EOF'
firmware-vX.Y.Z

Highlights since <previous tag>:
- ...

This tag marks a firmware release point, independent of host-tools
and board-variant releases.
EOF
)"
```

One firmware tag covers every board variant/revision buildable from that
commit — there's no per-variant firmware version (design doc §6). Bump
**MAJOR** for a breaking behavior/protocol change, **MINOR** for
backward-compatible additions, **PATCH** for fixes.

### A host tool

```sh
git tag -a psb_demo_tui-vX.Y.Z -m "psb_demo_tui-vX.Y.Z — <one-line summary>"
```

Repeat per tool (`psb_demo_cli`, `psb_factory_tui`, ...) — each is tagged
independently, on its own release cadence. `psb_modbus_core` (the shared
client library) is **not** tagged separately — it's never distributed
standalone, only linked into the tools above.

### After tagging

Tags are local until pushed:

```sh
git push origin firmware-vX.Y.Z psb_demo_tui-vX.Y.Z ...
```

`tools/deploy_linux.sh`/`deploy_windows.sh` automatically pick up each
tool's own tag the next time they're run (`git describe --tags --match
"<tool>-v*"` per tool, not a shared repo-wide version) — no script changes
needed after tagging.

---

## 7. Where the mechanism lives (file map)

| Concern | File(s) |
|---|---|
| Firmware version generation | `cmake/fw_version.cmake`, `cmake/fw_version.h.in` |
| Firmware version register | `lib/sys_status/sys_uptime.c` (`FW_VERSION`), `lib/sys_status/sys_status_shell.c` (shell display) |
| Board HW revision register | `lib/voltage_control/Kconfig` (`CONFIG_VC_BOARD_HW_REVISION`), `include/reg_store/vc_global_regs.def`, `include/reg_store/modbus_view.def`, `lib/voltage_control/vc_runtime.c` |
| Protocol version constants | `include/reg_store/reg_map.h` (`VC_PROTOCOL_MAJOR/MINOR`) |
| Channel-count build overlays | `boards/jianwei/jw_hvb/jw_hvb_1ch.overlay`, documented in `README.md` |
| Host-tool version resolution | `tools/deploy_linux.sh`, `tools/deploy_windows.sh` (`resolve_version()`) |
| Host-tool compat-check | `tools/psb_modbus_core/psb_modbus_client.cpp` (`connect()`), `register_map.h` (`protocolCompatible()`) |
| Board/variant/revision lookup table | `tools/psb_modbus_core/board_catalog.h` |
| Firmware version formatting | `tools/psb_modbus_core/register_map.h` (`formatFwVersion()`) |

---

## 8. Version history

**Keep this table current**: add a row every time a new tag is cut (§6),
in the same commit/PR as the tag itself. Newest first.

| Tag | Date | Component | Highlights |
|---|---|---|---|
| `firmware-v0.92.0` | 2026-07-19 | Firmware | First release under the new scheme. Version management contract implemented: `FW_VERSION` now real SemVer (sourced from this tag family at build time), new `BOARD_HW_REVISION` register, protocol 3.2→3.3, `jw_hvb_1ch.overlay` for 1-channel BOM populations. |
| `psb_demo_tui-v1.0.0` | 2026-07-19 | Host tool | First independently-tagged release. Variant/family/revision display, real SemVer firmware-version display, protocol compat-check (refuses to connect on mismatch, via the shared `PsbModbusClient`). |
| `psb_demo_cli-v1.0.0` | 2026-07-19 | Host tool | Same as `psb_demo_tui-v1.0.0` (shared `psb_modbus_core` changes). |
| `psb_factory_tui-v1.0.0` | 2026-07-19 | Host tool | Same as `psb_demo_tui-v1.0.0`. |
| `v0.91` | 2026-07-18 | Legacy whole-repo | jw_lvb board bring-up (hardware fixes, current calibration, ch5 default-off policy, zero-offset tool); `hvb_*`→`psb_*` host-tool rename; protocol v3.1 (calibration gain as decimal mantissa×10^exp); `jw_hvb_selfcal` no-instrument calibration tool. Predates the new tag scheme — left as a historical marker, never retagged. |
| `v0.90` | 2026-07-03 | Legacy whole-repo | Pre-release baseline. FW version `0x00000001`, protocol `3.0`. Software/repo release point only, not itself a firmware or protocol bump. Predates the new tag scheme. |

For anything not in this table (e.g. exactly which commit first shipped
protocol 3.2), `git log -S <symbol> -- <file>` against the real source is
authoritative — this table only promises to track tagged release points,
not to reconstruct untagged history.
