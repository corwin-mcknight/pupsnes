#include "app.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "panels/panels.h"
#include "pupsnes/debugger/file_trace_sink.h"
#include "pupsnes/debugger/sha1.h"
#include "pupsnes/hw/cartridge.h"
#include "pupsnes/hw/joypad.h"
#include "pupsnes/hw/sppu/ppu.h"
#include "pupsnes/rom_format.h"

namespace pupsnes::debugger {

DebuggerApp* DebuggerApp::current_app_ = nullptr;

namespace {

// Keyboard → P1 button map. Indices line up with DebuggerApp::p1_key_was_down_
// for edge detection. Layout matches a Snes9x-style default: arrows for the
// D-pad, ASZX for the face-button diamond (A=Y, S=X, Z=B, X=A), Q/W for L/R,
// Enter for Start, Right Shift for Select.
struct KeyMapping {
  ImGuiKey key;
  Joypad::Button button;
  const char* key_label;
};

constexpr std::array<KeyMapping, 13> kP1KeyMap{{
    {ImGuiKey_UpArrow, Joypad::Button::kUp, "Up"},
    {ImGuiKey_DownArrow, Joypad::Button::kDown, "Down"},
    {ImGuiKey_LeftArrow, Joypad::Button::kLeft, "Left"},
    {ImGuiKey_RightArrow, Joypad::Button::kRight, "Right"},
    {ImGuiKey_Z, Joypad::Button::kB, "Z"},
    {ImGuiKey_X, Joypad::Button::kA, "X"},
    {ImGuiKey_A, Joypad::Button::kY, "A"},
    {ImGuiKey_S, Joypad::Button::kX, "S"},
    {ImGuiKey_Q, Joypad::Button::kL, "Q"},
    {ImGuiKey_W, Joypad::Button::kR, "W"},
    {ImGuiKey_Enter, Joypad::Button::kStart, "Enter"},
    {ImGuiKey_KeypadEnter, Joypad::Button::kStart, "KP-Enter"},
    {ImGuiKey_RightShift, Joypad::Button::kSelect, "RShift"},
}};

}  // namespace

DebuggerApp::DebuggerApp()
    : trace_log_(512),
      bus_event_log_(2048),
      error_log_(2048),
      run_control_(snes_, breakpoints_, fan_out_trace_sink_, error_log_) {
  fan_out_trace_sink_.Attach(&trace_log_);
  snes_.GetSystemBus().SetEventSink(&bus_event_log_);
}

DebuggerApp::~DebuggerApp() { ShutdownWindow(); }

int DebuggerApp::Run(const std::optional<std::string>& initial_rom_path) {
  if (!InitWindow()) {
    return 1;
  }

  InitFileShortcuts(*this);
  LoadAppConfig();

  if (initial_rom_path.has_value()) {
    ui_state_.rom_path_input = *initial_rom_path;
    (void)LoadRomFromPath(*initial_rom_path);
  }

  last_tick_time_ = std::chrono::steady_clock::now();
  while (window_ != nullptr && !glfwWindowShouldClose(window_)) {
    glfwPollEvents();
    TickEmulation();
    Render();
  }

  SaveAppConfig();
  return fatal_error_.has_value() ? 1 : 0;
}

SnesAddrT DebuggerApp::GetCurrentPc() const {
  const CPU::Regs regs = snes_.GetCpu().GetRegs();
  return (static_cast<SnesAddrT>(regs.PBR) << 16U) | static_cast<SnesAddrT>(regs.PC);
}

bool DebuggerApp::LoadRomFromPath(const std::string& path) {
  namespace fs = std::filesystem;
  StopTraceRecording();

  std::ifstream stream(path, std::ios::binary);
  if (!stream.good()) {
    error_log_.Push({
        .master_time = snes_.GetMasterTime(),
        .severity = ErrorSeverity::kError,
        .source = ErrorSource::kRomLoader,
        .message = "Unable to open ROM: " + path,
    });
    return false;
  }

  std::vector<uint8_t> rom((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
  // Many .smc dumps carry a 512-byte copier header that is not part of the
  // ROM image. Strip it before the size validation so a headered LoROM is
  // accepted and mapped as its raw payload.
  StripSmcCopierHeader(rom);
  {
    Sha1 hasher;
    hasher.Update(rom.data(), rom.size());
    rom_sha1_ = hasher.Finalize();
  }
  if (rom.empty() || (rom.size() % Cartridge::kLoROMWindowSize) != 0U) {
    error_log_.Push({
        .master_time = snes_.GetMasterTime(),
        .severity = ErrorSeverity::kError,
        .source = ErrorSource::kRomLoader,
        .message = "Unsupported LoROM size for " + path,
    });
    return false;
  }

  try {
    snes_.LoadLoRom(rom);
    snes_.Reset();
    trace_log_.Clear();
    bus_event_log_.Clear();
    microop_trace_.Clear();
    snes_.GetCpu().SetMicroOpRecorder(&microop_trace_);
    breakpoints_.Clear();
    run_control_.ResetMachineState();
    loaded_rom_ = true;
    loaded_rom_path_ = path;
    std::error_code abs_ec;
    const fs::path absolute = fs::weakly_canonical(fs::path(path), abs_ec);
    const std::string resolved = abs_ec ? path : absolute.string();
    ui_state_.last_rom_path = resolved;
    const fs::path parent = fs::path(resolved).parent_path();
    if (!parent.empty()) {
      ui_state_.load_rom_dir = parent.string();
    }
    SaveAppConfig();
    JumpToAddress(GetCurrentPc());
    return true;
  } catch (const std::exception& ex) {
    error_log_.Push({
        .master_time = snes_.GetMasterTime(),
        .severity = ErrorSeverity::kFatal,
        .source = ErrorSource::kRomLoader,
        .message = ex.what(),
    });
    return false;
  }
}

void DebuggerApp::ResetMachine() {
  if (!loaded_rom_) {
    return;
  }
  StopTraceRecording();
  snes_.Reset();
  trace_log_.Clear();
  bus_event_log_.Clear();
  microop_trace_.Clear();
  run_control_.ResetMachineState();
  JumpToAddress(GetCurrentPc());
}

bool DebuggerApp::WriteMemory(SnesAddrT address, uint8_t value) {
  const DebugWriteResult result = snes_.GetSystemBus().DebugWrite(address, value);
  if (!result.ok) {
    error_log_.PushDebugWriteRefusal(snes_.GetMasterTime(), result, snes_.GetCpu().GetRegs());
    return false;
  }
  return true;
}

void DebuggerApp::JumpToAddress(SnesAddrT address) {
  const SnesAddrT wrapped = address & 0x00FFFFFFU;
  ui_state_.disasm_address = wrapped;
  ui_state_.memory_address = wrapped;
}

void DebuggerApp::PushHostError(std::string message, ErrorSeverity severity) {
  error_log_.Push({
      .master_time = snes_.GetMasterTime(),
      .severity = severity,
      .source = ErrorSource::kHost,
      .message = std::move(message),
  });
}

bool DebuggerApp::StartTraceRecording(const std::string& path, bool reset_rom) {
  if (!loaded_rom_) {
    trace_last_error_ = "cannot start trace: no ROM loaded";
    return false;
  }
  StopTraceRecording();

  if (reset_rom) {
    ResetMachine();
  }

  auto sink = std::make_unique<FileTraceSink>(snes_, path, rom_sha1_);
  if (sink->HasError()) {
    trace_last_error_ = sink->Error();
    PushHostError("trace record: " + sink->Error(), ErrorSeverity::kError);
    return false;
  }

  fan_out_trace_sink_.Attach(sink.get());
  file_trace_sink_ = std::move(sink);
  trace_recording_path_ = path;
  trace_last_error_.clear();
  return true;
}

void DebuggerApp::StopTraceRecording() {
  if (file_trace_sink_ == nullptr) return;
  fan_out_trace_sink_.Detach(file_trace_sink_.get());
  file_trace_sink_->Flush();
  file_trace_sink_.reset();
  trace_recording_path_.clear();
}

void DebuggerApp::FlushTraceRecording() {
  if (file_trace_sink_) {
    file_trace_sink_->Flush();
  }
}

uint64_t DebuggerApp::TraceRecordedLines() const { return file_trace_sink_ ? file_trace_sink_->LineCount() : 0U; }

bool DebuggerApp::InitWindow() {
  current_app_ = this;
  glfwSetErrorCallback(&DebuggerApp::GlfwErrorCallback);
  if (glfwInit() == 0) {
    fatal_error_ = "GLFW initialization failed";
    return false;
  }

  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif

  window_ = glfwCreateWindow(1600, 1000, "PupSNES Debugger", nullptr, nullptr);
  if (window_ == nullptr) {
    fatal_error_ = "OpenGL window creation failed";
    return false;
  }

  glfwMakeContextCurrent(window_);
  glfwSwapInterval(1);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  ImGui::StyleColorsDark();
  ImGuiStyle& style = ImGui::GetStyle();
  style.WindowPadding = ImVec2(4.0F, 2.0F);
  style.CellPadding = ImVec2(3.0F, 1.0F);
  style.ItemSpacing = ImVec2(4.0F, 2.0F);
  style.FramePadding = ImVec2(2.0F, 2.0F);
  style.FrameRounding = 0.0F;
  style.TabRounding = 0.0F;
  style.WindowBorderSize = 0.0F;
  style.DockingSeparatorSize = 1.0F;

  // Palette:
  //   lavender #B6B1E2 (0.714, 0.694, 0.886) — highlight
  //   purple   #8968B8 (0.537, 0.408, 0.722) — accent
  //   light    #C1C1C2 (0.757, 0.757, 0.761) — text
  //   dark     #3B3B3B (0.231, 0.231, 0.231) — base surface
  //   mid      #9794A0 (0.592, 0.580, 0.627) — muted
  const ImVec4 lavender = ImVec4(0.714F, 0.694F, 0.886F, 1.00F);
  const ImVec4 purple = ImVec4(0.537F, 0.408F, 0.722F, 1.00F);
  const ImVec4 light = ImVec4(0.757F, 0.757F, 0.761F, 1.00F);
  const ImVec4 dark = ImVec4(0.231F, 0.231F, 0.231F, 1.00F);
  const ImVec4 mid = ImVec4(0.592F, 0.580F, 0.627F, 1.00F);

  ImVec4* colors = style.Colors;
  colors[ImGuiCol_Text] = light;
  colors[ImGuiCol_TextDisabled] = ImVec4(mid.x, mid.y, mid.z, 0.70F);
  colors[ImGuiCol_WindowBg] = ImVec4(0.15F, 0.15F, 0.15F, 1.00F);
  colors[ImGuiCol_ChildBg] = ImVec4(0.12F, 0.12F, 0.12F, 1.00F);
  colors[ImGuiCol_PopupBg] = ImVec4(0.18F, 0.18F, 0.18F, 1.00F);
  colors[ImGuiCol_Border] = ImVec4(mid.x, mid.y, mid.z, 0.35F);
  colors[ImGuiCol_FrameBg] = dark;
  colors[ImGuiCol_FrameBgHovered] = ImVec4(0.32F, 0.28F, 0.40F, 1.00F);
  colors[ImGuiCol_FrameBgActive] = ImVec4(purple.x, purple.y, purple.z, 0.75F);
  colors[ImGuiCol_TitleBg] = ImVec4(0.18F, 0.18F, 0.18F, 1.00F);
  colors[ImGuiCol_TitleBgActive] = purple;
  colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.12F, 0.12F, 0.12F, 0.80F);
  colors[ImGuiCol_MenuBarBg] = ImVec4(0.18F, 0.18F, 0.18F, 1.00F);
  colors[ImGuiCol_ScrollbarBg] = ImVec4(0.12F, 0.12F, 0.12F, 1.00F);
  colors[ImGuiCol_ScrollbarGrab] = ImVec4(mid.x, mid.y, mid.z, 0.50F);
  colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(purple.x, purple.y, purple.z, 0.80F);
  colors[ImGuiCol_ScrollbarGrabActive] = purple;
  colors[ImGuiCol_CheckMark] = lavender;
  colors[ImGuiCol_SliderGrab] = purple;
  colors[ImGuiCol_SliderGrabActive] = lavender;
  colors[ImGuiCol_Button] = dark;
  colors[ImGuiCol_ButtonHovered] = ImVec4(purple.x, purple.y, purple.z, 0.75F);
  colors[ImGuiCol_ButtonActive] = purple;
  colors[ImGuiCol_Header] = ImVec4(purple.x, purple.y, purple.z, 0.55F);
  colors[ImGuiCol_HeaderHovered] = ImVec4(purple.x, purple.y, purple.z, 0.80F);
  colors[ImGuiCol_HeaderActive] = purple;
  colors[ImGuiCol_Separator] = ImVec4(mid.x, mid.y, mid.z, 0.40F);
  colors[ImGuiCol_SeparatorHovered] = purple;
  colors[ImGuiCol_SeparatorActive] = lavender;
  colors[ImGuiCol_ResizeGrip] = ImVec4(mid.x, mid.y, mid.z, 0.40F);
  colors[ImGuiCol_ResizeGripHovered] = ImVec4(purple.x, purple.y, purple.z, 0.80F);
  colors[ImGuiCol_ResizeGripActive] = lavender;
  colors[ImGuiCol_Tab] = ImVec4(0.20F, 0.20F, 0.20F, 1.00F);
  colors[ImGuiCol_TabHovered] = ImVec4(purple.x, purple.y, purple.z, 0.85F);
  colors[ImGuiCol_TabActive] = purple;
  colors[ImGuiCol_TabUnfocused] = ImVec4(0.15F, 0.15F, 0.15F, 1.00F);
  colors[ImGuiCol_TabUnfocusedActive] = ImVec4(purple.x, purple.y, purple.z, 0.60F);
  colors[ImGuiCol_DockingPreview] = ImVec4(lavender.x, lavender.y, lavender.z, 0.70F);
  colors[ImGuiCol_TextSelectedBg] = ImVec4(purple.x, purple.y, purple.z, 0.45F);
  colors[ImGuiCol_NavHighlight] = lavender;
  colors[ImGuiCol_PlotLines] = lavender;
  colors[ImGuiCol_PlotLinesHovered] = light;
  colors[ImGuiCol_PlotHistogram] = purple;
  colors[ImGuiCol_PlotHistogramHovered] = lavender;
  colors[ImGuiCol_DragDropTarget] = lavender;
  colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.00F, 0.00F, 0.00F, 0.55F);
  colors[ImGuiCol_TableHeaderBg] = dark;
  colors[ImGuiCol_TableBorderStrong] = ImVec4(mid.x, mid.y, mid.z, 0.60F);
  colors[ImGuiCol_TableBorderLight] = ImVec4(mid.x, mid.y, mid.z, 0.30F);
  colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.00F, 1.00F, 1.00F, 0.04F);

