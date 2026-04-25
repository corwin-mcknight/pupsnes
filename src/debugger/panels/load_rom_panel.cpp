#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "debugger/app.h"
#include "imgui.h"
#include "panels.h"

namespace pupsnes::debugger {

void RenderLoadRomDialog(DebuggerApp& app) {
  namespace fs = std::filesystem;
  UiState& ui = app.GetUiState();

  if (ui.open_load_rom_dialog) {
    ImGui::OpenPopup("Load ROM");
    ui.open_load_rom_dialog = false;
  }

  ImGui::SetNextWindowSize(ImVec2(760.0F, 460.0F), ImGuiCond_Appearing);
  if (!ImGui::BeginPopupModal("Load ROM", nullptr, ImGuiWindowFlags_NoCollapse)) {
    return;
  }

  // Sidebar: shortcuts.
  if (ImGui::BeginChild("rom_shortcuts", ImVec2(180.0F, -ImGui::GetFrameHeightWithSpacing()),
                        ImGuiChildFlags_Borders)) {
    ImGui::TextDisabled("Shortcuts");
    ImGui::Separator();
    if (!ui.last_rom_path.empty()) {
      if (ImGui::Button("Last ROM", ImVec2(-1.0F, 0.0F))) {
        if (app.LoadRomFromPath(ui.last_rom_path)) {
          ui.load_rom_error.clear();
          ImGui::CloseCurrentPopup();
        } else {
          ui.load_rom_error = "Failed to load: " + ui.last_rom_path;
        }
      }
      ImGui::Separator();
    }
    for (const FileShortcut& sc : ui.load_rom_shortcuts) {
      if (ImGui::Button(sc.label.c_str(), ImVec2(-1.0F, 0.0F))) {
        ui.load_rom_dir = sc.path;
        ui.load_rom_error.clear();
      }
    }
  }
  ImGui::EndChild();

  ImGui::SameLine();

  // Main pane: path bar + entries list.
  if (ImGui::BeginChild("rom_browser", ImVec2(0.0F, -ImGui::GetFrameHeightWithSpacing()))) {
    std::error_code ec;
    fs::path dir(ui.load_rom_dir);
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) {
      ImGui::TextColored(ImVec4(0.95F, 0.5F, 0.25F, 1.0F), "Directory not found: %s", ui.load_rom_dir.c_str());
    } else {
      if (ImGui::Button("Up")) {
        const fs::path parent = dir.parent_path();
        if (!parent.empty() && parent != dir) {
          ui.load_rom_dir = parent.string();
          dir = parent;
        }
      }
      ImGui::SameLine();
      ImGui::TextUnformatted(ui.load_rom_dir.c_str());
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
            ui.load_rom_dir = sub.string();
          }
        }
        for (const fs::path& path : files) {
          const std::string name = path.filename().string();
          if (ImGui::Selectable(name.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick)) {
            if (app.LoadRomFromPath(path.string())) {
              ui.load_rom_error.clear();
              ImGui::CloseCurrentPopup();
            } else {
              ui.load_rom_error = "Failed to load: " + name;
            }
          }
        }
      }
      ImGui::EndChild();
    }
  }
  ImGui::EndChild();

  if (!ui.load_rom_error.empty()) {
    ImGui::TextColored(ImVec4(0.95F, 0.25F, 0.25F, 1.0F), "%s", ui.load_rom_error.c_str());
  }

  if (ImGui::Button("Cancel")) {
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

void InitFileShortcuts(DebuggerApp& app) {
  namespace fs = std::filesystem;
  UiState& ui = app.GetUiState();
  ui.load_rom_shortcuts.clear();

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
    for (const FileShortcut& existing : ui.load_rom_shortcuts) {
      if (existing.path == path_str) {
        return;
      }
    }
    ui.load_rom_shortcuts.push_back({std::move(label), path_str});
  };

  add_if_exists("ROMs", cwd / "roms");
  add_if_exists("Test ROMs", cwd / "testroms");
  add_if_exists("Built Test ROMs", cwd / "build" / "dev" / "test-roms");

  const char* home = std::getenv("HOME");
  if (home != nullptr && *home != '\0') {
    const fs::path home_path(home);
    add_if_exists("Home", home_path);
  }
}

}  // namespace pupsnes::debugger
