# Board Lifecycle State Management — Roadmap

**Status:** Tier 1 implemented (2026-07-16). Tiers 2 and 3 are deferred,
documented here so the requirement and design thinking aren't lost.

## Problem

While verifying the calibration-format rework (v3.1, decimal-exponent
gain), `psb_demo_tui` showed CH8 defaulting to `Off` on a `jw_lvb` board
that should have every switchable channel (`VC_DEFAULT_OUTPUT_ENABLED=y`)
default to `On`. Root cause: `west flash` alone only programs the
application image. Every board variant's DTS defines a separate NVS
`storage_partition` for persisted config/calibration
(`boards/jianwei/jw_lvb/jw_lvb.dts:429`, offset `0x7e000`, 8 KB) —
deliberately outside the image region, so it survives ordinary firmware
updates in the field. That's correct behavior for a deployed unit, but
during bring-up/testing it means a plain reflash can leave a channel's
config at whatever some earlier test session left it at, silently
indistinguishable from a genuine factory-default boot.

This generalizes to a real gap: **this project has no concept of where a
given physical board is in its lifecycle** (never-configured vs.
mid-development vs. calibrated vs. "should be treated as production data,
don't touch"), and no tooling distinguishes "read the board's actual
current state" from "read the board's true factory-default state." Both
questions are legitimate and answered differently, and today only the
second one (factory default) can even be answered reliably, and only by
mass-erasing first.

A second, related gap surfaced during the calibration-format work itself
and was explicitly accepted as a known risk at the time (see
`docs/guide/channel-capability-model.md` and the calibration-format
session): `vc_channel_cal_config`'s NVS persistence
(`lib/voltage_control/vc_storage_settings.c`) has no schema version or
migration path. A struct-layout change silently discards old NVS data
(fails a size check, falls back to defaults) with **no operator-visible
signal** distinguishing "never calibrated" from "calibration lost by this
firmware upgrade." That's the same root problem as the CH8 bug — the
system can't tell a truly-fresh board apart from one whose persisted state
just became stale — approached from the firmware-upgrade angle instead of
the reflash angle.

## Strategy: three tiers, deliberately not built all at once

The project currently has 1-2 bench prototypes, no manufacturing line, no
fleet. A full device-lifecycle framework (provisioning database, serial
numbers, cloud-tracked calibration certs) would be pure overhead right now.
But *some* discipline is needed immediately, because the failure mode
already happened once. The right amount of investment scales with how many
units and how automated the process around them is:

| Tier | What | When it's worth it |
|---|---|---|
| 1 | Deterministic bring-up tooling (erase+flash+verify as one step) | Now — near-zero cost, directly fixes the bug that just happened |
| 2 | NVS schema version + explicit stale-state handling | Before the calibration-format change (or any future persisted-struct change) ships to a unit whose calibration data actually matters |
| 3 | Persisted provisioning-state model, gating firmware behavior | When there's an actual manufacturing/QA step processing more than one unit |

## Tier 1 — implemented

- **`tools/board_test/factory_bringup.sh`** (new): wraps
  `west flash -d <build-dir> -r jlink --erase` (mass-erases the whole chip,
  including the NVS partition) + a settle delay + `board_test.sh
  --assert-fresh`, so a clean, deterministic bring-up is one command instead
  of a flag nobody remembers.
- **`board_test.sh --assert-fresh`** (new flag): asserts every channel
  actually matches its documented Kconfig factory default
  (`CFG_OUTPUT_ENABLED=1` on switchable channels, `CFG_TARGET_VOLTAGE=0` on
  DAC channels) — not just that a value round-trips self-consistently, which
  is all the existing checks verify and which would have passed even with
  CH8 stuck at the wrong default. Gated behind the flag since a board mid
  development with deliberately-changed config is not a failure.
- Documented in `tools/board_test/README.md` ("Clean bring-up" section),
  including the NVS-partition mechanism as the "why."

**Verified:** live on `jw_lvb` — mass-erase + reflash + `--assert-fresh`
correctly confirmed all 9 switchable channels at `enabled=1`
(`tools/board_test/reports/board_test_20260716_040415.md`, 130/130 pass).

**Non-goal, found during verification, worth remembering:** a
`psb_demo_tui`/`psb_demo_cli monitor` process left running in another
terminal holds the serial port open and races every `board_test.sh`
invocation, producing symptoms indistinguishable from genuine USB/serial
hardware flakiness (intermittent "Connection error", multi-register reads
failing more than single-register ones). Check `fuser /dev/ttyUSB0` before
concluding a bad run means hardware or a regression — this cost real time
to track down and is now called out in `tools/board_test/README.md`.

## Tier 2 — NVS schema safety net (deferred, designed but not built)

**Goal:** a firmware upgrade that changes `vc_channel_config` or
`vc_channel_cal_config` layout should never *silently* revert a unit's real
persisted state to defaults with no signal — it should either migrate the
old data or make the loss visible and deliberate.

**Current mechanism** (`lib/voltage_control/vc_storage_settings.c`):
`settings_save_one(key, cfg, sizeof(*cfg))` / `settings_load_key(key, cfg,
sizeof(*cfg))` — a raw fixed-size blob, no version/magic field anywhere.
`settings_direct_loader` rejects any stored blob whose length doesn't match
`sizeof(*cfg)` exactly (`-EINVAL`), and callers treat that identically to
"never saved" (`-ENOENT`) — silently falling back to
`vc_channel_default_config()`/`default_cal_config()`. Appending new fields
at the end of a struct is the *safe* direction today (old blobs just fail
the size check rather than misinterpreting bytes under a shifted layout),
but "safe" here only means "doesn't corrupt memory," not "doesn't lose
data invisibly."

**Proposed design** (needs its own implementation pass, not fully
scoped yet):

1. Add a `uint16_t schema_version` field to both `vc_channel_config` and
   `vc_channel_cal_config` (or a shared wrapper struct persisted instead of
   the raw struct), bumped whenever the layout changes.
2. On load, if the read succeeds (`settings_load_key` returns 0, i.e. size
   matches) but `schema_version` doesn't match the current firmware's
   expected version, don't silently trust the rest of the blob — either:
   - refuse it and fall back to defaults **with a logged/reported reason**
     (a new fault cause or a shell/Modbus-visible "config was reset due to
     schema mismatch" flag the operator can see), or
   - (harder, later) run an explicit per-version migration function.
3. On a genuine size mismatch (pre-versioning-era blob, smaller than
   today's struct), same visible-reset treatment rather than the current
   silent fallback.
4. Surface this state somewhere an operator/tool can see it — e.g. a new
   system-level status bit or shell line ("config reset at last boot: yes,
   reason: schema v2→v3") — this is the part that actually closes the gap;
   the versioning alone just detects the condition, visibility is what
   prevents the CH8-style surprise.

**Where this intersects Tier 1:** `factory_bringup.sh`'s mass-erase makes
this moot for bench testing (a fresh chip has no stale blob to misread) —
Tier 2 matters once there's real calibration data on a unit that a firmware
update must not silently discard.

## Tier 3 — persisted provisioning-state model (deferred, sketch only)

**Goal:** make "where is this unit in its life" a first-class, persisted,
queryable piece of state, not something inferred from register values by an
operator who has to already know what to look for.

**Sketch:**
- A `vc_provisioning_state` enum (`UNPROVISIONED` → `CALIBRATED` →
  `PRODUCTION_LOCKED` → `RMA`), persisted in its own NVS key (separate from
  per-channel config, since it's a whole-unit concept), defaulting to
  `UNPROVISIONED` on a truly blank chip.
- Expose it read-only over Modbus (a new system input register) and in
  `vc sys status` / `psb_demo_cli info`.
- Optionally gate behavior on it — e.g. refuse to enter `AUTOMATIC` mode (or
  refuse `cal commit`'s effects to "count" as real calibration) while still
  `UNPROVISIONED`, forcing an explicit provisioning step rather than letting
  a truly-blank unit silently behave as if it were calibrated.
- A transition to `CALIBRATED` would naturally hook into the existing `cal
  commit` path (`vc_controller_channel_cal_commit`,
  `lib/voltage_control/vc_controller.c`); `PRODUCTION_LOCKED`/`RMA` would
  need new explicit commands, not automatic triggers.

**Why this is deferred, not scoped further:** this only pays for itself once
there's an actual process (manufacturing line, QA checklist, multiple
units) that needs the firmware itself to enforce or report lifecycle state,
rather than a human just knowing which bench unit is which. Building it now
against a single prototype would be designing against a phantom
requirement. Revisit when a second reason to touch this appears (a second
board, an actual hand-off to someone else, or a customer-facing unit).
