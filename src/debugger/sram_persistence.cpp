#include "pupsnes/debugger/sram_persistence.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "pupsnes/hw/rom/cartridge.h"

namespace pupsnes::debugger {

std::string SramPersistence::Load(Cartridge& cart, const std::filesystem::path& rom_path, bool enabled) {
  save_path_ = rom_path;
  save_path_.replace_extension(".srm");
  load_error_.clear();
  if (!enabled || cart.SramSize() == 0U) return {};

  std::error_code ec;
  const bool exists = std::filesystem::exists(save_path_, ec);
  if (!ec && !exists) return {};

  // A failed read must not later overwrite an existing save with fresh SRAM.
  if (ec || !std::filesystem::is_regular_file(save_path_, ec) || ec) {
    load_error_ = "Unable to read SRAM save: " + save_path_.string();
    return load_error_;
  }
  std::ifstream stream(save_path_, std::ios::binary);
  std::vector<uint8_t> data(cart.SramSize());
  stream.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
  if (stream.bad() || (stream.fail() && !stream.eof())) {
    load_error_ = "Unable to read SRAM save: " + save_path_.string();
    return load_error_;
  }
  data.resize(static_cast<std::size_t>(stream.gcount()));
  cart.LoadSram(data);
  return {};
}

std::string SramPersistence::Flush(Cartridge& cart, bool enabled) {
  if (!enabled || save_path_.empty() || cart.SramSize() == 0U || !cart.SramDirty()) return {};
  if (!load_error_.empty()) return load_error_;

  // Write beside the save, close successfully, then atomically replace it.
  // Exclusive creation avoids overwriting another instance's pending write.
  auto temporary = save_path_;
  temporary += ".tmp";
  std::FILE* stream = std::fopen(temporary.string().c_str(), "wbx");
  if (stream == nullptr) return "Unable to create SRAM save: " + temporary.string();
  const auto data = cart.SramView();
  const bool wrote = std::fwrite(data.data(), 1, data.size(), stream) == data.size();
  const bool closed = std::fclose(stream) == 0;
  std::error_code ec;
  if (wrote && closed) {
    std::filesystem::rename(temporary, save_path_, ec);
    if (!ec) {
      cart.ClearSramDirty();
      return {};
    }
  }
  std::filesystem::remove(temporary, ec);
  return "Unable to write SRAM save: " + save_path_.string();
}

}  // namespace pupsnes::debugger
