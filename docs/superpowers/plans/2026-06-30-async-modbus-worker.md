# Async Modbus Worker Thread Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the blocking `writeSync` + `pollThread` + `g_scanMutex` model with a single Modbus worker thread that owns all serial I/O, making the FTXUI event thread zero-latency for all Modbus operations.

**Architecture:** A single `modbusWorker` thread owns the Modbus client exclusively (no mutex needed). UI button handlers post `std::function<void()>` work items into a `std::queue` (protected by `workMutex`/`workCv`) and return immediately, so the FTXUI event loop is never blocked. The worker drains all queued write items before running the next periodic poll, giving writes natural priority over polling.

**Tech Stack:** C++17, FTXUI (terminal UI), libmodbus via `HvbModbusClient`.

## Global Constraints

- C++17 minimum — use `std::queue`, `std::condition_variable`, `std::function`, `std::unique_lock`.
- All Modbus calls (`g_client.*`) must execute **only** on `modbusWorker` — never on the UI thread or connect thread after this refactor.
- `data.valid` must remain `std::atomic<bool>` (from previous fix).
- `screen.PostEvent(Event::Custom)` is the only way to trigger a re-render from a non-UI thread — keep all calls to it.
- Do not change FTXUI component structure, key bindings, or visual layout — only the threading model changes.
- All four TUI source files are header-only (`.h`) except `main.cpp`.
- Compilation: `cmake --build build/` from repo root (or equivalent). No test binary exists — correctness verified by running the app.

---

## File Map

| File | Change |
|---|---|
| `tools/hvb_demo_app/tui/widgets.h` | Add `WorkQueue` struct; update `AppState`; replace `writeSync` with `postWrite` |
| `tools/hvb_demo_app/tui/main.cpp` | Add `workQueue/workMutex/workCv`; replace `pollThread`+`g_scanMutex` with `modbusWorker`; update `doConnect`; update `AppState` init; update SysConfig popup callers; fix shutdown |
| `tools/hvb_demo_app/tui/tab_monitor.h` | Replace `writeSync` → `postWrite`; remove unnecessary `readChannelCalConfig` from `refreshCh` |
| `tools/hvb_demo_app/tui/tab_channel.h` | Replace `writeSync` → `postWrite` |
| `tools/hvb_demo_app/tui/tab_system.h` | Replace `writeSync` → `postWrite` |

---

## Task 1 — Update `AppState` and replace `writeSync` with `postWrite` in `widgets.h`

**Files:**
- Modify: `tools/hvb_demo_app/tui/widgets.h`

**Interfaces produced (used by Tasks 2–4):**
- `postWrite(AppState&, ConfigInputs&, const std::string& label, std::function<bool()> writeFn, std::function<void()> refreshFn)` — enqueues a work item and returns immediately. Same call signature as old `writeSync`.
- `AppState::workQueue` — `std::queue<std::function<void()>>&`
- `AppState::workMutex` — `std::mutex&`
- `AppState::workCv` — `std::condition_variable&`

- [ ] **Step 1: Replace `AppState::scanMutex` with work-queue members**

In `widgets.h`, find the `AppState` struct and replace the `scanMutex` field with three new fields:

```cpp
struct AppState {
    hvb::HvbModbusClient&                    client;
    std::atomic<bool>&                       connected;
    ScannedData&                             data;
    std::string&                             statusMsg;
    std::mutex&                              statusMutex;
    std::queue<std::function<void()>>&       workQueue;  // replaces scanMutex
    std::mutex&                              workMutex;
    std::condition_variable&                 workCv;
    ftxui::ScreenInteractive&               screen;
};
```

Add `#include <condition_variable>` and `#include <queue>` to the include block at the top of `widgets.h` (alongside the existing `<mutex>` include).

- [ ] **Step 2: Add `postWrite` — the non-blocking replacement for `writeSync`**

