#pragma once

#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "pupsnes/hw/5a22/cpu.h"
#include "pupsnes/memory/systembus.h"

namespace pupsnes::debugger {

enum class ErrorSeverity : uint8_t {
  kInfo = 0,
  kWarning = 1,
  kError = 2,
  kFatal = 3,
};

enum class ErrorSource : uint8_t {
  kCpu = 0,
  kBus = 1,
  kScheduler = 2,
  kRomLoader = 3,
  kHost = 4,
};

struct ErrorEvent {
  TimeMasterT master_time = 0;
  ErrorSeverity severity = ErrorSeverity::kError;
  ErrorSource source = ErrorSource::kHost;
  std::string message;
  std::optional<CPU::Regs> regs = std::nullopt;
  std::optional<SnesAddrT> address = std::nullopt;
  bool acknowledged = false;
};

class ErrorLog {
 public:
  explicit ErrorLog(std::size_t capacity = 2048) : capacity_(capacity) {}

  void Push(ErrorEvent event);
  void PushCpuFault(TimeMasterT master_time, const CPU::Fault& fault);
  void PushBusAccessFailure(TimeMasterT master_time, SnesAddrT address, std::string message,
                            std::optional<CPU::Regs> regs = std::nullopt,
                            ErrorSeverity severity = ErrorSeverity::kError);
  void PushDebugWriteRefusal(TimeMasterT master_time, const DebugWriteResult& result,
                             std::optional<CPU::Regs> regs = std::nullopt);
  void PushSchedulerError(TimeMasterT master_time, std::string message, std::optional<CPU::Regs> regs = std::nullopt,
                          ErrorSeverity severity = ErrorSeverity::kFatal);
  void PushHostError(TimeMasterT master_time, std::string message, ErrorSeverity severity = ErrorSeverity::kFatal);
  void Clear();
  [[nodiscard]] std::vector<ErrorEvent> Snapshot() const;
  [[nodiscard]] const ErrorEvent* Latest() const;

 private:
  void TrimToCapacity();

  std::size_t capacity_ = 2048;
  std::deque<ErrorEvent> entries_;
};

[[nodiscard]] std::string DescribeDebugAccessFailure(const DebugWriteResult& result);
[[nodiscard]] std::string DescribeDebugAccessFailure(const DebugReadResult& result);

}  // namespace pupsnes::debugger
