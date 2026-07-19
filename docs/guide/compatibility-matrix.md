# Board / Hardware Compatibility Matrix

Which firmware a board variant needs, and why. This is a current-state
reference table, not a history — for the release timeline see
[`../../CHANGELOG.md`](../../CHANGELOG.md). For how board variant/revision
identity works, see
[`version-management-guide.md`](version-management-guide.md) §3.

*(中文版本: none yet.)*

**Keep this table current**: add a row (or extend an existing one) every
time a new board variant or a real `board.yml` hardware revision is added
— see `version-management-guide.md` §3 for the mechanism. A stale
compatibility table is worse than none, since it actively misleads whoever
checks it next.

| Board variant | `VARIANT_ID` | HW revision | Min. protocol | Min. firmware tag | Notes |
|---|---|---|---|---|---|
| `jw_hvb` | 1 | rev A (0) — only revision that exists | 3.0+ | `firmware-v0.92.0` (first tagged release; untagged commits back to protocol 3.0 also work) | Original variant. 2-channel by default; 1-channel BOM population via `jw_hvb_1ch.overlay` (`version-management-guide.md` §3, design doc §5) |
| `jw_lvb` | 2 | rev A (0) — only revision that exists | **3.2+** — a client that only knows protocol ≤3.1 has no way to read `CURRENT_UNIT_EXP` and will misinterpret `MEASURED_CURRENT`/`CURRENT_LIMIT_THRESHOLD` by ~9 orders of magnitude (jw_lvb's amp-scale load currents vs. the pre-3.2 universal 0.1 nA/LSB assumption) — see [`modbus-reference.md`](modbus-reference.md) §3, §6 | `firmware-v0.92.0` (first tagged release) | 10-channel, fixed-voltage (no DAC) |

No board in this tree declares a real `board.yml` hardware revision yet —
every row above is "rev A (0)" by construction (`CONFIG_VC_BOARD_HW_REVISION`
default). When a board gets a second revision, add a row per revision here,
since different revisions can have different minimum-firmware requirements
(e.g. a revision-scoped `defconfig` value a host tool needs to know how to
interpret).
