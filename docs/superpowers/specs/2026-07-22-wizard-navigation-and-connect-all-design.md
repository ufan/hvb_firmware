# Wizard Navigation Cleanup & Connect All/Disconnect All — Design

## Context

Follow-on polish to `psb_demo_tui`'s launch flow and Topology wizard,
requested as a batch of six items:

1. Path picker's `Open` on a file currently auto-loads it — should just set
   the path field, leaving `Load` as an explicit, separate step (the picker
   may be used to choose a save location by picking an arbitrary existing
   file as a landing spot, then hand-editing the filename — the "file"
   might not even be a valid topology file).
2. Delete the wizard's `bDone` button ("Save & Exit"/"Save & Close")
   entirely, from both the standalone and mid-session contexts — `Save`
   plus `Connect Now`/`Apply` already cover what it did.
3. Add always-visible `Connect All`/`Disconnect All` buttons to the
   multi-board global row, right before `Quit`.
4. Center the quick-connect popup (missing `| center`, same bug class as
   the mode-select popup fixed earlier).
5. Rename "Cancel" to "Exit" on the mode-select popup, the quick-connect
   form, and the *standalone* Topology wizard (mid-session Setup keeps
   "Cancel" — it closes a modal, not the app).
6. Add a "Back" button to the quick-connect form and the standalone
   Topology wizard, returning to the mode-selection popup.

Working through item 6 surfaced a real bug: the wizard's `bConnectNow`
label (`"Connect Now"` vs `"Apply"`) is keyed off `allowScan`, which the
previous round of work changed to always be `true` mid-session (to enable
live-bus scanning). Since then, reopening Topology mid-session has
incorrectly shown "Connect Now" instead of "Apply". This spec fixes it as
part of untangling "safe to scan" from "is this the launch flow" — exactly
what item 6 needs anyway.

## Confirmed Decisions

- **Item 2**: `bDone` and `WizardOutcome::SavedOnly` are removed entirely,
  not just relabeled — nothing in either context (standalone or
  mid-session) produces or needs them once `bDone` is gone.
