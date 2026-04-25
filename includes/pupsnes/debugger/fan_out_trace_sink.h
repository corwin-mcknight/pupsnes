#pragma once

#include <cstddef>
#include <vector>

#include "pupsnes/hw/debugger_contract.h"

namespace pupsnes::debugger {

// Non-owning multi-dispatch TraceSink. Forwards Record() to every attached
// sink in attach order. Attach is idempotent; Detach tolerates unknown
// pointers. Intended lifetime: sinks outlive the fan-out.
class FanOutTraceSink : public TraceSink {
 public:
  void Attach(TraceSink* sink);
  void Detach(TraceSink* sink);
  void Record(const TraceEntry& entry) override;
  [[nodiscard]] std::size_t Count() const { return sinks_.size(); }

 private:
  std::vector<TraceSink*> sinks_;
};

}  // namespace pupsnes::debugger
