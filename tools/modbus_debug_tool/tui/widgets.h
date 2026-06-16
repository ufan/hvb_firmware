#pragma once
#include "tui_format.h"
#include "hvb_modbus_client.h"
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace ftxui;

namespace hvb::tui {

struct AppState {
    hvb::HvbModbusClient&     client;
    std::atomic<bool>&        connected;
    ScannedData&              data;
    std::string&              statusMsg;
    ftxui::ScreenInteractive& screen;
};

// Dispatch fn on a background thread; update statusMsg on completion.
// fn must not capture any stack locals that may be destroyed before it runs.
inline void writeAsync(AppState& s, const std::string& label, std::function<bool()> fn) {
    s.statusMsg = "Writing " + label + "...";
    s.screen.PostEvent(Event::Custom);
    std::thread([&s, label, fn = std::move(fn)]() mutable {
        bool ok = fn();
        s.statusMsg = ok ? ("OK: " + label) : ("Error: " + s.client.lastError());
        s.screen.PostEvent(Event::Custom);
    }).detach();
}

// Input that calls onCommit when Enter is pressed (instead of inserting newline).
inline Component CommitInput(std::string* val,
                             const std::string& placeholder,
                             std::function<void()> onCommit) {
    auto inp = Input(val, placeholder);
    return CatchEvent(inp, [onCommit](Event e) {
        if (e == Event::Return) { onCommit(); return true; }
        return false;
    });
}

// Button-backed inline dropdown.
// Renders as "[current ▾]". Space/←/→ cycles; Enter commits.
// Uses Button so it participates in FTXUI's Tab-focus chain.
inline Component InlineCycler(std::vector<std::string> opts,
                               int* sel,
                               std::function<void()> onCommit) {
    auto optsPtr = std::make_shared<std::vector<std::string>>(std::move(opts));
    auto bopt    = ButtonOption{};
    bopt.transform = [sel, optsPtr](const EntryState& es) -> Element {
        std::string lbl = "[" + optsPtr->at(*sel) + " \xe2\x96\xbe]"; // UTF-8 ▾
        auto e = text(lbl);
        if (es.focused) e = e | inverted;
        return e;
    };
    // onClick: cycle forward (mouse-click support)
    auto btn = Button("", [sel, optsPtr] { *sel = (*sel + 1) % (int)optsPtr->size(); }, bopt);
    return CatchEvent(btn, [sel, optsPtr, onCommit](Event e) {
        int n = (int)optsPtr->size();
        if (e == Event::Character(' ') || e == Event::ArrowRight) {
            *sel = (*sel + 1) % n; return true;
        }
        if (e == Event::ArrowLeft) {
            *sel = (*sel - 1 + n) % n; return true;
        }
        if (e == Event::Return) { onCommit(); return true; }
        return false;
    });
}

// Styled action button: "[ label ]", inverted when focused.
inline Component ActionButton(const std::string& label, std::function<void()> onClick) {
    auto bopt = ButtonOption{};
    bopt.transform = [](const EntryState& es) -> Element {
        auto e = text("[ " + es.label + " ]");
        if (es.focused) e = e | inverted;
        return e;
    };
    return Button(label, std::move(onClick), bopt);
}

} // namespace hvb::tui