- **Item 3**: two separate, always-visible buttons (`Connect All`,
  `Disconnect All`), each unconditionally acting on every board — not one
  button whose label reflects aggregate state. Positioned past the
  filler, immediately before `Quit`, in the multi-board global row only
  (`board_switcher.h`) — never merged into the single-board row
  (`board_dashboard.h`'s existing Connect/Disconnect already covers the
  single-board case, per the original request's own "falls back
  naturally").
- **Items 5/6 scope**: a new `bool isLaunchFlow` parameter on
  `makeWizardScreen`, independent of `allowScan` — `allowScan` continues
  to mean only "safe to scan"; `isLaunchFlow` means "this is the
  pre-dashboard launch flow, not a mid-session reopen" and drives
  `bConnectNow`'s label, `bCancel`'s label, and `bBack`'s visibility.
  "Back" is only added at the wizard's own top level — the Add Bus/Add
  Board sub-modals keep their existing `Cancel` unchanged, not part of
  the launch-selection flow.

## Architecture

### `mode_select.h` — tri-state outcomes

```cpp
enum class ModeChoice { Single, Multi, Exit };  // was Cancelled -- renamed for clarity, same 3 cases
inline ModeChoice showModeChoicePopup(ScreenInteractive& screen);

enum class QuickConnectOutcome { Connected, Back, Exit };
struct QuickConnectResult {
    QuickConnectOutcome outcome;
    psb::TopologyConfig topo;  // valid only when outcome == Connected
};
inline QuickConnectResult showQuickConnectForm(ScreenInteractive& screen);
```

`showModeChoicePopup`'s `Cancel` button is renamed `Exit`, its enum case
renamed to match (`Cancelled` → `Exit`) for a consistent vocabulary across
this whole flow. `showQuickConnectForm` gains a `Back` button between
`Connect` and `Exit`; `| center` is added to its popup's returned Element
(item 4).

### `wizard_screen.h` — `isLaunchFlow`, `bBack`, `bDone` removed

```cpp
inline Component makeWizardScreen(WizardState& s, ScreenInteractive& screen,
                                  std::function<void(WizardOutcome)> onFinish,
                                  bool allowScan = true,
                                  ScanViaLiveBus scanViaLiveBus = {},
                                  bool isLaunchFlow = true);
```

`WizardOutcome` becomes `{ Cancelled, ConnectNow, Back }` (`SavedOnly`
removed). `bDone` ("Save & Exit"/"Save & Close") is deleted entirely —
construction, `mainContainer` membership, rendering, and Renderer capture
all removed. `bConnectNow`'s label switches from `allowScan ? ... : ...`
to `isLaunchFlow ? "Connect Now" : "Apply"` (the bug fix). `bCancel`'s
label becomes `isLaunchFlow ? "Exit" : "Cancel"` (item 5). A new `bBack`
(`ActionButton("Back", [onFinish] { onFinish(WizardOutcome::Back); })`) is
constructed unconditionally but only added to `mainContainer` and rendered
when `isLaunchFlow` is true — mirroring the exact pattern already used to
gate scan-related components on `allowScan`.

### `board_switcher.h` — `Connect All`/`Disconnect All`

`makeBoardSwitcher` gains two new `Component` parameters,
`globalConnectAll`/`globalDisconnectAll`, added to `globalMenuBar`'s
children (for Tab order) and rendered in the multi-board global row's
`hbox`, past the `filler()`, immediately before `globalQuit`:

```cpp
hbox({
    globalSetup->Render(), text(" "), globalPreferences->Render(),
    filler(),
    globalConnectAll->Render(), text(" "), globalDisconnectAll->Render(), text(" "),
    globalQuit->Render(),
})
```

Not threaded into `board_dashboard.h`'s single-board merge — the single
board's own `Connect`/`Disconnect` toggle already covers that case.

### `main.cpp` — implementation and the launch-flow loop

`bGlobalConnectAll`/`bGlobalDisconnectAll` iterate `rt.boards`, calling
each board's existing connect/disconnect machinery — reusing exactly the
same per-board `doConnect`/`doDisconnect` logic `board_dashboard.h`
already has, not a new implementation. Since that logic is currently
private to each board's own dashboard closure, `BoardSession` gains two
small `std::function<void()>` members (`connect`/`disconnect`, populated
by `makeBoardDashboard` when it builds `doConnect`/`doDisconnect`) that
`main.cpp` can call from outside — the minimal-surface way to reach
per-board connection logic without duplicating it or restructuring
ownership.

Topology resolution becomes an explicit loop, replacing the old
popup-once-then-maybe-wizard sequence:

```cpp
psb::TopologyConfig topo;
bool haveTopo = false;

for (;;) {
    auto choice = psb::tui::showModeChoicePopup(screen);
    if (choice == psb::tui::ModeChoice::Exit) return 0;
    if (choice == psb::tui::ModeChoice::Single) {
        auto quick = psb::tui::showQuickConnectForm(screen);
        if (quick.outcome == psb::tui::QuickConnectOutcome::Exit) return 0;
        if (quick.outcome == psb::tui::QuickConnectOutcome::Back) continue;
        topo = quick.topo;
        haveTopo = true;
        break;
    }
    // Multi: run the wizard right here (not deferred) so Back can loop
    // cleanly back to mode selection.
    psb::tui::WizardState wiz;
    wiz.topologyPath = topologyPath;
    psb::tui::WizardOutcome outcome = psb::tui::WizardOutcome::Cancelled;
    auto wizardRoot = psb::tui::makeWizardScreen(wiz, screen, [&](psb::tui::WizardOutcome o) {
        outcome = o;
        screen.ExitLoopClosure()();
    }, /*allowScan=*/true, /*scanViaLiveBus=*/{}, /*isLaunchFlow=*/true);
    screen.Loop(wizardRoot);

    if (outcome == psb::tui::WizardOutcome::Back) continue;
    if (outcome == psb::tui::WizardOutcome::Cancelled) return 0;
    topo = wiz.topo;
    haveTopo = true;
    if (topo.totalBoardCount() == 0) {
        std::cerr << "Topology has no boards configured — exiting.\n";
        return 0;
    }
    break;
}
```

This replaces the old separate `bool runWizard = !haveTopo; if (runWizard) { ... }`
block entirely — the wizard invocation moves inside the loop. The old
"Cancelled but `haveTopo` was already true" fallback-to-existing-topo
branch is dropped as genuinely dead code: `runWizard` could only ever be
`true` when `haveTopo` was `false` (single-board quick-connect is the only
path that sets it `true` before reaching that block, and it never reaches
the wizard block at all), so that case could never actually occur — the
new loop makes this invariant explicit instead of leaving it as an
unreachable comment.

The mid-session `makeWizardScreen` call site passes `isLaunchFlow=false`
alongside its existing `allowScan=true, scanViaLiveBus` — the fix for the
`bConnectNow` mislabeling bug.

## Error Handling

- No new error paths. `Back`/`Exit` are plain navigation, not failure
  states — nothing to report beyond what already exists.

## Testing

Consistent with this codebase's established practice: no meaningful unit-
test surface (FTXUI/threading-coupled UI); verification is tmux-driven
against real hardware, protecting any real `~/.psb_demo_app/topology.toml`
the same way prior rounds did.

- Path picker: select a file, confirm the path field updates but the
  Buses/Boards panels do *not* change until `Load` is clicked separately.
- Standalone wizard: confirm `Save & Exit` is gone, `bBack` returns to the
  mode popup, `Exit` (renamed from Cancel) still exits the app when there's
  no prior topology.
- Mid-session Setup: confirm it now correctly shows `Apply`/`Cancel` (not
  `Connect Now`/`Exit`), `Save & Close` is gone, and no `Back` button
  appears (nothing to go back to mid-session).
- Quick-connect: confirm it's centered, `Back` returns to the mode popup
  (and a subsequent Single Board choice re-shows quick-connect fresh),
  `Exit` (renamed from Cancel) exits the app.
- Multi-board dashboard: confirm `Connect All`/`Disconnect All` appear
  past the filler, right before `Quit`; clicking each acts on every board
  regardless of individual state; single-board dashboard shows neither
  (only its own existing Connect/Disconnect).

## Out of Scope

- The empty-board fallback UI (still deferred from the previous round).
- Any change to the Add Bus/Add Board sub-modals' own `Cancel` buttons.
