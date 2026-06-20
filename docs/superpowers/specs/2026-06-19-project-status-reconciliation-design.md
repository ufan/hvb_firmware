# Project Status Reconciliation Design

## Purpose

Historical implementation plans in this repository still contain unchecked `- [ ]` steps even when later commits implemented the described work. Future agents must not treat unchecked boxes in older plans as authoritative backlog without corroborating evidence from code, tests, specs, and commits.

This design defines a lightweight status reconciliation layer: one current status ledger that summarizes what is verified, what is superseded, and what remains actionable.

Implemented artifact: `docs/superpowers/project-status.md` is the current ledger described by this design.

## Problem

The repository now has several overlapping specs and plans for voltage-control domain behavior, Modbus, calibration, runtime, providers, static provider bus, host tools, and peripheral tuning. Many plans were executed through incremental commits but their checkboxes were not updated afterward.

This creates three risks:

- Agents may re-implement completed work because old checkboxes remain unchecked.
- Agents may skip real gaps because completed and incomplete work are mixed in the same stale plan.
- Users lose a fast way to answer: "what is the current project state?"

## Scope

This reconciliation slice covers documentation and workflow only.

In scope:

- A current project status ledger under `docs/superpowers/`.
- A clear rule that historical plan checkboxes are evidence, not truth.
- A compact inventory of completed/superseded plans and remaining gaps.
- Evidence links to commits, files, or verification commands where available.
- Update rules for future agents.

Out of scope:

- Editing every old plan checkbox line-by-line.
- Moving or archiving historical plans.
- Implementing any firmware, host-tool, or test changes.
- Resolving the currently uncommitted static-lifetime cleanup.

## Proposed Artifact

Create `docs/superpowers/project-status.md` as the authoritative current status ledger.

The ledger should include:

- **Verified Baseline**: current branch, latest relevant commits, clean/dirty state at time of update, and latest verification evidence.
- **Completed Or Superseded Plans**: each plan/spec with a status label and evidence summary.
- **Active Worktree Changes**: uncommitted work that should not be confused with merged baseline.
- **Real Remaining Gaps**: actionable next slices with short rationale.
- **Historical Plan Rule**: unchecked boxes in old plans are not authoritative unless confirmed against current code and tests.
- **Update Protocol**: when future agents finish a slice, they update this ledger before asking what comes next.

## Status Labels

Use a small fixed vocabulary:

- `verified`: implemented and recently verified with command evidence.
- `implemented-unverified`: code appears present, but no fresh verification is recorded in the ledger.
- `superseded`: replaced by a later spec/plan or implementation path.
- `planned`: approved design/plan exists, implementation not started or not found.
- `deferred`: intentionally out of current scope.
- `unknown`: insufficient evidence; requires investigation before action.

## Initial Ledger Content

The first ledger update should capture these facts:

- `main` was synchronized with `origin/main` after the user pushed prior completed work.
- Static provider bus, async runtime, provider worker integration, production runtime/provider seam, host-tools plan/diagrams, and resource-constraint AGENTS guidance have committed history.
- There is active uncommitted static-lifetime cleanup in:
  - `applications/hvb_controller/src/main.c`
  - `include/voltage_control/domain.h`
  - `include/voltage_control/runtime.h`
  - `lib/voltage_control/domain.c`
  - `lib/voltage_control/runtime.c`
  - `tests/voltage_control/domain/src/main.c`
  - `tests/voltage_control/runtime/src/main.c`
- Fresh verification for that active cleanup has passed:
  - domain native tests: `74/74`
  - runtime native tests: `21/21`
  - `jw_hvb` build: pass

## Remaining Gap Categories

The ledger should initially list these candidate gaps:

- **Production app hardening**: Modbus unit ID/baud configuration, heartbeat structure, and uptime ownership.
- **Provider-bus dispatch tests**: fake iterable provider binding coverage for start/dispatch behavior.
- **Settings persistence**: Zephyr settings/NVS behavior for save/load/factory-reset commands.
- **Evidence freshness**: stale measurement detection and capability-aware status/fault policy.
- **Startup safety confirmation**: prove provider outputs are safe before frontends accept commands.
- **Host-tools deferred validation**: factory GUI workflow and deploy-script follow-up.
- **Peripheral tuning hardware record**: hardware verification notes for bring-up demos.

## Update Protocol

When a slice is completed:

1. Run the relevant verification commands.
2. Update `docs/superpowers/project-status.md` with the new status and evidence.
3. If old plans caused confusion, add a one-line supersession note to the old plan rather than rewriting all checkboxes.
4. Commit the implementation and status update together when they belong to the same slice.
5. Keep unrelated active work out of the status commit.

## Non-Goals

The reconciliation ledger is not a full issue tracker, not a substitute for specs, and not a place for private implementation notes. It is a navigation map for humans and agents to decide what to do next safely.

## Acceptance Criteria

- A new `docs/superpowers/project-status.md` exists.
- The ledger identifies stale historical plan checkboxes as non-authoritative.
- The ledger distinguishes committed baseline from active uncommitted work.
- Remaining gaps are listed as actionable slices, not vague TODOs.
- Future agents can find the next safe task without re-reading every historical plan first.
