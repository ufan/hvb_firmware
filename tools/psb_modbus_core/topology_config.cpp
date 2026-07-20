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