  if (!ImGui_ImplGlfw_InitForOpenGL(window_, true)) {
    fatal_error_ = "ImGui GLFW backend initialization failed";
    return false;
  }
  if (!ImGui_ImplOpenGL3_Init("#version 150")) {
    fatal_error_ = "ImGui OpenGL backend initialization failed";
    return false;
  }

  return true;
}

void DebuggerApp::ShutdownWindow() {
  if (window_ == nullptr) {
    if (ImGui::GetCurrentContext() != nullptr) {
      ImGui::DestroyContext();
    }
    glfwTerminate();
    current_app_ = nullptr;
    return;
  }

  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  glfwDestroyWindow(window_);
  window_ = nullptr;
  glfwTerminate();
  current_app_ = nullptr;
}

void DebuggerApp::TickEmulation() {
  if (!loaded_rom_) {
    return;
  }

  const auto now = std::chrono::steady_clock::now();

  if (run_control_.GetState() == RunState::kPaused) {
    last_tick_time_ = now;
    return;
  }

  // Cap emulated master cycles to wall-clock elapsed * speed multiplier so the
  // emulator tracks the selected fraction of real-hardware speed regardless of
  // host refresh rate. Clamp dt so pause/stall doesn't produce a giant catch-up.
  double dt_secs = std::chrono::duration<double>(now - last_tick_time_).count();
  if (dt_secs < 0.0) {
    dt_secs = 0.0;
  } else if (dt_secs > 0.1) {
    dt_secs = 0.1;
  }
  last_tick_time_ = now;

  const double multiplier = std::clamp(static_cast<double>(ui_state_.speed_multiplier), 0.0, 10.0);
  const auto cycles_budget = static_cast<TimeMasterT>(dt_secs * kMasterClockHz * multiplier);

  try {
    run_control_.TickFrame(std::chrono::milliseconds(16), cycles_budget);
  } catch (const std::exception& ex) {
    fatal_error_ = ex.what();
  }

  if (file_trace_sink_ != nullptr && file_trace_sink_->HasError()) {
    const std::string err = file_trace_sink_->Error();
    trace_last_error_ = err;
    PushHostError("trace record: " + err, ErrorSeverity::kError);
    StopTraceRecording();
  }
}

