#include "app.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
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
#include "pupsnes/hw/cartridge.h"

namespace pupsnes::debugger {

DebuggerApp* DebuggerApp::current_app_ = nullptr;

DebuggerApp::DebuggerApp()
    : trace_log_(512),
      bus_event_log_(2048),
      error_log_(2048),
      run_control_(snes_, breakpoints_, trace_log_, error_log_) {
  snes_.GetSystemBus().SetEventSink(&bus_event_log_);
}

DebuggerApp::~DebuggerApp() { ShutdownWindow(); }

int DebuggerApp::Run(const std::optional<std::string>& initial_rom_path) {
  if (!InitWindow()) {
    return 1;
  }

  if (initial_rom_path.has_value()) {
    ui_state_.rom_path_input = *initial_rom_path;
    (void)LoadRomFromPath(*initial_rom_path);
  }

  while (window_ != nullptr && !glfwWindowShouldClose(window_)) {
    glfwPollEvents();
    TickEmulation();
    Render();
  }

  return fatal_error_.has_value() ? 1 : 0;
}

SnesAddrT DebuggerApp::GetCurrentPc() const {
  const CPU::Regs regs = snes_.GetCpu().GetRegs();
  return (static_cast<SnesAddrT>(regs.PBR) << 16U) | static_cast<SnesAddrT>(regs.PC);
}

bool DebuggerApp::LoadRomFromPath(const std::string& path) {
  namespace fs = std::filesystem;

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

  const std::vector<uint8_t> rom((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
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
  style.WindowPadding = ImVec2(0.0F, 0.0F);
  style.CellPadding = ImVec2(2.0F, 0.0F);
  style.ItemSpacing = ImVec2(4.0F, 2.0F);
  style.FramePadding = ImVec2(2.0F, 2.0F);
  style.FrameRounding = 10.0F;
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

  if (run_control_.GetState() == RunState::kPaused) {
    return;
  }

  try {
    run_control_.TickFrame(std::chrono::milliseconds(16));
  } catch (const std::exception& ex) {
    fatal_error_ = ex.what();
  }
}

void DebuggerApp::RenderMenuBar() {
  if (ImGui::BeginMainMenuBar()) {
    if (ImGui::BeginMenu("File")) {
      if (ImGui::MenuItem("Load ROM...")) {
        ui_state_.open_load_rom_dialog = true;
        ui_state_.load_rom_error.clear();
      }
      ImGui::Separator();
      if (ImGui::MenuItem("Exit")) {
        glfwSetWindowShouldClose(window_, GLFW_TRUE);
      }
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
        constexpr double kMasterClockHz = 21477272.0;
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

void DebuggerApp::RenderLoadRomDialog() {
  namespace fs = std::filesystem;

  if (ui_state_.open_load_rom_dialog) {
    ImGui::OpenPopup("Load ROM");
    ui_state_.open_load_rom_dialog = false;
  }

  ImGui::SetNextWindowSize(ImVec2(520.0F, 360.0F), ImGuiCond_Appearing);
  if (!ImGui::BeginPopupModal("Load ROM", nullptr, ImGuiWindowFlags_NoCollapse)) {
    return;
  }

  ImGui::TextUnformatted("Directory:");
  ImGui::SameLine();
  ImGui::TextUnformatted(ui_state_.load_rom_dir.c_str());

  ImGui::Separator();

  std::error_code ec;
  const fs::path dir(ui_state_.load_rom_dir);
  if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) {
    ImGui::TextColored(ImVec4(0.95F, 0.5F, 0.25F, 1.0F), "Directory not found: %s", ui_state_.load_rom_dir.c_str());
  } else {
    std::vector<fs::path> entries;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
      if (!entry.is_regular_file(ec)) {
        continue;
      }
      std::string ext = entry.path().extension().string();
      for (char& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
      if (ext == ".sfc" || ext == ".smc") {
        entries.push_back(entry.path());
      }
    }
    std::sort(entries.begin(), entries.end());

    if (ImGui::BeginChild("rom_list", ImVec2(0.0F, 240.0F), ImGuiChildFlags_Borders)) {
      if (entries.empty()) {
        ImGui::TextDisabled("No .sfc or .smc files found.");
      }
      for (const fs::path& path : entries) {
        const std::string name = path.filename().string();
        if (ImGui::Selectable(name.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick)) {
          if (LoadRomFromPath(path.string())) {
            ui_state_.load_rom_error.clear();
            ImGui::CloseCurrentPopup();
          } else {
            ui_state_.load_rom_error = "Failed to load: " + name;
          }
        }
      }
    }
    ImGui::EndChild();
  }

  if (!ui_state_.load_rom_error.empty()) {
    ImGui::TextColored(ImVec4(0.95F, 0.25F, 0.25F, 1.0F), "%s", ui_state_.load_rom_error.c_str());
  }

  if (ImGui::Button("Cancel")) {
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
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

  RenderMenuBar();
  ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());

  RenderControlsPanel(*this);
  RenderRegistersPanel(*this);
  RenderDisasmPanel(*this);
  RenderMemoryPanel(*this);
  RenderStackPanel(*this);
  RenderPpuPanel(*this);
  RenderTracePanel(*this);
  RenderMicroOpTracePanel(*this);
  RenderSchedulerPanel(*this);
  RenderErrorsPanel(*this);
  RenderBusEventPanel(*this);
  RenderLoadRomDialog();
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
