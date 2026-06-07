#include <OpenGL/gl3.h>

#include <array>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "debugger/app.h"
#include "imgui.h"
#include "panel_utils.h"
#include "panels.h"
#include "pupsnes/hw/sppu/ppu.h"
#include "pupsnes/hw/sppu/pixel_format.h"
#include "pupsnes/hw/sppu/ppu_regs.h"

namespace pupsnes::debugger {

namespace {

constexpr ImGuiTableFlags kKvTableFlags =
    ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit;

const char* VmainStepLabel(uint8_t vmain) {
  switch (vmain & sppu::regs::kVmainStepMask) {
    case 0x00U: return "+1";
    case 0x01U: return "+32";
    case 0x02U:
    case 0x03U: return "+128";
    default: return "?";
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

const char* BgModeLabel(uint8_t mode) {
  switch (mode & 0x07U) {
    case 0: return "0  (4x 2bpp)";
    case 1: return "1  (BG1/2 4bpp, BG3 2bpp)";
    case 2: return "2  (BG1/2 4bpp, offset-per-tile)";
    case 3: return "3  (BG1 8bpp, BG2 4bpp)";
    case 4: return "4  (BG1 8bpp, BG2 2bpp, offset)";
    case 5: return "5  (BG1 4bpp, BG2 2bpp, hi-res)";
    case 6: return "6  (BG1 4bpp, hi-res + offset)";
    case 7: return "7  (BG1 8bpp, rotation/scaling)";
    default: return "?";
  }
}

const char* BgLayoutLabel(uint8_t layout) {
  switch (layout & 0x03U) {
    case 0: return "32x32";
    case 1: return "64x32";
    case 2: return "32x64";
    case 3: return "64x64";
    default: return "?";
  }
}

const char* ObjSizeLabel(uint8_t select) {
  switch (select & 0x07U) {
    case 0: return "8x8 / 16x16";
    case 1: return "8x8 / 32x32";
    case 2: return "8x8 / 64x64";
    case 3: return "16x16 / 32x32";
    case 4: return "16x16 / 64x64";
    case 5: return "32x32 / 64x64";
    case 6: return "16x32 / 32x64";
    case 7: return "16x32 / 32x32";
    default: return "?";
  }
}

const char* MathRegionLabel(uint8_t value) {
  switch (value & 0x03U) {
    case 0: return "always";
    case 1: return "inside math-window";
    case 2: return "outside math-window";
    case 3: return "never";
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

__attribute__((format(printf, 2, 3))) void TableRowFmt(const char* name, const char* fmt, ...) {
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::TextUnformatted(name);
  ImGui::TableSetColumnIndex(1);
  va_list args;
  va_start(args, fmt);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
  ImGui::TextV(fmt, args);
#pragma clang diagnostic pop
  va_end(args);
}

void AppendLayerMask(char* buf, std::size_t cap, uint8_t mask) {
  const char* names[5] = {"BG1", "BG2", "BG3", "BG4", "OBJ"};
  bool first = true;
  buf[0] = '\0';
  std::size_t len = 0;
  for (int i = 0; i < 5; ++i) {
    if ((mask & (1U << i)) == 0U) continue;
    const int n = std::snprintf(buf + len, cap - len, "%s%s", first ? "" : " ", names[i]);
    if (n <= 0) break;
    len += static_cast<std::size_t>(n);
    first = false;
    if (len >= cap) break;
  }
  if (first) {
    std::snprintf(buf, cap, "(none)");
  }
}

void DrawDisplaySection(Ppu& ppu) {
  if (!ImGui::BeginTable("ppu_display", 2, kKvTableFlags)) return;
  ImGui::TableSetupColumn("Field");
  ImGui::TableSetupColumn("Value");

  TableRowText("BG mode", BgModeLabel(ppu.GetBgMode()));
  TableRowText("BG3 priority (Mode 1)", ppu.GetBg3Priority() ? "yes" : "no");
  TableRowText("Forced blank", ppu.IsForcedBlank() ? "yes" : "no");
  TableRowFmt("Brightness", "%u / 15", static_cast<unsigned>(ppu.GetBrightness()));
  TableRowText("Overscan (SETINI.2)", ppu.IsOverscan() ? "239 lines" : "224 lines");

  char main_buf[32];
  char sub_buf[32];
  AppendLayerMask(main_buf, sizeof(main_buf), ppu.GetMainScreenLayers());
  AppendLayerMask(sub_buf, sizeof(sub_buf), ppu.GetSubScreenLayers());
  TableRowFmt("Main screen (TM)", "%02X  %s", static_cast<unsigned>(ppu.GetMainScreenLayers()), main_buf);
  TableRowFmt("Sub screen  (TS)", "%02X  %s", static_cast<unsigned>(ppu.GetSubScreenLayers()), sub_buf);
  ImGui::EndTable();

  bool force_overscan = ppu.GetForceOverscanDraw();
  if (ImGui::Checkbox("Force overscan draw (239 lines)", &force_overscan)) {
    ppu.SetForceOverscanDraw(force_overscan);
  }
}

void DrawBackgroundsSection(const Ppu& ppu) {
  constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_BordersInnerV |
                                     ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit;
  if (!ImGui::BeginTable("ppu_bgs", 7, kFlags)) return;
  ImGui::TableSetupColumn("BG");
  ImGui::TableSetupColumn("Tile");
  ImGui::TableSetupColumn("Layout");
  ImGui::TableSetupColumn("Tilemap");
  ImGui::TableSetupColumn("Chars");
  ImGui::TableSetupColumn("HOfs");
  ImGui::TableSetupColumn("VOfs");
  ImGui::TableHeadersRow();

  for (uint8_t bg = 0; bg < 4U; ++bg) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::Text("BG%u", static_cast<unsigned>(bg + 1U));
    ImGui::TableSetColumnIndex(1);
    ImGui::TextUnformatted(ppu.GetBgTile16x16(bg) ? "16x16" : "8x8");
    ImGui::TableSetColumnIndex(2);
    ImGui::TextUnformatted(BgLayoutLabel(ppu.GetBgTilemapLayout(bg)));
    ImGui::TableSetColumnIndex(3);
    ImGui::Text("$%04X", static_cast<unsigned>(ppu.GetBgTilemapWordBase(bg)));
    ImGui::TableSetColumnIndex(4);
    ImGui::Text("$%04X", static_cast<unsigned>(ppu.GetBgCharWordBase(bg)));
    ImGui::TableSetColumnIndex(5);
    ImGui::Text("%u", static_cast<unsigned>(ppu.GetBgHofs(bg)));
    ImGui::TableSetColumnIndex(6);
    ImGui::Text("%u", static_cast<unsigned>(ppu.GetBgVofs(bg)));
  }
  ImGui::EndTable();
}

void DrawSpritesSection(const Ppu& ppu) {
  if (!ImGui::BeginTable("ppu_obj", 2, kKvTableFlags)) return;
  ImGui::TableSetupColumn("Field");
  ImGui::TableSetupColumn("Value");

  const uint8_t obsel = ppu.GetShadow(sppu::regs::kObsel);
  TableRowText("OBJ size pair", ObjSizeLabel(ppu.GetObjSizeSelect()));
  TableRowFmt("OBSEL.name-select", "%u  (gap = %u × 4K words)",
              static_cast<unsigned>((obsel & sppu::regs::kObselNameSelectMask) >> sppu::regs::kObselNameSelectShift),
              static_cast<unsigned>((obsel & sppu::regs::kObselNameSelectMask) >> sppu::regs::kObselNameSelectShift));
  TableRowFmt("OBSEL.name-base", "%u  (×8K words)",
              static_cast<unsigned>(obsel & sppu::regs::kObselNameBaseMask));
  TableRowFmt("Region 0 word base", "$%04X", static_cast<unsigned>(ppu.GetObjRegion0Word()));
  TableRowFmt("Region 1 word base", "$%04X", static_cast<unsigned>(ppu.GetObjRegion1Word()));
  TableRowFmt("OAM byte address", "$%03X (reload $%03X)", static_cast<unsigned>(ppu.GetOamByteAddr()),
              static_cast<unsigned>(ppu.GetOamByteAddrReload()));
  TableRowText("Priority rotation", ppu.GetOamPriorityRotation() ? "yes" : "no");
  ImGui::EndTable();
}

void DrawVramSection(const Ppu& ppu) {
  if (!ImGui::BeginTable("ppu_vram", 2, kKvTableFlags)) return;
  ImGui::TableSetupColumn("Field");
  ImGui::TableSetupColumn("Value");

  TableRowFmt("VMADD (word)", "$%04X", static_cast<unsigned>(ppu.GetVmadd()));
  const uint8_t vmain = ppu.GetVmain();
  TableRowFmt("VMAIN raw", "$%02X", static_cast<unsigned>(vmain));
  TableRowText("VMAIN.step", VmainStepLabel(vmain));
  TableRowText("VMAIN.translate", VmainTranslateLabel(vmain));
  TableRowText("VMAIN.inc-on",
               (vmain & sppu::regs::kVmainIncrementOnHighMask) != 0U ? "$2119 (high)" : "$2118 (low)");
  TableRowFmt("Prefetch word", "$%04X", static_cast<unsigned>(ppu.GetVramPrefetch()));
  TableRowFmt("VRAM size", "%zu bytes", sppu::regs::kVramSize);
  ImGui::EndTable();
}

void DrawCgramPortSection(const Ppu& ppu) {
  if (!ImGui::BeginTable("ppu_cgport", 2, kKvTableFlags)) return;
  ImGui::TableSetupColumn("Field");
  ImGui::TableSetupColumn("Value");

  TableRowFmt("CGADD (word)", "$%02X", static_cast<unsigned>(ppu.GetCgadd()));
  TableRowFmt("Write latch", "%s (data $%02X)", ppu.GetCgramWriteLatchHigh() ? "high pending" : "low next",
              static_cast<unsigned>(ppu.GetCgramWriteLatchData()));
  TableRowText("Read latch", ppu.GetCgramReadLatchHigh() ? "high pending" : "low next");
  ImGui::EndTable();
}

void DrawColorMathSection(const Ppu& ppu) {
  if (!ImGui::BeginTable("ppu_math", 2, kKvTableFlags)) return;
  ImGui::TableSetupColumn("Field");
  ImGui::TableSetupColumn("Value");

  const uint8_t cgwsel = ppu.GetCgwsel();
  const uint8_t cgadsub = ppu.GetCgadsub();
  TableRowText("CGWSEL.direct color", (cgwsel & sppu::regs::kCgwselDirectColorMask) != 0U ? "yes" : "no");
  TableRowText("CGWSEL.sub BG/OBJ", (cgwsel & sppu::regs::kCgwselSubScreenEnableMask) != 0U ? "enabled"
                                                                                            : "fixed only");
  TableRowText("CGWSEL.math region",
               MathRegionLabel((cgwsel & sppu::regs::kCgwselMathEnableRegionMask) >>
                               sppu::regs::kCgwselMathEnableRegionShift));
  TableRowText("CGWSEL.force-black region",
               MathRegionLabel((cgwsel & sppu::regs::kCgwselForceMainBlackRegionMask) >>
                               sppu::regs::kCgwselForceMainBlackRegionShift));

  char enables[48];
  std::snprintf(enables, sizeof(enables), "%s%s%s%s%s%s",
                (cgadsub & sppu::regs::kCgadsubBg1Mask) != 0U ? "BG1 " : "",
                (cgadsub & sppu::regs::kCgadsubBg2Mask) != 0U ? "BG2 " : "",
                (cgadsub & sppu::regs::kCgadsubBg3Mask) != 0U ? "BG3 " : "",
                (cgadsub & sppu::regs::kCgadsubBg4Mask) != 0U ? "BG4 " : "",
                (cgadsub & sppu::regs::kCgadsubObjMask) != 0U ? "OBJ " : "",
                (cgadsub & sppu::regs::kCgadsubBackdropMask) != 0U ? "BD" : "");
  TableRowFmt("CGADSUB enables", "%s", enables[0] != '\0' ? enables : "(none)");
  TableRowText("CGADSUB.op",
               (cgadsub & sppu::regs::kCgadsubSubtractMask) != 0U ? "subtract" : "add");
  TableRowText("CGADSUB.half",
               (cgadsub & sppu::regs::kCgadsubHalfMask) != 0U ? "yes" : "no");

  const uint16_t fixed_bgr = static_cast<uint16_t>(ppu.GetColdataR() | (ppu.GetColdataG() << 5U) |
                                                    (ppu.GetColdataB() << 10U));
  TableRowFmt("COLDATA latches", "R=%u G=%u B=%u", static_cast<unsigned>(ppu.GetColdataR()),
              static_cast<unsigned>(ppu.GetColdataG()), static_cast<unsigned>(ppu.GetColdataB()));
  TableRowFmt("Fixed BGR (assembled)", "$%04X", static_cast<unsigned>(fixed_bgr));
  ImGui::EndTable();
}

void DrawTimingSection(const Ppu& ppu, uint32_t pending_writes) {
  if (!ImGui::BeginTable("ppu_timing", 2, kKvTableFlags)) return;
  ImGui::TableSetupColumn("Field");
  ImGui::TableSetupColumn("Value");

  TableRowFmt("Dot (H, V)", "(%u, %u)", ppu.GetDotH(), ppu.GetDotV());
  TableRowText("Field", ppu.GetField() ? "odd" : "even");
  TableRowText("VBlank NMI latch", ppu.PeekVblankNmiFlag() ? "set" : "clear");
  TableRowText("/NMI line", ppu.PeekNmiLine() ? "low (asserted)" : "high");
  TableRowFmt("Pending writes", "%u", pending_writes);
  ImGui::EndTable();
}

void DrawCgramPaletteSection(const Ppu& ppu) {
  const uint16_t* cgram = ppu.GetCgram();
  if (cgram == nullptr) {
    ImGui::TextDisabled("(CGRAM unavailable)");
    return;
  }
  const float swatch = ImGui::GetFontSize() + 4.0F;
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(1.0F, 1.0F));
  for (int i = 0; i < 256; ++i) {
    if ((i & 0x0F) != 0) ImGui::SameLine();
    const uint16_t c = cgram[i];
    const float r = static_cast<float>((c >> 0U) & 0x1FU) / 31.0F;
    const float g = static_cast<float>((c >> 5U) & 0x1FU) / 31.0F;
    const float b = static_cast<float>((c >> 10U) & 0x1FU) / 31.0F;
    char id[16];
    std::snprintf(id, sizeof(id), "##cg%d", i);
    ImGui::ColorButton(id, ImVec4(r, g, b, 1.0F),
                       ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop |
                           ImGuiColorEditFlags_NoBorder,
                       ImVec2(swatch, swatch));
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("[$%02X]  $%04X  R=%u G=%u B=%u", static_cast<unsigned>(i),
                        static_cast<unsigned>(c), static_cast<unsigned>((c >> 0U) & 0x1FU),
                        static_cast<unsigned>((c >> 5U) & 0x1FU), static_cast<unsigned>((c >> 10U) & 0x1FU));
    }
  }
  ImGui::PopStyleVar();
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

PreviewTexture& GetOverlayTexture() {
  static PreviewTexture tex;
  return tex;
}

using sppu::Bgr555ToRgba8;
using sppu::Expand5To8;
using sppu::kMaxLogicalPixels;

// CPU-side conversion buffer: BGR555 -> RGBA8. Sized for the max logical
// view (256 × 239 with overscan); the unused tail doesn't cost anything.
std::array<uint32_t, kMaxLogicalPixels>& GetScratchBuffer() {
  static std::array<uint32_t, kMaxLogicalPixels> buf{};
  return buf;
}

void UploadFrameToTexture(const FrameBufferView& view, bool darkened = false) {
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
      uint32_t px = Bgr555ToRgba8(src[x]);
      if (darkened) {
        // Halve each RGB channel to dim the previous frame.
        const uint32_t r = ((px >> 0U) & 0xFFU) >> 1U;
        const uint32_t g = ((px >> 8U) & 0xFFU) >> 1U;
        const uint32_t b = ((px >> 16U) & 0xFFU) >> 1U;
        px = r | (g << 8U) | (b << 16U) | (0xFFU << 24U);
      }
      dst[x] = px;
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
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(view.width), static_cast<GLsizei>(view.height), 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, scratch.data());
    tex.width = view.width;
    tex.height = view.height;
  } else {
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(view.width), static_cast<GLsizei>(view.height),
                    GL_RGBA, GL_UNSIGNED_BYTE, scratch.data());
  }

