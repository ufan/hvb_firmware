#pragma once

#include "board_session.h"
#include "group_alias_status.h"
#include "tab_monitor.h"
#include "topology_config.h"

#include <ftxui/component/component.hpp>
#include <ftxui/dom/table.hpp>

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace psb::tui {

using namespace ftxui;

using SaveGroupName = std::function<std::string(const std::string&, const std::string&)>;

inline std::string groupNameSaveStatus(const std::string& err) {
    return err.empty() ? "OK: group renamed" : "Error: " + err;
}

inline bool groupStatusIsWarningOrError(const psb::MessageRecord& record) {
    return record.severity == psb::MessageSeverity::Warning ||
           record.severity == psb::MessageSeverity::Error;
}

inline std::optional<psb::MessageRecord>
selectGroupStatus(const std::optional<psb::MessageRecord>& localStatus,
                  const std::vector<std::optional<psb::MessageRecord>>& memberStatuses) {
    if (localStatus)
        return localStatus;
    for (const auto& status : memberStatuses)
        if (status && groupStatusIsWarningOrError(*status))
            return status;
    for (const auto& status : memberStatuses)
        if (status)
            return status;
    return std::nullopt;
}

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
                                    std::function<void(const std::string&, int)> jumpToBoard,
                                    std::function<std::string(const std::string&, int, const std::string&)> saveGroupAlias = {},
                                    SaveGroupName saveGroupName = {}) {
    struct MemberRow {
        psb::GroupChannelRef ref;
        BoardSession* board = nullptr;  // nullptr if the board doesn't exist at all
        MonitorRow row;                 // only meaningful when board != nullptr
        std::shared_ptr<std::string> alias;
        Component aliasInp;
        Component jumpBtn;              // only meaningful when board != nullptr
    };
    auto memberRows = std::make_shared<std::vector<MemberRow>>();
    auto messages = std::make_shared<psb::MessageCenter>();
    Components rowComps;
    auto titleName = std::make_shared<std::string>(groupName);
    auto titleInp = CommitInput(titleName.get(), "group name",
                                [saveGroupName, titleName, messages, previousName = groupName] {
                                    if (!saveGroupName) return;
                                    std::string err = saveGroupName(previousName, *titleName);
                                    if (!err.empty())
                                        *titleName = previousName;
                                    auto action = messages->beginAction("group");
                                    messages->publish(action,
                                                      err.empty() ? psb::MessageSeverity::Success
                                                                  : psb::MessageSeverity::Error,
                                                      "group", groupNameSaveStatus(err));
                                });
    rowComps.push_back(titleInp);

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
        mr.alias = std::make_shared<std::string>(
            ref.alias.empty() ? psb::defaultChannelAlias(ref.channelIndex) : ref.alias);
        mr.aliasInp = CommitInput(mr.alias.get(), psb::defaultChannelAlias(ref.channelIndex),
                                  [saveGroupAlias, alias = mr.alias, messages,
                                   nickname = ref.boardNickname, ch = ref.channelIndex,
                                   previousAlias = *mr.alias] {
                                      if (!saveGroupAlias) return;
                                      std::string err = saveGroupAlias(nickname, ch, *alias);
                                      if (!err.empty())
                                          *alias = previousAlias;
                                      auto action = messages->beginAction("group");
                                      messages->publish(action,
                                                        err.empty() ? psb::MessageSeverity::Success
                                                                    : psb::MessageSeverity::Error,
                                                        "group", groupAliasSaveStatus(err));
                                  });
        rowComps.push_back(mr.aliasInp);
        if (owner) {
            mr.row = makeMonitorRow(*owner->appState, owner->inputs, ref.channelIndex);
            std::string label = ref.boardNickname + "/" + psb::defaultChannelAlias(ref.channelIndex);
            mr.jumpBtn = MouseOnlyActionButton(label, [jumpToBoard, nickname = ref.boardNickname, ch = ref.channelIndex] {
                jumpToBoard(nickname, ch);
            });
            rowComps.push_back(mr.row.row);
            rowComps.push_back(mr.jumpBtn);
        }
        memberRows->push_back(std::move(mr));
    }

    auto tableContainer = Container::Vertical(rowComps);

    return Renderer(tableContainer, [=] {
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
        MonitorRenderOptions groupOpts{MonitorIdentityMode::GroupAlias, "Name"};
        auto headerLabels = monitorHeaderLabels(*headerSource, groupOpts);
        headerLabels.push_back("Go");

        std::vector<std::vector<Element>> grid;
        {
            std::vector<Element> hdr;
            for (const auto& h : headerLabels)
                hdr.push_back(text(h) | bold | center);
            grid.push_back(std::move(hdr));
        }

        for (const auto& mr : *memberRows) {
            bool online = mr.board && mr.board->connected.load() && mr.board->data.valid &&
                          !monitorChannelOffline(mr.board->data, mr.ref.channelIndex);
            Element identity = mr.aliasInp->Render();
            if (!online) {
                auto cells = monitorOfflineRowCells(identity, headerLabels.size() - 1);
                cells.push_back(text("--") | color(Color::Red) | dim | center);
                grid.push_back(std::move(cells));
                continue;
            }
            auto cells = monitorRowCells(identity, mr.board->data, mr.ref.channelIndex, mr.row);
            cells.push_back(mr.jumpBtn->Render() | center);
            grid.push_back(std::move(cells));
        }

        int onlineCount = 0;
        std::vector<std::optional<psb::MessageRecord>> memberStatuses;
        std::string fallbackStatusMsg;
        for (const auto& mr : *memberRows) {
            bool online = mr.board && mr.board->connected.load() && mr.board->data.valid &&
                          !monitorChannelOffline(mr.board->data, mr.ref.channelIndex);
            if (online) ++onlineCount;

            if (!mr.board) {
                psb::MessageRecord missing;
                missing.severity = psb::MessageSeverity::Error;
                missing.source = "group";
                missing.text = "Error: board " + mr.ref.boardNickname + " not attached";
                memberStatuses.push_back(missing);
                continue;
            }

            if (auto boardStatus = mr.board->messages.currentStatus(); boardStatus.has_value()) {
                memberStatuses.push_back(boardStatus);
                continue;
            }

            std::string boardMsg;
            {
                std::lock_guard<std::mutex> lk(mr.board->statusMutex);
                boardMsg = mr.board->statusMsg;
            }
            if (boardMsg.rfind("Error", 0) == 0) {
                psb::MessageRecord legacy;
                legacy.severity = psb::MessageSeverity::Error;
                legacy.source = mr.ref.boardNickname;
                legacy.text = mr.ref.boardNickname + ": " + boardMsg;
                memberStatuses.push_back(legacy);
                continue;
            }
            if (fallbackStatusMsg.empty() && !boardMsg.empty())
                fallbackStatusMsg = mr.ref.boardNickname + ": " + boardMsg;
        }
        std::string statusMsg;
        bool isErr = false;
        if (auto selected = selectGroupStatus(messages->currentStatus(), memberStatuses); selected.has_value()) {
            statusMsg = selected->text;
            isErr = groupStatusIsWarningOrError(*selected);
        }
        if (statusMsg.empty())
            statusMsg = fallbackStatusMsg;
        if (statusMsg.empty())
            statusMsg = members.empty() ? "No channels in this group" : "Group ready";

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
            titleInp->Render() | bold | center,
            table.Render(),
            filler(),
            separator(),
            statusBarEl,
        });
    });
}

} // namespace psb::tui
