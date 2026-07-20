# Multi-Board Topology — Phase 2 (TUI Dashboard) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace `psb_demo_tui`'s single global board (one `PsbModbusClient`, one `ScannedData`, one worker thread) with N boards across M buses — one worker thread per `PsbSerialBus` (Phase 1), a board switcher UI, and startup that actually connects every board in the resolved topology instead of erroring past one.

**Architecture:** `tab_monitor.h`/`tab_channel.h`/`widgets.h` already take all board state through `AppState&` with zero file-scope globals — confirmed by grep, zero hits for `g_client`/`g_connected` outside `main.cpp`. So they need **no changes**. Everything currently global in `main.cpp` (client, connected flag, `ScannedData`, work queue, worker thread, the whole dashboard UI) becomes one `BoardSession` per board; boards sharing a bus share one `BusWorker` (thread + work queue), naturally serializing their I/O exactly as `client-architecture-and-pitfalls.md` §2.1 requires. `AppState::client`'s type changes from `PsbModbusClient&` to `PsbBoardSession&` (Phase 1's per-board session) — a safe one-line change, since neither `tab_monitor.h` nor `tab_channel.h` ever calls the four facade-only methods (`connect`/`scanPorts`/`availableBaudRates`/`setFrameCallback`) `PsbBoardSession` lacks.

**Tech Stack:** C++17, FTXUI (vendored), CLI11 (vendored), the Phase 1 `PsbSerialBus`/`PsbBoardSession`/`TopologyConfig` (already on `main`).

## Global Constraints