void DebuggerApp::RenderMenuBar() {
  if (ImGui::BeginMainMenuBar()) {
    if (ImGui::BeginMenu("File")) {
      if (ImGui::MenuItem("Load ROM...")) {
        ui_state_.open_load_rom_dialog = true;
        ui_state_.load_rom_error.clear();
      }
      const bool has_last = !ui_state_.last_rom_path.empty();
      const std::string last_label =
          has_last ? ("Load Last ROM (" + std::filesystem::path(ui_state_.last_rom_path).filename().string() + ")")
                   : std::string("Load Last ROM");
      if (ImGui::MenuItem(last_label.c_str(), nullptr, false, has_last)) {
        if (!LoadRomFromPath(ui_state_.last_rom_path)) {
          ui_state_.load_rom_error = "Failed to load: " + ui_state_.last_rom_path;
          ui_state_.open_load_rom_dialog = true;
        }
      }
      ImGui::Separator();
      if (ImGui::MenuItem("Exit")) {
        glfwSetWindowShouldClose(window_, GLFW_TRUE);
      }
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
      if (ImGui::BeginMenu("Time Display")) {
        for (int i = 0; i < 4; ++i) {
          const auto mode = static_cast<TimeDisplayMode>(i);
          const bool selected = ui_state_.time_display_mode == mode;
          if (ImGui::MenuItem(TimeDisplayModeLongLabel(mode), nullptr, selected)) {
            ui_state_.time_display_mode = mode;
            SaveAppConfig();
          }
        }
        ImGui::EndMenu();
      }
      // Emulator-only override that lets the PPU panel render the 15-line
      // overscan strip even when the ROM left SETINI bit 2 clear. Mirrors
      // the checkbox in the PPU panel so it's reachable without opening the
      // window.
      bool force_overscan = snes_.GetPpu().GetForceOverscanDraw();
      if (ImGui::MenuItem("Force PPU Overscan Draw", nullptr, &force_overscan)) {
        snes_.GetPpu().SetForceOverscanDraw(force_overscan);
      }
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Emulation")) {
      if (ImGui::BeginMenu("Speed")) {
        struct SpeedPreset {
          const char* label;
          float value;
        };
        static constexpr SpeedPreset kPresets[] = {
            {"1%", 0.01F}, {"5%", 0.05F},  {"10%", 0.10F}, {"25%", 0.25F},
            {"50%", 0.5F}, {"100%", 1.0F}, {"200%", 2.0F}, {"400%", 4.0F},
        };
        for (const SpeedPreset& p : kPresets) {
          const bool selected = std::fabs(ui_state_.speed_multiplier - p.value) < 1e-4F;
          if (ImGui::MenuItem(p.label, nullptr, selected)) {
            ui_state_.speed_multiplier = p.value;
            last_tick_time_ = std::chrono::steady_clock::now();
            SaveAppConfig();
          }
        }
        ImGui::Separator();
        float custom_pct = ui_state_.speed_multiplier * 100.0F;
        ImGui::SetNextItemWidth(120.0F);
        if (ImGui::InputFloat("Custom %", &custom_pct, 10.0F, 100.0F, "%.1f")) {
          ui_state_.speed_multiplier = std::clamp(custom_pct / 100.0F, 0.001F, 10.0F);
          last_tick_time_ = std::chrono::steady_clock::now();
          SaveAppConfig();
        }
        ImGui::EndMenu();
      }
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Windows")) {
      ImGui::MenuItem("Registers", nullptr, &ui_state_.show_registers_panel);
      ImGui::MenuItem("Disassembly", nullptr, &ui_state_.show_disasm_panel);
      ImGui::MenuItem("Memory", nullptr, &ui_state_.show_memory_panel);
      ImGui::MenuItem("Stack", nullptr, &ui_state_.show_stack_panel);
      ImGui::MenuItem("PPU", nullptr, &ui_state_.show_ppu_panel);
      ImGui::MenuItem("PPU Viewer", nullptr, &ui_state_.show_ppu_viewer_panel);
      ImGui::MenuItem("DMA / HDMA", nullptr, &ui_state_.show_dma_panel);
      ImGui::MenuItem("Trace", nullptr, &ui_state_.show_trace_panel);
      ImGui::MenuItem("Trace Record", nullptr, &ui_state_.show_trace_record_panel);
      ImGui::MenuItem("Micro-op Trace", nullptr, &ui_state_.show_microop_trace_panel);
      ImGui::MenuItem("Scheduler", nullptr, &ui_state_.show_scheduler_panel);
      ImGui::MenuItem("Errors", nullptr, &ui_state_.show_errors_panel);
      ImGui::MenuItem("Bus", nullptr, &ui_state_.show_bus_panel);
      ImGui::MenuItem("P1 Controller", nullptr, &ui_state_.show_controller_panel);
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Debug")) {
      ImGui::MenuItem("Style Editor", nullptr, &ui_state_.show_style_editor);
      ImGui::MenuItem("Metrics/Debugger", nullptr, &ui_state_.show_metrics_window);
      ImGui::MenuItem("Debug Log", nullptr, &ui_state_.show_debug_log_window);
      ImGui::MenuItem("ID Stack Tool", nullptr, &ui_state_.show_id_stack_tool);
      ImGui::MenuItem("Demo Window", nullptr, &ui_state_.show_demo_window);
      ImGui::Separator();
      ImGui::MenuItem("About Dear ImGui", nullptr, &ui_state_.show_about_window);
      ImGui::EndMenu();
    }
    {
      const double now = glfwGetTime();
      const TimeMasterT master_now = snes_.GetMasterTime();
      const double dt = now - perf_last_time_;
      if (perf_last_time_ == 0.0) {
        perf_last_time_ = now;
        perf_last_master_ = master_now;
      } else if (dt >= 0.25) {
        const auto master_delta = static_cast<double>(master_now - perf_last_master_);
        perf_fps_ = ImGui::GetIO().Framerate;
        perf_realtime_pct_ = static_cast<float>((master_delta / kMasterClockHz) / dt * 100.0);
        perf_last_time_ = now;
        perf_last_master_ = master_now;
      }

      const float region_width = ImGui::GetContentRegionAvail().x;
      char overlay[64];
      std::snprintf(overlay, sizeof(overlay), "FPS: %5.1f  |  Speed: %6.1f%%", static_cast<double>(perf_fps_),
                    static_cast<double>(perf_realtime_pct_));
      const float text_width = ImGui::CalcTextSize(overlay).x;
      if (text_width < region_width) {
        ImGui::SameLine(0.0F, region_width - text_width - ImGui::GetStyle().ItemSpacing.x);
      }
      ImGui::TextUnformatted(overlay);
    }
    ImGui::EndMainMenuBar();
  }

  if (ui_state_.show_style_editor) {
    if (ImGui::Begin("Dear ImGui Style Editor", &ui_state_.show_style_editor)) {
      ImGui::ShowStyleEditor();
    }
    ImGui::End();
  }
  if (ui_state_.show_demo_window) {
    ImGui::ShowDemoWindow(&ui_state_.show_demo_window);
  }
  if (ui_state_.show_metrics_window) {
    ImGui::ShowMetricsWindow(&ui_state_.show_metrics_window);
  }
  if (ui_state_.show_debug_log_window) {
    ImGui::ShowDebugLogWindow(&ui_state_.show_debug_log_window);
  }
  if (ui_state_.show_id_stack_tool) {
    ImGui::ShowIDStackToolWindow(&ui_state_.show_id_stack_tool);
  }
  if (ui_state_.show_about_window) {
    ImGui::ShowAboutWindow(&ui_state_.show_about_window);
  }
}

