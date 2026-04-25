#include "pupsnes/debugger/fan_out_trace_sink.h"

#include <algorithm>

namespace pupsnes::debugger {

void FanOutTraceSink::Attach(TraceSink* sink) {
  if (sink == nullptr) return;
  if (std::find(sinks_.begin(), sinks_.end(), sink) != sinks_.end()) return;
  sinks_.push_back(sink);
}

void FanOutTraceSink::Detach(TraceSink* sink) {
  sinks_.erase(std::remove(sinks_.begin(), sinks_.end(), sink), sinks_.end());
}

void FanOutTraceSink::Record(const TraceEntry& entry) {
  for (TraceSink* sink : sinks_) {
    sink->Record(entry);
  }
}

}  // namespace pupsnes::debugger
