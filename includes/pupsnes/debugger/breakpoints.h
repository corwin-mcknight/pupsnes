#pragma once

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "pupsnes/types.h"

namespace pupsnes::debugger {

struct Breakpoint {
  SnesAddrT address = 0;
  bool enabled = true;
};

class BreakpointSet {
 public:
  void Set(SnesAddrT address, bool enabled = true);
  void Remove(SnesAddrT address);
  void Toggle(SnesAddrT address);
  [[nodiscard]] bool Contains(SnesAddrT address) const;
  [[nodiscard]] bool IsEnabled(SnesAddrT address) const {
    if (enabled_addresses_.empty()) return false;
    return enabled_addresses_.find(address) != enabled_addresses_.end();
  }
  [[nodiscard]] bool Empty() const { return breakpoints_.empty(); }
  [[nodiscard]] bool AnyEnabled() const { return !enabled_addresses_.empty(); }
  [[nodiscard]] std::vector<Breakpoint> Snapshot() const;
  void Clear();

 private:
  std::unordered_map<SnesAddrT, Breakpoint> breakpoints_;
  std::unordered_set<SnesAddrT> enabled_addresses_;
};

}  // namespace pupsnes::debugger
