# Changelog

Every tagged release point across this repo's independently-versioned
components — firmware (`firmware-vX.Y.Z`) and each host tool
(`<tool-name>-vX.Y.Z`) — plus the legacy whole-repo tags that predate that
scheme. See
[`docs/guide/version-management-guide.md`](docs/guide/version-management-guide.md)
for what these tags mean and how to cut a new one.

**Keep this file current**: add a row every time a new tag is cut, in the
same commit/PR as the tag itself. Newest first.

| Tag | Date | Component | Highlights |
|---|---|---|---|
| `psb_demo_tui-v1.3.0` | 2026-07-24 | Host tool | Core-backed message center for action-scoped status/log policy; TUI board and group status bars now clear stale warning/error messages on new actions while preserving message history, with GUI status adapter wired through the same core mechanism. |
| `psb_modbus_core-v1.0.0` | 2026-07-24 | Host library | First explicit core-library tag, covering topology/group configuration services and the new action-scoped `MessageCenter` used by TUI/GUI status handling. |
| `psb_demo_tui-v1.2.0` | 2026-07-24 | Host tool | Core-backed topology/group rules and CLI endpoint resolution, TUI test split from core tests, architecture diagrams, board menu cleanup: nickname identity, connected channel/variant summary, and duplicate mode/save controls removed from the board menu. |
| `psb_demo_tui-v1.1.0` | 2026-07-23 | Host tool | Topology/group workflow release: dedicated app menu bar and sidebar polish, topology switching fixes, group-channel naming rules, alias sync between board and group views, group wizard channel picker, readable uptime, minimum-width guard, updated TUI guide. |
| `firmware-v0.92.0` | 2026-07-19 | Firmware | First release under the new scheme. Version management contract implemented: `FW_VERSION` now real SemVer (sourced from this tag family at build time), new `BOARD_HW_REVISION` register, protocol 3.2→3.3, `jw_hvb_1ch.overlay` for 1-channel BOM populations. |
| `psb_demo_tui-v1.0.0` | 2026-07-19 | Host tool | First independently-tagged release. Variant/family/revision display, real SemVer firmware-version display, protocol compat-check (refuses to connect on mismatch, via the shared `PsbModbusClient`). |
| `psb_demo_cli-v1.0.0` | 2026-07-19 | Host tool | Same as `psb_demo_tui-v1.0.0` (shared `psb_modbus_core` changes). |
| `psb_factory_tui-v1.0.0` | 2026-07-19 | Host tool | Same as `psb_demo_tui-v1.0.0`. |
| `v0.91` | 2026-07-18 | Legacy whole-repo | jw_lvb board bring-up (hardware fixes, current calibration, ch5 default-off policy, zero-offset tool); `hvb_*`→`psb_*` host-tool rename; protocol v3.1 (calibration gain as decimal mantissa×10^exp); `jw_hvb_selfcal` no-instrument calibration tool. Predates the new tag scheme — left as a historical marker, never retagged. |
| `v0.90` | 2026-07-03 | Legacy whole-repo | Pre-release baseline. FW version `0x00000001`, protocol `3.0`. Software/repo release point only, not itself a firmware or protocol bump. Predates the new tag scheme. |

For anything not in this table (e.g. exactly which commit first shipped
protocol 3.2), `git log -S <symbol> -- <file>` against the real source is
authoritative — this file only promises to track tagged release points,
not to reconstruct untagged history.
