#include "app.h"

// GLFW would otherwise pull in OpenGL/gl.h, which collides with gl3.h.
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <OpenGL/gl3.h>

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
#include "pupsnes/hw/5a22/cpu.h"
#include "pupsnes/hw/cartridge.h"
#include "pupsnes/hw/joypad.h"
#include "pupsnes/hw/scheduler.h"
#include "pupsnes/hw/sppu/ppu.h"
#include "pupsnes/rom_format.h"

namespace pupsnes::emulator {

namespace {

// Master-clock frequency of the SNES (21.477272 MHz). The wall-clock budget
// scales this by the speed multiplier each frame.
constexpr uint64_t kMasterClockHz = 21477272U;

// Maximum logical PPU framebuffer (256 wide × 239 with overscan). The unused
// tail when overscan is off costs nothing.
constexpr std::size_t kMaxLogicalPixels = 256U * 239U;

// Defensive cap on TickToTarget loop iterations without forward progress —
// breaks out of stalls (e.g., a STP) so the UI stays responsive.
constexpr int kMaxStuckIterations = 16;

// Interval between automatic .srm flushes while the ROM is running. Real save
// writes are coalesced behind the dirty flag so this is the worst-case window
// in which a crash would lose unwritten SRAM.
constexpr std::chrono::seconds kSramFlushInterval{2};

std::string DeriveSavePath(const std::string& rom_path) {
  std::filesystem::path p(rom_path);
  p.replace_extension(".srm");
  return p.string();
}

constexpr uint32_t Expand5To8(uint32_t v5) { return (v5 << 3U) | (v5 >> 2U); }

uint32_t Bgr555ToRgba8(uint16_t c) {
  const uint32_t r = Expand5To8(static_cast<uint32_t>(c) & 0x1FU);
  const uint32_t g = Expand5To8((static_cast<uint32_t>(c) >> 5U) & 0x1FU);
  const uint32_t b = Expand5To8((static_cast<uint32_t>(c) >> 10U) & 0x1FU);
  return r | (g << 8U) | (b << 16U) | (0xFFU << 24U);
}

std::array<uint32_t, kMaxLogicalPixels>& GetScratchBuffer() {
  static std::array<uint32_t, kMaxLogicalPixels> buf{};
  return buf;
}

// Maps a GLFW key state to a joypad button press for the current frame.
// Keyboard layout (matches common SNES emulator conventions):
//   Arrows → D-pad     Z/X → B/A     A/S → Y/X
//   Q/W   → L/R        Enter → Start, R-Shift → Select
void ApplyKeyboardToJoypad(GLFWwindow* window, Joypad& joypad) {
  using Btn = Joypad::Button;
  const auto press = [&](int key, Btn b) { joypad.SetButton(b, glfwGetKey(window, key) == GLFW_PRESS); };
  press(GLFW_KEY_UP, Btn::kUp);
  press(GLFW_KEY_DOWN, Btn::kDown);
  press(GLFW_KEY_LEFT, Btn::kLeft);
  press(GLFW_KEY_RIGHT, Btn::kRight);
  press(GLFW_KEY_Z, Btn::kB);
  press(GLFW_KEY_X, Btn::kA);
  press(GLFW_KEY_A, Btn::kY);
  press(GLFW_KEY_S, Btn::kX);
  press(GLFW_KEY_Q, Btn::kL);
  press(GLFW_KEY_W, Btn::kR);
  press(GLFW_KEY_ENTER, Btn::kStart);
  press(GLFW_KEY_RIGHT_SHIFT, Btn::kSelect);
}

}  // namespace

EmulatorApp* EmulatorApp::current_app_ = nullptr;

EmulatorApp::EmulatorApp() = default;
EmulatorApp::~EmulatorApp() { ShutdownWindow(); }

int EmulatorApp::Run(const std::optional<std::string>& initial_rom_path) {
  if (!InitWindow()) {
    return 1;
  }

  InitFileShortcuts();
  LoadConfig();

  if (initial_rom_path.has_value()) {
    (void)LoadRomFromPath(*initial_rom_path);
  }

  last_tick_time_ = std::chrono::steady_clock::now();
  while (window_ != nullptr && glfwWindowShouldClose(window_) == 0) {
    glfwPollEvents();
    PollControllerInput();
    TickEmulation();
    Render();
  }

  FlushSramToDisk();
  SaveConfig();
  return fatal_error_.has_value() ? 1 : 0;
}

bool EmulatorApp::LoadRomFromPath(const std::string& path) {
  namespace fs = std::filesystem;

  std::ifstream stream(path, std::ios::binary);
  if (!stream.good()) {
    ui_state_.load_rom_error = "Unable to open ROM: " + path;
    return false;
  }

  std::vector<uint8_t> rom((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
  StripSmcCopierHeader(rom);

  // Flush the outgoing cart's SRAM before we tear it down. Skipped when no
  // ROM is loaded yet — FlushSramToDisk is a no-op in that case.
  FlushSramToDisk();

  try {
    const RomLoadResult load_result = snes_.LoadRom(rom);
    if (!load_result.ok) {
      ui_state_.load_rom_error = path + ": " + load_result.message;
      return false;
    }
    snes_.Reset();
    loaded_rom_ = true;
    loaded_rom_path_ = path;
    loaded_rom_save_path_ = DeriveSavePath(path);

    // Load the .srm next to the ROM if it exists and the cart actually has
    // SRAM. A missing file is normal (new game, first run) — leave SRAM as
    // the 0xFF-initialised default.
    Cartridge& cart = snes_.GetCartridge();
    if (cart.SramSize() > 0U) {
      std::ifstream save_stream(loaded_rom_save_path_, std::ios::binary);
      if (save_stream.good()) {
        std::vector<uint8_t> save_data((std::istreambuf_iterator<char>(save_stream)), std::istreambuf_iterator<char>());
        cart.LoadSram(save_data);
      }
    }

    ui_state_.paused = false;
    std::error_code abs_ec;
    const fs::path absolute = fs::weakly_canonical(fs::path(path), abs_ec);
    ui_state_.last_rom_path = abs_ec ? path : absolute.string();
    const fs::path parent = fs::path(ui_state_.last_rom_path).parent_path();
    if (!parent.empty()) {
      ui_state_.load_rom_dir = parent.string();
    }
    SaveConfig();
    last_tick_time_ = std::chrono::steady_clock::now();
    last_sram_flush_time_ = last_tick_time_;
    return true;
  } catch (const std::exception& ex) {
    ui_state_.load_rom_error = ex.what();
    return false;
  }
}

void EmulatorApp::FlushSramToDisk() {
  if (!loaded_rom_ || loaded_rom_save_path_.empty()) {
    return;
  }
  Cartridge& cart = snes_.GetCartridge();
  if (cart.SramSize() == 0U || !cart.SramDirty()) {
    return;
  }
  std::ofstream stream(loaded_rom_save_path_, std::ios::binary | std::ios::trunc);
  if (!stream.good()) {
    // Don't surface as fatal — a non-writable save dir shouldn't crash the
    // emulator. The dirty flag stays set so the next attempt retries.
    return;
  }
  const auto view = cart.SramView();
  stream.write(reinterpret_cast<const char*>(view.data()), static_cast<std::streamsize>(view.size()));
  if (stream.good()) {
    cart.ClearSramDirty();
  }
}

void EmulatorApp::ResetMachine() {
  if (!loaded_rom_) {
    return;
  }
  snes_.Reset();
  last_tick_time_ = std::chrono::steady_clock::now();
}

void EmulatorApp::PollControllerInput() {
  if (!loaded_rom_) {
    return;
  }
  const ImGuiIO& io = ImGui::GetIO();
  // Don't steal keys while the user is typing in an ImGui text widget.
  if (io.WantCaptureKeyboard) {
    snes_.GetJoypad().ReleaseAll();
    return;
  }
  ApplyKeyboardToJoypad(window_, snes_.GetJoypad());
}

void EmulatorApp::TickEmulation() {
  if (!loaded_rom_) {
    last_tick_time_ = std::chrono::steady_clock::now();
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  if (ui_state_.paused) {
    last_tick_time_ = now;
    return;
  }

  double dt_secs = std::chrono::duration<double>(now - last_tick_time_).count();
  if (dt_secs < 0.0) {
    dt_secs = 0.0;
  } else if (dt_secs > 0.1) {
    // Clamp catch-up: pause/stall shouldn't trigger a multi-second burst.
    dt_secs = 0.1;
  }
  last_tick_time_ = now;

  const double multiplier = std::clamp(static_cast<double>(ui_state_.speed_multiplier), 0.0, 10.0);
  const TimeMasterT cycles_budget =
      static_cast<TimeMasterT>(dt_secs * static_cast<double>(kMasterClockHz) * multiplier);
  if (cycles_budget == 0U) {
    return;
  }

  const TimeMasterT start_master = snes_.GetMasterTime();
  const TimeMasterT cap_master = start_master + cycles_budget;
  TimeMasterT last_master = start_master;
  int stuck_iterations = 0;

  try {
    while (snes_.GetMasterTime() < cap_master) {
      TimeMasterT next_event = snes_.GetScheduler().NextEventMasterTime();
      TimeMasterT target = next_event;
      if (cap_master < target) {
        target = cap_master;
      }
      static_cast<void>(snes_.GetCpu().TickToTarget(target));
      const TimeMasterT after_tick = snes_.GetMasterTime();
      snes_.MachineSync(after_tick);
      snes_.GetScheduler().FireEventsThrough(after_tick);

      if (after_tick == last_master) {
        if (++stuck_iterations >= kMaxStuckIterations) {
          // Stalled (STP or insufficient budget). Bail until next host frame
          // so the UI stays responsive instead of looping forever.
          break;
        }
      } else {
        stuck_iterations = 0;
        last_master = after_tick;
      }
    }
  } catch (const std::exception& ex) {
    fatal_error_ = ex.what();
  }

  if (now - last_sram_flush_time_ >= kSramFlushInterval) {
    FlushSramToDisk();
    last_sram_flush_time_ = now;
  }
}

void EmulatorApp::UploadFrontBufferToTexture() {
  const FrameBufferView view = snes_.GetPpu().BuildFrontView();
  if (view.pixels == nullptr || view.width == 0U || view.height == 0U) {
    return;
  }
  const std::size_t pixel_count = static_cast<std::size_t>(view.width) * view.height;
  if (pixel_count > kMaxLogicalPixels) {
    return;
  }

  auto& scratch = GetScratchBuffer();
  for (uint32_t y = 0; y < view.height; ++y) {
    const uint16_t* src = view.pixels + static_cast<std::size_t>(y) * view.stride;
    uint32_t* dst = scratch.data() + static_cast<std::size_t>(y) * view.width;
    for (uint32_t x = 0; x < view.width; ++x) {
      dst[x] = Bgr555ToRgba8(src[x]);
    }
  }

  GLint previous_unpack_alignment = 4;
  glGetIntegerv(GL_UNPACK_ALIGNMENT, &previous_unpack_alignment);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

  if (ppu_tex_id_ == 0U) {
    GLuint id = 0;
    glGenTextures(1, &id);
    ppu_tex_id_ = id;
    glBindTexture(GL_TEXTURE_2D, ppu_tex_id_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  } else {
    glBindTexture(GL_TEXTURE_2D, ppu_tex_id_);
  }

  if (ppu_tex_w_ != view.width || ppu_tex_h_ != view.height) {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(view.width), static_cast<GLsizei>(view.height), 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, scratch.data());
    ppu_tex_w_ = view.width;
    ppu_tex_h_ = view.height;
  } else {
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(view.width), static_cast<GLsizei>(view.height),
                    GL_RGBA, GL_UNSIGNED_BYTE, scratch.data());
  }

  glPixelStorei(GL_UNPACK_ALIGNMENT, previous_unpack_alignment);
}

void EmulatorApp::RenderBackgroundFrame() {
  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  // Region below the main menu bar.
  const float menu_h = ImGui::GetFrameHeight();
  const ImVec2 region_pos(viewport->WorkPos.x, viewport->WorkPos.y);
  const ImVec2 region_size(viewport->WorkSize.x, viewport->WorkSize.y);
  // Work area should already exclude the menu bar; fall back if it doesn't.
  ImVec2 origin = region_pos;
  ImVec2 size = region_size;
  if (size.y >= viewport->Size.y - 0.5F) {
    origin.y += menu_h;
    size.y -= menu_h;
  }
  if (size.x <= 0.0F || size.y <= 0.0F) {
    return;
  }

  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  // Solid black backdrop (letterbox fill).
  dl->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(0, 0, 0, 255));

  if (ppu_tex_id_ == 0U || ppu_tex_w_ == 0U || ppu_tex_h_ == 0U) {
    return;
  }

  const float tex_w = static_cast<float>(ppu_tex_w_);
  const float tex_h = static_cast<float>(ppu_tex_h_);
  float draw_w = size.x;
  float draw_h = size.y;

  if (ui_state_.maintain_aspect) {
    const float aspect = tex_w / tex_h;
    draw_h = draw_w / aspect;
    if (draw_h > size.y) {
      draw_h = size.y;
      draw_w = draw_h * aspect;
    }
  }
  if (ui_state_.integer_scale) {
    int scale_x = static_cast<int>(size.x / tex_w);
    int scale_y = static_cast<int>(size.y / tex_h);
    int scale = std::min(scale_x, scale_y);
    if (scale < 1) scale = 1;
    draw_w = tex_w * static_cast<float>(scale);
    draw_h = tex_h * static_cast<float>(scale);
  }

  const ImVec2 dst_min(origin.x + (size.x - draw_w) * 0.5F, origin.y + (size.y - draw_h) * 0.5F);
  const ImVec2 dst_max(dst_min.x + draw_w, dst_min.y + draw_h);
  dl->AddImage(static_cast<ImTextureID>(static_cast<intptr_t>(ppu_tex_id_)), dst_min, dst_max);
}

void EmulatorApp::RenderMenuBar() {
  if (!ImGui::BeginMainMenuBar()) {
    return;
  }
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
        ui_state_.open_load_rom_dialog = true;
      }
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Exit")) {
      glfwSetWindowShouldClose(window_, GLFW_TRUE);
    }
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Emulation")) {
    const bool can_run = loaded_rom_;
    if (ImGui::MenuItem(ui_state_.paused ? "Resume" : "Pause", "Space", false, can_run)) {
      ui_state_.paused = !ui_state_.paused;
      last_tick_time_ = std::chrono::steady_clock::now();
    }
    if (ImGui::MenuItem("Reset", nullptr, false, can_run)) {
      ResetMachine();
    }
    if (ImGui::BeginMenu("Speed", can_run)) {
      struct SpeedPreset {
        const char* label;
        float value;
      };
      static constexpr std::array<SpeedPreset, 8> kPresets = {{
          {"1%", 0.01F},
          {"5%", 0.05F},
          {"10%", 0.10F},
          {"25%", 0.25F},
          {"50%", 0.5F},
          {"100%", 1.0F},
          {"200%", 2.0F},
          {"400%", 4.0F},
      }};
      for (const SpeedPreset& p : kPresets) {
        const bool selected = std::fabs(ui_state_.speed_multiplier - p.value) < 1e-4F;
        if (ImGui::MenuItem(p.label, nullptr, selected)) {
          ui_state_.speed_multiplier = p.value;
          last_tick_time_ = std::chrono::steady_clock::now();
          SaveConfig();
        }
      }
      ImGui::Separator();
      float custom_pct = ui_state_.speed_multiplier * 100.0F;
      ImGui::SetNextItemWidth(120.0F);
      if (ImGui::InputFloat("Custom %", &custom_pct, 10.0F, 100.0F, "%.1f")) {
        ui_state_.speed_multiplier = std::clamp(custom_pct / 100.0F, 0.001F, 10.0F);
        last_tick_time_ = std::chrono::steady_clock::now();
        SaveConfig();
      }
      ImGui::EndMenu();
    }
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("View")) {
    if (ImGui::MenuItem("Maintain Aspect Ratio", nullptr, &ui_state_.maintain_aspect)) {
      SaveConfig();
    }
    if (ImGui::MenuItem("Integer Scale", nullptr, &ui_state_.integer_scale)) {
      SaveConfig();
    }
    ImGui::Separator();
    bool force_overscan = snes_.GetPpu().GetForceOverscanDraw();
    if (ImGui::MenuItem("Force PPU Overscan Draw", nullptr, &force_overscan)) {
      snes_.GetPpu().SetForceOverscanDraw(force_overscan);
    }
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Help")) {
    ImGui::MenuItem("About PupSNES", nullptr, &ui_state_.show_about);
    ImGui::EndMenu();
  }

  // FPS / speed overlay aligned right.
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
      perf_realtime_pct_ = static_cast<float>((master_delta / static_cast<double>(kMasterClockHz)) / dt * 100.0);
      perf_last_time_ = now;
      perf_last_master_ = master_now;
    }

    const float region_width = ImGui::GetContentRegionAvail().x;
    char overlay[80];
    if (loaded_rom_) {
      std::snprintf(overlay, sizeof(overlay), "%s  |  FPS %5.1f  |  Speed %6.1f%%",
                    ui_state_.paused ? "PAUSED" : "      ", static_cast<double>(perf_fps_),
                    static_cast<double>(perf_realtime_pct_));
    } else {
      std::snprintf(overlay, sizeof(overlay), "No ROM loaded");
    }
    const float text_width = ImGui::CalcTextSize(overlay).x;
    if (text_width < region_width) {
      ImGui::SameLine(0.0F, region_width - text_width - ImGui::GetStyle().ItemSpacing.x);
    }
    ImGui::TextUnformatted(overlay);
  }
  ImGui::EndMainMenuBar();
}

