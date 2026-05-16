#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "pupsnes/core/snes.h"
#include "pupsnes/core/types.h"

struct GLFWwindow;

namespace pupsnes::emulator {

struct FileShortcut {
  std::string label;
  std::string path;
};

struct UiState {
  // ROM browser state.
  std::string last_rom_path;
  std::string load_rom_dir = "build/dev/test-roms";
  std::string load_rom_error;
  std::vector<FileShortcut> load_rom_shortcuts;
  bool open_load_rom_dialog = false;

  // Display options.
  bool integer_scale = false;
  bool maintain_aspect = true;
  bool show_about = false;

  // Emulation control.
  bool paused = false;
  // 1.0 = real-hardware speed. Applied to the wall-clock master-cycle budget.
  float speed_multiplier = 1.0F;
};

class EmulatorApp {
 public:
  EmulatorApp();
  ~EmulatorApp();
  EmulatorApp(const EmulatorApp&) = delete;
  EmulatorApp(EmulatorApp&&) = delete;
  EmulatorApp& operator=(const EmulatorApp&) = delete;
  EmulatorApp& operator=(EmulatorApp&&) = delete;

  int Run(const std::optional<std::string>& initial_rom_path);

 private:
  bool InitWindow();
  void ShutdownWindow();
  bool LoadRomFromPath(const std::string& path);
  void ResetMachine();
  void TickEmulation();
  // Write the cartridge SRAM out to `loaded_rom_save_path_` if the cart has
  // SRAM and it's been written since the last flush. Safe to call when no ROM
  // is loaded (no-op).
  void FlushSramToDisk();
  void PollControllerInput();
  void Render();
  void RenderMenuBar();
  void RenderBackgroundFrame();
  void RenderLoadRomDialog();
  void RenderFatalModal();
  void UploadFrontBufferToTexture();
  void InitFileShortcuts();
  void LoadConfig();
  void SaveConfig();
  static std::string GetConfigPath();
  static void GlfwErrorCallback(int code, const char* description);

  static EmulatorApp* current_app_;

  GLFWwindow* window_ = nullptr;
  SNES snes_;
  bool loaded_rom_ = false;
  std::string loaded_rom_path_;
  // ROM path with extension replaced by ".srm". Empty when no ROM is loaded
  // or the cart declares no SRAM. Cached at load time so that FlushSramToDisk
  // doesn't have to re-derive it during shutdown teardown.
  std::string loaded_rom_save_path_;
  std::optional<std::string> fatal_error_;
  std::chrono::steady_clock::time_point last_tick_time_{};
  std::chrono::steady_clock::time_point last_sram_flush_time_{};

  // Performance overlay sample.
  double perf_last_time_ = 0.0;
  TimeMasterT perf_last_master_ = 0;
  float perf_fps_ = 0.0F;
  float perf_realtime_pct_ = 0.0F;

  // GL texture carrying the most-recently-uploaded PPU front buffer.
  uint32_t ppu_tex_id_ = 0;
  uint32_t ppu_tex_w_ = 0;
  uint32_t ppu_tex_h_ = 0;

  UiState ui_state_;
};

}  // namespace pupsnes::emulator
