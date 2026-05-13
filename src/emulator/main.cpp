#include <exception>
#include <optional>
#include <string>

#include "app.h"

int main(int argc, char** argv) {
  try {
    std::optional<std::string> rom_path = std::nullopt;
    if (argc >= 2 && argv[1] != nullptr) {
      rom_path = argv[1];
    }

    pupsnes::emulator::EmulatorApp app;
    return app.Run(rom_path);
  } catch (const std::exception&) {
    return 1;
  }
}
