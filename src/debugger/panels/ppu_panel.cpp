#include <cstdint>

#include "debugger/app.h"
#include "imgui.h"
#include "panels.h"
#include "pupsnes/hw/sppu/ppu.h"
#include "pupsnes/hw/sppu/ppu_regs.h"

namespace pupsnes::debugger {

namespace {

// Convert a SNES BGR555 word to an ImGui ABGR32 colour. Each 5-bit channel
// is shifted up to 8 bits — correct for a debug preview; the real frontend
// will want a proper lookup (or the brightness-scaled channel already stored
// in the framebuffer — which is exactly what we read here).
ImU32 Bgr555ToAbgr32(uint16_t c) {
  const uint32_t r = static_cast<uint32_t>(c & 0x1FU) << 3U;
  const uint32_t g = static_cast<uint32_t>((c >> 5U) & 0x1FU) << 3U;
  const uint32_t b = static_cast<uint32_t>((c >> 10U) & 0x1FU) << 3U;
  return IM_COL32(r, g, b, 255);
}

const char* VmainStepLabel(uint8_t vmain) {
  switch (vmain & sppu::regs::kVmainStepMask) {
    case 0x00U: return "+1";
    case 0x01U: return "+32";
    case 0x02U:
    case 0x03U: return "+128";
    default:    return "?";
  }
}

const char* VmainTranslateLabel(uint8_t vmain) {
  switch ((vmain & sppu::regs::kVmainTranslateMask) >> sppu::regs::kVmainTranslateShift) {
    case 0: return "none";
    case 1: return "8x32 (2bpp)";
    case 2: return "8x64 (4bpp)";
    case 3: return "8x128 (8bpp)";
    default: return "?";
  }
}

void TableRowText(const char* name, const char* value) {
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::TextUnformatted(name);
  ImGui::TableSetColumnIndex(1);
  ImGui::TextUnformatted(value);
}

void DrawRegisterTable(const Ppu& ppu) {
  constexpr ImGuiTableFlags kFlags =
      ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit;
  if (!ImGui::BeginTable("ppu_regs", 2, kFlags)) {
    return;
  }
  ImGui::TableSetupColumn("Field");
  ImGui::TableSetupColumn("Value");

  TableRowText("Forced blank", ppu.IsForcedBlank() ? "yes" : "no");

  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::TextUnformatted("Brightness");
  ImGui::TableSetColumnIndex(1);
  ImGui::Text("%u / 15", static_cast<unsigned>(ppu.GetBrightness()));

  TableRowText("Overscan", ppu.IsOverscan() ? "239 lines" : "224 lines");

  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::TextUnformatted("Dot (H,V)");
  ImGui::TableSetColumnIndex(1);
  ImGui::Text("(%u, %u)", ppu.GetDotH(), ppu.GetDotV());

  TableRowText("Field", ppu.GetField() ? "odd" : "even");

  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::TextUnformatted("CGADD");
  ImGui::TableSetColumnIndex(1);
  ImGui::Text("0x%02X", static_cast<unsigned>(ppu.GetShadow(sppu::regs::kCgAdd)));

  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::TextUnformatted("OAMADD (raw)");
  ImGui::TableSetColumnIndex(1);
  ImGui::Text("H=0x%02X L=0x%02X", static_cast<unsigned>(ppu.GetShadow(sppu::regs::kOamAddH)),
              static_cast<unsigned>(ppu.GetShadow(sppu::regs::kOamAddL)));

  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::TextUnformatted("VMADD");
  ImGui::TableSetColumnIndex(1);
  ImGui::Text("0x%02X%02X", static_cast<unsigned>(ppu.GetShadow(sppu::regs::kVmAddH)),
              static_cast<unsigned>(ppu.GetShadow(sppu::regs::kVmAddL)));

  const uint8_t vmain = ppu.GetShadow(sppu::regs::kVmain);
  TableRowText("VMAIN.step", VmainStepLabel(vmain));
  TableRowText("VMAIN.translate", VmainTranslateLabel(vmain));
  TableRowText("VMAIN.inc-on",
               (vmain & sppu::regs::kVmainIncrementOnHighMask) != 0U ? "$2119 (high)" : "$2118 (low)");

  ImGui::EndTable();
}

// Draw the PPU front buffer using row run-length encoding. Backdrop-only
// rendering makes every row uniform colour, so this collapses to ~1 rect
// per scanline instead of 57k per-pixel rects. Once BG/OBJ rendering lands
// we'll want a proper GL texture; until then this keeps the panel
// responsive without any OpenGL plumbing.
void DrawFramebufferPreview(const Ppu& ppu) {
  const FrameBufferView view = ppu.BuildFrontView();
  if (view.pixels == nullptr || view.width == 0 || view.height == 0) {
    ImGui::TextDisabled("(no frame yet)");
    return;
  }

  const float avail = ImGui::GetContentRegionAvail().x;
  const float scale_x = avail / static_cast<float>(view.width);
  // Preserve aspect. Clamp minimum scale so the preview stays usable.
  const float scale = (scale_x > 0.5F) ? scale_x : 0.5F;

  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const float total_w = static_cast<float>(view.width) * scale;
  const float total_h = static_cast<float>(view.height) * scale;

  ImDrawList* draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(origin, ImVec2(origin.x + total_w, origin.y + total_h),
                      IM_COL32(0, 0, 0, 255));

  for (uint32_t y = 0; y < view.height; ++y) {
    const uint16_t* row = view.pixels + static_cast<std::size_t>(y) * view.stride;
    uint32_t run_start = 0;
    uint16_t run_color = row[0];
    for (uint32_t x = 1; x <= view.width; ++x) {
      const uint16_t here = (x < view.width) ? row[x] : static_cast<uint16_t>(~run_color);
      if (here != run_color) {
        if (run_color != 0) {
          const float x0 = origin.x + static_cast<float>(run_start) * scale;
          const float x1 = origin.x + static_cast<float>(x) * scale;
          const float y0 = origin.y + static_cast<float>(y) * scale;
          const float y1 = y0 + scale;
          draw->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), Bgr555ToAbgr32(run_color));
        }
        run_start = x;
        run_color = here;
      }
    }
  }

  ImGui::Dummy(ImVec2(total_w, total_h));
}

}  // namespace

void RenderPpuPanel(DebuggerApp& app) {
  if (!app.GetUiState().show_ppu_panel) {
    return;
  }
  if (!ImGui::Begin("PPU", &app.GetUiState().show_ppu_panel)) {
    ImGui::End();
    return;
  }

  Ppu& ppu = app.GetSnes().GetPpu();

  if (ImGui::CollapsingHeader("State", ImGuiTreeNodeFlags_DefaultOpen)) {
    DrawRegisterTable(ppu);
    ImGui::Text("Pending writes: %u", ppu.GetPendingWriteCount());

    bool force_overscan = ppu.GetForceOverscanDraw();
    if (ImGui::Checkbox("Force overscan draw (239 lines)", &force_overscan)) {
      ppu.SetForceOverscanDraw(force_overscan);
    }
  }

  if (ImGui::CollapsingHeader("Framebuffer", ImGuiTreeNodeFlags_DefaultOpen)) {
    DrawFramebufferPreview(ppu);
  }

  ImGui::End();
}

}  // namespace pupsnes::debugger
