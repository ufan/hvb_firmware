# Sidebar Board Connection Indicator — Design

## Context

In a multi-board session, `psb_demo_tui`'s vertical sidebar (`board_switcher.h`)
lists each board's nickname but gives no at-a-glance signal of its connection
health — the user has to switch to each board's own dashboard to see its
status dot. Requested addition: a small colored symbol in front of each
nickname in the sidebar, reusing the same connection/staleness logic the
per-board dashboard already computes for its own status dot
(`board_dashboard.h`'s `connDotEl`/`sysStale` logic).

## Confirmed Decisions

- **State mapping** (the request named two symbols; the dashboard's own logic
  actually has more states — resolved via clarifying questions):
  - Connected, polling normal → green filled circle ● — `connected && !sysStale`
  - Connected, but data stale (polling failed) → red square ■ —
    `connected && sysStale`, using the same 10 s threshold
    (`kSysStaleThreshold`) the dashboard already uses.
  - Connecting (in progress) → yellow half-circle ◐ (static glyph, not a true
    spinning animation — the app has no periodic redraw ticker during an
    in-flight connect attempt, and the user confirmed a static glyph is fine).
  - Connection failed (not connected, not connecting, last status was an
    error) → red square ■ — reuses the dashboard status bar's own
    `statusMsg.find("Error") != npos` check.
  - Idle / never connected (not connected, not connecting, no error) → gray
    hollow circle ○.
- All five conditions reuse existing `BoardSession` fields/logic verbatim
  (`connected`, `connecting`, `data.sysStale(...)`, `statusMsg`) — no new
  state is introduced on `BoardSession`.
- Symbols are drawn from the Geometric Shapes Unicode block (U+25A0–U+25FF),
  matching the existing dashboard's own glyph choices where they overlap (●
  and ○ are literally the same characters the dashboard already uses).

## Architecture

**`tui_format.h`**: `kSysStaleThreshold` (currently a `static constexpr`
local to a lambda inside `board_dashboard.h`) is promoted to a
`inline constexpr auto kSysStaleThreshold = std::chrono::seconds(10);` at
namespace scope, next to `ScannedData::sysStale()` itself. Both
`board_dashboard.h` and `board_switcher.h` reference this one definition —
avoids duplicating the threshold now that a second call site needs it.

**`board_switcher.h`**: the sidebar is `Menu(boardNames.get(),
activeBoard.get(), switcherOpt)` — a plain FTXUI `Menu` bound directly to
nickname strings, currently using `MenuOption::Vertical()`'s default
rendering (`(active ? "> " : "  ") + label`, `inverted` when focused, `bold`
when active). `switcherOpt.transform` is set to a custom lambda that:
1. Looks up the `BoardSession` matching `state.label` (the nickname) by
   linear scan over `boards` (the function's existing
   `std::vector<std::unique_ptr<BoardSession>>&` parameter) — boards are
   few in practice, and this mirrors `detachBoard`'s existing nickname-based
   lookup pattern in the same file.
2. Computes which of the five states applies and builds a colored one-glyph
   `Element` for it.
3. Renders `hbox({ text(prefix), dot, text(state.label) })` where `prefix`
   is the existing `"> "`/`"  "` active-marker, preserving today's
   focus/active styling (`inverted`/`bold`) on the whole row exactly as
   `DefaultOptionTransform` does today — the only change is the new dot
   inserted between the arrow and the nickname.

No new locking: `boards` is already read unguarded once at switcher
construction time in this same file; per `Runtime::boardsMutex`'s own
documented purpose (guarding `rt.boards` vector mutations, all of which are
themselves issued from the UI thread), reading it during a render pass on
that same single UI thread is safe without an additional lock — consistent
with this file's existing precedent, not a new pattern.

## Error Handling

None — this is a read-only display of existing state; no new failure modes.

## Testing

No automated test surface (FTXUI rendering, consistent with this codebase's
established practice). Manual tmux verification: connect a multi-board
session (as prior rounds did, using the real hardware or the test topology
file), confirm each of the five states renders its correct symbol/color at
the right point in that board's lifecycle (idle before connecting,
half-circle while connecting, green once connected and polling, red if
disconnected mid-poll with an error, gray after a clean manual disconnect).
Protect the real `~/.psb_demo_app/topology.toml` the same way every prior
round did.

## Out of Scope

- True animation for the connecting state (static glyph only, per user
  confirmation).
- Any change to the per-board dashboard's own status dot — this only adds a
  second, independent indicator in the sidebar.
