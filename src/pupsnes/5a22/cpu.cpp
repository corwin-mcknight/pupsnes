#include "pupsnes/hw/5a22/cpu.h"

#include <cassert>
#include <cstdint>
#include <optional>
#include <stdexcept>

#include "pupsnes/hw/scheduler.h"
#include "pupsnes/hw/snes.h"
#include "pupsnes/hw/systembus.h"

namespace pupsnes {

// ---------------------------------------------------------------------------
// CpuFlags
// ---------------------------------------------------------------------------

uint8_t CpuFlags::ToByte() const {
  uint8_t p = 0;
  if (N) p |= 0x80U;
  if (V) p |= 0x40U;
  if (M) p |= 0x20U;
  if (X) p |= 0x10U;
  if (D) p |= 0x08U;
  if (I) p |= 0x04U;
  if (Z) p |= 0x02U;
  if (C) p |= 0x01U;
  return p;
}

void CpuFlags::FromByte(uint8_t p, bool emulation_mode) {
  N = (p & 0x80U) != 0U;
  V = (p & 0x40U) != 0U;
  // In emulation mode M and X are forced to 1 and cannot be changed via P.
  if (!emulation_mode) {
    M = (p & 0x20U) != 0U;
    X = (p & 0x10U) != 0U;
  }
  D = (p & 0x08U) != 0U;
  I = (p & 0x04U) != 0U;
  Z = (p & 0x02U) != 0U;
  C = (p & 0x01U) != 0U;
}

// ---------------------------------------------------------------------------
// CPU
// ---------------------------------------------------------------------------

CPU::CPU(SNES* snes) : Device(snes) {}

void CPU::Reset() {
  regs_ = Regs();
  regs_.SP = 0x01FFU;
  regs_.P = CpuFlags{};
  regs_.P.E = true;
  regs_.P.M = true;
  regs_.P.X = true;
  regs_.P.I = true;

  micro_op_index_ = 0;
  fetch_data_ = 0;
  addr_ = 0;
  timing_context_ = TimingContext{};
  fault_.reset();
  last_debugger_stop_.reset();
  retired_instruction_count_ = 0;
  current_instr_ = nullptr;
  needs_drain_ = false;
  system_bus_raw_ = (snes_ != nullptr) ? snes_->system_bus.get() : nullptr;
  local_time_ = (snes_ != nullptr) ? snes_->GetMasterTime() : 0;

  // First DRAM refresh fires kDramRefreshStartCycle master cycles after reset.
  next_refresh_time_ = local_time_ + kDramRefreshStartCycle;
  refresh_cycles_remaining_ = 0;
  retired_refresh_windows_ = 0;
  retired_refresh_cycles_ = 0;

  const uint8_t vector_lo = ReadResetVectorByte(0x00FFFCU);
  const uint8_t vector_hi = ReadResetVectorByte(0x00FFFDU);

  regs_.PBR = 0;
  regs_.PC = static_cast<uint16_t>(static_cast<uint16_t>(vector_hi) << 8U) | vector_lo;
}

void CPU::FinishInstruction() {
  if (micro_op_recorder_ != nullptr && current_instr_ != nullptr) {
    micro_op_recorder_->OnInstructionEnd(retired_instruction_count_ + 1);
  }
  // Trace-push is success-only: callers that retire via a fault (RecordFault)
  // skip the push so the faulted instruction is not logged as executed.
  if (current_instr_ != nullptr && debugger_contract_.trace_sink != nullptr) {
    debugger_contract_.trace_sink->Record(pending_trace_);
  }
  retired_instruction_count_++;
  current_instr_ = nullptr;
  needs_drain_ = false;
  micro_op_index_ = 0;
  timing_context_ = TimingContext{};
}

void CPU::DrainSkippedMicroOpsSlow() {
  while (current_instr_ != nullptr) {
    const InstructionEntry* instr = current_instr_;
    const MicroOp* const ops = instr->ops.data();
    const uint32_t* const rules = instr->rules.data();
    const uint8_t remaining = instr->remaining_op_count;

    const uint8_t op_idx = static_cast<uint8_t>(micro_op_index_ - 1U);
    assert(op_idx < remaining);

    const MicroOp& mop = ops[op_idx];
    assert(mop.rule_index < instr->rule_count);
    if (EvaluateTimingRule(rules[mop.rule_index])) {
      return;
    }

    if (micro_op_recorder_ != nullptr) {
      MicroOpRecord rec;
      rec.index = micro_op_index_;
      rec.bus_action = mop.bus_action;
      rec.internal_op = mop.internal_op;
      rec.status = MicroOpStatus::kSkipped;
      rec.fetch_data = fetch_data_;
      rec.addr = addr_;
      micro_op_recorder_->OnMicroOp(rec);
    }

    ++micro_op_index_;
    if (micro_op_index_ - 1U >= remaining) {
      FinishInstruction();
    }
  }
}

void CPU::RecordFault(Fault::Type type, uint8_t opcode, SnesAddrT opcode_address) {
  fault_ = Fault{type, opcode, opcode_address, regs_};
  FinishInstruction();
}

BusFollowResult CPU::PlanAndFollow(SnesAddrT addr, BusAccessType type, uint8_t data, TimeMasterDeltaT cycle_time) {
  auto plan = snes_->system_bus->Plan(addr, type, data);
  return snes_->system_bus->Follow(plan, local_time_ + cycle_time, device_id_);
}

uint8_t CPU::ReadResetVectorByte(SnesAddrT addr) {
  if (snes_ == nullptr || snes_->system_bus == nullptr) {
    return 0xFFU;
  }

  auto result = PlanAndFollow(addr, BusAccessType::kRead, 0, 0);
  if (result.outcome == BusPlanOutcome::kScheduledComplete) {
    throw std::logic_error("CPU reset vector fetch cannot block on asynchronous bus access");
  }
  return result.data;
}

TickResult CPU::BusReadSlow(SnesAddrT addr, TimeMasterDeltaT cycle_time) {
  auto result = PlanAndFollow(addr, BusAccessType::kRead, 0, cycle_time);
  if (result.outcome == BusPlanOutcome::kScheduledComplete) {
    return TickResult{cycle_time, TickStopReason::kBlockedOnToken, result.token};
  }
  fetch_data_ = result.data;
  return TickResult{0, TickStopReason::kContinue};
}

TickResult CPU::BusWriteSlow(SnesAddrT addr, uint8_t data, TimeMasterDeltaT cycle_time) {
  auto result = PlanAndFollow(addr, BusAccessType::kWrite, data, cycle_time);
  if (result.outcome == BusPlanOutcome::kScheduledComplete) {
    return TickResult{cycle_time, TickStopReason::kBlockedOnToken, result.token};
  }
  return TickResult{0, TickStopReason::kContinue};
}

CPU::StepResult CPU::FetchOpcode(TimeMasterDeltaT cycle_time) {
  const SnesAddrT opcode_address = PcAddr();

  if (const auto* bp = debugger_contract_.breakpoints;
      bp != nullptr && bp->AnyEnabled() && bp->IsEnabled(opcode_address)) {
    if (debugger_contract_.suppressed_breakpoint_pc == opcode_address) {
      debugger_contract_.suppressed_breakpoint_pc.reset();
    } else {
      last_debugger_stop_ = TickStopReason::kDebuggerBreakpoint;
      return StepResult{false, TickResult{cycle_time, TickStopReason::kDebuggerBreakpoint}};
    }
  }

  pending_trace_ = TraceEntry{local_time_ + cycle_time, opcode_address, regs_};

  TickResult blocked = BusRead(opcode_address, cycle_time);
  if (blocked.reason != TickStopReason::kContinue) {
    return StepResult{false, blocked};
  }
  regs_.PC = static_cast<uint16_t>(regs_.PC + 1U);
  timing_context_ = TimingContext{};

  const InstructionEntry& entry = kOpcodeTable[fetch_data_];
  if (entry.disposition == InstructionDisposition::kFaultUnimplemented) {
    RecordFault(Fault::Type::kUnimplementedOpcode, fetch_data_, opcode_address);
    return StepResult{true, TickResult{static_cast<TimeMasterDeltaT>(cycle_time + 1U), TickStopReason::kFaulted}};
  }

  current_instr_ = &entry;
  needs_drain_ = entry.rule_count > 1;
  micro_op_index_ = 1;
  if (micro_op_recorder_ != nullptr) {
    micro_op_recorder_->OnInstructionBegin(fetch_data_, opcode_address);
    MicroOpRecord rec;
    rec.index = 0;
    rec.bus_action = MicroBusAction::kFetchPc;
    rec.internal_op = MicroInternalOp::kNone;
    rec.status = MicroOpStatus::kExecuted;
    rec.fetch_data = fetch_data_;
    rec.addr = addr_;
    rec.has_bus = true;
    rec.bus_addr = opcode_address;
    rec.bus_value = fetch_data_;
    micro_op_recorder_->OnMicroOp(rec);
  }
  DrainSkippedMicroOps();
  return StepResult{true, TickResult{0, TickStopReason::kContinue}};
}

TickResult CPU::PerformBusAction(MicroBusAction action, TimeMasterDeltaT cycle_time) {
  switch (action) {
    case MicroBusAction::kNone:
      return TickResult{0, TickStopReason::kContinue};
    case MicroBusAction::kFetchPc: {
      TickResult blocked = BusRead(PcAddr(), cycle_time);
      if (blocked.reason != TickStopReason::kContinue) return blocked;
      regs_.PC = static_cast<uint16_t>(regs_.PC + 1U);
      return TickResult{0, TickStopReason::kContinue};
    }
    case MicroBusAction::kReadAddr:
      return BusRead(addr_, cycle_time);
    case MicroBusAction::kWriteAddr:
      return BusWrite(addr_, fetch_data_, cycle_time);
    case MicroBusAction::kWriteA8Addr:
      return BusWrite(addr_, static_cast<uint8_t>(regs_.A), cycle_time);
    case MicroBusAction::kWriteX8Addr:
      return BusWrite(addr_, static_cast<uint8_t>(regs_.X), cycle_time);
    case MicroBusAction::kWriteY8Addr:
      return BusWrite(addr_, static_cast<uint8_t>(regs_.Y), cycle_time);
    case MicroBusAction::kWriteAHighAddr:
      return BusWrite(addr_, static_cast<uint8_t>(regs_.A >> 8U), cycle_time);
    case MicroBusAction::kWriteXHighAddr:
      return BusWrite(addr_, static_cast<uint8_t>(regs_.X >> 8U), cycle_time);
    case MicroBusAction::kWriteYHighAddr:
      return BusWrite(addr_, static_cast<uint8_t>(regs_.Y >> 8U), cycle_time);
    case MicroBusAction::kPushA8:
      return BusWrite(StackAddr(), static_cast<uint8_t>(regs_.A), cycle_time);
    case MicroBusAction::kPushAHigh:
      return BusWrite(StackAddr(), static_cast<uint8_t>(regs_.A >> 8U), cycle_time);
    case MicroBusAction::kPushDbr:
      return BusWrite(StackAddr(), regs_.DBR, cycle_time);
    case MicroBusAction::kPushPch:
      return BusWrite(StackAddr(), static_cast<uint8_t>(regs_.PC >> 8U), cycle_time);
    case MicroBusAction::kPushPcl:
      return BusWrite(StackAddr(), static_cast<uint8_t>(regs_.PC), cycle_time);
    case MicroBusAction::kPushPbr:
      return BusWrite(StackAddr(), regs_.PBR, cycle_time);
    case MicroBusAction::kPushP:
      return BusWrite(StackAddr(), regs_.P.ToByte(), cycle_time);
    case MicroBusAction::kPushX8:
      return BusWrite(StackAddr(), static_cast<uint8_t>(regs_.X), cycle_time);
    case MicroBusAction::kPushXHigh:
      return BusWrite(StackAddr(), static_cast<uint8_t>(regs_.X >> 8U), cycle_time);
    case MicroBusAction::kPushY8:
      return BusWrite(StackAddr(), static_cast<uint8_t>(regs_.Y), cycle_time);
    case MicroBusAction::kPushYHigh:
      return BusWrite(StackAddr(), static_cast<uint8_t>(regs_.Y >> 8U), cycle_time);
    case MicroBusAction::kPushDpLow:
      return BusWrite(StackAddr(), static_cast<uint8_t>(regs_.DP), cycle_time);
    case MicroBusAction::kPushDpHigh:
      return BusWrite(StackAddr(), static_cast<uint8_t>(regs_.DP >> 8U), cycle_time);
    case MicroBusAction::kPushAddrLow:
      return BusWrite(StackAddr(), static_cast<uint8_t>(addr_ & 0xFFU), cycle_time);
    case MicroBusAction::kPushAddrHigh:
      return BusWrite(StackAddr(), static_cast<uint8_t>((addr_ >> 8U) & 0xFFU), cycle_time);
    case MicroBusAction::kPullStack:
      return BusRead(StackAddr(), cycle_time);
    case MicroBusAction::kPreIncPullStack:
      // Stack pulls: the SP must point at the top of the stack before reading.
      // Increment first, then read.
      if (regs_.P.E) {
        const uint8_t sp_lo = static_cast<uint8_t>(static_cast<uint8_t>(regs_.SP) + 1U);
        regs_.SP = static_cast<uint16_t>(0x0100U | sp_lo);
      } else {
        regs_.SP = static_cast<uint16_t>(regs_.SP + 1U);
      }
      return BusRead(StackAddr(), cycle_time);
  }
  return TickResult{0, TickStopReason::kContinue};
}

CPU::StepResult CPU::ExecuteMicroOp(TimeMasterDeltaT cycle_time) {
  assert(current_instr_ != nullptr);
  // Rules have already been drained by the previous FetchOpcode /
  // ExecuteMicroOp call that transitioned us here; draining again would
  // re-evaluate them for no benefit.
  const InstructionEntry* instr = current_instr_;
  const uint8_t op_idx = static_cast<uint8_t>(micro_op_index_ - 1U);
  assert(op_idx < instr->remaining_op_count);

  const MicroOp* const ops = instr->ops.data();
  const MicroOp& mop = ops[op_idx];
  const SnesAddrT pre_pc = PcAddr();
  TickResult blocked = PerformBusAction(mop.bus_action, cycle_time);
  if (blocked.reason != TickStopReason::kContinue) {
    return StepResult{false, blocked};
  }
  ExecuteInternalOp(mop.internal_op);

  if (micro_op_recorder_ != nullptr) {
    MicroOpRecord rec;
    rec.index = micro_op_index_;
    rec.bus_action = mop.bus_action;
    rec.internal_op = mop.internal_op;
    rec.status = MicroOpStatus::kExecuted;
    rec.fetch_data = fetch_data_;
    rec.addr = addr_;
    rec.has_bus = (mop.bus_action != MicroBusAction::kNone);
    rec.bus_addr = (mop.bus_action == MicroBusAction::kFetchPc) ? pre_pc : addr_;
    rec.bus_value = fetch_data_;
    micro_op_recorder_->OnMicroOp(rec);
  }

  ++micro_op_index_;
  if (micro_op_index_ - 1U >= instr->remaining_op_count) {
    FinishInstruction();
  } else {
    DrainSkippedMicroOps();
  }
  return StepResult{true, TickResult{0, TickStopReason::kContinue}};
}

TickResult CPU::Tick(TimeMasterDeltaT budget) {
  if (fault_.has_value()) {
    return {0, TickStopReason::kFaulted};
  }

  TimeMasterDeltaT cycle_time = 0;

  while (cycle_time < budget) {
    // DRAM refresh stalls the CPU mid-scanline. When the refresh window is
    // active, consume cycles without issuing any bus ops. When we cross the
    // scheduled refresh start, arm the window and arrange the next one.
    if (refresh_cycles_remaining_ > 0) {
      const TimeMasterDeltaT available = budget - cycle_time;
      const TimeMasterDeltaT take =
          (refresh_cycles_remaining_ < available) ? refresh_cycles_remaining_ : available;
      cycle_time += take;
      refresh_cycles_remaining_ -= take;
      retired_refresh_cycles_ += take;
      continue;
    }
    if (local_time_ + cycle_time >= next_refresh_time_) {
      refresh_cycles_remaining_ = kDramRefreshDurationCycles;
      next_refresh_time_ += kMasterCyclesPerScanline;
      ++retired_refresh_windows_;
      continue;
    }

    StepResult step = ShouldFetchInstruction() ? FetchOpcode(cycle_time) : ExecuteMicroOp(cycle_time);
    if (step.stop.reason != TickStopReason::kContinue) {
      return step.stop;
    }
    if (step.consumed_cycle) {
      ++cycle_time;
      if (ShouldFetchInstruction() && debugger_contract_.step_target > 0) {
        --debugger_contract_.step_target;
        if (debugger_contract_.step_target == 0) {
          last_debugger_stop_ = TickStopReason::kDebuggerStepComplete;
          return {cycle_time, TickStopReason::kDebuggerStepComplete};
        }
      }
    }
  }

  return {cycle_time, TickStopReason::kBudgetExhausted};
}

void CPU::OnEvent(const SchedulerEvent& /*event*/) {
  // CommitComplete/WakeSample do not currently require CPU-side mutation.
  // The scheduler wakes blocked CPU runs by replacing the authoritative Run
  // wake.
}

}  // namespace pupsnes
