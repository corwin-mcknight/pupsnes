#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

struct GLFWwindow;

#include "pupsnes/debugger/breakpoints.h"
#include "pupsnes/debugger/error_log.h"
#include "pupsnes/debugger/run_control.h"
#include "pupsnes/debugger/trace.h"
#include "pupsnes/hw/snes.h"

namespace pupsnes::debugger {

struct UiState {
  std::string rom_path_input;
  uint64_t step_count = 10;
  bool follow_pc = true;
  SnesAddrT disasm_address = 0x008000;
  SnesAddrT memory_address = 0x7E0000;
  std::optional<SnesAddrT> selected_memory_address = std::nullopt;
  uint8_t memory_edit_value = 0;
  int memory_region = 0;
  int error_source_filter = -1;
  int error_severity_filter = -1;
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
  void RenderFatalModal();

  static void GlfwErrorCallback(int code, const char* description);

  static DebuggerApp* current_app_;

  GLFWwindow* window_ = nullptr;
  bool loaded_rom_ = false;
  std::string loaded_rom_path_;
  std::optional<std::string> fatal_error_;
  SNES snes_;
  BreakpointSet breakpoints_;
  TraceLog trace_log_;
  ErrorLog error_log_;
  RunControl run_control_;
  UiState ui_state_;
};

}  // namespace pupsnes::debugger