Add the following function immediately after the closing brace of `syncDataToInputs` and before `CommitInput`. Remove the old `writeSync` function entirely at the same time.

```cpp
// Non-blocking write — enqueues work on the Modbus worker thread and returns
// immediately so the FTXUI event loop is never stalled.
inline void postWrite(AppState& s, ConfigInputs& inputs,
                      const std::string& label,
                      std::function<bool()> writeFn,
                      std::function<void()> refreshFn) {
    // Show "Writing..." immediately — the worker hasn't started yet, but the
    // UI thread is free to render this before the worker executes.
    { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Writing " + label + "..."; }
    s.screen.PostEvent(Event::Custom);

    // Capture by value everything the worker needs; capture s by ref only for
    // members that outlive the worker (screen, statusMutex, statusMsg, data, client).
    std::function<void()> item = [&s, &inputs, label, writeFn, refreshFn] {
        bool ok = writeFn();
        if (ok) refreshFn();
        if (ok) {
            syncDataToInputs(s.data, inputs);
            { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "OK: " + label; }
        } else {
            { std::lock_guard<std::mutex> lk(s.statusMutex);
              s.statusMsg = "Error: " + s.client.lastError(); }
        }
        s.screen.PostEvent(Event::Custom);
    };

    { std::lock_guard<std::mutex> lk(s.workMutex); s.workQueue.push(std::move(item)); }
    s.workCv.notify_one();
}
```

- [ ] **Step 3: Verify the file compiles in isolation**

```bash
cd /home/yong/backup/src/xlab/jianwei/hvb_wkspc/hvb_firmware.git
cmake --build build/ 2>&1 | head -40
```

Expected: errors only about `AppState` aggregate-init mismatches in `main.cpp` (because `scanMutex` was removed from the struct) — those are fixed in Task 2. No errors inside `widgets.h` itself.

---

## Task 2 — Replace `pollThread` + `g_scanMutex` with `modbusWorker` in `main.cpp`

**Files:**
- Modify: `tools/hvb_demo_app/tui/main.cpp`

**Interfaces consumed from Task 1:**
- `AppState` now takes `workQueue`, `workMutex`, `workCv` instead of `scanMutex`
- `postWrite(...)` — used for SysConfig popup buttons in this file

- [ ] **Step 1: Remove `g_scanMutex` global; add worker-thread state in `main()`**

Delete the global `static std::mutex g_scanMutex;` line (near the top of the file after the global declarations).

Then, directly after the `pendingSync` atomic declarations that already exist in `main()`, add:

```cpp
    std::queue<std::function<void()>> workQueue;
    std::mutex                        workMutex;
    std::condition_variable           workCv;
```

- [ ] **Step 2: Replace `pollThread` with `modbusWorker`**

Delete the entire `// ---- Poll thread ----` block (the `std::thread pollThread([&] { ... });` declaration).

Replace it with:

```cpp
    // ---- Modbus worker thread ----
    // Owns all serial I/O. Drains write queue first, then polls at g_pollInterval.
    std::thread modbusWorker([&] {
        auto nextPoll = std::chrono::steady_clock::now();
        while (running) {
            std::unique_lock<std::mutex> lk(workMutex);
            workCv.wait_until(lk, nextPoll,
                [&] { return !workQueue.empty() || !running.load(); });

            // Drain all queued writes before polling (writes have implicit priority).
            while (!workQueue.empty()) {
                auto item = std::move(workQueue.front()); workQueue.pop();
                lk.unlock();
                item();
                lk.lock();
            }

            // Poll when interval has elapsed and queue is empty.
            if (running && g_connected &&
                std::chrono::steady_clock::now() >= nextPoll) {
                lk.unlock();
                doPollScan(data);
                data.valid.store(g_client.isConnected());
                if (running) screen.PostEvent(Event::Custom);
                nextPoll = std::chrono::steady_clock::now()
                         + std::chrono::seconds(g_pollInterval);
                lk.lock();
            }
        }
    });
```

