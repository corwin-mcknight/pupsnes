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
  stop_at_instruction_boundary_ = false;
  retired_instruction_count_ = 0;
  current_instr_ = nullptr;
  local_time_ = (snes_ != nullptr) ? snes_->GetMasterTime() : 0;

  const uint8_t vector_lo = ReadResetVectorByte(0x00FFFCU);
  const uint8_t vector_hi = ReadResetVectorByte(0x00FFFDU);

  regs_.PBR = 0;
  regs_.PC = static_cast<uint16_t>(static_cast<uint16_t>(vector_hi) << 8U) | vector_lo;
}

bool CPU::IsAccumulator16Bit() const { return !regs_.P.E && !regs_.P.M; }

bool CPU::IsIndex16Bit() const { return !regs_.P.E && !regs_.P.X; }

void CPU::OpLoadA8UpdateNz() {
  regs_.A = static_cast<uint16_t>((regs_.A & 0xFF00U) | fetch_data_);
  regs_.P.Z = (static_cast<uint8_t>(regs_.A) == 0U);
  regs_.P.N = (regs_.A & 0x0080U) != 0U;
}

void CPU::OpLoadALow() { regs_.A = static_cast<uint16_t>((regs_.A & 0xFF00U) | fetch_data_); }

void CPU::OpLoadAHighUpdateNz() {
  const uint16_t high = static_cast<uint16_t>(static_cast<uint16_t>(fetch_data_) << 8U);
  regs_.A = static_cast<uint16_t>(high | (regs_.A & 0x00FFU));
  regs_.P.Z = (regs_.A == 0U);
  regs_.P.N = (regs_.A & 0x8000U) != 0U;
}

void CPU::OpLoadX8UpdateNz() {
  regs_.X = static_cast<uint16_t>((regs_.X & 0xFF00U) | fetch_data_);
  regs_.P.Z = (static_cast<uint8_t>(regs_.X) == 0U);
  regs_.P.N = (regs_.X & 0x0080U) != 0U;
}

void CPU::OpLoadXLow() { regs_.X = static_cast<uint16_t>((regs_.X & 0xFF00U) | fetch_data_); }

void CPU::OpLoadXHighUpdateNz() {
  const uint16_t high = static_cast<uint16_t>(static_cast<uint16_t>(fetch_data_) << 8U);
  regs_.X = static_cast<uint16_t>(high | (regs_.X & 0x00FFU));
  regs_.P.Z = (regs_.X == 0U);
  regs_.P.N = (regs_.X & 0x8000U) != 0U;
}

void CPU::OpLoadY8UpdateNz() {
  regs_.Y = static_cast<uint16_t>((regs_.Y & 0xFF00U) | fetch_data_);
  regs_.P.Z = (static_cast<uint8_t>(regs_.Y) == 0U);
  regs_.P.N = (regs_.Y & 0x0080U) != 0U;
}

void CPU::OpLoadYLow() { regs_.Y = static_cast<uint16_t>((regs_.Y & 0xFF00U) | fetch_data_); }

void CPU::OpLoadYHighUpdateNz() {
  const uint16_t high = static_cast<uint16_t>(static_cast<uint16_t>(fetch_data_) << 8U);
  regs_.Y = static_cast<uint16_t>(high | (regs_.Y & 0x00FFU));
  regs_.P.Z = (regs_.Y == 0U);
  regs_.P.N = (regs_.Y & 0x8000U) != 0U;
}

void CPU::OpSetBranchTaken(bool taken) { timing_context_.branch_taken = taken; }

void CPU::OpSetBranchTakenIfNotZero() { OpSetBranchTaken(!regs_.P.Z); }

void CPU::OpBranchRelative8() {
  const int8_t displacement = static_cast<int8_t>(fetch_data_);
  const uint16_t old_pc = regs_.PC;
  regs_.PC = static_cast<uint16_t>(regs_.PC + displacement);
  timing_context_.branch_page_crossed = ((old_pc ^ regs_.PC) & 0xFF00U) != 0U;
}

void CPU::OpDecrementSp() {
  if (regs_.P.E) {
    const uint8_t sp_lo = static_cast<uint8_t>(static_cast<uint8_t>(regs_.SP) - 1U);
    regs_.SP = static_cast<uint16_t>(0x0100U | sp_lo);
  } else {
    regs_.SP = static_cast<uint16_t>(regs_.SP - 1U);
  }
}

void CPU::OpIncrementSp() {
  if (regs_.P.E) {
    const uint8_t sp_lo = static_cast<uint8_t>(static_cast<uint8_t>(regs_.SP) + 1U);
    regs_.SP = static_cast<uint16_t>(0x0100U | sp_lo);
  } else {
    regs_.SP = static_cast<uint16_t>(regs_.SP + 1U);
  }
}

