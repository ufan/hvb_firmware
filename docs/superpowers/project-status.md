# Project Status

This ledger is the current navigation map for project state. Historical implementation plans may contain unchecked `- [ ]` steps even when later commits implemented the work. Treat old checkboxes as evidence to inspect, not as authoritative backlog.

## Status Labels

- `verified`: implemented and recently verified with command evidence.
- `implemented-unverified`: code appears present, but no fresh verification is recorded here.
- `superseded`: replaced by a later spec, plan, or implementation path.
- `planned`: approved design or plan exists, but implementation is not started or not found.
- `deferred`: intentionally out of current scope.
- `unknown`: insufficient evidence; investigate before acting.

## Current Baseline

- Branch: `main`
- Remote tracking at last local check: `main...origin/main [ahead 2]` because reconciliation design and plan docs are local.
- Worktree at last local check: dirty with active static-lifetime cleanup in firmware and tests.
- No git stashes were present after prior cleanup.

## Verified Committed Work

| Area | Status | Evidence |
| --- | --- | --- |
| Production runtime/provider seam | `verified` | Commit `4c92b6f feat: add voltage control runtime provider integration`; previously verified domain `71/71`, runtime `8/8`, and `jw_hvb` build. |
| Async runtime and SMF slice | `verified` | Runtime worker, command/evidence queues, SMF scaffolding, app uptime through runtime; previously verified domain `73/73`, runtime `12/12`, and `jw_hvb` build. |
| Static provider bus | `verified` | Commits through `86fcce5 feat: dispatch static provider bus messages`; verified after merge with domain `73/73`, runtime `20/20`, and `jw_hvb` build. |
| Host tools architecture plan and diagrams | `verified` | Commit `7c45f35 docs: add host tools architecture plan`. |
| Resource-constraint guidance | `verified` | Commit `96b008a docs: note firmware resource constraints`. |
| Project status reconciliation design | `verified` | Commit `94c97de docs: design project status reconciliation`. |

## Active Uncommitted Work

Static-lifetime cleanup is active in the worktree and is not part of the committed baseline until committed.

Files:

- `applications/hvb_controller/src/main.c`
- `include/voltage_control/domain.h`
- `include/voltage_control/runtime.h`
- `lib/voltage_control/domain.c`
- `lib/voltage_control/runtime.c`
- `tests/voltage_control/domain/src/main.c`
- `tests/voltage_control/runtime/src/main.c`

Fresh verification for this active cleanup passed before this ledger was planned:

- Domain native tests: `74/74`
- Runtime native tests: `21/21`
- `jw_hvb` build: pass

## Historical Plan Interpretation

- Do not assume unchecked historical checkboxes are remaining work.
- Check current code, tests, and commits before acting on an old plan step.
- Prefer this ledger when choosing the next slice.
- If a historical plan causes confusion, add a one-line supersession note rather than rewriting every checkbox.

## Remaining Gaps

| Gap | Status | Next Action |
| --- | --- | --- |
| Commit or revise static-lifetime cleanup | `planned` | Review active changes, commit if accepted, or adjust before further firmware work. |
| Production app hardening | `planned` | Design Modbus unit ID/baud source, heartbeat structure, and uptime ownership before implementation. |
| Provider-bus dispatch tests | `planned` | Add fake iterable provider binding coverage for provider bus start/dispatch behavior. |
| Settings persistence | `deferred` | Design Zephyr settings/NVS behavior for save/load/factory-reset commands. |
| Evidence freshness | `deferred` | Design stale measurement detection and capability-aware status/fault policy. |
| Startup safety confirmation | `deferred` | Design provider output-safe confirmation before Modbus/shell command acceptance. |
| Host-tools deferred validation | `deferred` | Validate factory GUI workflow and deployment scripts separately. |
| Peripheral tuning hardware record | `deferred` | Record hardware verification for bring-up demos when hardware is available. |

## Update Protocol

When a slice finishes:

1. Run the relevant verification commands.
2. Update this ledger with status and evidence.
3. Keep committed baseline and active uncommitted work separate.
4. If old plans caused confusion, add a concise supersession note to the old plan.
5. Commit implementation and ledger updates together only when they belong to the same slice.