- With exactly one board resolved (today's common case: `-p`, or a topology with one board, or the `/dev/ttyUSB0` first-run guess), the dashboard must be pixel-identical to today's — board switcher hidden, same layout, same behavior. This is the regression backstop: every existing single-board manual `tmux` check from Phase 1 must still pass unchanged.
- `PsbSerialBus`/`PsbBoardSession` are not internally thread-safe (Phase 1) — exactly one thread drives a given bus. Enforced here by construction: each `BusWorker` owns exactly one thread, and every board on that bus is only ever touched from that thread (or via its work queue, which that same thread drains).
- No live add/remove of boards mid-session in this phase — the board set is fixed at startup from the resolved topology. Interactive add/remove is Phase 3 (the setup wizard).
- Preserve every comment explaining a non-obvious historical bug fix (torn-table publishing, uptime decoupling, offline-threshold reasoning, etc.) when moving code — these encode hard-won lessons from `client-architecture-and-pitfalls.md`, not decoration.
- Build via `cd tools && cmake --build build --target psb_demo_tui`. Test via `./build/psb_modbus_core/tests/psb_tests` (must stay unaffected — this phase touches no `psb_modbus_core` files). Manual verification via `tmux` per `docs/guide/client-architecture-and-pitfalls.md` §3's methodology.

---

## Task 1: `BoardSession`/`BusWorker` data model + parameterized poll functions

**Files:**
- Create: `tools/psb_demo_app/tui/board_session.h`
- Modify: `tools/psb_demo_app/tui/widgets.h:23` (`AppState::client` type)

**Interfaces:**
- Consumes: `psb::PsbSerialBus`, `psb::PsbBoardSession` (Phase 1, `tools/psb_modbus_core/`).
- Produces: `psb::tui::BoardSession` (one board's full live state), `psb::tui::BusWorker` (one bus's thread + shared work queue), `psb::tui::doFullScan(PsbBoardSession&, std::atomic<bool>& connected, ScannedData&, ScreenInteractive&, std::atomic<bool>& running)`, `psb::tui::doPollScan(PsbBoardSession&, ScannedData&, ScreenInteractive&, std::atomic<bool>& running, const std::function<bool()>& hasPendingWork, std::string& statusMsg, std::mutex& statusMutex)` — both moved from `main.cpp` and parameterized away from the global `g_client`.

- [ ] **Step 1: Change `AppState::client`'s type**

Edit `tools/psb_demo_app/tui/widgets.h`:

Old:
```cpp
struct AppState {
    psb::PsbModbusClient&                    client;
```

New:
```cpp
struct AppState {
    psb::PsbBoardSession&                    client;
```

Add the include (Phase 1's per-board session type) alongside the existing `psb_modbus_client.h` include:

Old:
```cpp
#include "tui_format.h"
#include "psb_modbus_client.h"
```

New:
```cpp
#include "tui_format.h"
#include "psb_modbus_client.h"
#include "psb_board_session.h"
```

`psb_modbus_client.h` stays included — `tui_policy.h`/other headers may still reference `psb::` types it declares (`OpMode`, `ParamAction`, etc., transitively via `types.h`); only the `AppState::client` member's type changes.

- [ ] **Step 2: Write `board_session.h`**

```cpp
#pragma once

#include "tui_format.h"
#include "widgets.h"
#include "psb_board_session.h"
#include "psb_serial_bus.h"

#include <ftxui/component/screen_interactive.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace psb::tui {

// Everything that was a single file-scope global in the pre-multi-board
// main.cpp (g_client, g_connected, ScannedData data, statusMsg, the
// connection-modal's live input strings, tab titles/active tab) — now one
// instance per board. AppState's reference members (see widgets.h) bind
// straight into this struct's fields, so tab_monitor.h/tab_channel.h need
// no changes at all: they only ever see "an AppState", never this struct.
//
// `appState` is a pointer, not a value, because AppState holds references
// into this struct's own other fields (client/data/statusMsg/...) — it must
// be constructed after them, in a fixed memory location. BoardSession
// itself is therefore only ever held via std::unique_ptr<BoardSession> in a
// vector (see main.cpp) — never by value in a container that could
// reallocate and move it.
struct BoardSession {
    std::string nickname;
    std::shared_ptr<PsbSerialBus> bus;   // shared with sibling boards on the same bus
    std::unique_ptr<PsbBoardSession> client;

    std::atomic<bool> connected{false};
    std::atomic<bool> connecting{false};
    std::atomic<bool> abortConnect{false};
    std::chrono::steady_clock::time_point connectStart;

    ScannedData data;
    ConfigInputs inputs;
    std::string statusMsg;
    std::mutex statusMutex;

    std::unique_ptr<AppState> appState;

    // Connection-modal live fields (pre-filled from topology/CLI at
    // startup; editable if the user reopens the modal to reconnect).
    std::string portVal, baudVal, slaveVal;

    // Per-board dashboard UI state.
    std::vector<std::string> tabTitles{"Monitor"};
    int activeTab = 0;
    bool showSysCfg = false;
    bool showConnModal = false;
    std::atomic<int> pendingChannelCount{-1};
    std::atomic<bool> pendingSync{false};

    ftxui::Component dashboard;  // built once by makeBoardDashboard() (Task 2)
};

// One physical bus — owns exactly one worker thread, shared by every board
// attached to it. This is what makes multi-drop safe: PsbSerialBus is not
// internally thread-safe (Phase 1), so exactly one thread must ever drive a
// given bus, and every board sharing it is naturally serialized by sharing
// that one thread — the same single-writer rule
// client-architecture-and-pitfalls.md §2.1 already established for the
// single-board case, generalized from one port total to one port per bus.
struct BusWorker {
    std::shared_ptr<PsbSerialBus> bus;
    std::vector<BoardSession*> boards;   // non-owning — BoardSessions live in main.cpp's board list
    std::queue<std::function<void()>> workQueue;
    std::mutex workMutex;
    std::condition_variable workCv;
    std::thread thread;
};

// Reads never surface failure to the caller (readChannelInfo/Config/CalConfig
// return a plain struct, defaulted/partial on a transient read error, with
// isConnected() unaffected). doFullScan runs exactly once at connect time,
// right where this codebase has repeatedly observed USB-serial (e.g. CH340)
// reopen flakiness bite hardest — so retry each read once, using lastError()
// changing as the per-call failure signal (it's sticky/never cleared on
// success, so comparing before/after isolates whether *this* call set a new
// one). Without this, a single glitched read permanently corrupts that one
// channel's displayed data until the user manually interacts with it
// (triggering refreshCh) or reconnects.
template <typename Fn>
inline auto readWithRetry(PsbBoardSession& client, Fn&& fn) -> decltype(fn()) {
    auto before = client.lastError();
    auto result = fn();
    if (client.lastError() != before) {
        result = fn();
    }
    return result;
}

// Scans only what the Monitor table actually displays: ChannelInfo (status/
// V/I/Vop/faults) plus the output, protection, and output-enabled
// ChannelConfig blocks (Vset, ramp, I-limit, iProtMode for the Fault
// column). Recovery-policy and derate-step — shown only on the Channel tab —
// are deliberately left out here and fetched lazily the first time that
// channel's tab is opened (see tab_channel.h), since scanning them for every
// channel up front was pure overhead for what Monitor needs to render.
//
// Stages all channel results locally and publishes (chInfo/chCfg/chLoaded)
// in one shot at the very end, so Monitor shows a single "Scanning
// channels... X/N" message throughout (via scanProgress) and then reveals
// the whole table at once — not a row-by-row trickle, which read as a
// torn/inconsistent table rather than an obviously-still-loading one.
//
// Interleaves a system-status read after every channel so the menu bar's
// uptime/temp/humidity keep ticking with real (not extrapolated) data
// throughout the scan instead of freezing for its whole duration — the
// serial bus is still shared with the channel reads (can't run truly
// concurrently), but this keeps the readout at most one channel's-worth
// stale rather than stuck for the whole scan.
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
    if (running) screen.PostEvent(ftxui::Event::Custom);

    psb::ChannelInfo   chInfoStaging[MAX_CHANNELS];
    psb::ChannelConfig chCfgStaging[MAX_CHANNELS];

    for (int ch = 0; ch < n; ++ch) {
        chInfoStaging[ch] = readWithRetry(client, [&] { return client.readChannelInfo(ch); });
        // Zero capability flags means the connect-time probe itself failed
        // both of readWithRetry's attempts (readChannelInfo's capability
        // fetch is one all-or-nothing transaction — no real channel
        // legitimately reports zero capability bits). Worth fighting harder
        // for here: getting this wrong silently renders the whole row "n/a"
        // for the rest of the session (doPollScan's self-heal below covers
        // the case this still misses).
        for (int attempt = 0; attempt < 2 && chInfoStaging[ch].chCapFlags == 0; ++attempt)
            chInfoStaging[ch] = client.readChannelInfo(ch);
        uint16_t caps = chInfoStaging[ch].chCapFlags;
        client.readChannelOutputBlock(ch, caps, chCfgStaging[ch]);
        client.readChannelProtectionBlock(ch, caps, chCfgStaging[ch]);
        client.readChannelOutputEnabledBlock(ch, caps, chCfgStaging[ch]);

        data.scanProgress = ch + 1;
        client.readSystemStatus(data.sysInfo);
        if (running) screen.PostEvent(ftxui::Event::Custom);
    }

    for (int ch = 0; ch < n; ++ch) {
        data.chInfo[ch]   = chInfoStaging[ch];
        data.chCfg[ch]    = chCfgStaging[ch];
        data.chLoaded[ch] = true;
    }
    if (running) screen.PostEvent(ftxui::Event::Custom);
}

// A board that fails this many consecutive status polls in a row is
// flagged offline (see doPollScan below) — a real, user-visible fault
// (unresponsive channel, not just one glitched transaction), not a
// transient blip; readWithRetry-style single-retry is deliberately not
// enough tolerance for this class of decision.
inline constexpr int kChannelOfflineThreshold = 5;

// Response timeout used for routine polling reads only (system/channel
// status, capability self-heal) — deliberately much shorter than the port's
// normal connect-time timeout (default 3000ms). Confirmed live against real
// hardware that this board/cable link has genuine, occasional transient
// Modbus failures (independently reproduced with mbpoll alone, no TUI
// involved) — with the default timeout, one such failure mid-sweep froze
// the whole poll cycle (and the uptime counter with it) for up to 3 full
// seconds. A routine poll that fails is expected to just succeed next
// cycle, so it should fail fast here rather than block the UI.
inline constexpr int kPollTimeoutMs = 300;

// Publishes system status and channel status on two independent cadences,
// each a single PostEvent:
//  - System status (uptime/temp/humidity) publishes the instant it's read,
//    so the menu bar ticks every poll cycle regardless of how long the
//    channel sweep below takes — coupling it to the sweep's completion is
//    what previously made uptime visibly update only once every ~3-5s on a
//    10-channel board instead of every cycle.
//  - Channel status is swept into a local staging copy and published to
//    `data` in one shot once the sweep is done (or interrupted — see
//    below), rather than mutating `data` in place channel-by-channel. The
//    latter let the ~12 Hz breathing-LED animation thread's continuous
//    redraws catch the table mid-sweep, showing a torn mix of channels
//    already refreshed this cycle and channels still showing the previous
//    cycle's values; staging then publishing atomically means every repaint
//    sees one consistent snapshot of the whole table.
//
// Tracks consecutive read failures per channel in `data` directly (rare,
// edge-triggered changes — no torn-read concern like the per-cycle
// measurement data above) and flags a channel offline, with a one-time
// status message, after kChannelOfflineThreshold consecutive failures; any
// single success clears it.
//
// `hasPendingWork` lets the sweep bail out early when a write is queued —
// for a multi-board bus, this is now a bus-wide check (shared with sibling
// boards' writes), so a write targeting *any* board on this bus can still
// interrupt whichever board's sweep is currently running, exactly as it
// interrupted the single global sweep before. The worker loop drains that
// write right away instead of making it wait; any channels not yet reached
// this tick simply keep their staged (pre-sweep) values and get re-polled
// next tick — no data loss, since this is continuous live polling, not a
// one-shot scan.
inline void doPollScan(PsbBoardSession& client, ScannedData& data, ftxui::ScreenInteractive& screen,
                       std::atomic<bool>& running,
                       const std::function<bool()>& hasPendingWork,
                       std::string& statusMsg, std::mutex& statusMutex) {
    int n = data.numChannels();

    if (client.readSystemStatus(data.sysInfo, kPollTimeoutMs))
        data.lastSysUpdate = std::chrono::steady_clock::now();
    if (running) screen.PostEvent(ftxui::Event::Custom);

    psb::ChannelInfo chStaging[MAX_CHANNELS];
    for (int ch = 0; ch < n; ++ch) chStaging[ch] = data.chInfo[ch];

    std::vector<int> newlyOffline;
    for (int ch = 0; ch < n; ++ch) {
        if (hasPendingWork()) break;
        if (!shouldPollChannel(ch, n)) continue;

        // Self-heal a channel whose capability flags never got captured
        // correctly (0 is not a real hardware configuration — every
        // channel reports at least one bit). doFullScan's connect-time
        // retries can still miss a persistent glitch; without this, such a
        // channel is otherwise stuck showing "n/a" everywhere for the rest
        // of the session, since readChannelStatus only ever reuses the caps
        // it's handed, never re-derives them. One cheap extra transaction
        // per affected channel per poll cycle, only while still unknown.
        if (chStaging[ch].chCapFlags == 0) {
            uint16_t caps = 0;
            if (client.readChannelCapabilities(ch, caps, kPollTimeoutMs) && caps != 0)
                chStaging[ch].chCapFlags = caps;
        }

        bool ok = client.readChannelStatus(ch, chStaging[ch].chCapFlags, chStaging[ch], kPollTimeoutMs);
        if (ok) {
            data.chPollFailCount[ch] = 0;
            data.chOffline[ch] = false;
        } else if (++data.chPollFailCount[ch] > kChannelOfflineThreshold && !data.chOffline[ch]) {
            data.chOffline[ch] = true;
            newlyOffline.push_back(ch);
        }
    }

    // Publish the channel sweep atomically — a render can never observe a
    // partially-refreshed set of channels.
    for (int ch = 0; ch < n; ++ch) data.chInfo[ch] = chStaging[ch];

    if (!newlyOffline.empty()) {
        std::lock_guard<std::mutex> lk(statusMutex);
        statusMsg = "Error: CH" + std::to_string(newlyOffline.front()) + " not responding — marked offline";
    }
    if (running) screen.PostEvent(ftxui::Event::Custom);
}

inline void rebuildChannelTitles(std::vector<std::string>& titles, int numChannels) {
    titles.resize(1);
    for (int ch = 0; ch < numChannels; ++ch)
        titles.push_back("CH" + std::to_string(ch));
}

} // namespace psb::tui
```

- [ ] **Step 2: Build (compile-only check — nothing references these yet)**

Run: `cd tools && cmake --build build --target psb_demo_tui 2>&1 | tail -60`
Expected: FAIL — `main.cpp` still defines its own `doFullScan`/`doPollScan`/`readWithRetry`/`rebuildChannelTitles` and still uses `psb::PsbModbusClient g_client` bound to `AppState`, which no longer type-matches `AppState::client`'s new `PsbBoardSession&` type. This is expected; Task 3 replaces `main.cpp`'s startup section, which resolves it. Confirm the failure is specifically a type mismatch on `AppState{g_client, ...}` and not something unrelated (e.g. a typo in `board_session.h` — check the error points at `main.cpp`'s `AppState appState{g_client, ...}` line, not inside `board_session.h` itself).

- [ ] **Step 3: Commit**

```bash
git add tools/psb_demo_app/tui/board_session.h tools/psb_demo_app/tui/widgets.h
git commit -m "feat(psb_demo_tui): add BoardSession/BusWorker data model (main.cpp not yet updated — expected build break until Task 3)"
```

---

## Task 2: `PsbBoardSession::rebind()` + per-board dashboard UI extraction

**Why `rebind()` first:** `AppState::client` is a reference, bound once (at `BoardSession` construction) to `*board.client`. The connection modal's Connect action needs to be able to change *which slave ID* a board addresses (matching today's behavior — editing the Slave field and clicking Connect takes effect) without reassigning `board.client` to a new object, which would leave `AppState::client` dangling. A tiny mutator on `PsbBoardSession` that changes its slave ID in place (same object, same address) solves this cleanly.

**Files:**
- Modify: `tools/psb_modbus_core/psb_board_session.h` (add `rebind()`)
- Modify: `tools/psb_modbus_core/psb_board_session.cpp` (implement it)
- Modify: `tools/psb_modbus_core/tests/test_board_session.cpp` (new test case)
- Create: `tools/psb_demo_app/tui/board_dashboard.h`
- Modify: `tools/psb_demo_app/tui/main.cpp` (delete the extracted block — full replacement lands in Task 3)

**Interfaces:**
- Produces: `psb::PsbBoardSession::rebind(int slaveId)`; `psb::tui::makeBoardDashboard(BoardSession& board, BusWorker& busWorker, ftxui::ScreenInteractive& screen, std::atomic<bool>& running, int timeoutMs) -> ftxui::Component`.

- [ ] **Step 1: Write the failing test for `rebind()`**

Add to `tools/psb_modbus_core/tests/test_board_session.cpp`:

```cpp
TEST_CASE("PsbBoardSession — rebind() changes slave ID without changing object identity", "[board_session]") {
    auto bus = std::make_shared<psb::PsbSerialBus>();
    uint16_t inputA[8] = {}, holdingA[8] = {};
    uint16_t inputB[8] = {}, holdingB[8] = {};
    inputA[0] = VC_PROTOCOL_MAJOR; inputA[1] = VC_PROTOCOL_MINOR;
    inputB[0] = VC_PROTOCOL_MAJOR; inputB[1] = VC_PROTOCOL_MINOR;
    bus->attachTestArrays(1, inputA, holdingA, 8);
    bus->attachTestArrays(2, inputB, holdingB, 8);

    psb::PsbBoardSession session(bus, 1);
    REQUIRE(session.verifyProtocol());
    CHECK(session.slaveId() == 1);

    session.rebind(2);
    CHECK(session.slaveId() == 2);
    // rebind() resets verified state — the previous verifyProtocol() result
    // was for slave 1, not slave 2, even though this happens to also be a
    // valid slave here.
    CHECK_FALSE(session.isConnected());
    REQUIRE(session.verifyProtocol());
    CHECK(session.isConnected());
}
```

- [ ] **Step 2: Confirm it fails to build**

Run: `cd tools && cmake --build build --target psb_tests 2>&1 | tail -20`
Expected: FAIL — `no member named 'rebind' in 'psb::PsbBoardSession'`.

- [ ] **Step 3: Add `rebind()` to `psb_board_session.h`**

Edit `tools/psb_modbus_core/psb_board_session.h`:

Old:
```cpp
    bool verifyProtocol();
    void disconnect();
    bool isConnected() const;
    std::string lastError() const;
    int slaveId() const;
    int16_t currentUnitExp() const;
```

New:
```cpp
    bool verifyProtocol();
    void disconnect();
    bool isConnected() const;
    std::string lastError() const;
    int slaveId() const;
    int16_t currentUnitExp() const;

    // Changes which slave ID this session addresses on its bus, in place —
    // same object, same address, so existing references to it (e.g.
    // AppState::client in psb_demo_tui) stay valid. Resets verified state:
    // the previous verifyProtocol() result was for the old slave ID and
    // doesn't apply to the new one. Used by the TUI's connection modal to
    // let the user correct a wrong slave ID and reconnect without
    // restarting the session that references this object.
    void rebind(int slaveId);
```

- [ ] **Step 4: Implement `rebind()`**

Edit `tools/psb_modbus_core/psb_board_session.cpp`:

Old:
```cpp
void PsbBoardSession::disconnect() { m_impl->verified = false; }
```

New:
```cpp
void PsbBoardSession::disconnect() { m_impl->verified = false; }
void PsbBoardSession::rebind(int slaveId) { m_impl->slaveId = slaveId; m_impl->verified = false; }
```

- [ ] **Step 5: Build and run**

Run: `cd tools && cmake --build build --target psb_tests 2>&1 | tail -20 && ./build/psb_modbus_core/tests/psb_tests "[board_session]"`
Expected: PASS — 6 test cases now (was 5).

- [ ] **Step 6: Commit the `rebind()` addition on its own**

```bash
git add tools/psb_modbus_core/psb_board_session.h tools/psb_modbus_core/psb_board_session.cpp \
        tools/psb_modbus_core/tests/test_board_session.cpp
git commit -m "feat(psb_modbus_core): add PsbBoardSession::rebind() — change slave ID in place"
```

- [ ] **Step 7: Write `board_dashboard.h`**

This is the extraction of everything in today's `main()` from `portVal`/`baudVal`/`slaveVal` construction through the `root` component (the connection modal, SysConfig popup, menu bar, tab bar/content, status bar, and the `Modal`/`CatchEvent` wrapping) — parameterized per-board. `tab_monitor.h`/`tab_channel.h` are called exactly as before (`makeMonitorTab`/`makeChannelTab` already take `AppState&`, unchanged). Every comment explaining a non-obvious historical fix is preserved verbatim.

```cpp
#pragma once

#include "board_session.h"
#include "tab_monitor.h"
#include "tab_channel.h"
#include "tui_policy.h"
#include "modbus_settings.h"
#include "register_map.h"
#include "board_catalog.h"
#include "tool_version.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include <cmath>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace psb::tui {

using namespace ftxui;

inline const std::vector<std::string> kOpModes  = {"Normal", "Automatic"};
inline const std::vector<std::string> kStartPol = {"Load NVS Config", "Factory Default"};
inline const std::vector<std::string> kBaudNames = {"115200", "9600", "19200", "38400"};

// Builds one board's full dashboard: connection modal, SysConfig popup,
// menu bar, tab bar/content (Monitor + CH0..CHn), status bar. Call once per
// board at startup (Task 3); the board switcher (Task 4) picks which
// already-built dashboard is visible. Everything here reads/writes only
// `board`'s own fields (or `busWorker`'s shared queue for this board's
// bus) — never another board's state, so N of these coexisting is safe.
inline Component makeBoardDashboard(BoardSession& board, BusWorker& busWorker,
                                    ScreenInteractive& screen, std::atomic<bool>& running,
                                    int timeoutMs) {
    // ---- Connection inputs (live in the connection modal) ----
    auto baudInp  = Input(&board.baudVal,  "baud");
    auto slaveInp = Input(&board.slaveVal, "id");

    // Port list & selection
    auto portList = std::make_shared<std::vector<std::string>>();
    auto portIdx = std::make_shared<int>(-1);

    auto doScanPorts = [&board, portList, portIdx, &screen] {
        *portList = PsbSerialBus::scanPorts();
        *portIdx = selectedPortIndex(*portList, board.portVal);
        board.portVal = *portIdx >= 0 ? (*portList)[*portIdx] : std::string{};
        screen.PostEvent(Event::Custom);
    };

    auto portDropdown = Dropdown(portList.get(), portIdx.get());
    auto visiblePortDropdown = Maybe(portDropdown, [portList] { return !portList->empty(); });
    auto bScan = Button("Rescan", [doScanPorts] { doScanPorts(); });

    auto doConnect = [&board, &busWorker, &screen, &running, timeoutMs] {
        if (board.portVal.empty() || board.connecting) return;
        board.abortConnect = false;
        board.connecting = true;
        board.connectStart = std::chrono::steady_clock::now();
        { std::lock_guard<std::mutex> lk(board.statusMutex); board.statusMsg = "Connecting to " + board.portVal + "..."; }
        screen.PostEvent(Event::Custom);
        std::thread([&board, &busWorker, &screen, &running, timeoutMs] {
            int baud = 115200, slave = 1;
            try { baud  = std::stoi(board.baudVal);  } catch (...) {}
            try { slave = std::stoi(board.slaveVal); } catch (...) {}
            // rebind() first (see Steps 1-6 above) so a corrected slave ID
            // takes effect even if the bus itself is already open (a
            // sibling board on this bus connected first).
            board.client->rebind(slave);
            bool ok = board.bus->isConnected() || board.bus->connect(board.portVal, baud, timeoutMs);
            if (ok) ok = board.client->verifyProtocol();
            board.connected = ok && !board.abortConnect;
            if (board.abortConnect) { ok = false; }
            if (!running) { board.connecting = false; return; }
            if (ok) {
                { std::lock_guard<std::mutex> lk(busWorker.workMutex);
                  busWorker.workQueue.push([&board, &screen, &running] {
                      doFullScan(*board.client, board.connected, board.data, screen, running);
                      board.data.valid = board.connected.load();
                      board.pendingChannelCount.store(board.data.numChannels(), std::memory_order_release);
                      board.pendingSync.store(true, std::memory_order_release);
                      screen.PostEvent(Event::Custom);
                  }); }
                busWorker.workCv.notify_one();
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
        }).detach();
    };

    auto doDisconnect = [&board, &busWorker, &screen] {
        board.abortConnect = true;
        board.connected = false; board.data.valid = false;
        // Enqueue disconnect on this board's bus worker to serialise with
        // in-flight Modbus I/O — avoids use-after-free on the bus's port.
        // Only disconnects the bus if no *other* board on it is still
        // connected — a shared bus stays open for sibling boards.
        { std::lock_guard<std::mutex> lk(busWorker.workMutex);
          busWorker.workQueue.push([&board, &busWorker] {
              board.client->disconnect();
              bool anyOtherConnected = false;
              for (BoardSession* b : busWorker.boards)
                  if (b != &board && b->connected.load()) { anyOtherConnected = true; break; }
              if (!anyOtherConnected) busWorker.bus->disconnect();
          }); }
        busWorker.workCv.notify_one();
        board.tabTitles = {"Monitor"}; board.activeTab = std::min(board.activeTab, 0);
        { std::lock_guard<std::mutex> lk(board.statusMutex); board.statusMsg = "Disconnected"; }
        screen.PostEvent(Event::Custom);
    };

    // ---- Connect/Disconnect/Abort toggle button ----
    ButtonOption connBtnOpt{};
    connBtnOpt.transform = [&board](const EntryState& es) -> Element {
        std::string lbl = board.connected.load() ? "Disconnect"
                        : board.connecting.load()  ? "Abort"
                        : "Connect";
        auto e = text("[ " + lbl + " ]");
        if (es.focused) e = e | inverted;
        return e;
    };
    auto bConnToggle = Button("", [&board, &screen, doConnect, doDisconnect, doScanPorts] {
        if (board.connected.load())     doDisconnect();
        else if (board.connecting.load()) board.abortConnect = true;
        else {
            doScanPorts();
            board.showConnModal = true;
            screen.PostEvent(Event::Custom);
        }
    }, connBtnOpt);

    // ---- Connection modal ----
    auto bConnInModal = ActionButton("Connect", [&board, doConnect] {
        if (!board.portVal.empty() && !board.connecting) { doConnect(); board.showConnModal = false; }
    });
    auto bCancelConn = ActionButton("Cancel", [&board, &screen] {
        board.showConnModal = false; screen.PostEvent(Event::Custom);
    });
    auto connModalForm   = Container::Vertical({visiblePortDropdown, bScan, baudInp, slaveInp, bConnInModal, bCancelConn});
    auto connModalPopup  = Renderer(connModalForm, [&board, portList, portIdx, visiblePortDropdown, bScan, bConnInModal, bCancelConn] {
        if (*portIdx >= 0 && *portIdx < static_cast<int>(portList->size()))
            board.portVal = (*portList)[*portIdx];
        Element portChoice = portList->empty()
            ? text("(no ports found)") | dim | flex
            : visiblePortDropdown->Render() | flex;
        return vbox({
            text(" Connection Settings — " + board.nickname + " ") | bold | center,
            separator(),
            hbox({ text("Port  : "), portChoice, text(" "), bScan->Render() }),
            hbox({ text("Baud  : "), baudInp->Render()  | size(WIDTH, EQUAL, 8)  }),
            hbox({ text("Slave : "), slaveInp->Render() | size(WIDTH, EQUAL, 5)  }),
            separator(),
            hbox({ bConnInModal->Render(), text("  "), bCancelConn->Render() }) | center,
        }) | border | size(WIDTH, EQUAL, 42);
    });

    auto bQuit = ActionButton("Quit", [&running, &busWorker, &screen] {
        running = false; busWorker.workCv.notify_all(); screen.ExitLoopClosure()();
    });

    // ---- SysConfig popup ----
    auto bSysCfg = ActionButton("Setting", [&board, &screen] {
        if (!board.showSysCfg && board.data.valid) syncDataToInputs(board.data, board.inputs);
        board.showSysCfg = !board.showSysCfg; screen.PostEvent(Event::Custom);
    });

    // scOpMode shares inputs.opModeIdx with menuModeC; autoCommit=true writes on every click.
    auto scOpMode  = InlineCycler(kOpModes, &board.inputs.opModeIdx, [&board] {
        postWrite(*board.appState, board.inputs, "OpMode",
            [&board] { return board.client->writeOperatingMode(static_cast<OpMode>(board.inputs.opModeIdx)); },
            [&board] { board.data.sysCfg = board.client->readSystemConfig(); });
    }, /*autoCommit=*/true);
    auto scStartup = InlineCycler(kStartPol, &board.inputs.startupIdx, [&board] {
        postWrite(*board.appState, board.inputs, "StartupPol",
            [&board] { return board.client->writeStartupChannelPolicy((uint16_t)board.inputs.startupIdx); },
            [&board] { board.data.sysCfg = board.client->readSystemConfig(); });
    });
    auto scSlave = Input(&board.inputs.slaveAddr, "1-247");
    auto scBaud = InlineCycler(kBaudNames, &board.inputs.baudIdx, [] {});

    auto scSaveModbus = ActionButton("Save Modbus", [&board, &busWorker, &screen] {
        uint16_t slaveAddress = 0;
        if (!parseModbusSlaveAddress(board.inputs.slaveAddr, slaveAddress)) {
            { std::lock_guard<std::mutex> lk(board.statusMutex); board.statusMsg = "Error: slave address must be 1-247"; }
            screen.PostEvent(Event::Custom);
            return;
        }
        if (board.inputs.baudIdx < 0 || board.inputs.baudIdx >= static_cast<int>(kBaudNames.size())) {
            { std::lock_guard<std::mutex> lk(board.statusMutex); board.statusMsg = "Error: invalid baud rate"; }
            screen.PostEvent(Event::Custom);
            return;
        }

        const std::string stagedSlave = board.inputs.slaveAddr;
        const int stagedBaud = board.inputs.baudIdx;
        const SystemConfig current = board.data.sysCfg;
        { std::lock_guard<std::mutex> lk(board.statusMutex); board.statusMsg = "Writing Modbus config..."; }
        screen.PostEvent(Event::Custom);

        std::function<void()> item = [&board, &screen, stagedSlave, stagedBaud, current] {
            auto result = saveModbusSettings(
                stagedSlave, stagedBaud, current,
                [&board](uint16_t value) { return board.client->writeSlaveAddress(value); },
                [&board](uint16_t value) { return board.client->writeBaudRateCode(value); });

            if (result == ModbusSettingsSaveResult::Success) {
                board.data.sysCfg = board.client->readSystemConfig();
                syncDataToInputs(board.data, board.inputs);
            }
            std::string resultMessage = modbusSettingsStatusMessage(result, board.client->lastError());
            { std::lock_guard<std::mutex> lk(board.statusMutex); board.statusMsg = std::move(resultMessage); }
            screen.PostEvent(Event::Custom);
        };

        { std::lock_guard<std::mutex> lk(busWorker.workMutex); busWorker.workQueue.push(std::move(item)); }
        busWorker.workCv.notify_one();
    });

    auto saveSystemConfig = [&board] {
        postWrite(*board.appState, board.inputs, "Save",
            [&board] { return board.client->sendParamAction(-1, ParamAction::Save); },
            [&board] { board.data.sysCfg = board.client->readSystemConfig(); });
    };
    auto scSave    = Button("Save", saveSystemConfig);
    auto scLoad    = Button("Load",    [&board] { postWrite(*board.appState, board.inputs, "Load",
        [&board] { return board.client->sendParamAction(-1, ParamAction::Load);         }, [&board] { board.data.sysCfg = board.client->readSystemConfig(); }); });
    auto scFactory = Button("Factory", [&board] { postWrite(*board.appState, board.inputs, "Factory",
        [&board] { return board.client->sendParamAction(-1, ParamAction::FactoryReset); }, [&board] { board.data.sysCfg = board.client->readSystemConfig(); }); });
    // SoftwareReset: send reset and mark disconnected — device will reboot.
    auto scReset   = Button("Reset",   [&board] {
        postWrite(*board.appState, board.inputs, "SysReset",
            [&board] { return board.client->sendParamAction(-1, ParamAction::SoftwareReset); },
            [&board] { board.connected = false; board.data.valid = false; board.client->disconnect(); });
        board.showSysCfg = false;
    });
    auto scClose   = Button("Close",   [&board, &screen] { board.showSysCfg = false; screen.PostEvent(Event::Custom); });

    auto sysCfgForm = Container::Vertical({
        scOpMode, scStartup, scSave, scLoad, scFactory,
        scSlave, scBaud, scSaveModbus,
        scReset, scClose,
    });
    auto sysCfgPopup = Renderer(sysCfgForm, [&board, scOpMode, scStartup, scSave, scLoad, scFactory, scSlave, scBaud, scSaveModbus, scReset, scClose] {
        return vbox({
            text(" System Config — " + board.nickname + " ") | bold | center, separator(),
            hbox({ text("Working Mode  : "), scOpMode->Render()  }),
            hbox({ text("Startup Policy: "), scStartup->Render() }),
            separator(),
            hbox({ scSave->Render(), text("  "), scLoad->Render(), text("  "), scFactory->Render() }) | center,
            separator(),
            text(" Modbus (next boot) ") | bold | center,
            hbox({ text("Slave Address : "), scSlave->Render() | size(WIDTH, EQUAL, 8) }),
            hbox({ text("Baud Rate     : "), scBaud->Render() }),
            scSaveModbus->Render() | center,
            separator(),
            scReset->Render() | center,
            separator(),
            scClose->Render() | center,
        }) | border | size(WIDTH, EQUAL, 48);
    });

    // ---- Menu bar mode cycler (shares opModeIdx with scOpMode) ----
    auto menuModeC = InlineCycler(kOpModes, &board.inputs.opModeIdx, [&board] {
        postWrite(*board.appState, board.inputs, "OpMode",
            [&board] { return board.client->writeOperatingMode(static_cast<OpMode>(board.inputs.opModeIdx)); },
            [&board] { board.data.sysCfg = board.client->readSystemConfig(); });
    }, /*autoCommit=*/true);

    auto menuSave = ActionButton("Save", saveSystemConfig);
    auto connectedMenuSave = Maybe(menuSave, [&board] { return board.connected.load(); });
    auto menuBar = Container::Horizontal({menuModeC, connectedMenuSave, bConnToggle, bQuit});

    // ---- Tab bar ----
    MenuOption tabOpt = MenuOption::Horizontal();
    tabOpt.entries_option.transform = [](const EntryState& e) -> Element {
        auto t = text("  " + e.label + "  ");
        if (e.active)                t = t | bold | color(Color::Cyan);
        if (e.focused && !e.active)  t = t | inverted;
        if (!e.active && !e.focused) t = t | dim;
        return t;
    };
    auto tabBar = Menu(&board.tabTitles, &board.activeTab, tabOpt);

    // ---- Tab content: Monitor + CH0..CH15 ----
    Components tabComponents = { makeMonitorTab(*board.appState, board.inputs) };
    for (int ch = 0; ch < MAX_CHANNELS; ++ch)
        tabComponents.push_back(makeChannelTab(*board.appState, board.inputs, ch));
    auto tabContent = Container::Tab(tabComponents, &board.activeTab);

    // ---- Status bar (connection details + SysConfig; Connect lives in the menu) ----
    auto statusBar    = Container::Horizontal({bSysCfg});
    auto mainContainer = Container::Vertical({menuBar, tabBar, tabContent, statusBar});

    auto root = Renderer(mainContainer, [&board, &screen, menuModeC, connectedMenuSave, bConnToggle, bQuit, tabBar, tabContent, bSysCfg] {
        if (board.pendingSync.exchange(false, std::memory_order_acq_rel)) {
            if (board.connected.load() && board.data.valid) {
                int nc = board.pendingChannelCount.load(std::memory_order_acquire);
                rebuildChannelTitles(board.tabTitles, nc);
                int maxTab = static_cast<int>(board.tabTitles.size()) - 1;
                if (board.activeTab > maxTab) board.activeTab = maxTab;
                syncDataToInputs(board.data, board.inputs);
            }
        }
        reconcileDisconnectedTabs(
            board.connected.load() && board.data.valid, board.tabTitles, board.activeTab);

        std::string msg;
        { std::lock_guard<std::mutex> lk(board.statusMutex); msg = board.statusMsg; }

        // --- Channel count + system telemetry ---
        std::string chTxt = "--";
        if (board.data.valid) chTxt = std::to_string(board.data.numChannels());

        std::string fwTxt = "--", protoTxt = "--";
        std::string variantTxt = "PSB";
        std::string uptimeTxt = "--s";
        bool hasEnvSensor = false;
        char tmpS[16], humS[16];
        tmpS[0] = humS[0] = 0;
        if (board.data.valid) {
            const auto& si = board.data.sysInfo;
            fwTxt    = reg::formatFwVersion(si.fwVersion);
            protoTxt = std::to_string(si.protoMajor) + "." + std::to_string(si.protoMinor);
            variantTxt = catalog::variantName(si.variantId);
            uptimeTxt = std::to_string(si.uptimeSec) + "s";
            hasEnvSensor = (si.sysCapFlags & SysCap::ENV_SENSOR) != 0;
            if (hasEnvSensor) {
                snprintf(tmpS, sizeof(tmpS), "%.1fC",  si.boardTempRaw    * 0.1);
                snprintf(humS, sizeof(humS), "%.1f%%", si.boardHumidityRaw * 0.1);
            }
        }

        // --- Breathing green: cosine wave 0→1→0 over 2 s ----
        auto breathColor = []() -> Color {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch()).count() % 2000;
            double b = 0.5 - 0.5 * std::cos(2.0 * M_PI * static_cast<double>(ms) / 2000.0);
            return Color::RGB(0, 50 + static_cast<int>(b * 200), 0);
        };

        static constexpr auto kSysStaleThreshold = std::chrono::seconds(10);
        bool sysStale = board.connected.load() && board.data.sysStale(kSysStaleThreshold);
        Element connDotEl;
        if (sysStale) {
            connDotEl = text(" \xe2\x97\x8f ") | color(Color::Red) | bold;
        } else if (board.connected.load()) {
            connDotEl = text(" \xe2\x97\x8f ") | color(breathColor()) | bold;
        } else if (board.connecting.load()) {
            connDotEl = text(" \xe2\x8f\xb3 ") | color(Color::Yellow) | bold;
        } else {
            connDotEl = text(" \xe2\x97\x8b ") | color(Color::GrayDark);
        }

        // --- Menu bar ---
        bool isOnline = board.connected.load();
        Element centerGroup;
        if (isOnline) {
            std::string telemetry = " " + uptimeTxt;
            if (hasEnvSensor)
                telemetry += "  |  T: " + std::string(tmpS) + "  H: " + std::string(humS);
            telemetry += " ";
            centerGroup = hbox({ connDotEl, text(telemetry) });
        } else {
            centerGroup = text("");
        }
        Element modeElement = isOnline || board.connecting.load()
            ? menuModeC->Render()
            : text("[ " + kOpModes[board.inputs.opModeIdx] + " ]") | dim;
        Element saveElement = isOnline
            ? connectedMenuSave->Render()
            : text("[ Save ]") | dim;
        auto menuBarEl = hbox({
            text(" " + board.nickname + " (" + variantTxt + ") ") | bold,
            separator(),
            text(" " + chTxt + " Channels "),
            separator(),
            modeElement,
            text(" "),
            saveElement,
            filler(),
            centerGroup,
            filler(),
            bConnToggle->Render(),
            text(" "),
            bQuit->Render(),
        });

        // --- Status bar (static colour — no breathing) ---
        auto connColor = board.connected.load() ? Color::Green
                       : board.connecting.load()  ? Color::Yellow
                       : Color::GrayDark;
        Element connTextEl;
        if (board.connected.load()) {
            connTextEl = text(" " + board.portVal + " @" + board.baudVal + " #" + board.slaveVal + " ") | color(connColor);
        } else if (board.connecting.load()) {
            connTextEl = text(" Connecting... ") | color(Color::Yellow);
        } else {
            connTextEl = text(" offline ") | color(Color::GrayDark);
        }

        bool isErr = msg.find("Error") != std::string::npos;
        auto statusBarEl = hbox({
            text(" " + msg + " ") | (isErr ? color(Color::Red) : color(Color::Green))
                                  | size(WIDTH, GREATER_THAN, 30),
            filler(),
            text((isOnline ? " FW:" + fwTxt + "  Proto:" + protoTxt + "  " : " ")
                 + "TUI:" TOOL_VERSION_STRING " "),
            filler(),
            connTextEl,
            text(" "),
            bSysCfg->Render(),
        });

        return vbox({
            menuBarEl,
            separator(),
            hbox({ tabBar->Render() | flex }),
            separator(),
            tabContent->Render() | flex,
            separator(),
            statusBarEl,
        });
    }) | Modal(sysCfgPopup,  &board.showSysCfg)
       | Modal(connModalPopup, &board.showConnModal)
       | CatchEvent([&board, &screen](Event e) {
           if (board.showSysCfg    && e == Event::Escape) { board.showSysCfg    = false; screen.PostEvent(Event::Custom); return true; }
           if (board.showConnModal && e == Event::Escape) { board.showConnModal  = false; screen.PostEvent(Event::Custom); return true; }
           return false;
       });

    return root;
}

} // namespace psb::tui
```

`screen.ExitLoopClosure()` is only obtainable after `ScreenInteractive::Loop()` starts — but that's just as true here as in the original single-board `main()`, since Task 3's structure (build all N dashboards, *then* call `screen.Loop()`) keeps the same ordering; `bQuit`'s closure captures `screen` by reference and only calls `ExitLoopClosure()` inside its click handler, by which point the loop is running.

- [ ] **Step 8: Delete the now-duplicated block from `main.cpp`**

This step just removes dead code ahead of Task 3's replacement — `main.cpp` won't build again until Task 3 lands (expected, same pattern as Task 1).

Edit `tools/psb_demo_app/tui/main.cpp` — delete everything from `static void doFullScan(...)` through the closing brace of `static void rebuildChannelTitles(...)` (now living in `board_session.h`), and delete the connection-modal-through-`root`-Renderer block inside `main()` (now living in `board_dashboard.h`'s `makeBoardDashboard()`) — i.e. everything from `// ---- Connection inputs (live in the connection modal) ----` through the `CatchEvent(...)` chain closing `;`. Leave `main()`'s CLI11 parsing/topology-resolution block (lines 235-293) and the final `screen.Loop(root); ...; return 0; }` block — Task 3 replaces what's between them.

- [ ] **Step 9: Commit**

```bash
git add tools/psb_demo_app/tui/board_dashboard.h tools/psb_demo_app/tui/main.cpp
git commit -m "feat(psb_demo_tui): extract per-board dashboard UI into makeBoardDashboard() (main.cpp not yet updated — expected build break until Task 3)"
```

---

## Task 3: Topology-driven multi-board startup + bus worker threads

**Files:**
- Modify: `tools/psb_demo_app/tui/main.cpp` (full replacement)

**Interfaces:**
- Consumes: `psb::tui::BoardSession`/`BusWorker`/`doFullScan`/`doPollScan` (Task 1), `psb::tui::makeBoardDashboard` (Task 2), `psb::tui::makeBoardSwitcher` (Task 4 — `main.cpp` references it here; Task 4 lands next so the build stays broken between Tasks 3 and 4, same pattern as Tasks 1→3).
- Produces: the resolved-topology → `BusWorker`/`BoardSession` construction, and the `bool autoConnectAll` rule (`!portArg.empty() || topo.totalBoardCount() > 1`) that decides whether every resolved board auto-connects at startup or only pre-fills its connection modal.

**Auto-connect rule, spelled out:** `-p` still auto-connects its one synthesized board (unchanged from Phase 1). A topology resolving to exactly one board still only pre-fills that board's modal — unchanged from Phase 1's tested behavior (`ConfigManager`'s old auto-load-as-default never auto-connected by itself either). A topology resolving to more than one board is new in this phase: every board in it auto-connects at startup, since there's no single "Connect" action left once N>1 boards each have their own modal reachable only after switching to them — a user who pointed `--topology` at a multi-board file clearly wants all of it live, not N manual clicks.

- [ ] **Step 1: Replace `tools/psb_demo_app/tui/main.cpp` in full**

```cpp
#include "psb_serial_bus.h"
#include "psb_board_session.h"
#include "topology_config.h"
#include "board_session.h"
#include "board_dashboard.h"
#include "board_switcher.h"
#include "tool_version.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include <CLI/CLI.hpp>

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace ftxui;

static int g_pollInterval = 1;

int main(int argc, char** argv) {
    CLI::App app{"PSB Demo TUI"};
    app.set_version_flag("--version", std::string("psb_demo_tui ") + TOOL_VERSION_STRING);

    std::string portArg;
    int baudArg = 115200, slaveArg = 1, timeoutArg = 3000;
    std::string topologyPath = psb::TopologyConfig::defaultPath();
    app.add_option("-p,--port", portArg, "Serial port (quick single-board connect; auto-connects at startup)");
    app.add_option("-b,--baud", baudArg, "Baud rate")
        ->check(CLI::IsMember({9600, 19200, 38400, 115200}));
    app.add_option("-i,--id", slaveArg, "Slave ID")->check(CLI::Range(0, 247));
    app.add_option("-t,--timeout", timeoutArg, "Timeout ms");
    app.add_option("-s,--poll-interval", g_pollInterval, "Idle poll interval (s)");
    auto* topologyOpt = app.add_option("-T,--topology", topologyPath,
        "Topology config file (default: " + topologyPath + ")");
    CLI11_PARSE(app, argc, argv);

    psb::TopologyConfig topo;
    if (!portArg.empty()) {
        topo = psb::TopologyConfig::singleBoard(portArg, baudArg, slaveArg, "board1");
    } else {
        bool topologyExplicit = topologyOpt->count() > 0;
        if (psb::TopologyConfig::exists(topologyPath)) {
            auto loaded = psb::TopologyConfig::load(topologyPath);
            if (!loaded.has_value()) {
                std::cerr << "Topology config error: could not parse " << topologyPath << "\n";
                return 1;
            }
            topo = std::move(*loaded);
        } else if (topologyExplicit) {
            std::cerr << "Topology config error: " << topologyPath << " not found\n";
            return 1;
        } else {
            // Neither -p nor a resolvable --topology: fall back to today's
            // hardcoded first-run guess.
            topo = psb::TopologyConfig::singleBoard("/dev/ttyUSB0", 115200, 1, "board1");
        }
    }
    if (topo.totalBoardCount() == 0) {
        std::cerr << "Topology config " << topologyPath << " has no boards configured.\n";
        return 1;
    }
    bool autoConnectAll = !portArg.empty() || topo.totalBoardCount() > 1;

    auto screen = ScreenInteractive::Fullscreen();
    std::atomic<bool> running{true};

    // ---- Build one BusWorker per bus, one BoardSession per board ----
    std::vector<std::unique_ptr<psb::tui::BusWorker>> busWorkers;
    std::vector<std::unique_ptr<psb::tui::BoardSession>> boards;
    for (const auto& busCfg : topo.buses) {
        auto bw = std::make_unique<psb::tui::BusWorker>();
        bw->bus = std::make_shared<psb::PsbSerialBus>();
        for (const auto& boardCfg : busCfg.boards) {
            auto b = std::make_unique<psb::tui::BoardSession>();
            b->nickname = boardCfg.nickname;
            b->bus = bw->bus;
            b->client = std::make_unique<psb::PsbBoardSession>(bw->bus, boardCfg.slaveId);
            b->portVal = busCfg.port;
            b->baudVal = std::to_string(busCfg.baudRate);
            b->slaveVal = std::to_string(boardCfg.slaveId);
            b->appState = std::make_unique<psb::tui::AppState>(
                *b->client, b->connected, b->data, b->statusMsg, b->statusMutex,
                bw->workQueue, bw->workMutex, bw->workCv, screen);
            b->dashboard = psb::tui::makeBoardDashboard(*b, *bw, screen, running, timeoutArg);
            bw->boards.push_back(b.get());
            boards.push_back(std::move(b));
        }
        busWorkers.push_back(std::move(bw));
    }

    // ---- Bus worker threads — one per bus, serialises all I/O for every
    //      board sharing it (client-architecture-and-pitfalls.md §2.1,
    //      generalized from one port total to one port per bus). Each
    //      thread's first action, before entering its poll loop, is an
    //      auto-connect sweep over its own boards if autoConnectAll — the
    //      bus itself connects once (all boards on it share one physical
    //      port/baud, by definition of "same bus" in the topology schema),
    //      then each board is verified and full-scanned in turn. ----
    for (auto& bwPtr : busWorkers) {
        psb::tui::BusWorker& bw = *bwPtr;
        bw.thread = std::thread([&bw, &running, &screen, autoConnectAll, timeoutArg] {
            if (autoConnectAll && !bw.boards.empty()) {
                std::string port = bw.boards.front()->portVal;
                int baud = 115200;
                try { baud = std::stoi(bw.boards.front()->baudVal); } catch (...) {}
                bool busOk = bw.bus->connect(port, baud, timeoutArg);
                for (psb::tui::BoardSession* b : bw.boards) {
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
            }
            while (running) {
                {
                    bool anyConnected = false;
                    for (psb::tui::BoardSession* b : bw.boards)
                        if (b->connected.load()) { anyConnected = true; break; }
                    auto waitDur = anyConnected ? std::chrono::milliseconds(50)
                                                : std::chrono::seconds(g_pollInterval);
                    std::unique_lock<std::mutex> lk(bw.workMutex);
                    bw.workCv.wait_for(lk, waitDur, [&] { return !bw.workQueue.empty() || !running; });
                }
                for (;;) {
                    std::function<void()> item;
                    { std::lock_guard<std::mutex> lk(bw.workMutex);
                      if (bw.workQueue.empty()) break;
                      item = std::move(bw.workQueue.front()); bw.workQueue.pop(); }
                    item();
                }
                for (psb::tui::BoardSession* b : bw.boards) {
                    if (!running) break;
                    if (b->connected.load()) {
                        auto hasPendingWork = [&bw] {
                            std::lock_guard<std::mutex> lk(bw.workMutex);
                            return !bw.workQueue.empty();
                        };
                        psb::tui::doPollScan(*b->client, b->data, screen, running, hasPendingWork, b->statusMsg, b->statusMutex);
                        b->data.valid = b->connected.load() && b->client->isConnected();
                    }
                }
            }
        });
    }

    // ---- Animation thread — drives breathing LED at ~12 Hz for whichever
    //      board is currently visible. Each board's Renderer computes its
    //      own breathing color fresh every repaint (a pure function of
    //      wall-clock time), so one shared periodic repaint trigger is
    //      enough regardless of which board is on screen. ----
    std::thread animThread([&] {
        while (running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            if (!running) break;
            bool anyConnected = false;
            for (auto& b : boards) if (b->connected.load()) { anyConnected = true; break; }
            if (anyConnected) screen.PostEvent(Event::Custom);
        }
    });

    // ---- Board switcher + active dashboard (Task 4) ----
    auto root = psb::tui::makeBoardSwitcher(boards, screen);

    screen.Loop(root);
    running = false;
    for (auto& bw : busWorkers) bw->workCv.notify_all();
    for (auto& bw : busWorkers) if (bw->thread.joinable()) bw->thread.join();
    if (animThread.joinable()) animThread.join();
    for (auto& bw : busWorkers) bw->bus->disconnect();
    return 0;
}
```

- [ ] **Step 2: Confirm the expected build failure**

Run: `cd tools && cmake --build build --target psb_demo_tui 2>&1 | tail -20`
Expected: FAIL — `fatal error: 'board_switcher.h' file not found` (Task 4 hasn't landed yet). This confirms everything up through the bus-worker construction compiles; the only missing piece is the switcher itself.

- [ ] **Step 3: Commit**

```bash
git add tools/psb_demo_app/tui/main.cpp
git commit -m "feat(psb_demo_tui): topology-driven multi-board startup, one worker thread per bus (board_switcher.h not yet added — expected build break until Task 4)"
```

---

## Task 4: Board switcher

**Files:**
- Create: `tools/psb_demo_app/tui/board_switcher.h`

**Interfaces:**
- Consumes: `psb::tui::BoardSession` (Task 1), each board's pre-built `dashboard` Component (Task 2/3).
- Produces: `psb::tui::makeBoardSwitcher(std::vector<std::unique_ptr<BoardSession>>& boards, ftxui::ScreenInteractive& screen) -> ftxui::Component`.

- [ ] **Step 1: Write `board_switcher.h`**

```cpp
#pragma once

#include "board_session.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include <memory>
#include <string>
#include <vector>

namespace psb::tui {

using namespace ftxui;

// With exactly one board, returns its dashboard directly — pixel-identical
// to today's single-board layout, no switcher visible (Global Constraints).
// With more than one, adds a board-switcher bar above the active board's
// dashboard, reusing each board's already-built Component from
// makeBoardDashboard() unchanged — nothing about a board's own UI is aware
// a switcher exists around it. Mirrors the existing Monitor/CHx tab pattern
// (Container::Tab + a Menu selecting the index) one level up.
inline Component makeBoardSwitcher(std::vector<std::unique_ptr<BoardSession>>& boards,
                                    ScreenInteractive& screen) {
    (void)screen;  // not needed directly here — each dashboard already captured it
    if (boards.size() == 1) return boards.front()->dashboard;

    auto boardNames = std::make_shared<std::vector<std::string>>();
    for (auto& b : boards) boardNames->push_back(b->nickname);
    auto activeBoard = std::make_shared<int>(0);

    MenuOption switcherOpt = MenuOption::Horizontal();
    switcherOpt.entries_option.transform = [](const EntryState& e) -> Element {
        auto t = text("  " + e.label + "  ");
        if (e.active)                t = t | bold | color(Color::Cyan);
        if (e.focused && !e.active)  t = t | inverted;
        if (!e.active && !e.focused) t = t | dim;
        return t;
    };
    auto switcherBar = Menu(boardNames.get(), activeBoard.get(), switcherOpt);

    Components dashboards;
    for (auto& b : boards) dashboards.push_back(b->dashboard);
    auto dashboardStack = Container::Tab(dashboards, activeBoard.get());

    auto mainContainer = Container::Vertical({switcherBar, dashboardStack});
    return Renderer(mainContainer, [switcherBar, dashboardStack] {
        return vbox({
            text(" Boards ") | bold | dim,
            switcherBar->Render(),
            separator(),
            dashboardStack->Render() | flex,
        });
    });
}

} // namespace psb::tui
```

- [ ] **Step 2: Build**

Run: `cd tools && cmake --build build --target psb_demo_tui 2>&1 | tail -60`
Expected: clean build.

- [ ] **Step 3: Commit**

```bash
git add tools/psb_demo_app/tui/board_switcher.h
git commit -m "feat(psb_demo_tui): add board switcher — hidden for a single board, a nickname bar above the dashboard for more than one"
```

---

## Task 5: Manual smoke tests

**Files:** none (verification only), following `docs/guide/client-architecture-and-pitfalls.md` §3's `tmux` methodology.

- [ ] **Step 1: Single-board via `-p` — must be pixel-identical to Phase 1**

```bash
cd tools
tmux new-session -d -s p2_single -x 120 -y 40 "./bin/psb_demo_tui -p /dev/ttyUSB0"
sleep 1
tmux capture-pane -t p2_single -p
tmux kill-session -t p2_single
```
Expected: no board-switcher bar visible at all (single board → `makeBoardSwitcher` returns the dashboard directly) — same menu bar / tab bar / status bar layout as every Phase 1 `tmux` capture. If a real board is attached at that path, it auto-connects exactly as before; if not, the same "Connecting..." → error flow as Phase 1.

- [ ] **Step 2: Single-board via `--topology` (one board) — pre-fill only, no auto-connect**

```bash
mkdir -p /tmp/psb_tui_p2_test
cat > /tmp/psb_tui_p2_test/one_board.toml <<'EOF'
[[bus]]
name = "bus1"
port = "/dev/ttyUSB0"
baud_rate = 115200

  [[bus.board]]
  nickname = "solo"
  slave_id = 1
EOF
tmux new-session -d -s p2_solo -x 120 -y 40 "./bin/psb_demo_tui --topology /tmp/psb_tui_p2_test/one_board.toml"
sleep 1
tmux capture-pane -t p2_solo -p
tmux kill-session -t p2_solo
```
Expected: no switcher bar (still exactly one board); dashboard shows "Not connected — click Connect" (no auto-connect), matching Phase 1's Task 7 Step 7 result verbatim.

- [ ] **Step 3: Multi-board via `--topology` — switcher appears, both auto-connect**

If two real boards are available (e.g. this session's `jw_lvb` on `/dev/ttyACM0` and `jw_hvb` on `/dev/ttyACM2` — see Phase 1's live-hardware pass), use their real ports; otherwise use two placeholder ports (`/dev/ttyUSB0`, `/dev/ttyUSB1`) and expect connection errors rather than live data — the point of this step is confirming the switcher/threading/auto-connect *mechanism*, not requiring hardware.

```bash
cat > /tmp/psb_tui_p2_test/two_boards.toml <<'EOF'
[[bus]]
name = "bus1"
port = "/dev/ttyACM0"
baud_rate = 115200

  [[bus.board]]
  nickname = "lvb-bench"
  slave_id = 1

[[bus]]
name = "bus2"
port = "/dev/ttyACM2"
baud_rate = 115200

  [[bus.board]]
  nickname = "hvb-bench"
  slave_id = 1
EOF
tmux new-session -d -s p2_multi -x 140 -y 45 "./bin/psb_demo_tui --topology /tmp/psb_tui_p2_test/two_boards.toml"
sleep 3
tmux capture-pane -t p2_multi -p
```
Expected: a switcher bar reading ` Boards ` with two entries (`lvb-bench`, `hvb-bench`), the first one active/highlighted; its dashboard below shows live data if a real board answers at that port, or a connection error if not — either way, both boards should show `connecting`/`connected`/error state independently (not one blocking the other — confirms the two `BusWorker` threads ran their auto-connect sweeps concurrently, not serialized against each other).

- [ ] **Step 4: Switch boards via the switcher bar**

```bash
tmux send-keys -t p2_multi Tab
sleep 0.3
tmux send-keys -t p2_multi Right
sleep 0.3
tmux capture-pane -t p2_multi -p
tmux kill-session -t p2_multi
```
Expected: the dashboard content changes to the second board's (`hvb-bench`) — different channel count / variant name in its menu bar if a real board answered there, confirming `Container::Tab`'s index correctly switches which board's pre-built dashboard renders.

- [ ] **Step 5: Confirm `psb_tests` is unaffected (this phase touches no `psb_modbus_core` files except the small `rebind()` addition, already covered by Task 2's own test)**

Run: `./build/psb_modbus_core/tests/psb_tests 2>&1 | tail -5`
Expected: PASS, same count as Task 2 Step 5's result (335 assertions, 86 test cases — the Phase 1 baseline of 334/85 plus `rebind()`'s one new assertion... — count the actual `psb_tests` output rather than trust this number blindly; the invariant that matters is "unchanged since Task 2, no regressions from Tasks 3-5").

- [ ] **Step 6: Clean up test artifacts**

```bash
rm -rf /tmp/psb_tui_p2_test
```

---

## Task 6: Full-repo verification

**Files:** none (verification only).

- [ ] **Step 1: Clean rebuild of every touched target**

Run: `cd tools && cmake --build build --target psb_modbus_core psb_tests psb_demo_cli psb_demo_tui 2>&1 | tail -40`
Expected: clean build — `psb_demo_cli` and `psb_modbus_core` weren't touched this phase (except the small `rebind()` addition), so this is mostly confirming nothing broke transitively.

- [ ] **Step 2: Full `psb_tests` run**

Run: `./build/psb_modbus_core/tests/psb_tests`
Expected: PASS.

- [ ] **Step 3: `psb_demo_gui` still builds (Global Constraint from Phase 1 — still untouched)**

Only if Qt is available (see Phase 1's Task 7 Step 3 for the path used on this machine):

Run: `cd tools/build && cmake -DBUILD_GUI=ON -DCMAKE_PREFIX_PATH=<qt-path> . && cd .. && cmake --build build --target psb_demo_gui`
Expected: clean build — `PsbModbusClient`'s public API is still untouched by this phase (Phase 2 only builds on top of `PsbBoardSession`/`PsbSerialBus` directly, in the TUI, never touching `psb_modbus_client.h/.cpp`).

- [ ] **Step 4: Live-hardware pass, if boards are available**

Repeat Task 5's Steps 1-4 against real hardware if not already done there with real boards attached, paying particular attention to:
- Uptime/telemetry on a *background* (not currently selected) board continuing to update — switch away from board A, watch board B for a while, switch back to A and confirm its uptime/status kept advancing the whole time (proves per-bus threads keep polling boards that aren't currently visible, per the spec's "all boards poll continuously in background" requirement).
- A write action (e.g. toggling output, editing Vset) on the currently-selected board completes correctly and doesn't affect the other board's display.
- Quit (`[ Quit ]`) cleanly joins every bus thread and the animation thread with boards in a mix of connected/disconnected state — no hang, matching Phase 1's `~/backup` session's documented "bounded-wait destructor" discipline (`psb_modbus_core`'s equivalent is the join loop at the end of `main()` here, not a destructor, but the same "must not hang forever" principle applies).

---

## Self-Review

**Spec coverage** (against `docs/superpowers/specs/2026-07-20-multi-board-topology-design.md`'s Rollout §2 "TUI multi-board dashboard"):
- ✅ Per-bus worker threads — Task 3 (`BusWorker::thread`, one per resolved bus).
- ✅ Per-board `ScannedData` — Task 1 (`BoardSession::data`).
- ✅ Board switcher UI — Task 4, hidden for one board (pixel-identical to Phase 1) per the spec's explicit requirement.
- ✅ Offline detection (§2.7) and capability self-heal (§1.5) — unchanged, inherited for free since `doPollScan`/`doFullScan` (Task 1) are the same logic, just parameterized; they already implement both.
- ✅ "All boards poll continuously in background" — Task 3's bus worker loop runs regardless of which board the switcher currently shows; Task 5 Step 4 / Task 6 Step 4 explicitly verify this live.

**Placeholder scan:** the one placeholder introduced during drafting (`bQuit`'s `screen_uninitialized_guard` label) was caught and fixed inline before this plan was finalized — grep confirms zero remaining occurrences. No other TBD/TODO markers; every code step contains complete, compilable content.

**Type consistency:** cross-checked `BoardSession`/`BusWorker` field names and `doFullScan`/`doPollScan`/`makeBoardDashboard`/`makeBoardSwitcher` signatures across Tasks 1, 2, 3, 4 — all call sites match the declarations exactly (`board.client` is `unique_ptr<PsbBoardSession>`, dereferenced consistently as `*board.client` wherever `PsbBoardSession&` is expected; `busWorker.workQueue`/`workMutex`/`workCv` referenced identically in Task 2's dashboard closures and Task 3's thread body).

Plan complete and saved to `docs/superpowers/plans/2026-07-20-multi-board-topology-phase2-tui-dashboard.md`. Two execution options:

1. **Subagent-Driven (recommended)** — I dispatch a fresh subagent per task, review between tasks, fast iteration
2. **Inline Execution** — Execute tasks in this session using executing-plans, batch execution with checkpoints

Which approach?
