#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>

#include "pupsnes/debugger/bus_event_log.h"
#include "pupsnes/core/bus_event.h"
#include "pupsnes/hw/rom/cartridge.h"
#include "pupsnes/core/snes.h"
#include "pupsnes/memory/systembus.h"

using namespace pupsnes;            // NOLINT(google-build-using-namespace)
using namespace pupsnes::debugger;  // NOLINT(google-build-using-namespace)

TEST_CASE("BusEventLog stores and snapshots pushes", "[unit][bus_event]") {
  BusEventLog log(4);
  log.Push({10, 0x0001, 0x01, BusEventKind::kFastRead});
  log.Push({11, 0x0002, 0x02, BusEventKind::kFastWrite});

  const auto snap = log.Snapshot();
  REQUIRE(snap.size() == 2);
  REQUIRE(snap[0].address == 0x0001);
  REQUIRE(snap[0].kind == BusEventKind::kFastRead);
  REQUIRE(snap[1].data == 0x02);
}

TEST_CASE("BusEventLog wraps at capacity and preserves oldest-to-newest order", "[unit][bus_event]") {
  BusEventLog log(3);
  for (uint8_t i = 1; i <= 5; ++i) {
    log.Push({i, static_cast<SnesAddrT>(i), i, BusEventKind::kInlineRead});
  }
  const auto snap = log.Snapshot();
  REQUIRE(snap.size() == 3);
  REQUIRE(snap[0].data == 3);
  REQUIRE(snap[1].data == 4);
  REQUIRE(snap[2].data == 5);
}

TEST_CASE("BusEventLog Clear drops all entries", "[unit][bus_event]") {
  BusEventLog log(4);
  log.Push({1, 0, 0, BusEventKind::kFastRead});
  log.Clear();
  REQUIRE(log.Size() == 0);
}

TEST_CASE("SystemBus fast-path reads and writes emit bus events when a sink is attached", "[unit][bus_event]") {
  SNES snes;
  BusEventLog log(32);
  snes.GetSystemBus().SetEventSink(&log);

  std::array<uint8_t, Cartridge::kLoROMWindowSize> rom{};
  rom.fill(0xEA);
  rom[0x7FFCU] = 0x00;  // Reset vector low
  rom[0x7FFDU] = 0x80;  // Reset vector high → $008000
  snes.LoadRom(rom);
  snes.Reset();

  // snes.Reset() fetches the reset vector via the bus; at minimum those two
  // reads should have been captured.
  const auto snap = log.Snapshot();
  REQUIRE(snap.size() >= 2);
  bool saw_vector_low = false;
  bool saw_vector_high = false;
  const auto is_read = [](BusEventKind k) {
    return k == BusEventKind::kFastRead || k == BusEventKind::kInlineRead || k == BusEventKind::kScheduledRead;
  };
  for (const BusEvent& e : snap) {
    if (e.address == 0x00FFFCU && is_read(e.kind)) saw_vector_low = true;
    if (e.address == 0x00FFFDU && is_read(e.kind)) saw_vector_high = true;
  }
  REQUIRE(saw_vector_low);
  REQUIRE(saw_vector_high);
}

TEST_CASE("SystemBus emits no events when no sink is attached", "[unit][bus_event]") {
  SNES snes;
  BusEventLog log(8);
  // Intentionally do not call SetEventSink.

  std::array<uint8_t, Cartridge::kLoROMWindowSize> rom{};
  rom.fill(0xEA);
  rom[0x7FFCU] = 0x00;
  rom[0x7FFDU] = 0x80;
  snes.LoadRom(rom);
  snes.Reset();

  REQUIRE(log.Size() == 0);
}