void EmulatorApp::RenderLoadRomDialog() {
  namespace fs = std::filesystem;
  if (ui_state_.open_load_rom_dialog) {
    ImGui::OpenPopup("Load ROM");
    ui_state_.open_load_rom_dialog = false;
  }
  ImGui::SetNextWindowSize(ImVec2(760.0F, 460.0F), ImGuiCond_Appearing);
  if (!ImGui::BeginPopupModal("Load ROM", nullptr, ImGuiWindowFlags_NoCollapse)) {
    return;
  }

  if (ImGui::BeginChild("rom_shortcuts", ImVec2(180.0F, -ImGui::GetFrameHeightWithSpacing()),
                        ImGuiChildFlags_Borders)) {
    ImGui::TextDisabled("Shortcuts");
    ImGui::Separator();
    if (!ui_state_.last_rom_path.empty()) {
      if (ImGui::Button("Last ROM", ImVec2(-1.0F, 0.0F))) {
        if (LoadRomFromPath(ui_state_.last_rom_path)) {
          ui_state_.load_rom_error.clear();
          ImGui::CloseCurrentPopup();
        }
      }
      ImGui::Separator();
    }
    for (const FileShortcut& sc : ui_state_.load_rom_shortcuts) {
      if (ImGui::Button(sc.label.c_str(), ImVec2(-1.0F, 0.0F))) {
        ui_state_.load_rom_dir = sc.path;
        ui_state_.load_rom_error.clear();
      }
    }
  }
  ImGui::EndChild();

  ImGui::SameLine();

  if (ImGui::BeginChild("rom_browser", ImVec2(0.0F, -ImGui::GetFrameHeightWithSpacing()))) {
    std::error_code ec;
    fs::path dir(ui_state_.load_rom_dir);
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) {
      ImGui::TextColored(ImVec4(0.95F, 0.5F, 0.25F, 1.0F), "Directory not found: %s", ui_state_.load_rom_dir.c_str());
    } else {
      if (ImGui::Button("Up")) {
        const fs::path parent = dir.parent_path();
        if (!parent.empty() && parent != dir) {
          ui_state_.load_rom_dir = parent.string();
          dir = parent;
        }
      }
      ImGui::SameLine();
      ImGui::TextUnformatted(ui_state_.load_rom_dir.c_str());
      ImGui::Separator();

      std::vector<fs::path> subdirs;
      std::vector<fs::path> files;
      for (const auto& entry : fs::directory_iterator(dir, ec)) {
        const bool is_dir = entry.is_directory(ec);
        const bool is_file = entry.is_regular_file(ec);
        if (is_dir) {
          const std::string name = entry.path().filename().string();
          if (!name.empty() && name.front() == '.') {
            continue;
          }
          subdirs.push_back(entry.path());
        } else if (is_file) {
          std::string ext = entry.path().extension().string();
          for (char& c : ext) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
          }
          if (ext == ".sfc" || ext == ".smc") {
            files.push_back(entry.path());
          }
        }
      }
      std::sort(subdirs.begin(), subdirs.end());
      std::sort(files.begin(), files.end());

      if (ImGui::BeginChild("rom_list", ImVec2(0.0F, 0.0F), ImGuiChildFlags_Borders)) {
        if (subdirs.empty() && files.empty()) {
          ImGui::TextDisabled("No subdirectories or .sfc/.smc files.");
        }
        for (const fs::path& sub : subdirs) {
          const std::string label = "[DIR] " + sub.filename().string();
          if (ImGui::Selectable(label.c_str(), false)) {
            ui_state_.load_rom_dir = sub.string();
          }
        }
        for (const fs::path& path : files) {
          const std::string name = path.filename().string();
          if (ImGui::Selectable(name.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick)) {
            if (LoadRomFromPath(path.string())) {
              ui_state_.load_rom_error.clear();
              ImGui::CloseCurrentPopup();
            }
          }
        }
      }
      ImGui::EndChild();
    }
  }
  ImGui::EndChild();

  if (!ui_state_.load_rom_error.empty()) {
    ImGui::TextColored(ImVec4(0.95F, 0.25F, 0.25F, 1.0F), "%s", ui_state_.load_rom_error.c_str());
  }

  if (ImGui::Button("Cancel")) {
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

void EmulatorApp::RenderFatalModal() {
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

void EmulatorApp::Render() {
  // Sample the Space key once per frame as a pause hotkey when not typing.
  {
    const ImGuiIO& io = ImGui::GetIO();
    if (!io.WantCaptureKeyboard && loaded_rom_ && ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
      ui_state_.paused = !ui_state_.paused;
      last_tick_time_ = std::chrono::steady_clock::now();
    }
  }

  if (loaded_rom_) {
    UploadFrontBufferToTexture();
  }

  ImGui_ImplOpenGL3_NewFrame();
  ImGui_ImplGlfw_NewFrame();
  ImGui::NewFrame();

  RenderMenuBar();
  RenderBackgroundFrame();
  RenderLoadRomDialog();

  if (!loaded_rom_) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 center(viewport->WorkPos.x + viewport->WorkSize.x * 0.5F,
                        viewport->WorkPos.y + viewport->WorkSize.y * 0.5F);
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5F, 0.5F));
    ImGui::SetNextWindowBgAlpha(0.0F);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize;
    if (ImGui::Begin("##no_rom_overlay", nullptr, kFlags)) {
      ImGui::TextDisabled("No ROM loaded — File > Load ROM…");
    }
    ImGui::End();
  }

  if (ui_state_.show_about) {
    if (ImGui::Begin("About PupSNES", &ui_state_.show_about, ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::TextUnformatted("PupSNES Emulator");
      ImGui::Separator();
      ImGui::TextWrapped("LoROM Super Famicom emulator built on the PupSNES core.");
      ImGui::TextDisabled("For the debugger build, run pupsnes-debugger instead.");
    }
    ImGui::End();
  }

  RenderFatalModal();

  ImGui::Render();
  int display_w = 0;
  int display_h = 0;
  glfwGetFramebufferSize(window_, &display_w, &display_h);
  glViewport(0, 0, display_w, display_h);
  glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
  glClear(GL_COLOR_BUFFER_BIT);
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
  glfwSwapBuffers(window_);
}