std::string DebuggerApp::GetConfigPath() {
  namespace fs = std::filesystem;
  const char* home = std::getenv("HOME");
  if (home != nullptr && *home != '\0') {
    return (fs::path(home) / ".pupsnes_config.ini").string();
  }
  return (fs::current_path() / ".pupsnes_config.ini").string();
}

namespace {

struct BoolField {
  const char* key;
  bool UiState::* member;
};
struct StringField {
  const char* key;
  std::string UiState::* member;
};

constexpr std::array<BoolField, 15> kBoolFields{{
    {"show_registers_panel", &UiState::show_registers_panel},
    {"show_disasm_panel", &UiState::show_disasm_panel},
    {"show_memory_panel", &UiState::show_memory_panel},
    {"show_stack_panel", &UiState::show_stack_panel},
    {"show_ppu_panel", &UiState::show_ppu_panel},
    {"show_ppu_viewer_panel", &UiState::show_ppu_viewer_panel},
    {"show_dma_panel", &UiState::show_dma_panel},
    {"show_trace_panel", &UiState::show_trace_panel},
    {"show_trace_record_panel", &UiState::show_trace_record_panel},
    {"trace_record_reset_on_start", &UiState::trace_record_reset_on_start},
    {"show_microop_trace_panel", &UiState::show_microop_trace_panel},
    {"show_scheduler_panel", &UiState::show_scheduler_panel},
    {"show_errors_panel", &UiState::show_errors_panel},
    {"show_bus_panel", &UiState::show_bus_panel},
    {"show_controller_panel", &UiState::show_controller_panel},
}};

constexpr std::array<StringField, 3> kStringFields{{
    {"last_rom_path", &UiState::last_rom_path},
    {"last_rom_dir", &UiState::load_rom_dir},
    {"trace_record_path", &UiState::trace_record_path},
}};

}  // namespace

