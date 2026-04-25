#pragma once

#include "debugger/ui_utils.h"
#include "imgui.h"
#include "pupsnes/types.h"

namespace pupsnes::debugger {

// ImGui wrapper around FormatAddress24 for the common `$BB:OOOO` render case.
inline void TextAddress24(SnesAddrT address) {
  const std::string text = FormatAddress24(address);
  ImGui::TextUnformatted(text.c_str());
}

// RAII wrapper for the panel show/Begin/End boilerplate. Use:
//   ScopedPanel panel("Title", ui.show_xxx_panel);
//   if (!panel) return;
//   // ...content; no explicit ImGui::End() needed.
class ScopedPanel {
 public:
  ScopedPanel(const char* title, bool& show_flag) : show_flag_(show_flag) {
    if (show_flag_) {
      open_ = ImGui::Begin(title, &show_flag_);
      if (!open_) {
        ImGui::End();
      }
    }
  }
  ~ScopedPanel() {
    if (open_) ImGui::End();
  }
  ScopedPanel(const ScopedPanel&) = delete;
  ScopedPanel& operator=(const ScopedPanel&) = delete;

  explicit operator bool() const { return open_; }

 private:
  bool& show_flag_;
  bool open_ = false;
};

}  // namespace pupsnes::debugger