bool EmulatorApp::InitWindow() {
  current_app_ = this;
  glfwSetErrorCallback(&EmulatorApp::GlfwErrorCallback);
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

  // 256×224 logical * 3 = 768×672, plus generous letterbox padding for the
  // common 4:3 aspect.
  window_ = glfwCreateWindow(1024, 768, "PupSNES", nullptr, nullptr);
  if (window_ == nullptr) {
    fatal_error_ = "OpenGL window creation failed";
    return false;
  }

  glfwMakeContextCurrent(window_);
  glfwSwapInterval(1);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  // Don't let nav focus auto-set io.WantCaptureKeyboard. Otherwise clicking any
  // ImGui window (e.g., the About box) leaves it as the nav window forever, and
  // PollControllerInput keeps releasing the joypad on every frame.
  io.ConfigNavCaptureKeyboard = false;
  ImGui::StyleColorsDark();
  ImGuiStyle& style = ImGui::GetStyle();
  style.WindowRounding = 0.0F;
  style.FrameRounding = 0.0F;
  style.TabRounding = 0.0F;

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

void EmulatorApp::ShutdownWindow() {
  if (window_ == nullptr) {
    if (ImGui::GetCurrentContext() != nullptr) {
      ImGui::DestroyContext();
    }
    glfwTerminate();
    current_app_ = nullptr;
    return;
  }

  if (ppu_tex_id_ != 0U) {
    GLuint id = ppu_tex_id_;
    glDeleteTextures(1, &id);
    ppu_tex_id_ = 0;
  }
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  glfwDestroyWindow(window_);
  window_ = nullptr;
  glfwTerminate();
  current_app_ = nullptr;
}

void EmulatorApp::InitFileShortcuts() {
  namespace fs = std::filesystem;
  ui_state_.load_rom_shortcuts.clear();

  std::error_code ec;
  const fs::path cwd = fs::current_path(ec);

  auto add_if_exists = [&](std::string label, const fs::path& candidate) {
    std::error_code exists_ec;
    if (!fs::exists(candidate, exists_ec) || !fs::is_directory(candidate, exists_ec)) {
      return;
    }
    std::error_code canon_ec;
    const fs::path resolved = fs::weakly_canonical(candidate, canon_ec);
    const std::string path_str = canon_ec ? candidate.string() : resolved.string();
    for (const FileShortcut& existing : ui_state_.load_rom_shortcuts) {
      if (existing.path == path_str) {
        return;
      }
    }
    ui_state_.load_rom_shortcuts.push_back({std::move(label), path_str});
  };

  add_if_exists("ROMs", cwd / "roms");
  add_if_exists("Test ROMs", cwd / "testroms");
  add_if_exists("Built Test ROMs", cwd / "build" / "dev" / "test-roms");

  const char* home = std::getenv("HOME");
  if (home != nullptr && *home != '\0') {
    add_if_exists("Home", fs::path(home));
  }
}

std::string EmulatorApp::GetConfigPath() {
  namespace fs = std::filesystem;
  const char* home = std::getenv("HOME");
  if (home != nullptr && *home != '\0') {
    return (fs::path(home) / ".pupsnes_emulator.ini").string();
  }
  return (fs::current_path() / ".pupsnes_emulator.ini").string();
}

void EmulatorApp::LoadConfig() {
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

    if (key == "last_rom_path") {
      ui_state_.last_rom_path = value;
    } else if (key == "load_rom_dir") {
      ui_state_.load_rom_dir = value;
    } else if (key == "speed_multiplier") {
      const float parsed = std::strtof(value.c_str(), nullptr);
      if (parsed > 0.0F) {
        ui_state_.speed_multiplier = std::clamp(parsed, 0.001F, 10.0F);
      }
    } else if (key == "maintain_aspect") {
      ui_state_.maintain_aspect = (value != "0");
    } else if (key == "integer_scale") {
      ui_state_.integer_scale = (value != "0");
    }
  }
}

void EmulatorApp::SaveConfig() {
  const std::string path = GetConfigPath();
  std::ofstream stream(path, std::ios::trunc);
  if (!stream.good()) {
    return;
  }
  stream << "last_rom_path=" << ui_state_.last_rom_path << "\n";
  stream << "load_rom_dir=" << ui_state_.load_rom_dir << "\n";
  stream << "speed_multiplier=" << ui_state_.speed_multiplier << "\n";
  stream << "maintain_aspect=" << (ui_state_.maintain_aspect ? 1 : 0) << "\n";
  stream << "integer_scale=" << (ui_state_.integer_scale ? 1 : 0) << "\n";
}

void EmulatorApp::GlfwErrorCallback(int /*code*/, const char* description) {
  if (current_app_ == nullptr || description == nullptr) {
    return;
  }
  // GLFW errors during shutdown/init are rare; surface as fatal so the user
  // sees the message instead of a silent black window.
  current_app_->fatal_error_ = description;
}

}  // namespace pupsnes::emulator
