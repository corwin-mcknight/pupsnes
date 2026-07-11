#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "pupsnes/core/types.h"

namespace pupsnes {

// Category of a structured emulation event. Values are stable and double as
// the high byte of every EmuEventKind in the category, so category extraction
// is a shift and filtering is a mask test.
enum class EmuEventCategory : uint8_t {
  kNone = 0x00,
  kPpu = 0x01,
  kDma = 0x02,
  kInterrupt = 0x03,
  kError = 0x0E,
  kHost = 0x0F,
};

// One kind per architecturally meaningful state transition. Values are
// explicit and banded by category (high byte) like SignalKind: appending new
// kinds never renumbers existing ones, so recorded event files stay
// comparable across builds. Arg meanings are fixed per kind; the matching
// human-readable format strings live in emu_event.cpp.
enum class EmuEventKind : uint16_t {
  kNone = 0x0000,

  // PPU (0x01xx) — emitted from ReplayWrite at the write's replay cycle, and
  // only when the decoded value actually changed (raw register writes are
  // already visible in the bus event log).
  kPpuBgModeChange = 0x0100,       // args: old mode, new mode, BG3 priority
  kPpuForcedBlankChange = 0x0101,  // args: old, new (0/1)
  kPpuBrightnessChange = 0x0102,   // args: old, new (0..15)
  kPpuMainScreenChange = 0x0103,   // args: old TM, new TM (layer bits)
  kPpuSubScreenChange = 0x0104,    // args: old TS, new TS (layer bits)

  // DMA / HDMA (0x02xx)
  kDmaBurstStart = 0x0200,     // args: channel mask
  kDmaBurstComplete = 0x0201,  // args: channel mask, master cycles consumed
  kHdmaFrameInit = 0x0202,     // args: latched channel mask
  kHdmaLineRun = 0x0203,       // args: V, effective channel mask, active count

  // Interrupts (0x03xx)
  kNmiAsserted = 0x0300,      // args: V of the VBlank entry line
  kIrqAsserted = 0x0301,      // args: HTIME, VTIME of the match
  kIrqAcknowledged = 0x0302,  // args: (none) — $4211 read cleared the latch

  // Errors (0x0Exx) — bridged from the debugger ErrorLog (one kind per
  // ErrorSource), not emitted by core devices directly. The human-readable
  // text rides in EmuEvent::message; args: severity, address, has_address.
  // Unlike machine events these are forwarded unconditionally — errors are
  // not subject to the events::kEnabled compile-time gate.
  kErrorCpu = 0x0E00,
  kErrorBus = 0x0E01,
  kErrorScheduler = 0x0E02,
  kErrorRomLoader = 0x0E03,
  kErrorHost = 0x0E04,

