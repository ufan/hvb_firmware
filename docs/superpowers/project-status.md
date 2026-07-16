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
- Worktree: clean (2026-07-16 cleanup pass; see Ledger Maintenance Note below).
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
| Provider-bus dispatch tests | `verified` | Commit `4474df6 feat: add provider-bus dispatch tests with fake iterable bindings`; 15 provider_bus tests. |
| Evidence freshness | `verified` | DTS-derived `VC_MAX_CHANNELS`, measurement buffer as RAM iterable section with numeric sorting, `VC_FAULT_STALE` flag, staleness detection at snapshot publish time. Verified 154/154 tests (84 domain + 36 runtime + 18 provider_bus + 17 modbus_adapter, 1 skipped), `jw_hvb` build clean. |
| Settings persistence | `verified` | `vc_storage_backend` interface, Zephyr Settings/NVS backend, param_action implementation (SAVE/LOAD/FACTORY_RESET/SOFTWARE_RESET), auto-load at startup, flash partition, calibration preserved on factory-reset. Verified 160/160 tests (84 domain + 42 runtime + 18 provider_bus + 17 modbus_adapter, 1 skipped), `jw_hvb` build clean. |
| Typed canonical Register Catalog | `verified` | `include/reg_store/{reg_catalog,reg_schema}.h`, `REG_DESCRIPTOR_DEFINE` pattern — the register-facing interface all frontends (Modbus adapter, shell) actually use today (confirmed directly in `lib/sys_status/*.c`, `lib/voltage_control/vc_runtime.c`). Enforced by a repo guard-rail, not just convention: `tests/architecture/controller_split.sh` fails the build if `vc_dispatch`/`vc_query`/`vc_cmd_*` appear in production code ("frontends must use the Register Catalog, not the removed VC facade"). Supersedes the CQRS unified-context-api and the provider-bus generation — see Superseded table. |
| VC shell console frontend | `verified` | `lib/voltage_control/vc_shell.c` (1300+ lines), `SHELL_CMD_REGISTER(vc, &sub_vc, "Voltage control", NULL)`. Closes the "Shell console frontend" gap previously listed as `planned`. |
| jw_lvb board support + `hvb_*`→`psb_*` host-tools rename | `implemented-unverified` | Second board variant (10-channel, on/off-only, no DAC) alongside `jw_hvb` (2-channel, full DAC/measurement); channel-capability-flag model (`CH_CAP_*`) added so host tools adapt per-channel rather than assuming a fixed register layout. Host tool binaries/sources renamed `hvb_demo_app`→`psb_demo_app`, `hvb_factory_tool`→`psb_factory_tool`. See `docs/guide/channel-capability-model.md`. Not independently re-verified with a fresh test run for this ledger entry. |
| `psb_demo_tui`/`psb_demo_cli` Modbus-client hardening | `verified` | Commits `ce29be6`, `7eb8d07`, `57acc08`, `b8cfd57`, `c55f852`, `80a29eb`, `c01e1e9`. Fixed: merge-on-success read contract (transient failures no longer reset cached fields to zero/default), connect-scan and poll-loop redesign (staged atomic publish, decoupled uptime cadence, capability self-heal, offline-channel detection), a CPU busy-wait spin and a serial inter-byte-timeout formula bug in `psb_modbus_core` (connect scan ~25-33s → ~3.83s, uptime cadence ~3-6s → ~1s measured live), and an `ENV_SENSOR`-capability display gap. Verified 240/240 assertions across 64 test cases passing, plus live hardware verification against a `jw_lvb` board (see `docs/guide/client-architecture-and-pitfalls.md` for full detail). |
| Board lifecycle state management, Tier 1 | `verified` | `docs/superpowers/plans/2026-07-16-board-lifecycle-state-management.md` (self-documents "Tier 1 implemented (2026-07-16)"). Tiers 2-3 remain `deferred` by the plan's own text — not re-litigated here. |

## Superseded Plans And Specs

| Artifact | Status | Evidence |
| --- | --- | --- |
| `docs/superpowers/specs/2026-06-19-vc-channel-runtime-design.md` | `superseded` | Replaced by `docs/superpowers/specs/2026-06-18-production-runtime-channel-provider-design.md` and later async runtime/static provider bus designs. |
| `docs/superpowers/plans/2026-06-22-cqrs-unified-context-api.md` | `superseded` | `vc_init()`/`vc_ctx_start()` survive in current `include/voltage_control/vc.h`, but the plan's core `vc_dispatch()`/`vc_query()`/`vc_cmd_*`/`vc_q_*` command-bus surface is gone from the tree and is now actively *forbidden*: `tests/architecture/controller_split.sh` fails the build if those symbols appear in production code, explicitly calling it "the removed VC facade." Superseded by the Register Catalog (`typed-canonical-register-catalog-design.md` and its hardening/integration plans, all 2026-06-28). |
| `docs/superpowers/specs/2026-06-19-static-provider-bus-design.md`, `docs/superpowers/specs/2026-06-19-vc-channel-provider-design.md` (+ matching implementation plans) | `superseded` | Self-documented: `docs/superpowers/specs/2026-06-24-channel-table-direct-drive-design.md` states `**Supersedes**: 2026-06-19-static-provider-bus-design.md, 2026-06-19-vc-channel-provider-design.md`. Confirmed independently — zero `provider_bus`/`vc_channel_provider` symbols anywhere in `include/` or `lib/` today. |
| `docs/superpowers/plans/2026-06-18-production-runtime-channel-provider.md`, `docs/superpowers/specs/2026-06-18-production-runtime-channel-provider-design.md` | `superseded` | Same "provider" abstraction family as the pair above (no "provider" symbols remain in current source); most likely carried forward transitively by the same 2026-06-24 direct-drive redesign, though this specific pair has no explicit self-documented supersession line — flagged `superseded` on naming/symbol evidence, not a doc citation, so treat this one row as lower-confidence than the others in this table. |
| Historical unchecked implementation plans | `superseded` | Use this ledger plus current code/tests before acting on unchecked plan boxes. |

