#pragma once

#include <cstddef>
#include <cstdint>

#include "pupsnes/core/scheduler.h"
#include "pupsnes/core/signal_event.h"
#include "pupsnes/core/token.h"

namespace pupsnes {

struct SchedulerTestAccess {
  static std::size_t signal_queue_size(const Scheduler& s) {
    return s.signal_queue_.size();
  }
  static uint64_t next_signal_seq(const Scheduler& s) {
    return s.next_signal_seq_;
  }
  static TokenTable& token_table(Scheduler& s) {
    return s.token_table_;
  }
};

}  // namespace pupsnes
