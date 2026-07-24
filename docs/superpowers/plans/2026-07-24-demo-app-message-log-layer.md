# Demo App Message Log Layer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace sticky ad hoc status strings with an action-scoped message center and retained log that can be shared by TUI and GUI while preserving UI-specific rendering.

**Architecture:** Add a core `MessageCenter` to `psb_modbus_core` that owns message severity, action ids, timestamps, retention, and status replacement/clearing rules. TUI and GUI publish events into that center and render the current status/log through their own widgets, colors, timers, and framework bindings.

**Tech Stack:** C++17, Catch2, Qt/QML for GUI adapter, FTXUI for TUI adapter, existing `psb_modbus_core` static library.

**Implementation Status:** Implemented in commits `9fad9cd`, `71699b7`, `283eb26`, and `0b92b60`. Core and TUI tests passed on 2026-07-24. GUI compile verification was not available in this environment because Qt6 development package discovery failed during `BUILD_GUI=ON` CMake configuration.

---

## File Structure

- Create `tools/psb_modbus_core/message_log.h`
  - Defines `MessageSeverity`, `MessageRecord`, `MessageCenter`, and helpers.
- Create `tools/psb_modbus_core/message_log.cpp`
  - Implements append, action start, action finish, clear stale warning/error, bounded retention.
- Modify `tools/psb_modbus_core/CMakeLists.txt`
  - Adds `message_log.cpp` to `psb_modbus_core`.
- Create `tools/psb_modbus_core/tests/test_message_log.cpp`
  - Core behavior tests.
- Modify `tools/psb_modbus_core/tests/CMakeLists.txt`
  - Adds the new test file if tests enumerate sources explicitly.
- Modify `tools/psb_demo_app/tui/board_session.h`
  - Replace or wrap `statusMsg/statusMutex` with a per-board message center.
- Modify `tools/psb_demo_app/tui/widgets.h`
  - Route `postWrite()` through the message center.
- Modify `tools/psb_demo_app/tui/board_dashboard.h`
  - Render current board status from message center; add/prepare log-view access.
- Modify `tools/psb_demo_app/tui/group_monitor.h`
  - Render group status from member boards’ current message records, not stale copied strings.
- Modify `tools/psb_demo_app/tui/main.cpp`
  - Update connect/disconnect/poll/offline messages to use message center.
- Modify `tools/psb_demo_app/tui/tests/test_tui_widgets.cpp`
  - Add UI policy tests for status text/color derivation.
- Modify `tools/psb_demo_app/gui/modbus_backend.h/.cpp`
  - Introduce GUI adapter over `MessageCenter`, deprecating direct `m_statusMessage` mutation and keeping `rawLog` separate or migrated as noted below.

## Design Rules

- Every user action starts a new action scope.
- Starting an action clears/replaces any previous warning/error in the status bar immediately with an info message such as `Writing Target V...`.
- If an action has no meaningful info to show, it must still clear stale warning/error status by publishing an empty or neutral info message.
- Error/warning messages remain visible until the next action starts or a newer message replaces them.
- Success/info messages may auto-clear in UI layer after a short timeout.
- Full message history is retained in the log ring buffer regardless of status-bar auto-clear.
- Core library does not know FTXUI, Qt, colors, widgets, or timers.
- TUI/GUI do not implement message severity or stale-message policy independently.

## Core API Sketch

```cpp
enum class MessageSeverity {
    Debug,
    Info,
    Success,
    Warning,
    Error,
};

struct MessageRecord {
    uint64_t sequence = 0;
    uint64_t actionId = 0;
    MessageSeverity severity = MessageSeverity::Info;
    std::string source;
    std::string text;
    std::chrono::system_clock::time_point timestamp{};
};

class MessageCenter {
public:
    explicit MessageCenter(size_t capacity = 500);

    uint64_t beginAction(const std::string& source, const std::string& text = "");
    void publish(uint64_t actionId, MessageSeverity severity,
                 const std::string& source, const std::string& text);
    void clearStatus(uint64_t actionId, const std::string& source);

    std::optional<MessageRecord> currentStatus() const;
    std::vector<MessageRecord> records() const;
    void clearLog();
};
```

## Task 1: Core Message Center

