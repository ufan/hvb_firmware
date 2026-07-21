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

    // cursor_position defaults to an internally-owned int starting at 0 and
    // is never touched by code that mutates the bound string directly
    // (as open() does below, bypassing Input's own edit path entirely) —
    // left unbound, a freshly-populated multi-character value renders with
    // the cursor stuck mid-string, which visibly corrupts the layout for
    // anything longer than a single character (found via manual tmux
    // testing: "1" rendered fine, "3000" wrapped onto its own line).
    // Binding it explicitly and resetting it to end-of-string in open()
    // keeps it in sync with the string every time the dialog reopens.
    auto cursorTimeout = std::make_shared<int>(0);
    auto cursorPoll = std::make_shared<int>(0);

    InputOption timeoutOpt;
    timeoutOpt.placeholder = "ms";
    timeoutOpt.cursor_position = cursorTimeout.get();
    auto timeoutInp = Input(timeoutVal.get(), timeoutOpt);

    InputOption pollOpt;
    pollOpt.placeholder = "s";
    pollOpt.cursor_position = cursorPoll.get();
    auto pollInp = Input(pollVal.get(), pollOpt);

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

    auto open = [timeoutVal, pollVal, cursorTimeout, cursorPoll, showPreferences, &screen, &timeoutMs, &pollIntervalS] {
        *timeoutVal = std::to_string(timeoutMs);
        *pollVal = std::to_string(pollIntervalS);
        *cursorTimeout = static_cast<int>(timeoutVal->size());
        *cursorPoll = static_cast<int>(pollVal->size());
        *showPreferences = true;
        screen.PostEvent(Event::Custom);
    };

    return PreferencesDialog{root, open};
}

} // namespace psb::tui
