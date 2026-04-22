#include <array>
#include <cstddef>
#include <cstdint>

#include <OpenGL/gl3.h>

#include "debugger/app.h"
#include "imgui.h"
#include "panels.h"
#include "pupsnes/hw/sppu/ppu.h"
#include "pupsnes/hw/sppu/ppu_regs.h"

namespace pupsnes::debugger {

namespace {

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

// GL texture carrying the last uploaded PPU front buffer. Lazily created,
// resized when the PPU switches between 224 and 239 rows, and reused for
// the lifetime of the app (the GL context reclaims it at shutdown).
struct PreviewTexture {
  GLuint id = 0;
  uint32_t width = 0;
  uint32_t height = 0;
};

PreviewTexture& GetPreviewTexture() {
  static PreviewTexture tex;
  return tex;
}

// CPU-side conversion buffer: BGR555 -> RGBA8. Sized for the max logical
// view (256 × 239 with overscan); the unused tail doesn't cost anything.
constexpr std::size_t kMaxLogicalPixels = 256U * 239U;
std::array<uint32_t, kMaxLogicalPixels>& GetScratchBuffer() {
  static std::array<uint32_t, kMaxLogicalPixels> buf{};
  return buf;
}

// 5-to-8 bit expansion: replicate the high bits into the low bits so the
// 5-bit value 31 maps to 255, not 248. Correct for colour fidelity in the
// debug preview; the emulator frontend may use its own table later.
constexpr uint32_t Expand5To8(uint32_t v5) {
  return (v5 << 3U) | (v5 >> 2U);
}

uint32_t Bgr555ToRgba8(uint16_t c) {
  const uint32_t r = Expand5To8(static_cast<uint32_t>(c) & 0x1FU);
  const uint32_t g = Expand5To8((static_cast<uint32_t>(c) >> 5U) & 0x1FU);
  const uint32_t b = Expand5To8((static_cast<uint32_t>(c) >> 10U) & 0x1FU);
  // IM_COL32 and glTexSubImage2D(GL_RGBA, GL_UNSIGNED_BYTE) both expect
  // RGBA byte order in memory on little-endian hosts: [R][G][B][A].
  return r | (g << 8U) | (b << 16U) | (0xFFU << 24U);
}

void UploadFrameToTexture(const FrameBufferView& view) {
  if (view.pixels == nullptr || view.width == 0 || view.height == 0) {
    return;
  }
  const std::size_t pixel_count = static_cast<std::size_t>(view.width) * view.height;
  if (pixel_count > kMaxLogicalPixels) {
    return;  // logical view shouldn't exceed 256×239; defend anyway
  }

  auto& scratch = GetScratchBuffer();
  for (uint32_t y = 0; y < view.height; ++y) {
    const uint16_t* src = view.pixels + static_cast<std::size_t>(y) * view.stride;
    uint32_t* dst = scratch.data() + static_cast<std::size_t>(y) * view.width;
    for (uint32_t x = 0; x < view.width; ++x) {
      dst[x] = Bgr555ToRgba8(src[x]);
    }
  }

  PreviewTexture& tex = GetPreviewTexture();
  const GLint previous_unpack_alignment = [] {
    GLint value = 4;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &value);
    return value;
  }();
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

  if (tex.id == 0) {
    glGenTextures(1, &tex.id);
    glBindTexture(GL_TEXTURE_2D, tex.id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  } else {
    glBindTexture(GL_TEXTURE_2D, tex.id);
  }

  if (tex.width != view.width || tex.height != view.height) {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(view.width),
                 static_cast<GLsizei>(view.height), 0, GL_RGBA, GL_UNSIGNED_BYTE, scratch.data());
    tex.width = view.width;
    tex.height = view.height;
  } else {
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(view.width),
                    static_cast<GLsizei>(view.height), GL_RGBA, GL_UNSIGNED_BYTE, scratch.data());
  }

  glPixelStorei(GL_UNPACK_ALIGNMENT, previous_unpack_alignment);
}

void DrawFramebufferPreview(const Ppu& ppu) {
  const FrameBufferView view = ppu.BuildFrontView();
  if (view.pixels == nullptr || view.width == 0 || view.height == 0) {
    ImGui::TextDisabled("(no frame yet)");
    return;
  }

  UploadFrameToTexture(view);
  const PreviewTexture& tex = GetPreviewTexture();
  if (tex.id == 0) {
    ImGui::TextDisabled("(texture unavailable)");
    return;
  }

  // Preserve aspect; don't down-scale below 1× so pixels stay distinct.
  const float aspect = static_cast<float>(view.width) / static_cast<float>(view.height);
  const float avail_w = ImGui::GetContentRegionAvail().x;
  float w = (avail_w > static_cast<float>(view.width)) ? avail_w : static_cast<float>(view.width);
  float h = w / aspect;

  const ImTextureID texture_id = static_cast<ImTextureID>(static_cast<intptr_t>(tex.id));
  ImGui::Image(texture_id, ImVec2(w, h));

  ImGui::Text("%ux%u (%s)", view.width, view.height,
              ppu.IsOverscan() || ppu.GetForceOverscanDraw() ? "overscan" : "standard");
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
