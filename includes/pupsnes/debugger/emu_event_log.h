#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "pupsnes/core/emu_event.h"
#include "pupsnes/debugger/ring_buffer.h"

namespace pupsnes::debugger {

// Fixed-capacity ring of structured emulation events for the debugger event
// panel. Capture-side filtering is a category mask (default: everything);
// the panel applies its own display filter on top, so narrowing here is only
// needed when a category is too chatty to keep in the ring at all.
class EmuEventLog : public EmuEventSink {
 public:
  explicit EmuEventLog(std::size_t capacity = 4096) : ring_(capacity) {}

  void OnEmuEvent(const EmuEvent& event) override {
    if ((category_mask_ & EmuEventCategoryBit(EmuEventCategoryOf(event.kind))) == 0U) {
      return;
    }
    ring_.Push(event);
  }

  void Clear() { ring_.Clear(); }
  void SetCategoryMask(uint32_t mask) { category_mask_ = mask; }
  [[nodiscard]] uint32_t GetCategoryMask() const { return category_mask_; }

  [[nodiscard]] std::size_t Size() const { return ring_.Size(); }
  [[nodiscard]] std::size_t Capacity() const { return ring_.Capacity(); }
  [[nodiscard]] std::vector<EmuEvent> Snapshot() const { return ring_.Snapshot(); }

 private:
  RingBuffer<EmuEvent> ring_;
  uint32_t category_mask_ = kAllEmuEventCategoriesMask;
};

}  // namespace pupsnes::debugger
