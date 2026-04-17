#include "pupsnes/debugger/error_log.h"

#include <format>
#include <string>
#include <utility>
#include <vector>

namespace pupsnes::debugger {

namespace {

std::string FormatAddress(SnesAddrT address) {
  return std::format("${:02X}:{:04X}", static_cast<unsigned>(address >> 16), static_cast<unsigned>(address & 0xFFFFU));
}

}  // namespace

void ErrorLog::TrimToCapacity() {
  while (entries_.size() > capacity_) {
    entries_.pop_front();
  }
}

void ErrorLog::Push(ErrorEvent event) {
  entries_.push_back(std::move(event));
  TrimToCapacity();
}

void ErrorLog::PushCpuFault(TimeMasterT master_time, const CPU::Fault& fault) {
  Push({
      .master_time = master_time,
      .severity = ErrorSeverity::kError,
      .source = ErrorSource::kCpu,
      .message = std::format("Unimplemented opcode 0x{:02X} at {}", fault.opcode, FormatAddress(fault.opcode_address)),
      .regs = fault.regs,
      .address = fault.opcode_address,
  });
}

void ErrorLog::PushBusAccessFailure(TimeMasterT master_time, SnesAddrT address, std::string message,
                                    std::optional<CPU::Regs> regs, ErrorSeverity severity) {
  Push({
      .master_time = master_time,
      .severity = severity,
      .source = ErrorSource::kBus,
      .message = std::move(message),
      .regs = regs,
      .address = address,
  });
}

void ErrorLog::PushDebugWriteRefusal(TimeMasterT master_time, const DebugWriteResult& result,
                                     std::optional<CPU::Regs> regs) {
  PushBusAccessFailure(master_time, result.address, DescribeDebugAccessFailure(result), regs);
}

void ErrorLog::PushSchedulerError(TimeMasterT master_time, std::string message, std::optional<CPU::Regs> regs,
                                  ErrorSeverity severity) {
  Push({
      .master_time = master_time,
      .severity = severity,
      .source = ErrorSource::kScheduler,
      .message = std::move(message),
      .regs = regs,
  });
}

void ErrorLog::PushHostError(TimeMasterT master_time, std::string message, ErrorSeverity severity) {
  Push({
      .master_time = master_time,
      .severity = severity,
      .source = ErrorSource::kHost,
      .message = std::move(message),
  });
}

void ErrorLog::Clear() { entries_.clear(); }

std::vector<ErrorEvent> ErrorLog::Snapshot() const { return {entries_.begin(), entries_.end()}; }

const ErrorEvent* ErrorLog::Latest() const { return entries_.empty() ? nullptr : &entries_.back(); }

std::string DescribeDebugAccessFailure(const DebugWriteResult& result) {
  return std::format("Debug write refused at {} (device {}, reason {})", FormatAddress(result.address),
                     result.device_id, static_cast<int>(result.failure));
}

std::string DescribeDebugAccessFailure(const DebugReadResult& result) {
  return std::format("Debug read failed at {} (device {}, reason {})", FormatAddress(result.address), result.device_id,
                     static_cast<int>(result.failure));
}

}  // namespace pupsnes::debugger