**Files:**
- Create: `tools/psb_modbus_core/message_log.h`
- Create: `tools/psb_modbus_core/message_log.cpp`
- Test: `tools/psb_modbus_core/tests/test_message_log.cpp`
- Modify: `tools/psb_modbus_core/CMakeLists.txt`

- [ ] **Step 1: Write failing core tests**

Add tests covering:

```cpp
TEST_CASE("MessageCenter - starting a new action replaces stale error status") {
    psb::MessageCenter messages;
    auto a1 = messages.beginAction("board1", "Connecting...");
    messages.publish(a1, psb::MessageSeverity::Error, "board1", "Error: timeout");
    REQUIRE(messages.currentStatus()->severity == psb::MessageSeverity::Error);

    auto a2 = messages.beginAction("board1", "Writing Target V...");

    REQUIRE(messages.currentStatus().has_value());
    CHECK(messages.currentStatus()->actionId == a2);
    CHECK(messages.currentStatus()->severity == psb::MessageSeverity::Info);
    CHECK(messages.currentStatus()->text == "Writing Target V...");
}

TEST_CASE("MessageCenter - empty action start clears stale status but preserves log") {
    psb::MessageCenter messages;
    auto a1 = messages.beginAction("board1", "Connecting...");
    messages.publish(a1, psb::MessageSeverity::Error, "board1", "Error: timeout");

    auto a2 = messages.beginAction("board1");

    CHECK_FALSE(messages.currentStatus().has_value());
    CHECK(messages.records().size() == 2);
    CHECK(messages.records().back().actionId == a1);
}

TEST_CASE("MessageCenter - bounded log drops oldest records") {
    psb::MessageCenter messages(2);
    auto a1 = messages.beginAction("board1", "A");
    messages.publish(a1, psb::MessageSeverity::Success, "board1", "OK A");
    auto a2 = messages.beginAction("board1", "B");

    auto log = messages.records();
    REQUIRE(log.size() == 2);
    CHECK(log[0].text == "OK A");
    CHECK(log[1].text == "B");
}
```

- [ ] **Step 2: Verify tests fail**

Run:

```bash
cmake --build tools/build --target psb_tests -j
```

Expected: compile failure because `message_log.h` / `MessageCenter` do not exist.

- [ ] **Step 3: Implement minimal core**

Create the header and implementation with mutex-protected state. `beginAction(source, "")` increments the action id, clears current status, and does not append an empty log record. `beginAction(source, text)` appends an info record and makes it current.

- [ ] **Step 4: Wire CMake and pass core tests**

Run:

```bash
cmake --build tools/build --target psb_tests -j
./tools/build/psb_modbus_core/tests/psb_tests
```

Expected: all core tests pass.

- [ ] **Step 5: Commit**

```bash
git add tools/psb_modbus_core/message_log.* tools/psb_modbus_core/CMakeLists.txt tools/psb_modbus_core/tests
git commit -m "feat(psb_modbus_core): add message center"
```

## Task 2: TUI Board Message Adapter

**Files:**
- Modify: `tools/psb_demo_app/tui/board_session.h`
- Modify: `tools/psb_demo_app/tui/widgets.h`
- Modify: `tools/psb_demo_app/tui/board_dashboard.h`
- Test: `tools/psb_demo_app/tui/tests/test_tui_widgets.cpp`

- [ ] **Step 1: Write failing TUI status policy tests**

Add tests for converting `MessageRecord` to status-bar text and error color policy:

```cpp
TEST_CASE("TUI status derives text and error flag from message severity") {
    psb::MessageRecord ok{1, 1, psb::MessageSeverity::Success, "board1", "OK: Save", {}};
    auto okStatus = boardStatusLine(ok);
    CHECK(okStatus.text == "OK: Save");
    CHECK_FALSE(okStatus.isError);

    psb::MessageRecord err{2, 2, psb::MessageSeverity::Error, "board1", "Error: timeout", {}};
    auto errStatus = boardStatusLine(err);
    CHECK(errStatus.text == "Error: timeout");
    CHECK(errStatus.isError);
}
```

- [ ] **Step 2: Verify test fails**

Run:

```bash
cmake --build tools/build --target psb_demo_tui_tests -j
```

Expected: compile failure for missing `boardStatusLine()`.

- [ ] **Step 3: Add board status rendering helper**