  glPixelStorei(GL_UNPACK_ALIGNMENT, previous_unpack_alignment);
}

void UploadBackOverlayToTexture(const Ppu& ppu) {
  const FrameBufferView view = ppu.BuildFrontView();  // reuse logical dims
  if (view.width == 0 || view.height == 0) {
    return;
  }
  const std::size_t pixel_count = static_cast<std::size_t>(view.width) * view.height;
  if (pixel_count > kMaxLogicalPixels) {
    return;
  }

  const uint16_t* back = ppu.GetBackBuffer();
  const uint8_t* mask = ppu.GetDrawnMask();
  auto& scratch = GetScratchBuffer();

  // The back buffer is the raw 341×313 grid; the view offsets into it via
  // BuildFrontView() which starts at (kVisibleHStart, kVisibleVStartNtsc).
  // Iterate using full-grid coordinates so back[] and mask[] indexing matches
  // the same origin as BuildFrontView / FramebufferIndexFor.
  for (uint32_t y_logical = 0; y_logical < view.height; ++y_logical) {
    const uint32_t y_grid = y_logical + sppu::regs::kVisibleVStartNtsc;
    const uint16_t* back_row = back + static_cast<std::size_t>(y_grid) * view.stride;
    uint32_t* dst_row = scratch.data() + static_cast<std::size_t>(y_logical) * view.width;
    for (uint32_t x_logical = 0; x_logical < view.width; ++x_logical) {
      const uint32_t x_grid = x_logical + sppu::regs::kVisibleHStart;
      const std::size_t idx = static_cast<std::size_t>(y_grid) * view.stride + x_grid;
      const bool drawn = ((mask[idx >> 3U] >> (idx & 7U)) & 1U) != 0U;
      if (!drawn) {
        dst_row[x_logical] = 0;  // fully transparent
        continue;
      }
      const uint32_t r = Expand5To8(static_cast<uint32_t>(back_row[x_grid]) & 0x1FU);
      const uint32_t g = Expand5To8((static_cast<uint32_t>(back_row[x_grid]) >> 5U) & 0x1FU);
      const uint32_t b = Expand5To8((static_cast<uint32_t>(back_row[x_grid]) >> 10U) & 0x1FU);
      dst_row[x_logical] = r | (g << 8U) | (b << 16U) | (0xFFU << 24U);
    }
  }

  PreviewTexture& tex = GetOverlayTexture();
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
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(view.width), static_cast<GLsizei>(view.height), 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, scratch.data());
    tex.width = view.width;
    tex.height = view.height;
  } else {
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(view.width), static_cast<GLsizei>(view.height),
                    GL_RGBA, GL_UNSIGNED_BYTE, scratch.data());
  }

  glPixelStorei(GL_UNPACK_ALIGNMENT, previous_unpack_alignment);
}

