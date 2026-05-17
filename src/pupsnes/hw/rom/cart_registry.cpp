#include "pupsnes/hw/rom/cart_registry.h"

#include <format>
#include <memory>
#include <string_view>
#include <utility>

#include "pupsnes/core/snes.h"
#include "pupsnes/hw/rom/cartridge.h"
#include "pupsnes/hw/rom/rom_format.h"
#include "pupsnes/memory/systembus.h"

namespace pupsnes {

namespace {

[[nodiscard]] std::string_view MapperLabel(MapperKind kind) noexcept {
  switch (kind) {
    case MapperKind::kNone: return "None";
    case MapperKind::kLoROM: return "LoROM";
    case MapperKind::kHiROM: return "HiROM";
    case MapperKind::kExHiROM: return "ExHiROM";
  }
  return "?";
}

[[nodiscard]] std::string_view CoprocLabel(Coprocessor coproc) noexcept {
  switch (coproc) {
    case Coprocessor::kNone: return "None";
    case Coprocessor::kDSP: return "DSP";
    case Coprocessor::kGSU: return "GSU";
    case Coprocessor::kOBC1: return "OBC1";
    case Coprocessor::kSA1: return "SA-1";
    case Coprocessor::kSDD1: return "S-DD1";
    case Coprocessor::kSRTC: return "S-RTC";
    case Coprocessor::kSPC7110: return "SPC7110";
    case Coprocessor::kSTxxx: return "STxxx";
    case Coprocessor::kCX4: return "CX4";
    case Coprocessor::kSGB: return "Super Game Boy";
    case Coprocessor::kSatellaview: return "Satellaview";
    case Coprocessor::kSufamiTurbo: return "Sufami Turbo";
  }
  return "?";
}

// Each builder constructs a FRESH Cartridge, populates its rom/sram, runs
// its mapper, and returns the cart via BuildResult.cart. The caller
// (SNES::LoadRomWithProfile) destroys the old cartridge after installation
// — the old cart's dtor scrubs the page table of its DeviceId entries via
// SNES::DeregisterDevice → SystemBus::UnmapByDeviceId. New entries written
// by this builder use the new cart's DeviceId and survive the cleanup.
BuildResult BuildLoRom(SNES* snes, std::span<const uint8_t> bytes, const CartProfile& profile) {
  auto cart = std::make_unique<Cartridge>(snes);
  RomLoadResult legacy = cart->LoadLoRom(bytes);
  if (!legacy.ok) {
    return BuildResult{false, nullptr, profile, legacy.message};
  }
  cart->MapLoRom(snes->GetSystemBus());
  return BuildResult{true, std::move(cart), profile, legacy.message};
}

BuildResult BuildHiRom(SNES* snes, std::span<const uint8_t> bytes, const CartProfile& profile) {
  auto cart = std::make_unique<Cartridge>(snes);
  RomLoadResult legacy = cart->LoadHiRom(bytes);
  if (!legacy.ok) {
    return BuildResult{false, nullptr, profile, legacy.message};
  }
  cart->MapHiRom(snes->GetSystemBus());
  return BuildResult{true, std::move(cart), profile, legacy.message};
}

BuildResult BuildExHiRom(SNES* snes, std::span<const uint8_t> bytes, const CartProfile& profile) {
  auto cart = std::make_unique<Cartridge>(snes);
  RomLoadResult legacy = cart->LoadExHiRom(bytes);
  if (!legacy.ok) {
    return BuildResult{false, nullptr, profile, legacy.message};
  }
  cart->MapExHiRom(snes->GetSystemBus());
  return BuildResult{true, std::move(cart), profile, legacy.message};
}

}  // namespace

void CartridgeRegistry::Register(Key key, BuildFn fn) { builders_[key] = std::move(fn); }

void CartridgeRegistry::RegisterBuiltins() {
  Register({MapperKind::kLoROM, Coprocessor::kNone}, &BuildLoRom);
  Register({MapperKind::kHiROM, Coprocessor::kNone}, &BuildHiRom);
  Register({MapperKind::kExHiROM, Coprocessor::kNone}, &BuildExHiRom);
}

BuildResult CartridgeRegistry::Build(SNES* snes, std::span<const uint8_t> bytes, const CartProfile& profile) const {
  const Key key{profile.mapper, profile.coproc};
  const auto it = builders_.find(key);
  if (it == builders_.end()) {
    BuildResult result;
    result.ok = false;
    result.profile = profile;
    result.message = std::format("No builder registered for cart ({}, coproc {}); not yet implemented.",
                                 MapperLabel(profile.mapper), CoprocLabel(profile.coproc));
    return result;
  }
  return it->second(snes, bytes, profile);
}

}  // namespace pupsnes