void CPU::OpLoadDbrUpdateNz() {
  regs_.DBR = fetch_data_;
  regs_.P.Z = (regs_.DBR == 0U);
  regs_.P.N = (regs_.DBR & 0x80U) != 0U;
}

SnesAddrT CPU::StackAddr() const { return static_cast<SnesAddrT>(regs_.SP); }

void CPU::SetAddrByteFromFetch(unsigned shift) {
  const uint32_t mask = ~(uint32_t{0xFFU} << shift) & 0xFFFFFFU;
  addr_ = (addr_ & mask) | (static_cast<uint32_t>(fetch_data_) << shift);
}

void CPU::ExecuteInternalOp(MicroInternalOp op) {
  switch (op) {
    case MicroInternalOp::kNone:
      break;
    case MicroInternalOp::kLoadA8UpdateNz:
      OpLoadA8UpdateNz();
      break;
    case MicroInternalOp::kLoadALow:
      OpLoadALow();
      break;
    case MicroInternalOp::kLoadAHighUpdateNz:
      OpLoadAHighUpdateNz();
      break;
    case MicroInternalOp::kLoadX8UpdateNz:
      OpLoadX8UpdateNz();
      break;
    case MicroInternalOp::kLoadXLow:
      OpLoadXLow();
      break;
    case MicroInternalOp::kLoadXHighUpdateNz:
      OpLoadXHighUpdateNz();
      break;
    case MicroInternalOp::kLoadY8UpdateNz:
      OpLoadY8UpdateNz();
      break;
    case MicroInternalOp::kLoadYLow:
      OpLoadYLow();
      break;
    case MicroInternalOp::kLoadYHighUpdateNz:
      OpLoadYHighUpdateNz();
      break;
    case MicroInternalOp::kSetBranchTaken:
      OpSetBranchTaken(true);
      break;
    case MicroInternalOp::kSetBranchTakenIfNotZero:
      OpSetBranchTakenIfNotZero();
      break;
    case MicroInternalOp::kBranchRelative8:
      OpBranchRelative8();
      break;
    case MicroInternalOp::kSetAddrLowFromFetch:
      SetAddrByteFromFetch(0);
      break;
    case MicroInternalOp::kSetAddrHighFromFetch:
      SetAddrByteFromFetch(8);
      break;
    case MicroInternalOp::kSetAddrBankFromFetch:
      SetAddrByteFromFetch(16);
      break;
    case MicroInternalOp::kSetAddrHighFromFetchAndBankFromDbr:
      SetAddrByteFromFetch(8);
      addr_ = (addr_ & 0x00FFFFU) | (static_cast<uint32_t>(regs_.DBR) << 16U);
      break;
    case MicroInternalOp::kIncrementAddr:
      addr_ = (addr_ + 1U) & 0xFFFFFFU;
      break;
    case MicroInternalOp::kDecrementSp:
      OpDecrementSp();
      break;
    case MicroInternalOp::kIncrementSp:
      OpIncrementSp();
      break;
    case MicroInternalOp::kLoadDbrUpdateNz:
      OpLoadDbrUpdateNz();
      break;
  }
}

void CPU::FinishInstruction() {
  retired_instruction_count_++;
  current_instr_ = nullptr;
  micro_op_index_ = 0;
  timing_context_ = TimingContext{};
}

void CPU::DrainSkippedMicroOps() {
  while (current_instr_ != nullptr) {
    const uint8_t op_idx = static_cast<uint8_t>(micro_op_index_ - 1U);
    assert(op_idx < current_instr_->remaining_op_count);

    const MicroOp& mop = current_instr_->ops[op_idx];
    if (ShouldExecuteMicroOp(mop)) {
      return;
    }

    ++micro_op_index_;
    if (micro_op_index_ - 1U >= current_instr_->remaining_op_count) {
      FinishInstruction();
    }
  }
}

void CPU::RecordFault(Fault::Type type, uint8_t opcode, SnesAddrT opcode_address) {
  fault_ = Fault{type, opcode, opcode_address, regs_};
  FinishInstruction();
}

bool CPU::EvaluateTimingCondition(TimingCondition condition) const {
  switch (condition) {
    case TimingCondition::kBranchTaken:
      return timing_context_.branch_taken;
    case TimingCondition::kAccumulator16:
      return IsAccumulator16Bit();
    case TimingCondition::kIndex16:
      return IsIndex16Bit();
    case TimingCondition::kEmulationMode:
      return regs_.P.E;
    case TimingCondition::kBranchPageCrossed:
      return timing_context_.branch_page_crossed;
  }
  return false;
}

