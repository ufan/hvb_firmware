# Sidebar Board Connection Indicator Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a colored connection-status symbol in front of each board's nickname in `psb_demo_tui`'s multi-board vertical sidebar, reusing the exact connection/staleness logic the per-board dashboard's own status dot already computes.

**Architecture:** Promote the dashboard's local `kSysStaleThreshold` constant to namespace scope in `tui_format.h` (next to the `sysStale()` method it parameterizes) so both the dashboard and the sidebar reference the same definition. Give the sidebar's `Menu` a richer `entries_option.transform` that looks up each entry's `BoardSession` by nickname and prepends a colored one-glyph symbol, computed from the same `connected`/`connecting`/`sysStale`/`statusMsg` fields the dashboard's own status dot already reads — no new state on `BoardSession`.

**Tech Stack:** C++17, FTXUI (vendored), the existing `BoardSession`/`ScannedData` structs from prior rounds.

## Global Constraints

- The five-state mapping is fixed by the approved design — do not add, remove, or reorder states: connected+fresh → green ● (`connected && !sysStale`); connected+stale → red ■ (`connected && sysStale`); connecting → yellow ◐ (`connecting`); connection failed → red ■ (`!connected && !connecting && statusMsg contains "Error"`); idle/never connected → gray ○ (everything else).
- No true animation for the connecting state — a static glyph, confirmed acceptable by the user.
- This only adds a second, independent indicator to the sidebar — the per-board dashboard's own status dot (`board_dashboard.h`'s `connDotEl`) is untouched.

---

## Task 1: `tui_format.h` — promote `kSysStaleThreshold` to namespace scope

**Files:**
- Modify: `tools/psb_demo_app/tui/tui_format.h`

**Interfaces:**
- Produces: `inline constexpr auto kSysStaleThreshold = std::chrono::seconds(10);` at `psb::tui` namespace scope, usable from any file that includes `tui_format.h` (directly or transitively via `board_session.h`).

- [ ] **Step 1: Add the shared constant**

Edit `tools/psb_demo_app/tui/tui_format.h`, find:

```cpp
namespace psb::tui {

static constexpr int MAX_CHANNELS = static_cast<int>(psb::reg::MAX_CHANNELS);

struct ScannedData {
```

Change to:

```cpp
namespace psb::tui {

static constexpr int MAX_CHANNELS = static_cast<int>(psb::reg::MAX_CHANNELS);

// How long since the last successful system-status poll before a board is
// treated as "connected but not actually responding" rather than genuinely
// live — shared by the per-board dashboard's own status dot
// (board_dashboard.h) and the sidebar's connection indicator
// (board_switcher.h), so both agree on what "stale" means.
inline constexpr auto kSysStaleThreshold = std::chrono::seconds(10);

struct ScannedData {
```

- [ ] **Step 2: Build to confirm no breakage**

Run: `cd tools && cmake --build build --target psb_demo_tui -j$(nproc) 2>&1 | tail -20`
Expected: clean build — this is purely additive (a new unused-so-far constant), nothing references it yet.

- [ ] **Step 3: Commit**

```bash
git add tools/psb_demo_app/tui/tui_format.h
git commit -m "feat(psb_demo_tui): promote kSysStaleThreshold to namespace scope (not yet wired to board_switcher.h)"
```

---

## Task 2: `board_dashboard.h` — use the shared constant

**Files:**
- Modify: `tools/psb_demo_app/tui/board_dashboard.h`

**Interfaces:**
- Consumes: `kSysStaleThreshold` from `tui_format.h` (Task 1), reached transitively via this file's existing `#include "board_session.h"`.

- [ ] **Step 1: Remove the now-redundant local definition**

Edit `tools/psb_demo_app/tui/board_dashboard.h`, find:

```cpp
        static constexpr auto kSysStaleThreshold = std::chrono::seconds(10);
        bool sysStale = board.connected.load() && board.data.sysStale(kSysStaleThreshold);
```

Change to:

