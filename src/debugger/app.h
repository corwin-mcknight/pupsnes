#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct GLFWwindow;

#include "pupsnes/core/snes.h"
#include "pupsnes/debugger/breakpoints.h"
#include "pupsnes/debugger/bus_event_log.h"
#include "pupsnes/debugger/emu_event_log.h"
#include "pupsnes/debugger/error_log.h"
#include "pupsnes/debugger/fan_out_trace_sink.h"
#include "pupsnes/debugger/file_trace_sink.h"
#include "pupsnes/debugger/microop_trace.h"
#include "pupsnes/debugger/run_control.h"
#include "pupsnes/debugger/sha1.h"
#include "pupsnes/debugger/trace.h"
#include "time_format.h"

namespace pupsnes::debugger {

struct RegisterHistory {
  bool valid = false;
  uint64_t retired = 0;
  uint16_t A = 0;
  uint16_t X = 0;
  uint16_t Y = 0;
  uint16_t SP = 0;
  uint16_t DP = 0;
  uint16_t PC = 0;
  uint8_t PBR = 0;
  uint8_t DBR = 0;
  bool N = false, V = false, M = true, Xf = true, D = false, I = true, Z = false, C = false, E = true;
  float hi_A = 0.0F, hi_X = 0.0F, hi_Y = 0.0F, hi_SP = 0.0F, hi_DP = 0.0F;
  float hi_PC = 0.0F, hi_PBR = 0.0F, hi_DBR = 0.0F;
  float hi_N = 0.0F, hi_V = 0.0F, hi_M = 0.0F, hi_Xf = 0.0F, hi_D = 0.0F;
  float hi_I = 0.0F, hi_Z = 0.0F, hi_C = 0.0F, hi_E = 0.0F;
};

struct FileShortcut {
  std::string label;
  std::string path;
};

struct UiState {
  std::string rom_path_input;
  uint64_t step_count = 10;
  RegisterHistory register_history;
  bool open_load_rom_dialog = false;
  std::string load_rom_dir = "build/dev/test-roms";
  std::string load_rom_error;
  std::vector<FileShortcut> load_rom_shortcuts;
  std::string last_rom_path;
  bool follow_pc = true;
  SnesAddrT disasm_address = 0x008000;
  SnesAddrT memory_address = 0x7E0000;
  std::optional<SnesAddrT> selected_memory_address = std::nullopt;
  uint8_t memory_edit_value = 0;
  int memory_region = 0;
  size_t trace_last_seen_size = 0;
  size_t bus_last_seen_size = 0;
  size_t event_last_seen_size = 0;
  uint32_t event_category_filter = kAllEmuEventCategoriesMask;
  TimeDisplayMode time_display_mode = TimeDisplayMode::kAbsolute;
  bool show_style_editor = false;
  bool show_demo_window = false;
  bool show_metrics_window = false;
  bool show_debug_log_window = false;
  bool show_id_stack_tool = false;
  bool show_about_window = false;
  bool show_registers_panel = true;
  bool show_disasm_panel = true;
  bool show_memory_panel = true;
  bool show_stack_panel = true;
  bool show_ppu_panel = true;
  bool show_ppu_viewer_panel = true;
  bool show_dma_panel = true;
  bool show_trace_panel = true;
  bool show_trace_record_panel = true;
  std::string trace_record_path = "pupsnes-trace.log";
  bool trace_record_reset_on_start = true;
  bool show_microop_trace_panel = true;
  bool show_scheduler_panel = true;
  bool show_bus_panel = true;
  bool show_log_panel = true;
  bool show_controller_panel = true;
  bool show_snes_panel = false;
  // Emulated SNES time per real time. 1.0 = 100% real-hardware speed. Always
  // applied (the CPU is budgeted dt * kMasterClockHz * multiplier cycles per
  // host frame). When < 0.25, the PPU preview shows the in-progress frame
  // overlay so the user can watch mid-frame rendering at slow speeds.
  float speed_multiplier = 1.0F;
};

class DebuggerApp {
 public:
  DebuggerApp();
  ~DebuggerApp();

  int Run(const std::optional<std::string>& initial_rom_path);

  [[nodiscard]] bool HasLoadedRom() const { return loaded_rom_; }
  [[nodiscard]] std::string_view GetLoadedRomPath() const { return loaded_rom_path_; }
  [[nodiscard]] SnesAddrT GetCurrentPc() const;

