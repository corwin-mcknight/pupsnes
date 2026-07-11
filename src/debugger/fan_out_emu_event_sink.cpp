#include "pupsnes/debugger/fan_out_emu_event_sink.h"

#include <algorithm>

namespace pupsnes::debugger {

void FanOutEmuEventSink::Attach(EmuEventSink* sink) {
  if (sink == nullptr) return;
  if (std::find(sinks_.begin(), sinks_.end(), sink) != sinks_.end()) return;
  sinks_.push_back(sink);
}

void FanOutEmuEventSink::Detach(EmuEventSink* sink) {
  sinks_.erase(std::remove(sinks_.begin(), sinks_.end(), sink), sinks_.end());
}

void FanOutEmuEventSink::OnEmuEvent(const EmuEvent& event) {
  for (EmuEventSink* sink : sinks_) {
    sink->OnEmuEvent(event);
  }
}

}  // namespace pupsnes::debugger
