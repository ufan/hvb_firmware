#pragma once

#include "wizard_state.h"
#include "wizard_scan.h"
#include "widgets.h"
#include "tui_policy.h"
#include "psb_serial_bus.h"

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

// Builds the setup wizard's Component — a bus/board list plus Add Bus, Add
// Board (Manual or Scan), Remove, Save/Save As, and Connect Now/Done.
// Reused unmodified as both `main()`'s standalone pre-dashboard root (Task
// 6) and a Modal overlay atop a live dashboard (Task 7's mid-session entry)
// — this function has no opinion on which; `onFinish` is how the caller
// finds out what happened.
inline Component makeWizardScreen(WizardState& s, ScreenInteractive& screen,
                                  std::function<void(WizardOutcome)> onFinish) {
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
                                                rebuildBusNames, showAddBusPtr, &screen] {
        int baud = 115200;
        try { baud = std::stoi(*newBusBaud); } catch (...) {}
        std::string err = addBus(s, *newBusName, *newBusPort, baud);
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

    auto boardNickInp = Input(newBoardNick.get(), "nickname");
    auto boardSlaveInp = Input(newBoardSlave.get(), "1-247");
    auto scanStartInp = Input(scanStart.get(), "start");
    auto scanEndInp = Input(scanEnd.get(), "end");
    auto scanResultsMenu = Menu(scanResultLabels.get(), scanResultIdx.get());

    auto bAddBoardConfirm = ActionButton("Add", [&s, newBoardNick, newBoardSlave,
                                                  rebuildBoardNames, showAddBoardPtr, &screen] {
        int slave = 1;
        try { slave = std::stoi(*newBoardSlave); } catch (...) {}
        std::string err = addBoard(s, s.selectedBus, *newBoardNick, slave);
        s.statusMsg = err.empty() ? "" : "Error: " + err;
        if (err.empty()) {
            rebuildBoardNames();
            *showAddBoardPtr = false;
            newBoardNick->clear(); *newBoardSlave = "1";
        }
        screen.PostEvent(Event::Custom);
    });

    auto bStartScan = ActionButton("Start Scan", [&s, scanStart, scanEnd, scanning,
                                                   scanProgress, scanResults, scanResultLabels,
                                                   scanStatus, &screen] {
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
        scanResults->clear();
        scanResultLabels->clear();
        *scanStatus = "Connecting to " + port + "...";
        screen.PostEvent(Event::Custom);

        std::thread([&screen, scanning, scanProgress, scanResults, scanResultLabels,
                     scanStatus, port, baud, start, end] {
            auto scanBusHandle = std::make_shared<PsbSerialBus>();
            if (!scanBusHandle->connect(port, baud, 500)) {
                *scanStatus = "Error: " + scanBusHandle->lastError();
                scanning->store(false);
                screen.PostEvent(Event::Custom);
                return;
            }
            auto results = scanBus(scanBusHandle, start, end, [&](int id) {
                scanProgress->store(id);
                screen.PostEvent(Event::Custom);
            });
            scanBusHandle->disconnect();
            *scanResults = results;
            for (const auto& r : results)
                scanResultLabels->push_back(r.variantName + "  #" + std::to_string(r.slaveId));
            *scanStatus = results.empty()
                ? "No boards found in range."
                : std::to_string(results.size()) + " board(s) found.";
            scanning->store(false);
            screen.PostEvent(Event::Custom);
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

    auto addBoardForm = Container::Vertical({
        boardNickInp, boardSlaveInp, bAddBoardConfirm,
        scanStartInp, scanEndInp, bStartScan, scanResultsMenu, bUseScanResult,
        bAddBoardCancel,
    });
    auto addBoardPopup = Renderer(addBoardForm, [&s, boardNickInp, boardSlaveInp, bAddBoardConfirm,
                                                  scanStartInp, scanEndInp, bStartScan,
                                                  scanResultsMenu, bUseScanResult, bAddBoardCancel,
                                                  scanning, scanProgress, scanStatus, scanResultLabels] {
        std::string busLabel = (s.selectedBus >= 0 && s.selectedBus < static_cast<int>(s.topo.buses.size()))
            ? s.topo.buses[s.selectedBus].name : "(none)";
        Element scanStatusEl = scanning->load()
            ? text("Scanning... #" + std::to_string(scanProgress->load())) | color(Color::Yellow)
            : text(*scanStatus) | dim;
        return vbox({
            text(" Add Board — " + busLabel + " ") | bold | center, separator(),
            hbox({ text("Nickname : "), boardNickInp->Render() }),
            hbox({ text("Slave ID : "), boardSlaveInp->Render() | size(WIDTH, EQUAL, 5) }),
            bAddBoardConfirm->Render() | center,
            separator(),
            text(" Or scan for boards ") | bold | center,
            hbox({ text("Range: "), scanStartInp->Render() | size(WIDTH, EQUAL, 4),
                   text(" - "), scanEndInp->Render() | size(WIDTH, EQUAL, 4),
                   text(" "), bStartScan->Render() }),
            scanStatusEl,
            scanResultLabels->empty() ? text("") : scanResultsMenu->Render() | frame | size(HEIGHT, LESS_THAN, 6),
            scanResultLabels->empty() ? filler() : bUseScanResult->Render() | center,
            separator(),
            bAddBoardCancel->Render() | center,
        }) | border | size(WIDTH, EQUAL, 46);
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
    auto busSelectable = Maybe(bRemoveBus, [&s] { return s.selectedBus >= 0; });

    auto bAddBoard = ActionButton("Add Board", [showAddBoardPtr, scanResultLabels, scanResults, &screen] {
        scanResultLabels->clear(); scanResults->clear();
        *showAddBoardPtr = true; screen.PostEvent(Event::Custom);
    });
    auto addBoardEnabled = Maybe(bAddBoard, [&s] { return s.selectedBus >= 0; });
    auto bRemoveBoard = ActionButton("Remove Board", [&s, rebuildBoardNames, &screen] {
        if (s.selectedBus < 0 || s.selectedBoard < 0) return;
        s.statusMsg = removeBoard(s, s.selectedBus, s.selectedBoard);
        rebuildBoardNames();
        screen.PostEvent(Event::Custom);
    });
    auto boardSelectable = Maybe(bRemoveBoard, [&s] { return s.selectedBus >= 0 && s.selectedBoard >= 0; });

    // ---- Save / Save As / Load / Connect / Cancel ----
    auto topologyPathInp = Input(&s.topologyPath, "topology file path");
    auto bLoadTopology = ActionButton("Load", [&s, rebuildBusNames, rebuildBoardNames, &screen] {
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
    });
    auto bSave = ActionButton("Save", [&s, onFinish, &screen] {
        if (s.topo.save(s.topologyPath)) {
            s.dirty = false;
            s.statusMsg = "Saved to " + s.topologyPath;
        } else {
            s.statusMsg = "Error: could not save to " + s.topologyPath;
        }
        screen.PostEvent(Event::Custom);
    });
    auto bConnectNow = ActionButton("Connect Now", [&s, onFinish] {
        onFinish(WizardOutcome::ConnectNow);
    });
    auto bDone = ActionButton("Save & Exit", [&s, onFinish] {
        bool saved = s.topo.save(s.topologyPath);
        onFinish(saved ? WizardOutcome::SavedOnly : WizardOutcome::Cancelled);
    });
    auto bCancel = ActionButton("Cancel", [onFinish] { onFinish(WizardOutcome::Cancelled); });

    auto mainContainer = Container::Vertical({
        busMenu, bAddBus, busSelectable,
        boardMenu, addBoardEnabled, boardSelectable,
        topologyPathInp, bLoadTopology,
        bSave, bConnectNow, bDone, bCancel,
    });

    auto root = Renderer(mainContainer, [&s, busMenu, bAddBus, busSelectable,
                                         boardMenu, addBoardEnabled, boardSelectable,
                                         topologyPathInp, bLoadTopology,
                                         bSave, bConnectNow, bDone, bCancel] {
        return vbox({
            text(" Setup Wizard " + std::string(s.dirty ? "*" : "") + " ") | bold | center,
            separator(),
            hbox({
                vbox({ text("Buses") | bold, busMenu->Render() | frame | flex,
                       hbox({ bAddBus->Render(), text(" "), busSelectable->Render() }) }) | flex | border,
                vbox({ text("Boards") | bold, boardMenu->Render() | frame | flex,
                       hbox({ addBoardEnabled->Render(), text(" "), boardSelectable->Render() }) }) | flex | border,
            }) | flex,
            separator(),
            hbox({ text("Path: "), topologyPathInp->Render() | flex, text(" "), bLoadTopology->Render() }),
            text(" " + s.statusMsg + " ") | (s.statusMsg.rfind("Error", 0) == 0 ? color(Color::Red) : color(Color::Green)),
            separator(),
            hbox({ bSave->Render(), text("  "), bConnectNow->Render(), text("  "),
                   bDone->Render(), text("  "), bCancel->Render() }) | center,
        });
    }) | Modal(addBusPopup, showAddBusPtr.get())
       | Modal(addBoardPopup, showAddBoardPtr.get());

    return root;
}

} // namespace psb::tui
