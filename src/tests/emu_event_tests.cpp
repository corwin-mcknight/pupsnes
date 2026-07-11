#include <unistd.h>  // getpid

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "pupsnes/core/emu_event.h"
#include "pupsnes/core/snes.h"
#include "pupsnes/debugger/emu_event_format.h"
#include "pupsnes/debugger/emu_event_log.h"
#include "pupsnes/debugger/error_log.h"
#include "pupsnes/debugger/fan_out_emu_event_sink.h"
#include "pupsnes/debugger/file_event_sink.h"
#include "pupsnes/hw/5a22/dma_controller.h"
#include "pupsnes/hw/rom/cartridge.h"
#include "pupsnes/hw/sppu/ppu.h"
#include "pupsnes/hw/sppu/ppu_regs.h"
#include "pupsnes/memory/systembus.h"

using namespace pupsnes;            // NOLINT(google-build-using-namespace)
using namespace pupsnes::debugger;  // NOLINT(google-build-using-namespace)

// These tests build under the CI/DEBUG profiles, where events::kEnabled is
// true. PRODUCTION compiles emission out; that configuration is covered by
// the release build itself, not by this binary.
static_assert(events::kEnabled, "event tests require a non-PRODUCTION profile");

namespace {

void BusWrite(SNES& snes, SnesAddrT address, uint8_t data, TimeMasterT now) {
  BusPlan plan = snes.system_bus->Plan(address, BusAccessType::kWrite, data);
  REQUIRE(plan.outcome == BusPlanOutcome::kInlineComplete);
  (void)snes.system_bus->Follow(plan, now, 0);
}

[[nodiscard]] std::vector<EmuEvent> EventsOfKind(const EmuEventLog& log, EmuEventKind kind) {
  std::vector<EmuEvent> out;
  for (const EmuEvent& event : log.Snapshot()) {
    if (event.kind == kind) out.push_back(event);
  }
  return out;
}

}  // namespace

TEST_CASE("Every EmuEventKind row resolves with a valid category band", "[unit][events]") {
  std::set<std::string> names;
  for (const EmuEventInfo& info : AllEmuEventInfos()) {
    const EmuEventInfo& looked_up = GetEmuEventInfo(info.kind);
    REQUIRE(looked_up.name == info.name);
    REQUIRE(std::string(info.name) != "");
    REQUIRE(std::string(info.format) != "");
    REQUIRE(names.insert(info.name).second);  // names are unique

    const EmuEventCategory category = EmuEventCategoryOf(info.kind);
    const bool valid_category = category == EmuEventCategory::kNone || category == EmuEventCategory::kPpu ||
                                category == EmuEventCategory::kDma || category == EmuEventCategory::kInterrupt ||
                                category == EmuEventCategory::kError || category == EmuEventCategory::kHost;
    REQUIRE(valid_category);
    REQUIRE(std::string(EmuEventCategoryName(category)) != "?");
  }
}

TEST_CASE("FormatEmuEventMessage renders args through the kind's template", "[unit][events]") {
  EmuEvent event;
  event.kind = EmuEventKind::kPpuBgModeChange;
  event.args = {1, 7, 1, 0};
  REQUIRE(FormatEmuEventMessage(event) == "BG mode 1 -> 7 (BG3 priority 1)");

  EmuEvent unknown;
  // Deliberately out-of-range kind to exercise the sentinel fallback.
  unknown.kind = static_cast<EmuEventKind>(0x7FFFU);  // NOLINT(clang-analyzer-optin.core.EnumCastOutOfRange)
  REQUIRE(FormatEmuEventMessage(unknown) == "unknown event kind 0x7fff");
}

TEST_CASE("FormatEmuEventMessage prefers the carried message text", "[unit][events]") {
  EmuEvent event;
  event.kind = EmuEventKind::kErrorBus;
  event.message = "open bus at $00:5000";
  REQUIRE(FormatEmuEventMessage(event) == "open bus at $00:5000");
}

