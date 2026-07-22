# Connection Success Requires a Complete Channel Scan Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `board.connected` (and everything that reads it — status dots, the sidebar indicator, toggle labels) only become `true` once a board has successfully discovered all its channels, not merely once the Modbus protocol handshake passes.

**Architecture:** `doFullScan`'s abort-detection parameter changes from `connected` to `abortConnect` (a flag that already exists and already means "this connect attempt was cancelled," independent of when `connected` itself changes). All three places that establish a connection (`board_dashboard.h`'s `doConnect`, `main.cpp`'s initial `autoConnectAll` sweep, and `main.cpp`'s `applyNewBoardsLive`) move their `board.connected = ...` assignment to after the scan, gated on `ScannedData::allChannelsLoaded()` (already used elsewhere to decide when the Monitor table reveals itself).

**Tech Stack:** C++17, FTXUI (vendored), the existing `BoardSession`/`ScannedData`/`BusWorker` machinery from prior rounds.

## Global Constraints

- `ScannedData::allChannelsLoaded()` is reused exactly as it already behaves (returns `false` whenever `numChannels() == 0`) — no new per-channel-read verification is added.
- All three connect-establishing call sites get the same treatment for consistency, per the approved design.
- Tasks 1–4 leave the tree in an intermediate state that builds cleanly but is only behaviorally *complete* once all four have landed (Task 1 changes what `doFullScan`'s first parameter means; Tasks 2–4 are what make each call site actually pass the right thing and gate `connected` correctly). This mirrors this codebase's established "not yet wired" multi-commit pattern — don't stop and manually test after Task 1 alone.

---

## Task 1: `board_session.h` — `doFullScan`'s abort signal

**Files:**
- Modify: `tools/psb_demo_app/tui/board_session.h`

**Interfaces:**
- Produces: `doFullScan(PsbBoardSession&, std::atomic<bool>& abortConnect, ScannedData&, ScreenInteractive&, std::atomic<bool>& running)` — the second parameter's meaning changes from "is this board currently marked connected" to "has this connect attempt been cancelled." Callers (Tasks 2–4) must pass their board's `abortConnect` member, not `connected`.

- [ ] **Step 1: Change the parameter and its internal use**

Edit `tools/psb_demo_app/tui/board_session.h`, find:

```cpp
inline void doFullScan(PsbBoardSession& client, std::atomic<bool>& connected,
                       ScannedData& data, ftxui::ScreenInteractive& screen,
                       std::atomic<bool>& running) {
    for (int ch = 0; ch < MAX_CHANNELS; ++ch) {
        data.chLoaded[ch] = false;
        data.chDetailLoaded[ch] = false;
    }

    data.sysInfo = readWithRetry(client, [&] { return client.readSystemInfo(); });
    data.lastSysUpdate = std::chrono::steady_clock::now();
    data.sysCfg  = readWithRetry(client, [&] { return client.readSystemConfig(); });
    int n = data.numChannels();
    // Gate on `connected`, not an unconditional true — if the user hits
    // Disconnect while this (queued) scan is running, `connected` already
    // flipped false on the UI thread, and this must not resurrect `valid`
    // out from under it.
    data.valid = connected.load();
    data.scanProgress = 0;
```

Change to:

```cpp
inline void doFullScan(PsbBoardSession& client, std::atomic<bool>& abortConnect,
                       ScannedData& data, ftxui::ScreenInteractive& screen,
                       std::atomic<bool>& running) {
    for (int ch = 0; ch < MAX_CHANNELS; ++ch) {
        data.chLoaded[ch] = false;
        data.chDetailLoaded[ch] = false;
    }

    data.sysInfo = readWithRetry(client, [&] { return client.readSystemInfo(); });
    data.lastSysUpdate = std::chrono::steady_clock::now();
    data.sysCfg  = readWithRetry(client, [&] { return client.readSystemConfig(); });
    int n = data.numChannels();
    // Gate on abortConnect, not connected — connected no longer flips true
    // until *after* this scan succeeds (see each call site), so it can't be
    // used as an "was this scan invalidated mid-flight" signal anymore.
    // abortConnect already means exactly that: set true by doDisconnect if
    // the user bails out of an in-flight connect (board_dashboard.h), left
    // false throughout for call sites that aren't interruptible.
    data.valid = !abortConnect.load();
    data.scanProgress = 0;
```

- [ ] **Step 2: Build to confirm no breakage**

Run: `cd tools && cmake --build build --target psb_demo_tui -j$(nproc) 2>&1 | tail -20`
Expected: clean build. The parameter's type (`std::atomic<bool>&`) is unchanged, only its name and meaning — every existing call site still compiles (they still pass a `std::atomic<bool>&`, just the wrong one semantically until Tasks 2–4 update them). This is expected and intentional at this point in the plan — see this plan's Global Constraints.

- [ ] **Step 3: Commit**

```bash
git add tools/psb_demo_app/tui/board_session.h
git commit -m "refactor(psb_demo_tui): doFullScan reads abortConnect, not connected, for its abort signal (callers not yet updated)"
```

---

## Task 2: `board_dashboard.h` — gate `doConnect`'s success on the full scan

**Files:**
- Modify: `tools/psb_demo_app/tui/board_dashboard.h`

**Interfaces:**
- Consumes: `doFullScan`'s new signature (Task 1).
- Produces: `board.connected` is set exactly once per connect attempt, after the scan, using `board.data.allChannelsLoaded() && !board.abortConnect` as the success criterion.

- [ ] **Step 1: Restructure the queued connect work item**

Edit `tools/psb_demo_app/tui/board_dashboard.h`, find:

```cpp
              board.client->rebind(slave);
              bool ok = board.bus->isConnected() || board.bus->connect(board.portVal, baud, timeoutMs);
              if (ok) ok = board.client->verifyProtocol();
              board.connected = ok && !board.abortConnect;
              if (board.abortConnect) { ok = false; }
              if (!running) { board.connecting = false; return; }
              if (ok) {
                  doFullScan(*board.client, board.connected, board.data, screen, running);
                  board.data.valid = board.connected.load();
                  board.pendingChannelCount.store(board.data.numChannels(), std::memory_order_release);
                  board.pendingSync.store(true, std::memory_order_release);
              }
              auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - board.connectStart).count();
              { std::lock_guard<std::mutex> lk(board.statusMutex);
                if (board.abortConnect)
                    board.statusMsg = "Connection aborted";
                else if (ok)
                    board.statusMsg = "";
                else {
                    auto e = board.client->lastError();
                    board.statusMsg = "Error: " + (e.empty() ? "connection failed" : e)
                                + " (after " + std::to_string(static_cast<int>(elapsed / 1000.0)) + "s)";
                }
              }
              board.connecting = false;
              screen.PostEvent(Event::Custom);
```

Change to:

```cpp
              board.client->rebind(slave);
              bool ok = board.bus->isConnected() || board.bus->connect(board.portVal, baud, timeoutMs);
              if (ok) ok = board.client->verifyProtocol();
              if (board.abortConnect) ok = false;
              if (!running) { board.connecting = false; return; }
              // A board only counts as connected once it has discovered all
              // its channels, not merely once the protocol handshake
              // passes — otherwise every UI surface reading board.connected
              // (status dot, sidebar indicator, toggle label) would show
              // "connected" while still mid-scan. board.connected is set
              // exactly once, below, from this fuller ok.
              if (ok) {
                  doFullScan(*board.client, board.abortConnect, board.data, screen, running);
                  ok = board.data.allChannelsLoaded() && !board.abortConnect;
                  board.pendingChannelCount.store(board.data.numChannels(), std::memory_order_release);
                  board.pendingSync.store(true, std::memory_order_release);
              }
              board.connected = ok;
              board.data.valid = board.connected.load();
              auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - board.connectStart).count();
              { std::lock_guard<std::mutex> lk(board.statusMutex);
                if (board.abortConnect)
                    board.statusMsg = "Connection aborted";
                else if (ok)
                    board.statusMsg = "";
                else {
                    auto e = board.client->lastError();
                    board.statusMsg = "Error: " + (e.empty() ? "connection failed" : e)
                                + " (after " + std::to_string(static_cast<int>(elapsed / 1000.0)) + "s)";
                }
              }
              board.connecting = false;
              screen.PostEvent(Event::Custom);
```

- [ ] **Step 2: Build**

Run: `cd tools && cmake --build build --target psb_demo_tui -j$(nproc) 2>&1 | tail -20`
Expected: clean build.

- [ ] **Step 3: Commit**

```bash
git add tools/psb_demo_app/tui/board_dashboard.h
git commit -m "fix(psb_demo_tui): doConnect only marks a board connected after a full successful channel scan"
```

---

## Task 3: `main.cpp` — gate the initial `autoConnectAll` sweep the same way

**Files:**
- Modify: `tools/psb_demo_app/tui/main.cpp`

**Interfaces:**
- Consumes: `doFullScan`'s new signature (Task 1).

- [ ] **Step 1: Restructure the per-board loop in `buildRuntime`'s initial sweep**

Edit `tools/psb_demo_app/tui/main.cpp`, find:

```cpp
                for (psb::tui::BoardSession* b : initialBoards) {
                    b->connecting = true;
                    screen.PostEvent(Event::Custom);
                    bool ok = busOk && b->client->verifyProtocol();
                    b->connected = ok;
                    b->connecting = false;
                    { std::lock_guard<std::mutex> lk(b->statusMutex);
                      b->statusMsg = ok ? "" : "Error: " + (busOk ? b->client->lastError() : bw.bus->lastError()); }
                    if (ok) {
                        psb::tui::doFullScan(*b->client, b->connected, b->data, screen, running);
                        b->data.valid = b->connected.load();
                        b->pendingChannelCount.store(b->data.numChannels(), std::memory_order_release);
                        b->pendingSync.store(true, std::memory_order_release);
                    }
                    screen.PostEvent(Event::Custom);
                }
```

Change to:

```cpp
                for (psb::tui::BoardSession* b : initialBoards) {
                    b->connecting = true;
                    screen.PostEvent(Event::Custom);
                    bool ok = busOk && b->client->verifyProtocol();
                    if (b->abortConnect) ok = false;
                    // b->connecting stays true through the scan below (not
                    // cleared right after verifyProtocol, as a prior version
                    // of this loop did) — this dashboard is already live and
                    // interactive at this point (built earlier in
                    // buildRuntime, before this sweep runs on the bus
                    // worker thread), so a fast user can reach the toggle
                    // button's Abort branch, which itself checks
                    // board.connecting.load() to decide whether a click sets
                    // abortConnect. Clearing connecting early would make a
                    // multi-second scan look fully idle instead of
                    // in-progress, and would make Abort unreachable during
                    // that window.
                    if (ok) {
                        psb::tui::doFullScan(*b->client, b->abortConnect, b->data, screen, running);
                        ok = b->data.allChannelsLoaded() && !b->abortConnect;
                        b->pendingChannelCount.store(b->data.numChannels(), std::memory_order_release);
                        b->pendingSync.store(true, std::memory_order_release);
                    }
                    b->connected = ok;
                    b->data.valid = b->connected.load();
                    b->connecting = false;
                    { std::lock_guard<std::mutex> lk(b->statusMutex);
                      b->statusMsg = b->abortConnect ? "Connection aborted"
                                   : ok ? "" : "Error: " + (busOk ? b->client->lastError() : bw.bus->lastError()); }
                    screen.PostEvent(Event::Custom);
                }
```

- [ ] **Step 2: Build**

Run: `cd tools && cmake --build build --target psb_demo_tui -j$(nproc) 2>&1 | tail -20`
Expected: clean build.

- [ ] **Step 3: Commit**

```bash
git add tools/psb_demo_app/tui/main.cpp
git commit -m "fix(psb_demo_tui): the initial connect-all-boards sweep only marks a board connected after a full scan"
```

---

## Task 4: `main.cpp` — gate `applyNewBoardsLive`'s hot-attach the same way

**Files:**
- Modify: `tools/psb_demo_app/tui/main.cpp`

**Interfaces:**
- Consumes: `doFullScan`'s new signature (Task 1).

- [ ] **Step 1: Restructure the queued connect work item in `applyNewBoardsLive`**

Edit `tools/psb_demo_app/tui/main.cpp`, find:

```cpp
              bwPtr->workQueue.push([bPtr, &screen, &running] {
                  bool ok = bPtr->client->verifyProtocol();
                  bPtr->connected = ok;
                  { std::lock_guard<std::mutex> lk2(bPtr->statusMutex);
                    bPtr->statusMsg = ok ? "" : "Error: " + bPtr->client->lastError(); }
                  if (ok) {
                      psb::tui::doFullScan(*bPtr->client, bPtr->connected, bPtr->data, screen, running);
                      bPtr->data.valid = bPtr->connected.load();
                      bPtr->pendingChannelCount.store(bPtr->data.numChannels(), std::memory_order_release);
                      bPtr->pendingSync.store(true, std::memory_order_release);
                  }
                  screen.PostEvent(Event::Custom);
              });
```

Change to:

```cpp
              bwPtr->workQueue.push([bPtr, &screen, &running] {
                  bool ok = bPtr->client->verifyProtocol();
                  if (bPtr->abortConnect) ok = false;
                  if (ok) {
                      psb::tui::doFullScan(*bPtr->client, bPtr->abortConnect, bPtr->data, screen, running);
                      ok = bPtr->data.allChannelsLoaded() && !bPtr->abortConnect;
                      bPtr->pendingChannelCount.store(bPtr->data.numChannels(), std::memory_order_release);
                      bPtr->pendingSync.store(true, std::memory_order_release);
                  }
                  bPtr->connected = ok;
                  bPtr->data.valid = bPtr->connected.load();
                  { std::lock_guard<std::mutex> lk2(bPtr->statusMutex);
                    bPtr->statusMsg = bPtr->abortConnect ? "Connection aborted"
                                     : ok ? "" : "Error: " + bPtr->client->lastError(); }
                  screen.PostEvent(Event::Custom);
              });
```

- [ ] **Step 2: Build**

Run: `cd tools && cmake --build build --target psb_demo_tui -j$(nproc) 2>&1 | tail -20`
Expected: clean build.

- [ ] **Step 3: Commit**

```bash
git add tools/psb_demo_app/tui/main.cpp
git commit -m "fix(psb_demo_tui): mid-session hot-attach only marks a board connected after a full scan"
```

---

## Task 5: Full verification

**Files:** none (verification only).

- [ ] **Step 1: Clean rebuild of every touched target**

Run: `cd tools && cmake --build build --target psb_modbus_core psb_tests psb_demo_cli psb_demo_tui 2>&1 | tail -20`
Expected: clean build.

- [ ] **Step 2: Full test suite**

Run: `./build/psb_modbus_core/tests/psb_tests 2>&1 | tail -6`
Expected: PASS, 382 assertions / 107 test cases — unchanged (no `psb_modbus_core` files touched by this plan).

- [ ] **Step 3: Manual tmux verification**

Following `docs/guide/client-architecture-and-pitfalls.md` §3's methodology, and protecting any real `~/.psb_demo_app/topology.toml` the same way every prior round did (back it up, diff it after testing):

1. Connect a multi-board session (via the standalone wizard's "Connect Now", which exercises Task 3's initial sweep). While boards are mid-scan ("Scanning channels... X/N" visible on the active tab), confirm the sidebar shows the yellow half-circle ◐ (connecting), not green — and confirm the per-board dashboard's own status dot/menu bar also do not show green until the scan finishes and the channel columns actually appear.
2. Once fully connected, confirm the sidebar turns green ● and the dashboard shows live channel data — matching pre-fix behavior for the *end state*, just correctly delayed until the scan is done.
3. Disconnect one board, then reconnect it (exercises Task 2's `doConnect` path) — confirm the same non-green-until-scanned behavior, and confirm it still ends up green and fully populated (regression check against the previous round's race-condition fix — this plan must not reintroduce it, since Task 2 keeps the connect I/O and scan inside the same queued work item).
4. If a topology file with an Add Board flow is convenient, hot-attach a board mid-session (Setup → Add Board → Apply) to exercise Task 4's path — confirm the newly attached board also shows non-green until its scan completes.
5. Click Abort on a board while it's mid-scan (if timing allows — real hardware may scan too fast to catch this window; acceptable to verify by code inspection of the `abortConnect` checks added in Tasks 2–4 if not reproducible manually) — confirm it ends up showing "Connection aborted" and gray/idle, not stuck or crashed.

- [ ] **Step 4: Confirm the real topology file, if one exists, is untouched**

```bash
diff ~/.psb_demo_app/topology.toml /path/to/your/backup/user_topology_backup.toml
```

Expected: identical, or no real file existed before testing began.