Add a small helper in `board_dashboard.h`:

```cpp
struct BoardStatusLine {
    std::string text;
    bool isError = false;
};

inline BoardStatusLine boardStatusLine(const psb::MessageRecord& record) {
    return {record.text,
            record.severity == psb::MessageSeverity::Error ||
            record.severity == psb::MessageSeverity::Warning};
}
```

- [ ] **Step 4: Replace board `statusMsg` writes incrementally**

In `BoardSession`, add:

```cpp
psb::MessageCenter messages;
```

Keep `statusMsg/statusMutex` temporarily during migration if needed, but new writes should use `messages.beginAction()` and `messages.publish()`.

- [ ] **Step 5: Update `postWrite()`**

In `widgets.h`, change `postWrite()` to:

```cpp
uint64_t action = s.messages.beginAction("board", "Writing " + label + "...");
...
if (ok) {
    s.messages.publish(action, psb::MessageSeverity::Success, "board", "OK: " + label);
} else {
    s.messages.publish(action, psb::MessageSeverity::Error, "board", "Error: " + s.client.lastError());
}
```

This requires `AppState` to carry `MessageCenter& messages`.

- [ ] **Step 6: Update board status bar render**

In `board_dashboard.h`, read:

```cpp
auto current = board.messages.currentStatus();
std::string msg = current ? current->text : "";
bool isErr = current && (current->severity == psb::MessageSeverity::Error ||
                         current->severity == psb::MessageSeverity::Warning);
```

- [ ] **Step 7: Run TUI tests and app build**

```bash
cmake --build tools/build --target psb_demo_tui_tests psb_demo_tui -j
./tools/build/psb_demo_app/tui/tests/psb_demo_tui_tests
```

- [ ] **Step 8: Commit**

```bash
git add tools/psb_demo_app/tui
git commit -m "feat(psb_demo_tui): route board status through message center"
```

## Task 3: TUI Group and App Status

**Files:**
- Modify: `tools/psb_demo_app/tui/group_monitor.h`
- Modify: `tools/psb_demo_app/tui/main.cpp`
- Optional create: `tools/psb_demo_app/tui/message_log_view.h`
- Test: `tools/psb_demo_app/tui/tests/test_tui_widgets.cpp`

- [ ] **Step 1: Write failing group status selection test**

Add a helper test:

```cpp
TEST_CASE("Group status prefers local action message over stale member error") {
    psb::MessageCenter memberMessages;
    auto old = memberMessages.beginAction("board1", "Connecting...");
    memberMessages.publish(old, psb::MessageSeverity::Error, "board1", "Error: timeout");

    psb::MessageCenter groupMessages;
    auto action = groupMessages.beginAction("group", "Renaming group...");
    groupMessages.publish(action, psb::MessageSeverity::Success, "group", "OK: group renamed");

    auto selected = selectGroupStatus(groupMessages.currentStatus(),
                                      {memberMessages.currentStatus()});
    REQUIRE(selected.has_value());
    CHECK(selected->text == "OK: group renamed");
}
```

- [ ] **Step 2: Verify failure**

Run:

```bash
cmake --build tools/build --target psb_demo_tui_tests -j
```

Expected: missing `selectGroupStatus()`.

- [ ] **Step 3: Implement group status selection**

In `group_monitor.h`, replace `localStatusMsg` string with `MessageCenter`. Add `selectGroupStatus()` that:
- returns local group current status first,
- otherwise returns first member error/warning,
- otherwise returns first member info/success,
- otherwise returns neutral `"Group ready"` rendered by UI, not logged.

- [ ] **Step 4: Add log view plumbing**

Add a top-level `Log` or `Messages` button later if desired. Minimum viable version can expose retained logs through an about/log popup or debug pane. Do not block status correctness on this UI.

- [ ] **Step 5: Build/test and commit**

```bash
cmake --build tools/build --target psb_demo_tui_tests psb_demo_tui -j
./tools/build/psb_demo_app/tui/tests/psb_demo_tui_tests
git add tools/psb_demo_app/tui
git commit -m "feat(psb_demo_tui): add action-scoped group messages"
```

## Task 4: GUI Adapter

**Files:**
- Modify: `tools/psb_demo_app/gui/modbus_backend.h`
- Modify: `tools/psb_demo_app/gui/modbus_backend.cpp`
- Modify QML only if a visible log panel is added.