TEST_CASE("ProjectHvAt agrees with the PPU's real dot walk from a lagging cursor", "[unit][events]") {
  // The projection is pure counter arithmetic over the same DotCost/field
  // model the catch-up loop executes. For each sample time, project from the
  // current (lagging) cursor first, then actually catch the PPU up and
  // require the live counters to land on the same dot. Samples cover line
  // starts, mid-line dots, the 6-cycle dots at H=323/327, the short line
  // V=240 on the field=true frame, and both frame boundaries.
  SNES snes;
  Ppu& ppu = snes.GetPpu();
  ppu.Reset();

  constexpr TimeMasterT kFrame0 = static_cast<TimeMasterT>(262) * 1364;                    // field=false
  constexpr TimeMasterT kFrame1Start240 = kFrame0 + static_cast<TimeMasterT>(240) * 1364;  // short line start
  const std::array<TimeMasterT, 14> samples = {
      0,
      1,
      100,
      4 * 323,                       // first 6-cycle dot of line 0
      4 * 323 + 5,                   // inside that 6-cycle dot
      1364 + 7,                      // early line 1
      kFrame0 - 1,                   // last cycle of frame 0
      kFrame0,                       // frame boundary (field flips)
      kFrame1Start240,               // short-line start, field=true
      kFrame1Start240 + 1359,        // last cycle of the short line
      kFrame1Start240 + 1360,        // first cycle of V=241
      kFrame0 + kFrame0 - 4 - 1,     // last cycle of the short frame
      kFrame0 + kFrame0 - 4,         // next frame boundary (field flips back)
      2 * kFrame0 - 4 + 225 * 1364,  // NMI dot of frame 2
  };

  for (const TimeMasterT t : samples) {
    const EmuEventHv projected = ppu.ProjectHvAt(t);
    ppu.CatchUpTo(t);
    const Ppu::Cursor cursor = ppu.GetCursor();
    INFO("t=" << t);
    REQUIRE(projected.v == cursor.v);
    REQUIRE(projected.h == cursor.h);
  }
}

TEST_CASE("ErrorLog pushes mirror into the event stream with severity and address", "[unit][events]") {
  ErrorLog errors(16);
  EmuEventLog events(16);
  errors.SetEventSink(&events);

  errors.PushBusAccessFailure(1234, 0x7E1000, "write rejected", std::nullopt, ErrorSeverity::kWarning);
  errors.PushHostError(5678, "config missing", ErrorSeverity::kFatal);

  REQUIRE(errors.Snapshot().size() == 2);  // ErrorLog itself unchanged
  const auto snap = events.Snapshot();
  REQUIRE(snap.size() == 2);

  REQUIRE(snap[0].kind == EmuEventKind::kErrorBus);
  REQUIRE(snap[0].master_time == 1234);
  REQUIRE(snap[0].args[0] == static_cast<uint32_t>(ErrorSeverity::kWarning));
  REQUIRE(snap[0].args[1] == 0x7E1000);
  REQUIRE(snap[0].args[2] == 1);
  REQUIRE(snap[0].message == "write rejected");

  REQUIRE(snap[1].kind == EmuEventKind::kErrorHost);
  REQUIRE(snap[1].args[2] == 0);  // no address

  // The rendered line carries the severity label and the message text;
  // bridged errors have no counter position, so V/H render as dashes.
  REQUIRE(snap[0].hv.v == kEmuEventHvUnknown);
  const std::string line = FormatEmuEventLine(snap[0]);
  REQUIRE(line.find("V:--- H:---") != std::string::npos);
  REQUIRE(line.find("ERROR_BUS") != std::string::npos);
  REQUIRE(line.find("[Warn] write rejected") != std::string::npos);
}

TEST_CASE("events::Emit packs args in order and tolerates a null sink", "[unit][events]") {
  events::Emit(nullptr, 1, EmuEventKind::kNmiAsserted, 225);  // must not crash

  EmuEventLog log(8);
  events::Emit(&log, 42, EmuEventKind::kPpuBgModeChange, uint8_t{1}, uint8_t{7}, true);
  REQUIRE(log.Size() == 1);
  const EmuEvent event = log.Snapshot().front();
  REQUIRE(event.master_time == 42);
  REQUIRE(event.kind == EmuEventKind::kPpuBgModeChange);
  REQUIRE(event.args[0] == 1);
  REQUIRE(event.args[1] == 7);
  REQUIRE(event.args[2] == 1);
  REQUIRE(event.args[3] == 0);
}

