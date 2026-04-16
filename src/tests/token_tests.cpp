#include <catch2/catch_test_macros.hpp>

#include "pupsnes/hw/token.h"

TEST_CASE("TokenTable creates tokens with sequential IDs", "[unit]") {
    pupsnes::TokenTable table;

    auto id1 = table.Create({pupsnes::TokenType::kBusRead, 0, 100, 0x2100, 0});
    auto id2 = table.Create({pupsnes::TokenType::kBusWrite, 1, 105, 0x2101, 0x42});

    REQUIRE(id1 == 1);
    REQUIRE(id2 == 2);

    auto* tok1 = table.Get(id1);
    auto* tok2 = table.Get(id2);

    REQUIRE(tok1 != nullptr);
    REQUIRE(tok1->type == pupsnes::TokenType::kBusRead);
    REQUIRE(tok1->state == pupsnes::TokenState::kPending);
    REQUIRE(tok1->source_device == 0);
    REQUIRE(tok1->completion_time == 100);
    REQUIRE(tok1->address == 0x2100);

    REQUIRE(tok2 != nullptr);
    REQUIRE(tok2->type == pupsnes::TokenType::kBusWrite);
    REQUIRE(tok2->data == 0x42);
}

TEST_CASE("TokenTable get returns nullptr for unknown ID", "[unit]") {
    pupsnes::TokenTable table;
    REQUIRE(table.Get(999) == nullptr);
}

TEST_CASE("TokenTable complete sets state and data", "[unit]") {
    pupsnes::TokenTable table;
    auto id = table.Create({pupsnes::TokenType::kBusRead, 0, 100, 0x2100, 0});

    table.Complete(id, 0xAB);

    auto* tok = table.Get(id);
    REQUIRE(tok != nullptr);
    REQUIRE(tok->state == pupsnes::TokenState::kCompleted);
    REQUIRE(tok->data == 0xAB);
}

TEST_CASE("TokenTable resolveAt completes tokens due at given time", "[unit]") {
    pupsnes::TokenTable table;

    auto id1 = table.Create({pupsnes::TokenType::kBusRead, 0, 100, 0x2100, 0});
    auto id2 = table.Create({pupsnes::TokenType::kBusRead, 1, 200, 0x2101, 0});
    auto id3 = table.Create({pupsnes::TokenType::kBusWrite, 2, 100, 0x2102, 0xFF});

    (void)table.ResolveAt(100);

    REQUIRE(table.Get(id1)->state == pupsnes::TokenState::kCompleted);
    REQUIRE(table.Get(id3)->state == pupsnes::TokenState::kCompleted);
    REQUIRE(table.Get(id2)->state == pupsnes::TokenState::kPending);

    REQUIRE(table.Get(id1)->data == 0);
    REQUIRE(table.Get(id3)->data == 0xFF);
}

TEST_CASE("TokenTable remove deletes a token", "[unit]") {
    pupsnes::TokenTable table;
    auto id = table.Create({pupsnes::TokenType::kBusRead, 0, 100, 0x2100, 0});

    table.Remove(id);

    REQUIRE(table.Get(id) == nullptr);
}

TEST_CASE("TokenTable resolveAt returns device IDs to wake", "[unit]") {
    pupsnes::TokenTable table;

    auto id1 = table.Create({pupsnes::TokenType::kBusRead, 0, 100, 0x2100, 0});
    auto id2 = table.Create({pupsnes::TokenType::kBusRead, 1, 100, 0x2101, 0});

    table.SetBlocked(id1, 0);
    table.SetBlocked(id2, 1);

    auto woken = table.ResolveAt(100);

    REQUIRE(woken.size() == 2);
    bool has_dev0 = (woken[0].device_id == 0 || woken[1].device_id == 0);
    bool has_dev1 = (woken[0].device_id == 1 || woken[1].device_id == 1);
    REQUIRE(has_dev0);
    REQUIRE(has_dev1);
}

TEST_CASE("TokenTable resolveAt does not wake non-blocked tokens", "[unit]") {
    pupsnes::TokenTable table;

    auto id1 = table.Create({pupsnes::TokenType::kBusRead, 0, 100, 0x2100, 0});

    auto woken = table.ResolveAt(100);

    REQUIRE(woken.empty());
    REQUIRE(table.Get(id1)->state == pupsnes::TokenState::kCompleted);
}
