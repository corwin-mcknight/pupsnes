#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include "pupsnes/hw/5a22/cpu_regs.h"
#include "pupsnes/hw/debugger_contract.h"
#include "pupsnes/hw/device.h"
#include "pupsnes/hw/systembus.h"
#include "pupsnes/types.h"

namespace pupsnes {

class SNES;

// Bus actions a micro-op can perform in a single master clock cycle.
enum class MicroBusAction : uint8_t {
  kNone,            // Internal cycle — no bus transaction
  kFetchPc,         // Read byte from PBR:PC, increment PC
  kReadAddr,        // Read byte from effective address (addr_)
  kWriteAddr,       // Write fetch_data_ to effective address (addr_)
  kWriteA8Addr,     // Write A low byte to effective address (addr_)
  kWriteX8Addr,     // Write X low byte to effective address (addr_)
  kWriteY8Addr,     // Write Y low byte to effective address (addr_)
  kWriteAHighAddr,  // Write A high byte to effective address (addr_)
  kWriteXHighAddr,  // Write X high byte to effective address (addr_)
  kWriteYHighAddr,  // Write Y high byte to effective address (addr_)
  kPushA8,          // Write A low byte to stack ($00:SP)
  kPushAHigh,       // Write A high byte to stack ($00:SP)
  kPushDbr,         // Write DBR to stack ($00:SP)
  kPullStack,       // Read byte from stack ($00:SP) into fetch_data_
};

// Internal register operations performed after the bus action completes.
enum class MicroInternalOp : uint8_t {
  kNone,
  kLoadA8UpdateNz,                      // A_lo = fetch_data_; update N/Z using effective 8-bit accumulator width
  kLoadALow,                            // A_lo = fetch_data_
  kLoadAHighUpdateNz,                   // A_hi = fetch_data_; update N/Z using effective 16-bit accumulator width
  kLoadX8UpdateNz,                      // X_lo = fetch_data_; update N/Z using effective 8-bit index width
  kLoadXLow,                            // X_lo = fetch_data_
  kLoadXHighUpdateNz,                   // X_hi = fetch_data_; update N/Z using effective 16-bit index width
  kLoadY8UpdateNz,                      // Y_lo = fetch_data_; update N/Z using effective 8-bit index width
  kLoadYLow,                            // Y_lo = fetch_data_
  kLoadYHighUpdateNz,                   // Y_hi = fetch_data_; update N/Z using effective 16-bit index width
  kSetBranchTaken,                      // branch_taken = true
  kSetBranchTakenIfNotZero,             // branch_taken = !Z
  kBranchRelative8,                     // Apply signed 8-bit branch offset stored in fetch_data_
                                        // to PC
  kSetAddrLowFromFetch,                 // addr_[7:0] = fetch_data_
  kSetAddrHighFromFetch,                // addr_[15:8] = fetch_data_
  kSetAddrBankFromFetch,                // addr_[23:16] = fetch_data_
  kSetAddrHighFromFetchAndBankFromDbr,  // addr_[15:8] = fetch_data_, addr_[23:16] = DBR
  kIncrementAddr,                       // addr_ = addr_ + 1
  kDecrementSp,                         // Decrement SP (wraps in page 1 when E=1)
  kIncrementSp,                         // Increment SP (wraps in page 1 when E=1)
  kLoadDbrUpdateNz,                     // DBR = fetch_data_; update N/Z (8-bit)
  kIncA,                                // A = A + 1 (width per M flag); update N/Z
  kDecA,                                // A = A - 1 (width per M flag); update N/Z
  kIncX,                                // X = X + 1 (width per X flag); update N/Z
  kDecX,                                // X = X - 1 (width per X flag); update N/Z
  kIncY,                                // Y = Y + 1 (width per X flag); update N/Z
  kDecY,                                // Y = Y - 1 (width per X flag); update N/Z
};

enum class TimingCondition : uint8_t {
  kBranchTaken = 0,
  kAccumulator16 = 1,
  kIndex16 = 2,
  kEmulationMode = 3,
  kBranchPageCrossed = 4,
};

// Bit index of each TimingCondition within the packed-condition word used to
// index a rule's precomputed 32-entry truth table.
inline constexpr uint8_t kTimingConditionCount = 5;

enum class TimingRuleOp : uint8_t {
  kAlways = 0,
  kCondition = 1,
  kNot = 2,
  kAllOf = 3,
  kAnyOf = 4,
};

struct TimingRuleNode {
  TimingRuleOp op = TimingRuleOp::kAlways;
  TimingCondition condition = TimingCondition::kBranchTaken;
  uint8_t lhs = 0;
  uint8_t rhs = 0;
};

inline constexpr uint8_t kMaxTimingRuleNodes = 7;

struct TimingRuleExpr {
  uint8_t node_count = 0;
  uint8_t root_index = 0;
  std::array<TimingRuleNode, kMaxTimingRuleNodes> nodes{};
  // Precomputed truth table: bit k is set iff the rule evaluates true when
  // packed-condition bits == k. Filled by LowerOpcode for rules baked into
  // InstructionEntry; left zero (meaning "no precomputation") for transient
  // spec-time rules. EvaluateTimingRule reads this directly.
  uint32_t truth_table = 0;
};

struct MicroOp {
  MicroBusAction bus_action = MicroBusAction::kNone;
  MicroInternalOp internal_op = MicroInternalOp::kNone;
  uint8_t rule_index = 0;
};

// Maximum micro-ops remaining after the opcode fetch (longest 65C816
// instruction = 7 cycles).
inline constexpr uint8_t kMaxRemainingOps = 7;
inline constexpr uint8_t kMaxInstructionRules = 4;

enum class InstructionDisposition : uint8_t {
  kImplemented = 0,
  kFaultUnimplemented = 1,
};

// Per-opcode micro-op sequence (the cycles that follow the initial opcode
// fetch). Total instruction cycles = 1 (opcode fetch) + remaining_op_count.
struct InstructionEntry {
  InstructionDisposition disposition = InstructionDisposition::kFaultUnimplemented;
  uint8_t remaining_op_count = 0;
  uint8_t rule_count = 0;
  std::array<MicroOp, kMaxRemainingOps> ops{};
  std::array<TimingRuleExpr, kMaxInstructionRules> rules{};
};

enum class MicroOpStatus : uint8_t {
  kPending = 0,
  kExecuted = 1,
  kSkipped = 2,
};

struct MicroOpRecord {
  uint8_t index = 0;  // 0 = opcode fetch, 1..N = remaining ops (1-based)
  MicroBusAction bus_action = MicroBusAction::kNone;
  MicroInternalOp internal_op = MicroInternalOp::kNone;
  MicroOpStatus status = MicroOpStatus::kPending;
  uint8_t fetch_data = 0;
  uint32_t addr = 0;
  bool has_bus = false;
  uint32_t bus_addr = 0;
  uint8_t bus_value = 0;
};

class MicroOpRecorder {
 public:
  virtual ~MicroOpRecorder() = default;
  virtual void OnInstructionBegin(uint8_t opcode, SnesAddrT pc) = 0;
  virtual void OnMicroOp(const MicroOpRecord& rec) = 0;
  virtual void OnInstructionEnd(uint64_t retired_seq) = 0;
};

// 65C816 CPU device.
//
// tick() walks a micro-op table: cycle 0 always fetches the opcode via the
// SystemBus, then the per-opcode remaining ops execute one per cycle. All bus
// accesses use SystemBus plan/follow.
class CPU : public Device {
 public:
  // Alias so existing call sites can keep using CPU::Regs while the type lives
  // in a standalone header for reuse without pulling in the full CPU class.
  using Regs = CpuRegs;

