// HiROM mapper test suite. Layered the same way as fastrom_tests.cpp:
//   1. Header helpers / SRAM sizing.
//   2. Page-table layout — half banks, full banks, FASTROM mirrors.
//   3. Scope preservation — WRAM, LowRAM mirror, CPU MMIO untouched.
//   4. SRAM mapping at $20:6000-$3F:7FFF (and the FASTROM mirror).
//   5. MEMSEL / FASTROM retiming over the HiROM fast-bank surface.
//   6. ROM byte reads via Debug API to validate the offset formula.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <vector>

#include "pupsnes/core/snes.h"
#include "pupsnes/hw/5a22/cpu_mmio.h"
#include "pupsnes/hw/rom/cartridge.h"
#include "pupsnes/hw/rom/rom_format.h"
#include "pupsnes/memory/systembus.h"
#include "systembus_test_access.h"

using namespace pupsnes;  // NOLINT(google-build-using-namespace)

namespace {

// 1 MiB minimum to populate the full half-bank ($00-$0F) plus a slice of the
// full-bank space ($40-$4F). Anything smaller drops the fast-path pointer
// because the 256-byte windows wouldn't fit the buffer; the bus would still
// be correct via the modulo slow-path, but the page-table layout tests want
// to see fast pointers wired up.
constexpr std::size_t kSmallHiRomSize = 1U * 1024U * 1024U;
// 4 MiB image — covers the full $00-$3F half-bank space and the $40-$7D
// full-bank slow space, which is the widest unique-ROM-byte surface HiROM
// can expose without bank mirroring across $40-$7D.
constexpr std::size_t kFullHiRomSize = 4U * 1024U * 1024U;
constexpr uint8_t kNopOpcode = 0xEAU;

std::vector<uint8_t> MakeHiRom(std::size_t size, uint8_t sram_byte = 0x00U) {
  std::vector<uint8_t> rom(size, kNopOpcode);
  // Plant a recognisable byte at every page boundary inside the bank-encoded
  // ROM offset to support layout assertions: rom[offset] = (offset >> 8) ^
  // 0x5A. Bank-encoded reads can then verify which ROM byte the page-table
  // entry resolved to.
  for (std::size_t i = 0; i < rom.size(); ++i) {
    rom[i] = static_cast<uint8_t>(((i >> 8) & 0xFFU) ^ 0x5AU);
  }
  // Header bytes — DetectCartProfile uses $FFD5 (map mode) and $FFD6
  // (chipset). Without these the auto-detector falls back to LoROM and
  // misreads the 0xA5 test-pattern byte at $FFD6 as a coprocessor chipset.
  rom[kHiRomMapModeOffset] = 0x21U;  // HiROM, slow
  rom[0xFFD6U] = 0x00U;              // plain ROM, no coprocessor
  // Reset vector — irrelevant for these tests but keeps the header well-formed.
  rom[kHiRomHeaderOffset + (0xFFFCU - 0xFFB0U)] = 0x00U;
  rom[kHiRomHeaderOffset + (0xFFFDU - 0xFFB0U)] = 0x80U;
  if (size > kHiRomSramSizeOffset) {
    rom[kHiRomSramSizeOffset] = sram_byte;
  }
  return rom;
}

uint8_t GetAccessSpeed(const SNES& snes, uint8_t bank, uint8_t page) {
  return SystemBusTestAccess::GetPageEntry(*snes.system_bus, bank, page).access_speed;
}

const PageTableEntry& GetEntry(const SNES& snes, uint8_t bank, uint8_t page) {
  return SystemBusTestAccess::GetPageEntry(*snes.system_bus, bank, page);
}

void WriteMemSel(SNES& snes, uint8_t value) {
  auto result = snes.system_bus->DebugWrite(0x00'420DU, value);
  REQUIRE(result.ok);
}

}  // namespace

// ---------------------------------------------------------------------------
// Layer 1: HiRomSramSize header helper
// ---------------------------------------------------------------------------

TEST_CASE("HiRomSramSize: returns 0 for header byte 0 (no SRAM)", "[unit][rom_format]") {
  auto rom = MakeHiRom(kSmallHiRomSize, 0x00U);
  REQUIRE(HiRomSramSize(rom) == 0U);
}

TEST_CASE("HiRomSramSize: 1024 << N for supported byte values", "[unit][rom_format]") {
  REQUIRE(HiRomSramSize(MakeHiRom(kSmallHiRomSize, 0x01U)) == 0x800U);    // 2 KiB
  REQUIRE(HiRomSramSize(MakeHiRom(kSmallHiRomSize, 0x03U)) == 0x2000U);   // 8 KiB
  REQUIRE(HiRomSramSize(MakeHiRom(kSmallHiRomSize, 0x05U)) == 0x8000U);   // 32 KiB
  REQUIRE(HiRomSramSize(MakeHiRom(kSmallHiRomSize, 0x09U)) == 0x80000U);  // 512 KiB
}

TEST_CASE("HiRomSramSize: rejects out-of-range values", "[unit][rom_format]") {
  REQUIRE(HiRomSramSize(MakeHiRom(kSmallHiRomSize, 0x0AU)) == 0U);
  REQUIRE(HiRomSramSize(MakeHiRom(kSmallHiRomSize, 0xFFU)) == 0U);
}

TEST_CASE("HiRomSramSize: returns 0 when ROM is shorter than the header", "[unit][rom_format]") {
  std::vector<uint8_t> short_rom(0x100U, 0x00U);
  REQUIRE(HiRomSramSize(short_rom) == 0U);
}

// ---------------------------------------------------------------------------
// Layer 2: page-table layout
// ---------------------------------------------------------------------------

TEST_CASE("HiROM: half-bank ROM at $00:8000-$00:FFFF maps to ROM bytes $8000-$FFFF", "[unit][hirom]") {
  SNES snes;
  auto rom = MakeHiRom(kFullHiRomSize);
  snes.LoadRom(rom);

  // Bank $00, page $80 → ROM byte $008000.
  const auto& entry_lo = GetEntry(snes, 0x00U, 0x80U);
  REQUIRE(entry_lo.kind == PageDeviceKind::kMemory);
  REQUIRE(entry_lo.base_offset == 0x008000U);
  // Bank $3F, page $FF → ROM byte $3FFF00 (top of the half-bank surface).
  const auto& entry_hi = GetEntry(snes, 0x3FU, 0xFFU);
  REQUIRE(entry_hi.kind == PageDeviceKind::kMemory);
  REQUIRE(entry_hi.base_offset == 0x3FFF00U);
}

TEST_CASE("HiROM: full-bank ROM at $40:0000-$7D:FFFF strides every 64 KiB", "[unit][hirom]") {
  SNES snes;
  auto rom = MakeHiRom(kFullHiRomSize);
  snes.LoadRom(rom);

  // Bank $40, page $00 → ROM byte $000000 (aliases the LowRAM mirror in
  // banks $00-$3F at the ROM byte level, even though the CPU sees WRAM there).
  REQUIRE(GetEntry(snes, 0x40U, 0x00U).base_offset == 0x000000U);
  // Bank $40, page $80 → ROM byte $008000 (aliases bank $00 page $80).
  REQUIRE(GetEntry(snes, 0x40U, 0x80U).base_offset == 0x008000U);
  // Bank $7D, page $FF → ROM byte $3DFF00 (top of the slow full-bank surface).
  REQUIRE(GetEntry(snes, 0x7DU, 0xFFU).base_offset == 0x3DFF00U);
}

TEST_CASE("HiROM: FASTROM mirrors map to the same ROM bytes as the slow banks", "[unit][hirom]") {
  SNES snes;
  auto rom = MakeHiRom(kFullHiRomSize);
  snes.LoadRom(rom);

  // $80:80 mirrors $00:80, $C0:00 mirrors $40:00, $FF:FF mirrors $7F:FF.
  // Bank $7F is WRAM (so we compare against the underlying ROM byte formula
  // directly): (0x7F & 0x3F) << 16 | $FF00 = $3FFF00.
  REQUIRE(GetEntry(snes, 0x80U, 0x80U).base_offset == 0x008000U);
  REQUIRE(GetEntry(snes, 0xC0U, 0x00U).base_offset == 0x000000U);
  REQUIRE(GetEntry(snes, 0xFFU, 0xFFU).base_offset == 0x3FFF00U);
}

TEST_CASE("HiROM: kMemory page entries carry a fast pointer when the ROM is big enough", "[unit][hirom]") {
  SNES snes;
  auto rom = MakeHiRom(kFullHiRomSize);
  snes.LoadRom(rom);

  const auto& entry = GetEntry(snes, 0x40U, 0x00U);
  REQUIRE(entry.fast_read_ptr != nullptr);
  // Writes go through the slow path on HiROM ROM (read-only), so the write
  // pointer must be null even when reads are fast.
  REQUIRE(entry.fast_write_ptr == nullptr);
}

// ---------------------------------------------------------------------------
// Layer 3: HiROM mapping does not clobber other devices
// ---------------------------------------------------------------------------

TEST_CASE("HiROM: LowRAM mirror at banks $00-$3F pages $00-$1F stays WRAM-backed", "[unit][hirom]") {
  SNES snes;
  auto rom = MakeHiRom(kFullHiRomSize);
  snes.LoadRom(rom);

  // Drop a sentinel into WRAM via $7E:0000, observe it through the LowRAM
  // mirror at $00:0000 — proves the page-table slot still points at WRAM.
  auto w = snes.system_bus->DebugWrite(0x7E'0000U, 0xC3U);
  REQUIRE(w.ok);
  auto r = snes.system_bus->DebugRead(0x00'0000U);
  REQUIRE(r.ok);
  REQUIRE(r.value == 0xC3U);
}

TEST_CASE("HiROM: WRAM at banks $7E-$7F survives the full-bank ROM mapping", "[unit][hirom]") {
  SNES snes;
  auto rom = MakeHiRom(kFullHiRomSize);
  snes.LoadRom(rom);

  auto w = snes.system_bus->DebugWrite(0x7E'1234U, 0x77U);
  REQUIRE(w.ok);
  auto r = snes.system_bus->DebugRead(0x7E'1234U);
  REQUIRE(r.ok);
  REQUIRE(r.value == 0x77U);
}

TEST_CASE("HiROM: CPU MMIO page at $00:42xx still routes to CpuMmio", "[unit][hirom]") {
  SNES snes;
  auto rom = MakeHiRom(kFullHiRomSize);
  snes.LoadRom(rom);

  // $4210 (RDNMI) is a read-clear register inside the CPU MMIO region — that
  // its page entry isn't kMemory proves we didn't blanket-map ROM over the
  // MMIO surface.
  const auto& entry = GetEntry(snes, 0x00U, 0x42U);
  REQUIRE(entry.kind == PageDeviceKind::kSameClockMmio);
  REQUIRE(GetEntry(snes, 0x80U, 0x42U).kind == PageDeviceKind::kSameClockMmio);
}

// ---------------------------------------------------------------------------
// Layer 4: HiROM SRAM
// ---------------------------------------------------------------------------

TEST_CASE("Cartridge: LoadHiRom sizes SRAM from the internal header", "[unit][hirom]") {
  SNES snes;
  auto rom = MakeHiRom(kFullHiRomSize, 0x03U);  // 8 KiB
  snes.LoadRom(rom);
  REQUIRE(snes.GetCartridge().SramSize() == 0x2000U);
}

TEST_CASE("HiROM: SRAM round-trip at $20:6000 (round-trip through the bus)", "[unit][hirom]") {
  SNES snes;
  auto rom = MakeHiRom(kFullHiRomSize, 0x03U);  // 8 KiB
  snes.LoadRom(rom);

  Cartridge& cart = snes.GetCartridge();
  auto& bus = snes.GetSystemBus();

  auto wp = bus.Plan(0x20'6000U, BusAccessType::kWrite, 0xA5U);
  (void)bus.Follow(wp, 0, cart.GetDeviceId());
  auto rp = bus.Plan(0x20'6000U, BusAccessType::kRead);
  auto rr = bus.Follow(rp, 0, cart.GetDeviceId());
  REQUIRE(rr.data == 0xA5U);
  REQUIRE(cart.SramDirty());
}

TEST_CASE("HiROM: SRAM mirror $A0:6000 aliases $20:6000 (FASTROM SRAM half)", "[unit][hirom]") {
  SNES snes;
  auto rom = MakeHiRom(kFullHiRomSize, 0x03U);
  snes.LoadRom(rom);

  Cartridge& cart = snes.GetCartridge();
  auto& bus = snes.GetSystemBus();

  auto wp = bus.Plan(0xA0'6000U, BusAccessType::kWrite, 0x5AU);
  (void)bus.Follow(wp, 0, cart.GetDeviceId());
  auto rp = bus.Plan(0x20'6000U, BusAccessType::kRead);
  auto rr = bus.Follow(rp, 0, cart.GetDeviceId());
  REQUIRE(rr.data == 0x5AU);
}

TEST_CASE("HiROM: SRAM mirrors across the 8 KiB window when SRAM is smaller", "[unit][hirom]") {
  // 2 KiB SRAM in an 8 KiB CPU window — writing the second 2 KiB chunk via
  // $20:6800 must alias back to $20:6000.
  SNES snes;
  auto rom = MakeHiRom(kFullHiRomSize, 0x01U);  // 2 KiB
  snes.LoadRom(rom);
  REQUIRE(snes.GetCartridge().SramSize() == 0x800U);

  Cartridge& cart = snes.GetCartridge();
  auto& bus = snes.GetSystemBus();

  auto wp = bus.Plan(0x20'6800U, BusAccessType::kWrite, 0x33U);
  (void)bus.Follow(wp, 0, cart.GetDeviceId());
  auto rp = bus.Plan(0x20'6000U, BusAccessType::kRead);
  auto rr = bus.Follow(rp, 0, cart.GetDeviceId());
  REQUIRE(rr.data == 0x33U);
}

// ---------------------------------------------------------------------------
// Layer 5: FASTROM scope on HiROM
// ---------------------------------------------------------------------------

TEST_CASE("HiROM: fast-bank pages start at 8 mcyc and flip to 6 with MEMSEL=1", "[unit][hirom][fastrom]") {
  SNES snes;
  auto rom = MakeHiRom(kFullHiRomSize);
  snes.LoadRom(rom);

  // Half-bank ROM in the FASTROM mirror.
  REQUIRE(GetAccessSpeed(snes, 0x80U, 0x80U) == 8);
  // Full-bank ROM in the FASTROM mirror.
  REQUIRE(GetAccessSpeed(snes, 0xC0U, 0x00U) == 8);

  WriteMemSel(snes, 0x01U);

  REQUIRE(GetAccessSpeed(snes, 0x80U, 0x80U) == 6);
  REQUIRE(GetAccessSpeed(snes, 0xBFU, 0xFFU) == 6);
  REQUIRE(GetAccessSpeed(snes, 0xC0U, 0x00U) == 6);
  REQUIRE(GetAccessSpeed(snes, 0xFFU, 0xFFU) == 6);
}

TEST_CASE("HiROM: slow banks $00-$3F and $40-$7D never become fast", "[unit][hirom][fastrom]") {
  SNES snes;
  auto rom = MakeHiRom(kFullHiRomSize);
  snes.LoadRom(rom);
  WriteMemSel(snes, 0x01U);

  REQUIRE(GetAccessSpeed(snes, 0x00U, 0x80U) == 8);
  REQUIRE(GetAccessSpeed(snes, 0x3FU, 0xFFU) == 8);
  REQUIRE(GetAccessSpeed(snes, 0x40U, 0x00U) == 8);
  REQUIRE(GetAccessSpeed(snes, 0x7DU, 0xFFU) == 8);
}

TEST_CASE("HiROM: MEMSEL does not retime the LowRAM mirror or CPU MMIO", "[unit][hirom][fastrom]") {
  SNES snes;
  auto rom = MakeHiRom(kFullHiRomSize);
  snes.LoadRom(rom);
  WriteMemSel(snes, 0x01U);

  // LowRAM mirror under the fast-bank mirror $80-$BF pages $00-$1F.
  REQUIRE(GetAccessSpeed(snes, 0x80U, 0x00U) == 8);
  REQUIRE(GetAccessSpeed(snes, 0x80U, 0x1FU) == 8);
  // CPU MMIO under $80-$BF page $42 — the 6-cycle fast bus class; MEMSEL
  // does not retime it.
  REQUIRE(GetAccessSpeed(snes, 0x80U, 0x42U) == 6);
}

TEST_CASE("HiROM: SNES::Reset clears MEMSEL and reverts HiROM fast banks to slow", "[unit][hirom][fastrom]") {
  SNES snes;
  auto rom = MakeHiRom(kFullHiRomSize);
  snes.LoadRom(rom);
  WriteMemSel(snes, 0x01U);
  REQUIRE(GetAccessSpeed(snes, 0xC0U, 0x00U) == 6);

  snes.Reset();

  REQUIRE_FALSE(snes.GetCpuMmio().IsFastRomEnabled());
  REQUIRE(GetAccessSpeed(snes, 0x80U, 0x80U) == 8);
  REQUIRE(GetAccessSpeed(snes, 0xC0U, 0x00U) == 8);
}

// ---------------------------------------------------------------------------
// Layer 6: ROM byte reads via the bus
// ---------------------------------------------------------------------------

TEST_CASE("HiROM: bus read at $40:1234 returns ROM[$001234]", "[unit][hirom]") {
  SNES snes;
  auto rom = MakeHiRom(kFullHiRomSize);
  // Plant a deterministic byte that doesn't match the page-fill pattern.
  rom[0x001234U] = 0xDEU;
  rom[0x3D5678U] = 0xADU;
  snes.LoadRom(rom);

  auto r1 = snes.system_bus->DebugRead(0x40'1234U);
  REQUIRE(r1.ok);
  REQUIRE(r1.value == 0xDEU);

  // FASTROM mirror $C0+: same ROM byte as the slow $40+ surface.
  auto r2 = snes.system_bus->DebugRead(0x7D'5678U);
  REQUIRE(r2.ok);
  REQUIRE(r2.value == 0xADU);
}

TEST_CASE("HiROM: half-bank read at $00:F000 returns ROM[$00F000]", "[unit][hirom]") {
  SNES snes;
  auto rom = MakeHiRom(kFullHiRomSize);
  rom[0x00F000U] = 0xBEU;
  snes.LoadRom(rom);

  auto r = snes.system_bus->DebugRead(0x00'F000U);
  REQUIRE(r.ok);
  REQUIRE(r.value == 0xBEU);
}

// ---------------------------------------------------------------------------
// Layer 7: bookkeeping
// ---------------------------------------------------------------------------

TEST_CASE("HiROM: Cartridge reports MapperKind::kHiROM after LoadHiRom", "[unit][hirom]") {
  SNES snes;
  REQUIRE(snes.GetCartridge().GetMapperKind() == MapperKind::kNone);

  auto rom = MakeHiRom(kFullHiRomSize);
  snes.LoadRom(rom);
  REQUIRE(snes.GetCartridge().GetMapperKind() == MapperKind::kHiROM);
}

TEST_CASE("HiROM: short ROM falls back to slow-path modulo wrap (no fast pointer)", "[unit][hirom]") {
  // 64 KiB image — one HiROM bank. ROM is too small to back a fast pointer
  // for pages near the top of the bank-encoded offset surface, so the entry
  // there must drop its fast_read_ptr and rely on the modulo slow path.
  SNES snes;
  auto rom = MakeHiRom(0x10000U);
  snes.LoadRom(rom);

  // $40:00 is in-range (offset 0) — fast pointer must be live.
  REQUIRE(GetEntry(snes, 0x40U, 0x00U).fast_read_ptr != nullptr);
  // $7D:FF is at offset $3DFF00, well past 64 KiB — fast pointer must be null,
  // forcing the slow path which mod-wraps inside ReadRegister.
  REQUIRE(GetEntry(snes, 0x7DU, 0xFFU).fast_read_ptr == nullptr);
}