  [[nodiscard]] SNES& GetSnes() { return snes_; }
  [[nodiscard]] const SNES& GetSnes() const { return snes_; }
  [[nodiscard]] BreakpointSet& GetBreakpoints() { return breakpoints_; }
  [[nodiscard]] const BreakpointSet& GetBreakpoints() const { return breakpoints_; }
  [[nodiscard]] TraceLog& GetTraceLog() { return trace_log_; }
  [[nodiscard]] const TraceLog& GetTraceLog() const { return trace_log_; }
  [[nodiscard]] BusEventLog& GetBusEventLog() { return bus_event_log_; }
  [[nodiscard]] const BusEventLog& GetBusEventLog() const { return bus_event_log_; }
  [[nodiscard]] EmuEventLog& GetEmuEventLog() { return emu_event_log_; }
  [[nodiscard]] const EmuEventLog& GetEmuEventLog() const { return emu_event_log_; }
  [[nodiscard]] MicroOpTrace& GetMicroOpTrace() { return microop_trace_; }
  [[nodiscard]] const MicroOpTrace& GetMicroOpTrace() const { return microop_trace_; }
  [[nodiscard]] ErrorLog& GetErrorLog() { return error_log_; }
  [[nodiscard]] const ErrorLog& GetErrorLog() const { return error_log_; }
  [[nodiscard]] RunControl& GetRunControl() { return run_control_; }
  [[nodiscard]] const RunControl& GetRunControl() const { return run_control_; }
  [[nodiscard]] UiState& GetUiState() { return ui_state_; }
  [[nodiscard]] const UiState& GetUiState() const { return ui_state_; }

  bool LoadRomFromPath(const std::string& path);
  void ResetMachine();
  bool WriteMemory(SnesAddrT address, uint8_t value);
  void JumpToAddress(SnesAddrT address);
  void PushHostError(std::string message, ErrorSeverity severity = ErrorSeverity::kError);

  bool StartTraceRecording(const std::string& path, bool reset_rom);
  void StopTraceRecording();
  void FlushTraceRecording();
  [[nodiscard]] bool IsTraceRecording() const { return file_trace_sink_ != nullptr; }
  [[nodiscard]] uint64_t TraceRecordedLines() const;
  [[nodiscard]] const std::string& TraceRecordingPath() const { return trace_recording_path_; }
  [[nodiscard]] const std::string& TraceLastError() const { return trace_last_error_; }

 private:
  bool InitWindow();
  void ShutdownWindow();
  void TickEmulation();
  void Render();
  void RenderMenuBar();
  void RenderFatalModal();
  // Reads the keyboard each frame and forwards key transitions to the P1
  // joypad. Edge-detected so the on-screen controller panel can still toggle
  // bits when the user isn't using the keyboard.
  void PollGameInput();
  void LoadAppConfig();
  void SaveAppConfig();
  static std::string GetConfigPath();

  static void GlfwErrorCallback(int code, const char* description);

  static DebuggerApp* current_app_;

  GLFWwindow* window_ = nullptr;
  double perf_last_time_ = 0.0;
  TimeMasterT perf_last_master_ = 0;
  float perf_fps_ = 0.0F;
  float perf_realtime_pct_ = 0.0F;
  std::chrono::steady_clock::time_point last_tick_time_{};
  bool loaded_rom_ = false;
  std::string loaded_rom_path_;
  std::optional<std::string> fatal_error_;
  SNES snes_;
  BreakpointSet breakpoints_;
  TraceLog trace_log_;
  FanOutTraceSink fan_out_trace_sink_;
  BusEventLog bus_event_log_;
  EmuEventLog emu_event_log_;
  MicroOpTrace microop_trace_;
  ErrorLog error_log_;
  RunControl run_control_;
  Sha1Digest rom_sha1_{};
  std::unique_ptr<FileTraceSink> file_trace_sink_;
  std::string trace_recording_path_;
  std::string trace_last_error_;
  UiState ui_state_;
  // Edge-detection state for keyboard → P1 polling. Index lines up with the
  // mapping table in app.cpp.
  std::array<bool, 13> p1_key_was_down_{};
};

}  // namespace pupsnes::debugger
