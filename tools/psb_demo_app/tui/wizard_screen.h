#pragma once

#include "wizard_state.h"
#include "wizard_scan.h"
#include "widgets.h"
#include "tui_policy.h"
#include "psb_serial_bus.h"
#include "topology_path_picker.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace psb::tui {

using namespace ftxui;

enum class WizardOutcome { Cancelled, SavedOnly, ConnectNow };

// Hand-off payload for the scan thread → UI thread transition. The scan
// thread never writes scanResults/scanResultLabels/scanStatus directly —
// those vectors have Menu/Renderer widgets holding raw pointers into them
// (the same non-owning-pointer convention documented in board_switcher.h),
// and FTXUI's Menu reads its backing vector internally during Render()/
// OnEvent() with no lock of its own, so a background thread mutating that
// vector concurrently (clear()/push_back() can reallocate) would be
// unsynchronized access to memory the UI thread's Render() is also
// touching — undefined behavior, not just a display glitch. Instead the
// scan thread stages a completed result set here under scanMutex and flips
// scanUpdateReady; the UI thread (which is the only thread that ever
// touches the live vectors) drains it at the top of addBoardPopup's
// Renderer, before any widget that reads those vectors runs — so the live
// vectors are never touched by more than one thread.
struct ScanUpdate {
    std::vector<DiscoveredBoard> results;
    std::vector<std::string> labels;
    std::string status;
};

// Builds the Topology wizard's Component — a bus/board list plus Add Bus, Add
// Board (Manual or Scan), Remove, Save/Save As, and Connect Now/Done.
// Reused unmodified as both `main()`'s standalone pre-dashboard root (Task
// 6) and a Modal overlay atop a live dashboard (Task 7's mid-session entry)
// — this function has no opinion on which; `onFinish` is how the caller
// finds out what happened.
//
// scanViaLiveBus: if non-empty and it returns true for a given port, a live
// BusWorker took the scan (routed through its own already-open connection —
// see main.cpp's implementation) and reports back asynchronously via the
// callback passed to it. Start Scan's handler falls back to today's direct-
// connect scan when this is empty or returns false for that port. The
// standalone pre-dashboard entry point passes nothing (no Runtime exists
// yet to route through).
using ScanViaLiveBus = std::function<bool(const std::string& port, int start, int end,
                                          std::function<void(std::vector<DiscoveredBoard>, std::string)> onDone)>;