```cpp
        bool sysStale = board.connected.load() && board.data.sysStale(kSysStaleThreshold);
```

- [ ] **Step 2: Build to confirm it resolves to the same constant**

Run: `cd tools && cmake --build build --target psb_demo_tui -j$(nproc) 2>&1 | tail -20`
Expected: clean build. `board_dashboard.h` includes `board_session.h` (line 3), which includes `tui_format.h` (line 3 of `board_session.h`) — the namespace-scope `kSysStaleThreshold` from Task 1 is visible without a new include.

- [ ] **Step 3: Commit**

```bash
git add tools/psb_demo_app/tui/board_dashboard.h
git commit -m "refactor(psb_demo_tui): use the shared kSysStaleThreshold instead of a local duplicate"
```

---

## Task 3: `board_switcher.h` — add the per-board connection indicator

**Files:**
- Modify: `tools/psb_demo_app/tui/board_switcher.h`

**Interfaces:**
- Consumes: `kSysStaleThreshold` (Task 1); `BoardSession::nickname/connected/connecting/data/statusMsg/statusMutex` (all pre-existing, unchanged).
- Produces: no new interfaces — this is a rendering-only change to `makeBoardSwitcher`'s internal `switcherOpt.entries_option.transform`.

- [ ] **Step 1: Add `<mutex>` to the include list**

Edit `tools/psb_demo_app/tui/board_switcher.h`, find:

```cpp
#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <vector>
```

Change to:

```cpp
#include <algorithm>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
```

