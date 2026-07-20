# Multi-Board Topology — Phase 1 (Core) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Split `psb_modbus_core`'s single-board `PsbModbusClient` into a `PsbSerialBus` (owns one physical serial connection) / `PsbBoardSession` (one slave ID on a bus) pair, add a `TopologyConfig` TOML module superseding `ConfigManager`, and wire both `psb_demo_cli` and `psb_demo_tui` onto CLI11 + the new config — with zero observable behavior change for today's single-board flags/config.

**Architecture:** `PsbSerialBus` owns the port and exposes slave-ID-keyed transaction primitives (real Modbus RTU or, for tests, per-slave-ID in-memory register arrays). `PsbBoardSession` is a thin per-board handle referencing a bus; its ~50 high-level read/write methods are mechanically unchanged from today's `PsbModbusClient` (they only ever call the private `checkConnected()`/`readRegsInternal()`/`writeRegsInternal()` helpers, which are now the only pieces that change). `PsbModbusClient`'s public API stays byte-identical, reimplemented as a thin facade over one bus + one session, so `psb_demo_gui` (untouched this phase) keeps compiling.

**Tech Stack:** C++17, ModbusLib (vendored, RTU client/server), toml++ (vendored), CLI11 (vendored), Catch2 (vendored, `psb_tests`).

**Reference spec:** `docs/superpowers/specs/2026-07-20-multi-board-topology-design.md`

## Global Constraints