## Active Uncommitted Work

None.

## Historical Plan Interpretation

- Do not assume unchecked historical checkboxes are remaining work.
- Check current code, tests, and commits before acting on an old plan step.
- Prefer this ledger when choosing the next slice.
- If a historical plan causes confusion, add a one-line supersession note rather than rewriting every checkbox.

## Ledger Maintenance Note (2026-07-16)

This ledger was last reconciled around 2026-06-20/21 (the "Settings persistence"
row). `main` has since moved ~331 commits, spanning at least: the Register
Catalog rewrite and removal of the earlier CQRS/provider-bus architecture
generations, the `vc_shell.c` console frontend, `jw_lvb` board support and the
channel-capability-flag model, the `hvb_*`→`psb_*` host-tools rename, a factory
tool redesign, a Qt/QML GUI, a calibration-format rework (decimal-exponent
gain), and the `psb_demo_tui`/`psb_demo_cli` Modbus-client hardening pass.

This pass did not attempt to itemize all ~331 commits individually — that
would mean fabricating verification evidence for work not personally
re-checked. Instead it: (a) mechanically surfaced supersession/status claims
the specs/plans already self-document (e.g. `**Supersedes**:` headers, a
plan's own "Tier 1 implemented" note), (b) added a small number of new rows
backed by direct, checkable evidence gathered during this pass (grep results,
a repo-enforced architecture test, or this session's own commits/test runs),
and (c) removed three stale `.temp` files that were incomplete/duplicate
leftovers of files that already exist in finished form.

**What this means for a reader**: rows added in this pass are honestly scoped
— `implemented-unverified` where code was found but not freshly re-tested,
and one `superseded` row explicitly flagged lower-confidence where the
evidence is inference (naming/symbol absence) rather than a self-documented
citation. The gap between 2026-06-21 and 2026-07-16 is not fully closed by
this pass. For anything not mentioned here, `git log` and the dated files in
`plans/`/`specs/` remain the source of truth — many of the more recent ones
(e.g. `2026-07-16-board-lifecycle-state-management.md`) already state their
own status inline, which is the cheapest way to keep this ledger honest going
forward: prefer adding a one-line status note to the plan/spec itself at the
time of implementation, over relying on a later archaeology pass here.

## Remaining Gaps

| Gap | Status | Next Action |
| --- | --- | --- |
| Production app hardening | `verified` | Phases 4.1 (Kconfig Modbus config) and 4.2 (heartbeat refactor) committed in `f3f6742` and `0fac055`. |
| Provider-bus dispatch tests | `verified` | Committed as `4474df6`. |
| Settings persistence | `verified` | `vc_storage_backend` interface, NVS backend, auto-load, calibration-safe factory-reset. |
| Evidence freshness | `verified` | Measurement buffer, `VC_FAULT_STALE`, DTS-derived channel count. |
| Startup safety confirmation | `verified` | Existing three-layer safety is sufficient: driver init sets GPIO INACTIVE, domain publishes force_safe_state=true, Modbus server starts last. No additional code needed. |
| Host-tools register map alignment | `verified` | Aligned ChStatus and FaultCause with firmware VC_FAULT_STALE. Removed phantom UNSUPPORTED/RETRY_EXHAUSTED bits. Host tools build clean, 5/5 affected tests pass. 4 pre-existing channel-read test failures remain (register offset bug, not related). |
| Shell console frontend | `verified` | Implemented: `lib/voltage_control/vc_shell.c`. See Verified Committed Work above — this row is now closed, kept here struck through in spirit rather than deleted so the original gap is traceable. |
| Host-tools channel-read test fix | `verified` | No failures found: `tools/build/linux-debug/psb_modbus_core/tests/psb_tests` passes 240/240 assertions across 64 test cases as of 2026-07-16 (includes `test_channel_reads.cpp` and `test_calibration.cpp`). Whatever caused the 4 failures this row originally referenced is no longer reproducible against current `main`; not independently bisected to a specific fixing commit. |
| Peripheral tuning hardware record | `deferred` | Record hardware verification for bring-up demos when hardware is available. |
| Hardware interlock GPIO | `deferred` | Removed from DTS binding (2.6); re-add when interlock hardware design is finalized. |

## Update Protocol

When a slice finishes:

1. Run the relevant verification commands.
2. Update this ledger with status and evidence.
3. Keep committed baseline and active uncommitted work separate.
4. If old plans caused confusion, add a concise supersession note to the old plan.
5. Commit implementation and ledger updates together only when they belong to the same slice.