TEST_CASE("EmuEventLog wraps at capacity preserving oldest-to-newest order", "[unit][events]") {
  EmuEventLog log(3);
  for (uint32_t i = 1; i <= 5; ++i) {
    events::Emit(&log, i, EmuEventKind::kNmiAsserted, i);
  }
  const auto snap = log.Snapshot();
  REQUIRE(snap.size() == 3);
  REQUIRE(snap[0].args[0] == 3);
  REQUIRE(snap[2].args[0] == 5);
}

TEST_CASE("EmuEventLog category mask drops filtered categories at capture", "[unit][events]") {
  EmuEventLog log(8);
  log.SetCategoryMask(EmuEventCategoryBit(EmuEventCategory::kDma));
  events::Emit(&log, 1, EmuEventKind::kNmiAsserted, 225);
  events::Emit(&log, 2, EmuEventKind::kDmaBurstStart, 0x01);
  REQUIRE(log.Size() == 1);
  REQUIRE(log.Snapshot().front().kind == EmuEventKind::kDmaBurstStart);
}

TEST_CASE("FanOutEmuEventSink attach is idempotent and forwards to all sinks", "[unit][events]") {
  EmuEventLog a(4);
  EmuEventLog b(4);
  FanOutEmuEventSink fan_out;
  fan_out.Attach(&a);
  fan_out.Attach(&a);
  fan_out.Attach(&b);
  REQUIRE(fan_out.Count() == 2);

  events::Emit(&fan_out, 1, EmuEventKind::kHdmaFrameInit, 0xFF);
  REQUIRE(a.Size() == 1);
  REQUIRE(b.Size() == 1);

  fan_out.Detach(&a);
  fan_out.Detach(&a);  // unknown detach tolerated
  REQUIRE(fan_out.Count() == 1);
}

TEST_CASE("FileEventSink writes a versioned header and one line per event", "[unit][events]") {
  const std::filesystem::path tmp_path =
      std::filesystem::temp_directory_path() /
      (std::string("pupsnes_events_test_") + std::to_string(static_cast<int64_t>(::getpid())) + ".log");

  Sha1Digest sha1{};
  sha1[0] = 0xDE;
  sha1[1] = 0xAD;
  {
    FileEventSink sink(tmp_path.string(), sha1);
    REQUIRE_FALSE(sink.HasError());

    EmuEvent event;
    event.master_time = 0x123456;
    event.kind = EmuEventKind::kPpuBgModeChange;
    event.hv = {88, 123};
    event.args = {0, 1, 0, 0};
    sink.OnEmuEvent(event);
    REQUIRE(sink.LineCount() == 1);
  }

  std::ifstream in(tmp_path, std::ios::binary);
  std::string header;
  std::string line;
  REQUIRE(std::getline(in, header));
  REQUIRE(std::getline(in, line));
  REQUIRE(header.starts_with("# pupsnes-events v1  rom-sha1=dead"));
  REQUIRE(line.starts_with("MT:000000123456"));
  REQUIRE(line.find("V:088 H:123") != std::string::npos);
  REQUIRE(line.find("PPU") != std::string::npos);
  REQUIRE(line.find("BG_MODE_CHANGE") != std::string::npos);
  REQUIRE(line.find("BG mode 0 -> 1") != std::string::npos);

  std::error_code ec;
  std::filesystem::remove(tmp_path, ec);
}

TEST_CASE("BGMODE replay emits one BG-mode-change event at the write's cycle", "[unit][events]") {
  SNES snes;
  EmuEventLog log(64);
  snes.SetEmuEventSink(&log);
  Ppu& ppu = snes.GetPpu();
  ppu.Reset();

  BusWrite(snes, sppu::regs::kBgmode, 0x07, /*now=*/100);
  // Lazy replay: nothing observable until the PPU catches up past the write.
  REQUIRE(EventsOfKind(log, EmuEventKind::kPpuBgModeChange).empty());

  ppu.CatchUpTo(200);
  auto events = EventsOfKind(log, EmuEventKind::kPpuBgModeChange);
  REQUIRE(events.size() == 1);
  REQUIRE(events[0].master_time == 100);
  REQUIRE(events[0].args[0] == 0);  // reset mode
  REQUIRE(events[0].args[1] == 7);
  // Stamped from the live cursor: the write applies at the dot starting at
  // cycle 100 — dot 25 of line 0 (4-cycle dots below H=323).
  REQUIRE(events[0].hv.v == 0);
  REQUIRE(events[0].hv.h == 25);

  // A redundant write decodes to the same state: no second event.
  BusWrite(snes, sppu::regs::kBgmode, 0x07, /*now=*/300);
  ppu.CatchUpTo(400);
  REQUIRE(EventsOfKind(log, EmuEventKind::kPpuBgModeChange).size() == 1);
}