  // Host / loader (0x0Fxx)
  kRomLoaded = 0x0F00,  // args: ROM size in bytes, MapperKind value
};

[[nodiscard]] constexpr EmuEventCategory EmuEventCategoryOf(EmuEventKind kind) {
  return static_cast<EmuEventCategory>((static_cast<uint16_t>(kind) >> 8U) & 0xFFU);
}

// Bit for a category in a filter mask. Category values stay below 32 by
// construction, so a uint32_t mask covers them all.
[[nodiscard]] constexpr uint32_t EmuEventCategoryBit(EmuEventCategory category) {
  return 1U << (static_cast<uint32_t>(category) & 31U);
}

inline constexpr uint32_t kAllEmuEventCategoriesMask = 0xFFFFFFFFU;

inline constexpr std::size_t kEmuEventArgCount = 4;

// Sentinel for events that carry no H/V counter position (host events,
// bridged errors). Real counter values stay far below this.
inline constexpr uint16_t kEmuEventHvUnknown = 0xFFFFU;

// H/V counter snapshot stamped at emission. These are the PPU's counters
// (H is the dot counter, 0..339, OPHCT semantics) — captured from the live
// cursor or projected forward from it, never re-derived from master_time
// later. The counters are stateful on hardware (interlace/PAL change line
// and frame lengths), so position is recorded at the source, not computed
// at display time.
struct EmuEventHv {
  uint16_t v = kEmuEventHvUnknown;
  uint16_t h = kEmuEventHvUnknown;
};

// Record of one event. Args are raw words whose meaning is fixed per kind
// (see EmuEventKind). Formatting is deferred to the sink/UI so the emission
// hot path is a single struct store: machine events leave `message` empty
// (SSO — no allocation on copy); only bridged error events carry text.
struct EmuEvent {
  TimeMasterT master_time = 0;
  EmuEventKind kind = EmuEventKind::kNone;
  EmuEventHv hv;
  std::array<uint32_t, kEmuEventArgCount> args{};
  std::string message;
};

// Recorder interface, mirroring BusEventSink. Devices reach the sink via
// SNES::GetEmuEventSink(); null means no recorder attached. Implementations
// must be cheap and thread-free — the emulator is single-threaded.
class EmuEventSink {
 public:
  virtual ~EmuEventSink() = default;
  virtual void OnEmuEvent(const EmuEvent& event) = 0;
};

// Static description of a kind: stable token name (file/console output) and
// a std::format-style template over {0}..{3} (the event's args).
struct EmuEventInfo {
  EmuEventKind kind = EmuEventKind::kNone;
  const char* name = "";
  const char* format = "";
};

[[nodiscard]] std::span<const EmuEventInfo> AllEmuEventInfos();
// Falls back to a sentinel "UNKNOWN" entry rather than failing; a unit test
// asserts every EmuEventKind resolves to a real row.
[[nodiscard]] const EmuEventInfo& GetEmuEventInfo(EmuEventKind kind);
[[nodiscard]] const char* EmuEventCategoryName(EmuEventCategory category);
// Renders the event's human-readable message: the carried `message` text
// when present (error events), otherwise the kind's format string + args.
[[nodiscard]] std::string FormatEmuEventMessage(const EmuEvent& event);

namespace events {

// Compile-time master switch: PRODUCTION builds compile event emission out
// entirely; DEBUG and CI keep it.
#if defined(PUPSNES_PROFILE_PRODUCTION)
inline constexpr bool kEnabled = false;
#else
inline constexpr bool kEnabled = true;
#endif

// Emission helper for sites that know their H/V counter position (the PPU
// itself, or fence-fired handlers that captured it). Cost when enabled: a
// null check plus whatever the sink's OnEmuEvent does (a ring-buffer store
// for the debugger log). When disabled the body is discarded and the call
// folds away — call sites must keep argument computation trivial
// (already-decoded integers).
template <typename... Args>
inline void Emit(EmuEventSink* sink, TimeMasterT master_time, EmuEventHv hv, EmuEventKind kind, Args... args) {
  static_assert(sizeof...(Args) <= kEmuEventArgCount, "EmuEvent carries at most kEmuEventArgCount args");
  if constexpr (kEnabled) {
    if (sink == nullptr) {
      return;
    }
    EmuEvent event;
    event.master_time = master_time;
    event.kind = kind;
    event.hv = hv;
    [[maybe_unused]] std::size_t index = 0;
    ((event.args[index++] = static_cast<uint32_t>(args)), ...);
    sink->OnEmuEvent(event);
  }
}

// Emission without a counter position (host/loader events). The line
// formats render the position columns as dashes.
template <typename... Args>
inline void Emit(EmuEventSink* sink, TimeMasterT master_time, EmuEventKind kind, Args... args) {
  Emit(sink, master_time, EmuEventHv{}, kind, args...);
}

// Emission with a deferred position lookup: `hv_fn` (typically a lambda
// projecting the PPU counters to `master_time`) runs only when emission is
// enabled AND a sink is attached, so position math never burdens unlogged
// runs and is compiled out entirely in PRODUCTION.
template <typename HvFn, typename... Args>
inline void EmitWithHv(EmuEventSink* sink, TimeMasterT master_time, HvFn&& hv_fn, EmuEventKind kind, Args... args) {
  if constexpr (kEnabled) {
    if (sink == nullptr) {
      return;
    }
    Emit(sink, master_time, hv_fn(), kind, args...);
  }
}

}  // namespace events
}  // namespace pupsnes
