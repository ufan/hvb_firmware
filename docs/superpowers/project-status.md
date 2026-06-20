# Project Status

This ledger is the current navigation map for project state. Historical implementation plans may contain unchecked `- [ ]` steps even when later commits implemented the work. Treat old checkboxes as evidence to inspect, not as authoritative backlog.

## Status Labels

- `verified`: implemented and recorded with summarized verification evidence.
- `implemented-unverified`: code appears present, but no fresh verification is recorded here.
- `superseded`: replaced by a later spec, plan, or implementation path.
- `planned`: approved design or plan exists, but implementation is not started or not found.
- `deferred`: intentionally out of current scope.
- `unknown`: insufficient evidence; investigate before acting.

## Current Baseline

- Branch: `main`
- Worktree: clean (2026-06-20 reconciliation).
- Remote: local is ahead of origin; run `git status --short --branch` for current count.

## Verified Committed Work

| Area | Status | Evidence |
| --- | --- | --- |
| Production runtime/provider seam | `verified` | Commit `4c92b6f feat: add voltage control runtime provider integration`; previously verified domain `71/71`, runtime `8/8`, and `jw_hvb` build. |
| Async runtime and SMF slice | `verified` | Runtime worker, command/evidence queues, SMF scaffolding, app uptime through runtime; previously verified domain `73/73`, runtime `12/12`, and `jw_hvb` build. |
| Static provider bus | `verified` | Commits through `86fcce5 feat: dispatch static provider bus messages`; verified after merge with domain `73/73`, runtime `20/20`, and `jw_hvb` build. |
| Host tools architecture plan and diagrams | `verified` | Commit `7c45f35 docs: add host tools architecture plan`. |
| Resource-constraint guidance | `verified` | Commit `96b008a docs: note firmware resource constraints`. |
| Project status reconciliation design | `verified` | Commit `94c97de docs: design project status reconciliation`. |
| Project status reconciliation plan | `verified` | Commit `a64cee9 docs: plan project status reconciliation`. |
| Project status ledger | `verified` | Created by `e0907e1 docs: add project status ledger`, aligned by `4a8c227 docs: align project status ledger with plan`, and clarified by `686e000 docs: clarify status ledger evidence`. |
| Reconciliation design ledger link | `verified` | Commit `98a2cc5 docs: link status reconciliation ledger`. |
| Static voltage runtime allocation | `verified` | Commit `cd41b64 feat: add static voltage runtime allocation`; verified with domain native tests `74/74`, runtime native tests `21/21`, and `jw_hvb` build. |
| Unified event-driven domain runtime | `verified` | Commit `f3f6742 feat: unified event-driven domain runtime library`; remediation phases 1–3.7 and 4.1–4.2 (driver fixes, code quality, per-field commands, unified creation API, SMF activation, published snapshot, config versioning, Modbus adapter rewrite, runtime integration tests, Kconfig Modbus config, heartbeat refactor). |
| Event-driven protection and Modbus adapter tests | `verified` | Commit `0fac055 feat: event-driven protection triggers and Modbus adapter tests`; 17 Modbus adapter tests added. Verified 134/134 tests (84 domain + 33 runtime + 17 modbus_adapter), `jw_hvb` build clean. |

## Superseded Plans And Specs

| Artifact | Status | Evidence |
| --- | --- | --- |
| `docs/superpowers/specs/2026-06-19-vc-channel-runtime-design.md` | `superseded` | Replaced by `docs/superpowers/specs/2026-06-18-production-runtime-channel-provider-design.md` and later async runtime/static provider bus designs. |
| Historical unchecked implementation plans | `superseded` | Use this ledger plus current code/tests before acting on unchecked plan boxes. |

## Active Uncommitted Work

- Provider-bus dispatch tests: 15 tests with fake iterable bindings covering `start_all`, `notify_channel`, `dispatch_one`, config slot round-trip, and validation. Verified 149/149 tests (84 domain + 33 runtime + 17 modbus_adapter + 15 provider_bus), `jw_hvb` build clean.

## Historical Plan Interpretation

- Do not assume unchecked historical checkboxes are remaining work.
- Check current code, tests, and commits before acting on an old plan step.
- Prefer this ledger when choosing the next slice.
- If a historical plan causes confusion, add a one-line supersession note rather than rewriting every checkbox.

## Remaining Gaps

| Gap | Status | Next Action |
| --- | --- | --- |
| Production app hardening | `verified` | Phases 4.1 (Kconfig Modbus config) and 4.2 (heartbeat refactor) committed in `f3f6742` and `0fac055`. |
| Provider-bus dispatch tests | `implemented-unverified` | 15 tests in `tests/voltage_control/provider_bus/`; commit pending. |
| Settings persistence | `deferred` | Design Zephyr settings/NVS behavior for save/load/factory-reset commands. |
| Evidence freshness | `deferred` | Design stale measurement detection and capability-aware status/fault policy. |
| Startup safety confirmation | `deferred` | Design provider output-safe confirmation before Modbus/shell command acceptance. |
| Host-tools deferred validation | `deferred` | Validate factory GUI workflow and deployment scripts separately. |
| Peripheral tuning hardware record | `deferred` | Record hardware verification for bring-up demos when hardware is available. |
| Hardware interlock GPIO | `deferred` | Removed from DTS binding (2.6); re-add when interlock hardware design is finalized. |

## Update Protocol

When a slice finishes:

1. Run the relevant verification commands.
2. Update this ledger with status and evidence.
3. Keep committed baseline and active uncommitted work separate.
4. If old plans caused confusion, add a concise supersession note to the old plan.
5. Commit implementation and ledger updates together only when they belong to the same slice.
