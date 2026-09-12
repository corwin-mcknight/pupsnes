#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>

#include "pupsnes/hw/rom/cart_profile.h"
#include "pupsnes/hw/rom/cartridge.h"

namespace pupsnes {

class SNES;

// Result of building a cartridge from raw bytes + a detected profile.
// A successful registry builder returns ownership of the newly mapped cart
// with `ok=true`. SNES::LoadRom / LoadRomWithProfile consume that pointer
// when installing the cartridge, so their returned result has an empty cart.
// `message` describes the load or failure, and `profile` records the profile
// used for dispatch (auto-detected or supplied by the caller).
struct BuildResult {
  bool ok = false;
  std::unique_ptr<Cartridge> cart;
  CartProfile profile{};
  std::string message;
};

// Registry of cartridge builders keyed by (mapper, coprocessor). Adding a
// new cart kind is a single Register() call. Coprocessors that are not yet
// implemented are simply not registered — the build path returns a
// helpful "(MapperKind, Coprocessor) is not yet supported" diagnostic when
// asked to construct one.
//
// Owned per-SNES so tests can install fake builders without polluting
// process-wide state. Built-ins are registered by RegisterBuiltins() which
// SNES::SNES() calls during construction.
class CartridgeRegistry {
 public:
  struct Key {
    MapperKind mapper;
    Coprocessor coproc;
    bool operator==(const Key& other) const noexcept {
      return mapper == other.mapper && coproc == other.coproc;
    }
  };
  struct KeyHash {
    std::size_t operator()(const Key& k) const noexcept {
      // Pack into the low 16 bits — MapperKind and Coprocessor are both
      // uint8_t-backed so there is no collision risk in practice.
      return (static_cast<std::size_t>(k.mapper) << 8) | static_cast<std::size_t>(k.coproc);
    }
  };

  // Builder signature. Takes the owning SNES (for Device registration of
  // coprocessors when those land), the raw cart bytes, and the profile that
  // drove dispatch. Returns a populated BuildResult.
  using BuildFn = std::function<BuildResult(SNES*, std::span<const uint8_t>, const CartProfile&)>;

  void Register(Key key, BuildFn fn);

  // Register the standard LoROM / HiROM / ExHiROM builders. Called by
  // SNES::SNES(). Idempotent — re-registering overrides the existing entry,
  // which tests use to swap in fakes.
  void RegisterBuiltins();

  [[nodiscard]] BuildResult Build(SNES* snes, std::span<const uint8_t> bytes, const CartProfile& profile) const;

 private:
  std::unordered_map<Key, BuildFn, KeyHash> builders_;
};

}  // namespace pupsnes