void DrawFittedFramebuffer(const Ppu& ppu, bool show_in_progress) {
  const FrameBufferView view = ppu.BuildFrontView();
  if (view.pixels == nullptr || view.width == 0 || view.height == 0) {
    ImGui::TextDisabled("(no frame yet)");
    return;
  }

  // When the in-progress overlay is active (paused or slow-mo): upload front
  // DARKENED (dim ghost of previous frame) as the base layer, then composite
  // the in-progress back-buffer at full color on top. Otherwise: upload front
  // at full color with no overlay.
  UploadFrameToTexture(view, /*darkened=*/show_in_progress);
  const PreviewTexture& front = GetPreviewTexture();
  if (front.id == 0) {
    ImGui::TextDisabled("(texture unavailable)");
    return;
  }

  // Fit aspect within the available rect (both H and V), then center.
  const float aspect = static_cast<float>(view.width) / static_cast<float>(view.height);
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  if (avail.x <= 0.0F || avail.y <= 0.0F) {
    return;
  }
  float w = avail.x;
  float h = w / aspect;
  if (h > avail.y) {
    h = avail.y;
    w = h * aspect;
  }
  const ImVec2 origin = ImGui::GetCursorPos();
  ImGui::SetCursorPos(ImVec2(origin.x + (avail.x - w) * 0.5F, origin.y + (avail.y - h) * 0.5F));

  const ImVec2 cursor_screen = ImGui::GetCursorScreenPos();
  ImGui::Image(static_cast<ImTextureID>(static_cast<intptr_t>(front.id)), ImVec2(w, h));

  if (show_in_progress) {
    UploadBackOverlayToTexture(ppu);
    const PreviewTexture& overlay = GetOverlayTexture();
    if (overlay.id != 0) {
      ImGui::GetWindowDrawList()->AddImage(static_cast<ImTextureID>(static_cast<intptr_t>(overlay.id)), cursor_screen,
                                           ImVec2(cursor_screen.x + w, cursor_screen.y + h));
    }
  }
}

}  // namespace