- `PsbModbusClient`'s public header (`psb_modbus_client.h`) does not change — every existing call site in `psb_demo_gui`, and every existing test in `psb_tests`, must compile and pass unchanged against the new implementation.
- `PsbSerialBus` and `PsbBoardSession` are **not** internally thread-safe — callers serialize their own access to a given bus (documented in each class's header), matching `PsbModbusClient`'s existing contract.
- Every new/modified file uses `namespace psb`, C++17, and matches this codebase's existing comment density for non-obvious decisions (see `psb_modbus_client.cpp` for the house style — comments explain *why*, not *what*).
- Preserve every existing user-facing error-message substring exactly (`"not connected"`, `"out of range"`, `"no response from board"`, `"unexpected protocol version"`, the protocol-incompatibility message) — several existing tests and this plan's new tests check these by substring.
- `ConfigManager`'s `--save`/auto-load convenience (both tools) must be preserved via `TopologyConfig`, not dropped — see spec's CLI flags precedence section.
- Build via `cd tools && cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build` (add `-DBUILD_GUI=ON -DCMAKE_PREFIX_PATH=<qt-path>` only if touching GUI — not needed for this plan). Test via `./build/psb_modbus_core/tests/psb_tests`.

---

## Task 1: `PsbSerialBus` — owns the physical port, slave-ID-keyed transactions

**Files:**
- Create: `tools/psb_modbus_core/psb_serial_bus.h`
- Create: `tools/psb_modbus_core/psb_serial_bus.cpp`
- Create: `tools/psb_modbus_core/tests/test_serial_bus.cpp`
- Modify: `tools/psb_modbus_core/CMakeLists.txt`
- Modify: `tools/psb_modbus_core/tests/CMakeLists.txt`

**Interfaces:**
- Produces: `psb::PsbSerialBus` with `connect(port, baud=115200, timeoutMs=500) -> bool`, `disconnect()`, `isConnected() const -> bool`, `lastError() const -> std::string`, `attachTestArrays(int slaveId, uint16_t* inputRegs, uint16_t* holdingRegs, int maxAddr)`, `detachTestArrays(int slaveId)`, `readInputRegs(int slaveId, uint16_t addr, uint16_t count, uint16_t* out, int timeoutOverrideMs=-1) -> bool`, `readHoldingRegs(...)` (same shape), `writeRegs(int slaveId, uint16_t addr, uint16_t count, const uint16_t* values, int timeoutOverrideMs=-1) -> bool`, `setFrameCallback(FrameCallback)` where `FrameCallback = std::function<void(int slaveId, bool tx, const std::vector<uint8_t>&)>`, static `scanPorts() -> std::vector<std::string>`, static `availableBaudRates() -> std::vector<int>`.

- [ ] **Step 1: Write the failing tests**

Create `tools/psb_modbus_core/tests/test_serial_bus.cpp`:

```cpp
#include "psb_serial_bus.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("PsbSerialBus — scanPorts returns only USB serial (ttyUSB/ttyACM)", "[serial_bus]") {
    auto ports = psb::PsbSerialBus::scanPorts();
    for (const auto& p : ports) {
        INFO("Port: " << p);
        CHECK((p.rfind("/dev/ttyUSB", 0) == 0 || p.rfind("/dev/ttyACM", 0) == 0));
    }
}

TEST_CASE("PsbSerialBus — read fails when not connected and no test arrays", "[serial_bus]") {
    psb::PsbSerialBus bus;
    uint16_t out[2] = {};
    CHECK_FALSE(bus.readInputRegs(1, 0, 2, out));
    CHECK(bus.lastError() == "not connected");
    CHECK_FALSE(bus.isConnected());
}

TEST_CASE("PsbSerialBus — two slave IDs on one bus read/write independently, no cross-contamination", "[serial_bus]") {
    psb::PsbSerialBus bus;
    uint16_t inputA[8] = {}, holdingA[8] = {};
    uint16_t inputB[8] = {}, holdingB[8] = {};
    inputA[0] = 111;
    inputB[0] = 222;

    bus.attachTestArrays(1, inputA, holdingA, 8);
    bus.attachTestArrays(2, inputB, holdingB, 8);
    REQUIRE(bus.isConnected());

    uint16_t outA = 0, outB = 0;
    REQUIRE(bus.readInputRegs(1, 0, 1, &outA));
    REQUIRE(bus.readInputRegs(2, 0, 1, &outB));
    CHECK(outA == 111);
    CHECK(outB == 222);

    uint16_t writeVal = 42;
    REQUIRE(bus.writeRegs(1, 0, 1, &writeVal));
    CHECK(holdingA[0] == 42);
    CHECK(holdingB[0] == 0);  // slave 2's holding regs untouched by slave 1's write

    bus.detachTestArrays(1);
    uint16_t outAfterDetach = 0;
    CHECK_FALSE(bus.readInputRegs(1, 0, 1, &outAfterDetach));  // no port connected, no test arrays for slave 1 anymore
    REQUIRE(bus.readInputRegs(2, 0, 1, &outB));  // slave 2 still works
}

TEST_CASE("PsbSerialBus — out-of-range test address fails clearly", "[serial_bus]") {
    psb::PsbSerialBus bus;
    uint16_t inputRegs[8] = {}, holdingRegs[8] = {};
    bus.attachTestArrays(1, inputRegs, holdingRegs, 8);

    uint16_t out[1] = {};
    REQUIRE_FALSE(bus.readInputRegs(1, 100, 1, out));
    CHECK(bus.lastError().find("out of range") != std::string::npos);
}
```

- [ ] **Step 2: Register the new test file**

Edit `tools/psb_modbus_core/tests/CMakeLists.txt` — add `test_serial_bus.cpp` to the `add_executable(psb_tests ...)` list, alongside the existing `test_connection.cpp` entry.

- [ ] **Step 3: Confirm it fails to build (header doesn't exist yet)**

Run: `cd tools && cmake --build build --target psb_tests`
Expected: FAIL — `fatal error: 'psb_serial_bus.h' file not found`

- [ ] **Step 4: Write `psb_serial_bus.h`**

```cpp
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace psb {

// Owns exactly one physical serial connection and serializes every
// transaction on it by slave ID — the correct model for RS-485 multi-drop
// (one wire, many boards addressed by slave ID) as well as the single-board
// case (one bus, one board). Not thread-safe: exactly one thread must drive
// a given bus at a time, mirroring PsbModbusClient's historical contract —
// see docs/guide/client-architecture-and-pitfalls.md §2.1. PsbBoardSession
// is the per-board handle that uses this.
class PsbSerialBus {
public:
    using FrameCallback = std::function<void(int slaveId, bool tx, const std::vector<uint8_t>& data)>;

    PsbSerialBus();
    ~PsbSerialBus();

    bool connect(const std::string& port, int baud = 115200, int timeoutMs = 500);
    void disconnect();
    bool isConnected() const;
    std::string lastError() const;

    // Test mode — direct array access per slave ID, bypasses Modbus RTU
    // entirely. Lets tests simulate multiple distinct boards sharing one
    // bus without a real serial port. Independent of connect()/disconnect().
    void attachTestArrays(int slaveId, uint16_t* inputRegs, uint16_t* holdingRegs, int maxAddr);
    void detachTestArrays(int slaveId);

    // Serialized transaction primitives — not thread-safe; caller (one
    // worker thread per bus) must serialize its own access. Every
    // PsbBoardSession attached to this bus routes through these.
    bool readInputRegs(int slaveId, uint16_t addr, uint16_t count, uint16_t* out,
                        int timeoutOverrideMs = -1);
    bool readHoldingRegs(int slaveId, uint16_t addr, uint16_t count, uint16_t* out,
                          int timeoutOverrideMs = -1);
    bool writeRegs(int slaveId, uint16_t addr, uint16_t count, const uint16_t* values,
                   int timeoutOverrideMs = -1);

    void setFrameCallback(FrameCallback cb);

    static std::vector<std::string> scanPorts();
    static std::vector<int> availableBaudRates();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;

    bool readRegsInternal(bool holding, int slaveId, uint16_t addr, uint16_t count, uint16_t* out,
                          int timeoutOverrideMs);
};

} // namespace psb
```

- [ ] **Step 5: Write `psb_serial_bus.cpp`**

```cpp
#include "psb_serial_bus.h"

#include <ModbusClientPort.h>
#include <ModbusPort.h>
#include <Modbus.h>

#include <chrono>
#include <cstring>
#include <thread>

namespace psb {

// See PsbModbusClient's historical version of this comment
// (git history / client-architecture-and-pitfalls.md §1.3) for the full
// story: ModbusClientPort::process() is a non-blocking state machine meant
// to be pumped from an external loop: a zero-delay spin pegs a CPU core at
// ~100% for the duration of every transaction. This sleep is required, not
// cosmetic.
static constexpr auto kNonBlockingPollInterval = std::chrono::milliseconds(1);

struct PsbSerialBus::Impl {
    ModbusClientPort* port = nullptr;
    bool connected = false;
    std::string errorText;
    FrameCallback frameCb;
    int lastSlaveId = 0;

    struct TestBoard {
        uint16_t* inputRegs;
        uint16_t* holdingRegs;
        int maxAddr;
    };
    std::map<int, TestBoard> testBoards;

    ~Impl() { disconnect(); }
    void disconnect() {
        if (port) { delete port; port = nullptr; }
        connected = false;
    }

    void onTx(const Modbus::Char*, const uint8_t* buff, uint16_t size) {
        if (frameCb) frameCb(lastSlaveId, true, std::vector<uint8_t>(buff, buff + size));
    }
    void onRx(const Modbus::Char*, const uint8_t* buff, uint16_t size) {
        if (frameCb) frameCb(lastSlaveId, false, std::vector<uint8_t>(buff, buff + size));
    }
};

PsbSerialBus::PsbSerialBus() : m_impl(std::make_unique<Impl>()) {}
PsbSerialBus::~PsbSerialBus() = default;

bool PsbSerialBus::connect(const std::string& portName, int baud, int timeoutMs) {
    m_impl->disconnect();

    Modbus::SerialSettings settings;
    settings.portName = portName.c_str();
    settings.baudRate = baud;
    settings.dataBits = 8;
    settings.parity = Modbus::NoParity;
    settings.stopBits = Modbus::OneStop;
    settings.flowControl = Modbus::NoFlowControl;
    settings.timeoutFirstByte = static_cast<uint32_t>(timeoutMs);
    // timeoutInterByte governs how long ModbusLib waits for each subsequent
    // byte within an already-started response before concluding the frame
    // is complete — a property of the serial link and adapter, unrelated to
    // timeoutFirstByte (how long to wait for a response to even begin) and
    // must NOT be derived from it. See
    // docs/guide/client-architecture-and-pitfalls.md §1.2 — a
    // timeoutMs/10-style formula here was a real, previously-fixed bug that
    // made every transaction (not just failures) ~6x slower than necessary.
    // 50ms has generous margin over both the Modbus RTU spec's
    // frame-silence threshold (~0.3ms at 115200 baud) and real USB-serial
    // adapter buffering behavior (e.g. CH340).
    settings.timeoutInterByte = 50;

    // Non-blocking mode: engages ModbusLib's inter-byte accumulation loop.
    // The blocking path's single ::read() treats a partial frame as
    // complete, truncating responses >~31 bytes over USB-serial. Our
    // read/write wrappers poll until the request stops returning
    // Status_Processing (see kNonBlockingPollInterval above).
    m_impl->port = Modbus::createClientPort(Modbus::RTU, &settings, false);
    if (!m_impl->port) {
        m_impl->errorText = "failed to create RTU client port";
        return false;
    }
    m_impl->port->connect(&ModbusClientPort::signalTx, m_impl.get(), &Impl::onTx);
    m_impl->port->connect(&ModbusClientPort::signalRx, m_impl.get(), &Impl::onRx);
    m_impl->connected = true;
    return true;
}

void PsbSerialBus::disconnect() { m_impl->disconnect(); }
bool PsbSerialBus::isConnected() const { return m_impl->connected || !m_impl->testBoards.empty(); }
std::string PsbSerialBus::lastError() const { return m_impl->errorText; }

void PsbSerialBus::attachTestArrays(int slaveId, uint16_t* inputRegs, uint16_t* holdingRegs, int maxAddr) {
    m_impl->testBoards[slaveId] = Impl::TestBoard{inputRegs, holdingRegs, maxAddr};
}
void PsbSerialBus::detachTestArrays(int slaveId) {
    m_impl->testBoards.erase(slaveId);
}

namespace {
// RAII helper: temporarily overrides a ModbusPort's response timeout for
// the duration of a single request, restoring the previous value on scope
// exit (including early returns). No-op if port is null or override is
// negative.
class ScopedPortTimeout {
public:
    ScopedPortTimeout(ModbusPort* port, int overrideMs) : m_port(nullptr) {
        if (port && overrideMs >= 0) {
            m_port = port;
            m_saved = m_port->timeout();
            m_port->setTimeout(static_cast<uint32_t>(overrideMs));
        }
    }
    ~ScopedPortTimeout() {
        if (m_port) m_port->setTimeout(m_saved);
    }
    ScopedPortTimeout(const ScopedPortTimeout&) = delete;
    ScopedPortTimeout& operator=(const ScopedPortTimeout&) = delete;

private:
    ModbusPort* m_port;
    uint32_t m_saved = 0;
};
} // namespace

bool PsbSerialBus::readRegsInternal(bool holding, int slaveId, uint16_t addr, uint16_t count,
                                    uint16_t* out, int timeoutOverrideMs) {
    auto it = m_impl->testBoards.find(slaveId);
    if (it != m_impl->testBoards.end()) {
        auto& tb = it->second;
        uint16_t* src = holding ? tb.holdingRegs : tb.inputRegs;
        if (!src || addr + count > static_cast<uint16_t>(tb.maxAddr)) {
            m_impl->errorText = "test: address out of range";
            return false;
        }
        memcpy(out, src + addr, count * sizeof(uint16_t));
        return true;
    }
    if (!m_impl->connected || !m_impl->port) {
        m_impl->errorText = "not connected";
        return false;
    }
    m_impl->lastSlaveId = slaveId;
    ScopedPortTimeout timeoutGuard(m_impl->port->port(), timeoutOverrideMs);
    Modbus::StatusCode s;
    auto unit = static_cast<uint8_t>(slaveId);
    do {
        if (holding)
            s = m_impl->port->readHoldingRegisters(unit, addr, count, out);
        else
            s = m_impl->port->readInputRegisters(unit, addr, count, out);
        if (Modbus::StatusIsProcessing(s)) std::this_thread::sleep_for(kNonBlockingPollInterval);
    } while (Modbus::StatusIsProcessing(s));
    if (!Modbus::StatusIsGood(s)) {
        m_impl->errorText = std::string("Modbus error: ") + m_impl->port->lastErrorText();
        return false;
    }
    return true;
}

bool PsbSerialBus::readInputRegs(int slaveId, uint16_t addr, uint16_t count, uint16_t* out,
                                 int timeoutOverrideMs) {
    return readRegsInternal(false, slaveId, addr, count, out, timeoutOverrideMs);
}
bool PsbSerialBus::readHoldingRegs(int slaveId, uint16_t addr, uint16_t count, uint16_t* out,
                                   int timeoutOverrideMs) {
    return readRegsInternal(true, slaveId, addr, count, out, timeoutOverrideMs);
}

bool PsbSerialBus::writeRegs(int slaveId, uint16_t addr, uint16_t count, const uint16_t* values,
                             int timeoutOverrideMs) {
    auto it = m_impl->testBoards.find(slaveId);
    if (it != m_impl->testBoards.end()) {
        auto& tb = it->second;
        if (!tb.holdingRegs || addr + count > static_cast<uint16_t>(tb.maxAddr)) {
            m_impl->errorText = "test: address out of range";
            return false;
        }
        memcpy(tb.holdingRegs + addr, values, count * sizeof(uint16_t));
        return true;
    }
    if (!m_impl->connected || !m_impl->port) {
        m_impl->errorText = "not connected";
        return false;
    }
    m_impl->lastSlaveId = slaveId;
    ScopedPortTimeout timeoutGuard(m_impl->port->port(), timeoutOverrideMs);
    auto unit = static_cast<uint8_t>(slaveId);
    for (uint16_t i = 0; i < count; i++) {
        Modbus::StatusCode s;
        do {
            s = m_impl->port->writeSingleRegister(unit, addr + i, values[i]);
            if (Modbus::StatusIsProcessing(s)) std::this_thread::sleep_for(kNonBlockingPollInterval);
        } while (Modbus::StatusIsProcessing(s));
        if (!Modbus::StatusIsGood(s)) {
            m_impl->errorText = std::string("Modbus error: ") + m_impl->port->lastErrorText();
            return false;
        }
    }
    return true;
}

void PsbSerialBus::setFrameCallback(FrameCallback cb) {
    m_impl->frameCb = std::move(cb);
}

std::vector<std::string> PsbSerialBus::scanPorts() {
    std::vector<std::string> result;
    for (const auto& p : Modbus::availableSerialPorts()) {
        std::string path(p);
#if defined(_WIN32)
        if (path.rfind("COM", 0) == 0)
            result.push_back(path);
#else
        // ttyUSB: USB-to-serial adapters (FTDI, CH340, etc.) using a
        // separate UART chip. ttyACM: USB CDC-ACM devices — boards with
        // native USB, and multi-port USB-serial adapters (e.g. WCH
        // "Quad_Serial") that enumerate as ACM rather than USB. Both are
        // real external serial links worth listing; ttyS* (onboard,
        // non-USB serial) stays excluded.
        if (path.rfind("/dev/ttyUSB", 0) == 0 || path.rfind("/dev/ttyACM", 0) == 0)
            result.push_back(path);
#endif
    }
    return result;
}

std::vector<int> PsbSerialBus::availableBaudRates() {
    std::vector<int> result;
    for (auto r : Modbus::availableBaudRate())
        result.push_back(static_cast<int>(r));
    return result;
}

} // namespace psb
```

- [ ] **Step 6: Register the new source file**

Edit `tools/psb_modbus_core/CMakeLists.txt` — add `psb_serial_bus.cpp` to the `add_library(psb_modbus_core STATIC ...)` list, alongside `psb_modbus_client.cpp`.

- [ ] **Step 7: Build and run the new tests**

Run: `cd tools && cmake --build build --target psb_tests && ./build/psb_modbus_core/tests/psb_tests "[serial_bus]"`
Expected: PASS — 4 test cases, all assertions green.

- [ ] **Step 8: Commit**

```bash
git add tools/psb_modbus_core/psb_serial_bus.h tools/psb_modbus_core/psb_serial_bus.cpp \
        tools/psb_modbus_core/tests/test_serial_bus.cpp \
        tools/psb_modbus_core/CMakeLists.txt tools/psb_modbus_core/tests/CMakeLists.txt
git commit -m "feat(psb_modbus_core): add PsbSerialBus — shared-port multi-board transaction primitive"
```

---

## Task 2: `PsbBoardSession` — one board (slave ID) on a bus

`PsbModbusClient`'s ~50 high-level read/write methods only ever call its
private `checkConnected()`/`readRegsInternal()`/`writeRegsInternal()`
helpers — never `m_impl->port`/`m_impl->slaveId` directly. So this task is a
copy of `psb_modbus_client.cpp`, class-renamed, with only those helpers (and
connection setup) reimplemented to route through a `PsbSerialBus` — the
~600 lines of high-level method bodies move verbatim, untouched.

**Files:**
- Create: `tools/psb_modbus_core/psb_board_session.h`
- Create: `tools/psb_modbus_core/psb_board_session.cpp` (derived from `tools/psb_modbus_core/psb_modbus_client.cpp` — see Step 3)
- Create: `tools/psb_modbus_core/tests/test_board_session.cpp`
- Modify: `tools/psb_modbus_core/CMakeLists.txt`
- Modify: `tools/psb_modbus_core/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `psb::PsbSerialBus` (Task 1) — `readInputRegs`/`readHoldingRegs`/`writeRegs`/`attachTestArrays`/`detachTestArrays`/`isConnected`/`lastError`, all keyed by slave ID.
- Produces: `psb::PsbBoardSession` — constructor `(std::shared_ptr<PsbSerialBus> bus, int slaveId)`, `verifyProtocol() -> bool`, `disconnect()`, `isConnected() const -> bool`, `lastError() const -> std::string`, `slaveId() const -> int`, `currentUnitExp() const -> int16_t`, `attachTestArrays(...)`/`detachTestArrays()`, plus the complete high-level read/write API (identical method names/signatures to today's `PsbModbusClient`, minus `connect()`/`scanPorts()`/`availableBaudRates()`/`setFrameCallback()`).

- [ ] **Step 1: Write the failing tests**

Create `tools/psb_modbus_core/tests/test_board_session.cpp`:

```cpp
#include "psb_board_session.h"
#include "psb_serial_bus.h"
#include "register_map.h"

#include <catch2/catch_test_macros.hpp>
#include <memory>

TEST_CASE("PsbBoardSession — verifyProtocol succeeds against a compatible protocol version", "[board_session]") {
    auto bus = std::make_shared<psb::PsbSerialBus>();
    uint16_t inputRegs[8] = {}, holdingRegs[8] = {};
    inputRegs[0] = VC_PROTOCOL_MAJOR;
    inputRegs[1] = VC_PROTOCOL_MINOR;
    bus->attachTestArrays(1, inputRegs, holdingRegs, 8);

    psb::PsbBoardSession session(bus, 1);
    REQUIRE(session.verifyProtocol());
    CHECK(session.isConnected());
}

TEST_CASE("PsbBoardSession — verifyProtocol fails on no response", "[board_session]") {
    auto bus = std::make_shared<psb::PsbSerialBus>();
    // No test arrays attached for slave 1, bus never connected — every read fails.
    psb::PsbBoardSession session(bus, 1);
    REQUIRE_FALSE(session.verifyProtocol());
    CHECK(session.lastError().find("no response from board") != std::string::npos);
    CHECK_FALSE(session.isConnected());
}

TEST_CASE("PsbBoardSession — verifyProtocol fails on out-of-range protocol major", "[board_session]") {
    auto bus = std::make_shared<psb::PsbSerialBus>();
    uint16_t inputRegs[8] = {}, holdingRegs[8] = {};
    inputRegs[0] = 0;  // protoMajor 0 is invalid (< 1)
    bus->attachTestArrays(1, inputRegs, holdingRegs, 8);

    psb::PsbBoardSession session(bus, 1);
    REQUIRE_FALSE(session.verifyProtocol());
    CHECK(session.lastError().find("unexpected protocol version") != std::string::npos);
}

TEST_CASE("PsbBoardSession — two sessions on one bus don't share slave-ID-scoped state", "[board_session]") {
    auto bus = std::make_shared<psb::PsbSerialBus>();
    uint16_t inputA[8] = {}, holdingA[8] = {};
    uint16_t inputB[8] = {}, holdingB[8] = {};
    inputA[0] = VC_PROTOCOL_MAJOR; inputA[1] = VC_PROTOCOL_MINOR;
    inputB[0] = 0;  // deliberately invalid, to prove session B's failure doesn't affect session A
    bus->attachTestArrays(1, inputA, holdingA, 8);
    bus->attachTestArrays(2, inputB, holdingB, 8);

    psb::PsbBoardSession sessionA(bus, 1);
    psb::PsbBoardSession sessionB(bus, 2);
    REQUIRE(sessionA.verifyProtocol());
    REQUIRE_FALSE(sessionB.verifyProtocol());
    CHECK(sessionA.isConnected());
    CHECK_FALSE(sessionB.isConnected());
}

TEST_CASE("PsbBoardSession — attachTestArrays bypasses verifyProtocol, matching PsbModbusClient's existing contract", "[board_session]") {
    auto bus = std::make_shared<psb::PsbSerialBus>();
    uint16_t inputRegs[280] = {}, holdingRegs[280] = {};
    psb::PsbBoardSession session(bus, 1);
    session.attachTestArrays(inputRegs, holdingRegs, 280);
    CHECK(session.isConnected());

    session.detachTestArrays();
    CHECK_FALSE(session.isConnected());
}
```

- [ ] **Step 2: Register the new test file**

Edit `tools/psb_modbus_core/tests/CMakeLists.txt` — add `test_board_session.cpp` to the `psb_tests` source list.

- [ ] **Step 3: Confirm it fails to build (header doesn't exist yet)**

Run: `cd tools && cmake --build build --target psb_tests`
Expected: FAIL — `fatal error: 'psb_board_session.h' file not found`

- [ ] **Step 4: Write `psb_board_session.h`**

```cpp
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "types.h"

namespace psb {

class PsbSerialBus;

// One board (one slave ID) on a PsbSerialBus. Every high-level read/write
// method below is mechanically identical to PsbModbusClient's original,
// pre-split implementation — the only things that changed are how a
// transaction reaches the wire (now via m_bus, keyed by slave ID) and
// connection setup (now a constructor + verifyProtocol(), since the bus —
// not this class — owns the physical port).
class PsbBoardSession {
public:
    PsbBoardSession(std::shared_ptr<PsbSerialBus> bus, int slaveId);
    ~PsbBoardSession();

    // Reads PROTOCOL_MAJOR/MINOR (one transaction) and validates
    // compatibility — the same probe PsbModbusClient::connect() used to run
    // inline. Callers decide when to run it: PsbModbusClient's facade runs
    // it once right after construction (mirroring today's connect()
    // contract); a future topology scan can run it repeatedly against
    // different candidate slave IDs on an already-connected bus without
    // reconnecting anything.
    bool verifyProtocol();
    void disconnect();
    bool isConnected() const;
    std::string lastError() const;
    int slaveId() const;
    int16_t currentUnitExp() const;

    // Direct test mode — inject register arrays (bypasses Modbus RTU),
    // scoped to this session's slave ID on its bus. Unlike verifyProtocol(),
    // this marks the session connected immediately without probing —
    // matches PsbModbusClient::attachTestArrays()'s existing contract.
    void attachTestArrays(uint16_t* inputRegs, uint16_t* holdingRegs, int maxAddr);
    void detachTestArrays();

    // High-level reads (raw LSB values)
    SystemInfo        readSystemInfo();
    ChannelInfo       readChannelInfo(int ch, uint16_t caps = 0);
    SystemConfig      readSystemConfig();
    ChannelConfig     readChannelConfig(int ch, uint16_t caps = 0);
    ChannelCalConfig  readChannelCalConfig(int ch, uint16_t caps = 0);

    bool readSystemStatus(SystemInfo& info, int timeoutOverrideMs = -1);
    bool readChannelStatus(int ch, uint16_t caps, ChannelInfo& info, int timeoutOverrideMs = -1);
    bool readChannelCapabilities(int ch, uint16_t& caps, int timeoutOverrideMs = -1);

    void readChannelConfig(int ch, uint16_t caps, ChannelConfig& out);
    void readChannelCalConfig(int ch, uint16_t caps, ChannelCalConfig& out);
    void readSystemInfo(SystemInfo& out);
    void readChannelInfo(int ch, uint16_t caps, ChannelInfo& out);

    void readChannelOutputBlock(int ch, uint16_t caps, ChannelConfig& out);
    void readChannelRecoveryBlock(int ch, ChannelConfig& out);
    void readChannelProtectionBlock(int ch, uint16_t caps, ChannelConfig& out);
    void readChannelDerateBlock(int ch, uint16_t caps, ChannelConfig& out);
    void readChannelOutputEnabledBlock(int ch, uint16_t caps, ChannelConfig& out);

    // High-level writes — system
    bool writeOperatingMode(OpMode mode);
    bool writeStartupChannelPolicy(uint16_t policy);
    bool writeSlaveAddress(uint16_t addr);
    bool writeBaudRateCode(uint16_t code);
    bool sendParamAction(int chScope, ParamAction action);

    // High-level writes — channel
    bool writeConfiguredTargetVoltage(int ch, int16_t raw);
    bool writeOutputEnabled(int ch, bool enabled);
    bool sendOutputAction(int ch, OutputAction action);
    bool sendChannelFaultCommand(int ch, ChannelFaultCommand cmd);
    bool writeRampUp(int ch, uint16_t stepRaw, uint16_t interval);
    bool writeRampDown(int ch, uint16_t stepRaw, uint16_t interval);
    bool writeChannelRecovery(int ch, RecoveryPolicy policy, int delay, int max, int window);
    bool writeChannelSafeBand(int ch, uint16_t pct);
    bool writeCurrentProtection(int ch, ProtectionMode mode, OutputAction action, int16_t thresholdRaw);
    bool writeDerateStep(int ch, uint16_t stepRaw);
    bool writeCalibrationOutput(int ch, uint16_t k, int16_t b);
    bool writeCalibrationMeasV(int ch, uint16_t k, int16_t b);
    bool writeCalibrationMeasI(int ch, uint16_t k, int16_t b);
    bool writeCalibrationOutputExp(int ch, int16_t exp);
    bool writeCalibrationMeasVExp(int ch, int16_t exp);
    bool writeCalibrationMeasIExp(int ch, int16_t exp);

    // Calibration Mode operations (v2.1)
    bool unlockCalibrationStep(uint16_t value);
    bool enterCalibrationMode();
    bool exitCalibrationMode();
    bool writeCalibrationOutputEnable(int ch, bool enable);
    bool writeRawDacCode(int ch, uint16_t code);
    bool sendCalibrationSampleCommand(int ch);
    bool sendCalibrationCommitCommand(int ch);
    CalibrationSnapshot readCalibrationSnapshot(int ch);

    // Low-level
    bool readInputRegs(uint16_t addr, uint16_t count, uint16_t* out);
    bool readHoldingRegs(uint16_t addr, uint16_t count, uint16_t* out);
    bool writeReg16(uint16_t addr, uint16_t value);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    bool readRegsInternal(bool holding, uint16_t addr, uint16_t count, uint16_t* out,
                          int timeoutOverrideMs = -1);
    bool writeRegsInternal(uint16_t addr, uint16_t count, const uint16_t* values,
                            int timeoutOverrideMs = -1);
    bool checkConnected();
};

} // namespace psb
```

- [ ] **Step 5: Create `psb_board_session.cpp` from `psb_modbus_client.cpp`**

Run:
```bash
cp tools/psb_modbus_core/psb_modbus_client.cpp tools/psb_modbus_core/psb_board_session.cpp
```

Now apply the following four structural edits to `tools/psb_modbus_core/psb_board_session.cpp` (the ~600 lines of high-level read/write method bodies between them are untouched — they only call `checkConnected()`/`readRegsInternal()`/`writeRegsInternal()`, all reimplemented below, never `m_impl->port` directly).

**Edit A — replace includes, `Impl`, constructor/destructor** (find this exact block near the top of the file and replace it):

Old:
```cpp
#include "psb_modbus_client.h"
#include "register_map.h"

#include <ModbusClientPort.h>
#include <ModbusPort.h>
#include <Modbus.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <sstream>
#include <thread>

namespace psb {

// The non-blocking port's request state machine (ModbusClientPort::process())
// is explicitly designed to be pumped from an external timer/event loop —
// it returns Status_Processing immediately while waiting for I/O rather than
// blocking (that's the whole point of non-blocking mode; see its STATE_TIMEOUT
// case, which only sleeps in blocking mode). Driving it with a zero-delay
// do/while spin — as a naive port of the intended "poll until done" pattern
// would — pegs a CPU core at ~100% for the full duration of every single
// transaction. Measured live: this doesn't just waste CPU, it visibly
// degrades real transaction latency under sustained continuous polling
// (confirmed by comparing against mbpoll's much faster, non-spinning
// isolated reads on the same board/cable) — almost certainly by starving
// the kernel's USB-serial interrupt servicing of CPU time on whatever core
// the spin lands on. This tiny yield between checks is cheap next to a
// Modbus RTU round-trip (tens of ms) and eliminates the spin entirely.
static constexpr auto kNonBlockingPollInterval = std::chrono::milliseconds(1);

struct PsbModbusClient::Impl {
    ModbusClientPort* port = nullptr;
    int slaveId = 1;
    bool connected = false;
    std::string errorText;
    FrameCallback frameCb;
    int16_t currentUnitExp = -10;  // see PsbModbusClient::currentUnitExp()

    // Test mode — direct array access (bypasses Modbus RTU)
    uint16_t* testInputRegs = nullptr;
    uint16_t* testHoldingRegs = nullptr;
    int testMaxAddr = 0;

    ~Impl() { disconnect(); }
    void disconnect() {
        if (port) { delete port; port = nullptr; }
        connected = false;
    }

    // ModbusClientPort's signalTx/signalRx are the library's own raw-frame
    // hooks (see ModbusClientPort.h) — wired here so setFrameCallback()
    // actually receives data. Previously frameCb was stored but nothing
    // ever invoked it, so the GUI's raw-log/debug panel was always empty.
    void onTx(const Modbus::Char*, const uint8_t* buff, uint16_t size) {
        if (frameCb) frameCb(true, std::vector<uint8_t>(buff, buff + size));
    }
    void onRx(const Modbus::Char*, const uint8_t* buff, uint16_t size) {
        if (frameCb) frameCb(false, std::vector<uint8_t>(buff, buff + size));
    }
};

PsbModbusClient::PsbModbusClient() : m_impl(std::make_unique<Impl>()) {}
PsbModbusClient::~PsbModbusClient() = default;
```

New:
```cpp
#include "psb_board_session.h"
#include "psb_serial_bus.h"
#include "register_map.h"

namespace psb {

struct PsbBoardSession::Impl {
    std::shared_ptr<PsbSerialBus> bus;
    int slaveId = 1;
    bool verified = false;
    std::string errorText;
    int16_t currentUnitExp = -10;  // see PsbBoardSession::currentUnitExp()
};

PsbBoardSession::PsbBoardSession(std::shared_ptr<PsbSerialBus> bus, int slaveId)
    : m_impl(std::make_unique<Impl>()) {
    m_impl->bus = std::move(bus);
    m_impl->slaveId = slaveId;
}
PsbBoardSession::~PsbBoardSession() = default;
```

**Edit B — replace `connect()` through `checkConnected()`** (find this exact block and replace it — it spans from `bool PsbModbusClient::connect` through the closing brace of `checkConnected()`):

Old:
```cpp
bool PsbModbusClient::connect(const std::string& portName, int baud, int slaveId, int timeoutMs) {
    m_impl->disconnect();
    m_impl->slaveId = slaveId;

    Modbus::SerialSettings settings;
    settings.portName = portName.c_str();
    settings.baudRate = baud;
    settings.dataBits = 8;
    settings.parity = Modbus::NoParity;
    settings.stopBits = Modbus::OneStop;
    settings.flowControl = Modbus::NoFlowControl;
    settings.timeoutFirstByte = static_cast<uint32_t>(timeoutMs);
    // timeoutInterByte governs how long ModbusLib waits for each subsequent
    // byte within an already-started response before concluding the frame is
    // complete — a property of the serial link and adapter, unrelated to
    // timeoutFirstByte (how long to wait for a response to even begin) and
    // must not be derived from it. The previous timeoutMs/10 formula happened
    // to produce 300ms at this class's typical 3000ms caller default (the
    // TUI) — confirmed via syscall-level tracing that EVERY transaction, not
    // just failures, paid this in full (measured ~306-308ms per transaction,
    // matching 3000/10 almost exactly), since the library waits out the full
    // inter-byte timeout after the last byte arrives before returning, even
    // for an already-complete, healthy response. This is why the CLI's
    // commands (default timeoutMs=500 -> 50ms) looked fine while the TUI's
    // (3000ms -> 300ms) was consistently ~6x slower than a healthy transaction
    // needs to be. 50ms is exactly what the CLI's default timeoutMs=500 has
    // always produced via this same formula — already a proven-safe value in
    // this codebase, not a new guess — with generous margin over both the
    // Modbus RTU spec's frame-silence threshold (~0.3ms at 115200 baud) and
    // the several-ms USB-serial adapter buffering/latency-timer behavior
    // (e.g. CH340) that non-blocking mode exists to tolerate (see the
    // mode-choice comment below).
    settings.timeoutInterByte = 50;

    // Non-blocking mode: engages ModbusLib's inter-byte accumulation loop
    // (nonBlockingRead). The blocking path does a single ::read() and treats a
    // partial frame as complete, which truncates responses >~31 bytes (>=14
    // registers) over USB-serial and yields spurious CRC errors. Our read/write
    // wrappers poll until the request stops returning Status_Processing.
    m_impl->port = Modbus::createClientPort(Modbus::RTU, &settings, false);
    if (!m_impl->port) {
        m_impl->errorText = "failed to create RTU client port";
        return false;
    }
    m_impl->port->connect(&ModbusClientPort::signalTx, m_impl.get(), &Impl::onTx);
    m_impl->port->connect(&ModbusClientPort::signalRx, m_impl.get(), &Impl::onRx);

    // Mark connected so readRegsInternal allows the probe transaction.
    m_impl->connected = true;

    // Probe: verify the board actually responds before claiming success.
    // Read PROTOCOL_MAJOR/MINOR (input offsets 0-1) in one transaction.
    uint16_t probe[2] = {0xFFFF, 0xFFFF};
    if (!readRegsInternal(false, reg::sysAddr(0), 2, probe)) {
        m_impl->errorText = "no response from board — check baud rate, slave ID, and cabling";
        m_impl->disconnect();
        return false;
    }
    int protoMajor = static_cast<int>(probe[0]);
    int protoMinor = static_cast<int>(probe[1]);
    if (protoMajor < 1 || protoMajor > 15) {
        m_impl->errorText = "unexpected protocol version " + std::to_string(protoMajor)
                            + " — check baud rate and slave ID";
        m_impl->disconnect();
        return false;
    }
    // Real compatibility gate (design spec §6): refuse to operate, not just
    // warn, on a protocol mismatch — an exact major mismatch means this
    // client cannot correctly speak the wire format at all, and a lower
    // minor means the firmware predates a register this client may rely on.
    if (!reg::protocolCompatible(protoMajor, protoMinor)) {
        m_impl->errorText = "firmware protocol v" + std::to_string(protoMajor) + "."
                            + std::to_string(protoMinor) + " is incompatible with this tool "
                            + "(requires v" + std::to_string(VC_PROTOCOL_MAJOR) + "."
                            + std::to_string(VC_PROTOCOL_MINOR) + " or newer, same major version)";
        m_impl->disconnect();
        return false;
    }

    return true;
}

void PsbModbusClient::disconnect()    { m_impl->disconnect(); }
bool PsbModbusClient::isConnected() const { return m_impl->connected || m_impl->testInputRegs; }
std::string PsbModbusClient::lastError() const { return m_impl->errorText; }
int PsbModbusClient::slaveId() const { return m_impl->slaveId; }
int16_t PsbModbusClient::currentUnitExp() const { return m_impl->currentUnitExp; }

void PsbModbusClient::attachTestArrays(uint16_t* inputRegs, uint16_t* holdingRegs, int maxAddr) {
    m_impl->testInputRegs = inputRegs;
    m_impl->testHoldingRegs = holdingRegs;
    m_impl->testMaxAddr = maxAddr;
    m_impl->connected = true;
}

void PsbModbusClient::detachTestArrays() {
    m_impl->testInputRegs = nullptr;
    m_impl->testHoldingRegs = nullptr;
    m_impl->testMaxAddr = 0;
    m_impl->connected = false;
}

// ============================================================================
//  Internal
// ============================================================================

bool PsbModbusClient::checkConnected() {
    if (m_impl->testInputRegs || m_impl->testHoldingRegs) return true;
    if (!m_impl->connected || !m_impl->port) {
        m_impl->errorText = "not connected";
        return false;
    }
    return true;
}
```

New:
```cpp
// Reads PROTOCOL_MAJOR/MINOR (input offsets 0-1) in one transaction and
// validates compatibility — moved here verbatim from
// PsbModbusClient::connect(), which used to run this inline right after
// opening the port. Now decoupled from opening the port at all: the bus is
// already connected (or in test mode) by the time this runs.
bool PsbBoardSession::verifyProtocol() {
    uint16_t probe[2] = {0xFFFF, 0xFFFF};
    if (!readRegsInternal(false, reg::sysAddr(0), 2, probe)) {
        m_impl->errorText = "no response from board — check baud rate, slave ID, and cabling";
        m_impl->verified = false;
        return false;
    }
    int protoMajor = static_cast<int>(probe[0]);
    int protoMinor = static_cast<int>(probe[1]);
    if (protoMajor < 1 || protoMajor > 15) {
        m_impl->errorText = "unexpected protocol version " + std::to_string(protoMajor)
                            + " — check baud rate and slave ID";
        m_impl->verified = false;
        return false;
    }
    // Real compatibility gate (design spec §6): refuse to operate, not just
    // warn, on a protocol mismatch — an exact major mismatch means this
    // client cannot correctly speak the wire format at all, and a lower
    // minor means the firmware predates a register this client may rely on.
    if (!reg::protocolCompatible(protoMajor, protoMinor)) {
        m_impl->errorText = "firmware protocol v" + std::to_string(protoMajor) + "."
                            + std::to_string(protoMinor) + " is incompatible with this tool "
                            + "(requires v" + std::to_string(VC_PROTOCOL_MAJOR) + "."
                            + std::to_string(VC_PROTOCOL_MINOR) + " or newer, same major version)";
        m_impl->verified = false;
        return false;
    }
    m_impl->verified = true;
    return true;
}

void PsbBoardSession::disconnect() { m_impl->verified = false; }
bool PsbBoardSession::isConnected() const { return m_impl->verified; }
std::string PsbBoardSession::lastError() const { return m_impl->errorText; }
int PsbBoardSession::slaveId() const { return m_impl->slaveId; }
int16_t PsbBoardSession::currentUnitExp() const { return m_impl->currentUnitExp; }

// Bypasses verifyProtocol() entirely, matching PsbModbusClient's existing
// contract (attachTestArrays() has always marked a client connected
// immediately, with no probe).
void PsbBoardSession::attachTestArrays(uint16_t* inputRegs, uint16_t* holdingRegs, int maxAddr) {
    m_impl->bus->attachTestArrays(m_impl->slaveId, inputRegs, holdingRegs, maxAddr);
    m_impl->verified = true;
}

void PsbBoardSession::detachTestArrays() {
    m_impl->bus->detachTestArrays(m_impl->slaveId);
    m_impl->verified = false;
}

// ============================================================================
//  Internal
// ============================================================================

// A cheap early bail-out before attempting a doomed transaction — not the
// only safety net. readRegsInternal/writeRegsInternal below still fail
// cleanly (via the bus's own "not connected" check) even without this,
// since they always go through m_impl->bus regardless. The one imprecision
// here — this checks *the bus's* connectivity, not specifically whether
// *this session's* slave ID has anything listening — is harmless: an
// unconfigured session sharing a bus with a verified one would still get a
// correct "not connected" a layer deeper, from the bus's own per-slave-ID
// lookup finding nothing.
bool PsbBoardSession::checkConnected() {
    if (!m_impl->bus || !m_impl->bus->isConnected()) {
        m_impl->errorText = "not connected";
        return false;
    }
    return true;
}
```

**Edit C — replace `ScopedPortTimeout` + `readRegsInternal` + `writeRegsInternal`** (find this exact block and replace it):

Old:
```cpp
namespace {
// RAII helper: temporarily overrides a ModbusPort's response timeout for the
// duration of a single request, restoring the previous value on scope exit
// (including early returns). No-op if port is null or override is negative.
class ScopedPortTimeout {
public:
    ScopedPortTimeout(ModbusPort* port, int overrideMs) : m_port(nullptr) {
        if (port && overrideMs >= 0) {
            m_port = port;
            m_saved = m_port->timeout();
            m_port->setTimeout(static_cast<uint32_t>(overrideMs));
        }
    }
    ~ScopedPortTimeout() {
        if (m_port) m_port->setTimeout(m_saved);
    }
    ScopedPortTimeout(const ScopedPortTimeout&) = delete;
    ScopedPortTimeout& operator=(const ScopedPortTimeout&) = delete;

private:
    ModbusPort* m_port;
    uint32_t m_saved = 0;
};
} // namespace

bool PsbModbusClient::readRegsInternal(bool holding, uint16_t addr, uint16_t count, uint16_t* out,
                                       int timeoutOverrideMs) {
    // Test mode — direct array access
    if (m_impl->testInputRegs || m_impl->testHoldingRegs) {
        uint16_t* src = holding ? m_impl->testHoldingRegs : m_impl->testInputRegs;
        if (!src || addr + count > static_cast<uint16_t>(m_impl->testMaxAddr)) {
            m_impl->errorText = "test: address out of range";
            return false;
        }
        memcpy(out, src + addr, count * sizeof(uint16_t));
        return true;
    }
    if (!checkConnected()) return false;
    ScopedPortTimeout timeoutGuard(m_impl->port ? m_impl->port->port() : nullptr, timeoutOverrideMs);
    Modbus::StatusCode s;
    auto unit = static_cast<uint8_t>(m_impl->slaveId);
    // Non-blocking port: drive the request to completion. ModbusLib's internal
    // timeout/retry logic guarantees this terminates (Good or Bad). The sleep
    // is required, not cosmetic — see kNonBlockingPollInterval's comment.
    do {
        if (holding)
            s = m_impl->port->readHoldingRegisters(unit, addr, count, out);
        else
            s = m_impl->port->readInputRegisters(unit, addr, count, out);
        if (Modbus::StatusIsProcessing(s)) std::this_thread::sleep_for(kNonBlockingPollInterval);
    } while (Modbus::StatusIsProcessing(s));
    if (!Modbus::StatusIsGood(s)) {
        m_impl->errorText = std::string("Modbus error: ") + m_impl->port->lastErrorText();
        return false;
    }
    return true;
}

bool PsbModbusClient::writeRegsInternal(uint16_t addr, uint16_t count, const uint16_t* values,
                                         int timeoutOverrideMs) {
    // Test mode — direct array access
    if (m_impl->testHoldingRegs) {
        if (addr + count > static_cast<uint16_t>(m_impl->testMaxAddr)) {
            m_impl->errorText = "test: address out of range";
            return false;
        }
        memcpy(m_impl->testHoldingRegs + addr, values, count * sizeof(uint16_t));
        return true;
    }
    if (!checkConnected()) return false;
    ScopedPortTimeout timeoutGuard(m_impl->port ? m_impl->port->port() : nullptr, timeoutOverrideMs);
    auto unit = static_cast<uint8_t>(m_impl->slaveId);
    for (uint16_t i = 0; i < count; i++) {
        // Non-blocking port: poll until the write request completes. The
        // sleep is required, not cosmetic — see kNonBlockingPollInterval's
        // comment.
        Modbus::StatusCode s;
        do {
            s = m_impl->port->writeSingleRegister(unit, addr + i, values[i]);
            if (Modbus::StatusIsProcessing(s)) std::this_thread::sleep_for(kNonBlockingPollInterval);
        } while (Modbus::StatusIsProcessing(s));
        if (!Modbus::StatusIsGood(s)) {
            m_impl->errorText = std::string("Modbus error: ") + m_impl->port->lastErrorText();
            return false;
        }
    }
    return true;
}
```

New:
```cpp
bool PsbBoardSession::readRegsInternal(bool holding, uint16_t addr, uint16_t count, uint16_t* out,
                                       int timeoutOverrideMs) {
    if (!checkConnected()) return false;
    bool ok = holding
        ? m_impl->bus->readHoldingRegs(m_impl->slaveId, addr, count, out, timeoutOverrideMs)
        : m_impl->bus->readInputRegs(m_impl->slaveId, addr, count, out, timeoutOverrideMs);
    if (!ok) m_impl->errorText = m_impl->bus->lastError();
    return ok;
}

bool PsbBoardSession::writeRegsInternal(uint16_t addr, uint16_t count, const uint16_t* values,
                                        int timeoutOverrideMs) {
    if (!checkConnected()) return false;
    bool ok = m_impl->bus->writeRegs(m_impl->slaveId, addr, count, values, timeoutOverrideMs);
    if (!ok) m_impl->errorText = m_impl->bus->lastError();
    return ok;
}
```

**Edit D — delete `setFrameCallback`/`scanPorts`/`availableBaudRates`** (these are bus-level only; find this exact block near the end of the file and replace it):

Old:
```cpp
void PsbModbusClient::setFrameCallback(FrameCallback cb) {
    m_impl->frameCb = std::move(cb);
}

std::vector<std::string> PsbModbusClient::scanPorts() {
    std::vector<std::string> result;
    for (const auto& p : Modbus::availableSerialPorts()) {
        std::string path(p);
#if defined(_WIN32)
        if (path.rfind("COM", 0) == 0)
            result.push_back(path);
#else
        /* ttyUSB: USB-to-serial adapters (FTDI, CH340, etc.) using a
         * separate UART chip. ttyACM: USB CDC-ACM devices — boards with
         * native USB, and multi-port USB-serial adapters (e.g. WCH
         * "Quad_Serial") that enumerate as ACM rather than USB. Both are
         * real external serial links worth listing; ttyS* (onboard,
         * non-USB serial) stays excluded. */
        if (path.rfind("/dev/ttyUSB", 0) == 0 || path.rfind("/dev/ttyACM", 0) == 0)
            result.push_back(path);
#endif
    }
    return result;
}

std::vector<int> PsbModbusClient::availableBaudRates() {
    std::vector<int> result;
    for (auto r : Modbus::availableBaudRate())
        result.push_back(static_cast<int>(r));
    return result;
}

} // namespace psb
```

New:
```cpp
} // namespace psb
```

- [ ] **Step 6: Rename remaining `PsbModbusClient::` qualifiers to `PsbBoardSession::`**

The four edits above already handled every method whose *implementation* changed (including the constructor/destructor, which needed hand renaming since `PsbModbusClient::PsbModbusClient()`'s second occurrence of the class name is the constructor name itself, not a qualifier a blanket rename would catch correctly). Every other method — the ~600 unchanged lines — still reads `PsbModbusClient::methodName(...)`. Run:

```bash
sed -i 's/PsbModbusClient::/PsbBoardSession::/g' tools/psb_modbus_core/psb_board_session.cpp
```

- [ ] **Step 7: Register the new source file**

Edit `tools/psb_modbus_core/CMakeLists.txt` — add `psb_board_session.cpp` to the `psb_modbus_core` library source list.

- [ ] **Step 8: Build and run the new tests**

Run: `cd tools && cmake --build build --target psb_tests && ./build/psb_modbus_core/tests/psb_tests "[board_session]"`
Expected: PASS — 5 test cases, all assertions green. If it fails to compile, the most likely cause is a stray `m_impl->port`/`m_impl->slaveId`/`m_impl->testInputRegs` reference the sed pass didn't touch (those field names no longer exist on the new `Impl`) — grep the file for `m_impl->port` to confirm none remain outside what Edits A-D already handled.

- [ ] **Step 9: Commit**

```bash
git add tools/psb_modbus_core/psb_board_session.h tools/psb_modbus_core/psb_board_session.cpp \
        tools/psb_modbus_core/tests/test_board_session.cpp \
        tools/psb_modbus_core/CMakeLists.txt tools/psb_modbus_core/tests/CMakeLists.txt
git commit -m "feat(psb_modbus_core): add PsbBoardSession — per-board handle on a PsbSerialBus"
```

---

## Task 3: `PsbModbusClient` — rewrite as a thin facade

**Files:**
- Modify: `tools/psb_modbus_core/psb_modbus_client.cpp` (full replacement — `psb_modbus_client.h` is **not** touched, per the Global Constraints)

**Interfaces:**
- Consumes: `psb::PsbSerialBus` (Task 1), `psb::PsbBoardSession` (Task 2).
- Produces: nothing new — `PsbModbusClient`'s existing public API, unchanged, now backed by a bus+session pair. This is the regression gate for Tasks 1-2: every existing test in `psb_tests` must pass against this new implementation without modification.

A subtlety worth getting right up front: `m_impl->session` must **never** be null once the object is constructed (create it eagerly in the constructor, not lazily in `connect()`), or any high-level method called on a never-connected, never-`attachTestArrays()`'d client would dereference a null `unique_ptr` and crash — where today's original code instead safely returns a default value via `checkConnected()`'s early return. `test_error_handling.cpp`'s first test does exactly this (constructs a client, calls `detachTestArrays()`, then `readSystemInfo()`, with no `connect()`/`attachTestArrays()` ever called) — it will crash if this isn't handled.

- [ ] **Step 1: Confirm the existing suite passes before touching anything (baseline)**

Run: `cd tools && cmake --build build --target psb_tests && ./build/psb_modbus_core/tests/psb_tests`
Expected: PASS — same assertion count as before Tasks 1-2 (Tasks 1-2 only added new test files under new `[serial_bus]`/`[board_session]` tags; nothing about the existing suite should have changed yet, since `psb_modbus_client.cpp` hasn't been touched).

- [ ] **Step 2: Replace `tools/psb_modbus_core/psb_modbus_client.cpp` in full**

```cpp
#include "psb_modbus_client.h"
#include "psb_serial_bus.h"
#include "psb_board_session.h"

namespace psb {

struct PsbModbusClient::Impl {
    std::shared_ptr<PsbSerialBus> bus;
    std::unique_ptr<PsbBoardSession> session;
};

PsbModbusClient::PsbModbusClient() : m_impl(std::make_unique<Impl>()) {
    m_impl->bus = std::make_shared<PsbSerialBus>();
    // Created eagerly, not lazily in connect() — every high-level method
    // below forwards to m_impl->session unconditionally, so it must never
    // be null. A freshly-constructed, never-connected client behaves
    // exactly like today's: every read/write fails cleanly via
    // PsbBoardSession::checkConnected() rather than crashing.
    m_impl->session = std::make_unique<PsbBoardSession>(m_impl->bus, 1);
}
PsbModbusClient::~PsbModbusClient() = default;

// ============================================================================
//  Connection
// ============================================================================

bool PsbModbusClient::connect(const std::string& portName, int baud, int slaveId, int timeoutMs) {
    m_impl->session->disconnect();
    if (!m_impl->bus->connect(portName, baud, timeoutMs)) {
        // Bus-level failure (e.g. "failed to create RTU client port") — no
        // probe was ever attempted, so replace the session (picking up the
        // requested slaveId for a subsequent retry) but don't touch its
        // verified state; lastError() below surfaces the bus's error since
        // the bus itself isn't connected.
        m_impl->session = std::make_unique<PsbBoardSession>(m_impl->bus, slaveId);
        return false;
    }
    m_impl->session = std::make_unique<PsbBoardSession>(m_impl->bus, slaveId);
    if (!m_impl->session->verifyProtocol()) {
        m_impl->bus->disconnect();
        return false;
    }
    return true;
}

void PsbModbusClient::disconnect() {
    m_impl->bus->disconnect();
    m_impl->session->disconnect();
}
bool PsbModbusClient::isConnected() const { return m_impl->session->isConnected(); }
std::string PsbModbusClient::lastError() const {
    // A bus-open failure (before any session ever existed to record its own
    // error) must surface here too, not just a session-level verify
    // failure — see connect()'s early-return path above.
    if (!m_impl->bus->isConnected()) {
        auto busErr = m_impl->bus->lastError();
        if (!busErr.empty()) return busErr;
    }
    return m_impl->session->lastError();
}
int PsbModbusClient::slaveId() const { return m_impl->session->slaveId(); }
int16_t PsbModbusClient::currentUnitExp() const { return m_impl->session->currentUnitExp(); }

void PsbModbusClient::attachTestArrays(uint16_t* inputRegs, uint16_t* holdingRegs, int maxAddr) {
    m_impl->session->attachTestArrays(inputRegs, holdingRegs, maxAddr);
}
void PsbModbusClient::detachTestArrays() {
    m_impl->session->detachTestArrays();
}

// ============================================================================
//  High-level reads — forwarded to the active board session
// ============================================================================

SystemInfo PsbModbusClient::readSystemInfo() { return m_impl->session->readSystemInfo(); }
void PsbModbusClient::readSystemInfo(SystemInfo& out) { m_impl->session->readSystemInfo(out); }

ChannelInfo PsbModbusClient::readChannelInfo(int ch, uint16_t caps) { return m_impl->session->readChannelInfo(ch, caps); }
void PsbModbusClient::readChannelInfo(int ch, uint16_t caps, ChannelInfo& out) { m_impl->session->readChannelInfo(ch, caps, out); }

SystemConfig PsbModbusClient::readSystemConfig() { return m_impl->session->readSystemConfig(); }

ChannelConfig PsbModbusClient::readChannelConfig(int ch, uint16_t caps) { return m_impl->session->readChannelConfig(ch, caps); }
void PsbModbusClient::readChannelConfig(int ch, uint16_t caps, ChannelConfig& out) { m_impl->session->readChannelConfig(ch, caps, out); }

ChannelCalConfig PsbModbusClient::readChannelCalConfig(int ch, uint16_t caps) { return m_impl->session->readChannelCalConfig(ch, caps); }
void PsbModbusClient::readChannelCalConfig(int ch, uint16_t caps, ChannelCalConfig& out) { m_impl->session->readChannelCalConfig(ch, caps, out); }

bool PsbModbusClient::readSystemStatus(SystemInfo& info, int timeoutOverrideMs) { return m_impl->session->readSystemStatus(info, timeoutOverrideMs); }
bool PsbModbusClient::readChannelStatus(int ch, uint16_t caps, ChannelInfo& info, int timeoutOverrideMs) { return m_impl->session->readChannelStatus(ch, caps, info, timeoutOverrideMs); }
bool PsbModbusClient::readChannelCapabilities(int ch, uint16_t& caps, int timeoutOverrideMs) { return m_impl->session->readChannelCapabilities(ch, caps, timeoutOverrideMs); }

void PsbModbusClient::readChannelOutputBlock(int ch, uint16_t caps, ChannelConfig& out) { m_impl->session->readChannelOutputBlock(ch, caps, out); }
void PsbModbusClient::readChannelRecoveryBlock(int ch, ChannelConfig& out) { m_impl->session->readChannelRecoveryBlock(ch, out); }
void PsbModbusClient::readChannelProtectionBlock(int ch, uint16_t caps, ChannelConfig& out) { m_impl->session->readChannelProtectionBlock(ch, caps, out); }
void PsbModbusClient::readChannelDerateBlock(int ch, uint16_t caps, ChannelConfig& out) { m_impl->session->readChannelDerateBlock(ch, caps, out); }
void PsbModbusClient::readChannelOutputEnabledBlock(int ch, uint16_t caps, ChannelConfig& out) { m_impl->session->readChannelOutputEnabledBlock(ch, caps, out); }

// ============================================================================
//  High-level writes — forwarded to the active board session
// ============================================================================

bool PsbModbusClient::writeOperatingMode(OpMode mode) { return m_impl->session->writeOperatingMode(mode); }
bool PsbModbusClient::writeStartupChannelPolicy(uint16_t policy) { return m_impl->session->writeStartupChannelPolicy(policy); }
bool PsbModbusClient::writeSlaveAddress(uint16_t addr) { return m_impl->session->writeSlaveAddress(addr); }
bool PsbModbusClient::writeBaudRateCode(uint16_t code) { return m_impl->session->writeBaudRateCode(code); }
bool PsbModbusClient::sendParamAction(int chScope, ParamAction action) { return m_impl->session->sendParamAction(chScope, action); }

bool PsbModbusClient::writeConfiguredTargetVoltage(int ch, int16_t raw) { return m_impl->session->writeConfiguredTargetVoltage(ch, raw); }
bool PsbModbusClient::writeOutputEnabled(int ch, bool enabled) { return m_impl->session->writeOutputEnabled(ch, enabled); }
bool PsbModbusClient::sendOutputAction(int ch, OutputAction action) { return m_impl->session->sendOutputAction(ch, action); }
bool PsbModbusClient::sendChannelFaultCommand(int ch, ChannelFaultCommand cmd) { return m_impl->session->sendChannelFaultCommand(ch, cmd); }
bool PsbModbusClient::writeRampUp(int ch, uint16_t stepRaw, uint16_t interval) { return m_impl->session->writeRampUp(ch, stepRaw, interval); }
bool PsbModbusClient::writeRampDown(int ch, uint16_t stepRaw, uint16_t interval) { return m_impl->session->writeRampDown(ch, stepRaw, interval); }
bool PsbModbusClient::writeChannelRecovery(int ch, RecoveryPolicy policy, int delay, int max, int window) { return m_impl->session->writeChannelRecovery(ch, policy, delay, max, window); }
bool PsbModbusClient::writeChannelSafeBand(int ch, uint16_t pct) { return m_impl->session->writeChannelSafeBand(ch, pct); }
bool PsbModbusClient::writeCurrentProtection(int ch, ProtectionMode mode, OutputAction action, int16_t thresholdRaw) { return m_impl->session->writeCurrentProtection(ch, mode, action, thresholdRaw); }
bool PsbModbusClient::writeDerateStep(int ch, uint16_t stepRaw) { return m_impl->session->writeDerateStep(ch, stepRaw); }
bool PsbModbusClient::writeCalibrationOutput(int ch, uint16_t k, int16_t b) { return m_impl->session->writeCalibrationOutput(ch, k, b); }
bool PsbModbusClient::writeCalibrationMeasV(int ch, uint16_t k, int16_t b) { return m_impl->session->writeCalibrationMeasV(ch, k, b); }
bool PsbModbusClient::writeCalibrationMeasI(int ch, uint16_t k, int16_t b) { return m_impl->session->writeCalibrationMeasI(ch, k, b); }
bool PsbModbusClient::writeCalibrationOutputExp(int ch, int16_t exp) { return m_impl->session->writeCalibrationOutputExp(ch, exp); }
bool PsbModbusClient::writeCalibrationMeasVExp(int ch, int16_t exp) { return m_impl->session->writeCalibrationMeasVExp(ch, exp); }
bool PsbModbusClient::writeCalibrationMeasIExp(int ch, int16_t exp) { return m_impl->session->writeCalibrationMeasIExp(ch, exp); }

// ============================================================================
//  Calibration Mode operations (v2.1)
// ============================================================================

bool PsbModbusClient::unlockCalibrationStep(uint16_t value) { return m_impl->session->unlockCalibrationStep(value); }
bool PsbModbusClient::enterCalibrationMode() { return m_impl->session->enterCalibrationMode(); }
bool PsbModbusClient::exitCalibrationMode() { return m_impl->session->exitCalibrationMode(); }
bool PsbModbusClient::writeCalibrationOutputEnable(int ch, bool enable) { return m_impl->session->writeCalibrationOutputEnable(ch, enable); }
bool PsbModbusClient::writeRawDacCode(int ch, uint16_t code) { return m_impl->session->writeRawDacCode(ch, code); }
bool PsbModbusClient::sendCalibrationSampleCommand(int ch) { return m_impl->session->sendCalibrationSampleCommand(ch); }
bool PsbModbusClient::sendCalibrationCommitCommand(int ch) { return m_impl->session->sendCalibrationCommitCommand(ch); }
CalibrationSnapshot PsbModbusClient::readCalibrationSnapshot(int ch) { return m_impl->session->readCalibrationSnapshot(ch); }

// ============================================================================
//  Low-level / misc
// ============================================================================

bool PsbModbusClient::readInputRegs(uint16_t addr, uint16_t count, uint16_t* out) { return m_impl->session->readInputRegs(addr, count, out); }
bool PsbModbusClient::readHoldingRegs(uint16_t addr, uint16_t count, uint16_t* out) { return m_impl->session->readHoldingRegs(addr, count, out); }
bool PsbModbusClient::writeReg16(uint16_t addr, uint16_t value) { return m_impl->session->writeReg16(addr, value); }

void PsbModbusClient::setFrameCallback(FrameCallback cb) {
    // The bus's callback is tagged with slaveId (multiple boards can share
    // one bus); PsbModbusClient's single-board API predates that and never
    // needed it, so this adapter just drops the tag.
    m_impl->bus->setFrameCallback([cb](int /*slaveId*/, bool tx, const std::vector<uint8_t>& data) {
        if (cb) cb(tx, data);
    });
}

std::vector<std::string> PsbModbusClient::scanPorts() { return PsbSerialBus::scanPorts(); }
std::vector<int> PsbModbusClient::availableBaudRates() { return PsbSerialBus::availableBaudRates(); }

} // namespace psb
```

- [ ] **Step 3: Build**

Run: `cd tools && cmake --build build --target psb_tests 2>&1 | tail -60`
Expected: clean build. If it fails, the most likely cause is a signature mismatch between a forwarded call here and `PsbBoardSession`'s actual method signature (Task 2, Step 4) — compare the two headers side by side for the failing method.

- [ ] **Step 4: Run the full existing suite — this is the regression gate**

Run: `./build/psb_modbus_core/tests/psb_tests`
Expected: PASS, same total assertion count as the Step 1 baseline (every pre-existing test file — `test_connection.cpp`, `test_system_reads.cpp`, `test_channel_reads.cpp`, `test_writes.cpp`, `test_enum_validation.cpp`, `test_error_handling.cpp`, `test_monitor_output.cpp`, `test_tui_format.cpp`, `test_tui_policy.cpp`, `test_tui_modbus_settings.cpp`, `test_calibration.cpp` — plus the two new `[serial_bus]`/`[board_session]` files from Tasks 1-2). If any pre-existing test fails, do not patch the test — the bug is in the facade; re-check the specific behavior it exercises against `PsbModbusClient`'s original implementation (available via `git show HEAD:tools/psb_modbus_core/psb_modbus_client.cpp` from before this task's commit).

- [ ] **Step 5: Build and smoke-test `psb_demo_tui`, `psb_demo_cli` (unmodified call sites)**

Run: `cd tools && cmake --build build --target psb_demo_tui psb_demo_cli`
Expected: clean build — neither tool's source changes in this task, so this only proves `psb_modbus_client.h`'s public API truly didn't change shape.

- [ ] **Step 6: Commit**

```bash
git add tools/psb_modbus_core/psb_modbus_client.cpp
git commit -m "refactor(psb_modbus_core): PsbModbusClient becomes a facade over PsbSerialBus+PsbBoardSession"
```

---

## Task 4: `TopologyConfig` — TOML topology config, supersedes `ConfigManager`

**Files:**
- Create: `tools/psb_modbus_core/topology_config.h`
- Create: `tools/psb_modbus_core/topology_config.cpp`
- Create: `tools/psb_modbus_core/tests/test_topology_config.cpp`
- Delete: `tools/psb_modbus_core/config_manager.h`
- Delete: `tools/psb_modbus_core/config_manager.cpp`
- Modify: `tools/psb_modbus_core/CMakeLists.txt`
- Modify: `tools/psb_modbus_core/tests/CMakeLists.txt`

**Interfaces:**
- Produces: `psb::BoardConfig{nickname, slaveId}`, `psb::BusConfig{name, port, baudRate, boards}`, `psb::TopologyConfig{buses}` with `static std::optional<TopologyConfig> load(const std::string& path)`, `bool save(const std::string& path) const`, `static bool exists(const std::string& path)`, `static TopologyConfig singleBoard(const std::string& port, int baud, int slaveId, const std::string& nickname = "board1")`, `static std::string defaultPath()` (`~/.psb_demo_app/topology.toml`), `int totalBoardCount() const`.

This task doesn't touch `psb_demo_cli`/`psb_demo_tui` yet (Tasks 5-6) — it only removes `ConfigManager` from the library. Both tools currently `#include "config_manager.h"` and will fail to build after this task's deletion step until Tasks 5-6 land; that's expected and covered by Step 6 below (build them last, confirm the *specific* expected failure, don't try to make them pass yet).

- [ ] **Step 1: Write the failing tests**

Create `tools/psb_modbus_core/tests/test_topology_config.cpp`:

```cpp
#include "topology_config.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <fstream>

TEST_CASE("TopologyConfig — round trip through TOML preserves all fields", "[topology_config]") {
    psb::TopologyConfig cfg;
    psb::BusConfig bus1;
    bus1.name = "bus1";
    bus1.port = "/dev/ttyUSB0";
    bus1.baudRate = 115200;
    bus1.boards.push_back({"hvb-bench", 1});
    bus1.boards.push_back({"hvb-bench-2", 2});
    cfg.buses.push_back(bus1);

    psb::BusConfig bus2;
    bus2.name = "bus2";
    bus2.port = "/dev/ttyUSB1";
    bus2.baudRate = 9600;
    bus2.boards.push_back({"lvb-rack3", 1});
    cfg.buses.push_back(bus2);

    const std::string path = "/tmp/psb_topology_test_roundtrip.toml";
    std::remove(path.c_str());
    REQUIRE(cfg.save(path));

    auto loaded = psb::TopologyConfig::load(path);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->buses.size() == 2);

    CHECK(loaded->buses[0].name == "bus1");
    CHECK(loaded->buses[0].port == "/dev/ttyUSB0");
    CHECK(loaded->buses[0].baudRate == 115200);
    REQUIRE(loaded->buses[0].boards.size() == 2);
    CHECK(loaded->buses[0].boards[0].nickname == "hvb-bench");
    CHECK(loaded->buses[0].boards[0].slaveId == 1);
    CHECK(loaded->buses[0].boards[1].nickname == "hvb-bench-2");
    CHECK(loaded->buses[0].boards[1].slaveId == 2);

    CHECK(loaded->buses[1].name == "bus2");
    CHECK(loaded->buses[1].port == "/dev/ttyUSB1");
    CHECK(loaded->buses[1].baudRate == 9600);
    REQUIRE(loaded->buses[1].boards.size() == 1);
    CHECK(loaded->buses[1].boards[0].nickname == "lvb-rack3");

    CHECK(loaded->totalBoardCount() == 3);

    std::remove(path.c_str());
}

TEST_CASE("TopologyConfig — load returns nullopt for missing file", "[topology_config]") {
    auto loaded = psb::TopologyConfig::load("/tmp/psb_topology_test_does_not_exist.toml");
    CHECK_FALSE(loaded.has_value());
}

TEST_CASE("TopologyConfig — load returns nullopt for malformed TOML", "[topology_config]") {
    const std::string path = "/tmp/psb_topology_test_malformed.toml";
    { std::ofstream ofs(path); ofs << "this is [ not valid toml"; }
    auto loaded = psb::TopologyConfig::load(path);
    CHECK_FALSE(loaded.has_value());
    std::remove(path.c_str());
}

TEST_CASE("TopologyConfig — singleBoard helper builds a one-bus/one-board topology", "[topology_config]") {
    auto cfg = psb::TopologyConfig::singleBoard("/dev/ttyUSB0", 115200, 3, "quick-connect");
    REQUIRE(cfg.buses.size() == 1);
    REQUIRE(cfg.buses[0].boards.size() == 1);
    CHECK(cfg.buses[0].port == "/dev/ttyUSB0");
    CHECK(cfg.buses[0].boards[0].slaveId == 3);
    CHECK(cfg.buses[0].boards[0].nickname == "quick-connect");
    CHECK(cfg.totalBoardCount() == 1);
}

TEST_CASE("TopologyConfig — save creates parent directories that don't exist yet", "[topology_config]") {
    const std::string dir = "/tmp/psb_topology_test_newdir";
    const std::string path = dir + "/topology.toml";
    std::remove(path.c_str());
    std::remove(dir.c_str());

    auto cfg = psb::TopologyConfig::singleBoard("/dev/ttyUSB0", 115200, 1);
    REQUIRE(cfg.save(path));
    CHECK(psb::TopologyConfig::exists(path));

    std::remove(path.c_str());
    std::remove(dir.c_str());
}
```

- [ ] **Step 2: Register the new test file**

Edit `tools/psb_modbus_core/tests/CMakeLists.txt` — add `test_topology_config.cpp` to the `psb_tests` source list.

- [ ] **Step 3: Confirm it fails to build (header doesn't exist yet)**

Run: `cd tools && cmake --build build --target psb_tests`
Expected: FAIL — `fatal error: 'topology_config.h' file not found`

- [ ] **Step 4: Write `topology_config.h`**

```cpp
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace psb {

struct BoardConfig {
    std::string nickname;
    int slaveId = 1;
};

struct BusConfig {
    std::string name;
    std::string port;
    int baudRate = 115200;
    std::vector<BoardConfig> boards;
};

// Supersedes ConfigManager (~/.psb_demo_app.toml, single board/bus only).
// See docs/superpowers/specs/2026-07-20-multi-board-topology-design.md.
struct TopologyConfig {
    std::vector<BusConfig> buses;

    // Returns std::nullopt if the file doesn't exist, or fails to parse
    // (malformed TOML) — use exists() first if the caller needs to tell
    // those two cases apart (e.g. to decide whether to offer to create one
    // vs. report a parse error).
    static std::optional<TopologyConfig> load(const std::string& path);
    bool save(const std::string& path) const;
    static bool exists(const std::string& path);
    static TopologyConfig singleBoard(const std::string& port, int baud,
                                       int slaveId, const std::string& nickname = "board1");
    static std::string defaultPath();  // ~/.psb_demo_app/topology.toml
    int totalBoardCount() const;       // sum of boards across all buses
};

} // namespace psb
```

- [ ] **Step 5: Write `topology_config.cpp`**

```cpp
#include "topology_config.h"

#include <toml++/toml.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

#if defined(_WIN32)
#include <cstdlib>
#else
#include <unistd.h>
#include <pwd.h>
#endif

namespace psb {

static std::string homeDir() {
#if defined(_WIN32)
    const char* hd = std::getenv("USERPROFILE");
    return hd ? hd : ".";
#else
    const char* hd = std::getenv("HOME");
    if (hd) return hd;
    struct passwd* pw = getpwuid(getuid());
    return pw && pw->pw_dir ? pw->pw_dir : ".";
#endif
}

std::string TopologyConfig::defaultPath() {
    return homeDir() + "/.psb_demo_app/topology.toml";
}

bool TopologyConfig::exists(const std::string& path) {
    std::ifstream ifs(path);
    return ifs.good();
}

std::optional<TopologyConfig> TopologyConfig::load(const std::string& path) {
    if (!exists(path)) return std::nullopt;
    try {
        auto tbl = toml::parse_file(path);
        TopologyConfig cfg;
        auto busArr = tbl["bus"].as_array();
        if (!busArr) return std::nullopt;
        int busIndex = 0;
        for (auto&& busNode : *busArr) {
            ++busIndex;
            auto busTbl = busNode.as_table();
            if (!busTbl) continue;
            BusConfig bus;
            bus.name = (*busTbl)["name"].value_or(std::string("bus") + std::to_string(busIndex));
            bus.port = (*busTbl)["port"].value_or(std::string(""));
            bus.baudRate = static_cast<int>((*busTbl)["baud_rate"].value_or(115200));
            auto boardArr = (*busTbl)["board"].as_array();
            if (boardArr) {
                for (auto&& boardNode : *boardArr) {
                    auto boardTbl = boardNode.as_table();
                    if (!boardTbl) continue;
                    BoardConfig board;
                    board.nickname = (*boardTbl)["nickname"].value_or(std::string(""));
                    board.slaveId = static_cast<int>((*boardTbl)["slave_id"].value_or(1));
                    bus.boards.push_back(std::move(board));
                }
            }
            cfg.buses.push_back(std::move(bus));
        }
        return cfg;
    } catch (const std::exception& e) {
        std::cerr << "Topology config parse error (" << path << "): " << e.what() << "\n";
        return std::nullopt;
    }
}

bool TopologyConfig::save(const std::string& path) const {
    try {
        toml::table root;
        toml::array busArr;
        for (const auto& bus : buses) {
            toml::table busTbl;
            busTbl.insert_or_assign("name", bus.name);
            busTbl.insert_or_assign("port", bus.port);
            busTbl.insert_or_assign("baud_rate", bus.baudRate);
            toml::array boardArr;
            for (const auto& board : bus.boards) {
                toml::table boardTbl;
                boardTbl.insert_or_assign("nickname", board.nickname);
                boardTbl.insert_or_assign("slave_id", board.slaveId);
                boardArr.push_back(std::move(boardTbl));
            }
            busTbl.insert_or_assign("board", std::move(boardArr));
            busArr.push_back(std::move(busTbl));
        }
        root.insert_or_assign("bus", std::move(busArr));

        std::filesystem::path fsPath(path);
        if (fsPath.has_parent_path())
            std::filesystem::create_directories(fsPath.parent_path());

        std::ofstream ofs(path);
        if (!ofs) return false;
        ofs << root;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Topology config save error (" << path << "): " << e.what() << "\n";
        return false;
    }
}

TopologyConfig TopologyConfig::singleBoard(const std::string& port, int baud,
                                            int slaveId, const std::string& nickname) {
    TopologyConfig cfg;
    BusConfig bus;
    bus.name = "bus1";
    bus.port = port;
    bus.baudRate = baud;
    BoardConfig board;
    board.nickname = nickname;
    board.slaveId = slaveId;
    bus.boards.push_back(std::move(board));
    cfg.buses.push_back(std::move(bus));
    return cfg;
}

int TopologyConfig::totalBoardCount() const {
    int total = 0;
    for (const auto& bus : buses) total += static_cast<int>(bus.boards.size());
    return total;
}

} // namespace psb
```

- [ ] **Step 6: Register the new source file, build, run the new tests**

Edit `tools/psb_modbus_core/CMakeLists.txt` — add `topology_config.cpp` to the `psb_modbus_core` library source list (it already links `tomlplusplus::tomlplusplus`, used by the soon-to-be-removed `config_manager.cpp` — no new CMake dependency needed).

Run: `cd tools && cmake --build build --target psb_tests && ./build/psb_modbus_core/tests/psb_tests "[topology_config]"`
Expected: PASS — 5 test cases, all assertions green.

- [ ] **Step 7: Remove `ConfigManager`**

```bash
git rm tools/psb_modbus_core/config_manager.h tools/psb_modbus_core/config_manager.cpp
```

Edit `tools/psb_modbus_core/CMakeLists.txt` — remove `config_manager.cpp` from the `psb_modbus_core` library source list.

- [ ] **Step 8: Confirm the expected (temporary) breakage in the two tools**

Run: `cd tools && cmake --build build --target psb_demo_cli psb_demo_tui 2>&1 | grep -i "config_manager\|ConfigManager"`
Expected: both fail with `config_manager.h: No such file or directory` (from `cli/main.cpp:2` and `tui/main.cpp:2`'s `#include "config_manager.h"`) — this is expected and resolved by Tasks 5-6. Do **not** try to fix it in this task.

- [ ] **Step 9: Confirm `psb_tests` itself still builds and passes (it never depended on `ConfigManager`)**

Run: `cd tools && cmake --build build --target psb_tests && ./build/psb_modbus_core/tests/psb_tests`
Expected: PASS, same total assertion count as Task 3's Step 4 baseline plus the 5 new `[topology_config]` assertions' worth of test cases.

- [ ] **Step 10: Commit**

```bash
git add tools/psb_modbus_core/topology_config.h tools/psb_modbus_core/topology_config.cpp \
        tools/psb_modbus_core/tests/test_topology_config.cpp \
        tools/psb_modbus_core/CMakeLists.txt tools/psb_modbus_core/tests/CMakeLists.txt \
        tools/psb_modbus_core/config_manager.h tools/psb_modbus_core/config_manager.cpp
git commit -m "feat(psb_modbus_core): add TopologyConfig, remove ConfigManager

psb_demo_cli and psb_demo_tui intentionally fail to build until Tasks
5-6 replace their ConfigManager usage with TopologyConfig."
```

---

## Task 5: `psb_demo_cli` — `--topology`/`--board`, `--save` writes `TopologyConfig`

**Files:**
- Modify: `tools/psb_demo_app/cli/main.cpp`

**Interfaces:**
- Consumes: `psb::TopologyConfig` (Task 4) — `load`, `exists`, `singleBoard`, `defaultPath`, `totalBoardCount`; `psb::BoardConfig`/`psb::BusConfig` fields.

Implements the spec's CLI precedence rules 1-4 exactly (see
`docs/superpowers/specs/2026-07-20-multi-board-topology-design.md`'s CLI
flags section): `-p` wins outright; otherwise `--topology` (explicit or
defaulted) is tried, erroring only if the user *explicitly* named a path
that doesn't exist; if nothing resolves, `port` stays empty exactly like
today (commands that don't need a connection keep working; connection-needing
ones fail with the client's own "not connected" error, unchanged from today).

- [ ] **Step 1: Update includes**

Edit `tools/psb_demo_app/cli/main.cpp`:

Old:
```cpp
#include "psb_modbus_client.h"
#include "config_manager.h"
#include "register_map.h"
```

New:
```cpp
#include "psb_modbus_client.h"
#include "topology_config.h"
#include "register_map.h"
```

- [ ] **Step 2: Replace option setup at the top of `main()`**

Old:
```cpp
int main(int argc, char** argv) {
    CLI::App app{"PSB Demo App"};
    app.set_version_flag("--version", std::string("psb_demo_cli ") + TOOL_VERSION_STRING);
    psb::ConfigManager cfgMgr;
    cfgMgr.load();

    // ===================================================================
    //  ALL option variables MUST be declared here — nested {} scopes
    //  would cause stack-use-after-scope when CLI11 writes back values.
    // ===================================================================

    // Global
    std::string port;
    int baud = 115200, slaveId = 1, timeout = 500;
    bool save = false;
    app.add_option("-p,--port", port, "Serial port");
    app.add_option("-b,--baud", baud, "Baud rate")
        ->check(CLI::IsMember({9600, 19200, 38400, 115200}));
    app.add_option("-i,--id", slaveId, "Slave ID")->check(CLI::Range(0, 247));
    app.add_option("-t,--timeout", timeout, "Timeout ms");
    app.add_flag("--save", save, "Save connection to config");
```

New:
```cpp
int main(int argc, char** argv) {
    CLI::App app{"PSB Demo App"};
    app.set_version_flag("--version", std::string("psb_demo_cli ") + TOOL_VERSION_STRING);

    // ===================================================================
    //  ALL option variables MUST be declared here — nested {} scopes
    //  would cause stack-use-after-scope when CLI11 writes back values.
    // ===================================================================

    // Global
    std::string port;
    int baud = 115200, slaveId = 1, timeout = 500;
    bool save = false;
    std::string topologyPath = psb::TopologyConfig::defaultPath();
    std::string boardNickname;
    app.add_option("-p,--port", port, "Serial port (quick single-board connect, bypasses --topology)");
    app.add_option("-b,--baud", baud, "Baud rate")
        ->check(CLI::IsMember({9600, 19200, 38400, 115200}));
    app.add_option("-i,--id", slaveId, "Slave ID")->check(CLI::Range(0, 247));
    app.add_option("-t,--timeout", timeout, "Timeout ms");
    auto* topologyOpt = app.add_option("-T,--topology", topologyPath,
        "Topology config file (default: " + topologyPath + ")");
    app.add_option("--board", boardNickname,
        "Board nickname to use from --topology (required if it has more than one board)");
    app.add_flag("--save", save, "Save the connection just used to --topology (or its default path)");
```

- [ ] **Step 3: Replace the connect-resolution and save block**

Old:
```cpp
    psb::PsbModbusClient client;
    g_client = &client;

    // Resolve port from config early (CLI args override below)
    if (port.empty() && cfgMgr.hasConnectionSettings()) {
        port = cfgMgr.port; baud = cfgMgr.baudRate; slaveId = cfgMgr.slaveId; timeout = cfgMgr.timeoutMs;
    }

    // Connect after parsing but before subcommand callbacks fire
    app.parse_complete_callback([&]() {
        if (!port.empty()) {
            if (!client.connect(port, baud, slaveId, timeout)) {
                std::cerr << "Connection error: " << client.lastError() << "\n";
                std::exit(1);
            }
        }
    });

    CLI11_PARSE(app, argc, argv);

    if (!port.empty()) {
        cfgMgr.setFromArgs(port, baud, slaveId, timeout);
        if (save) cfgMgr.save();
    }

    return 0;
}
```

New:
```cpp
    psb::PsbModbusClient client;
    g_client = &client;

    // Connect after parsing but before subcommand callbacks fire. Must run
    // post-parse (not before, like the old ConfigManager fallback could)
    // because --topology/--board are themselves CLI flags — their final
    // values, and whether --topology was explicitly passed at all
    // (topologyOpt->count()), are only known once parsing has happened.
    app.parse_complete_callback([&]() {
        bool topologyExplicit = topologyOpt->count() > 0;
        if (port.empty()) {
            if (psb::TopologyConfig::exists(topologyPath)) {
                auto topo = psb::TopologyConfig::load(topologyPath);
                if (!topo.has_value()) {
                    std::cerr << "Topology config error: could not parse " << topologyPath << "\n";
                    std::exit(1);
                }
                const psb::BoardConfig* board = nullptr;
                const psb::BusConfig* bus = nullptr;
                if (!boardNickname.empty()) {
                    for (const auto& b : topo->buses) {
                        for (const auto& brd : b.boards) {
                            if (brd.nickname == boardNickname) { board = &brd; bus = &b; break; }
                        }
                        if (board) break;
                    }
                    if (!board) {
                        std::cerr << "Topology config error: no board named '" << boardNickname
                                  << "' in " << topologyPath << "\n";
                        std::exit(1);
                    }
                } else if (topo->totalBoardCount() == 1) {
                    bus = &topo->buses.front();
                    board = &bus->boards.front();
                } else if (topo->totalBoardCount() > 1) {
                    std::cerr << "Topology config " << topologyPath << " has " << topo->totalBoardCount()
                              << " boards — specify one with --board <nickname>\n";
                    std::exit(1);
                }
                if (board && bus) {
                    port = bus->port;
                    baud = bus->baudRate;
                    slaveId = board->slaveId;
                }
            } else if (topologyExplicit) {
                std::cerr << "Topology config error: " << topologyPath << " not found\n";
                std::exit(1);
            }
            // Neither -p nor a resolvable --topology: port stays empty,
            // exactly like today — commands that don't need a connection
            // still work; connection-needing ones fail with the client's
            // own "not connected" error, unchanged.
        }
        if (!port.empty()) {
            if (!client.connect(port, baud, slaveId, timeout)) {
                std::cerr << "Connection error: " << client.lastError() << "\n";
                std::exit(1);
            }
        }
    });

    CLI11_PARSE(app, argc, argv);

    if (!port.empty() && save) {
        auto cfg = psb::TopologyConfig::singleBoard(port, baud, slaveId);
        cfg.save(topologyPath);
    }

    return 0;
}
```

- [ ] **Step 4: Build**

Run: `cd tools && cmake --build build --target psb_demo_cli`
Expected: clean build.

- [ ] **Step 5: Manual smoke test — quick single-board path unchanged**

Run: `./bin/psb_demo_cli --help`
Expected: help text lists `-p/-b/-i/-t/-T/--board/--save`, no crash.

Run (against a board, or skip if none attached — this step just confirms the `-p` path still behaves identically to before this task): `./bin/psb_demo_cli -p /dev/ttyUSB0 info`
Expected: same output shape as before this task (this path doesn't touch `--topology` logic at all).

- [ ] **Step 6: Manual smoke test — `--save` / `--topology` round trip**

Run: `rm -f /tmp/psb_demo_cli_test_topology.toml`
Run: `./bin/psb_demo_cli --port /dev/ttyUSB0 --topology /tmp/psb_demo_cli_test_topology.toml --save info` (or any subcommand; if no board is attached this will print a connection error and exit 1 *before* reaching the save step — that's expected per Step 3's code, since `--save` only runs `if (!port.empty() && save)`, and `port` is still set from `-p` even though `connect()` itself failed and called `std::exit(1)` inside the callback, so this specific combination can't be smoke-tested without real hardware; skip to Step 7 for the part that doesn't need a live board)

Run (no hardware needed — tests topology *resolution*, not a live connection): first hand-write a topology file, then confirm `--topology` resolves without `-p`:
```bash
mkdir -p /tmp/psb_cli_topo_test
cat > /tmp/psb_cli_topo_test/topology.toml <<'EOF'
[[bus]]
name = "bus1"
port = "/dev/ttyUSB0"
baud_rate = 115200

  [[bus.board]]
  nickname = "test-board"
  slave_id = 1
EOF
./bin/psb_demo_cli --topology /tmp/psb_cli_topo_test/topology.toml info
```
Expected: attempts to connect to `/dev/ttyUSB0` at slave 1 (visible via a "Connection error: ..." if no real board is there — the point is confirming it resolved `/dev/ttyUSB0`/slave 1 from the file, not that it succeeded).

Run (multi-board error path):
```bash
cat >> /tmp/psb_cli_topo_test/topology.toml <<'EOF'

  [[bus.board]]
  nickname = "test-board-2"
  slave_id = 2
EOF
./bin/psb_demo_cli --topology /tmp/psb_cli_topo_test/topology.toml info
```
Expected: `Topology config /tmp/psb_cli_topo_test/topology.toml has 2 boards — specify one with --board <nickname>` on stderr, exit code 1.

Run (explicit missing-file error path):
```bash
./bin/psb_demo_cli --topology /tmp/psb_cli_topo_test/does_not_exist.toml info
```
Expected: `Topology config error: /tmp/psb_cli_topo_test/does_not_exist.toml not found` on stderr, exit code 1.

- [ ] **Step 7: Run the full `psb_tests` suite once more (nothing in this task should have touched it, but confirm)**

Run: `./build/psb_modbus_core/tests/psb_tests`
Expected: PASS, unchanged assertion count from Task 4's Step 9 baseline.

- [ ] **Step 8: Commit**

```bash
git add tools/psb_demo_app/cli/main.cpp
git commit -m "feat(psb_demo_cli): wire --topology/--board, --save now writes TopologyConfig"
```

---

## Task 6: `psb_demo_tui` — CLI11 + `--topology`, Phase 1 single-board restriction

**Files:**
- Modify: `tools/psb_demo_app/tui/main.cpp`
- Modify: `tools/psb_demo_app/tui/CMakeLists.txt`

**Interfaces:**
- Consumes: `psb::TopologyConfig` (Task 4), `CLI::App` (CLI11, vendored — not yet linked into `psb_demo_tui`, see Step 4).

Only the flag-parsing and startup connection-defaults section changes
(`main()`'s first ~20 lines plus its final auto-connect trigger) — the
worker thread, poll loop, and every UI component are untouched, since the
TUI dashboard stays genuinely single-board this phase. **Phase 1 scoping
note, not in the spec's final-state precedence table:** unlike the
end-state design (where a missing `--topology` file auto-launches the
setup wizard), this phase has no wizard yet — a missing explicit
`--topology` file is a clear error here too, same as `psb_demo_cli`. That
comes in a later phase's plan once the wizard exists.

- [ ] **Step 1: Update includes and drop the `ConfigManager` global**

Edit `tools/psb_demo_app/tui/main.cpp`:

Old:
```cpp
#include "psb_modbus_client.h"
#include "config_manager.h"
#include "register_map.h"
#include "board_catalog.h"
#include "tool_version.h"
#include "tab_monitor.h"
#include "tab_channel.h"
#include "tui_policy.h"
#include "modbus_settings.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

using namespace ftxui;

static psb::PsbModbusClient g_client;
static psb::ConfigManager   g_cfg;
static std::atomic<bool>    g_connected{false};
static int g_pollInterval = 1;
```

New:
```cpp
#include "psb_modbus_client.h"
#include "topology_config.h"
#include "register_map.h"
#include "board_catalog.h"
#include "tool_version.h"
#include "tab_monitor.h"
#include "tab_channel.h"
#include "tui_policy.h"
#include "modbus_settings.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include <CLI/CLI.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

using namespace ftxui;

static psb::PsbModbusClient g_client;
static std::atomic<bool>    g_connected{false};
static int g_pollInterval = 1;
```

- [ ] **Step 2: Replace the hand-rolled `argv` loop with CLI11 + topology resolution**

Old:
```cpp
int main(int argc, char** argv) {
    g_cfg.load();

    std::string portArg;
    int baudArg = 115200, slaveArg = 1, timeoutArg = 3000;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "-p" && i+1 < argc) portArg        = argv[++i];
        else if (a == "-b" && i+1 < argc) baudArg        = std::stoi(argv[++i]);
        else if (a == "-i" && i+1 < argc) slaveArg       = std::stoi(argv[++i]);
        else if (a == "-t" && i+1 < argc) timeoutArg     = std::stoi(argv[++i]);
        else if (a == "-s" && i+1 < argc) g_pollInterval = std::stoi(argv[++i]);
    }

    std::string cfgPort    = portArg.empty() ? (g_cfg.port.empty() ? "/dev/ttyUSB0" : g_cfg.port) : portArg;
    std::string cfgBaud    = std::to_string(baudArg != 115200 ? baudArg : g_cfg.baudRate);
    std::string cfgSlaveId = std::to_string(slaveArg != 1     ? slaveArg : g_cfg.slaveId);
```

New:
```cpp
int main(int argc, char** argv) {
    CLI::App app{"PSB Demo TUI"};
    app.set_version_flag("--version", std::string("psb_demo_tui ") + TOOL_VERSION_STRING);

    std::string portArg;
    int baudArg = 115200, slaveArg = 1, timeoutArg = 3000;
    std::string topologyPath = psb::TopologyConfig::defaultPath();
    app.add_option("-p,--port", portArg, "Serial port (quick single-board connect; auto-connects at startup)");
    auto* baudOpt = app.add_option("-b,--baud", baudArg, "Baud rate")
        ->check(CLI::IsMember({9600, 19200, 38400, 115200}));
    auto* slaveOpt = app.add_option("-i,--id", slaveArg, "Slave ID")->check(CLI::Range(0, 247));
    app.add_option("-t,--timeout", timeoutArg, "Timeout ms");
    app.add_option("-s,--poll-interval", g_pollInterval, "Idle poll interval (s)");
    auto* topologyOpt = app.add_option("-T,--topology", topologyPath,
        "Topology config file (default: " + topologyPath + ")");
    CLI11_PARSE(app, argc, argv);

    // -p auto-connects at startup, exactly like today; --topology (or its
    // default path) only pre-fills the connection modal's fields — it does
    // NOT by itself auto-connect, same distinction ConfigManager's old
    // auto-load-as-default behavior had (the user still clicks Connect).
    bool autoConnect = !portArg.empty();
    std::string cfgPort = portArg;
    if (portArg.empty()) {
        bool topologyExplicit = topologyOpt->count() > 0;
        if (psb::TopologyConfig::exists(topologyPath)) {
            auto topo = psb::TopologyConfig::load(topologyPath);
            if (!topo.has_value()) {
                std::cerr << "Topology config error: could not parse " << topologyPath << "\n";
                return 1;
            }
            // Phase 1: the dashboard below is still single-board only. A
            // topology resolving to more than one board is a clear, named
            // error rather than silently only using the first one — the
            // multi-board dashboard lands in a later phase (see
            // docs/superpowers/specs/2026-07-20-multi-board-topology-design.md).
            if (topo->totalBoardCount() > 1) {
                std::cerr << "Topology config " << topologyPath << " has " << topo->totalBoardCount()
                          << " boards — the multi-board dashboard isn't available yet in this build.\n"
                          << "Use --port for a specific board, or trim the topology file to one board.\n";
                return 1;
            }
            if (topo->totalBoardCount() == 1) {
                const auto& bus = topo->buses.front();
                const auto& board = bus.boards.front();
                cfgPort = bus.port;
                if (baudOpt->count() == 0) baudArg = bus.baudRate;
                if (slaveOpt->count() == 0) slaveArg = board.slaveId;
            }
        } else if (topologyExplicit) {
            std::cerr << "Topology config error: " << topologyPath << " not found\n";
            return 1;
        }
        // Neither -p nor a resolvable --topology: fall back to today's
        // hardcoded first-run guess.
        if (cfgPort.empty()) cfgPort = "/dev/ttyUSB0";
    }
    std::string cfgBaud    = std::to_string(baudArg);
    std::string cfgSlaveId = std::to_string(slaveArg);
```

- [ ] **Step 3: Update the auto-connect trigger at the end of `main()`**

Old:
```cpp
    if (!portArg.empty()) doConnect();
```

New:
```cpp
    if (autoConnect) doConnect();
```

- [ ] **Step 4: Link CLI11 into `psb_demo_tui`**

Edit `tools/psb_demo_app/tui/CMakeLists.txt`:

Old:
```cmake
target_link_libraries(psb_demo_tui PRIVATE psb_modbus_core ftxui::screen ftxui::dom ftxui::component)
```

New:
```cmake
target_link_libraries(psb_demo_tui PRIVATE psb_modbus_core ftxui::screen ftxui::dom ftxui::component CLI11::CLI11)
```

- [ ] **Step 5: Build**

Run: `cd tools && cmake --build build --target psb_demo_tui`
Expected: clean build.

- [ ] **Step 6: Manual smoke test — quick single-board path unchanged, via `tmux`**

Following the architecture doc's §3 methodology (`tmux` for a real PTY):

```bash
tmux new-session -d -s tui_test -x 120 -y 40 "./bin/psb_demo_tui -p /dev/ttyUSB0"
sleep 1
tmux capture-pane -t tui_test -p
tmux kill-session -t tui_test
```
Expected: the pane shows the dashboard attempting to connect (menu bar, "Connecting to /dev/ttyUSB0..." or a connection error if no board is attached) — same as before this task. If no board is attached, a "Connection error: ..." status message is expected and fine; the point of this check is that the app *launches and reaches the dashboard*, not that it successfully connects.

- [ ] **Step 7: Manual smoke test — `--topology` resolution and Phase 1 multi-board error, via `tmux`**

```bash
mkdir -p /tmp/psb_tui_topo_test
cat > /tmp/psb_tui_topo_test/one_board.toml <<'EOF'
[[bus]]
name = "bus1"
port = "/dev/ttyUSB0"
baud_rate = 115200

  [[bus.board]]
  nickname = "test-board"
  slave_id = 1
EOF
tmux new-session -d -s tui_test2 -x 120 -y 40 \
  "./bin/psb_demo_tui --topology /tmp/psb_tui_topo_test/one_board.toml"
sleep 1
tmux capture-pane -t tui_test2 -p
tmux kill-session -t tui_test2
```
Expected: dashboard launches (not auto-connected — no `-p` was given — but the connection modal, opened via the Connect button, should show `/dev/ttyUSB0` / `115200` / `1` pre-filled).

```bash
cat >> /tmp/psb_tui_topo_test/one_board.toml <<'EOF'

  [[bus.board]]
  nickname = "test-board-2"
  slave_id = 2
EOF
./bin/psb_demo_tui --topology /tmp/psb_tui_topo_test/one_board.toml
echo "exit code: $?"
```
Expected: no TUI screen launches at all — prints `Topology config /tmp/psb_tui_topo_test/one_board.toml has 2 boards — the multi-board dashboard isn't available yet in this build.` to stderr and exits with code 1, *before* `ScreenInteractive::Fullscreen()` ever runs (confirm by checking the terminal wasn't taken over — the shell prompt returns immediately).

- [ ] **Step 8: Run the full `psb_tests` suite once more (nothing in this task touches it, but confirm)**

Run: `./build/psb_modbus_core/tests/psb_tests`
Expected: PASS, unchanged assertion count from Task 5's Step 7 baseline.

- [ ] **Step 9: Commit**

```bash
git add tools/psb_demo_app/tui/main.cpp tools/psb_demo_app/tui/CMakeLists.txt
git commit -m "feat(psb_demo_tui): migrate to CLI11, wire --topology (single-board only this phase)"
```

---

## Task 7: Full-repo verification

**Files:** none (verification only).

- [ ] **Step 1: Clean full rebuild of every touched target**

Run: `cd tools && cmake --build build --target psb_modbus_core psb_tests psb_demo_cli psb_demo_tui 2>&1 | tail -80`
Expected: clean build, zero warnings introduced beyond what already existed before this plan (compare against a `git stash`-based baseline build if unsure).

- [ ] **Step 2: Full `psb_tests` run**

Run: `./build/psb_modbus_core/tests/psb_tests`
Expected: PASS. Total test-case count should be the original baseline (from Task 3 Step 1) plus 4 (`[serial_bus]`) + 5 (`[board_session]`) + 5 (`[topology_config]`) = baseline + 14 new test cases, 0 failures.

- [ ] **Step 3: Confirm `psb_demo_gui` still builds unmodified (Global Constraints — not touched this phase)**

Only run this step if Qt 6 is available in the environment (see `docs/guide/*` for the Qt path used in prior sessions on this machine, e.g. `~/backup/Qt/6.8.5/gcc_64`):

Run: `cd tools && cmake -DBUILD_GUI=ON -DCMAKE_PREFIX_PATH=<qt-path> . && cmake --build build --target psb_demo_gui`
Expected: clean build — this is the concrete proof that `psb_modbus_client.h`'s public API truly never changed shape across Tasks 1-3.

- [ ] **Step 4: Live-hardware pass, if a board is available**

Per the spec's Testing section and this codebase's established discipline (re-measure, don't just re-read the diff): connect a real board and confirm, for both `psb_demo_cli` and `psb_demo_tui`:
- `-p <port>` quick-connect still works and behaves identically to before this plan (same connect latency — no regression from the bus/session split's extra indirection; a couple hundred µs of function-call overhead per transaction is not observable against tens-of-ms Modbus RTU round trips, but confirm nothing *else* changed, e.g. via `strace -tt` on a handful of transactions compared against a pre-plan build, matching the methodology in `docs/guide/client-architecture-and-pitfalls.md` §3).
- `--topology` with a hand-written one-board file connects correctly.
- `psb_demo_cli --save` followed by `psb_demo_cli --topology <path>` (no `-p`) round-trips to the same board.

- [ ] **Step 5: Final commit (only if any fixups were needed in Steps 1-4)**

If everything passed clean, there's nothing to commit here — Tasks 1-6 already each committed their own work. If Step 1-4 surfaced a fixup, commit it against the specific task it belongs to (amend that task's understanding, don't bundle an unrelated fix into this verification task).

---

## Self-Review

**Spec coverage** (against `docs/superpowers/specs/2026-07-20-multi-board-topology-design.md`'s Rollout §1 "Core"):
- ✅ `PsbSerialBus`/`PsbBoardSession` split — Tasks 1-2.
- ✅ `TopologyConfig` + TOML schema — Task 4.
- ✅ CLI11 flag wiring (`--topology`, single-board synthesis) for both tools — Tasks 5-6.
- ✅ `ConfigManager` removal (corrected: superseded, not dropped — `--save`/auto-load behavior preserved) — Task 4 (removal) + Tasks 5-6 (behavior migration).
- ✅ Unit tests — every task includes them; Task 3 additionally re-runs the *entire* pre-existing suite as an explicit regression gate.
- ✅ "Landable alone — no UI changes; existing single-board TUI/CLI/GUI keep working" — Task 3 Step 5 and Task 7 Step 3 explicitly verify this for GUI; Tasks 5-6's manual smoke tests verify it for CLI/TUI's `-p` path.
- Deferred to later phases per the spec's own Rollout: the TUI multi-board dashboard (§2) and interactive setup wizard (§3) are out of scope for this plan by design — Task 6 explicitly errors rather than silently mishandling a multi-board topology in the meantime.

**Placeholder scan:** no TBD/TODO markers; every code step contains complete, compilable content; no "similar to Task N" shortcuts — Task 2's copy-derived file is specified as literal find/replace blocks against the real current file content, not a description of a pattern to follow.

**Type consistency:** cross-checked method names/signatures across Tasks 1-2-3 — `PsbSerialBus::readInputRegs/readHoldingRegs/writeRegs` (Task 1) match what `PsbBoardSession::readRegsInternal/writeRegsInternal` (Task 2, Edit C) call; `PsbBoardSession`'s full method list (Task 2, Step 4 header) matches what the Task 3 facade forwards to, one-for-one, including the low-level `readInputRegs(uint16_t,uint16_t,uint16_t*)`/`readHoldingRegs`/`writeReg16` (session-level, no slave ID parameter — implicit from the session) versus the bus-level versions (Task 1, slave-ID-parameterized) — verified these are not accidentally conflated anywhere a call site was written.

Plan complete and saved to `docs/superpowers/plans/2026-07-20-multi-board-topology-phase1-core.md`. Two execution options:

1. **Subagent-Driven (recommended)** — I dispatch a fresh subagent per task, review between tasks, fast iteration
2. **Inline Execution** — Execute tasks in this session using executing-plans, batch execution with checkpoints

Which approach?
