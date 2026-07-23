#pragma once

#include "board_session.h"
#include "tab_monitor.h"
#include "topology_config.h"

#include <ftxui/component/component.hpp>
#include <ftxui/dom/table.hpp>

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace psb::tui {

using namespace ftxui;

// Builds one group's aggregating Monitor-style table — one row per member
// channel, each built via the same makeMonitorRow() a board's own Monitor
// tab uses, bound to that member's real owning BoardSession's AppState/
// ConfigInputs. Editing a cell here writes through to the real board exactly
// as editing it on the board's own tab would: both are separate widget
// instances over the same underlying data, the same pattern ConfigInputs
// already establishes between a single board's Monitor and Channel tabs
// (see widgets.h) — "a view into the same underlying board channels," never
// a second poll or a second copy of the data.
//
// A member gets real, live-bound widgets whenever its owning board exists in
// `boards` at construction time (regardless of whether it happens to be
// connected right now) — mirroring how a board's own Monitor tab keeps a
// channel's widgets even after that channel goes offline, rather than
// discarding them. Each render then decides, fresh, whether that member is
// currently online (owning board connected, data valid, channel index still
// in range) or should show the offline placeholder — so a board connecting
// or disconnecting is reflected immediately without rebuilding anything. A
// member whose board doesn't exist in `boards` at all (removed from the
// topology entirely) gets no widgets — always the static offline
// placeholder.
//
// Critical lifetime invariant: each member row's widgets are bound, at
// construction, to whichever BoardSession currently owns that member's
// nickname in `boards`. If that BoardSession is later erased from `boards`
// (a board removed from the running session), those references dangle. This
// function does not solve that itself — main.cpp's refreshGroupDashboards()
// tears down and rebuilds every group dashboard fresh any time `boards` or
// `topo.groups` can change while the app is running. A Component built here
// is therefore only valid until the next such rebuild — never hold on to
// one past that point.
//
// jumpToBoard performs the compound "Go" action: switch the shared switcher
// selection to the member's board and set that board's own tab bar to the
// member's Channel tab.
inline Component makeGroupDashboard(const std::string& groupName,
                                    const std::vector<psb::GroupChannelRef>& members,
                                    std::vector<std::unique_ptr<BoardSession>>& boards,
                                    std::function<void(const std::string&, int)> jumpToBoard) {
    struct MemberRow {
        psb::GroupChannelRef ref;
        BoardSession* board = nullptr;  // nullptr if the board doesn't exist at all
        MonitorRow row;                 // only meaningful when board != nullptr
        Component jumpBtn;              // only meaningful when board != nullptr
    };
    auto memberRows = std::make_shared<std::vector<MemberRow>>();
    Components rowComps;

    // Safe without a lock on rt.boardsMutex — this scan, like board_switcher.h's
    // own by-nickname status-dot lookup, only ever runs on the UI thread, and
    // every mutation of `boards` (add/remove) is also UI-thread-only, so the
    // two can never interleave.
    for (const auto& ref : members) {
        BoardSession* owner = nullptr;
        for (auto& b : boards)
            if (b->nickname == ref.boardNickname) { owner = b.get(); break; }

        MemberRow mr;
        mr.ref = ref;
        mr.board = owner;
        if (owner) {
            mr.row = makeMonitorRow(*owner->appState, owner->inputs, ref.channelIndex, owner->saveChannelAlias);
            ButtonOption bopt{};
            bopt.transform = [owner, ch = ref.channelIndex](const EntryState& es) -> Element {
                // Re-read the alias fresh every render (not baked in at
                // construction) — it can change via this same row's own
                // aliasInp, or the board's own Monitor/Channel tab, at any
                // time after this button is built.
                std::string label = owner->inputs.chAlias[ch].empty()
                    ? "CH" + std::to_string(ch) : owner->inputs.chAlias[ch];
                auto e = text("[ " + label + " ]");
                if (es.focused) e = e | inverted;
                return e;
            };
            mr.jumpBtn = Button("", [jumpToBoard, nickname = ref.boardNickname, ch = ref.channelIndex] {
                jumpToBoard(nickname, ch);
            }, bopt);
            rowComps.push_back(mr.row.row);
            rowComps.push_back(mr.jumpBtn);
        }
        memberRows->push_back(std::move(mr));
    }

    auto tableContainer = Container::Vertical(rowComps);

    return Renderer(tableContainer, [=] {
        if (members.empty()) {
            return vbox({
                text(" No channels in this group yet \xe2\x80\x94 use the Group wizard to add some ") | center | dim,
                filler(),
            });
        }

        // Header labels are capability-derived (see monitorHeaderLabels) —
        // use whichever member's board happens to be connected first as the
        // source; if none are connected right now, the generic "Vset/En"
        // fallback shows, same as a just-connecting board's own Monitor tab.
        ScannedData emptyData;
        const ScannedData* headerSource = &emptyData;
        for (const auto& mr : *memberRows) {
            if (mr.board && mr.board->connected.load() && mr.board->data.valid) {
                headerSource = &mr.board->data;
                break;
            }
        }
        auto headerLabels = monitorHeaderLabels(*headerSource);
        headerLabels.push_back("Go");

        std::vector<std::vector<Element>> grid;
        {
            std::vector<Element> hdr;
            for (const auto& h : headerLabels)
                hdr.push_back(text(h) | bold | center);
            grid.push_back(std::move(hdr));
        }

        for (const auto& mr : *memberRows) {
            std::string chLabel = mr.ref.boardNickname + " CH" + std::to_string(mr.ref.channelIndex);
            bool online = mr.board && mr.board->connected.load() && mr.board->data.valid
                        && mr.ref.channelIndex < mr.board->data.numChannels();
            if (!online) {
                auto cells = monitorOfflineRowCells(
                    chLabel, mr.board ? mr.row.aliasInp : Component{}, chLabel, headerLabels.size() - 1);
                cells.push_back(text("--") | color(Color::Red) | dim | center);
                grid.push_back(std::move(cells));
                continue;
            }
            auto cells = monitorRowCells(chLabel, mr.board->data, mr.ref.channelIndex, mr.row);
            cells.push_back(mr.jumpBtn->Render() | center);
            grid.push_back(std::move(cells));
        }

        int onlineCount = 0;
        std::string statusMsg;
        for (const auto& mr : *memberRows) {
            bool online = mr.board && mr.board->connected.load() && mr.board->data.valid
                        && mr.ref.channelIndex < mr.board->data.numChannels();
            if (online) ++onlineCount;

            if (!mr.board) {
                if (statusMsg.empty()) statusMsg = "Error: board " + mr.ref.boardNickname + " not attached";
                continue;
            }

            std::string boardMsg;
            {
                std::lock_guard<std::mutex> lk(mr.board->statusMutex);
                boardMsg = mr.board->statusMsg;
            }
            if (boardMsg.rfind("Error", 0) == 0) {
                statusMsg = mr.ref.boardNickname + ": " + boardMsg;
                break;
            }
            if (statusMsg.empty() && !boardMsg.empty())
                statusMsg = mr.ref.boardNickname + ": " + boardMsg;
        }
        if (statusMsg.empty())
            statusMsg = members.empty() ? "No channels in this group" : "Group ready";

        bool isErr = statusMsg.rfind("Error", 0) == 0
                  || statusMsg.find(": Error") != std::string::npos;
        auto statusBarEl = hbox({
            text(" " + statusMsg + " ") | (isErr ? color(Color::Red) : color(Color::Green))
                                       | size(WIDTH, GREATER_THAN, 30),
            filler(),
            text(" Members:" + std::to_string(onlineCount) + "/" + std::to_string(members.size()) + " online "),
        });

        auto table = Table(std::move(grid));
        table.SelectAll().Separator(LIGHT);
        table.SelectRow(0).Decorate(bold);
        table.SelectRow(0).SeparatorVertical(LIGHT);
        table.SelectRow(0).Border(DOUBLE);
        for (size_t c = 0; c < headerLabels.size(); ++c)
            table.SelectColumn(static_cast<int>(c)).Decorate(flex);

        return vbox({
            text(" " + groupName + " ") | bold | center,
            table.Render(),
            filler(),
            separator(),
            statusBarEl,
        });
    });
}

} // namespace psb::tui