- [ ] **Step 3: Update `AppState` initialisation**

Find the `AppState` aggregate-init line and replace `g_scanMutex` with the three new members:

```cpp
    hvb::tui::AppState appState{g_client, g_connected, data,
                                 statusMsg, statusMutex,
                                 workQueue, workMutex, workCv,
                                 screen};
```

- [ ] **Step 4: Update `doConnect` — post `doFullScan` to the worker instead of running it inline**

Inside `doConnect`'s detached thread lambda, find the block:

```cpp
            if (ok) {
                { std::lock_guard<std::mutex> lk(g_scanMutex); doFullScan(data); }
                data.valid = true;
                // Signal the UI thread to apply tabTitles/activeTab/inputs; direct writes
                // from this detached thread would race with FTXUI reads on the UI thread.
                pendingChannelCount.store(data.numChannels(), std::memory_order_release);
                pendingSync.store(true, std::memory_order_release);
            }
```

Replace it with:

```cpp
            if (ok) {
                // Post full scan to worker so it is serialised with any queued writes.
                auto scanItem = [&, connectStart] {
                    if (!running) { connecting = false; return; }
                    doFullScan(data);
                    data.valid.store(true);
                    pendingChannelCount.store(data.numChannels(), std::memory_order_release);
                    pendingSync.store(true, std::memory_order_release);
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - connectStart).count();
                    { std::lock_guard<std::mutex> lk(statusMutex);
                      statusMsg = ""; }  // success shown in menu bar indicator
                    connecting = false;
                    screen.PostEvent(Event::Custom);
                };
                { std::lock_guard<std::mutex> lk(workMutex); workQueue.push(std::move(scanItem)); }
                workCv.notify_one();
                // The worker sets connecting=false and posts Event::Custom after scan.
                // Skip the remainder of this thread — worker takes it from here.
                return;
            }
```

Because the worker now sets `connecting = false` on the success path, also remove the `connecting = false;` and `screen.PostEvent(Event::Custom);` lines that come **after** the modified block (they remain only for the failure path). The failure path structure should now look like:

```cpp
            // Only reached on failure or abort — success returns early above.
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - connectStart).count();
            { std::lock_guard<std::mutex> lk(statusMutex);
              double sec = elapsed / 1000.0;
              if (abortConnect)
                  statusMsg = "Connection aborted";
              else {
                  auto e = g_client.lastError();
                  statusMsg = "Error: " + (e.empty() ? "connection failed" : e)
                              + " (after " + std::to_string(static_cast<int>(sec)) + "s)";
              }
            }
            connecting = false;
            screen.PostEvent(Event::Custom);
```

- [ ] **Step 5: Update SysConfig popup buttons in `main.cpp` to use `postWrite`**

There are five buttons in the SysConfig popup (`scOpMode`, `scStartup`, `scSave`, `scLoad`, `scFactory`) that call `hvb::tui::writeSync(...)`. Change every one of these to `hvb::tui::postWrite(...)` — same arguments, just the function name changes. Example:

```cpp
    auto scOpMode  = hvb::tui::InlineCycler(kOpModes, &inputs.opModeIdx, [&]{
        hvb::tui::postWrite(appState, inputs, "OpMode",
            [&]{ return g_client.writeOperatingMode(static_cast<hvb::OpMode>(inputs.opModeIdx)); },
            [&]{ data.sysCfg = g_client.readSystemConfig(); });
    });
    auto scStartup = hvb::tui::InlineCycler(kStartPol, &inputs.startupIdx, [&]{
        hvb::tui::postWrite(appState, inputs, "StartupPol",
            [&]{ return g_client.writeStartupChannelPolicy((uint16_t)inputs.startupIdx); },
            [&]{ data.sysCfg = g_client.readSystemConfig(); });
    });
    auto scSave    = Button("Save",    [&]{ hvb::tui::postWrite(appState, inputs, "Save",
        [&]{ return g_client.sendParamAction(-1, hvb::ParamAction::Save); },
        [&]{ data.sysCfg = g_client.readSystemConfig(); }); });
    auto scLoad    = Button("Load",    [&]{ hvb::tui::postWrite(appState, inputs, "Load",
        [&]{ return g_client.sendParamAction(-1, hvb::ParamAction::Load); },
        [&]{ data.sysCfg = g_client.readSystemConfig(); }); });
    auto scFactory = Button("Factory", [&]{ hvb::tui::postWrite(appState, inputs, "Factory",
        [&]{ return g_client.sendParamAction(-1, hvb::ParamAction::FactoryReset); },
        [&]{ data.sysCfg = g_client.readSystemConfig(); }); });
```