void DebuggerApp::LoadAppConfig() {
  const std::string path = GetConfigPath();
  std::ifstream stream(path);
  if (!stream.good()) {
    return;
  }
  std::string line;
  while (std::getline(stream, line)) {
    const std::size_t eq = line.find('=');
    if (eq == std::string::npos) {
      continue;
    }
    const std::string key = line.substr(0, eq);
    const std::string value = line.substr(eq + 1);

    bool handled = false;
    for (const auto& f : kBoolFields) {
      if (key == f.key) {
        ui_state_.*f.member = value != "0";
        handled = true;
        break;
      }
    }
    if (!handled) {
      for (const auto& f : kStringFields) {
        if (key == f.key) {
          ui_state_.*f.member = value;
          handled = true;
          break;
        }
      }
    }
    if (handled) continue;

    if (key == "time_display_mode") {
      const int parsed = std::atoi(value.c_str());
      if (parsed >= 0 && parsed <= static_cast<int>(TimeDisplayMode::kPpu)) {
        ui_state_.time_display_mode = static_cast<TimeDisplayMode>(parsed);
      }
    } else if (key == "speed_multiplier") {
      const float parsed = std::strtof(value.c_str(), nullptr);
      if (parsed > 0.0F) {
        ui_state_.speed_multiplier = std::clamp(parsed, 0.001F, 10.0F);
      }
    }
  }
}