TEST_CASE("INIDISP replay emits forced-blank and brightness changes separately", "[unit][events]") {
  SNES snes;
  EmuEventLog log(64);
  snes.SetEmuEventSink(&log);
  Ppu& ppu = snes.GetPpu();
  ppu.Reset();

  // Reset state is forced blank + brightness 0; this flips both.
  BusWrite(snes, sppu::regs::kInidisp, 0x0F, /*now=*/50);
  ppu.CatchUpTo(100);

  auto blank_events = EventsOfKind(log, EmuEventKind::kPpuForcedBlankChange);
  REQUIRE(blank_events.size() == 1);
  REQUIRE(blank_events[0].args[0] == 1);
  REQUIRE(blank_events[0].args[1] == 0);

  auto brightness_events = EventsOfKind(log, EmuEventKind::kPpuBrightnessChange);
  REQUIRE(brightness_events.size() == 1);
  REQUIRE(brightness_events[0].args[1] == 0x0F);
}

TEST_CASE("PPU emits an NMI-assert event when V reaches the VBlank entry line", "[unit][events]") {
  SNES snes;
  EmuEventLog log(64);
  snes.SetEmuEventSink(&log);
  Ppu& ppu = snes.GetPpu();
  ppu.Reset();

  // V=225 starts at 225 lines x 1364 mcyc; run a little past it.
  ppu.CatchUpTo(static_cast<TimeMasterT>(225) * 1364 + 8);
  auto events = EventsOfKind(log, EmuEventKind::kNmiAsserted);
  REQUIRE(events.size() == 1);
  REQUIRE(events[0].args[0] == 225);
  // Counter position at the V transition: first dot of line 225.
  REQUIRE(events[0].hv.v == 225);
  REQUIRE(events[0].hv.h == 0);
}

TEST_CASE("LoadRom emits a host ROM_LOADED event with the image size", "[unit][events]") {
  SNES snes;
  EmuEventLog log(8);
  snes.SetEmuEventSink(&log);

  std::array<uint8_t, Cartridge::kLoROMWindowSize> rom{};
  rom.fill(0xEA);
  rom[0x7FFC] = 0x00;
  rom[0x7FFD] = 0x80;
  REQUIRE(snes.LoadRom(rom).ok);

  auto events = EventsOfKind(log, EmuEventKind::kRomLoaded);
  REQUIRE(events.size() == 1);
  REQUIRE(events[0].args[0] == rom.size());
}

TEST_CASE("GP-DMA trigger emits burst start and complete events", "[unit][events]") {
  SNES snes;
  EmuEventLog log(64);
  snes.SetEmuEventSink(&log);

  // Channel 0: 4 bytes from WRAM $7E0000 to $2122 (CGDATA), mode 0.
  BusWrite(snes, 0x004300, 0x00, /*now=*/10);  // DMAP0: A->B, increment
  BusWrite(snes, 0x004301, 0x22, /*now=*/11);  // BBAD0
  BusWrite(snes, 0x004302, 0x00, /*now=*/12);  // A1T0L
  BusWrite(snes, 0x004303, 0x00, /*now=*/13);  // A1T0H
  BusWrite(snes, 0x004304, 0x7E, /*now=*/14);  // A1B0
  BusWrite(snes, 0x004305, 0x04, /*now=*/15);  // DAS0L = 4 bytes
  BusWrite(snes, 0x004306, 0x00, /*now=*/16);  // DAS0H
  BusWrite(snes, 0x00420B, 0x01, /*now=*/20);  // MDMAEN: fire channel 0

  auto starts = EventsOfKind(log, EmuEventKind::kDmaBurstStart);
  REQUIRE(starts.size() == 1);
  REQUIRE(starts[0].args[0] == 0x01);
  REQUIRE(starts[0].master_time == 20);

  auto completes = EventsOfKind(log, EmuEventKind::kDmaBurstComplete);
  REQUIRE(completes.size() == 1);
  REQUIRE(completes[0].args[0] == 0x01);
  // 8 mcyc startup + 4 bytes x 8 mcyc.
  REQUIRE(completes[0].args[1] == 8 + 4 * 8);
}