  struct Fault {
    enum class Type : uint8_t {
      kUnimplementedOpcode = 0,
    };

    Type type = Type::kUnimplementedOpcode;
    uint8_t opcode = 0;
    SnesAddrT opcode_address = 0;
    Regs regs{};
  };

  explicit CPU(SNES* snes);
  ~CPU() override = default;

  void Reset();

  [[nodiscard]] TickResult Tick(TimeMasterDeltaT budget) override;
  void OnEvent(const SchedulerEvent& event) override;

  [[nodiscard]] Regs GetRegs() const { return regs_; }
  void SetRegs(const Regs& r) { regs_ = r; }
  [[nodiscard]] uint8_t GetMicroOpIndex() const { return micro_op_index_; }
  [[nodiscard]] const std::optional<Fault>& GetFault() const { return fault_; }
  [[nodiscard]] uint64_t GetRetiredInstructionCount() const { return retired_instruction_count_; }

  // Last debugger-driven stop reason (breakpoint or step-complete) emitted by
  // Tick. RunControl consumes this via TakeLastDebuggerStop() after a
  // Scheduler::Step to map the reason to a pause transition. Fault stops use
  // GetFault() instead; ordinary stops (kBudgetExhausted / kBlockedOnToken /
  // kReachedLocalBoundary / kNoWork) don't land here.
  [[nodiscard]] std::optional<TickStopReason> TakeLastDebuggerStop() {
    std::optional<TickStopReason> out = last_debugger_stop_;
    last_debugger_stop_.reset();
    return out;
  }

  void SetMicroOpRecorder(MicroOpRecorder* recorder) { micro_op_recorder_ = recorder; }
  [[nodiscard]] MicroOpRecorder* GetMicroOpRecorder() const { return micro_op_recorder_; }

  // The debugger (RunControl) owns the contract and mutates it between Tick
  // calls to request step-N behavior or acknowledge a suppressed breakpoint.
  // CPU reads it inside FetchOpcode / on instruction retire and writes back
  // into suppressed_breakpoint_pc / step_target as it hits stop conditions.
  void SetDebuggerContract(const DebuggerContract& contract) { debugger_contract_ = contract; }
  [[nodiscard]] const DebuggerContract& GetDebuggerContract() const { return debugger_contract_; }
  [[nodiscard]] DebuggerContract& MutableDebuggerContract() { return debugger_contract_; }