inline Component makeWizardScreen(WizardState& s, ScreenInteractive& screen,
                                  std::function<void(WizardOutcome)> onFinish,
                                  bool allowScan = true,
                                  ScanViaLiveBus scanViaLiveBus = {}) {
    // ---- Bus/board list (left pane) ----
    auto busNames = std::make_shared<std::vector<std::string>>();
    auto rebuildBusNames = [&s, busNames] {
        busNames->clear();
        for (const auto& b : s.topo.buses)
            busNames->push_back(b.name + " (" + b.port + ")");
    };
    rebuildBusNames();
    auto busMenu = Menu(busNames.get(), &s.selectedBus);

    auto boardNames = std::make_shared<std::vector<std::string>>();
    auto rebuildBoardNames = [&s, boardNames] {
        boardNames->clear();
        if (s.selectedBus >= 0 && s.selectedBus < static_cast<int>(s.topo.buses.size()))
            for (const auto& b : s.topo.buses[s.selectedBus].boards)
                boardNames->push_back(b.nickname + " (#" + std::to_string(b.slaveId) + ")");
    };
    rebuildBoardNames();
    auto boardMenu = Menu(boardNames.get(), &s.selectedBoard);

    // ---- Add Bus modal ----
    bool showAddBus = false;
    auto newBusName = std::make_shared<std::string>();
    auto newBusPort = std::make_shared<std::string>();
    auto newBusBaud = std::make_shared<std::string>("115200");
    auto portList = std::make_shared<std::vector<std::string>>();
    auto portIdx = std::make_shared<int>(-1);
    auto doScanPorts = [portList, portIdx, newBusPort, &screen] {
        *portList = PsbSerialBus::scanPorts();
        *portIdx = selectedPortIndex(*portList, *newBusPort);
        *newBusPort = *portIdx >= 0 ? (*portList)[*portIdx] : std::string{};
        screen.PostEvent(Event::Custom);
    };
    auto portDropdown = Dropdown(portList.get(), portIdx.get());
    auto visiblePortDropdown = Maybe(portDropdown, [portList] { return !portList->empty(); });
    auto bScanPorts = Button("Rescan", [doScanPorts] { doScanPorts(); });
    auto busNameInp = Input(newBusName.get(), "name (optional)");
    auto busBaudInp = Input(newBusBaud.get(), "baud");
    auto showAddBusPtr = std::make_shared<bool>(false);

    auto bAddBusConfirm = ActionButton("Add", [&s, newBusName, newBusPort, newBusBaud,
                                                portList, portIdx,
                                                rebuildBusNames, showAddBusPtr, &screen] {
        int baud = 115200;
        try { baud = std::stoi(*newBusBaud); } catch (...) {}
        // Dropdown() only ever mutates portIdx as the user navigates it —
        // newBusPort is a separate string, written once by doScanPorts()
        // right after a scan and never re-synced afterward. Reading
        // portList[*portIdx] here (the live selection) instead of the
        // stale *newBusPort fixes Add always using whichever port was
        // selected at scan time (index 0) regardless of what the user
        // picked in the dropdown afterward. Falls back to *newBusPort only
        // when the list is empty (dropdown hidden, nothing to select).
        std::string port = (*portIdx >= 0 && *portIdx < static_cast<int>(portList->size()))
            ? (*portList)[*portIdx] : *newBusPort;
        std::string err = addBus(s, *newBusName, port, baud);
        s.statusMsg = err.empty() ? "" : "Error: " + err;
        if (err.empty()) {
            rebuildBusNames();
            *showAddBusPtr = false;
            newBusName->clear(); newBusPort->clear(); *newBusBaud = "115200";
        }
        screen.PostEvent(Event::Custom);
    });
    auto bAddBusCancel = ActionButton("Cancel", [showAddBusPtr, &screen] {
        *showAddBusPtr = false; screen.PostEvent(Event::Custom);
    });
    auto addBusForm = Container::Vertical({visiblePortDropdown, bScanPorts, busNameInp, busBaudInp, bAddBusConfirm, bAddBusCancel});
    auto addBusPopup = Renderer(addBusForm, [newBusPort, portList, visiblePortDropdown, bScanPorts, busNameInp, busBaudInp, bAddBusConfirm, bAddBusCancel] {
        Element portChoice = portList->empty()
            ? text("(no ports found — Rescan)") | dim | flex
            : visiblePortDropdown->Render() | flex;
        return vbox({
            text(" Add Bus ") | bold | center, separator(),
            hbox({ text("Port : "), portChoice, text(" "), bScanPorts->Render() }),
            hbox({ text("Name : "), busNameInp->Render() }),
            hbox({ text("Baud : "), busBaudInp->Render() | size(WIDTH, EQUAL, 8) }),
            separator(),
            hbox({ bAddBusConfirm->Render(), text("  "), bAddBusCancel->Render() }) | center,
        }) | border | size(WIDTH, EQUAL, 44);
    });

    // ---- Add Board modal (Manual fields always visible; Scan is a
    //      separate action within the same modal, results feed the same
    //      nickname/slave-ID fields for a one-click add) ----
    auto showAddBoardPtr = std::make_shared<bool>(false);
    auto newBoardNick = std::make_shared<std::string>();
    auto newBoardSlave = std::make_shared<std::string>("1");
    auto scanStart = std::make_shared<std::string>("1");
    auto scanEnd = std::make_shared<std::string>("32");
    auto scanning = std::make_shared<std::atomic<bool>>(false);
    auto scanProgress = std::make_shared<std::atomic<int>>(0);
    auto scanResults = std::make_shared<std::vector<DiscoveredBoard>>();
    auto scanResultLabels = std::make_shared<std::vector<std::string>>();
    auto scanResultIdx = std::make_shared<int>(-1);
    auto scanStatus = std::make_shared<std::string>();
    // Cross-thread hand-off for scanResults/scanResultLabels/scanStatus —
    // see ScanUpdate's comment above. scanMutex guards scanStaged only; the
    // live vectors above are touched exclusively by the UI thread.
    auto scanMutex = std::make_shared<std::mutex>();
    auto scanStaged = std::make_shared<ScanUpdate>();
    auto scanUpdateReady = std::make_shared<std::atomic<bool>>(false);

    // Removes already-added boards from the candidate list, keeping
    // scanResults/scanResultLabels in sync and scanResultIdx clamped into
    // range. Run after every successful Add (so the candidate list narrows
    // to what's still worth adding, without forcing a Rescan) and after
    // every scan-thread drain (so a fresh Rescan's results are filtered the
    // same way against whatever's already in s.topo by then). This is what
    // makes "select/add one board" leave the rest of a multi-board scan's
    // results visible and ready for the next Add, instead of the whole
    // list vanishing until the user rescans from scratch.
    auto filterAlreadyAdded = [&s, scanResults, scanResultLabels, scanResultIdx] {
        if (s.selectedBus < 0 || s.selectedBus >= static_cast<int>(s.topo.buses.size())) return;
        const auto& existing = s.topo.buses[s.selectedBus].boards;
        std::vector<DiscoveredBoard> keptResults;
        std::vector<std::string> keptLabels;
        for (size_t i = 0; i < scanResults->size(); ++i) {
            bool already = false;
            for (const auto& b : existing)
                if (b.slaveId == (*scanResults)[i].slaveId) { already = true; break; }
            if (!already) {
                keptResults.push_back((*scanResults)[i]);
                keptLabels.push_back((*scanResultLabels)[i]);
            }
        }
        *scanResults = std::move(keptResults);
        *scanResultLabels = std::move(keptLabels);
        if (*scanResultIdx >= static_cast<int>(scanResultLabels->size()))
            *scanResultIdx = scanResultLabels->empty() ? -1 : static_cast<int>(scanResultLabels->size()) - 1;
    };

    auto boardNickInp = Input(newBoardNick.get(), "nickname");
    auto boardSlaveInp = Input(newBoardSlave.get(), "1-247");
    auto scanStartInp = Input(scanStart.get(), "start");
    auto scanEndInp = Input(scanEnd.get(), "end");
    auto scanResultsMenu = Menu(scanResultLabels.get(), scanResultIdx.get());

    // Deliberately does not close the modal on success (no
    // `*showAddBoardPtr = false`) — closing after every single add forced
    // the user to reopen "Add Board" from scratch for each board on the
    // same bus. Staying open lets them add several boards back-to-back
    // (manual entry or scan-assisted) in one uninterrupted pass; Cancel is
    // now the only way to close it, once they're done with this bus.
    auto bAddBoardConfirm = ActionButton("Add", [&s, newBoardNick, newBoardSlave,
                                                  rebuildBoardNames, filterAlreadyAdded, &screen] {
        int slave = 1;
        try { slave = std::stoi(*newBoardSlave); } catch (...) {}
        std::string err = addBoard(s, s.selectedBus, *newBoardNick, slave);
        if (err.empty()) {
            s.statusMsg = "Added " + *newBoardNick + " (#" + std::to_string(slave) + ").";
            rebuildBoardNames();
            filterAlreadyAdded();
            newBoardNick->clear(); *newBoardSlave = "1";
        } else {
            s.statusMsg = "Error: " + err;
        }
        screen.PostEvent(Event::Custom);
    });

    auto bStartScan = ActionButton("Start Scan", [&s, scanStart, scanEnd, scanning,
                                                   scanProgress, scanMutex, scanStaged,
                                                   scanUpdateReady, &screen, scanViaLiveBus] {
        if (scanning->load() || s.selectedBus < 0) return;
        int start = 1, end = 32;
        try { start = std::stoi(*scanStart); } catch (...) {}
        try { end = std::stoi(*scanEnd); } catch (...) {}
        if (start < 0) start = 0;
        if (end > 247) end = 247;
        if (end < start) return;

        const std::string port = s.topo.buses[s.selectedBus].port;
        const int baud = s.topo.buses[s.selectedBus].baudRate;
        scanning->store(true);
        // Stage the "connecting" status through the same hand-off as the
        // final result — bStartScan runs on the UI thread, so writing
        // scanStatus directly here would be safe in isolation, but staging
        // it too keeps exactly one path ever touching the live vectors.
        {
            std::lock_guard<std::mutex> lk(*scanMutex);
            scanStaged->results.clear();
            scanStaged->labels.clear();
            scanStaged->status = "Connecting to " + port + "...";
        }
        scanUpdateReady->store(true);
        screen.PostEvent(Event::Custom);

        // Stages a completed (or errored) scan result through the same
        // hand-off regardless of which path produced it — shared so a
        // live-bus scan (running on a BusWorker's own thread, reached via
        // scanViaLiveBus) and a direct-connect scan (its own std::thread,
        // below) report back identically; the UI-side drain in
        // addBoardPopup's Renderer doesn't need to know which one ran.
        auto stageResult = [scanning, scanMutex, scanStaged, scanUpdateReady, &screen]
                           (std::vector<DiscoveredBoard> results, std::string status) {
            std::vector<std::string> labels;
            for (const auto& r : results)
                labels.push_back(r.variantName + "  #" + std::to_string(r.slaveId));
            {
                std::lock_guard<std::mutex> lk(*scanMutex);
                scanStaged->results = std::move(results);
                scanStaged->labels = std::move(labels);
                scanStaged->status = std::move(status);
            }
            scanUpdateReady->store(true);
            scanning->store(false);
            screen.PostEvent(Event::Custom);
        };

        // Route through an already-open connection when this bus is part
        // of the currently-running session — no second PsbSerialBus opened
        // on a port a BusWorker thread may already be driving. Falls back
        // to the direct-connect path below when no live worker owns this
        // port (a bus just added in this wizard session but not yet
        // Applied, or the standalone pre-dashboard entry point, which
        // never has a scanViaLiveBus at all).
        if (scanViaLiveBus && scanViaLiveBus(port, start, end, stageResult)) {
            return;
        }

        std::thread([&screen, scanProgress, port, baud, start, end, stageResult] {
            auto scanBusHandle = std::make_shared<PsbSerialBus>();
            if (!scanBusHandle->connect(port, baud, 500)) {
                stageResult({}, "Error: " + scanBusHandle->lastError());
                return;
            }
            auto results = scanBus(scanBusHandle, start, end, [&](int id) {
                scanProgress->store(id);
                screen.PostEvent(Event::Custom);
            });
            scanBusHandle->disconnect();
            std::string status = results.empty()
                ? "No boards found in range."
                : std::to_string(results.size()) + " board(s) found.";
            stageResult(std::move(results), std::move(status));
        }).detach();
    });

    auto bUseScanResult = ActionButton("Use Selected", [scanResults, scanResultIdx,
                                                          newBoardNick, newBoardSlave, &screen] {
        int i = *scanResultIdx;
        if (i < 0 || i >= static_cast<int>(scanResults->size())) return;
        if (newBoardNick->empty()) *newBoardNick = (*scanResults)[i].variantName;
        *newBoardSlave = std::to_string((*scanResults)[i].slaveId);
        screen.PostEvent(Event::Custom);
    });

    auto bAddBoardCancel = ActionButton("Cancel", [showAddBoardPtr, &screen] {
        *showAddBoardPtr = false; screen.PostEvent(Event::Custom);
    });

    // Scan-related Components (scanStartInp/scanEndInp/bStartScan/
    // scanResultsMenu/bUseScanResult) are only added to the container tree
    // when allowScan is true. Hiding them from the Renderer's Elements
    // alone would not be enough: FTXUI's keyboard navigation (Tab/Down)
    // walks the *container* tree, not what a Renderer chooses to draw, so a
    // Component present here but merely undrawn would still be reachable
    // and clickable. Mid-session (allowScan=false) that would let a user
    // Tab onto the invisible "Start Scan" button and open a second
    // PsbSerialBus on a port a BusWorker thread may already be driving —
    // the exact two-threads-on-one-port hazard this task's scope note
    // explicitly rules out. So the exclusion happens here, at container
    // construction, not just in the Renderer below.
    Components addBoardChildren = { boardNickInp, boardSlaveInp, bAddBoardConfirm };
    if (allowScan) {
        addBoardChildren.insert(addBoardChildren.end(),
            { scanStartInp, scanEndInp, bStartScan, scanResultsMenu, bUseScanResult });
    }
    addBoardChildren.push_back(bAddBoardCancel);
    auto addBoardForm = Container::Vertical(addBoardChildren);
    auto addBoardPopup = Renderer(addBoardForm, [&s, boardNickInp, boardSlaveInp, bAddBoardConfirm,
                                                  scanStartInp, scanEndInp, bStartScan,
                                                  scanResultsMenu, bUseScanResult, bAddBoardCancel,
                                                  scanning, scanProgress, scanStatus, scanResults,
                                                  scanResultLabels, scanMutex, scanStaged,
                                                  scanUpdateReady, filterAlreadyAdded, allowScan] {
        // Drain any scan-thread hand-off before any widget below reads
        // scanResultLabels/scanResults/scanStatus (scanResultsMenu->Render()
        // in particular touches scanResultLabels internally) — this Renderer
        // lambda only ever runs on the UI/event-loop thread, so applying the
        // staged update here means the live vectors are never touched by
        // more than one thread. See ScanUpdate's comment for why a mutex
        // around the live vectors alone wouldn't be enough.
        if (scanUpdateReady->exchange(false)) {
            {
                std::lock_guard<std::mutex> lk(*scanMutex);
                *scanResults = scanStaged->results;
                *scanResultLabels = scanStaged->labels;
                *scanStatus = scanStaged->status;
            }
            // A fresh scan can rediscover a board added since the previous
            // scan (e.g. add one, then sweep the range again) — filter it
            // back out so the candidate list only ever shows what's still
            // worth adding.
            filterAlreadyAdded();
        }
        std::string busLabel = (s.selectedBus >= 0 && s.selectedBus < static_cast<int>(s.topo.buses.size()))
            ? s.topo.buses[s.selectedBus].name : "(none)";
        Element scanStatusEl = scanning->load()
            ? text("Scanning... #" + std::to_string(scanProgress->load())) | color(Color::Yellow)
            : text(*scanStatus) | dim;
        Elements body = {
            text(" Add Board — " + busLabel + " ") | bold | center, separator(),
            hbox({ text("Nickname : "), boardNickInp->Render() }),
            hbox({ text("Slave ID : "), boardSlaveInp->Render() | size(WIDTH, EQUAL, 5) }),
            bAddBoardConfirm->Render() | center,
        };
        if (allowScan) {
            body.push_back(separator());
            body.push_back(text(" Or scan for boards ") | bold | center);
            body.push_back(hbox({ text("Range: "), scanStartInp->Render() | size(WIDTH, EQUAL, 4),
                                   text(" - "), scanEndInp->Render() | size(WIDTH, EQUAL, 4),
                                   text(" "), bStartScan->Render() }));
            body.push_back(scanStatusEl);
            if (!scanResultLabels->empty()) {
                body.push_back(scanResultsMenu->Render() | frame | size(HEIGHT, LESS_THAN, 6));
                body.push_back(bUseScanResult->Render() | center);
            }
        }
        body.push_back(separator());
        body.push_back(bAddBoardCancel->Render() | center);
        return vbox(std::move(body)) | border | size(WIDTH, EQUAL, 46);
    });

    // ---- List actions ----
    auto bAddBus = ActionButton("Add Bus", [showAddBusPtr, doScanPorts, &screen] {
        doScanPorts(); *showAddBusPtr = true; screen.PostEvent(Event::Custom);
    });
    auto bRemoveBus = ActionButton("Remove Bus", [&s, rebuildBusNames, rebuildBoardNames, &screen] {
        if (s.selectedBus < 0) return;
        s.statusMsg = removeBus(s, s.selectedBus);
        rebuildBusNames(); rebuildBoardNames();
        screen.PostEvent(Event::Custom);
    });
    // FTXUI's MenuBase::Clamp() runs on every Render() and, for an empty
    // list, evaluates util::clamp(-1, 0, -1) to 0 — silently promoting
    // busMenu/boardMenu's "-1 = none selected" sentinel to 0 the moment an
    // empty wizard first renders. A plain `s.selectedBus >= 0` check is
    // fooled by that promotion (0 >= 0 is true even with zero buses), so
    // these predicates check the index is actually in range instead —
    // robust to the same promotion for boards, and to any other stale
    // out-of-range selection value, not just the empty-list case.
    auto busInRange = [&s] { return s.selectedBus >= 0 && s.selectedBus < static_cast<int>(s.topo.buses.size()); };
    auto boardInRange = [&s, busInRange] {
        return busInRange() && s.selectedBoard >= 0
            && s.selectedBoard < static_cast<int>(s.topo.buses[s.selectedBus].boards.size());
    };
    auto busSelectable = Maybe(bRemoveBus, busInRange);

    auto bAddBoard = ActionButton("Add Board", [showAddBoardPtr, scanResultLabels, scanResults,
                                                 scanUpdateReady, &screen] {
        scanResultLabels->clear(); scanResults->clear();
        // Discard any not-yet-applied result from a scan started before
        // this reopen — without this, a stale scanUpdateReady could
        // overwrite this fresh clear the next time addBoardPopup renders.
        scanUpdateReady->store(false);
        *showAddBoardPtr = true; screen.PostEvent(Event::Custom);
    });
    auto addBoardEnabled = Maybe(bAddBoard, busInRange);
    auto bRemoveBoard = ActionButton("Remove Board", [&s, rebuildBoardNames, &screen] {
        if (s.selectedBus < 0 || s.selectedBoard < 0) return;
        s.statusMsg = removeBoard(s, s.selectedBus, s.selectedBoard);
        rebuildBoardNames();
        screen.PostEvent(Event::Custom);
    });
    auto boardSelectable = Maybe(bRemoveBoard, boardInRange);

    // ---- Save / Save As / Load / Browse / Connect / Cancel ----
    auto topologyPathInp = Input(&s.topologyPath, "topology file path");
    // Extracted so the path picker's own "Open" (on a selected file) runs
    // the identical load — Load's own button and a picked file both funnel
    // through this one place, never two copies of the same logic.
    auto doLoadTopology = [&s, rebuildBusNames, rebuildBoardNames, &screen] {
        auto loaded = psb::TopologyConfig::load(s.topologyPath);
        if (loaded.has_value()) {
            s.topo = std::move(*loaded);
            s.selectedBus = -1;
            s.selectedBoard = -1;
            s.dirty = false;
            s.statusMsg = "Loaded " + s.topologyPath;
            rebuildBusNames();
            rebuildBoardNames();
        } else {
            s.statusMsg = "Error: could not load " + s.topologyPath;
        }
        screen.PostEvent(Event::Custom);
    };
    auto bLoadTopology = ActionButton("Load", doLoadTopology);
    auto showPathPicker = std::make_shared<bool>(false);
    auto pathPicker = makePathPicker(screen, showPathPicker, s.topologyPath, doLoadTopology);
    auto bBrowsePath = ActionButton("Browse...", pathPicker.open);
    auto bSave = ActionButton("Save", [&s, onFinish, &screen] {
        if (s.topo.save(s.topologyPath)) {
            s.dirty = false;
            s.statusMsg = "Saved to " + s.topologyPath;
        } else {
            s.statusMsg = "Error: could not save to " + s.topologyPath;
        }
        screen.PostEvent(Event::Custom);
    });
    // Mid-session (allowScan=false), ConnectNow applies additive changes to
    // the already-running session rather than starting one, and Save &
    // Exit only closes this modal — the dashboard underneath keeps running,
    // nothing actually "exits". Reusing allowScan as the mid-session signal
    // (the same construction-time parameter that already distinguishes the
    // two call sites) to pick the labels that describe what each button
    // does in context, rather than always showing the pre-dashboard wording.
    auto bConnectNow = ActionButton(allowScan ? "Connect Now" : "Apply", [&s, onFinish] {
        onFinish(WizardOutcome::ConnectNow);
    });
    auto bDone = ActionButton(allowScan ? "Save & Exit" : "Save & Close", [&s, onFinish, &screen] {
        if (s.topo.save(s.topologyPath)) {
            s.dirty = false;
            onFinish(WizardOutcome::SavedOnly);
        } else {
            // Stay in the wizard on failure — exiting via onFinish(Cancelled)
            // here would be indistinguishable from the user clicking plain
            // Cancel, silently discarding their edits with no diagnostic.
            s.statusMsg = "Error: could not save to " + s.topologyPath;
            screen.PostEvent(Event::Custom);
        }
    });
    auto bCancel = ActionButton("Cancel", [onFinish] { onFinish(WizardOutcome::Cancelled); });

    auto mainContainer = Container::Vertical({
        busMenu, bAddBus, busSelectable,
        boardMenu, addBoardEnabled, boardSelectable,
        topologyPathInp, bBrowsePath, bLoadTopology,
        bSave, bConnectNow, bDone, bCancel,
    });

    auto root = Renderer(mainContainer, [&s, busMenu, bAddBus, busSelectable,
                                         boardMenu, addBoardEnabled, boardSelectable,
                                         topologyPathInp, bBrowsePath, bLoadTopology,
                                         bSave, bConnectNow, bDone, bCancel,
                                         rebuildBusNames, rebuildBoardNames] {
        // Re-derive busNames/boardNames from s.topo on every render rather
        // than relying solely on the wizard's own Add/Remove/Load handlers
        // to keep them in sync. Those handlers are the only writers when the
        // wizard drives its own topo end-to-end (Tasks 5/6's pre-dashboard
        // entry point), but Task 7's mid-session entry point reseeds s.topo
        // from main()'s live `openSetup` closure — a write to s.topo that
        // happens entirely outside this file, which busNames/boardNames
        // would otherwise never learn about, leaving the modal's Buses/
        // Boards panels stuck showing whatever was cached at construction
        // time (empty, the very first time) instead of the pre-populated
        // list the mid-session entry point promises. Rebuilding here is
        // idempotent and cheap (two small string vectors) so it's harmless
        // for the pre-dashboard callers too.
        rebuildBusNames();
        rebuildBoardNames();
        return vbox({
            text(" Topology Wizard " + std::string(s.dirty ? "*" : "") + " ") | bold | center,
            separator(),
            hbox({
                vbox({ text("Buses") | bold, busMenu->Render() | frame | flex,
                       hbox({ bAddBus->Render(), text(" "), busSelectable->Render() }) }) | flex | border,
                vbox({ text("Boards") | bold, boardMenu->Render() | frame | flex,
                       hbox({ addBoardEnabled->Render(), text(" "), boardSelectable->Render() }) }) | flex | border,
            }) | flex,
            separator(),
            hbox({ text("Path: "), topologyPathInp->Render() | flex, text(" "), bBrowsePath->Render(),
                   text(" "), bLoadTopology->Render() }),
            text(" " + s.statusMsg + " ") | (s.statusMsg.rfind("Error", 0) == 0 ? color(Color::Red) : color(Color::Green)),
            separator(),
            hbox({ bSave->Render(), text("  "), bConnectNow->Render(), text("  "),
                   bDone->Render(), text("  "), bCancel->Render() }) | center,
        }) | border | size(WIDTH, GREATER_THAN, 100) | size(HEIGHT, GREATER_THAN, 30);
    }) | Modal(addBusPopup, showAddBusPtr.get())
       | Modal(addBoardPopup, showAddBoardPtr.get())
       | Modal(pathPicker.root, showPathPicker.get());

    return root;
}

} // namespace psb::tui
