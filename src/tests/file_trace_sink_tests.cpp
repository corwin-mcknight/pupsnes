#include <unistd.h>  // getpid

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "pupsnes/debugger/file_trace_sink.h"
#include "pupsnes/hw/rom/cartridge.h"
#include "pupsnes/core/snes.h"

using pupsnes::TraceEntry;
using pupsnes::debugger::FileTraceSink;
using pupsnes::debugger::Sha1Digest;

namespace {

struct FileSinkFixture {
  pupsnes::SNES snes;
  std::filesystem::path tmp_path;
  Sha1Digest rom_sha1{};

  FileSinkFixture() {
    // Deterministic tmp path. Test isolation comes from unique names.
    tmp_path = std::filesystem::temp_directory_path() /
               (std::string("pupsnes_trace_test_") +
                std::to_string(static_cast<int64_t>(::getpid())) + "_" +
                std::to_string(reinterpret_cast<uintptr_t>(this)) + ".log");
    // Minimal LoROM image so DisassembleInstructionRaw returns sane bytes.
    std::array<uint8_t, pupsnes::Cartridge::kLoROMWindowSize> rom{};
    rom.fill(0xEA);
    rom[0x7FFC] = 0x00;
    rom[0x7FFD] = 0x80;
    snes.LoadLoRom(rom);
    rom_sha1.fill(0);
    rom_sha1[0] = 0xDE;
    rom_sha1[1] = 0xAD;
  }

  ~FileSinkFixture() {
    std::error_code ec;
    std::filesystem::remove(tmp_path, ec);
  }