- [ ] **Step 6: Fix shutdown — join `modbusWorker` instead of `pollThread`**

Find:
```cpp
    running = false;
    if (pollThread.joinable()) pollThread.join();
```

Replace with:
```cpp
    running = false;
    workCv.notify_all();  // unblock worker's wait_until so it can see running=false
    if (modbusWorker.joinable()) modbusWorker.join();
```

Also update the `bQuit` handler to notify the condvar:
```cpp
    auto bQuit = hvb::tui::ActionButton("Quit", [&] {
        running = false;
        workCv.notify_all();
        screen.ExitLoopClosure()();
    });
```

- [ ] **Step 7: Build and verify**

```bash
cmake --build build/ 2>&1 | head -60
```

Expected: errors only in `tab_channel.h`, `tab_monitor.h`, `tab_system.h` about `writeSync` being undefined or `scanMutex` not found in `AppState` — those are fixed in Task 3. `main.cpp` and `widgets.h` should be clean.

---

## Task 3 — Replace `writeSync` → `postWrite` in all tab files; clean up `refreshCh`

**Files:**
- Modify: `tools/hvb_demo_app/tui/tab_channel.h`
- Modify: `tools/hvb_demo_app/tui/tab_monitor.h`
- Modify: `tools/hvb_demo_app/tui/tab_system.h`

**Interfaces consumed from Task 1:**
- `postWrite(AppState&, ConfigInputs&, const std::string&, std::function<bool()>, std::function<void()>)`

- [ ] **Step 1: `tab_channel.h` — replace all `writeSync` calls with `postWrite`**

There are 13 `writeSync(...)` call sites in `tab_channel.h`. Do a find-and-replace of the function name only — arguments are identical:

```bash
sed -i 's/\bwriteSync\b/postWrite/g' \
  /home/yong/backup/src/xlab/jianwei/hvb_wkspc/hvb_firmware.git/tools/hvb_demo_app/tui/tab_channel.h
```

Verify the count:
```bash
grep -c 'postWrite' \
  /home/yong/backup/src/xlab/jianwei/hvb_wkspc/hvb_firmware.git/tools/hvb_demo_app/tui/tab_channel.h
```
Expected: 13

- [ ] **Step 2: `tab_monitor.h` — replace all `writeSync` calls with `postWrite`**

```bash
sed -i 's/\bwriteSync\b/postWrite/g' \
  /home/yong/backup/src/xlab/jianwei/hvb_wkspc/hvb_firmware.git/tools/hvb_demo_app/tui/tab_monitor.h
```

Verify:
```bash
grep -c 'postWrite' \
  /home/yong/backup/src/xlab/jianwei/hvb_wkspc/hvb_firmware.git/tools/hvb_demo_app/tui/tab_monitor.h
```
Expected: 5

- [ ] **Step 3: `tab_system.h` — replace all `writeSync` calls with `postWrite`**

```bash
sed -i 's/\bwriteSync\b/postWrite/g' \
  /home/yong/backup/src/xlab/jianwei/hvb_wkspc/hvb_firmware.git/tools/hvb_demo_app/tui/tab_system.h
```