void RenderPpuPanel(DebuggerApp& app) {
  ScopedPanel panel("PPU", app.GetUiState().show_ppu_panel);
  if (!panel) return;

  Ppu& ppu = app.GetSnes().GetPpu();

  if (ImGui::CollapsingHeader("Display", ImGuiTreeNodeFlags_DefaultOpen)) {
    DrawDisplaySection(ppu);
  }
  if (ImGui::CollapsingHeader("Backgrounds", ImGuiTreeNodeFlags_DefaultOpen)) {
    DrawBackgroundsSection(ppu);
  }
  if (ImGui::CollapsingHeader("Sprites (OBJ)")) {
    DrawSpritesSection(ppu);
  }
  if (ImGui::CollapsingHeader("VRAM port")) {
    DrawVramSection(ppu);
  }
  if (ImGui::CollapsingHeader("CGRAM port")) {
    DrawCgramPortSection(ppu);
  }
  if (ImGui::CollapsingHeader("Color math")) {
    DrawColorMathSection(ppu);
  }
  if (ImGui::CollapsingHeader("Timing")) {
    DrawTimingSection(ppu, ppu.GetPendingWriteCount());
  }
  if (ImGui::CollapsingHeader("CGRAM palette")) {
    DrawCgramPaletteSection(ppu);
  }
}

void RenderPpuViewerPanel(DebuggerApp& app) {
  if (!app.GetUiState().show_ppu_viewer_panel) {
    return;
  }
  // Drop padding so the framebuffer fills edge-to-edge.
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
  const bool open = ImGui::Begin("PPU Viewer", &app.GetUiState().show_ppu_viewer_panel);
  ImGui::PopStyleVar();
  if (!open) {
    ImGui::End();
    return;
  }

  const Ppu& ppu = app.GetSnes().GetPpu();
  const bool paused = app.GetRunControl().GetState() == RunState::kPaused;
  const bool slow_mo = app.GetUiState().speed_multiplier < 0.25F;
  DrawFittedFramebuffer(ppu, paused || slow_mo);
  ImGui::End();
}

}  // namespace pupsnes::debugger