 private:
  struct TimingContext {
    bool branch_taken = false;
    bool branch_page_crossed = false;
  };

  Regs regs_;

  // Micro-op execution state.
  // micro_op_index_ == 0 means we are between instructions (next cycle fetches
  // the opcode); 1..N indexes into the current instruction's remaining ops.
  uint8_t micro_op_index_ = 0;
  uint8_t fetch_data_ = 0;  // Last byte read from bus
  uint32_t addr_ = 0;       // Effective address accumulator
  TimingContext timing_context_{};
  std::optional<Fault> fault_ = std::nullopt;
  uint64_t retired_instruction_count_ = 0;
  MicroOpRecorder* micro_op_recorder_ = nullptr;
  DebuggerContract debugger_contract_{};
  // Trace entry captured at instruction-begin (PC + pre-execute regs) and
  // pushed to the trace sink when the instruction retires.
  TraceEntry pending_trace_{};
  std::optional<TickStopReason> last_debugger_stop_ = std::nullopt;

  const InstructionEntry* current_instr_ = nullptr;

  // Opcode → micro-op sequence table.  Defined in cpu_opcodes.cpp.
  static const std::array<InstructionEntry, 256> kOpcodeTable;

  [[nodiscard]] bool ShouldFetchInstruction() const { return micro_op_index_ == 0; }

  struct StepResult {
    bool consumed_cycle = false;
    TickResult stop{0, TickStopReason::kContinue};
  };

  // Tick helpers: one per branch of the fetch/execute loop. Each returns a
  // TickResult whose reason is kContinue when the cycle completes inline, or
  // a real stop reason when the caller must return it.
  [[nodiscard]] StepResult FetchOpcode(TimeMasterDeltaT cycle_time);
  [[nodiscard]] StepResult ExecuteMicroOp(TimeMasterDeltaT cycle_time);
  [[nodiscard]] TickResult PerformBusAction(MicroBusAction action, TimeMasterDeltaT cycle_time);

  void ExecuteInternalOp(MicroInternalOp op);
  [[nodiscard]] bool IsAccumulator16Bit() const;
  [[nodiscard]] bool IsIndex16Bit() const;

  void OpLoadA8UpdateNz();
  void OpLoadALow();
  void OpLoadAHighUpdateNz();
  void OpLoadX8UpdateNz();
  void OpLoadXLow();
  void OpLoadXHighUpdateNz();
  void OpLoadY8UpdateNz();
  void OpLoadYLow();
  void OpLoadYHighUpdateNz();
  void OpSetBranchTaken(bool taken);
  void OpSetBranchTakenIfNotZero();
  void OpBranchRelative8();
  void OpDecrementSp();
  void OpIncrementSp();
  void OpLoadDbrUpdateNz();
  void OpIncA();
  void OpDecA();
  void OpIncX();
  void OpDecX();
  void OpIncY();
  void OpDecY();
  [[nodiscard]] SnesAddrT StackAddr() const;
  // Replace byte [shift, shift+7] of addr_ with fetch_data_.
  void SetAddrByteFromFetch(unsigned shift);
  void FinishInstruction();
  void DrainSkippedMicroOpsSlow();
  // Fast guard: instructions whose only rule is "Always" (rule_count == 1) have
  // no conditional micro-ops, so the drain loop can be skipped entirely. Most
  // 65C816 opcodes fall into this category, so inlining the guard eliminates
  // both the call and the loop-entry overhead on the common path.
  void DrainSkippedMicroOps() {
    if (current_instr_ == nullptr || current_instr_->rule_count <= 1) {
      return;
    }
    DrainSkippedMicroOpsSlow();
  }
  void RecordFault(Fault::Type type, uint8_t opcode, SnesAddrT opcode_address);
  [[nodiscard]] bool EvaluateTimingRule(const TimingRuleExpr& rule) const;

  [[nodiscard]] uint8_t ReadResetVectorByte(SnesAddrT addr);

  [[nodiscard]] SnesAddrT PcAddr() const;

  // Issue a Plan/Follow pair against the system bus at local_time_+cycle_time.
  // Returns the follow result; caller interprets outcome.
  [[nodiscard]] BusFollowResult PlanAndFollow(SnesAddrT addr, BusAccessType type, uint8_t data,
                                              TimeMasterDeltaT cycle_time);

  // Execute a bus read. Returns a TickResult whose reason is kContinue on
  // inline completion (fetch_data_ updated) or a stop reason when the access
  // blocks.
  [[nodiscard]] TickResult BusRead(SnesAddrT addr, TimeMasterDeltaT cycle_time);

  // Execute a bus write. Reason is kContinue on inline completion, or a stop
  // reason when the access blocks.
  [[nodiscard]] TickResult BusWrite(SnesAddrT addr, uint8_t data, TimeMasterDeltaT cycle_time);
};

}  // namespace pupsnes