  std::string ReadFile() const {
    std::ifstream in(tmp_path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
  }
};

std::vector<std::string> SplitLines(const std::string& content) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : content) {
    if (c == '\n') {
      out.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

}  // namespace

TEST_CASE("FileTraceSink emits body line with exact expected format", "[unit][debugger]") {
  FileSinkFixture fx;
  {
    FileTraceSink sink(fx.snes, fx.tmp_path.string(), fx.rom_sha1);
    TraceEntry entry{};
    entry.master_time = 0;
    entry.pc = 0x008000;
    entry.regs.A = 0x0000;
    entry.regs.X = 0x0000;
    entry.regs.Y = 0x0000;
    entry.regs.SP = 0x01FF;
    entry.regs.DP = 0x0000;
    entry.regs.DBR = 0x00;
    entry.regs.PBR = 0x00;
    entry.regs.PC = 0x8000;
    entry.regs.P.N = false;
    entry.regs.P.V = false;
    entry.regs.P.M = false;
    entry.regs.P.X = false;
    entry.regs.P.D = false;
    entry.regs.P.I = true;
    entry.regs.P.Z = false;
    entry.regs.P.C = false;
    entry.regs.P.E = true;
    sink.Record(entry);
  }

  const std::vector<std::string> lines = SplitLines(fx.ReadFile());
  REQUIRE(lines.size() >= 2);  // header + body
  const std::string& body = lines.back();

  // Build the expected string explicitly rather than embedding ambiguous
  // space-runs in a literal. Column layout (per spec):
  //   PBR:PC  [2 sep]  opcode-col(11)  [2 sep]  mnemonic-col(22)  [2 sep]
  //   reg-block(40)  [1 sep]  P:nvmxdizc  [1 sep]  E:x  [2 sep]
  //   MT:<12-hex>  [2 sep]  V:### H:####  [2 sep]  #<seq>
  const std::string expected =
      std::string("00:8000") + "  " +
      "EA" + std::string(9, ' ') + "  " +
      "NOP" + std::string(19, ' ') + "  " +
      "A:0000 X:0000 Y:0000 S:01FF D:0000 DB:00" + " " +
      "P:nvmxdIzc" + " " +
      "E:1" + "  " +
      "MT:000000000000" + "  " +
      "V:000 H:0000" + "  " +
      "#1";
  CHECK(body == expected);
}

TEST_CASE("FileTraceSink header has expected version and fields", "[unit][debugger]") {
  FileSinkFixture fx;
  Sha1Digest sha{};
  for (std::size_t i = 0; i < sha.size(); ++i) sha[i] = static_cast<uint8_t>(i);
  {
    FileTraceSink sink(fx.snes, fx.tmp_path.string(), sha);
  }
  const std::vector<std::string> lines = SplitLines(fx.ReadFile());
  REQUIRE(!lines.empty());
  CHECK(lines[0] ==
        "# pupsnes-trace v1  "
        "rom-sha1=000102030405060708090a0b0c0d0e0f10111213  "
        "master-hz=21477272  "
        "lines-per-frame=262");
}

TEST_CASE("FileTraceSink header is followed by body lines only", "[unit][debugger]") {
  FileSinkFixture fx;
  {
    FileTraceSink sink(fx.snes, fx.tmp_path.string(), fx.rom_sha1);
    TraceEntry entry{};
    entry.pc = 0x008000;
    entry.regs.SP = 0x01FF;
    entry.regs.P.I = true;
    entry.regs.P.E = true;
    sink.Record(entry);
    sink.Record(entry);
  }
  const std::vector<std::string> lines = SplitLines(fx.ReadFile());
  REQUIRE(lines.size() == 3);
  CHECK(lines[0].starts_with("# pupsnes-trace v1"));
  CHECK(!lines[1].starts_with("#"));
  CHECK(!lines[2].starts_with("#"));
}

TEST_CASE("FileTraceSink V/H derive from master_time", "[unit][debugger]") {
  FileSinkFixture fx;
  const uint64_t mcyc_per_line = FileTraceSink::kNominalMcycPerLine;

  TraceEntry entry{};
  entry.pc = 0x008000;
  entry.regs.SP = 0x01FF;
  entry.regs.P.I = true;
  entry.regs.P.E = true;

  {
    FileTraceSink sink(fx.snes, fx.tmp_path.string(), fx.rom_sha1);
    entry.master_time = 0;                           // V=000 H=0000
    sink.Record(entry);
    entry.master_time = mcyc_per_line - 1;           // V=000 H=1363
    sink.Record(entry);
    entry.master_time = mcyc_per_line;               // V=001 H=0000
    sink.Record(entry);
    entry.master_time = mcyc_per_line * 262ULL;      // V=000 H=0000 (wrapped frame)
    sink.Record(entry);
  }

  const std::vector<std::string> lines = SplitLines(fx.ReadFile());
  REQUIRE(lines.size() == 5);  // header + 4 body
  CHECK(lines[1].find("V:000 H:0000") != std::string::npos);
  CHECK(lines[2].find("V:000 H:1363") != std::string::npos);
  CHECK(lines[3].find("V:001 H:0000") != std::string::npos);
  CHECK(lines[4].find("V:000 H:0000") != std::string::npos);
}

TEST_CASE("FileTraceSink retired_seq is monotonic from 1", "[unit][debugger]") {
  FileSinkFixture fx;
  TraceEntry entry{};
  entry.pc = 0x008000;
  entry.regs.SP = 0x01FF;
  entry.regs.P.I = true;
  entry.regs.P.E = true;
  {
    FileTraceSink sink(fx.snes, fx.tmp_path.string(), fx.rom_sha1);
    sink.Record(entry);
    sink.Record(entry);
    sink.Record(entry);
    CHECK(sink.LineCount() == 3);
  }
  const std::vector<std::string> lines = SplitLines(fx.ReadFile());
  REQUIRE(lines.size() == 4);
  CHECK(lines[1].ends_with("#1"));
  CHECK(lines[2].ends_with("#2"));
  CHECK(lines[3].ends_with("#3"));
}

TEST_CASE("FileTraceSink P-flag letters upper when set, lower when clear", "[unit][debugger]") {
  FileSinkFixture fx;
  TraceEntry entry{};
  entry.pc = 0x008000;
  entry.regs.SP = 0x01FF;
  entry.regs.P.N = true;
  entry.regs.P.V = true;
  entry.regs.P.M = true;
  entry.regs.P.X = true;
  entry.regs.P.D = true;
  entry.regs.P.I = true;
  entry.regs.P.Z = true;
  entry.regs.P.C = true;
  entry.regs.P.E = false;
  {
    FileTraceSink sink(fx.snes, fx.tmp_path.string(), fx.rom_sha1);
    sink.Record(entry);
  }
  const std::vector<std::string> lines = SplitLines(fx.ReadFile());
  REQUIRE(lines.size() == 2);
  CHECK(lines[1].find("P:NVMXDIZC E:0") != std::string::npos);
}

TEST_CASE("FileTraceSink latches open failure", "[unit][debugger]") {
  pupsnes::SNES snes;
  std::array<uint8_t, pupsnes::Cartridge::kLoROMWindowSize> rom{};
  rom.fill(0xEA);
  rom[0x7FFC] = 0x00;
  rom[0x7FFD] = 0x80;
  snes.LoadLoRom(rom);

  // Deliberately impossible path — a non-existent directory.
  Sha1Digest sha{};
  FileTraceSink sink(snes,
                     "/nonexistent/directory/does/not/exist/trace.log",
                     sha);
  CHECK(sink.HasError());
  CHECK(!sink.Error().empty());
  CHECK(sink.Error().find("open") != std::string::npos);

  // Record on a failed sink must be a no-op — no crash, no increment.
  TraceEntry entry{};
  entry.pc = 0x008000;
  entry.regs.SP = 0x01FF;
  entry.regs.P.I = true;
  entry.regs.P.E = true;
  sink.Record(entry);
  CHECK(sink.LineCount() == 0);
}