(`<mutex>` is already reached transitively via `board_session.h`, but this file now uses `std::lock_guard`/`std::mutex` directly — don't rely on the transitive include.)

- [ ] **Step 2: Replace the switcher's entry transform with one that includes the connection dot**

Find:

```cpp
    MenuOption switcherOpt = MenuOption::Vertical();
    switcherOpt.entries_option.transform = [](const EntryState& e) -> Element {
        auto t = text("  " + e.label + "  ");
        if (e.active)                t = t | bold | color(Color::Cyan);
        if (e.focused && !e.active)  t = t | inverted;
        if (!e.active && !e.focused) t = t | dim;
        return t;
    };
    auto switcherBar = Menu(boardNames.get(), activeBoard.get(), switcherOpt);
```

Change to:

```cpp
    MenuOption switcherOpt = MenuOption::Vertical();
    // Looks up e.label (the nickname) in `boards` to read that board's live
    // connection state — Menu only ever hands the transform a label/active/
    // focused triple, never an index or the BoardSession itself, so a
    // by-nickname scan is the only way in. Boards are few in practice
    // (single digits), and this mirrors detachBoard's own by-nickname
    // lookup below. `boards` is `rt.boards` from main.cpp, captured by
    // reference the same way main.cpp's own Connect All/Disconnect All
    // buttons already capture it — safe without an extra lock: mutations to
    // the vector itself (add/remove) are, like this transform's reads, only
    // ever issued from the single UI thread, so they can't interleave with
    // this scan.
    //
    // Five states, reusing the exact fields/thresholds the per-board
    // dashboard's own status dot (board_dashboard.h) already computes, so
    // the two never disagree about what "connected" or "stale" means:
    //   green  ● : connected, polling normal
    //   red    ■ : connected, but data gone stale (polling failed)
    //   yellow ◐ : connecting (static glyph — no redraw ticker exists
    //              during an in-flight connect attempt for this to animate)
    //   red    ■ : not connected/connecting, last status was an error
    //              (connection failed)
    //   gray   ○ : not connected/connecting, no error (idle / never
    //              connected) — the loop's starting value below
    switcherOpt.entries_option.transform = [&boards](const EntryState& e) -> Element {
        Element dot = text("\xe2\x97\x8b ") | color(Color::GrayDark);  // ○
        for (auto& b : boards) {
            if (b->nickname != e.label) continue;
            bool connected = b->connected.load();
            bool connecting = b->connecting.load();
            bool stale = connected && b->data.sysStale(kSysStaleThreshold);
            if (connected && !stale) {
                dot = text("\xe2\x97\x8f ") | color(Color::Green);   // ●
            } else if (connected && stale) {
                dot = text("\xe2\x96\xa0 ") | color(Color::Red);     // ■
            } else if (connecting) {
                dot = text("\xe2\x97\x90 ") | color(Color::Yellow);  // ◐
            } else {
                std::lock_guard<std::mutex> lk(b->statusMutex);
                if (b->statusMsg.find("Error") != std::string::npos)
                    dot = text("\xe2\x96\xa0 ") | color(Color::Red); // ■
            }
            break;
        }
        auto t = hbox({ text("  "), dot, text(e.label + "  ") });
        if (e.active)                t = t | bold | color(Color::Cyan);
        if (e.focused && !e.active)  t = t | inverted;
        if (!e.active && !e.focused) t = t | dim;
        return t;
    };
    auto switcherBar = Menu(boardNames.get(), activeBoard.get(), switcherOpt);
```

- [ ] **Step 3: Build**

Run: `cd tools && cmake --build build --target psb_demo_tui -j$(nproc) 2>&1 | tail -30`
Expected: clean build.

- [ ] **Step 4: Commit**

```bash
git add tools/psb_demo_app/tui/board_switcher.h
git commit -m "feat(psb_demo_tui): add per-board connection indicator to the sidebar"
```

---

## Task 4: Full verification

**Files:** none (verification only).

- [ ] **Step 1: Run the existing test suite**

Run: `./build/psb_modbus_core/tests/psb_tests 2>&1 | tail -6`
Expected: PASS, 382 assertions / 107 test cases — unchanged (no `psb_modbus_core` files touched by this plan).

- [ ] **Step 2: Manual tmux verification of all five states**

Following `docs/guide/client-architecture-and-pitfalls.md` §3's methodology, and protecting any real `~/.psb_demo_app/topology.toml` the same way every prior round did (back it up, diff it after testing):

1. Launch a multi-board session (2+ boards, via the wizard — the test topology file used by prior rounds, `~/.psb_demo_app/topology-test.toml`, works if present; otherwise build one via Add Bus/Add Board).
2. **Idle / never connected**: before clicking Connect, confirm each unconnected board's sidebar entry shows a gray hollow circle ○ in front of its nickname.
3. **Connecting**: click Connect (or Connect All) and, in the brief window before it resolves, confirm the entry shows a yellow half-circle ◐. (This window is short on real hardware — a slow/incorrect port is one way to widen it if needed for the screenshot.)
4. **Connected, polling normal**: once connected, confirm the entry shows a green filled circle ● and that it matches the currently-active board's own dashboard status dot (also green).
5. **Connected, polling failed (stale)**: harder to trigger on demand against real hardware (needs ~10s of the board going unresponsive while still marked connected) — acceptable to verify by code review of the condition (`connected && sysStale`) plus confirming the dashboard's own equivalent red-dot state already exists and is exercised in prior rounds, rather than forcing this exact timing manually.
6. **Connection failed**: attempt to connect to a port that will fail (e.g., an already-in-use or nonexistent port) and confirm the entry shows a red square ■ after the attempt resolves.
7. **Disconnect All / manual disconnect**: after disconnecting a previously-connected board, confirm its entry returns to gray hollow circle ○ (clean disconnect clears `statusMsg` to a non-error value, per `doDisconnect`'s existing "Disconnected" status message — confirm this doesn't itself contain "Error" and therefore doesn't misroute to the red state).
8. Confirm the active/focused styling (bold+cyan when active, inverted when focused-not-active, dim otherwise) still looks correct with the new dot in place — no misalignment or color bleed between the dot and the surrounding row styling.

- [ ] **Step 3: Confirm the real topology file, if one exists, is untouched**

```bash
diff ~/.psb_demo_app/topology.toml /path/to/your/backup/user_topology_backup.toml
```

Expected: identical, or no real file existed before testing began.
