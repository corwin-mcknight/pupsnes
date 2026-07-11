#include "pupsnes/core/emu_event.h"

#include <format>
#include <string>

namespace pupsnes {

namespace {

// One row per EmuEventKind. The [events] unit tests cross-check that every
// kind in AllEmuEventInfos resolves through GetEmuEventInfo and that its
// category band is valid, so a kind added to the enum without a row here
// fails tests instead of printing "UNKNOWN".
constexpr std::array kEmuEventInfos = std::to_array<EmuEventInfo>({
    {EmuEventKind::kNone, "NONE", "(none)"},
    {EmuEventKind::kPpuBgModeChange, "BG_MODE_CHANGE", "BG mode {0} -> {1} (BG3 priority {2})"},
    {EmuEventKind::kPpuForcedBlankChange, "FORCED_BLANK_CHANGE", "forced blank {0} -> {1}"},
    {EmuEventKind::kPpuBrightnessChange, "BRIGHTNESS_CHANGE", "brightness {0} -> {1}"},
    {EmuEventKind::kPpuMainScreenChange, "MAIN_SCREEN_CHANGE", "TM layers {0:#04x} -> {1:#04x}"},
    {EmuEventKind::kPpuSubScreenChange, "SUB_SCREEN_CHANGE", "TS layers {0:#04x} -> {1:#04x}"},
    {EmuEventKind::kDmaBurstStart, "DMA_BURST_START", "GP-DMA burst start, channels {0:#04x}"},
    {EmuEventKind::kDmaBurstComplete, "DMA_BURST_COMPLETE", "GP-DMA burst complete, channels {0:#04x}, {1} mcyc"},
    {EmuEventKind::kHdmaFrameInit, "HDMA_FRAME_INIT", "HDMA frame init, channels {0:#04x}"},
    {EmuEventKind::kHdmaLineRun, "HDMA_LINE_RUN", "HDMA line V={0}, channels {1:#04x} ({2} active)"},
    {EmuEventKind::kNmiAsserted, "NMI_ASSERTED", "NMI latch armed at V={0}"},
    {EmuEventKind::kIrqAsserted, "IRQ_ASSERTED", "H/V IRQ match (HTIME={0}, VTIME={1})"},
    {EmuEventKind::kIrqAcknowledged, "IRQ_ACKNOWLEDGED", "TIMEUP read cleared the IRQ latch"},
    {EmuEventKind::kErrorCpu, "ERROR_CPU", "CPU fault"},
    {EmuEventKind::kErrorBus, "ERROR_BUS", "bus access failure"},
    {EmuEventKind::kErrorScheduler, "ERROR_SCHEDULER", "scheduler error"},
    {EmuEventKind::kErrorRomLoader, "ERROR_ROM_LOADER", "ROM loader error"},
    {EmuEventKind::kErrorHost, "ERROR_HOST", "host error"},
    {EmuEventKind::kRomLoaded, "ROM_LOADED", "ROM loaded: {0} bytes, mapper {1}"},
});

constexpr EmuEventInfo kUnknownEmuEventInfo{EmuEventKind::kNone, "UNKNOWN", "unknown event kind {0:#06x}"};

}  // namespace

std::span<const EmuEventInfo> AllEmuEventInfos() { return kEmuEventInfos; }

const EmuEventInfo& GetEmuEventInfo(EmuEventKind kind) {
  for (const EmuEventInfo& info : kEmuEventInfos) {
    if (info.kind == kind) {
      return info;
    }
  }
  return kUnknownEmuEventInfo;
}

const char* EmuEventCategoryName(EmuEventCategory category) {
  switch (category) {
    case EmuEventCategory::kNone: return "NONE";
    case EmuEventCategory::kPpu: return "PPU";
    case EmuEventCategory::kDma: return "DMA";
    case EmuEventCategory::kInterrupt: return "IRQ";
    case EmuEventCategory::kError: return "ERROR";
    case EmuEventCategory::kHost: return "HOST";
  }
  return "?";
}

std::string FormatEmuEventMessage(const EmuEvent& event) {
  if (!event.message.empty()) {
    return event.message;
  }
  const EmuEventInfo& info = GetEmuEventInfo(event.kind);
  // The sentinel's format references {0} as the unresolved kind value; real
  // rows reference only the args they document. Passing all four args plus
  // the kind keeps both cases well-formed (vformat ignores unreferenced
  // arguments).
  if (info.kind == EmuEventKind::kNone && event.kind != EmuEventKind::kNone) {
    const auto kind_value = static_cast<uint32_t>(event.kind);
    return std::vformat(info.format, std::make_format_args(kind_value));
  }
  return std::vformat(info.format, std::make_format_args(event.args[0], event.args[1], event.args[2], event.args[3]));
}

}  // namespace pupsnes
