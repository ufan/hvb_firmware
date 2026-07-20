# Multi-Board Topology — Phase 3 (TUI Interactive Setup Wizard) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give `psb_demo_tui` an interactive screen for building and editing a multi-bus/multi-board `TopologyConfig` — add/remove buses and boards (manually or via a slave-ID scan sweep), save/save-as/load, and either exit or jump straight into the live dashboard — reachable both before any board connects (`--setup`, or automatically when `--topology` names a file that doesn't exist yet) and from inside an already-running session (to add a board without restarting).

**Architecture:** A new `WizardState` (plain data: an in-progress `TopologyConfig` plus UI selection/status scratch) is edited by pure, FTXUI-independent mutator functions (`addBus`/`removeBus`/`addBoard`/`removeBoard`, all unit-tested directly, no hardware, no rendering) — mirroring how `tui_policy.h` already separates policy from rendering. `wizard_scan.h` adds the one new hardware-touching primitive: sweeping a slave-ID range on an already-open bus using the same `verifyProtocol()` probe every board's connect already runs, just with a short timeout override (new, small, additive `PsbBoardSession` change). `wizard_screen.h` is the FTXUI `Component` — list + Add Bus/Add Board/Edit/Remove/Save/Load/Connect-Now — built once and reused in both contexts: as `main()`'s top-level root for the pre-dashboard entry points, and as a `Modal` overlay for the in-session entry point. Making the second context work live (add a board without restarting) requires `board_switcher.h`'s component tree to be permanently hot-attach-capable rather than special-cased at construction time — Phase 2's "collapse to bare dashboard when there's only one board" becomes a rendering-time conditional instead of a structural one, so `Container::Tab`/`Menu`'s existing dynamic-children support (`ComponentBase::Add`) can append a new board's dashboard live.

**Tech Stack:** C++17, FTXUI (vendored), CLI11 (vendored), toml++ (vendored, via `TopologyConfig`), the Phase 1/2 `PsbSerialBus`/`PsbBoardSession`/`TopologyConfig`/`BoardSession`/`BusWorker`/`board_switcher.h` (already on `main`).

## Global Constraints

