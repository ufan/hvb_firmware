#include "message_log.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("MessageCenter - starting a new action replaces stale error status", "[message_log]") {
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

TEST_CASE("MessageCenter - empty action start clears stale status but preserves log", "[message_log]") {
    psb::MessageCenter messages;
    auto a1 = messages.beginAction("board1", "Connecting...");
    messages.publish(a1, psb::MessageSeverity::Error, "board1", "Error: timeout");

    auto a2 = messages.beginAction("board1");

    CHECK(a2 != a1);
    CHECK_FALSE(messages.currentStatus().has_value());
    CHECK(messages.records().size() == 2);
    CHECK(messages.records().back().actionId == a1);
}

TEST_CASE("MessageCenter - bounded log drops oldest records", "[message_log]") {
    psb::MessageCenter messages(2);
    auto a1 = messages.beginAction("board1", "A");
    messages.publish(a1, psb::MessageSeverity::Success, "board1", "OK A");
    messages.beginAction("board1", "B");

    auto log = messages.records();
    REQUIRE(log.size() == 2);
    CHECK(log[0].text == "OK A");
    CHECK(log[1].text == "B");
}
