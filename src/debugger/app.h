#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

struct GLFWwindow;

#include "pupsnes/debugger/breakpoints.h"
#include "pupsnes/debugger/bus_event_log.h"
#include "pupsnes/debugger/error_log.h"
#include "pupsnes/debugger/microop_trace.h"
#include "pupsnes/debugger/run_control.h"
#include "pupsnes/debugger/trace.h"
#include "pupsnes/hw/snes.h"

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

struct UiState {
  std::string rom_path_input;
  uint64_t step_count = 10;
  RegisterHistory register_history;
  bool open_load_rom_dialog = false;
  std::string load_rom_dir = "build/dev/test-roms";
  std::string load_rom_error;
  bool follow_pc = true;
  SnesAddrT disasm_address = 0x008000;
  SnesAddrT memory_address = 0x7E0000;
  std::optional<SnesAddrT> selected_memory_address = std::nullopt;
  uint8_t memory_edit_value = 0;
  int memory_region = 0;
  int error_source_filter = -1;
  int error_severity_filter = -1;
  size_t trace_last_seen_size = 0;
  bool show_style_editor = false;
  bool show_demo_window = false;
  bool show_metrics_window = false;
  bool show_debug_log_window = false;
  bool show_id_stack_tool = false;
  bool show_about_window = false;
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

 private:
  bool InitWindow();
  void ShutdownWindow();
  void TickEmulation();
  void Render();
  void RenderMenuBar();
  void RenderLoadRomDialog();
  void RenderFatalModal();

  static void GlfwErrorCallback(int code, const char* description);

  static DebuggerApp* current_app_;

  GLFWwindow* window_ = nullptr;
  double perf_last_time_ = 0.0;
  TimeMasterT perf_last_master_ = 0;
  float perf_fps_ = 0.0F;
  float perf_realtime_pct_ = 0.0F;
  bool loaded_rom_ = false;
  std::string loaded_rom_path_;
  std::optional<std::string> fatal_error_;
  SNES snes_;
  BreakpointSet breakpoints_;
  TraceLog trace_log_;
  BusEventLog bus_event_log_;
  MicroOpTrace microop_trace_;
  ErrorLog error_log_;
  RunControl run_control_;
  UiState ui_state_;
};

}  // namespace pupsnes::debugger