- With exactly one board, live rendering must stay pixel-identical to today's (Phase 2's constraint, still binding) — the switcher bar only appears once a second board exists, whether that happens at startup or live via the wizard.
- The wizard never touches hardware except during an explicit scan (bus already opened by the user's Add Bus step) — adding/editing/removing a bus or board in the list is a pure in-memory edit until Save or Connect Now/Apply is clicked.
- Mid-session hot-attach (Task 7) is strictly additive: it can add a new bus or a new board to an existing bus, live, without restarting. It never edits or removes an already-running board/bus — that still requires Disconnect + edit the topology file + restart, unchanged from today. This matches the spec's stated scope ("add a board without restarting"), not a general live-reconfiguration feature.
- `PsbSerialBus`/`PsbBoardSession` are not internally thread-safe (Phase 1) — exactly one thread drives a given bus. A scan run from the pre-dashboard wizard has no `BusWorker` yet, so it runs on a dedicated scan thread that owns the bus exclusively for the scan's duration; a scan run from the in-session wizard (future extension, not built in this phase — see Task 5) would have to route through that bus's existing `BusWorker` instead. This phase's scan UI is only wired to the pre-dashboard entry point (Task 5); the in-session entry point (Task 7) covers manual Add Board only, not Scan — see Task 7's scope note.
- Preserve every comment explaining a non-obvious historical bug fix when moving/adapting code (torn-table publishing, uptime decoupling, offline-threshold reasoning, the Phase 2 board-switcher use-after-free lesson, etc.).
- Build via `cd tools && cmake --build build --target psb_modbus_core psb_tests psb_demo_cli psb_demo_tui`. Test via `./build/psb_modbus_core/tests/psb_tests`. Manual verification via `tmux` per `docs/guide/client-architecture-and-pitfalls.md` §3's methodology.

---

## Task 1: Core — `PsbBoardSession::verifyProtocol()` gains a timeout override

**Files:**
- Modify: `tools/psb_modbus_core/psb_board_session.h:31` (declaration)
- Modify: `tools/psb_modbus_core/psb_board_session.cpp:31` (definition)
- Test: `tools/psb_modbus_core/tests/test_board_session.cpp`

**Interfaces:**
- Consumes: `PsbBoardSession::readRegsInternal(bool, uint16_t, uint16_t, uint16_t*, int timeoutOverrideMs = -1)` (already exists, already threads a timeout override through to the bus).
- Produces: `bool PsbBoardSession::verifyProtocol(int timeoutOverrideMs = -1)` — default argument preserves every existing call site (`board_dashboard.h`, `main.cpp`, every current test) unchanged; Task 3's scan driver is the only caller that passes a real value.

- [ ] **Step 1: Write the failing test**

Add to `tools/psb_modbus_core/tests/test_board_session.cpp`:

```cpp
TEST_CASE("PsbBoardSession — verifyProtocol accepts a timeout override without changing behavior", "[board_session]") {
    auto bus = std::make_shared<psb::PsbSerialBus>();
    uint16_t inputRegs[8] = {}, holdingRegs[8] = {};
    inputRegs[0] = VC_PROTOCOL_MAJOR;
    inputRegs[1] = VC_PROTOCOL_MINOR;
    bus->attachTestArrays(1, inputRegs, holdingRegs, 8);

    psb::PsbBoardSession session(bus, 1);
    // Test-mode arrays respond instantly regardless of the timeout value —
    // this only proves the overload compiles and still succeeds, matching
    // the existing no-argument call's behavior exactly.
    REQUIRE(session.verifyProtocol(50));
    CHECK(session.isConnected());
}
```

- [ ] **Step 2: Run test to verify it fails to compile**

Run: `cd tools && cmake --build build --target psb_tests 2>&1 | tail -20`
Expected: FAIL — `no matching function for call to 'PsbBoardSession::verifyProtocol(int)'`.

- [ ] **Step 3: Add the timeout override parameter**

Edit `tools/psb_modbus_core/psb_board_session.h`, change:

```cpp
    bool verifyProtocol();
```

to:

```cpp
    // timeoutOverrideMs: -1 uses the bus's normal timeout (today's
    // behavior, unchanged for every existing caller). A short override is
    // for the setup wizard's slave-ID scan (wizard_scan.h) — sweeping up to
    // ~32 candidate IDs at the bus's normal multi-second timeout would make
    // a scan take minutes; a non-responding ID is the overwhelmingly common
    // case during a sweep and must fail fast.
    bool verifyProtocol(int timeoutOverrideMs = -1);
```

Edit `tools/psb_modbus_core/psb_board_session.cpp`, change:

```cpp
bool PsbBoardSession::verifyProtocol() {
    uint16_t probe[2] = {0xFFFF, 0xFFFF};
    if (!readRegsInternal(false, reg::sysAddr(0), 2, probe)) {
```

to:

```cpp
bool PsbBoardSession::verifyProtocol(int timeoutOverrideMs) {
    uint16_t probe[2] = {0xFFFF, 0xFFFF};
    if (!readRegsInternal(false, reg::sysAddr(0), 2, probe, timeoutOverrideMs)) {
```

(The rest of the function body — protocol version validation, `m_impl->verified` — is unchanged.)

- [ ] **Step 4: Run test to verify it passes**

Run: `cd tools && cmake --build build --target psb_tests && ./build/psb_modbus_core/tests/psb_tests "[board_session]"`
Expected: PASS, all `[board_session]`-tagged tests including the new one.

- [ ] **Step 5: Commit**

```bash
git add tools/psb_modbus_core/psb_board_session.h tools/psb_modbus_core/psb_board_session.cpp tools/psb_modbus_core/tests/test_board_session.cpp
git commit -m "feat(psb_modbus_core): add timeout override to PsbBoardSession::verifyProtocol()"
```

---

## Task 2: `wizard_state.h` — pure topology-editing logic (TDD)

**Files:**
- Create: `tools/psb_demo_app/tui/wizard_state.h`
- Test: `tools/psb_modbus_core/tests/test_wizard_state.cpp` (new file; header-only code under test, no core changes)
- Modify: `tools/psb_modbus_core/tests/CMakeLists.txt` (register the new test file)

**Interfaces:**
- Consumes: `psb::TopologyConfig`, `psb::BusConfig`, `psb::BoardConfig` (Phase 1, `topology_config.h`).
- Produces: `psb::tui::WizardState`, `psb::tui::addBus`, `psb::tui::removeBus`, `psb::tui::addBoard`, `psb::tui::removeBoard`, `psb::tui::nicknameInUse`, `psb::tui::slaveIdInUse` — every mutator returns `std::string` (empty = success, non-empty = user-facing error message), never throws, never touches hardware. Task 5 wires these directly into button handlers.

- [ ] **Step 1: Check the test file into the build**

Edit `tools/psb_modbus_core/tests/CMakeLists.txt` — find the existing `add_executable(psb_tests ...)` (or equivalent `target_sources`) call listing files like `test_topology_config.cpp`, and add `test_wizard_state.cpp` alongside it. (Read the file first to match its exact list style before editing — do not guess the syntax.)

- [ ] **Step 2: Write the failing tests**

Create `tools/psb_modbus_core/tests/test_wizard_state.cpp`:

```cpp
#include "wizard_state.h"

#include <catch2/catch_test_macros.hpp>

using namespace psb::tui;

TEST_CASE("WizardState — addBus rejects an empty port", "[wizard_state]") {
    WizardState s;
    CHECK(addBus(s, "bus1", "", 115200) == "port required");
    CHECK(s.topo.buses.empty());
}

TEST_CASE("WizardState — addBus rejects a port already in use by another bus", "[wizard_state]") {
    WizardState s;
    REQUIRE(addBus(s, "bus1", "/dev/ttyUSB0", 115200).empty());
    CHECK(addBus(s, "bus2", "/dev/ttyUSB0", 9600) == "port already in use by bus \"bus1\"");
    CHECK(s.topo.buses.size() == 1);
}

TEST_CASE("WizardState — addBus defaults an empty name to busN", "[wizard_state]") {
    WizardState s;
    REQUIRE(addBus(s, "", "/dev/ttyUSB0", 115200).empty());
    CHECK(s.topo.buses[0].name == "bus1");
}

TEST_CASE("WizardState — removeBus rejects an out-of-range index", "[wizard_state]") {
    WizardState s;
    CHECK(removeBus(s, 0) == "invalid bus index");
}

TEST_CASE("WizardState — removeBus drops the bus and clears selection past the end", "[wizard_state]") {
    WizardState s;
    addBus(s, "bus1", "/dev/ttyUSB0", 115200);
    s.selectedBus = 0;
    REQUIRE(removeBus(s, 0).empty());
    CHECK(s.topo.buses.empty());
    CHECK(s.selectedBus == -1);
}

TEST_CASE("WizardState — addBoard rejects an empty nickname", "[wizard_state]") {
    WizardState s;
    addBus(s, "bus1", "/dev/ttyUSB0", 115200);
    CHECK(addBoard(s, 0, "", 1) == "nickname required");
}

TEST_CASE("WizardState — addBoard rejects an out-of-range slave ID", "[wizard_state]") {
    WizardState s;
    addBus(s, "bus1", "/dev/ttyUSB0", 115200);
    CHECK(addBoard(s, 0, "board1", 300) == "slave ID must be 0-247");
}

TEST_CASE("WizardState — addBoard rejects a duplicate nickname across the whole topology", "[wizard_state]") {
    WizardState s;
    addBus(s, "bus1", "/dev/ttyUSB0", 115200);
    addBus(s, "bus2", "/dev/ttyUSB1", 115200);
    REQUIRE(addBoard(s, 0, "hvb-bench", 1).empty());
    CHECK(addBoard(s, 1, "hvb-bench", 1) == "nickname \"hvb-bench\" already in use");
}

TEST_CASE("WizardState — addBoard rejects a duplicate slave ID on the same bus", "[wizard_state]") {
    WizardState s;
    addBus(s, "bus1", "/dev/ttyUSB0", 115200);
    REQUIRE(addBoard(s, 0, "board1", 1).empty());
    CHECK(addBoard(s, 0, "board2", 1) == "slave ID 1 already used on this bus");
}

TEST_CASE("WizardState — addBoard allows the same slave ID on different buses", "[wizard_state]") {
    WizardState s;
    addBus(s, "bus1", "/dev/ttyUSB0", 115200);
    addBus(s, "bus2", "/dev/ttyUSB1", 115200);
    REQUIRE(addBoard(s, 0, "board1", 1).empty());
    CHECK(addBoard(s, 1, "board2", 1).empty());
}

TEST_CASE("WizardState — removeBoard drops the board and clears selection past the end", "[wizard_state]") {
    WizardState s;
    addBus(s, "bus1", "/dev/ttyUSB0", 115200);
    addBoard(s, 0, "board1", 1);
    s.selectedBus = 0; s.selectedBoard = 0;
    REQUIRE(removeBoard(s, 0, 0).empty());
    CHECK(s.topo.buses[0].boards.empty());
    CHECK(s.selectedBoard == -1);
}

TEST_CASE("WizardState — successful mutations mark the state dirty", "[wizard_state]") {
    WizardState s;
    CHECK_FALSE(s.dirty);
    addBus(s, "bus1", "/dev/ttyUSB0", 115200);
    CHECK(s.dirty);
}
```

- [ ] **Step 3: Run tests to verify they fail**

Run: `cd tools && cmake --build build --target psb_tests 2>&1 | tail -20`
Expected: FAIL — `wizard_state.h: No such file or directory`.

- [ ] **Step 4: Write `wizard_state.h`**

Create `tools/psb_demo_app/tui/wizard_state.h`:

```cpp
#pragma once

#include "topology_config.h"

#include <string>

namespace psb::tui {

// In-progress topology edit, plus UI selection/status scratch — everything
// the wizard screen (wizard_screen.h) needs that isn't itself an FTXUI
// widget. Deliberately hardware- and FTXUI-free so the mutators below are
// unit-testable without a bus, a board, or a terminal (mirrors
// tui_policy.h's split between policy and rendering).
struct WizardState {
    psb::TopologyConfig topo;   // in-progress; may be unsaved
    std::string topologyPath;   // Save target
    int selectedBus = -1;       // index into topo.buses, -1 = none
    int selectedBoard = -1;     // index into topo.buses[selectedBus].boards, -1 = none
    std::string statusMsg;
    bool dirty = false;         // true after any mutation since the last successful save
};

inline bool nicknameInUse(const psb::TopologyConfig& topo, const std::string& nickname) {
    for (const auto& bus : topo.buses)
        for (const auto& board : bus.boards)
            if (board.nickname == nickname) return true;
    return false;
}

inline bool slaveIdInUse(const psb::BusConfig& bus, int slaveId) {
    for (const auto& board : bus.boards)
        if (board.slaveId == slaveId) return true;
    return false;
}

// Every mutator below: returns "" on success, a user-facing error message on
// failure; never throws; never touches hardware — Save/Connect Now/Apply
// (wizard_screen.h) decide separately when (if ever) to open a physical
// connection.

inline std::string addBus(WizardState& s, const std::string& name,
                          const std::string& port, int baud) {
    if (port.empty()) return "port required";
    for (const auto& b : s.topo.buses)
        if (b.port == port) return "port already in use by bus \"" + b.name + "\"";
    psb::BusConfig bus;
    bus.name = name.empty() ? ("bus" + std::to_string(s.topo.buses.size() + 1)) : name;
    bus.port = port;
    bus.baudRate = baud;
    s.topo.buses.push_back(std::move(bus));
    s.dirty = true;
    return "";
}

inline std::string removeBus(WizardState& s, int busIdx) {
    if (busIdx < 0 || busIdx >= static_cast<int>(s.topo.buses.size()))
        return "invalid bus index";
    s.topo.buses.erase(s.topo.buses.begin() + busIdx);
    if (s.selectedBus >= static_cast<int>(s.topo.buses.size()))
        s.selectedBus = static_cast<int>(s.topo.buses.size()) - 1;
    s.selectedBoard = -1;
    s.dirty = true;
    return "";
}

inline std::string addBoard(WizardState& s, int busIdx, const std::string& nickname, int slaveId) {
    if (busIdx < 0 || busIdx >= static_cast<int>(s.topo.buses.size()))
        return "invalid bus index";
    if (nickname.empty()) return "nickname required";
    if (slaveId < 0 || slaveId > 247) return "slave ID must be 0-247";
    if (nicknameInUse(s.topo, nickname)) return "nickname \"" + nickname + "\" already in use";
    if (slaveIdInUse(s.topo.buses[busIdx], slaveId))
        return "slave ID " + std::to_string(slaveId) + " already used on this bus";
    psb::BoardConfig board;
    board.nickname = nickname;
    board.slaveId = slaveId;
    s.topo.buses[busIdx].boards.push_back(std::move(board));
    s.dirty = true;
    return "";
}

inline std::string removeBoard(WizardState& s, int busIdx, int boardIdx) {
    if (busIdx < 0 || busIdx >= static_cast<int>(s.topo.buses.size()))
        return "invalid bus index";
    auto& boards = s.topo.buses[busIdx].boards;
    if (boardIdx < 0 || boardIdx >= static_cast<int>(boards.size()))
        return "invalid board index";
    boards.erase(boards.begin() + boardIdx);
    if (s.selectedBoard >= static_cast<int>(boards.size()))
        s.selectedBoard = static_cast<int>(boards.size()) - 1;
    s.dirty = true;
    return "";
}

} // namespace psb::tui
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `cd tools && cmake --build build --target psb_tests && ./build/psb_modbus_core/tests/psb_tests "[wizard_state]"`
Expected: PASS, all 11 `[wizard_state]` test cases.

- [ ] **Step 6: Commit**

```bash
git add tools/psb_demo_app/tui/wizard_state.h tools/psb_modbus_core/tests/test_wizard_state.cpp tools/psb_modbus_core/tests/CMakeLists.txt
git commit -m "feat(psb_demo_tui): add wizard_state.h — pure topology-editing logic, unit-tested"
```

---

## Task 3: `wizard_scan.h` — slave-ID scan-assisted discovery

**Files:**
- Create: `tools/psb_demo_app/tui/wizard_scan.h`
- Test: `tools/psb_modbus_core/tests/test_wizard_scan.cpp` (new file)
- Modify: `tools/psb_modbus_core/tests/CMakeLists.txt` (register the new test file)

**Interfaces:**
- Consumes: `psb::PsbSerialBus` (must already be `connect()`ed by the caller — this file never opens/closes a port), `psb::PsbBoardSession::verifyProtocol(int)` (Task 1), `psb::PsbBoardSession::readSystemInfo()`, `psb::catalog::variantName(int)` (`board_catalog.h`, already exists).
- Produces: `psb::tui::DiscoveredBoard{slaveId, variantName, fwVersion}`, `psb::tui::scanBus(shared_ptr<PsbSerialBus>, int startId, int endId, function<void(int)> onProgress = {}) -> vector<DiscoveredBoard>`. Task 5's scan UI runs this on a dedicated `std::thread` (the bus has no `BusWorker` yet at this point in the flow — see Global Constraints) and marshals results back via `screen.PostEvent(Event::Custom)`, the same pattern `doFullScan`/`doPollScan` already use.

- [ ] **Step 1: Check the test file into the build**

Same mechanism as Task 2 Step 1 — add `test_wizard_scan.cpp` to `tools/psb_modbus_core/tests/CMakeLists.txt`.

- [ ] **Step 2: Write the failing test**

Create `tools/psb_modbus_core/tests/test_wizard_scan.cpp`:

```cpp
#include "wizard_scan.h"
#include "register_map.h"

#include <catch2/catch_test_macros.hpp>

using namespace psb::tui;

TEST_CASE("scanBus — finds only the slave IDs that respond, in range order", "[wizard_scan]") {
    auto bus = std::make_shared<psb::PsbSerialBus>();

    uint16_t inputA[8] = {}, holdingA[8] = {};
    inputA[SYS_PROTOCOL_MAJOR] = VC_PROTOCOL_MAJOR;
    inputA[SYS_PROTOCOL_MINOR] = VC_PROTOCOL_MINOR;
    inputA[SYS_VARIANT_ID] = 1;  // jw_hvb
    bus->attachTestArrays(3, inputA, holdingA, 8);

    uint16_t inputB[8] = {}, holdingB[8] = {};
    inputB[SYS_PROTOCOL_MAJOR] = VC_PROTOCOL_MAJOR;
    inputB[SYS_PROTOCOL_MINOR] = VC_PROTOCOL_MINOR;
    inputB[SYS_VARIANT_ID] = 2;  // jw_lvb
    bus->attachTestArrays(7, inputB, holdingB, 8);
    // No test arrays attached for any other ID in [1, 10] — every other
    // candidate's verifyProtocol() fails, matching "nothing at this address"
    // on real hardware.

    auto found = scanBus(bus, 1, 10);

    REQUIRE(found.size() == 2);
    CHECK(found[0].slaveId == 3);
    CHECK(found[0].variantName == "jw_hvb");
    CHECK(found[1].slaveId == 7);
    CHECK(found[1].variantName == "jw_lvb");
}

TEST_CASE("scanBus — reports progress for every candidate in range, including non-responders", "[wizard_scan]") {
    auto bus = std::make_shared<psb::PsbSerialBus>();
    uint16_t input[8] = {}, holding[8] = {};
    input[SYS_PROTOCOL_MAJOR] = VC_PROTOCOL_MAJOR;
    input[SYS_PROTOCOL_MINOR] = VC_PROTOCOL_MINOR;
    bus->attachTestArrays(2, input, holding, 8);

    std::vector<int> probed;
    scanBus(bus, 1, 4, [&](int id) { probed.push_back(id); });

    CHECK(probed == std::vector<int>{1, 2, 3, 4});
}

TEST_CASE("scanBus — empty range with nothing attached finds nothing", "[wizard_scan]") {
    auto bus = std::make_shared<psb::PsbSerialBus>();
    CHECK(scanBus(bus, 1, 5).empty());
}
```

- [ ] **Step 3: Run tests to verify they fail**

Run: `cd tools && cmake --build build --target psb_tests 2>&1 | tail -20`
Expected: FAIL — `wizard_scan.h: No such file or directory`.

- [ ] **Step 4: Write `wizard_scan.h`**

Create `tools/psb_demo_app/tui/wizard_scan.h`:

```cpp
#pragma once

#include "psb_serial_bus.h"
#include "psb_board_session.h"
#include "board_catalog.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace psb::tui {

struct DiscoveredBoard {
    int slaveId;
    std::string variantName;
    uint32_t fwVersion;
};

// Short per-candidate timeout for a scan sweep — a non-responding ID is the
// overwhelmingly common case (a 1-32 sweep against a 2-board bus probes 30
// dead addresses), and each one must fail fast or a full-range sweep takes
// minutes instead of seconds. Confirmed via Task 1's PsbBoardSession::
// verifyProtocol(int) override.
inline constexpr int kScanProbeTimeoutMs = 200;

// Sweeps [startId, endId] (inclusive) on a bus the caller has already
// connect()ed — this function never opens or closes a port. Each candidate
// gets exactly the probe every board's real connect already runs
// (verifyProtocol), just with a short timeout; a candidate that answers gets
// one follow-up readSystemInfo() (default timeout — this call only ever
// targets an ID that just proved it's alive, so it's expected to succeed
// promptly) to report its variant for the picker. `onProgress(candidateId)`
// fires before every probe (responder or not) so a caller can drive a
// progress indicator; default no-op.
//
// Blocking — runs entirely on the calling thread. Callers driving this from
// an interactive UI must run it on a dedicated thread and marshal results
// back to the UI thread themselves (see wizard_screen.h), the same pattern
// board_session.h's doFullScan/doPollScan already establish for routine
// polling.
inline std::vector<DiscoveredBoard> scanBus(std::shared_ptr<PsbSerialBus> bus,
                                             int startId, int endId,
                                             const std::function<void(int)>& onProgress = {}) {
    std::vector<DiscoveredBoard> found;
    for (int id = startId; id <= endId; ++id) {
        if (onProgress) onProgress(id);
        PsbBoardSession probe(bus, id);
        if (!probe.verifyProtocol(kScanProbeTimeoutMs)) continue;
        SystemInfo info = probe.readSystemInfo();
        found.push_back({id, catalog::variantName(info.variantId), info.fwVersion});
    }
    return found;
}

} // namespace psb::tui
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `cd tools && cmake --build build --target psb_tests && ./build/psb_modbus_core/tests/psb_tests "[wizard_scan]"`
Expected: PASS, all 3 `[wizard_scan]` test cases.

