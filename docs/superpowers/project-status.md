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
- Remote tracking at last local check: local reconciliation and static-allocation commits are ahead of origin; run `git status --short --branch` for the current count.
- Worktree at last local check: clean after committing static-lifetime cleanup and this ledger update.
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
| Project status reconciliation plan | `verified` | Commit `a64cee9 docs: plan project status reconciliation`. |
| Project status ledger | `verified` | Created by `e0907e1 docs: add project status ledger`, aligned by `4a8c227 docs: align project status ledger with plan`, and clarified by `686e000 docs: clarify status ledger evidence`. |
| Reconciliation design ledger link | `verified` | Commit `98a2cc5 docs: link status reconciliation ledger`. |
| Static voltage runtime allocation | `verified` | Commit `cd41b64 feat: add static voltage runtime allocation`; verified with domain native tests `74/74`, runtime native tests `21/21`, and `jw_hvb` build. |

## Superseded Plans And Specs

| Artifact | Status | Evidence |
| --- | --- | --- |
| `docs/superpowers/specs/2026-06-19-vc-channel-runtime-design.md` | `superseded` | Replaced by `docs/superpowers/specs/2026-06-18-production-runtime-channel-provider-design.md` and later async runtime/static provider bus designs. |
| Historical unchecked implementation plans | `superseded` | Use this ledger plus current code/tests before acting on unchecked plan boxes. |

## Active Uncommitted Work

Remediation plan execution (2026-06-20):
- Phase 1 (driver fixes 1.1–1.6): applied, verified domain 74/74, runtime 21/21, jw_hvb build
- Phase 2 (code quality 2.1–2.6): applied, verified domain 74/74, runtime 21/21, jw_hvb build
- Phase 3.0: design spec written at `docs/superpowers/specs/2026-06-20-unified-event-driven-domain-runtime-design.md`
- Phase 3.1 (per-field command infra): applied, verified 95/95 tests, jw_hvb build
- Phase 3.2 (unified domain runtime): files renamed, domain_tick removed, unified creation API, verified 95/95 tests, jw_hvb build
- Phase 3.3 (SMF activation): channel/domain state tracking active, verified 95/95 tests, jw_hvb build
- Phase 3.4 (published domain snapshot): published snapshot with frontend read API, verified 95/95 tests, jw_hvb build
- Phase 3.6 (config version bump): domain_set_channel_config now bumps runtime_config_version, verified 95/95 tests, jw_hvb build
- Phase 3.5 (Modbus adapter rewrite): applied, verified 95/95 tests, jw_hvb build. Data race eliminated.
- Phase 3.7 (test updates): 12 new runtime integration tests added (unified creation, per-field commands, published snapshot), verified 117/117 tests total (84 domain + 33 runtime), jw_hvb build
- Phase 4.1 (Modbus config from Kconfig): CONFIG_VC_MODBUS_UNIT_ID and CONFIG_VC_MODBUS_BAUD_RATE added, main.c uses them
- Phase 4.2 (heartbeat refactor): heartbeat moved to k_work_delayable, uptime read internally by runtime, main.c no longer references struct domain, verified 117/117 tests, jw_hvb build

## Historical Plan Interpretation

- Do not assume unchecked historical checkboxes are remaining work.
- Check current code, tests, and commits before acting on an old plan step.
- Prefer this ledger when choosing the next slice.
- If a historical plan causes confusion, add a one-line supersession note rather than rewriting every checkbox.

## Remaining Gaps

| Gap | Status | Next Action |
| --- | --- | --- |
| Production app hardening | `planned` | Design Modbus unit ID/baud source, heartbeat structure, and uptime ownership before implementation. |
| Provider-bus dispatch tests | `planned` | Add fake iterable provider binding coverage for provider bus start/dispatch behavior. |
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