- [ ] **Step 1: Preserve existing GUI status API**

Keep:

```cpp
Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusMessageChanged)
Q_PROPERTY(QString rawLog READ rawLog NOTIFY rawLogChanged)
```

Add:

```cpp
psb::MessageCenter m_messages;
QStringList m_messageLogLines;
```

- [ ] **Step 2: Route `setStatus()` through core**

Replace direct `m_statusMessage = msg` logic with severity parsing only at the boundary:

```cpp
void ModbusBackend::setStatus(const QString& msg) {
    auto action = m_messages.beginAction("gui", msg.toStdString());
    auto current = m_messages.currentStatus();
    m_statusMessage = current ? QString::fromStdString(current->text) : QString();
    emit statusMessageChanged();
}
```

Then refine call sites to use explicit severity instead of parsing `Error:` strings.

- [ ] **Step 3: Keep GUI auto-clear UI-specific**

The existing `m_statusClearTimer` remains in GUI. It clears only display status, not `MessageCenter` history.

- [ ] **Step 4: Keep raw frame log separate or mirror into MessageCenter debug**

Raw Modbus frames are diagnostic transport logs. Keep `rawLog` as-is initially. Optionally mirror each raw frame with `MessageSeverity::Debug` into `m_messages` only after UI has filtering, so raw frames do not drown operational actions.

- [ ] **Step 5: Build GUI target**

Run the existing GUI build target if configured. If no GUI test target exists, at minimum run full tools build used by this repository.

- [ ] **Step 6: Commit**

```bash
git add tools/psb_demo_app/gui
git commit -m "feat(psb_demo_gui): route status through message center"
```

## Task 5: Cleanup and Migration

**Files:**
- Search all of `tools/psb_demo_app/tui`
- Search all of `tools/psb_demo_app/gui`

- [ ] **Step 1: Find remaining direct sticky status writes**

Run:

```bash
rg -n "statusMsg|statusMessage|setStatus\\(|Error:|OK:" tools/psb_demo_app/tui tools/psb_demo_app/gui
```

- [ ] **Step 2: Convert remaining action paths**

For each user action with no current message output, add:

```cpp
auto action = messages.beginAction(source);
```

This clears stale warning/error status without adding noise to the visible status bar.

- [ ] **Step 3: Convert remaining success/error paths**

Use explicit severity:

```cpp
messages.publish(action, psb::MessageSeverity::Success, source, "OK: ...");
messages.publish(action, psb::MessageSeverity::Error, source, "Error: ...");
```

- [ ] **Step 4: Remove legacy string fields when all call sites are migrated**

Remove stale `statusMsg/statusMutex` fields only after no direct call sites remain.

- [ ] **Step 5: Full verification**

```bash
cmake --build tools/build --target psb_tests psb_demo_tui_tests psb_demo_tui -j
./tools/build/psb_modbus_core/tests/psb_tests
./tools/build/psb_demo_app/tui/tests/psb_demo_tui_tests
git diff --check
```

- [ ] **Step 6: Commit**

```bash
git add tools/psb_modbus_core tools/psb_demo_app
git commit -m "refactor(demo_apps): complete message center migration"
```

## Manual Verification Scenarios

- Trigger an invalid board action that shows a red error.
- Perform a later valid action that emits no specific status text.
- Expected: red error disappears immediately.
- Trigger a valid write.
- Expected: status shows `Writing ...`, then `OK: ...`, then UI may clear the status display after its timeout.
- Trigger a failing write.
- Expected: error stays visible until next action starts.
- Open group dashboard after a board error.
- Perform a group rename or alias edit.
- Expected: local group success/error message overrides old member-board error.
- GUI: connect failure followed by scan/port action.
- Expected: old connection error does not remain as if it belongs to the scan action.

## Self-Review

- Spec coverage: core owns message semantics and retention; TUI/GUI own rendering and timers.
- Stale red-warning issue: solved by `beginAction()` clearing previous visible error/warning.
- Logging layer: solved by bounded retained `MessageCenter::records()`.
- UI separation: no FTXUI/Qt dependencies in core.
- Risk: migrating all status writers is broad; tasks split into core, TUI board, TUI group/app, GUI, cleanup to keep commits reviewable.
