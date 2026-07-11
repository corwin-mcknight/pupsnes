#pragma once

#include <cstdint>
#include <ostream>

#include "pupsnes/core/emu_event.h"

namespace pupsnes::debugger {

// Streams one formatted line per event to an ostream (headless tools pass
// std::cerr so event output interleaves with diagnostics, not tool output).
// Non-owning: the stream must outlive the sink.
class ConsoleEventSink : public EmuEventSink {
 public:
  explicit ConsoleEventSink(std::ostream& out) : out_(&out) {}

  void OnEmuEvent(const EmuEvent& event) override;

  void SetCategoryMask(uint32_t mask) { category_mask_ = mask; }
  [[nodiscard]] uint32_t GetCategoryMask() const { return category_mask_; }

 private:
  std::ostream* out_;
  uint32_t category_mask_ = kAllEmuEventCategoriesMask;
};

}  // namespace pupsnes::debugger
