#include <catch2/catch_test_macros.hpp>

#include "pupsnes/hw/token.h"

TEST_CASE("TokenTable creates tokens with sequential IDs", "[unit]") {
    pupsnes::TokenTable table;

    auto id1 = table.create(pupsnes::TokenType::BusRead, 0, 100, 0x2100, 0);
    auto id2 = table.create(pupsnes::TokenType::BusWrite, 1, 105, 0x2101, 0x42);

    REQUIRE(id1 == 0);
    REQUIRE(id2 == 1);

    auto *tok1 = table.get(id1);
    auto *tok2 = table.get(id2);

    REQUIRE(tok1 != nullptr);
    REQUIRE(tok1->type == pupsnes::TokenType::BusRead);
    REQUIRE(tok1->state == pupsnes::TokenState::Pending);
    REQUIRE(tok1->source_device == 0);
    REQUIRE(tok1->completion_time == 100);
    REQUIRE(tok1->address == 0x2100);

    REQUIRE(tok2 != nullptr);
    REQUIRE(tok2->type == pupsnes::TokenType::BusWrite);
    REQUIRE(tok2->data == 0x42);
}

TEST_CASE("TokenTable get returns nullptr for unknown ID", "[unit]") {
    pupsnes::TokenTable table;
    REQUIRE(table.get(999) == nullptr);
}

TEST_CASE("TokenTable complete sets state and data", "[unit]") {
    pupsnes::TokenTable table;
    auto id = table.create(pupsnes::TokenType::BusRead, 0, 100, 0x2100, 0);

    table.complete(id, 0xAB);

    auto *tok = table.get(id);
    REQUIRE(tok != nullptr);
    REQUIRE(tok->state == pupsnes::TokenState::Completed);
    REQUIRE(tok->data == 0xAB);
}

TEST_CASE("TokenTable resolveAt completes tokens due at given time", "[unit]") {
    pupsnes::TokenTable table;

    auto id1 = table.create(pupsnes::TokenType::BusRead, 0, 100, 0x2100, 0);
    auto id2 = table.create(pupsnes::TokenType::BusRead, 1, 200, 0x2101, 0);
    auto id3 = table.create(pupsnes::TokenType::BusWrite, 2, 100, 0x2102, 0xFF);

    auto woken = table.resolveAt(100);

    REQUIRE(table.get(id1)->state == pupsnes::TokenState::Completed);
    REQUIRE(table.get(id3)->state == pupsnes::TokenState::Completed);
    REQUIRE(table.get(id2)->state == pupsnes::TokenState::Pending);

    REQUIRE(table.get(id1)->data == 0);
    REQUIRE(table.get(id3)->data == 0xFF);
}

TEST_CASE("TokenTable remove deletes a token", "[unit]") {
    pupsnes::TokenTable table;
    auto id = table.create(pupsnes::TokenType::BusRead, 0, 100, 0x2100, 0);

    table.remove(id);

    REQUIRE(table.get(id) == nullptr);
}

TEST_CASE("TokenTable resolveAt returns device IDs to wake", "[unit]") {
    pupsnes::TokenTable table;

    auto id1 = table.create(pupsnes::TokenType::BusRead, 0, 100, 0x2100, 0);
    auto id2 = table.create(pupsnes::TokenType::BusRead, 1, 100, 0x2101, 0);

    table.setBlocked(id1, 0);
    table.setBlocked(id2, 1);

    auto woken = table.resolveAt(100);

    REQUIRE(woken.size() == 2);
    bool has_dev0 = (woken[0] == 0 || woken[1] == 0);
    bool has_dev1 = (woken[0] == 1 || woken[1] == 1);
    REQUIRE(has_dev0);
    REQUIRE(has_dev1);
}

TEST_CASE("TokenTable resolveAt does not wake non-blocked tokens", "[unit]") {
    pupsnes::TokenTable table;

    auto id1 = table.create(pupsnes::TokenType::BusRead, 0, 100, 0x2100, 0);

    auto woken = table.resolveAt(100);

    REQUIRE(woken.empty());
    REQUIRE(table.get(id1)->state == pupsnes::TokenState::Completed);
}
