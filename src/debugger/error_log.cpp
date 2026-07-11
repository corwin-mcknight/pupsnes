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

constexpr EmuEventKind EventKindForSource(ErrorSource source) {
  switch (source) {
    case ErrorSource::kCpu: return EmuEventKind::kErrorCpu;
    case ErrorSource::kBus: return EmuEventKind::kErrorBus;
    case ErrorSource::kScheduler: return EmuEventKind::kErrorScheduler;
    case ErrorSource::kRomLoader: return EmuEventKind::kErrorRomLoader;
    case ErrorSource::kHost: return EmuEventKind::kErrorHost;
  }
  return EmuEventKind::kErrorHost;
}

}  // namespace

void ErrorLog::TrimToCapacity() {
  while (entries_.size() > capacity_) {
    entries_.pop_front();
  }
}

void ErrorLog::Push(ErrorEvent event) {
  if (event_sink_ != nullptr) {
    EmuEvent forwarded;
    forwarded.master_time = event.master_time;
    forwarded.kind = EventKindForSource(event.source);
    forwarded.args[0] = static_cast<uint32_t>(event.severity);
    forwarded.args[1] = event.address.value_or(0);
    forwarded.args[2] = event.address.has_value() ? 1U : 0U;
    forwarded.message = event.message;
    event_sink_->OnEmuEvent(forwarded);
  }
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

const char* ErrorSeverityName(ErrorSeverity severity) {
  switch (severity) {
    case ErrorSeverity::kInfo: return "Info";
    case ErrorSeverity::kWarning: return "Warn";
    case ErrorSeverity::kError: return "Error";
    case ErrorSeverity::kFatal: return "Fatal";
  }
  return "?";
}

const char* ErrorSourceName(ErrorSource source) {
  switch (source) {
    case ErrorSource::kCpu: return "CPU";
    case ErrorSource::kBus: return "Bus";
    case ErrorSource::kScheduler: return "Scheduler";
    case ErrorSource::kRomLoader: return "ROM";
    case ErrorSource::kHost: return "Host";
  }
  return "?";
}

std::string DescribeDebugAccessFailure(const DebugWriteResult& result) {
  return std::format("Debug write refused at {} (device {}, reason {})", FormatAddress(result.address),
                     result.device_id, static_cast<int>(result.failure));
}

std::string DescribeDebugAccessFailure(const DebugReadResult& result) {
  return std::format("Debug read failed at {} (device {}, reason {})", FormatAddress(result.address), result.device_id,
                     static_cast<int>(result.failure));
}

}  // namespace pupsnes::debugger
