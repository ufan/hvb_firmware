# CLI Args → Preferences Dialog Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove every `psb_demo_tui` CLI flag except `--version`, giving connection timeout and idle poll interval — the two settings with no UI home — a new global **Preferences** dialog that persists across launches, and simplifying `main()`'s topology-resolution chain now that the flags that drove three of its four branches are gone.

**Architecture:** A new `AppPreferences` struct (`psb_modbus_core`, mirroring `TopologyConfig`'s `load()/save()/defaultPath()` shape) persists to `~/.psb_demo_app/preferences.toml`. A new `preferences_dialog.h` (`psb_demo_app/tui`) builds the dialog as a `Modal`, layered onto the switcher root the same way the mid-session Setup wizard's `Modal` already is. `Preferences` becomes a third global button threaded through the exact `globalQuit`/`globalSetup` machinery Sub-project B built — its own row at 2+ boards, folded into the single board's own row otherwise.

**Tech Stack:** C++17, FTXUI (vendored), toml++ (vendored), CLI11 (vendored), the existing `BoardSwitcher`/`BoardSession`/`Runtime` machinery from Phase 3 and Sub-projects A/B.

## Global Constraints

- **Only `--version` (and CLI11's own `--help`) remain on the command line.** No power-user override flags for timeout or poll interval.
- **The new button is labeled "Preferences"**, never "Settings" — deliberately distinct from the existing per-board **"Setting"** button (`board_dashboard.h`'s `bSysCfg`, which edits board register config, an unrelated concept).
- **Preferences persists to `~/.psb_demo_app/preferences.toml`**, loaded at the very top of `main()` (before any board is built) and re-saved on every Save.
- **`psb_demo_cli` is untouched** — its own independent `-t,--timeout` flag and default are unrelated to this change.
- Build via `cd tools && cmake --build build --target psb_modbus_core psb_tests psb_demo_cli psb_demo_tui`. Test via `./build/psb_modbus_core/tests/psb_tests`. Manual verification via `tmux` per `docs/guide/client-architecture-and-pitfalls.md` §3's methodology, against real hardware if available.

---

## Task 1: Extract `homeDir()` into a shared `platform_paths.h`/`.cpp`

**Files:**
- Create: `tools/psb_modbus_core/platform_paths.h`
- Create: `tools/psb_modbus_core/platform_paths.cpp`
- Modify: `tools/psb_modbus_core/topology_config.cpp`
- Modify: `tools/psb_modbus_core/CMakeLists.txt`

**Interfaces:**
- Produces: `std::string psb::homeDir()` — was `static` (internal linkage) inside `topology_config.cpp`; Task 2's `app_preferences.cpp` needs the identical logic from a second translation unit, so it moves here rather than being duplicated.

- [ ] **Step 1: Create `platform_paths.h`**

Create `tools/psb_modbus_core/platform_paths.h`:

```cpp
#pragma once

#include <string>

namespace psb {

// Shared by TopologyConfig and AppPreferences (both persist under
// homeDir() + "/.psb_demo_app/") — needed by more than one .cpp file
// within psb_modbus_core, so it isn't static/internal-linkage like it
// used to be when only topology_config.cpp needed it.
std::string homeDir();

} // namespace psb
```

- [ ] **Step 2: Create `platform_paths.cpp`**

Create `tools/psb_modbus_core/platform_paths.cpp`:

```cpp
#include "platform_paths.h"

#include <cstdlib>

#if defined(_WIN32)
#include <cstdlib>
#else
#include <unistd.h>
#include <pwd.h>
#endif

namespace psb {

std::string homeDir() {
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

} // namespace psb
```

- [ ] **Step 3: Update `topology_config.cpp` to use the shared version**

Edit `tools/psb_modbus_core/topology_config.cpp`, find:

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
```

Change to:

```cpp
#include "topology_config.h"
#include "platform_paths.h"

#include <toml++/toml.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>

namespace psb {

std::string TopologyConfig::defaultPath() {
```

- [ ] **Step 4: Add the new source file to the library**

Edit `tools/psb_modbus_core/CMakeLists.txt`, find:

```cmake
add_library(psb_modbus_core STATIC
    psb_modbus_client.cpp
    psb_serial_bus.cpp
    psb_board_session.cpp
    topology_config.cpp
    monitor_render.cpp
)
```

Change to:

```cmake
add_library(psb_modbus_core STATIC
    psb_modbus_client.cpp
    psb_serial_bus.cpp
    psb_board_session.cpp
    topology_config.cpp
    platform_paths.cpp
    monitor_render.cpp
)
```

- [ ] **Step 5: Build and run the existing test suite (regression check — this task adds no new tests, it's a pure refactor)**

Run: `cd tools && cmake --build build --target psb_tests -j$(nproc) 2>&1 | tail -30 && ./build/psb_modbus_core/tests/psb_tests "[topology_config]"`
Expected: clean build, PASS, same test/assertion counts as before this task (the refactor changes no observable behavior).

- [ ] **Step 6: Commit**

```bash
git add tools/psb_modbus_core/platform_paths.h tools/psb_modbus_core/platform_paths.cpp \
        tools/psb_modbus_core/topology_config.cpp tools/psb_modbus_core/CMakeLists.txt
git commit -m "refactor(psb_modbus_core): extract homeDir() into shared platform_paths.h"
```

---

## Task 2: `AppPreferences` struct + tests

**Files:**
- Create: `tools/psb_modbus_core/app_preferences.h`
- Create: `tools/psb_modbus_core/app_preferences.cpp`
- Test: `tools/psb_modbus_core/tests/test_app_preferences.cpp`
- Modify: `tools/psb_modbus_core/CMakeLists.txt`
- Modify: `tools/psb_modbus_core/tests/CMakeLists.txt`

**Interfaces:**
- Produces: `struct psb::AppPreferences { int timeoutMs = 3000; int pollIntervalS = 1; static std::optional<AppPreferences> load(const std::string&); bool save(const std::string&) const; static std::string defaultPath(); }`. Task 6 (`main.cpp`) calls `load()` at startup and `preferences_dialog.h` (Task 3) calls `save()`.

- [ ] **Step 1: Write the failing tests**

Create `tools/psb_modbus_core/tests/test_app_preferences.cpp`:

```cpp
#include "app_preferences.h"
#include "topology_config.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <fstream>

TEST_CASE("AppPreferences — round trip through TOML preserves both fields", "[app_preferences]") {
    psb::AppPreferences prefs;
    prefs.timeoutMs = 4500;
    prefs.pollIntervalS = 3;

    const std::string path = "/tmp/psb_app_preferences_test_roundtrip.toml";
    std::remove(path.c_str());
    REQUIRE(prefs.save(path));

    auto loaded = psb::AppPreferences::load(path);
    REQUIRE(loaded.has_value());
    CHECK(loaded->timeoutMs == 4500);
    CHECK(loaded->pollIntervalS == 3);

    std::remove(path.c_str());
}

TEST_CASE("AppPreferences — load returns nullopt for missing file", "[app_preferences]") {
    auto loaded = psb::AppPreferences::load("/tmp/psb_app_preferences_test_does_not_exist.toml");
    CHECK_FALSE(loaded.has_value());
}

TEST_CASE("AppPreferences — load returns nullopt for malformed TOML", "[app_preferences]") {
    const std::string path = "/tmp/psb_app_preferences_test_malformed.toml";
    { std::ofstream ofs(path); ofs << "this is [ not valid toml"; }
    auto loaded = psb::AppPreferences::load(path);
    CHECK_FALSE(loaded.has_value());
    std::remove(path.c_str());
}

TEST_CASE("AppPreferences — defaultPath differs from TopologyConfig's paths", "[app_preferences]") {
    CHECK(psb::AppPreferences::defaultPath() != psb::TopologyConfig::defaultPath());
    CHECK(psb::AppPreferences::defaultPath() != psb::TopologyConfig::lastSingleConnectPath());
}
```

- [ ] **Step 2: Register the test file and run to verify it fails to compile**

Edit `tools/psb_modbus_core/tests/CMakeLists.txt`, find:

```cmake
add_executable(psb_tests
    test_connection.cpp
    test_serial_bus.cpp
    test_board_session.cpp
    test_topology_config.cpp
```

Change to:

```cmake
add_executable(psb_tests
    test_connection.cpp
    test_serial_bus.cpp
    test_board_session.cpp
    test_topology_config.cpp
    test_app_preferences.cpp
```

Run: `cd tools && cmake --build build --target psb_tests 2>&1 | tail -20`
Expected: FAIL — `app_preferences.h: No such file or directory` (or `'psb::AppPreferences' has not been declared`, depending on how CMake reports the missing header).

- [ ] **Step 3: Create `app_preferences.h`**

Create `tools/psb_modbus_core/app_preferences.h`:

```cpp
#pragma once

#include <optional>
#include <string>

namespace psb {

// App-level runtime preferences for psb_demo_tui — connection timeout and
// idle poll interval. Distinct from TopologyConfig (what to connect to, not
// how long to wait / how often to poll) and from TopologyConfig's own
// lastSingleConnectPath() (single-board quick-connect's separate
// preference) — three separate files under ~/.psb_demo_app/, none of them
// ever confused for one another.
struct AppPreferences {
    int timeoutMs = 3000;
    int pollIntervalS = 1;

    // Returns std::nullopt for both a missing file and a malformed one —
    // unlike TopologyConfig, callers never need to tell those apart: a bad
    // preferences file just means "use the hardcoded defaults above",
    // never a hard error.
    static std::optional<AppPreferences> load(const std::string& path);
    bool save(const std::string& path) const;
    static std::string defaultPath();  // ~/.psb_demo_app/preferences.toml
};

} // namespace psb
```

- [ ] **Step 4: Create `app_preferences.cpp`**

Create `tools/psb_modbus_core/app_preferences.cpp`:

```cpp
#include "app_preferences.h"
#include "platform_paths.h"

#include <toml++/toml.hpp>

#include <filesystem>
#include <fstream>

namespace psb {

std::string AppPreferences::defaultPath() {
    return homeDir() + "/.psb_demo_app/preferences.toml";
}

std::optional<AppPreferences> AppPreferences::load(const std::string& path) {
    std::ifstream check(path);
    if (!check.good()) return std::nullopt;
    try {
        auto tbl = toml::parse_file(path);
        AppPreferences prefs;
        prefs.timeoutMs = static_cast<int>(tbl["timeout_ms"].value_or(3000));
        prefs.pollIntervalS = static_cast<int>(tbl["poll_interval_s"].value_or(1));
        return prefs;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

bool AppPreferences::save(const std::string& path) const {
    try {
        toml::table root;
        root.insert_or_assign("timeout_ms", timeoutMs);
        root.insert_or_assign("poll_interval_s", pollIntervalS);

        std::filesystem::path fsPath(path);
        if (fsPath.has_parent_path())
            std::filesystem::create_directories(fsPath.parent_path());

        std::ofstream ofs(path);
        if (!ofs) return false;
        ofs << root;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

} // namespace psb
```

- [ ] **Step 5: Add the new source file to the library**

Edit `tools/psb_modbus_core/CMakeLists.txt`, find:

```cmake
add_library(psb_modbus_core STATIC
    psb_modbus_client.cpp
    psb_serial_bus.cpp
    psb_board_session.cpp
    topology_config.cpp
    platform_paths.cpp
    monitor_render.cpp
)
```

Change to:

```cmake
add_library(psb_modbus_core STATIC
    psb_modbus_client.cpp
    psb_serial_bus.cpp
    psb_board_session.cpp
    topology_config.cpp
    platform_paths.cpp
    app_preferences.cpp
    monitor_render.cpp
)
```

- [ ] **Step 6: Build and run to verify the tests pass**

Run: `cd tools && cmake --build build --target psb_tests -j$(nproc) 2>&1 | tail -30 && ./build/psb_modbus_core/tests/psb_tests "[app_preferences]"`
Expected: clean build, PASS, all 4 `[app_preferences]`-tagged tests.

- [ ] **Step 7: Commit**

```bash
git add tools/psb_modbus_core/app_preferences.h tools/psb_modbus_core/app_preferences.cpp \
        tools/psb_modbus_core/tests/test_app_preferences.cpp \
        tools/psb_modbus_core/CMakeLists.txt tools/psb_modbus_core/tests/CMakeLists.txt
git commit -m "feat(psb_modbus_core): add AppPreferences (timeout/poll-interval persistence)"
```

---

## Task 3: `preferences_dialog.h` — Preferences dialog UI

**Files:**
- Create: `tools/psb_demo_app/tui/preferences_dialog.h`

**Interfaces:**
- Consumes: `psb::AppPreferences::load()/save()/defaultPath()` (Task 2), `ActionButton` (`widgets.h`).
- Produces: `struct psb::tui::PreferencesDialog { Component root; std::function<void()> open; }`, `psb::tui::makePreferencesDialog(ScreenInteractive&, std::shared_ptr<bool> showPreferences, int& timeoutMs, int& pollIntervalS) -> PreferencesDialog`. Task 6 (`main.cpp`) constructs one and wires `open` to the global Preferences button.

- [ ] **Step 1: Write `preferences_dialog.h`**

Create `tools/psb_demo_app/tui/preferences_dialog.h`:

```cpp
#pragma once

#include "app_preferences.h"
#include "widgets.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include <functional>
#include <memory>
#include <string>

namespace psb::tui {

using namespace ftxui;

// App-level runtime preferences (connection timeout, idle poll interval) —
// distinct from the per-board "Setting" button (board_dashboard.h's
// bSysCfg, which edits board register config, not app behavior). Shown as a
// Modal layered on the switcher root (main.cpp), gated by *showPreferences —
// the same pattern the mid-session Setup wizard's own Modal already uses.
// timeoutMs/pollIntervalS are taken by reference from the caller's own
// globals (main.cpp's g_connectTimeoutMs/g_pollInterval) — this file has no
// global-variable dependency of its own, consistent with mode_select.h.
struct PreferencesDialog {
    Component root;
    std::function<void()> open;  // resets fields from current values, then shows
};

inline PreferencesDialog makePreferencesDialog(ScreenInteractive& screen,
                                               std::shared_ptr<bool> showPreferences,
                                               int& timeoutMs, int& pollIntervalS) {
    auto timeoutVal = std::make_shared<std::string>();
    auto pollVal = std::make_shared<std::string>();

    auto bSave = ActionButton("Save", [&screen, showPreferences, timeoutVal, pollVal, &timeoutMs, &pollIntervalS] {
        // Falls back to the *current* value (not a hardcoded default) on a
        // parse failure, so a stray typo can't silently reset an
        // already-customized setting — same try/catch(stoi) pattern used
        // throughout this codebase (quick-connect's baud/slave, the
        // wizard's Add Bus/Add Board forms).
        try { timeoutMs = std::stoi(*timeoutVal); } catch (...) {}
        try { pollIntervalS = std::stoi(*pollVal); } catch (...) {}
        psb::AppPreferences prefs;
        prefs.timeoutMs = timeoutMs;
        prefs.pollIntervalS = pollIntervalS;
        prefs.save(psb::AppPreferences::defaultPath());
        *showPreferences = false;
        screen.PostEvent(Event::Custom);
    });
    auto bCancel = ActionButton("Cancel", [showPreferences, &screen] {
        *showPreferences = false;
        screen.PostEvent(Event::Custom);
    });

    auto timeoutInp = Input(timeoutVal.get(), "ms");
    auto pollInp = Input(pollVal.get(), "s");

    auto container = Container::Vertical({timeoutInp, pollInp, bSave, bCancel});
    auto root = Renderer(container, [timeoutVal, pollVal, timeoutInp, pollInp, bSave, bCancel] {
        return vbox({
            text(" Preferences ") | bold | center,
            separator(),
            hbox({ text("Timeout        : "), timeoutInp->Render() | size(WIDTH, EQUAL, 8), text(" ms") }),
            hbox({ text("Poll Interval  : "), pollInp->Render()    | size(WIDTH, EQUAL, 8), text(" s")  }),
            separator(),
            hbox({ bSave->Render(), text("  "), bCancel->Render() }) | center,
        }) | border | size(WIDTH, EQUAL, 44);
    });

    auto open = [timeoutVal, pollVal, showPreferences, &screen, &timeoutMs, &pollIntervalS] {
        *timeoutVal = std::to_string(timeoutMs);
        *pollVal = std::to_string(pollIntervalS);
        *showPreferences = true;
        screen.PostEvent(Event::Custom);
    };

    return PreferencesDialog{root, open};
}

} // namespace psb::tui
```

- [ ] **Step 2: Build to confirm it compiles**

Run: `cd tools && cmake --build build --target psb_demo_tui 2>&1 | tail -40`
Expected: FAIL — nothing includes `preferences_dialog.h` yet. This is a **compile-only sanity check**, not functional: temporarily add `#include "preferences_dialog.h"` to the top of `tools/psb_demo_app/tui/main.cpp` (right after the existing `#include "mode_select.h"` line), rebuild, confirm it compiles clean, then remove that temporary include — Task 6 adds the real include and call sites.

Expected after the temporary include: clean build (both `makePreferencesDialog`/`PreferencesDialog` are template-free/`inline`, only instantiated when actually called — an uncalled one produces no warnings, this step mainly catches syntax/type errors).

- [ ] **Step 3: Commit**

```bash
git add tools/psb_demo_app/tui/preferences_dialog.h
git commit -m "feat(psb_demo_tui): add preferences_dialog.h — global Preferences dialog (not yet wired to main.cpp)"
```

---

## Task 4: `board_switcher.h` — Preferences as a third global button

**Files:**
- Modify: `tools/psb_demo_app/tui/board_switcher.h`

**Interfaces:**
- Produces: `makeBoardSwitcher` gains a third `Component globalPreferences` parameter, added to `globalMenuBar`'s children.

- [ ] **Step 1: Update the signature**

Edit `tools/psb_demo_app/tui/board_switcher.h`, find:

```cpp
inline BoardSwitcher makeBoardSwitcher(std::vector<std::unique_ptr<BoardSession>>& boards,
                                       Component globalQuit, Component globalSetup) {
```

Change to:

```cpp
inline BoardSwitcher makeBoardSwitcher(std::vector<std::unique_ptr<BoardSession>>& boards,
                                       Component globalQuit, Component globalSetup,
                                       Component globalPreferences) {
```

- [ ] **Step 2: Add it to `globalMenuBar`**

Find:

```cpp
    auto globalMenuBar = Container::Horizontal({globalQuit, globalSetup});
```

Change to:

```cpp
    auto globalMenuBar = Container::Horizontal({globalQuit, globalSetup, globalPreferences});
```

- [ ] **Step 3: Build to confirm it compiles**

Run: `cd tools && cmake --build build --target psb_demo_tui 2>&1 | tail -40`
Expected: FAIL — `main.cpp`'s existing `makeBoardSwitcher(rt.boards, globalQuit, globalSetup)` call site doesn't pass the third argument yet. Confirm the *only* error is about the missing argument at that one call site — Task 6 updates it.

- [ ] **Step 4: Commit**

```bash
git add tools/psb_demo_app/tui/board_switcher.h
git commit -m "feat(psb_demo_tui): add Preferences as a third global switcher button (not yet wired to main.cpp)"
```

---

## Task 5: `board_dashboard.h` — merge Preferences into the single-board row too

**Files:**
- Modify: `tools/psb_demo_app/tui/board_dashboard.h`

**Interfaces:**
- Produces: `makeBoardDashboard` gains a fourth new parameter, `Component globalPreferences` (inserted between `globalSetup` and `liveBoardCount`). Rendered the same way `globalQuit`/`globalSetup` already are: folded into the single-board merged row, omitted when 2+ boards exist (the switcher's own `globalMenuBar` row renders it instead).

- [ ] **Step 1: Update the signature**

Edit `tools/psb_demo_app/tui/board_dashboard.h`, find:

```cpp
inline Component makeBoardDashboard(BoardSession& board, BusWorker& busWorker,
                                    ScreenInteractive& screen, std::atomic<bool>& running,
                                    int timeoutMs, std::function<void()> openSetup,
                                    std::function<void()> requestRemove,
                                    Component globalQuit, Component globalSetup,
                                    std::function<size_t()> liveBoardCount) {
```

Change to:

```cpp
inline Component makeBoardDashboard(BoardSession& board, BusWorker& busWorker,
                                    ScreenInteractive& screen, std::atomic<bool>& running,
                                    int timeoutMs, std::function<void()> openSetup,
                                    std::function<void()> requestRemove,
                                    Component globalQuit, Component globalSetup,
                                    Component globalPreferences,
                                    std::function<size_t()> liveBoardCount) {
```

- [ ] **Step 2: Update the root Renderer's capture list**

Find:

```cpp
    auto root = Renderer(mainContainer, [&board, &screen, menuModeC, connectedMenuSave, bConnToggle, bRemove,
                                         tabBar, tabContent, bSysCfg, globalQuit, globalSetup, liveBoardCount] {
```

Change to:

```cpp
    auto root = Renderer(mainContainer, [&board, &screen, menuModeC, connectedMenuSave, bConnToggle, bRemove,
                                         tabBar, tabContent, bSysCfg, globalQuit, globalSetup, globalPreferences,
                                         liveBoardCount] {
```

- [ ] **Step 3: Fold Preferences into the single-board merge**

Find:

```cpp
        if (boardCount <= 1) {
            menuBarParts.push_back(text(" "));
            menuBarParts.push_back(globalQuit->Render());
            menuBarParts.push_back(text(" "));
            menuBarParts.push_back(globalSetup->Render());
        }
```

Change to:

```cpp
        if (boardCount <= 1) {
            menuBarParts.push_back(text(" "));
            menuBarParts.push_back(globalQuit->Render());
            menuBarParts.push_back(text(" "));
            menuBarParts.push_back(globalSetup->Render());
            menuBarParts.push_back(text(" "));
            menuBarParts.push_back(globalPreferences->Render());
        }
```

- [ ] **Step 4: Build to confirm it compiles**

Run: `cd tools && cmake --build build --target psb_demo_tui 2>&1 | tail -60`
Expected: FAIL — `main.cpp`'s two existing `makeBoardDashboard(...)` call sites (in `buildRuntime` and `applyNewBoardsLive`) don't pass the fourth argument yet. Confirm the *only* errors are about missing arguments at those two call sites (plus the still-pending `makeBoardSwitcher` error from Task 4) — Task 6 updates all of them.

- [ ] **Step 5: Commit**

```bash
git add tools/psb_demo_app/tui/board_dashboard.h
git commit -m "feat(psb_demo_tui): merge Preferences into single-board's own menu row (not yet wired to main.cpp)"
```

---

## Task 6: `main.cpp` — remove CLI flags, simplify topology resolution, wire Preferences

**Files:**
- Modify: `tools/psb_demo_app/tui/main.cpp`

**Interfaces:**
- Consumes: `psb::AppPreferences::load()` (Task 2), `psb::tui::makePreferencesDialog`/`PreferencesDialog` (Task 3), `makeBoardSwitcher`'s new signature (Task 4), `makeBoardDashboard`'s new signature (Task 5).
- Produces: `g_connectTimeoutMs` global; `bGlobalPreferences`/`prefsDialog` constructed once in `main()`, threaded through `buildRuntime`/`applyNewBoardsLive`/`makeBoardSwitcher`/`onMidSessionFinish`; the simplified 2-branch topology-resolution chain; the CLI surface reduced to `--version`/`--help`.

- [ ] **Step 1: Add the new includes**

Edit `tools/psb_demo_app/tui/main.cpp`, find:

```cpp
#include "wizard_state.h"
#include "wizard_screen.h"
#include "mode_select.h"
#include "tool_version.h"
```

Change to:

```cpp
#include "wizard_state.h"
#include "wizard_screen.h"
#include "mode_select.h"
#include "preferences_dialog.h"
#include "app_preferences.h"
#include "tool_version.h"
```

- [ ] **Step 2: Add `g_connectTimeoutMs`**

Find:

```cpp
static int g_pollInterval = 1;
```

Change to:

```cpp
static int g_pollInterval = 1;
static int g_connectTimeoutMs = 3000;
```

- [ ] **Step 3: Thread `globalPreferences` through `buildRuntime`'s signature**

Find:

```cpp
void buildRuntime(Runtime& rt, const psb::TopologyConfig& topo, ScreenInteractive& screen,
                  std::atomic<bool>& running, int timeoutMs, bool autoConnectAll,
                  std::function<void()> openSetup, Component globalQuit, Component globalSetup) {
```

Change to:

```cpp
void buildRuntime(Runtime& rt, const psb::TopologyConfig& topo, ScreenInteractive& screen,
                  std::atomic<bool>& running, int timeoutMs, bool autoConnectAll,
                  std::function<void()> openSetup, Component globalQuit, Component globalSetup,
                  Component globalPreferences) {
```

- [ ] **Step 4: Thread it through `buildRuntime`'s `makeBoardDashboard` call**

Find:

```cpp
            b->dashboard = psb::tui::makeBoardDashboard(*b, *bw, screen, running, timeoutMs, openSetup,
                [&rt, &screen, &running, bPtr = b.get()] { removeBoardLive(rt, screen, running, bPtr); },
                globalQuit, globalSetup, [&rt] { return rt.boards.size(); });
```

Change to:

```cpp
            b->dashboard = psb::tui::makeBoardDashboard(*b, *bw, screen, running, timeoutMs, openSetup,
                [&rt, &screen, &running, bPtr = b.get()] { removeBoardLive(rt, screen, running, bPtr); },
                globalQuit, globalSetup, globalPreferences, [&rt] { return rt.boards.size(); });
```

- [ ] **Step 5: Thread it through `buildRuntime`'s `makeBoardSwitcher` call**

Find:

```cpp
    rt.switcher = psb::tui::makeBoardSwitcher(rt.boards, globalQuit, globalSetup);
```

Change to:

```cpp
    rt.switcher = psb::tui::makeBoardSwitcher(rt.boards, globalQuit, globalSetup, globalPreferences);
```

- [ ] **Step 6: Thread it through `applyNewBoardsLive`'s signature**

Find:

```cpp
void applyNewBoardsLive(Runtime& rt, const psb::TopologyConfig& newTopo,
                        ScreenInteractive& screen, std::atomic<bool>& running,
                        int timeoutMs, std::function<void()> openSetup,
                        Component globalQuit, Component globalSetup) {
```

Change to:

```cpp
void applyNewBoardsLive(Runtime& rt, const psb::TopologyConfig& newTopo,
                        ScreenInteractive& screen, std::atomic<bool>& running,
                        int timeoutMs, std::function<void()> openSetup,
                        Component globalQuit, Component globalSetup, Component globalPreferences) {
```

- [ ] **Step 7: Thread it through `applyNewBoardsLive`'s `makeBoardDashboard` call**

Find:

```cpp
            b->dashboard = psb::tui::makeBoardDashboard(*b, *targetBw, screen, running, timeoutMs, openSetup,
                [&rt, &screen, &running, bPtr = b.get()] { removeBoardLive(rt, screen, running, bPtr); },
                globalQuit, globalSetup, [&rt] { return rt.boards.size(); });
```

Change to:

```cpp
            b->dashboard = psb::tui::makeBoardDashboard(*b, *targetBw, screen, running, timeoutMs, openSetup,
                [&rt, &screen, &running, bPtr = b.get()] { removeBoardLive(rt, screen, running, bPtr); },
                globalQuit, globalSetup, globalPreferences, [&rt] { return rt.boards.size(); });
```

- [ ] **Step 8: Remove the CLI flags, keep only `--version`**

Find:

```cpp
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
```

Change to:

```cpp
int main(int argc, char** argv) {
    // psb_demo_tui is a double-click-launched UI app (Sub-project C) — the
    // only CLI-level concept left is --version (and CLI11's own --help),
    // both standard conventions even for GUI-first tools. Port/baud/slave,
    // topology path, and the setup wizard are all reachable from the UI
    // itself (the mode-select popup, the wizard's own path field, and the
    // global Setup button — Sub-project B); connection timeout and idle
    // poll interval are reachable via the global Preferences button (see
    // preferences_dialog.h) and persist across launches via AppPreferences.
    CLI::App app{"PSB Demo TUI"};
    app.set_version_flag("--version", std::string("psb_demo_tui ") + TOOL_VERSION_STRING);
    CLI11_PARSE(app, argc, argv);

    if (auto prefs = psb::AppPreferences::load(psb::AppPreferences::defaultPath()); prefs.has_value()) {
        g_connectTimeoutMs = prefs->timeoutMs;
        g_pollInterval = prefs->pollIntervalS;
    }

    const std::string topologyPath = psb::TopologyConfig::defaultPath();

    auto screen = ScreenInteractive::Fullscreen();
```

- [ ] **Step 9: Simplify the topology-resolution chain**

Find:

```cpp
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
    } else if (psb::TopologyConfig::exists(topologyPath)) {
        // Not gated on !setupFlag — --setup on an existing topology must
        // still seed the wizard with it (per the comment below); setupFlag
        // only decides whether the wizard also runs afterward.
        auto loaded = psb::TopologyConfig::load(topologyPath);
        if (!loaded.has_value()) {
            std::cerr << "Topology config error: could not parse " << topologyPath << "\n";
            return 1;
        }
        topo = std::move(*loaded);
        haveTopo = true;
    } else if (topologyExplicit) {
        // Case 3 — file named but missing (exists() already ruled out
        // above): wizard runs regardless of --setup, pre-targeting this path.
    } else if (!setupFlag) {
        // Neither -p nor a resolvable/explicit --topology, and --setup not
        // given: no CLI signal at all resolves what to do. Show the
        // mode-selection popup instead of guessing a hardcoded port (Sub-
        // project B; see mode_select.h and docs/superpowers/specs/
        // 2026-07-21-mode-architecture-design.md). Every other branch in
        // this chain (-p, an existing topology file, --topology naming a
        // missing file, --setup) is untouched — this replaces only the
        // old /dev/ttyUSB0 fallback.
        auto choice = psb::tui::showModeChoicePopup(screen);
        if (choice == psb::tui::ModeChoice::Cancelled) return 0;
        if (choice == psb::tui::ModeChoice::Single) {
            auto quick = psb::tui::showQuickConnectForm(screen);
            if (!quick.has_value()) return 0;
            topo = *quick;
            haveTopo = true;
        }
        // else Multi: leave haveTopo false — the existing runWizard logic
        // below launches the standalone wizard exactly as --setup/case-3
        // already do, with no changes needed here.
    }
    // else: setupFlag is true — always run the wizard next, regardless of
    // what topo/haveTopo currently hold (a pre-existing topology, if any,
    // seeds the wizard for editing rather than starting from empty).

    bool runWizard = setupFlag || !haveTopo;
```

Change to:

```cpp
    // ---- Resolve (or build) the topology. With every CLI flag gone
    //      (Sub-project C), there are only two ways to get here: an
    //      existing topology.toml auto-loads and auto-connects directly; or
    //      (first run, or the file was removed) the mode-selection popup
    //      decides between a single-board quick-connect and the standalone
    //      Setup wizard. See docs/superpowers/specs/
    //      2026-07-21-cli-to-preferences-design.md's main()-simplification
    //      section — this collapses what used to be a 4-branch chain built
    //      around -p/-T/--setup, none of which exist anymore. Editing an
    //      already-existing topology via the wizard before it auto-connects
    //      (--setup's old trick) is no longer reachable from a cold launch
    //      — a direct, accepted consequence of removing that flag; the
    //      wizard is still reachable afterward via the global Setup button,
    //      identical to today's mid-session editing. ----
    psb::TopologyConfig topo;
    bool haveTopo = false;

    if (psb::TopologyConfig::exists(topologyPath)) {
        auto loaded = psb::TopologyConfig::load(topologyPath);
        if (!loaded.has_value()) {
            std::cerr << "Topology config error: could not parse " << topologyPath << "\n";
            return 1;
        }
        topo = std::move(*loaded);
        haveTopo = true;
    } else {
        auto choice = psb::tui::showModeChoicePopup(screen);
        if (choice == psb::tui::ModeChoice::Cancelled) return 0;
        if (choice == psb::tui::ModeChoice::Single) {
            auto quick = psb::tui::showQuickConnectForm(screen);
            if (!quick.has_value()) return 0;
            topo = *quick;
            haveTopo = true;
        }
        // else Multi: leave haveTopo false — runWizard below launches the
        // standalone wizard, exactly as before.
    }

    bool runWizard = !haveTopo;
```

- [ ] **Step 10: Drop the dead `-p` clause from `autoConnectAll`**

Find:

```cpp
    bool autoConnectAll = !portArg.empty() || runWizard || topo.totalBoardCount() > 1;
```

Change to:

```cpp
    bool autoConnectAll = runWizard || topo.totalBoardCount() > 1;
```

- [ ] **Step 11: Construct `bGlobalPreferences`/`prefsDialog`, thread `g_connectTimeoutMs` into `buildRuntime`**

Find:

```cpp
    Runtime rt;

    // Constructed once, not once per board — Quit notifies every bus's
    // worker (not just one board's, unlike the old per-board version this
    // replaces), so quitting wakes every bus thread immediately rather
    // than letting some wait out their idle poll interval. Setup reuses
    // the existing openSetup closure unchanged.
    auto bGlobalQuit = psb::tui::ActionButton("Quit", [&running, &rt, &screen] {
        running = false;
        for (auto& bw : rt.busWorkers) bw->workCv.notify_all();
        screen.ExitLoopClosure()();
    });
    auto bGlobalSetup = psb::tui::ActionButton("Setup", [openSetup] { openSetup(); });

    buildRuntime(rt, topo, screen, running, timeoutArg, autoConnectAll, openSetup, bGlobalQuit, bGlobalSetup);
```

Change to:

```cpp
    Runtime rt;

    // Constructed once, not once per board — Quit notifies every bus's
    // worker (not just one board's, unlike the old per-board version this
    // replaces), so quitting wakes every bus thread immediately rather
    // than letting some wait out their idle poll interval. Setup reuses
    // the existing openSetup closure unchanged.
    auto bGlobalQuit = psb::tui::ActionButton("Quit", [&running, &rt, &screen] {
        running = false;
        for (auto& bw : rt.busWorkers) bw->workCv.notify_all();
        screen.ExitLoopClosure()();
    });
    auto bGlobalSetup = psb::tui::ActionButton("Setup", [openSetup] { openSetup(); });

    auto showPreferences = std::make_shared<bool>(false);
    auto prefsDialog = psb::tui::makePreferencesDialog(screen, showPreferences,
                                                        g_connectTimeoutMs, g_pollInterval);
    auto bGlobalPreferences = psb::tui::ActionButton("Preferences", prefsDialog.open);

    buildRuntime(rt, topo, screen, running, g_connectTimeoutMs, autoConnectAll, openSetup,
                bGlobalQuit, bGlobalSetup, bGlobalPreferences);
```

- [ ] **Step 12: Thread `bGlobalPreferences` and `g_connectTimeoutMs` through `onMidSessionFinish`**

Find:

```cpp
    auto onMidSessionFinish = [showSetup, midSessionWiz, &rt, &topo, &screen, &running, timeoutArg, openSetup,
                               bGlobalQuit, bGlobalSetup]
                              (psb::tui::WizardOutcome outcome) {
        if (outcome == psb::tui::WizardOutcome::ConnectNow) {
            // Tear down what's gone before attaching what's new — the two
            // sets are always disjoint (a board can't be both removed and
            // newly-added in the same edit), so the order between them
            // doesn't affect correctness, but removing first reads slightly
            // more naturally. topo is overwritten immediately below; the
            // live teardown/attach these two calls kick off both finish
            // asynchronously a frame or two later (removeBoardLive's
            // staged hand-off, applyNewBoardsLive's queued connect) — topo
            // is already correct by the time either one completes.
            removeGoneBoardsLive(rt, midSessionWiz->topo, screen, running);
            applyNewBoardsLive(rt, midSessionWiz->topo, screen, running, timeoutArg, openSetup,
                               bGlobalQuit, bGlobalSetup);
            topo = midSessionWiz->topo;
        }
        *showSetup = false;
        screen.PostEvent(Event::Custom);
    };
```

Change to:

```cpp
    auto onMidSessionFinish = [showSetup, midSessionWiz, &rt, &topo, &screen, &running, openSetup,
                               bGlobalQuit, bGlobalSetup, bGlobalPreferences]
                              (psb::tui::WizardOutcome outcome) {
        if (outcome == psb::tui::WizardOutcome::ConnectNow) {
            // Tear down what's gone before attaching what's new — the two
            // sets are always disjoint (a board can't be both removed and
            // newly-added in the same edit), so the order between them
            // doesn't affect correctness, but removing first reads slightly
            // more naturally. topo is overwritten immediately below; the
            // live teardown/attach these two calls kick off both finish
            // asynchronously a frame or two later (removeBoardLive's
            // staged hand-off, applyNewBoardsLive's queued connect) — topo
            // is already correct by the time either one completes.
            removeGoneBoardsLive(rt, midSessionWiz->topo, screen, running);
            applyNewBoardsLive(rt, midSessionWiz->topo, screen, running, g_connectTimeoutMs, openSetup,
                               bGlobalQuit, bGlobalSetup, bGlobalPreferences);
            topo = midSessionWiz->topo;
        }
        *showSetup = false;
        screen.PostEvent(Event::Custom);
    };
```

- [ ] **Step 13: Layer the Preferences dialog as a second `Modal`**

Find:

```cpp
    auto rootWithSetup = Renderer(rt.switcher.root, [&rt, &topo, &screen, &running] {
        drainPendingRemovals(rt, topo, screen, running);
        return rt.switcher.root->Render();
    }) | Modal(midSessionWizardRoot, showSetup.get());

    screen.Loop(rootWithSetup);
```

Change to:

```cpp
    auto rootWithSetup = Renderer(rt.switcher.root, [&rt, &topo, &screen, &running] {
        drainPendingRemovals(rt, topo, screen, running);
        return rt.switcher.root->Render();
    }) | Modal(midSessionWizardRoot, showSetup.get())
       | Modal(prefsDialog.root, showPreferences.get());

    screen.Loop(rootWithSetup);
```

- [ ] **Step 14: Build**

Run: `cd tools && cmake --build build --target psb_demo_tui psb_tests -j$(nproc) 2>&1 | tail -60`
Expected: clean build.

- [ ] **Step 15: `psb_tests` unaffected**

Run: `./build/psb_modbus_core/tests/psb_tests`
Expected: PASS, same 4-test increase over the pre-Task-1 baseline as Task 2 left it (this task touches no `psb_modbus_core` files).

- [ ] **Step 16: `--version` still works, removed flags are now rejected**

Run: `./bin/psb_demo_tui --version` — expected: prints `psb_demo_tui v...` and exits, no UI shown.
Run: `./bin/psb_demo_tui -p /dev/ttyUSB0` — expected: CLI11's standard "unknown option" error (`-p` no longer registered), non-zero exit, no UI shown.

- [ ] **Step 17: Manual verification — every remaining path via tmux**

Following `docs/guide/client-architecture-and-pitfalls.md` §3's methodology (move `~/.psb_demo_app/topology.toml` aside first if one exists from earlier testing, and restore it afterward per Step 19 below):

1. **Fresh launch, no topology file**: confirm the mode-selection popup still appears (unchanged from Sub-project B).
2. **Single Board → quick-connect → dashboard**: confirm the merged single row now shows *three* global controls folded in — `[ Quit ] [ Setup ] [ Preferences ]` — alongside the board's own controls, still no `Remove`.
3. **Click Preferences**: confirm the dialog opens pre-filled with `3000`/`1` (defaults, since no `preferences.toml` exists yet); change Timeout to `5000` and Poll Interval to `2`; click Save; confirm `~/.psb_demo_app/preferences.toml` now contains `timeout_ms = 5000` and `poll_interval_s = 2`.
4. **Reopen Preferences**: confirm it now pre-fills `5000`/`2` (the just-saved live values, not reloaded from disk — same object, same session).
5. **Relaunch the app entirely**: confirm a fresh Preferences open shows `5000`/`2` (persisted across the process restart, loaded from `preferences.toml` at startup).
6. **Poll interval takes effect live**: with a single board connected, open Preferences, drop Poll Interval to a distinctly different value (e.g. `1` if it's currently `2`), Save — no code change needed here to observe it, since `g_pollInterval` is read fresh every idle cycle; this is a sanity confirmation the wiring is live, not a new behavior.
7. **Cancel discards**: open Preferences, change Timeout to some other value, click Cancel; confirm reopening shows the last *saved* values, not the discarded edit.
8. **Multi-board**: load or build a 2-board topology (matching Sub-project B's own manual test setup); confirm the global row now shows all three buttons (`Quit`/`Setup`/`Preferences`) as their own separate row, and the per-board row no longer includes any of them.
9. **Existing topology file, no flags**: with `topology.toml` present, confirm the app goes straight to the dashboard with no popup (unchanged from Sub-project B, now flag-free by construction since there's no `--setup` to test against).

- [ ] **Step 18: Commit**

```bash
git add tools/psb_demo_app/tui/main.cpp
git commit -m "feat(psb_demo_tui): remove CLI flags except --version, wire the global Preferences dialog"
```

- [ ] **Step 19: Clean up any temporary/moved files from manual verification**

Restore any topology file moved aside in Step 17's setup back to its original location. Remove `~/.psb_demo_app/preferences.toml` if it was created purely for this manual test pass and wasn't already a real user preference (it wasn't, in a fresh test environment).

---

## Task 7: Full-repo verification

**Files:** none (verification only).

- [ ] **Step 1: Clean rebuild of every touched target**

Run: `cd tools && cmake --build build --target psb_modbus_core psb_tests psb_demo_cli psb_demo_tui 2>&1 | tail -40`
Expected: clean build.

- [ ] **Step 2: Full test suite**

Run: `./build/psb_modbus_core/tests/psb_tests`
Expected: PASS, 4 more test cases than this plan's starting baseline (Task 2's `[app_preferences]` tests — Task 1 is a pure refactor with no new tests, Tasks 3–6 add no automated tests, matching this codebase's established precedent that FTXUI/threading-coupled UI code is verified via tmux, not Catch2).

- [ ] **Step 3: Confirm `psb_demo_cli` is untouched**

Run: `./bin/psb_demo_cli --help 2>&1 | head -5`
Expected: unchanged output, still shows its own independent `-t,--timeout` among its flags — confirming this plan never touched `tools/psb_demo_app/cli/`.

- [ ] **Step 4: Re-run the full manual verification sequence once, end to end, against real hardware if available**

Repeat Task 6 Step 17's full sequence in one continuous pass where practical (fresh launch → single-board quick-connect → open/edit/save Preferences → relaunch to confirm persistence → promote to multi-board via Setup → confirm three-button global row), confirming nothing regresses when these transitions happen back to back rather than as isolated relaunches.

- [ ] **Step 5: Clean up any remaining temporary/moved files from manual verification**

Same as Task 6 Step 19 — restore any moved topology file, remove any preferences.toml created purely for testing.
