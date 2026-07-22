#pragma once

#include "topology_config.h"
#include "widgets.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace psb::tui {

using namespace ftxui;

// Mouse-driven directory browser for the Topology wizard's Path field —
// ".." first (unless already at a filesystem root), then subdirectories
// alphabetical, then *.toml files alphabetical. Open on a directory entry
// navigates into it; Open on a file entry sets targetPath to its full
// path, calls onFileSelected() (the caller reuses its own Load logic —
// this file never needs to know about WizardState/TopologyConfig
// loading), and closes.
struct PathPicker {
    Component root;
    std::function<void()> open;  // starts browsing at targetPath's parent dir, then shows
};

inline PathPicker makePathPicker(ScreenInteractive& screen,
                                 std::shared_ptr<bool> showPicker,
                                 std::string& targetPath) {
    auto currentDir = std::make_shared<std::string>();
    auto entries = std::make_shared<std::vector<std::string>>();
    auto entryIsDir = std::make_shared<std::vector<bool>>();
    auto entryFullPath = std::make_shared<std::vector<std::string>>();
    auto entryIdx = std::make_shared<int>(0);
    auto status = std::make_shared<std::string>();

    auto rebuild = [currentDir, entries, entryIsDir, entryFullPath, entryIdx, status] {
        namespace fs = std::filesystem;
        entries->clear();
        entryIsDir->clear();
        entryFullPath->clear();
        status->clear();

        fs::path dir(*currentDir);
        if (dir.has_parent_path() && dir != dir.root_path()) {
            entries->push_back("..");
            entryIsDir->push_back(true);
            entryFullPath->push_back(dir.parent_path().string());
        }

        std::vector<std::pair<std::string, std::string>> dirs, files;
        try {
            for (const auto& e : fs::directory_iterator(dir)) {
                if (e.is_directory()) {
                    dirs.push_back({e.path().filename().string() + "/", e.path().string()});
                } else if (e.is_regular_file() && e.path().extension() == ".toml") {
                    files.push_back({e.path().filename().string(), e.path().string()});
                }
            }
        } catch (const std::exception& ex) {
            *status = "Error: " + std::string(ex.what());
        }
        std::sort(dirs.begin(), dirs.end());
        std::sort(files.begin(), files.end());
        for (auto& entry : dirs) {
            entries->push_back(entry.first); entryIsDir->push_back(true); entryFullPath->push_back(entry.second);
        }
        for (auto& entry : files) {
            entries->push_back(entry.first); entryIsDir->push_back(false); entryFullPath->push_back(entry.second);
        }
        *entryIdx = 0;
    };

    auto entriesMenu = Menu(entries.get(), entryIdx.get());

    // Deliberately does not load the selected file — only sets the path
    // field and closes. The picker may also be used to choose a save
    // location by picking an arbitrary existing file as a landing spot and
    // then hand-editing the filename, and the picked file might not even
    // be a valid topology file; Load stays an explicit, separate step the
    // wizard's own Load button already provides.
    auto bOpen = ActionButton("Open", [entries, entryIsDir, entryFullPath, entryIdx,
                                       currentDir, rebuild, showPicker, &targetPath, &screen] {
        int i = *entryIdx;
        if (i < 0 || i >= static_cast<int>(entries->size())) return;
        if ((*entryIsDir)[i]) {
            *currentDir = (*entryFullPath)[i];
            rebuild();
            screen.PostEvent(Event::Custom);
            return;
        }
        targetPath = (*entryFullPath)[i];
        *showPicker = false;
        screen.PostEvent(Event::Custom);
    });
    auto bCancel = ActionButton("Cancel", [showPicker, &screen] {
        *showPicker = false;
        screen.PostEvent(Event::Custom);
    });

    auto container = Container::Vertical({entriesMenu, bOpen, bCancel});
    auto root = Renderer(container, [currentDir, entries, entriesMenu, status, bOpen, bCancel] {
        Element listEl = entries->empty()
            ? text("(empty)") | dim
            : entriesMenu->Render() | frame | size(HEIGHT, LESS_THAN, 12);
        Elements body = {
            text(" Select Topology File ") | bold | center,
            separator(),
            text(*currentDir) | dim,
            listEl,
        };
        if (!status->empty()) body.push_back(text(*status) | color(Color::Red));
        body.push_back(separator());
        body.push_back(hbox({ bOpen->Render(), text("  "), bCancel->Render() }) | center);
        return vbox(std::move(body)) | border | size(WIDTH, EQUAL, 56);
    });

    auto open = [currentDir, rebuild, showPicker, &targetPath, &screen] {
        namespace fs = std::filesystem;
        fs::path startDir;
        if (!targetPath.empty()) {
            fs::path p(targetPath);
            if (p.has_parent_path()) startDir = p.parent_path();
        }
        if (startDir.empty() || !fs::exists(startDir)) {
            startDir = fs::path(psb::TopologyConfig::defaultPath()).parent_path();
        }
        *currentDir = startDir.string();
        rebuild();
        *showPicker = true;
        screen.PostEvent(Event::Custom);
    };

    return PathPicker{root, open};
}

} // namespace psb::tui
