#pragma once

#include "topology_config.h"
#include "psb_serial_bus.h"
#include "tui_policy.h"
#include "widgets.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include <memory>
#include <string>
#include <vector>

namespace psb::tui {

using namespace ftxui;

enum class ModeChoice { Single, Multi, Exit };

// Pre-dashboard root offering a choice between a lightweight single-board
// quick-connect and the full multi-board Setup wizard. Shown whenever no
// other signal (an existing topology file, CLI args) has already resolved
// what to do — see main.cpp's topology-resolution loop and
// docs/superpowers/specs/2026-07-21-mode-architecture-design.md's
// Sequencing Note. Runs its own screen.Loop(), matching the same
// standalone pre-dashboard pattern the Setup wizard's own entry point
// (Phase 3, Task 6) already establishes — sequential Loop() calls on one
// ScreenInteractive is an already-proven pattern in this codebase.
inline ModeChoice showModeChoicePopup(ScreenInteractive& screen) {
    auto result = std::make_shared<ModeChoice>(ModeChoice::Exit);

    auto bSingle = ActionButton("Single Board", [result, &screen] {
        *result = ModeChoice::Single;
        screen.ExitLoopClosure()();
    });
    auto bMulti = ActionButton("Multi-Board Topology", [result, &screen] {
        *result = ModeChoice::Multi;
        screen.ExitLoopClosure()();
    });
    auto bExit = ActionButton("Exit", [result, &screen] {
        *result = ModeChoice::Exit;
        screen.ExitLoopClosure()();
    });

    auto container = Container::Horizontal({bSingle, bMulti, bExit});
    // `| center` on the returned Element is the whole fix for this popup
    // rendering pinned to the top-left of the fullscreen root (looking
    // like it "spans the left edge") — an unconstrained bordered box has
    // no reason to move there otherwise.
    auto root = Renderer(container, [bSingle, bMulti, bExit] {
        return vbox({
            text(" How many boards? ") | bold | center,
            separator(),
            text("Choose how you want to start.") | center,
            separator(),
            hbox({ bSingle->Render(), text("  "), bMulti->Render(), text("  "), bExit->Render() }) | center,
        }) | border | size(WIDTH, EQUAL, 60) | center;
    });

    screen.Loop(root);
    return *result;
}

enum class QuickConnectOutcome { Connected, Back, Exit };
struct QuickConnectResult {
    QuickConnectOutcome outcome;
    psb::TopologyConfig topo;  // valid only when outcome == Connected
};

// Quick-connect form for single-board mode: port/baud/slave ID only, no
// topology file involved. Pre-fills from lastSingleConnectPath() — a small
// stored preference, never the real topology file — and saves there again
// on success. Back returns to the mode-selection popup (see main.cpp's
// topology-resolution loop); Exit quits the app entirely.
inline QuickConnectResult showQuickConnectForm(ScreenInteractive& screen) {
    auto portVal = std::make_shared<std::string>();
    auto baudVal = std::make_shared<std::string>("115200");
    auto slaveVal = std::make_shared<std::string>("1");

    const std::string lastPath = psb::TopologyConfig::lastSingleConnectPath();
    if (auto prev = psb::TopologyConfig::load(lastPath);
        prev.has_value() && !prev->buses.empty() && !prev->buses[0].boards.empty()) {
        *portVal = prev->buses[0].port;
        *baudVal = std::to_string(prev->buses[0].baudRate);
        *slaveVal = std::to_string(prev->buses[0].boards[0].slaveId);
    }

    auto portList = std::make_shared<std::vector<std::string>>();
    auto portIdx = std::make_shared<int>(-1);
    auto doScanPorts = [portList, portIdx, portVal, &screen] {
        *portList = PsbSerialBus::scanPorts();
        *portIdx = selectedPortIndex(*portList, *portVal);
        *portVal = *portIdx >= 0 ? (*portList)[*portIdx] : std::string{};
        screen.PostEvent(Event::Custom);
    };
    doScanPorts();

    auto portDropdown = Dropdown(portList.get(), portIdx.get());
    auto visiblePortDropdown = Maybe(portDropdown, [portList] { return !portList->empty(); });
    auto bRescan = Button("Rescan", [doScanPorts] { doScanPorts(); });
    auto baudInp = Input(baudVal.get(), "baud");
    auto slaveInp = Input(slaveVal.get(), "1-247");

    auto outcome = std::make_shared<QuickConnectOutcome>(QuickConnectOutcome::Exit);

    auto bConnect = ActionButton("Connect", [&, portList, portIdx, portVal, outcome] {
        // Dropdown() only ever mutates portIdx as the user navigates it —
        // reading portList[*portIdx] here (the live selection) rather than
        // trusting a separately-tracked *portVal is required, or Connect
        // silently uses whatever port was selected at scan time regardless
        // of what the user picked afterward — the exact bug already found
        // and fixed in wizard_screen.h's Add Bus modal.
        std::string port = (*portIdx >= 0 && *portIdx < static_cast<int>(portList->size()))
            ? (*portList)[*portIdx] : *portVal;
        if (port.empty()) return;
        *portVal = port;
        *outcome = QuickConnectOutcome::Connected;
        screen.ExitLoopClosure()();
    });
    auto bBack = ActionButton("Back", [outcome, &screen] {
        *outcome = QuickConnectOutcome::Back;
        screen.ExitLoopClosure()();
    });
    auto bExit = ActionButton("Exit", [outcome, &screen] {
        *outcome = QuickConnectOutcome::Exit;
        screen.ExitLoopClosure()();
    });

    auto container = Container::Vertical({visiblePortDropdown, bRescan, baudInp, slaveInp, bConnect, bBack, bExit});
    auto root = Renderer(container, [portList, visiblePortDropdown, bRescan, baudInp, slaveInp, bConnect, bBack, bExit] {
        Element portChoice = portList->empty()
            ? text("(no ports found — Rescan)") | dim | flex
            : visiblePortDropdown->Render() | flex;
        return vbox({
            text(" Quick Connect ") | bold | center,
            separator(),
            hbox({ text("Port  : "), portChoice, text(" "), bRescan->Render() }),
            hbox({ text("Baud  : "), baudInp->Render()  | size(WIDTH, EQUAL, 8) }),
            hbox({ text("Slave : "), slaveInp->Render() | size(WIDTH, EQUAL, 5) }),
            separator(),
            hbox({ bConnect->Render(), text("  "), bBack->Render(), text("  "), bExit->Render() }) | center,
        }) | border | size(WIDTH, EQUAL, 44) | center;
    });

    screen.Loop(root);
    if (*outcome != QuickConnectOutcome::Connected) return QuickConnectResult{*outcome, {}};

    int baud = 115200, slave = 1;
    try { baud = std::stoi(*baudVal); } catch (...) {}
    try { slave = std::stoi(*slaveVal); } catch (...) {}
    auto cfg = psb::TopologyConfig::singleBoard(*portVal, baud, slave, "board1");
    cfg.save(lastPath);  // best-effort — a failed preference save isn't fatal
    return QuickConnectResult{QuickConnectOutcome::Connected, cfg};
}

} // namespace psb::tui
