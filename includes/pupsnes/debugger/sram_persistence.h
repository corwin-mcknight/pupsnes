#pragma once

#include <filesystem>
#include <string>

namespace pupsnes {
class Cartridge;
}

namespace pupsnes::debugger {

// Host-side battery save lifecycle. Changing enabled never replaces live SRAM;
// disk contents are restored only when a cartridge is loaded.
class SramPersistence {
 public:
  [[nodiscard]] std::string Load(Cartridge& cart, const std::filesystem::path& rom_path, bool enabled);
  [[nodiscard]] std::string Flush(Cartridge& cart, bool enabled);

 private:
  std::filesystem::path save_path_;
  std::string load_error_;
};

}  // namespace pupsnes::debugger