- [ ] **Step 6: Commit**

```bash
git add tools/psb_demo_app/tui/wizard_scan.h tools/psb_modbus_core/tests/test_wizard_scan.cpp tools/psb_modbus_core/tests/CMakeLists.txt
git commit -m "feat(psb_demo_tui): add wizard_scan.h — slave-ID scan-assisted discovery"
```

---

## Task 4: `board_switcher.h` — always-dynamic, hot-attach-capable

**Files:**
- Modify: `tools/psb_demo_app/tui/board_switcher.h` (rewritten)
- Modify: `tools/psb_demo_app/tui/main.cpp` (updated call site)

**Interfaces:**
- Consumes: `psb::tui::BoardSession` (Phase 2, `board_session.h`), FTXUI `ComponentBase::Add` (confirmed present in the vendored FTXUI's `component_base.hpp`).
- Produces: `psb::tui::BoardSwitcher{Component root; function<void(const string& nickname, Component dashboard)> attachBoard;}` — replaces the old free function `makeBoardSwitcher`. `root` is what `main()` passes to `screen.Loop`/embeds in a `Modal`; `attachBoard` is the hook Task 7 calls to hot-add a board's already-built dashboard live. Calling `attachBoard` when only one board existed before is what makes the switcher bar appear for the first time — no rebuild, no screen swap.

**Why this changes:** Phase 2's `makeBoardSwitcher` special-cased exactly one board by returning `boards.front()->dashboard` directly, skipping the whole `Menu`/`Container::Tab` machinery — clean for a fixed board count, but structurally incompatible with adding a second board later without tearing down and rebuilding the entire root component (which `ScreenInteractive::Loop` doesn't support mid-loop). Moving the single-board special case from *construction* to *rendering* (the switcher bar element is simply omitted from the `vbox` when there's only one name) keeps single-board output pixel-identical while making the underlying `Container::Tab`/`Menu` always present and always appendable.

- [ ] **Step 1: Rewrite `board_switcher.h`**

Replace the full contents of `tools/psb_demo_app/tui/board_switcher.h`:

```cpp
#pragma once

#include "board_session.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace psb::tui {

using namespace ftxui;

// The switcher's root Component (always Menu + Container::Tab, never just a
// bare dashboard) plus a live-append hook. Building it this way — instead of
// Phase 2's "collapse to a bare dashboard when there's only one board" —
// means a board can be attached after construction (Task 7's mid-session
// wizard) without rebuilding or swapping the root: Container::Tab and Menu
// both already support dynamic children (ComponentBase::Add; Menu re-reads
// its backing vector's live size every render), so appending is enough.
// Pixel-identical single-board rendering (Phase 2's Global Constraint) is
// preserved by omitting the switcher bar *element* — not the underlying
// component — whenever fewer than two boards exist.
struct BoardSwitcher {
    Component root;
    std::function<void(const std::string& nickname, Component dashboard)> attachBoard;
};

inline BoardSwitcher makeBoardSwitcher(std::vector<std::unique_ptr<BoardSession>>& boards) {
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

    auto dashboardStack = Container::Tab({}, activeBoard.get());
    for (auto& b : boards) dashboardStack->Add(b->dashboard);

    auto mainContainer = Container::Vertical({switcherBar, dashboardStack});
    // Capture boardNames/activeBoard, not just switcherBar/dashboardStack —
    // Menu()/Container::Tab() only hold raw pointers into them (FTXUI's
    // non-owning-pointer widget convention); losing the owning shared_ptrs
    // here was a real use-after-free, found live via gdb during Phase 2
    // (std::length_error inside MenuBase::Clamp() reading a corrupted
    // vector size from freed memory) — see that fix's commit for the full
    // diagnosis. The lesson generalizes: anything Menu/Tab is given a raw
    // pointer into must outlive this returned Component, which this closure
    // is what accomplishes.
    auto root = Renderer(mainContainer, [switcherBar, dashboardStack, boardNames, activeBoard] {
        bool showBar = boardNames->size() > 1;
        Elements top;
        if (showBar) {
            top.push_back(text(" Boards ") | bold | dim);
            top.push_back(switcherBar->Render());
            top.push_back(separator());
        }
        top.push_back(dashboardStack->Render() | flex);
        return vbox(std::move(top));
    });

    auto attachBoard = [boardNames, dashboardStack](const std::string& nickname, Component dashboard) {
        boardNames->push_back(nickname);
        dashboardStack->Add(std::move(dashboard));
    };

    return BoardSwitcher{root, attachBoard};
}

} // namespace psb::tui
```

- [ ] **Step 2: Update `main.cpp`'s call site**

Edit `tools/psb_demo_app/tui/main.cpp`, change:

```cpp
    // ---- Board switcher + active dashboard ----
    auto root = psb::tui::makeBoardSwitcher(boards, screen);

    screen.Loop(root);
```

to:

```cpp
    // ---- Board switcher + active dashboard ----
    auto switcher = psb::tui::makeBoardSwitcher(boards);

    screen.Loop(switcher.root);
```

- [ ] **Step 3: Rebuild and confirm single-board pixel-identical behavior**

Run: `cd tools && cmake --build build --target psb_demo_tui psb_tests 2>&1 | tail -30 && ./build/psb_modbus_core/tests/psb_tests`
Expected: clean build, all tests pass (this task touches no core files, so the count is unchanged from Task 3's).

Launch against a single-board topology exactly as Phase 2's Task 5 did (`tmux new-session -d ... "./bin/psb_demo_tui --topology <one-board-file>"`, `tmux capture-pane -p`) and confirm the menu bar/tab bar/layout are pixel-identical to before this task — no " Boards " strip visible. This is the regression backstop for the rendering-time-vs-construction-time change.

Then repeat against a two-board topology (reuse a topology file matching Phase 2's Task 5 verification) and confirm the switcher bar still appears and both boards still render and poll correctly — this task is a refactor of Phase 2's already-verified behavior, not new behavior, so this is a regression check, not new coverage.

- [ ] **Step 4: Commit**

```bash
git add tools/psb_demo_app/tui/board_switcher.h tools/psb_demo_app/tui/main.cpp
git commit -m "refactor(psb_demo_tui): make board switcher always-dynamic and hot-attach-capable

Moves Phase 2's single-board special case from construction time
(returning the bare dashboard directly) to render time (omitting the
switcher bar element), so a board can be attached live after
construction — needed by Phase 3's mid-session setup wizard (Task 7).
No behavior change for existing single- or multi-board startup."
```

---

## Task 5: `wizard_screen.h` — the wizard UI (pre-dashboard entry point)

**Files:**
- Create: `tools/psb_demo_app/tui/wizard_screen.h`

**Interfaces:**
- Consumes: `psb::tui::WizardState` + its mutators (Task 2), `psb::tui::scanBus`/`DiscoveredBoard` (Task 3), `psb::PsbSerialBus::scanPorts()`/`availableBaudRates()` (Phase 1), `psb::TopologyConfig::load()`/`save()` (Phase 1, `topology_config.h` — already returns `std::optional<TopologyConfig>`/`bool` respectively), `CommitInput`/`ActionButton` (`widgets.h`, already exist), `selectedPortIndex` (`tui_policy.h`, already exists).
- Produces: `enum class WizardOutcome { Cancelled, SavedOnly, ConnectNow }`, `psb::tui::makeWizardScreen(WizardState&, ScreenInteractive&, std::function<void(WizardOutcome)> onFinish) -> Component`. Task 6 uses this as `main()`'s standalone pre-dashboard root; Task 7 reuses the identical function as a `Modal` overlay — the function itself doesn't know or care which.

- [ ] **Step 1: Write `wizard_screen.h`**

Create `tools/psb_demo_app/tui/wizard_screen.h`:

```cpp
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
```

- [ ] **Step 2: Build to confirm it compiles**

Run: `cd tools && cmake --build build --target psb_demo_tui 2>&1 | tail -60`
Expected: FAIL — `main.cpp` doesn't call `makeWizardScreen` yet, so `wizard_screen.h` itself isn't even included/compiled by any translation unit. This step is a **compile-only sanity check**, not a functional one: temporarily add `#include "wizard_screen.h"` to the top of `main.cpp` (no call site yet), rebuild, confirm it compiles clean, then remove that temporary include — Task 6 adds the real include and call site.

Expected after the temporary include: clean build (only warnings about unused `makeWizardScreen`, if any — none, since it's a template-free `inline` function only instantiated when called, so an uncalled `inline Component makeWizardScreen(...)` produces no warning at all; this step mainly catches syntax/type errors).

- [ ] **Step 3: Commit**

```bash
git add tools/psb_demo_app/tui/wizard_screen.h
git commit -m "feat(psb_demo_tui): add wizard_screen.h — setup wizard UI (not yet wired into main)"
```

---

## Task 6: Wire the wizard into `main.cpp` — `--setup`, auto-launch, and the pre-dashboard flow

**Files:**
- Modify: `tools/psb_demo_app/tui/main.cpp` (rewritten)

**Interfaces:**
- Consumes: `psb::tui::makeWizardScreen`/`WizardOutcome` (Task 5), `psb::tui::BoardSwitcher`/`makeBoardSwitcher` (Task 4), everything Phase 2's `main.cpp` already used.
- Produces: a `buildRuntime(topo, screen, running, timeoutMs, autoConnectAll) -> RuntimeHandle` extraction (used by both the direct-startup path and the post-wizard path), plus the `--setup` CLI flag and the design spec's case-3 auto-launch (`--topology <path>` given but the file doesn't exist).

- [ ] **Step 1: Extract `buildRuntime()`**

This lets both "start normally" and "start after finishing the wizard" build the exact same runtime structures without duplicating Phase 2's per-bus-thread setup code.

Edit `tools/psb_demo_app/tui/main.cpp`. Replace the whole file:

```cpp
#include "psb_serial_bus.h"
#include "psb_board_session.h"
#include "topology_config.h"
#include "board_session.h"
#include "board_dashboard.h"
#include "board_switcher.h"
#include "wizard_state.h"
#include "wizard_screen.h"
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

// Everything the running program needs to join/tear down cleanly — built
// once by buildRuntime(), used identically whether that happened before the
// wizard even ran (the common case) or right after it finished (Task 5's
// Connect Now).
struct Runtime {
    std::vector<std::unique_ptr<psb::tui::BusWorker>> busWorkers;
    std::vector<std::unique_ptr<psb::tui::BoardSession>> boards;
    psb::tui::BoardSwitcher switcher;
    std::thread animThread;
};

// Builds BusWorker/BoardSession for every bus/board in `topo`, starts one
// worker thread per bus (Phase 2), starts the shared animation thread, and
// returns everything needed to run the dashboard loop and join cleanly
// afterward. Identical to Phase 2's inline main() body, extracted so Task 6
// can call it either at startup or after the wizard finishes.
Runtime buildRuntime(const psb::TopologyConfig& topo, ScreenInteractive& screen,
                     std::atomic<bool>& running, int timeoutMs, bool autoConnectAll) {
    Runtime rt;
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
            b->appState = std::unique_ptr<psb::tui::AppState>(new psb::tui::AppState{
                *b->client, b->connected, b->data, b->statusMsg, b->statusMutex,
                bw->workQueue, bw->workMutex, bw->workCv, screen});
            b->dashboard = psb::tui::makeBoardDashboard(*b, *bw, screen, running, timeoutMs);
            bw->boards.push_back(b.get());
            rt.boards.push_back(std::move(b));
        }
        rt.busWorkers.push_back(std::move(bw));
    }

    for (auto& bwPtr : rt.busWorkers) {
        psb::tui::BusWorker& bw = *bwPtr;
        bw.thread = std::thread([&bw, &running, &screen, autoConnectAll, timeoutMs] {
            if (autoConnectAll && !bw.boards.empty()) {
                std::string port = bw.boards.front()->portVal;
                int baud = 115200;
                try { baud = std::stoi(bw.boards.front()->baudVal); } catch (...) {}
                bool busOk = bw.bus->connect(port, baud, timeoutMs);
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

    rt.animThread = std::thread([&rt, &screen, &running] {
        while (running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            if (!running) break;
            bool anyConnected = false;
            for (auto& b : rt.boards) if (b->connected.load()) { anyConnected = true; break; }
            if (anyConnected) screen.PostEvent(Event::Custom);
        }
    });

    rt.switcher = psb::tui::makeBoardSwitcher(rt.boards);
    return rt;
}

void joinRuntime(Runtime& rt, std::atomic<bool>& running) {
    running = false;
    for (auto& bw : rt.busWorkers) bw->workCv.notify_all();
    for (auto& bw : rt.busWorkers) if (bw->thread.joinable()) bw->thread.join();
    if (rt.animThread.joinable()) rt.animThread.join();
    for (auto& bw : rt.busWorkers) bw->bus->disconnect();
}

int main(int argc, char** argv) {
    CLI::App app{"PSB Demo TUI"};
    app.set_version_flag("--version", std::string("psb_demo_tui ") + TOOL_VERSION_STRING);

    std::string portArg;
    int baudArg = 115200, slaveArg = 1, timeoutArg = 3000;
    std::string topologyPath = psb::TopologyConfig::defaultPath();
    bool setupFlag = false;
    app.add_option("-p,--port", portArg, "Serial port (quick single-board connect; auto-connects at startup)");
    app.add_option("-b,--baud", baudArg, "Baud rate")
        ->check(CLI::IsMember({9600, 19200, 38400, 115200}));
    app.add_option("-i,--id", slaveArg, "Slave ID")->check(CLI::Range(0, 247));
    app.add_option("-t,--timeout", timeoutArg, "Timeout ms");
    app.add_option("-s,--poll-interval", g_pollInterval, "Idle poll interval (s)");
    auto* topologyOpt = app.add_option("-T,--topology", topologyPath,
        "Topology config file (default: " + topologyPath + ")");
    app.add_flag("--setup", setupFlag, "Launch the interactive topology setup wizard");
    CLI11_PARSE(app, argc, argv);

    auto screen = ScreenInteractive::Fullscreen();

    // ---- Resolve (or build) the topology, running the wizard first when
    //      asked to or when needed (design spec case 3: a --topology path
    //      that doesn't exist yet auto-launches the wizard pre-targeting it
    //      as the Save destination, instead of silently falling through to
    //      hardcoded defaults). ----
    psb::TopologyConfig topo;
    bool haveTopo = false;
    bool topologyExplicit = topologyOpt->count() > 0;

    if (!portArg.empty() && !setupFlag) {
        topo = psb::TopologyConfig::singleBoard(portArg, baudArg, slaveArg, "board1");
        haveTopo = true;
    } else if (psb::TopologyConfig::exists(topologyPath) && !setupFlag) {
        auto loaded = psb::TopologyConfig::load(topologyPath);
        if (!loaded.has_value()) {
            std::cerr << "Topology config error: could not parse " << topologyPath << "\n";
            return 1;
        }
        topo = std::move(*loaded);
        haveTopo = true;
    } else if (topologyExplicit && !psb::TopologyConfig::exists(topologyPath)) {
        // Case 3 — file named but missing: wizard runs regardless of
        // --setup, pre-targeting this path.
    } else if (!setupFlag && !portArg.empty()) {
        // unreachable (portArg branch above already handled) — kept out for clarity.
    } else if (!setupFlag) {
        // Neither -p nor a resolvable/explicit --topology, and --setup not
        // given: today's genuinely-first-run hardcoded guess.
        topo = psb::TopologyConfig::singleBoard("/dev/ttyUSB0", 115200, 1, "board1");
        haveTopo = true;
    }
    // else: setupFlag is true — always run the wizard next, regardless of
    // what topo/haveTopo currently hold (a pre-existing topology, if any,
    // seeds the wizard for editing rather than starting from empty).

    bool runWizard = setupFlag || !haveTopo;
    if (runWizard) {
        psb::tui::WizardState wiz;
        wiz.topologyPath = topologyPath;
        if (haveTopo) wiz.topo = topo;

        psb::tui::WizardOutcome outcome = psb::tui::WizardOutcome::Cancelled;
        auto wizardRoot = psb::tui::makeWizardScreen(wiz, screen, [&](psb::tui::WizardOutcome o) {
            outcome = o;
            screen.ExitLoopClosure()();
        });
        screen.Loop(wizardRoot);

        if (outcome == psb::tui::WizardOutcome::Cancelled) {
            if (!haveTopo) return 0;  // first run, cancelled — nothing to connect to
            // else: fall through using the pre-existing topo unchanged.
        } else {
            topo = wiz.topo;
            haveTopo = true;
        }
        if (topo.totalBoardCount() == 0) {
            std::cerr << "Topology has no boards configured — exiting.\n";
            return 0;
        }
    }

    bool autoConnectAll = !portArg.empty() || runWizard || topo.totalBoardCount() > 1;

    std::atomic<bool> running{true};
    Runtime rt = buildRuntime(topo, screen, running, timeoutArg, autoConnectAll);

    screen.Loop(rt.switcher.root);
    joinRuntime(rt, running);
    return 0;
}
```

- [ ] **Step 2: Build**

Run: `cd tools && cmake --build build --target psb_demo_tui 2>&1 | tail -60`
Expected: clean build.

- [ ] **Step 3: Manual verification — every entry path via tmux**

Following `docs/guide/client-architecture-and-pitfalls.md` §3's methodology:

1. **Explicit `--setup`, no prior topology file** (`tmux new-session -d ... "./bin/psb_demo_tui --setup --topology /tmp/wiz1.toml"`): confirm the wizard screen renders (empty bus/board lists, path shown in the title), Add Bus opens its modal with a port dropdown, Add Board is disabled until a bus is selected.
2. **Case 3 auto-launch** (`--topology /tmp/wiz2.toml` where that file doesn't exist, no `--setup`): confirm the wizard launches automatically, pre-targeting `/tmp/wiz2.toml`.
3. **Existing topology, no `--setup`**: confirm the wizard is skipped entirely and the dashboard launches directly — this is the regression check that Task 6 didn't disturb the default (no-wizard) path Phase 2 already verified.
4. **Wizard → Save → Connect Now**: in the wizard, Add Bus (use a real port if hardware is attached, else any string), Add Board, Save, Connect Now; confirm the dashboard appears with that board attempting to connect.
5. **Wizard → Cancel with no prior topology**: confirm the program exits cleanly (no crash, no hang) rather than launching a dashboard with zero boards.

- [ ] **Step 4: `psb_tests` unaffected**

Run: `./build/psb_modbus_core/tests/psb_tests`
Expected: PASS, same count as Task 3 (this task touches no `psb_modbus_core` files).

- [ ] **Step 5: Commit**

```bash
git add tools/psb_demo_app/tui/main.cpp
git commit -m "feat(psb_demo_tui): wire the setup wizard into main() — --setup flag, case-3 auto-launch"
```

---

## Task 7: In-dashboard mid-session entry — add a board without restarting

**Files:**
- Modify: `tools/psb_demo_app/tui/board_dashboard.h` (add a "Setup" action to the status bar)
- Modify: `tools/psb_demo_app/tui/main.cpp` (own the wizard's Modal state + the live-attach logic, since it owns `Runtime`)

**Interfaces:**
- Consumes: `psb::tui::BoardSwitcher::attachBoard` (Task 4), `psb::tui::makeWizardScreen`/`WizardOutcome` (Task 5), `Runtime`/`buildRuntime`'s per-bus-thread structure (Task 6).
- Produces: a live "Setup" button reachable from any board's dashboard; on `ConnectNow` (relabeled "Apply" in this context — see below), any bus/board present in the wizard's edited topology but absent from the currently-running `Runtime` is attached live: a new port gets a new `BusWorker` + thread; a new board on an already-running bus is appended to that bus's `BusWorker.boards` and its dashboard is attached to the switcher via `attachBoard`.

**Scope note (Global Constraints):** this task only *adds*. Editing or removing an already-connected board/bus from the wizard while it's reachable mid-session is out of scope — the wizard's Remove/Edit actions still work on the in-memory `WizardState`, but applying a removal of something already running is not attempted here (matches today's constraint that live topology changes require Disconnect + edit the file + restart for anything except adding). Scan-assisted discovery mid-session is also out of scope for this task: `wizard_screen.h`'s scan (Task 5) opens its own throwaway `PsbSerialBus` for the swept port, which is safe pre-dashboard (nothing else is using that port yet) but is a real risk mid-session if the swept port happens to be a bus a `BusWorker` thread is already driving (two threads opening/talking to the same port). Mid-session, the wizard's Add Board flow only supports Manual entry — the button that opens Scan's controls is simply not present in this context (a `WizardScreen` opened with `bool allowScan` distinguishing the two call sites — see Step 1). Scan-assisted discovery mid-session is future work.

- [ ] **Step 1: Let `makeWizardScreen` suppress the Scan controls**

Edit `tools/psb_demo_app/tui/wizard_screen.h`, change the function signature:

```cpp
inline Component makeWizardScreen(WizardState& s, ScreenInteractive& screen,
                                  std::function<void(WizardOutcome)> onFinish) {
```

to:

```cpp
inline Component makeWizardScreen(WizardState& s, ScreenInteractive& screen,
                                  std::function<void(WizardOutcome)> onFinish,
                                  bool allowScan = true) {
```

In the `addBoardPopup`'s `Renderer` lambda, wrap the scan section so it's omitted when `allowScan` is false. Change:

```cpp
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
```

to:

```cpp
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
```

This changes the Renderer's outer lambda to capture `allowScan` too — add it to the capture list: `Renderer(addBoardForm, [&s, ..., allowScan] { ... })`.

- [ ] **Step 2: Add a "Setup" button to the dashboard's status bar**

Edit `tools/psb_demo_app/tui/board_dashboard.h`. `makeBoardDashboard` needs one new parameter — a callback invoked when the user wants to open the wizard, since only `main.cpp` has access to `Runtime` to actually apply the result:

Change the function signature:

```cpp
inline Component makeBoardDashboard(BoardSession& board, BusWorker& busWorker,
                                    ScreenInteractive& screen, std::atomic<bool>& running,
                                    int timeoutMs) {
```

to:

```cpp
inline Component makeBoardDashboard(BoardSession& board, BusWorker& busWorker,
                                    ScreenInteractive& screen, std::atomic<bool>& running,
                                    int timeoutMs, std::function<void()> openSetup) {
```

Add the `<functional>` include alongside the existing ones at the top of the file.

Change the status bar construction:

```cpp
    // ---- Status bar (connection details + SysConfig; Connect lives in the menu) ----
    auto statusBar    = Container::Horizontal({bSysCfg});
```

to:

```cpp
    // ---- Status bar (connection details + SysConfig; Connect lives in the menu) ----
    auto bOpenSetup = ActionButton("Setup", [openSetup] { openSetup(); });
    auto statusBar    = Container::Horizontal({bSysCfg, bOpenSetup});
```

And in the final `Renderer`'s `statusBarEl`, add it next to `bSysCfg->Render()` (change the capture list of that `Renderer` to also capture `bOpenSetup`):

```cpp
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
            text(" "),
            bOpenSetup->Render(),
        });
```

- [ ] **Step 3: `main.cpp` — own the mid-session wizard state and the live-attach logic**

Edit `tools/psb_demo_app/tui/main.cpp`. In `buildRuntime`, thread a real `openSetup` callback into every board's dashboard instead of the placeholder that doesn't exist yet — `buildRuntime` doesn't have access to the mid-session wizard machinery (that lives in `main()`, which owns `Runtime` and can mutate it), so it takes the callback as a parameter:

Change `buildRuntime`'s signature:

```cpp
Runtime buildRuntime(const psb::TopologyConfig& topo, ScreenInteractive& screen,
                     std::atomic<bool>& running, int timeoutMs, bool autoConnectAll) {
```

to:

```cpp
Runtime buildRuntime(const psb::TopologyConfig& topo, ScreenInteractive& screen,
                     std::atomic<bool>& running, int timeoutMs, bool autoConnectAll,
                     std::function<void()> openSetup) {
```

and its one call to `makeBoardDashboard`:

```cpp
            b->dashboard = psb::tui::makeBoardDashboard(*b, *bw, screen, running, timeoutMs);
```

to:

```cpp
            b->dashboard = psb::tui::makeBoardDashboard(*b, *bw, screen, running, timeoutMs, openSetup);
```

Now add the live-attach logic and wire everything together. Add this function above `main()`:

```cpp
// Attaches every bus/board present in `newTopo` but absent from the
// currently-running `rt` — strictly additive (see Task 7's Global
// Constraints scope note). A bus already running is matched by port,
// following the same technique the bus worker thread itself already uses
// to learn its own port (bw.boards.front()->portVal) — BusWorker has no
// port field of its own. A brand-new bus gets its own BusWorker and thread,
// built the same way buildRuntime() builds every other bus.
void applyNewBoardsLive(Runtime& rt, const psb::TopologyConfig& newTopo,
                        ScreenInteractive& screen, std::atomic<bool>& running,
                        int timeoutMs, std::function<void()> openSetup) {
    for (const auto& busCfg : newTopo.buses) {
        psb::tui::BusWorker* existingBw = nullptr;
        for (auto& bw : rt.busWorkers)
            if (!bw->boards.empty() && bw->boards.front()->portVal == busCfg.port)
                { existingBw = bw.get(); break; }

        for (const auto& boardCfg : busCfg.boards) {
            bool alreadyRunning = false;
            if (existingBw)
                for (auto* b : existingBw->boards)
                    if (b->nickname == boardCfg.nickname) { alreadyRunning = true; break; }
            if (alreadyRunning) continue;

            psb::tui::BusWorker* targetBw = existingBw;
            if (!targetBw) {
                auto bw = std::make_unique<psb::tui::BusWorker>();
                bw->bus = std::make_shared<psb::PsbSerialBus>();
                bw->bus->connect(busCfg.port, busCfg.baudRate, timeoutMs);
                targetBw = bw.get();
                rt.busWorkers.push_back(std::move(bw));
                psb::tui::BusWorker& bwRef = *targetBw;
                bwRef.thread = std::thread([&bwRef, &running, &screen] {
                    while (running) {
                        {
                            bool anyConnected = false;
                            for (auto* b : bwRef.boards) if (b->connected.load()) { anyConnected = true; break; }
                            auto waitDur = anyConnected ? std::chrono::milliseconds(50)
                                                        : std::chrono::seconds(g_pollInterval);
                            std::unique_lock<std::mutex> lk(bwRef.workMutex);
                            bwRef.workCv.wait_for(lk, waitDur, [&] { return !bwRef.workQueue.empty() || !running; });
                        }
                        for (;;) {
                            std::function<void()> item;
                            { std::lock_guard<std::mutex> lk(bwRef.workMutex);
                              if (bwRef.workQueue.empty()) break;
                              item = std::move(bwRef.workQueue.front()); bwRef.workQueue.pop(); }
                            item();
                        }
                        for (auto* b : bwRef.boards) {
                            if (!running) break;
                            if (b->connected.load()) {
                                auto hasPendingWork = [&bwRef] {
                                    std::lock_guard<std::mutex> lk(bwRef.workMutex);
                                    return !bwRef.workQueue.empty();
                                };
                                psb::tui::doPollScan(*b->client, b->data, screen, running, hasPendingWork, b->statusMsg, b->statusMutex);
                                b->data.valid = b->connected.load() && b->client->isConnected();
                            }
                        }
                    }
                });
                existingBw = targetBw;
            }

            auto b = std::make_unique<psb::tui::BoardSession>();
            b->nickname = boardCfg.nickname;
            b->bus = targetBw->bus;
            b->client = std::make_unique<psb::PsbBoardSession>(targetBw->bus, boardCfg.slaveId);
            b->portVal = busCfg.port;
            b->baudVal = std::to_string(busCfg.baudRate);
            b->slaveVal = std::to_string(boardCfg.slaveId);
            b->appState = std::unique_ptr<psb::tui::AppState>(new psb::tui::AppState{
                *b->client, b->connected, b->data, b->statusMsg, b->statusMutex,
                targetBw->workQueue, targetBw->workMutex, targetBw->workCv, screen});
            b->dashboard = psb::tui::makeBoardDashboard(*b, *targetBw, screen, running, timeoutMs, openSetup);

            // Connect + full-scan this one board right away, on its bus's
            // own worker queue — never block the UI thread, same discipline
            // doConnect() in board_dashboard.h already follows.
            psb::tui::BoardSession* bPtr = b.get();
            psb::tui::BusWorker* bwPtr = targetBw;
            { std::lock_guard<std::mutex> lk(bwPtr->workMutex);
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
              }); }
            bwPtr->workCv.notify_one();

            targetBw->boards.push_back(bPtr);
            rt.switcher.attachBoard(b->nickname, b->dashboard);
            rt.boards.push_back(std::move(b));
        }
    }
}
```

In `main()`, change the `buildRuntime` call site to pass a real `openSetup` callback that opens the wizard as a `Modal` atop whatever's currently on screen. Since `screen.Loop()` is already running the switcher's `root` by the time a user clicks "Setup", the wizard must be shown via a `Modal` flag rather than a second `screen.Loop()` call (the pre-dashboard entry point's approach, Task 6) — wrap `rt.switcher.root` once more:

Change:

```cpp
    std::atomic<bool> running{true};
    Runtime rt = buildRuntime(topo, screen, running, timeoutArg, autoConnectAll);

    screen.Loop(rt.switcher.root);
    joinRuntime(rt, running);
    return 0;
```

to:

```cpp
    std::atomic<bool> running{true};

    auto showSetup = std::make_shared<bool>(false);
    auto midSessionWiz = std::make_shared<psb::tui::WizardState>();
    midSessionWiz->topologyPath = topologyPath;

    std::function<void()> openSetup = [showSetup, midSessionWiz, &topo, &screen] {
        midSessionWiz->topo = topo;  // seed with the currently-running topology
        midSessionWiz->selectedBus = -1;
        midSessionWiz->selectedBoard = -1;
        midSessionWiz->statusMsg.clear();
        *showSetup = true;
        screen.PostEvent(Event::Custom);
    };

    Runtime rt = buildRuntime(topo, screen, running, timeoutArg, autoConnectAll, openSetup);

    auto onMidSessionFinish = [showSetup, midSessionWiz, &rt, &topo, &screen, &running, timeoutArg, openSetup]
                              (psb::tui::WizardOutcome outcome) {
        if (outcome == psb::tui::WizardOutcome::ConnectNow) {
            applyNewBoardsLive(rt, midSessionWiz->topo, screen, running, timeoutArg, openSetup);
            topo = midSessionWiz->topo;
        }
        *showSetup = false;
        screen.PostEvent(Event::Custom);
    };
    auto midSessionWizardRoot = psb::tui::makeWizardScreen(*midSessionWiz, screen, onMidSessionFinish, /*allowScan=*/false);

    auto rootWithSetup = Renderer(rt.switcher.root, [&rt] { return rt.switcher.root->Render(); })
        | Modal(midSessionWizardRoot, showSetup.get());

    screen.Loop(rootWithSetup);
    joinRuntime(rt, running);
    return 0;
```

- [ ] **Step 4: Build**

Run: `cd tools && cmake --build build --target psb_demo_tui psb_tests 2>&1 | tail -60 && ./build/psb_modbus_core/tests/psb_tests`
Expected: clean build, tests unaffected (same count as Task 3 — this task touches no `psb_modbus_core` files).

- [ ] **Step 5: Manual verification via tmux, with real hardware if available**

1. Launch with a single-board topology; click **Setup** in the status bar; confirm the wizard modal opens with that one bus/board pre-populated and the Scan section is absent (mid-session `allowScan=false`).
2. Add a second board **on the same already-connected bus** (matches the wizard's bus list — pick the existing bus, Add Board, a new nickname/slave ID); click **Connect Now**; confirm: the modal closes, the switcher bar appears for the first time (previously hidden per Task 4's single-board constraint), the new board's dashboard shows up and starts connecting/scanning, and the original board's display is undisturbed throughout (uptime keeps advancing — same check Phase 2's Task 6 already established for the switcher).
3. If a second physical port is available: repeat, this time using **Add Bus** for a new port + Add Board — confirm a brand-new `BusWorker` thread spins up and that board also connects independently.
4. Quit (`[ Quit ]`) with the mid-session-added board(s) in a mix of connected/disconnected states; confirm every thread joins (no hang) — `pgrep -af psb_demo_tui` shows nothing afterward, same check used throughout Phase 2.

- [ ] **Step 6: Commit**

```bash
git add tools/psb_demo_app/tui/wizard_screen.h tools/psb_demo_app/tui/board_dashboard.h tools/psb_demo_app/tui/main.cpp
git commit -m "feat(psb_demo_tui): in-dashboard Setup action — add a board without restarting

Strictly additive (Global Constraints): a new bus gets its own worker
thread, a new board on an already-running bus is appended to that
bus's worker and attached to the switcher live via
BoardSwitcher::attachBoard (Task 4). Editing/removing a running
board/bus, and scan-assisted discovery mid-session, remain out of
scope — see wizard_screen.h's allowScan parameter."
```

---

## Task 8: Full-repo verification

**Files:** none (verification only).

- [ ] **Step 1: Clean rebuild of every touched target**

Run: `cd tools && cmake --build build --target psb_modbus_core psb_tests psb_demo_cli psb_demo_tui 2>&1 | tail -40`
Expected: clean build.

- [ ] **Step 2: Full `psb_tests` run**

Run: `./build/psb_modbus_core/tests/psb_tests`
Expected: PASS — the running total should now include Task 2's 11 `[wizard_state]` cases and Task 3's 3 `[wizard_scan]` cases on top of the 340/86 baseline this plan started from (Task 1 adds 1 more assertion to an existing test case, not a new one).

- [ ] **Step 3: `psb_demo_gui` still builds (Global Constraint — still untouched; GUI multi-board work is a separate, not-yet-designed follow-on per the spec's "Out of Scope" section)**

Qt6 is installed at `/home/yong/backup/Qt/6.8.5` on this machine (confirmed by the user — supersedes the Qt5-only environment Phase 2's Task 6 found).

Run: `cd tools && cmake -DBUILD_GUI=ON -DCMAKE_PREFIX_PATH=/home/yong/backup/Qt/6.8.5/gcc_64/lib/cmake . && cmake --build build --target psb_demo_gui 2>&1 | tail -40`
Expected: clean build. (If the exact `CMAKE_PREFIX_PATH` subdirectory differs from `gcc_64/lib/cmake` on this install, list `/home/yong/backup/Qt/6.8.5/` first to find the actual `Qt6Config.cmake` location before running configure.)

After confirming the build, revert the cache to the project's normal (GUI-off) state: `cmake -DBUILD_GUI=OFF -S . -B build`.

- [ ] **Step 4: Live-hardware pass**

Repeat Task 6 Step 3 and Task 7 Step 5 against real hardware if not already done there — this is the same "don't call a phase done without a live pass" discipline every prior phase in this project has followed (§2.4/§2.5 pacing lessons apply to the wizard's scan sweep too: confirm a real 1-32 sweep against a bus with one or two real boards attached completes in a few seconds, not tens of seconds, validating Task 1/3's short-timeout design actually behaves as intended against real (not test-mode) hardware round-trip latency).

- [ ] **Step 5: Clean up any temporary topology files created during manual verification**

Run: `rm -rf /tmp/wiz1.toml /tmp/wiz2.toml` (and any other scratch topology paths used during Task 6/7's manual checks).

---

## Self-Review

**Spec coverage** (against `docs/superpowers/specs/2026-07-20-multi-board-topology-design.md`'s "Interactive setup wizard (TUI)" section and Rollout item 3):
- ✅ Entry points: `--setup` flag (Task 6) and an in-dashboard action to reach it mid-session (Task 7).
- ✅ Add Bus — port (via `PsbSerialBus::scanPorts()`) + baud rate (Task 5).
- ✅ Add Board — Manual (Task 5) and Add Board — Scan, sweeping a user-adjustable range defaulting to 1-32 (Task 5, via Task 3's `scanBus`).
- ✅ Edit/Remove bus or board — Remove is implemented (Task 5); the spec groups "Edit/Remove" together but the only per-field edit a bus/board config exposes (nickname, slave ID, port, baud) is fully covered by Remove-then-Add-again through the same modals, consistent with how little there is to "edit" on a two/three-field record.
- ✅ Connect Now — jump into the live dashboard using the current in-memory topology, even unsaved (Task 5's `bConnectNow`, wired in Task 6).
- ✅ Save (prompting for a path the first time)/Save As/Load — `s.topologyPath` is always pre-filled (from `--topology` or the default path) before the wizard ever renders, so Save always has a target; Task 5's code now renders an editable `topologyPathInp` bound directly to `s.topologyPath` (typing a new path before Save doubles as Save As) plus a `Load` button wired to `TopologyConfig::load()`.

**Placeholder/gap found during self-review, now fixed:** `wizard_screen.h` (Task 5) originally never rendered an editable field for `s.topologyPath`, so "Save As" and "Load" had no UI path. Fixed directly in Task 5's Step 1 code block above: a `topologyPathInp` `Input` bound to `&s.topologyPath` (typing a new path before Save is Save As) and a `bLoadTopology` `ActionButton` that calls `TopologyConfig::load(s.topologyPath)`, and on success replaces `s.topo`, clears selection/dirty, and calls `rebuildBusNames()`/`rebuildBoardNames()`. Both are wired into `mainContainer` and rendered in a "Path:" row above the status message.

**Type consistency:** cross-checked `WizardState`/`DiscoveredBoard`/`BoardSwitcher` field and function names across Tasks 2-7 — `addBus`/`removeBus`/`addBoard`/`removeBoard` signatures in Task 5's call sites match Task 2's declarations exactly; `scanBus`'s signature in Task 5's scan thread matches Task 3's; `BoardSwitcher::attachBoard`'s signature in Task 7's `applyNewBoardsLive` matches Task 4's.

**No-placeholders scan:** every code step contains complete, compilable content; the one gap found above (topology path editing) is called out explicitly with a concrete fix rather than left as a TBD.

---

Plan complete and saved to `docs/superpowers/plans/2026-07-20-multi-board-topology-phase3-tui-wizard.md`. Two execution options:

1. **Subagent-Driven (recommended)** — I dispatch a fresh subagent per task, review between tasks, fast iteration
2. **Inline Execution** — Execute tasks in this session using executing-plans, batch execution with checkpoints

Which approach?
