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

TEST_CASE("TopologyConfig — lastSingleConnectPath differs from defaultPath", "[topology_config]") {
    CHECK(psb::TopologyConfig::lastSingleConnectPath() != psb::TopologyConfig::defaultPath());
}
