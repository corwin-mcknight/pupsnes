#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace pupsnes {

// What kind of CPU-bus memory map this cartridge uses. The mapper governs
// how raw ROM bytes are stitched into the 24-bit CPU address space.
//
// LoROM ($20/$30) and HiROM ($21/$31) are the two standard layouts; about
// 2000 of the ~2200 commercial SNES carts use one of these. ExHiROM ($25/$35)
// is the extended-HiROM layout used by exactly two commercial games
// (Tales of Phantasia, Dai Kaiju Monogatari 2) where banks $00-$3F hold a
// DIFFERENT block of ROM bytes than banks $80-$FF.
//
// kNone is the "detection failed" sentinel — the cartridge factory rejects
// any profile whose mapper is kNone and routes the caller to a diagnostic
// message.
enum class MapperKind : uint8_t {
  kNone = 0,
  kLoROM = 1,
  kHiROM = 2,
  kExHiROM = 3,
};

// Optional coprocessor chip on the cart. Detection comes from the $FFD6
// chipset byte (high nibble) plus the $FFBF subtype byte for Fxh "custom"
// chipsets. Variant resolution (DSP-1 vs DSP-2 vs DSP-3 vs DSP-4; GSU1 vs
// GSU2; ST010 vs ST011) is left to the cartridge builder using game-code
// lookup — the header byte doesn't distinguish DSP variants.
//
// Most are placeholders for future implementation. Only kNone is supported
// in this round; the enum is defined now so CartProfile can carry the
// detected value through to a future builder without a future schema change.
enum class Coprocessor : uint8_t {
  kNone = 0,
  kDSP = 1,          // NEC uPD77C25: DSP-1 / DSP-1A / DSP-1B / DSP-2 / DSP-3 / DSP-4
  kGSU = 2,          // SuperFX: MarioChip1 / GSU1 / GSU2
  kOBC1 = 3,         // OBJ Controller (Metal Combat)
  kSA1 = 4,          // Super Accelerator 1 — 10.74 MHz 65C816
  kSDD1 = 5,         // Data decompression (Star Ocean, Street Fighter Alpha 2)
  kSRTC = 6,         // S-RTC (Dai Kaiju Monogatari 2)
  kSPC7110 = 7,      // Data decompression + optional RTC-4513 (Far East of Eden Zero, etc.)
  kSTxxx = 8,        // ST010 / ST011 / ST018 — custom pre-programmed coprocessors
  kCX4 = 9,          // Capcom CX4 — RISC math (Mega Man X2/X3)
  kSGB = 10,         // Super Game Boy bridge
  kSatellaview = 11, // BS-X — Satellaview data pack
  kSufamiTurbo = 12, // Sufami Turbo mini-cart adaptor
};

// Region carries the raw $FFD9 country byte and the derived display rate.
// is_pal collapses the country byte into the two practical timing groups
// emulators care about; corner cases (France SECAM ~50 Hz, Brazil PAL-M
// ~60 Hz) are documented but not yet enforced in PPU timing.
struct Region {
  uint8_t country_code;  // raw $FFD9 byte
  bool is_pal;
};

// Result of parsing a cartridge ROM image. Always fully populated: even when
// detection fails, the diagnostic fields are filled in so a downstream
// builder can construct a specific error message.
//
// On detection success (mapper != kNone), the identification fields drive
// CartridgeRegistry::Build dispatch. On detection failure (mapper == kNone),
// CartDetection::diagnostic explains why.
struct CartProfile {
  // ----- Identification (what to dispatch on) -----
  MapperKind mapper = MapperKind::kNone;
  Coprocessor coproc = Coprocessor::kNone;
  std::size_t sram_bytes = 0;
  bool has_battery = false;
  bool has_rtc = false;
  uint8_t custom_subtype = 0xFFU;  // $FFBF for Fxh chipsets; 0xFF when not applicable
  bool fastrom_capable = false;
  Region region{0xFFU, false};

  // ----- Raw header bytes (diagnostic + override convenience) -----
  std::size_t input_size = 0;
  bool has_smc_copier_header = false;
  uint8_t lorom_map_mode_byte = 0xFFU;
  uint8_t hirom_map_mode_byte = 0xFFU;
  uint8_t exhirom_map_mode_byte = 0xFFU;
  uint8_t chipset_byte = 0xFFU;        // $FFD6 of the chosen header
  bool lorom_checksum_valid = false;
  bool hirom_checksum_valid = false;
  bool exhirom_checksum_valid = false;
};

// Output of DetectCartProfile. `diagnostic` is empty when detection
// succeeded (profile.mapper != kNone). Otherwise it explains the failure
// in language a frontend can show the user — "ROM is empty", "SMC copier
// header still attached", "no recognised map mode byte and no valid
// checksum", etc.
struct CartDetection {
  CartProfile profile;
  std::string diagnostic;
};

// Parse `rom_data` as an SNES cartridge image and return what we found.
// Pure function: no allocation outside the result's std::string, no side
// effects, never throws. Tolerates empty / truncated / corrupt inputs by
// returning kNone with a specific diagnostic.
//
// Does NOT strip SMC copier headers. If a copier header is detected the
// function reports kNone with the "strip first" diagnostic — same policy
// as the legacy Cartridge loaders, preserved so a copier-prefixed image
// never silently maps as a 512-byte-offset cart.
[[nodiscard]] CartDetection DetectCartProfile(std::span<const uint8_t> rom_data);

}  // namespace pupsnes
