# Project Status Reconciliation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Create a current project status ledger so future agents can distinguish verified work, active uncommitted work, stale historical checkboxes, and real remaining gaps.

**Architecture:** This is a docs-only reconciliation slice. The authoritative artifact is `docs/superpowers/project-status.md`; historical specs/plans stay intact, with only a minimal link from the reconciliation design spec to the ledger once the ledger exists.

**Tech Stack:** Markdown, git, repository docs under `docs/superpowers/`.

---

## File Structure

- Create: `docs/superpowers/project-status.md`
  - Responsibility: authoritative current project status ledger for humans and agents.
- Modify: `docs/superpowers/specs/2026-06-19-project-status-reconciliation-design.md`
  - Responsibility: point readers from the design to the implemented ledger artifact.

Do not edit firmware source, tests, host tools, or historical plan checkbox bodies in this slice.

## Task 1: Create The Project Status Ledger

**Files:**
- Create: `docs/superpowers/project-status.md`

- [ ] **Step 1: Write the ledger document**

Create `docs/superpowers/project-status.md` with exactly this structure and content, updating only the status line if `git status --short --branch` shows a different branch/ahead state at execution time:

```markdown
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
- Remote tracking at last local check: `main...origin/main [ahead 1]` because `docs: design project status reconciliation` is local.
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
```

- [ ] **Step 2: Verify the ledger file exists and contains required sections**

Run:

```bash
test -f docs/superpowers/project-status.md
rg "^## (Status Labels|Current Baseline|Verified Committed Work|Active Uncommitted Work|Historical Plan Interpretation|Remaining Gaps|Update Protocol)$" docs/superpowers/project-status.md
```

Expected: `test` exits 0 and `rg` prints all seven section headings.

- [ ] **Step 3: Commit the ledger document**

Run:

```bash
git add docs/superpowers/project-status.md
git commit -m "docs: add project status ledger"
```

Expected: one docs-only commit creating `docs/superpowers/project-status.md`.

## Task 2: Link The Design Spec To The Ledger

**Files:**
- Modify: `docs/superpowers/specs/2026-06-19-project-status-reconciliation-design.md`

- [ ] **Step 1: Add implemented-artifact note**

Add this paragraph after the Purpose section's second paragraph:

```markdown
Implemented artifact: `docs/superpowers/project-status.md` is the current ledger described by this design.
```

- [ ] **Step 2: Verify the link text exists**

Run:

```bash
rg "Implemented artifact: `docs/superpowers/project-status.md`" docs/superpowers/specs/2026-06-19-project-status-reconciliation-design.md
```

Expected: `rg` prints the implemented-artifact line.

- [ ] **Step 3: Commit the spec link**

Run:

```bash
git add docs/superpowers/specs/2026-06-19-project-status-reconciliation-design.md
git commit -m "docs: link status reconciliation ledger"
```

Expected: one docs-only commit modifying the reconciliation design spec.

## Task 3: Final Documentation Verification

**Files:**
- Read: `docs/superpowers/project-status.md`
- Read: `docs/superpowers/specs/2026-06-19-project-status-reconciliation-design.md`

- [ ] **Step 1: Verify no placeholder language exists**

Run:

```bash
rg "TBD|FIXME|placeholder|fill in|implement later" docs/superpowers/project-status.md docs/superpowers/specs/2026-06-19-project-status-reconciliation-design.md
```

Expected: no matches and exit code 1 from `rg`.

- [ ] **Step 2: Verify active firmware changes were not staged by docs commits**

Run:

```bash
git status --short
```

Expected: firmware/test static-lifetime files may still appear as unstaged ` M`, but no staged changes remain.

- [ ] **Step 3: Report result**

Report the two commit hashes, confirm `docs/superpowers/project-status.md` exists, and list any remaining uncommitted non-doc changes.
