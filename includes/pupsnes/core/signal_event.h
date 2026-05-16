#pragma once

#include <cstdint>
#include <functional>

#include "pupsnes/core/types.h"

namespace pupsnes {

// Stable, growing enumeration of scheduler signal events. Explicit values so
// a new kind can be added without shuffling existing ones (save-state friendly).
enum class SignalKind : uint16_t {
  kFrameEnd = 0,           // PPU frame wrap; swap buffers, fire frame callback
  kVblankNmiBoundary = 1,  // start of vblank; checks NMI enable and raises CPU NMI
  kHIrqMatch = 2,          // H/V timer match; checks IRQ enable and raises CPU IRQ

  // Reserved; not fired in v1.
  kApuSampleDeadline = 16,
  kDmaBurstComplete = 32,
  kHdmaFire = 33,

  kMaxKind = 0xFFFF,
};

// One-shot handler invoked when the scheduler fires the event. Runs under a
// fully-synced machine (MachineSync has advanced every device to master_time).
using SignalEventHandler = std::function<void(TimeMasterT master_time)>;

struct SignalEvent {
  TimeMasterT master_time;
  SignalKind kind;
  SignalEventHandler handler;  // may be empty for pure timing marks
  uint64_t seq;                // stable secondary ordering key
};

struct SignalEventComparator {
  bool operator()(const SignalEvent& a, const SignalEvent& b) const {
    if (a.master_time != b.master_time) return a.master_time > b.master_time;
    if (a.kind != b.kind) return a.kind > b.kind;
    return a.seq > b.seq;
  }
};

struct SignalEventView {
  TimeMasterT master_time;
  SignalKind kind;
};

}  // namespace pupsnes
