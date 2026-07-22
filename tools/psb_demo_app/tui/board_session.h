#pragma once

#include "tui_format.h"
#include "tui_policy.h"
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

    ftxui::Component dashboard;  // built once by makeBoardDashboard()

    // Set by makeBoardDashboard() to the same doConnect/doDisconnect
    // closures its own Connect/Disconnect toggle button already uses — lets
    // an external caller (main.cpp's Connect All/Disconnect All) trigger
    // identical per-board connection logic without duplicating it or
    // restructuring ownership. Empty (never called) until the dashboard is
    // built, which always happens before these could be reached.
    std::function<void()> connect;
    std::function<void()> disconnect;
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
    // Set (only ever by the UI thread, only once this bus's `boards` is
    // confirmed empty — see main.cpp's drainPendingRemovals) to stop this
    // one bus's worker thread without touching any other bus's thread.
    // Every bus thread today shares one global `running` flag with no
    // per-bus granularity; this is what removing the last board on a bus
    // needs that `running` alone can't provide.
    std::atomic<bool> stopRequested{false};
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