Verify:
```bash
grep -c 'postWrite' \
  /home/yong/backup/src/xlab/jianwei/hvb_wkspc/hvb_firmware.git/tools/hvb_demo_app/tui/tab_system.h
```
Expected: 7

- [ ] **Step 4: Confirm no remaining `writeSync` references**

```bash
grep -r 'writeSync' \
  /home/yong/backup/src/xlab/jianwei/hvb_wkspc/hvb_firmware.git/tools/hvb_demo_app/tui/
```
Expected: no output.

- [ ] **Step 5: Remove `readChannelCalConfig` from `tab_monitor.h`'s `refreshCh`**

Open `tab_monitor.h` and find the `refreshCh` lambda inside `makeMonitorRow`:

```cpp
    auto refreshCh = [&s, &inputs, ch]() {
        uint16_t caps = s.data.chInfo[ch].chCapFlags;
        s.data.chCfg[ch] = s.client.readChannelConfig(ch, caps);
        s.data.chCalCfg[ch] = s.client.readChannelCalConfig(ch, caps);
        syncDataToInputs(s.data, inputs);
    };
```

Remove the `readChannelCalConfig` line — it adds a full Modbus round-trip after every monitor-tab write but cal config never changes during normal operation:

```cpp
    auto refreshCh = [&s, &inputs, ch]() {
        uint16_t caps = s.data.chInfo[ch].chCapFlags;
        s.data.chCfg[ch] = s.client.readChannelConfig(ch, caps);
        syncDataToInputs(s.data, inputs);
    };
```

- [ ] **Step 6: Full build — must be clean**

```bash
cmake --build build/ 2>&1
```

Expected: zero errors, zero warnings about `writeSync` or `scanMutex`.

- [ ] **Step 7: Confirm no `scanMutex` references remain**

```bash
grep -r 'scanMutex' \
  /home/yong/backup/src/xlab/jianwei/hvb_wkspc/hvb_firmware.git/tools/hvb_demo_app/tui/
```
Expected: no output.

- [ ] **Step 8: Commit**

```bash
cd /home/yong/backup/src/xlab/jianwei/hvb_wkspc/hvb_firmware.git
git add tools/hvb_demo_app/tui/
git commit -m "$(cat <<'EOF'
refactor(tui): async Modbus worker thread — eliminate UI freezes on writes

Replace writeSync+pollThread+scanMutex with a single modbusWorker thread
that owns all serial I/O. Button handlers now post work items to a queue
and return immediately; the FTXUI event loop is never blocked. Writes are
drained before each poll cycle, giving them implicit priority. 'Writing...'
status now renders before the operation executes.

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

---

## Self-Review

**Spec coverage:**
- [x] UI thread never blocks on Modbus — `postWrite` enqueues and returns; worker owns all I/O
- [x] Writes preempt poll — worker drains queue before running `doPollScan`
- [x] "Writing..." renders before write executes — `PostEvent` called before `workQueue.push`
- [x] Single thread owns Modbus client — `g_scanMutex` removed
- [x] `doFullScan` on connect serialised through worker queue
- [x] Shutdown clean — `workCv.notify_all()` + `modbusWorker.join()`
- [x] Detached connect thread lifetime safe — success path returns early; failure path unchanged
- [x] Unnecessary `readChannelCalConfig` in monitor refreshCh removed
- [x] All five tab files updated

**Placeholder scan:** No TBD/TODO/placeholder text found.

**Type consistency:**
- `postWrite` signature is identical across Tasks 1–3: `(AppState&, ConfigInputs&, const std::string&, std::function<bool()>, std::function<void()>)`
- `AppState` members `workQueue/workMutex/workCv` introduced in Task 1, wired in Task 2, consumed transparently in Task 3 via `AppState&` ref
- `data.valid` remains `std::atomic<bool>` throughout — `.store(true)` used correctly
