# CLI Args → UI (Preferences Dialog) — Design

## Context

This is Sub-project C of a three-part follow-on to Phase 3 (the TUI interactive
setup wizard), Sub-project A (board removal, merged), and Sub-project B (mode
architecture — mode-select popup, quick-connect form, global menu bar; merged).
The user's original request, decomposed during Sub-project A's brainstorming:

- **TUI optimization 3**: `psb_demo_tui` is normally launched by double-click,
  not from a command line (especially on Windows) — migrate its command-line
  arguments into the setting dialog, whether combined into one place or split
  across menus.

Sub-project B's design spec explicitly anticipated this: its Sequencing Note
says the whole "genuinely fresh launch" model only makes full sense "once
Sub-project C removes CLI args." This spec is that removal.

## Goal

Every flag `psb_demo_tui` currently accepts already has, or gains here, a UI
home — reducing the command line to `--version` (and `--help`, CLI11's own
convention), so a double-click launch never needs any argument.

## Current CLI Surface (before this change)

| Flag | Purpose | UI replacement |
|---|---|---|
| `-p,--port` | quick single-board connect | Sub-project B's quick-connect form |
| `-b,--baud` | baud for `-p` | Sub-project B's quick-connect form |
| `-i,--id` | slave ID for `-p` | Sub-project B's quick-connect form |
| `-T,--topology` | topology file override | the wizard's own path field + Load |
| `--setup` | launch wizard directly | mode popup's "Multi-Board Setup", and the always-available global Setup button |
| `-t,--timeout` | connect timeout ms | **new: Preferences dialog** (this spec) |
| `-s,--poll-interval` | idle poll interval s | **new: Preferences dialog** (this spec) |
| `--version` | print version, exit | unchanged — stays CLI-only |

## Confirmed Decisions

- All flags above except `--version` are removed entirely — no power-user
  CLI shortcuts retained. Timeout and poll interval get a persisted
  Preferences dialog instead, so removing their flags doesn't make them
  write-only: the value set once is remembered across launches, the same
  way quick-connect already remembers the last-used port/baud/slave.
- The new global button is labeled **Preferences**, not "Settings" —
  deliberately distinct from the existing per-board **Setting** button
  (singular, opens board register config in `board_dashboard.h`), which is
  a different concept entirely and sits right next to it in the UI.
- Preferences is threaded through the exact same global-button machinery
  Sub-project B built for `Quit`/`Setup`: its own row in multi-board mode,
  folded into the single board's own merged row otherwise. No new UI
  pattern — a third instance of an existing one.
- Preferences values persist to `~/.psb_demo_app/preferences.toml`, loaded
  at the very top of `main()` (before any board is built, so it affects the
  first connection attempt of the session too), and re-saved on every Save.
- `psb_demo_cli` (the separate, deliberately scriptable CLI tool) is
  untouched — it has its own independent `-t,--timeout` flag and default,
  unrelated to this TUI-specific change.

## Architecture

### `tools/psb_modbus_core/app_preferences.h` / `.cpp`

New struct, same shape as `TopologyConfig`:

```cpp
namespace psb {

struct AppPreferences {
    int timeoutMs = 3000;
    int pollIntervalS = 1;

    static std::optional<AppPreferences> load(const std::string& path);
    bool save(const std::string& path) const;
    static std::string defaultPath();  // ~/.psb_demo_app/preferences.toml
};

} // namespace psb
```

`load()` returns `std::nullopt` uniformly for "file missing" and "file
malformed" — unlike `TopologyConfig`, there's no need for callers to tell
those apart (a bad preferences file just means "use hardcoded defaults",
never a hard error).

**Shared cleanup**: `topology_config.cpp`'s `homeDir()` helper is currently
`static` (internal linkage), so `app_preferences.cpp` can't reuse it as-is.
It moves to a new `tools/psb_modbus_core/platform_paths.h` / `.cpp`
(non-static, `psb::homeDir()`), and `topology_config.cpp` is updated to call
the shared version instead of keeping its own copy. Small, tightly scoped to
what this change actually needs — not a speculative refactor.

### `tools/psb_demo_app/tui/preferences_dialog.h`

New file, mirroring `BoardSwitcher`'s "struct of root + functions" shape
rather than a bare `Component`, because opening the dialog needs to reset
its fields from the *current* live values first (the same reason
`main.cpp`'s `openSetup` closure resets `midSessionWiz`'s fields before
setting `*showSetup = true`):

```cpp
namespace psb::tui {

struct PreferencesDialog {
    Component root;
    std::function<void()> open;  // resets fields from current values, then shows
};

inline PreferencesDialog makePreferencesDialog(ScreenInteractive& screen,
                                               std::shared_ptr<bool> showPreferences,
                                               int& timeoutMs, int& pollIntervalS);

} // namespace psb::tui
```