void DebuggerApp::SaveAppConfig() {
  const std::string path = GetConfigPath();
  std::ofstream stream(path, std::ios::trunc);
  if (!stream.good()) {
    return;
  }
  for (const auto& f : kStringFields) {
    stream << f.key << "=" << ui_state_.*f.member << "\n";
  }
  stream << "time_display_mode=" << static_cast<int>(ui_state_.time_display_mode) << "\n";
  for (const auto& f : kBoolFields) {
    stream << f.key << "=" << (ui_state_.*f.member ? 1 : 0) << "\n";
  }
  stream << "speed_multiplier=" << ui_state_.speed_multiplier << "\n";
}

void DebuggerApp::PollGameInput() {
  if (!loaded_rom_) {
    // No ROM means the SNES is in reset state; still safe to forward inputs,
    // but the joypad is reset on every ROM load so don't bother.
    return;
  }

  const ImGuiIO& io = ImGui::GetIO();
  // When an ImGui text widget is active, suppress all polled input so typing
  // an address doesn't fire button presses. Treating every mapped key as
  // "not down" also releases any button that was held when focus moved into
  // the text field — otherwise a held key would stick on the pad until the
  // user happened to release it inside the text field (an event ImGui eats).
  // Same logic applies to Ctrl/Cmd/Alt — those compose into shortcuts, not
  // gameplay.
  const bool suppress = io.WantTextInput || io.KeyCtrl || io.KeyAlt || io.KeySuper;

  Joypad& joypad = snes_.GetJoypad();
  for (size_t i = 0; i < kP1KeyMap.size(); ++i) {
    const bool down = !suppress && ImGui::IsKeyDown(kP1KeyMap[i].key);
    if (down != p1_key_was_down_[i]) {
      joypad.SetButton(kP1KeyMap[i].button, down);
      p1_key_was_down_[i] = down;
    }
  }
}

