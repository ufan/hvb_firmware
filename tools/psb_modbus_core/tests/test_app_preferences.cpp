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
