#pragma once

#include <cstddef>
#include <vector>

#include "pupsnes/core/emu_event.h"

namespace pupsnes::debugger {

// Non-owning multi-dispatch EmuEventSink. Forwards OnEmuEvent() to every
// attached sink in attach order. Attach is idempotent; Detach tolerates
// unknown pointers. Intended lifetime: sinks outlive the fan-out.
class FanOutEmuEventSink : public EmuEventSink {
 public:
  void Attach(EmuEventSink* sink);
  void Detach(EmuEventSink* sink);
  void OnEmuEvent(const EmuEvent& event) override;
  [[nodiscard]] std::size_t Count() const { return sinks_.size(); }

 private:
  std::vector<EmuEventSink*> sinks_;
};

}  // namespace pupsnes::debugger