void DebuggerApp::RenderFatalModal() {
  if (!fatal_error_.has_value()) {
    return;
  }

  ImGui::OpenPopup("Fatal Error");
  if (ImGui::BeginPopupModal("Fatal Error", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextWrapped("%s", fatal_error_->c_str());
    if (ImGui::Button("Exit")) {
      glfwSetWindowShouldClose(window_, GLFW_TRUE);
    }
    ImGui::EndPopup();
  }
}

void DebuggerApp::Render() {
  ImGui_ImplOpenGL3_NewFrame();
  ImGui_ImplGlfw_NewFrame();
  ImGui::NewFrame();

  PollGameInput();

  RenderMenuBar();
  ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());

  RenderControlsPanel(*this);
  RenderRegistersPanel(*this);
  RenderDisasmPanel(*this);
  RenderMemoryPanel(*this);
  RenderStackPanel(*this);
  RenderPpuPanel(*this);
  RenderPpuViewerPanel(*this);
  RenderDmaPanel(*this);
  RenderTracePanel(*this);
  RenderTraceRecordPanel(*this);
  RenderMicroOpTracePanel(*this);
  RenderSchedulerPanel(*this);
  RenderErrorsPanel(*this);
  RenderBusEventPanel(*this);
  RenderControllerPanel(*this);
  RenderLoadRomDialog(*this);
  RenderFatalModal();

  ImGui::Render();
  int display_w = 0;
  int display_h = 0;
  glfwGetFramebufferSize(window_, &display_w, &display_h);
  glViewport(0, 0, display_w, display_h);
  glClearColor(0.09F, 0.09F, 0.11F, 1.0F);
  glClear(GL_COLOR_BUFFER_BIT);
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
  glfwSwapBuffers(window_);
}

void DebuggerApp::GlfwErrorCallback(int /*code*/, const char* description) {
  if (current_app_ == nullptr) {
    return;
  }
  current_app_->PushHostError(description != nullptr ? description : "Unknown GLFW error");
}

}  // namespace pupsnes::debugger