bool CPU::EvaluateTimingRule(const TimingRuleExpr& rule) const {
  if (rule.node_count == 0) {
    return true;
  }

  const auto eval_node = [&](const auto& self, uint8_t index) -> bool {
    assert(index < rule.node_count);
    const TimingRuleNode& node = rule.nodes[index];
    switch (node.op) {
      case TimingRuleOp::kAlways:
        return true;
      case TimingRuleOp::kCondition:
        return EvaluateTimingCondition(node.condition);
      case TimingRuleOp::kNot:
        return !self(self, node.lhs);
      case TimingRuleOp::kAllOf:
        return self(self, node.lhs) && self(self, node.rhs);
      case TimingRuleOp::kAnyOf:
        return self(self, node.lhs) || self(self, node.rhs);
    }
    return false;
  };

  return eval_node(eval_node, rule.root_index);
}

bool CPU::ShouldExecuteMicroOp(const MicroOp& op) const {
  assert(current_instr_ != nullptr);
  assert(op.rule_index < current_instr_->rule_count);
  return EvaluateTimingRule(current_instr_->rules[op.rule_index]);
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

std::optional<TickResult> CPU::BusRead(SnesAddrT addr, TimeMasterDeltaT cycle_time) {
  auto result = PlanAndFollow(addr, BusAccessType::kRead, 0, cycle_time);
  if (result.outcome == BusPlanOutcome::kScheduledComplete) {
    return TickResult{cycle_time, TickStopReason::kBlockedOnToken, result.token};
  }
  fetch_data_ = result.data;
  return std::nullopt;
}

std::optional<TickResult> CPU::BusWrite(SnesAddrT addr, uint8_t data, TimeMasterDeltaT cycle_time) {
  auto result = PlanAndFollow(addr, BusAccessType::kWrite, data, cycle_time);
  if (result.outcome == BusPlanOutcome::kScheduledComplete) {
    return TickResult{cycle_time, TickStopReason::kBlockedOnToken, result.token};
  }
  return std::nullopt;
}

SnesAddrT CPU::PcAddr() const { return (static_cast<uint32_t>(regs_.PBR) << 16U) | static_cast<uint32_t>(regs_.PC); }

CPU::StepResult CPU::FetchOpcode(TimeMasterDeltaT cycle_time) {
  const SnesAddrT opcode_address = PcAddr();
  if (auto blocked = BusRead(opcode_address, cycle_time)) {
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
  micro_op_index_ = 1;
  DrainSkippedMicroOps();
  return StepResult{true, std::nullopt};
}

std::optional<TickResult> CPU::PerformBusAction(MicroBusAction action, TimeMasterDeltaT cycle_time) {
  switch (action) {
    case MicroBusAction::kNone:
      return std::nullopt;
    case MicroBusAction::kFetchPc: {
      if (auto blocked = BusRead(PcAddr(), cycle_time)) return blocked;
      regs_.PC = static_cast<uint16_t>(regs_.PC + 1U);
      return std::nullopt;
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
    case MicroBusAction::kPullStack:
      return BusRead(StackAddr(), cycle_time);
  }
  return std::nullopt;
}

CPU::StepResult CPU::ExecuteMicroOp(TimeMasterDeltaT cycle_time) {
  assert(current_instr_ != nullptr);
  DrainSkippedMicroOps();
  if (current_instr_ == nullptr) {
    return StepResult{false, std::nullopt};
  }

  const uint8_t op_idx = static_cast<uint8_t>(micro_op_index_ - 1U);
  assert(op_idx < current_instr_->remaining_op_count);

  const MicroOp& mop = current_instr_->ops[op_idx];
  if (auto blocked = PerformBusAction(mop.bus_action, cycle_time)) {
    return StepResult{false, blocked};
  }
  ExecuteInternalOp(mop.internal_op);

  ++micro_op_index_;
  if (micro_op_index_ - 1U >= current_instr_->remaining_op_count) {
    FinishInstruction();
  } else {
    DrainSkippedMicroOps();
  }
  return StepResult{true, std::nullopt};
}

TickResult CPU::Tick(TimeMasterDeltaT budget) {
  if (fault_.has_value()) {
    return {0, TickStopReason::kFaulted};
  }

  TimeMasterDeltaT cycle_time = 0;

  while (cycle_time < budget) {
    StepResult step = ShouldFetchInstruction() ? FetchOpcode(cycle_time) : ExecuteMicroOp(cycle_time);
    if (step.stop.has_value()) {
      return *step.stop;
    }
    if (step.consumed_cycle) {
      ++cycle_time;
      if (stop_at_instruction_boundary_ && ShouldFetchInstruction()) {
        return {cycle_time, TickStopReason::kReachedLocalBoundary, 0, local_time_ + cycle_time};
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
