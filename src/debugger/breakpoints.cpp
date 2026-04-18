#include "pupsnes/debugger/breakpoints.h"

#include <vector>

namespace pupsnes::debugger {

void BreakpointSet::Set(SnesAddrT address, bool enabled) {
  breakpoints_[address] = Breakpoint{address, enabled};
  if (enabled) {
    enabled_addresses_.insert(address);
  } else {
    enabled_addresses_.erase(address);
  }
}

void BreakpointSet::Remove(SnesAddrT address) {
  breakpoints_.erase(address);
  enabled_addresses_.erase(address);
}

void BreakpointSet::Toggle(SnesAddrT address) {
  auto it = breakpoints_.find(address);
  if (it == breakpoints_.end()) {
    Set(address, true);
    return;
  }
  breakpoints_.erase(it);
  enabled_addresses_.erase(address);
}

bool BreakpointSet::Contains(SnesAddrT address) const { return breakpoints_.contains(address); }

std::vector<Breakpoint> BreakpointSet::Snapshot() const {
  std::vector<Breakpoint> snapshot;
  snapshot.reserve(breakpoints_.size());
  for (const auto& [_, breakpoint] : breakpoints_) {
    snapshot.push_back(breakpoint);
  }
  return snapshot;
}

void BreakpointSet::Clear() {
  breakpoints_.clear();
  enabled_addresses_.clear();
}

}  // namespace pupsnes::debugger
