#pragma once

#include <cstdint>
#include <queue>
#include <vector>

#include "pupsnes/core/signal_event.h"
#include "pupsnes/core/token.h"
#include "pupsnes/core/types.h"

namespace pupsnes {

class SNES;

class Scheduler {
 private:
  using SignalHeap = std::priority_queue<SignalEvent, std::vector<SignalEvent>, SignalEventComparator>;

  SNES* snes_;  // Non-owning; SNES owns this Scheduler.
  SignalHeap signal_queue_;
  uint64_t next_signal_seq_ = 0;
  TokenTable token_table_;  // Same-clock token primitive; unchanged.

  friend struct SchedulerTestAccess;

 public:
  explicit Scheduler(SNES* snes);
  ~Scheduler();

  void Reset();

  // Signal-event API.
  void ScheduleSignal(TimeMasterT master_time, SignalKind kind, SignalEventHandler handler);
  [[nodiscard]] TimeMasterT NextEventMasterTime() const;
  void FireEventsThrough(TimeMasterT through_time);
  [[nodiscard]] bool HasPendingEvents() const { return !signal_queue_.empty(); }
  [[nodiscard]] std::vector<SignalEventView> SnapshotSignalQueue() const;

  // Token API (unchanged; same-clock tokens remain useful for bus back-pressure).
  TokenIdT CreateToken(const TokenCreateParams& params);
  [[nodiscard]] const Token* GetToken(TokenIdT id) const;
  void RemoveToken(TokenIdT id);
};

}  // namespace pupsnes