`timeoutMs`/`pollIntervalS` are taken by reference from the caller (`main()`'s
globals) — `preferences_dialog.h` has no global-variable dependency of its
own, consistent with `mode_select.h`'s style. `Save` parses both fields with
the same `try { std::stoi(...) } catch (...) {}`-falls-back-to-current-value
pattern used throughout this codebase (quick-connect's baud/slave, the
wizard's Add Bus/Add Board forms), writes the parsed values back through the
references, persists via `AppPreferences::save(AppPreferences::defaultPath())`,
and closes. `Cancel` closes without touching anything.

### `main.cpp`

- `g_pollInterval` (already an existing global, read live every idle cycle
  in `runBusWorkerLoop` — no plumbing changes needed there) gains a sibling
  `g_connectTimeoutMs`, both initialized from `AppPreferences::load()` right
  after `CLI::App`/`CLI11_PARSE` (which now only parses `--version`/`--help`).
- The existing `timeoutMs` function-parameter threading through
  `buildRuntime`/`applyNewBoardsLive` is unchanged — only the *value* passed
  at the top-level call sites changes, from the old local `timeoutArg` to
  `g_connectTimeoutMs`. Smallest possible diff; no signatures change.
- A third global button, `bGlobalPreferences`, is constructed alongside
  `bGlobalQuit`/`bGlobalSetup` and threaded through the identical call sites
  (`buildRuntime`, `applyNewBoardsLive`, `makeBoardSwitcher`) — extending
  Sub-project B's 2-button machinery to 3, not a new mechanism.
- `board_switcher.h`'s `globalMenuBar` becomes
  `Container::Horizontal({globalQuit, globalSetup, globalPreferences})`.
- `board_dashboard.h`'s single-board merge (`liveBoardCount() <= 1`) folds
  in the third button's render the same way it already folds in the first
  two.
- The Preferences dialog itself is layered as a second `Modal`, chained
  alongside the existing mid-session wizard's:
  ```cpp
  auto rootFinal = Renderer(rt.switcher.root, [...] { ... })
      | Modal(midSessionWizardRoot, showSetup.get())
      | Modal(prefsDialog.root, showPreferences.get());
  ```

### `main()`'s topology-resolution simplification

Removing `-p`/`-T`/`--setup` makes `portArg`, `topologyOpt`,
`topologyExplicit`, and `setupFlag` disappear, which collapses the existing
4-branch if/else-if chain to 2 branches — 3 of the 4 branches only ever
fired when one of those now-removed flags was set:

```cpp
psb::TopologyConfig topo;
bool haveTopo = false;

if (psb::TopologyConfig::exists(topologyPath)) {   // topologyPath is always defaultPath() now
    auto loaded = psb::TopologyConfig::load(topologyPath);
    if (!loaded.has_value()) {
        std::cerr << "Topology config error: could not parse " << topologyPath << "\n";
        return 1;
    }
    topo = std::move(*loaded);
    haveTopo = true;
} else {
    auto choice = psb::tui::showModeChoicePopup(screen);
    if (choice == psb::tui::ModeChoice::Cancelled) return 0;
    if (choice == psb::tui::ModeChoice::Single) {
        auto quick = psb::tui::showQuickConnectForm(screen);
        if (!quick.has_value()) return 0;
        topo = *quick;
        haveTopo = true;
    }
}

bool runWizard = !haveTopo;  // was: setupFlag || !haveTopo
```

`--setup`'s old capability (jump straight to the wizard) is still fully
reachable — choosing "Multi-Board Setup" in the popup leaves `haveTopo`
false, triggering the same `runWizard` path `--setup` used to. Nothing
becomes unreachable.

`autoConnectAll`'s `!portArg.empty() ||` clause is dead code once `-p` is
gone — and it changes nothing observable, since the quick-connect path
never made this true anyway (confirmed during Sub-project B's manual
testing: quick-connect always required a manual Connect click). It becomes:

```cpp
bool autoConnectAll = runWizard || topo.totalBoardCount() > 1;
```

## Accepted Behavior Change

Editing an *already-existing* `topology.toml` via the wizard before any
board starts connecting is no longer reachable from a cold launch (that was
`--setup`'s job on top of an existing file). Without the flag, a normal
launch auto-loads and auto-connects that topology immediately; editing now
happens by opening the (already-global) Setup button afterward — identical
to today's mid-session editing, just always a step later than it used to be
in this one specific case. This is a direct, accepted consequence of
removing `--setup` entirely (Confirmed Decisions).

## Error Handling

- A missing or malformed `preferences.toml` is not an error — `main()`
  falls back to the hardcoded defaults (3000ms, 1s) silently, exactly like
  quick-connect's own pre-fill already does for a missing/malformed
  `last_single.toml`.
- Preferences dialog field parsing: invalid input falls back to the
  *current* value (not a hardcoded default), so a stray typo can't silently
  reset an already-customized setting to something the user didn't choose.

## Testing

- `AppPreferences` gets real Catch2 round-trip coverage, mirroring
  `test_topology_config.cpp`'s style (save → load → compare).
- Everything else — CLI flag removal, `main()`'s simplified branch, the
  Preferences dialog's UI and button-merge behavior, `--version` still
  working — is tmux-verified against real hardware, consistent with this
  codebase's established practice for FTXUI-coupled code (Phase 3,
  Sub-projects A and B).
- Specific checks: launching with no args goes straight to Preferences-less
  behavior changes (i.e. identical dashboard flow to before, just without
  CLI shortcuts); Preferences opens pre-filled with current values every
  time (not stale from a prior open in the same session); Save persists and
  applies immediately (poll interval visibly affects live idle-poll timing;
  timeout applies to the next Connect attempt); Cancel discards edits;
  `--version` still prints and exits without opening the UI; passing any of
  the removed flags (e.g. `-p`) produces CLI11's standard "unknown option"
  error rather than being silently ignored.

## Out of Scope

- `psb_demo_cli` — untouched, unrelated tool.
- Any CLI power-user shortcuts / override flags for timeout or poll
  interval — explicitly declined in Confirmed Decisions.
- Making the Preferences dialog reachable from the pre-dashboard mode-select
  popup — the dashboard (reached immediately after quick-connect or the
  wizard, regardless of actual connect success) is always reachable, so a
  4th popup button isn't needed.
