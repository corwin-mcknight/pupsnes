#pragma once

#include <cstdint>

namespace pupsnes {

class Cartridge;
class SystemBus;

// Abstract page-table layout strategy for a cartridge. Not a Device — has no
// DeviceId, no MMIO surface. Owned by Cartridge; holds non-owning references
// back to its Cartridge and the SystemBus so it can mutate page-table entries
// without re-plumbing context on every call.
//
// Subclasses (LoRomMapper / HiRomMapper / ExHiRomMapper / future SA-1 / S-DD1
// / SPC7110 mappers) override MapInitial() to write the initial page-table
// layout from the Cartridge's rom_/sram_ buffers. OnMemSelChanged() handles
// the FASTROM speed flip for mappers that expose a fast-bank range; the
// default no-op covers mappers without one (e.g. SA-1 which has its own
// clock).
//
// Runtime-banking mappers (SA-1 "Super MMC" via $2220-$2223, S-DD1's MMC,
// SPC7110 data-pack offsets) do NOT extend the base class with a virtual
// Rebank method — instead each mapper subclass exposes its own typed banking
// API, and the owning coprocessor Device holds a typed pointer to that
// subclass. Keeps the banking config type-safe and avoids encoding tricks.
class Mapper {
 public:
  Mapper(Cartridge& cart, SystemBus& bus) : cart_(cart), bus_(bus) {}
  virtual ~Mapper() = default;

  Mapper(const Mapper&) = delete;
  Mapper& operator=(const Mapper&) = delete;

  // Write the cartridge's initial page-table layout into the SystemBus.
  // Called once at cart-build time after Cartridge::rom_/sram_ have been
  // populated. ROM is wired with fast_read_ptr where the 256-byte window
  // fits entirely inside the buffer; sub-bank tails fall back to the slow
  // path's modulo wrap via Cartridge::ReadRegister.
  virtual void MapInitial() = 0;

  // Re-time the fast-bank range when MEMSEL bit 0 (CPU $420D bit 0) flips.
  // 6 master cycles when fast, 8 when slow. Default no-op for mappers
  // without a FastROM bank (SA-1, etc.).
  virtual void OnMemSelChanged(bool /*fast*/) {}

 protected:
  Cartridge& cart_;
  SystemBus& bus_;
};

// LoROM layout (~1500 commercial carts). ROM in $00-$7D/$80-$FF pages
// $80-$FF (32 KiB per bank). FASTROM affects $80-$FF only. SRAM, when
// present, lives in pages $00-$7F of banks $70-$7D and $F0-$FF.
class LoRomMapper : public Mapper {
 public:
  using Mapper::Mapper;
  void MapInitial() override;
  void OnMemSelChanged(bool fast) override;
};

// HiROM layout (~500 commercial carts). Half-bank ROM at $00-$3F / $80-$BF
// pages $80-$FF; full-bank ROM at $40-$7D / $C0-$FF. FASTROM affects the
// upper-half banks. SRAM at $20-$3F / $A0-$BF pages $60-$7F.
class HiRomMapper : public Mapper {
 public:
  using Mapper::Mapper;
  void MapInitial() override;
  void OnMemSelChanged(bool fast) override;
};

// ExHiROM layout (2 commercial carts: Tales of Phantasia, Dai Kaiju
// Monogatari 2). Like HiROM but the lower banks ($00-$3F / $40-$7D)
// see the SECOND half of the ROM (bytes $400000+), not a FASTROM
// mirror of the first half. Smaller-half ROMs wrap within the lower-bank
// space via modulo on the smaller-half size. SRAM placement matches HiROM
// ($20-$3F / $A0-$BF pages $60-$7F) for the initial bring-up; the
// SHVC-LJ3M-01 board variant which puts SRAM at $80-$BF is a known
// limitation handled separately if a real cart needs it.
class ExHiRomMapper : public Mapper {
 public:
  using Mapper::Mapper;
  void MapInitial() override;
  void OnMemSelChanged(bool fast) override;
};

}  // namespace pupsnes
