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

// SNES timing constants for CPU-owned DRAM refresh.
//
// The 5A22 stalls the bus for 40 master cycles every scanline to refresh DRAM.
// The pause begins at roughly master-cycle offset 538 into each 1364-cycle
// scanline. We model this as a CPU-internal stall: no bus ops are issued
// during the window, and local_time advances normally.
inline constexpr TimeMasterDeltaT kMasterCyclesPerScanline = 1364;
inline constexpr TimeMasterDeltaT kDramRefreshStartCycle = 538;
inline constexpr TimeMasterDeltaT kDramRefreshDurationCycles = 40;

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
  kPushPch,         // Write PC high byte to stack ($00:SP)
  kPushPcl,         // Write PC low byte to stack ($00:SP)
  kPushPbr,         // Write PBR to stack ($00:SP)
  kPushP,           // Write status register to stack ($00:SP)
  kPushX8,          // Write X low byte to stack
  kPushXHigh,       // Write X high byte to stack
  kPushY8,          // Write Y low byte to stack
  kPushYHigh,       // Write Y high byte to stack
  kPushDpLow,       // Write DP low byte to stack
  kPushDpHigh,      // Write DP high byte to stack
  kPushAddrLow,     // Write addr_[7:0] to stack
  kPushAddrHigh,    // Write addr_[15:8] to stack
  kPullStack,       // Read byte from stack ($00:SP) into fetch_data_
  kPreIncPullStack, // Increment SP then read from stack into fetch_data_
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
  kClearCarry,                          // P.C = 0
  kSetCarry,                            // P.C = 1
  kClearDecimal,                        // P.D = 0
  kSetDecimal,                          // P.D = 1
  kClearInterrupt,                      // P.I = 0
  kSetInterrupt,                        // P.I = 1
  kClearOverflow,                       // P.V = 0
  kRepFromFetch,                        // P &= ~fetch_data_ (E=1 forces M,X back to 1)
  kSepFromFetch,                        // P |= fetch_data_ (E=1 forces M,X to 1)
  kExchangeCarryEmulation,              // swap C and E; on E=1 force M,X=1, XH/YH=0, SH=$01
  kTransferAToX,                        // X = A (width per X flag); update N/Z
  kTransferAToY,                        // Y = A (width per X flag); update N/Z
  kTransferSToX,                        // X = SP (width per X flag); update N/Z
  kTransferXToA,                        // A = X (width per M flag); update N/Z
  kTransferXToS,                        // SP = X (16-bit native; SH forced to $01 when E=1)
  kTransferXToY,                        // Y = X (width per X flag); update N/Z
  kTransferYToA,                        // A = Y (width per M flag); update N/Z
  kTransferYToX,                        // X = Y (width per X flag); update N/Z
  kTransferAToD,                        // DP = C (16-bit); update N/Z
  kTransferAToS,                        // SP = C (16-bit; E=1 forces SH=$01)
  kTransferDToA,                        // C = DP (16-bit); update N/Z
  kTransferSToA,                        // C = SP (16-bit); update N/Z
  kSetBranchTakenIfZero,                // branch_taken = Z (BEQ)
  kSetBranchTakenIfCarry,               // branch_taken = C (BCS)
  kSetBranchTakenIfNotCarry,            // branch_taken = !C (BCC)
  kSetBranchTakenIfNegative,            // branch_taken = N (BMI)
  kSetBranchTakenIfNotNegative,         // branch_taken = !N (BPL)
  kSetBranchTakenIfOverflow,            // branch_taken = V (BVS)
  kSetBranchTakenIfNotOverflow,         // branch_taken = !V (BVC)
  kBranchRelative16,                    // PC += signed 16-bit from addr_[15:0]
  kSetPcFromAddr,                       // PC = addr_[15:0]
  kSetPcAndPbrFromAddr,                 // PC = addr_[15:0], PBR = addr_[23:16]
  kSetAddrHighFromFetchAndSetPc,        // addr_[15:8] = fetch; PC = addr_[15:0] (single-cycle JMP)
  kSetAddrBankFromFetchAndSetPcAndPbr,  // addr_[23:16] = fetch; PC+PBR from addr (single-cycle JML)
  kDecrementPc,                         // PC -= 1
  kIncrementPc,                         // PC += 1
  kSetPclFromFetch,                     // PC low = fetch_data_
  kSetPchFromFetch,                     // PC high = fetch_data_
  kSetPbrFromFetch,                     // PBR = fetch_data_
  kLoadPFromFetch,                      // P.FromByte(fetch_data_, E)
  kLoadDpLowFromFetch,                  // DP low = fetch_data_
  kLoadDpHighFromFetchUpdateNz,         // DP high = fetch_data_; update N/Z (16-bit)
  kLoadXHighFromFetchUpdateNz,          // X high = fetch_data_; update N/Z (16-bit)
  kLoadYHighFromFetchUpdateNz,          // Y high = fetch_data_; update N/Z (16-bit)
  // 8-bit ALU ops: use fetch_data_ as the operand, apply to A low byte (or X/Y).
  kAluAdc8FromFetch,
  kAluSbc8FromFetch,
  kAluAnd8FromFetch,
  kAluOra8FromFetch,
  kAluEor8FromFetch,
  kAluCmp8FromFetch,
  kAluCpx8FromFetch,
  kAluCpy8FromFetch,
  kAluBit8ImmFromFetch,  // Immediate BIT: only Z updated
  // 16-bit ALU ops: operand_low stashed in addr_[7:0]; fetch_data_ is operand high.
  kAluAdc16FromFetch,
  kAluSbc16FromFetch,
  kAluAnd16FromFetch,
  kAluOra16FromFetch,
  kAluEor16FromFetch,
  kAluCmp16FromFetch,
  kAluCpx16FromFetch,
  kAluCpy16FromFetch,
  kAluBit16ImmFromFetch,
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

  // DRAM-refresh telemetry. Windows counts the number of 40-cycle refresh
  // pauses the CPU has served since reset; cycles is the cumulative stall
  // total (normally windows * kDramRefreshDurationCycles unless a window was
  // cut short by a budget boundary, in which case the remainder is counted
  // on the next Tick). next-time is the absolute master time at which the
  // next refresh window will begin.
  [[nodiscard]] uint64_t GetRefreshStallWindows() const { return retired_refresh_windows_; }
  [[nodiscard]] uint64_t GetRefreshStallCycles() const { return retired_refresh_cycles_; }
  [[nodiscard]] TimeMasterT GetNextRefreshTime() const { return next_refresh_time_; }

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
  // Cached at FetchOpcode time from current_instr_->rule_count > 1. Lets the
  // per-micro-op drain guard be a single-load/branch check instead of
  // dereferencing current_instr_. Cleared in FinishInstruction / RecordFault.
  bool needs_drain_ = false;
  // Cached at Reset() from snes_->system_bus.get(). Lets the inline BusRead /
  // BusWrite fast path skip the unique_ptr<> deref (non-trivial in debug).
  SystemBus* system_bus_raw_ = nullptr;

  // DRAM refresh state. next_refresh_time_ is the absolute master time at
  // which the next 40-cycle refresh window begins. refresh_cycles_remaining_
  // is the number of stall cycles left in the current window (0 when no
  // refresh is active).
  TimeMasterT next_refresh_time_ = kDramRefreshStartCycle;
  TimeMasterDeltaT refresh_cycles_remaining_ = 0;
  uint64_t retired_refresh_windows_ = 0;
  uint64_t retired_refresh_cycles_ = 0;

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

  // Mode helpers. Inlined so IsIndex16Bit / IsAccumulator16Bit checks inside
  // the Op* helpers collapse into the surrounding switch.
  [[nodiscard]] [[gnu::always_inline]] inline bool IsAccumulator16Bit() const { return !regs_.P.E && !regs_.P.M; }
  [[nodiscard]] [[gnu::always_inline]] inline bool IsIndex16Bit() const { return !regs_.P.E && !regs_.P.X; }

  // Per-MicroInternalOp helpers. All marked always_inline so ExecuteInternalOp
  // (also always_inline) collapses its entire switch into ExecuteMicroOp.
  [[gnu::always_inline]] inline void OpLoadA8UpdateNz() {
    regs_.A = static_cast<uint16_t>((regs_.A & 0xFF00U) | fetch_data_);
    regs_.P.Z = (static_cast<uint8_t>(regs_.A) == 0U);
    regs_.P.N = (regs_.A & 0x0080U) != 0U;
  }
  [[gnu::always_inline]] inline void OpLoadALow() {
    regs_.A = static_cast<uint16_t>((regs_.A & 0xFF00U) | fetch_data_);
  }
  [[gnu::always_inline]] inline void OpLoadAHighUpdateNz() {
    const uint16_t high = static_cast<uint16_t>(static_cast<uint16_t>(fetch_data_) << 8U);
    regs_.A = static_cast<uint16_t>(high | (regs_.A & 0x00FFU));
    regs_.P.Z = (regs_.A == 0U);
    regs_.P.N = (regs_.A & 0x8000U) != 0U;
  }
  [[gnu::always_inline]] inline void OpLoadX8UpdateNz() {
    regs_.X = static_cast<uint16_t>((regs_.X & 0xFF00U) | fetch_data_);
    regs_.P.Z = (static_cast<uint8_t>(regs_.X) == 0U);
    regs_.P.N = (regs_.X & 0x0080U) != 0U;
  }
  [[gnu::always_inline]] inline void OpLoadXLow() {
    regs_.X = static_cast<uint16_t>((regs_.X & 0xFF00U) | fetch_data_);
  }
  [[gnu::always_inline]] inline void OpLoadXHighUpdateNz() {
    const uint16_t high = static_cast<uint16_t>(static_cast<uint16_t>(fetch_data_) << 8U);
    regs_.X = static_cast<uint16_t>(high | (regs_.X & 0x00FFU));
    regs_.P.Z = (regs_.X == 0U);
    regs_.P.N = (regs_.X & 0x8000U) != 0U;
  }
  [[gnu::always_inline]] inline void OpLoadY8UpdateNz() {
    regs_.Y = static_cast<uint16_t>((regs_.Y & 0xFF00U) | fetch_data_);
    regs_.P.Z = (static_cast<uint8_t>(regs_.Y) == 0U);
    regs_.P.N = (regs_.Y & 0x0080U) != 0U;
  }
  [[gnu::always_inline]] inline void OpLoadYLow() {
    regs_.Y = static_cast<uint16_t>((regs_.Y & 0xFF00U) | fetch_data_);
  }
  [[gnu::always_inline]] inline void OpLoadYHighUpdateNz() {
    const uint16_t high = static_cast<uint16_t>(static_cast<uint16_t>(fetch_data_) << 8U);
    regs_.Y = static_cast<uint16_t>(high | (regs_.Y & 0x00FFU));
    regs_.P.Z = (regs_.Y == 0U);
    regs_.P.N = (regs_.Y & 0x8000U) != 0U;
  }
  [[gnu::always_inline]] inline void OpSetBranchTaken(bool taken) { timing_context_.branch_taken = taken; }
  [[gnu::always_inline]] inline void OpSetBranchTakenIfNotZero() { OpSetBranchTaken(!regs_.P.Z); }
  [[gnu::always_inline]] inline void OpBranchRelative8() {
    const int8_t displacement = static_cast<int8_t>(fetch_data_);
    const uint16_t old_pc = regs_.PC;
    regs_.PC = static_cast<uint16_t>(regs_.PC + displacement);
    timing_context_.branch_page_crossed = ((old_pc ^ regs_.PC) & 0xFF00U) != 0U;
  }
  [[gnu::always_inline]] inline void OpDecrementSp() {
    if (regs_.P.E) {
      const uint8_t sp_lo = static_cast<uint8_t>(static_cast<uint8_t>(regs_.SP) - 1U);
      regs_.SP = static_cast<uint16_t>(0x0100U | sp_lo);
    } else {
      regs_.SP = static_cast<uint16_t>(regs_.SP - 1U);
    }
  }
  [[gnu::always_inline]] inline void OpIncrementSp() {
    if (regs_.P.E) {
      const uint8_t sp_lo = static_cast<uint8_t>(static_cast<uint8_t>(regs_.SP) + 1U);
      regs_.SP = static_cast<uint16_t>(0x0100U | sp_lo);
    } else {
      regs_.SP = static_cast<uint16_t>(regs_.SP + 1U);
    }
  }
  [[gnu::always_inline]] inline void OpLoadDbrUpdateNz() {
    regs_.DBR = fetch_data_;
    regs_.P.Z = (regs_.DBR == 0U);
    regs_.P.N = (regs_.DBR & 0x80U) != 0U;
  }
  [[gnu::always_inline]] inline void OpIncA() {
    if (IsAccumulator16Bit()) {
      regs_.A = static_cast<uint16_t>(regs_.A + 1U);
      regs_.P.Z = (regs_.A == 0U);
      regs_.P.N = (regs_.A & 0x8000U) != 0U;
    } else {
      const uint8_t lo = static_cast<uint8_t>(static_cast<uint8_t>(regs_.A) + 1U);
      regs_.A = static_cast<uint16_t>((regs_.A & 0xFF00U) | lo);
      regs_.P.Z = (lo == 0U);
      regs_.P.N = (lo & 0x80U) != 0U;
    }
  }
  [[gnu::always_inline]] inline void OpDecA() {
    if (IsAccumulator16Bit()) {
      regs_.A = static_cast<uint16_t>(regs_.A - 1U);
      regs_.P.Z = (regs_.A == 0U);
      regs_.P.N = (regs_.A & 0x8000U) != 0U;
    } else {
      const uint8_t lo = static_cast<uint8_t>(static_cast<uint8_t>(regs_.A) - 1U);
      regs_.A = static_cast<uint16_t>((regs_.A & 0xFF00U) | lo);
      regs_.P.Z = (lo == 0U);
      regs_.P.N = (lo & 0x80U) != 0U;
    }
  }
  [[gnu::always_inline]] inline void OpIncX() {
    if (IsIndex16Bit()) {
      regs_.X = static_cast<uint16_t>(regs_.X + 1U);
      regs_.P.Z = (regs_.X == 0U);
      regs_.P.N = (regs_.X & 0x8000U) != 0U;
    } else {
      const uint8_t lo = static_cast<uint8_t>(static_cast<uint8_t>(regs_.X) + 1U);
      regs_.X = static_cast<uint16_t>((regs_.X & 0xFF00U) | lo);
      regs_.P.Z = (lo == 0U);
      regs_.P.N = (lo & 0x80U) != 0U;
    }
  }
  [[gnu::always_inline]] inline void OpDecX() {
    if (IsIndex16Bit()) {
      regs_.X = static_cast<uint16_t>(regs_.X - 1U);
      regs_.P.Z = (regs_.X == 0U);
      regs_.P.N = (regs_.X & 0x8000U) != 0U;
    } else {
      const uint8_t lo = static_cast<uint8_t>(static_cast<uint8_t>(regs_.X) - 1U);
      regs_.X = static_cast<uint16_t>((regs_.X & 0xFF00U) | lo);
      regs_.P.Z = (lo == 0U);
      regs_.P.N = (lo & 0x80U) != 0U;
    }
  }
  [[gnu::always_inline]] inline void OpIncY() {
    if (IsIndex16Bit()) {
      regs_.Y = static_cast<uint16_t>(regs_.Y + 1U);
      regs_.P.Z = (regs_.Y == 0U);
      regs_.P.N = (regs_.Y & 0x8000U) != 0U;
    } else {
      const uint8_t lo = static_cast<uint8_t>(static_cast<uint8_t>(regs_.Y) + 1U);
      regs_.Y = static_cast<uint16_t>((regs_.Y & 0xFF00U) | lo);
      regs_.P.Z = (lo == 0U);
      regs_.P.N = (lo & 0x80U) != 0U;
    }
  }
  [[gnu::always_inline]] inline void OpDecY() {
    if (IsIndex16Bit()) {
      regs_.Y = static_cast<uint16_t>(regs_.Y - 1U);
      regs_.P.Z = (regs_.Y == 0U);
      regs_.P.N = (regs_.Y & 0x8000U) != 0U;
    } else {
      const uint8_t lo = static_cast<uint8_t>(static_cast<uint8_t>(regs_.Y) - 1U);
      regs_.Y = static_cast<uint16_t>((regs_.Y & 0xFF00U) | lo);
      regs_.P.Z = (lo == 0U);
      regs_.P.N = (lo & 0x80U) != 0U;
    }
  }
  [[gnu::always_inline]] inline void OpApplyEmulationForcing() {
    // When e=1 the m and x flags are forced to 1, XH/YH forced to $00, and the
    // stack is forced onto page 1 (SH = $01). Called after any op that can
    // change P or e.
    if (regs_.P.E) {
      regs_.P.M = true;
      regs_.P.X = true;
      regs_.X &= 0x00FFU;
      regs_.Y &= 0x00FFU;
      regs_.SP = static_cast<uint16_t>(0x0100U | (regs_.SP & 0x00FFU));
    }
  }
  [[gnu::always_inline]] inline void OpRepFromFetch() {
    uint8_t p = regs_.P.ToByte();
    p = static_cast<uint8_t>(p & ~fetch_data_);
    regs_.P.FromByte(p, regs_.P.E);
    OpApplyEmulationForcing();
  }
  [[gnu::always_inline]] inline void OpSepFromFetch() {
    uint8_t p = regs_.P.ToByte();
    p = static_cast<uint8_t>(p | fetch_data_);
    regs_.P.FromByte(p, regs_.P.E);
    OpApplyEmulationForcing();
  }
  [[gnu::always_inline]] inline void OpExchangeCarryEmulation() {
    const bool new_e = regs_.P.C;
    const bool new_c = regs_.P.E;
    regs_.P.E = new_e;
    regs_.P.C = new_c;
    OpApplyEmulationForcing();
  }
  [[gnu::always_inline]] inline void SetNzFromWidth(uint16_t value, bool wide) {
    if (wide) {
      regs_.P.Z = (value == 0U);
      regs_.P.N = (value & 0x8000U) != 0U;
    } else {
      const uint8_t lo = static_cast<uint8_t>(value);
      regs_.P.Z = (lo == 0U);
      regs_.P.N = (lo & 0x80U) != 0U;
    }
  }
  // dest = src but only replace the low byte (or full 16 bits) according to width.
  [[gnu::always_inline]] inline uint16_t MergeByWidth(uint16_t dest, uint16_t src, bool wide) {
    return wide ? src : static_cast<uint16_t>((dest & 0xFF00U) | (src & 0x00FFU));
  }
  [[gnu::always_inline]] inline void OpTransferAToX() {
    const bool wide = IsIndex16Bit();
    regs_.X = MergeByWidth(regs_.X, regs_.A, wide);
    SetNzFromWidth(regs_.X, wide);
  }
  [[gnu::always_inline]] inline void OpTransferAToY() {
    const bool wide = IsIndex16Bit();
    regs_.Y = MergeByWidth(regs_.Y, regs_.A, wide);
    SetNzFromWidth(regs_.Y, wide);
  }
  [[gnu::always_inline]] inline void OpTransferSToX() {
    const bool wide = IsIndex16Bit();
    regs_.X = MergeByWidth(regs_.X, regs_.SP, wide);
    SetNzFromWidth(regs_.X, wide);
  }
  [[gnu::always_inline]] inline void OpTransferXToA() {
    const bool wide = IsAccumulator16Bit();
    regs_.A = MergeByWidth(regs_.A, regs_.X, wide);
    SetNzFromWidth(regs_.A, wide);
  }
  [[gnu::always_inline]] inline void OpTransferXToS() {
    // SP is always written full width except that E=1 forces SH back to $01.
    regs_.SP = regs_.X;
    if (regs_.P.E) {
      regs_.SP = static_cast<uint16_t>(0x0100U | (regs_.SP & 0x00FFU));
    }
  }
  [[gnu::always_inline]] inline void OpTransferXToY() {
    const bool wide = IsIndex16Bit();
    regs_.Y = MergeByWidth(regs_.Y, regs_.X, wide);
    SetNzFromWidth(regs_.Y, wide);
  }
  [[gnu::always_inline]] inline void OpTransferYToA() {
    const bool wide = IsAccumulator16Bit();
    regs_.A = MergeByWidth(regs_.A, regs_.Y, wide);
    SetNzFromWidth(regs_.A, wide);
  }
  [[gnu::always_inline]] inline void OpTransferYToX() {
    const bool wide = IsIndex16Bit();
    regs_.X = MergeByWidth(regs_.X, regs_.Y, wide);
    SetNzFromWidth(regs_.X, wide);
  }
  [[gnu::always_inline]] inline void OpTransferAToD() {
    regs_.DP = regs_.A;
    SetNzFromWidth(regs_.DP, true);
  }
  [[gnu::always_inline]] inline void OpTransferAToS() {
    regs_.SP = regs_.A;
    if (regs_.P.E) {
      regs_.SP = static_cast<uint16_t>(0x0100U | (regs_.SP & 0x00FFU));
    }
  }
  [[gnu::always_inline]] inline void OpTransferDToA() {
    regs_.A = regs_.DP;
    SetNzFromWidth(regs_.A, true);
  }
  [[gnu::always_inline]] inline void OpTransferSToA() {
    regs_.A = regs_.SP;
    SetNzFromWidth(regs_.A, true);
  }
  // Binary ADC helper. BCD/decimal mode is not yet implemented; in D=1 the
  // behavior falls back to binary arithmetic (and v flag is overwritten).
  [[gnu::always_inline]] inline void OpAluAdc8(uint8_t operand) {
    const uint16_t a_lo = static_cast<uint8_t>(regs_.A);
    const uint16_t sum = static_cast<uint16_t>(a_lo + operand + (regs_.P.C ? 1U : 0U));
    const uint8_t result = static_cast<uint8_t>(sum);
    const bool overflow = ((~(a_lo ^ operand) & (a_lo ^ result)) & 0x80U) != 0U;
    regs_.A = static_cast<uint16_t>((regs_.A & 0xFF00U) | result);
    regs_.P.C = (sum & 0x0100U) != 0U;
    regs_.P.V = overflow;
    regs_.P.Z = (result == 0U);
    regs_.P.N = (result & 0x80U) != 0U;
  }
  [[gnu::always_inline]] inline void OpAluAdc16(uint16_t operand) {
    const uint32_t a = regs_.A;
    const uint32_t sum = a + operand + (regs_.P.C ? 1U : 0U);
    const uint16_t result = static_cast<uint16_t>(sum);
    const bool overflow = ((~(a ^ operand) & (a ^ result)) & 0x8000U) != 0U;
    regs_.A = result;
    regs_.P.C = (sum & 0x10000U) != 0U;
    regs_.P.V = overflow;
    regs_.P.Z = (result == 0U);
    regs_.P.N = (result & 0x8000U) != 0U;
  }
  [[gnu::always_inline]] inline void OpAluSbc8(uint8_t operand) {
    // SBC binary: A - operand - (1 - C). Implement as ADC of ~operand.
    OpAluAdc8(static_cast<uint8_t>(~operand));
  }
  [[gnu::always_inline]] inline void OpAluSbc16(uint16_t operand) {
    OpAluAdc16(static_cast<uint16_t>(~operand));
  }
  [[gnu::always_inline]] inline void OpAluAnd8(uint8_t operand) {
    const uint8_t result = static_cast<uint8_t>(regs_.A) & operand;
    regs_.A = static_cast<uint16_t>((regs_.A & 0xFF00U) | result);
    regs_.P.Z = (result == 0U);
    regs_.P.N = (result & 0x80U) != 0U;
  }
  [[gnu::always_inline]] inline void OpAluAnd16(uint16_t operand) {
    regs_.A = static_cast<uint16_t>(regs_.A & operand);
    regs_.P.Z = (regs_.A == 0U);
    regs_.P.N = (regs_.A & 0x8000U) != 0U;
  }
  [[gnu::always_inline]] inline void OpAluOra8(uint8_t operand) {
    const uint8_t result = static_cast<uint8_t>(regs_.A) | operand;
    regs_.A = static_cast<uint16_t>((regs_.A & 0xFF00U) | result);
    regs_.P.Z = (result == 0U);
    regs_.P.N = (result & 0x80U) != 0U;
  }
  [[gnu::always_inline]] inline void OpAluOra16(uint16_t operand) {
    regs_.A = static_cast<uint16_t>(regs_.A | operand);
    regs_.P.Z = (regs_.A == 0U);
    regs_.P.N = (regs_.A & 0x8000U) != 0U;
  }
  [[gnu::always_inline]] inline void OpAluEor8(uint8_t operand) {
    const uint8_t result = static_cast<uint8_t>(regs_.A) ^ operand;
    regs_.A = static_cast<uint16_t>((regs_.A & 0xFF00U) | result);
    regs_.P.Z = (result == 0U);
    regs_.P.N = (result & 0x80U) != 0U;
  }
  [[gnu::always_inline]] inline void OpAluEor16(uint16_t operand) {
    regs_.A = static_cast<uint16_t>(regs_.A ^ operand);
    regs_.P.Z = (regs_.A == 0U);
    regs_.P.N = (regs_.A & 0x8000U) != 0U;
  }
  [[gnu::always_inline]] inline void DoCompare8(uint8_t reg, uint8_t operand) {
    const uint16_t diff = static_cast<uint16_t>(reg) - static_cast<uint16_t>(operand);
    regs_.P.C = reg >= operand;
    regs_.P.Z = (static_cast<uint8_t>(diff) == 0U);
    regs_.P.N = (diff & 0x80U) != 0U;
  }
  [[gnu::always_inline]] inline void DoCompare16(uint16_t reg, uint16_t operand) {
    const uint32_t diff = static_cast<uint32_t>(reg) - static_cast<uint32_t>(operand);
    regs_.P.C = reg >= operand;
    regs_.P.Z = (static_cast<uint16_t>(diff) == 0U);
    regs_.P.N = (diff & 0x8000U) != 0U;
  }
  [[gnu::always_inline]] inline uint16_t AluOperand16FromFetch() const {
    const uint16_t low = static_cast<uint16_t>(addr_ & 0xFFU);
    const uint16_t high = static_cast<uint16_t>(static_cast<uint16_t>(fetch_data_) << 8U);
    return static_cast<uint16_t>(low | high);
  }
  [[gnu::always_inline]] inline void OpBranchRelative16() {
    const int16_t displacement = static_cast<int16_t>(static_cast<uint16_t>(addr_ & 0xFFFFU));
    regs_.PC = static_cast<uint16_t>(regs_.PC + displacement);
  }
  [[gnu::always_inline]] inline void OpSetPcFromAddr() {
    regs_.PC = static_cast<uint16_t>(addr_ & 0xFFFFU);
  }
  [[gnu::always_inline]] inline void OpSetPcAndPbrFromAddr() {
    regs_.PC = static_cast<uint16_t>(addr_ & 0xFFFFU);
    regs_.PBR = static_cast<uint8_t>((addr_ >> 16U) & 0xFFU);
  }
  [[nodiscard]] [[gnu::always_inline]] inline SnesAddrT StackAddr() const { return static_cast<SnesAddrT>(regs_.SP); }
  // Replace byte [shift, shift+7] of addr_ with fetch_data_.
  [[gnu::always_inline]] inline void SetAddrByteFromFetch(unsigned shift) {
    const uint32_t mask = ~(uint32_t{0xFFU} << shift) & 0xFFFFFFU;
    addr_ = (addr_ & mask) | (static_cast<uint32_t>(fetch_data_) << shift);
  }

  // Central dispatch for the per-cycle internal register op. Always inlined
  // so the switch collapses into ExecuteMicroOp's loop body.
  [[gnu::always_inline]] inline void ExecuteInternalOp(MicroInternalOp op) {
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
      case MicroInternalOp::kIncA:
        OpIncA();
        break;
      case MicroInternalOp::kDecA:
        OpDecA();
        break;
      case MicroInternalOp::kIncX:
        OpIncX();
        break;
      case MicroInternalOp::kDecX:
        OpDecX();
        break;
      case MicroInternalOp::kIncY:
        OpIncY();
        break;
      case MicroInternalOp::kDecY:
        OpDecY();
        break;
      case MicroInternalOp::kClearCarry:
        regs_.P.C = false;
        break;
      case MicroInternalOp::kSetCarry:
        regs_.P.C = true;
        break;
      case MicroInternalOp::kClearDecimal:
        regs_.P.D = false;
        break;
      case MicroInternalOp::kSetDecimal:
        regs_.P.D = true;
        break;
      case MicroInternalOp::kClearInterrupt:
        regs_.P.I = false;
        break;
      case MicroInternalOp::kSetInterrupt:
        regs_.P.I = true;
        break;
      case MicroInternalOp::kClearOverflow:
        regs_.P.V = false;
        break;
      case MicroInternalOp::kRepFromFetch:
        OpRepFromFetch();
        break;
      case MicroInternalOp::kSepFromFetch:
        OpSepFromFetch();
        break;
      case MicroInternalOp::kExchangeCarryEmulation:
        OpExchangeCarryEmulation();
        break;
      case MicroInternalOp::kTransferAToX:
        OpTransferAToX();
        break;
      case MicroInternalOp::kTransferAToY:
        OpTransferAToY();
        break;
      case MicroInternalOp::kTransferSToX:
        OpTransferSToX();
        break;
      case MicroInternalOp::kTransferXToA:
        OpTransferXToA();
        break;
      case MicroInternalOp::kTransferXToS:
        OpTransferXToS();
        break;
      case MicroInternalOp::kTransferXToY:
        OpTransferXToY();
        break;
      case MicroInternalOp::kTransferYToA:
        OpTransferYToA();
        break;
      case MicroInternalOp::kTransferYToX:
        OpTransferYToX();
        break;
      case MicroInternalOp::kTransferAToD:
        OpTransferAToD();
        break;
      case MicroInternalOp::kTransferAToS:
        OpTransferAToS();
        break;
      case MicroInternalOp::kTransferDToA:
        OpTransferDToA();
        break;
      case MicroInternalOp::kTransferSToA:
        OpTransferSToA();
        break;
      case MicroInternalOp::kSetBranchTakenIfZero:
        OpSetBranchTaken(regs_.P.Z);
        break;
      case MicroInternalOp::kSetBranchTakenIfCarry:
        OpSetBranchTaken(regs_.P.C);
        break;
      case MicroInternalOp::kSetBranchTakenIfNotCarry:
        OpSetBranchTaken(!regs_.P.C);
        break;
      case MicroInternalOp::kSetBranchTakenIfNegative:
        OpSetBranchTaken(regs_.P.N);
        break;
      case MicroInternalOp::kSetBranchTakenIfNotNegative:
        OpSetBranchTaken(!regs_.P.N);
        break;
      case MicroInternalOp::kSetBranchTakenIfOverflow:
        OpSetBranchTaken(regs_.P.V);
        break;
      case MicroInternalOp::kSetBranchTakenIfNotOverflow:
        OpSetBranchTaken(!regs_.P.V);
        break;
      case MicroInternalOp::kBranchRelative16:
        OpBranchRelative16();
        break;
      case MicroInternalOp::kSetPcFromAddr:
        OpSetPcFromAddr();
        break;
      case MicroInternalOp::kSetPcAndPbrFromAddr:
        OpSetPcAndPbrFromAddr();
        break;
      case MicroInternalOp::kSetAddrHighFromFetchAndSetPc:
        SetAddrByteFromFetch(8);
        OpSetPcFromAddr();
        break;
      case MicroInternalOp::kSetAddrBankFromFetchAndSetPcAndPbr:
        SetAddrByteFromFetch(16);
        OpSetPcAndPbrFromAddr();
        break;
      case MicroInternalOp::kDecrementPc:
        regs_.PC = static_cast<uint16_t>(regs_.PC - 1U);
        break;
      case MicroInternalOp::kIncrementPc:
        regs_.PC = static_cast<uint16_t>(regs_.PC + 1U);
        break;
      case MicroInternalOp::kSetPclFromFetch:
        regs_.PC = static_cast<uint16_t>((regs_.PC & 0xFF00U) | fetch_data_);
        break;
      case MicroInternalOp::kSetPchFromFetch: {
        const uint16_t high = static_cast<uint16_t>(static_cast<uint16_t>(fetch_data_) << 8U);
        regs_.PC = static_cast<uint16_t>((regs_.PC & 0x00FFU) | high);
        break;
      }
      case MicroInternalOp::kSetPbrFromFetch:
        regs_.PBR = fetch_data_;
        break;
      case MicroInternalOp::kLoadPFromFetch:
        regs_.P.FromByte(fetch_data_, regs_.P.E);
        break;
      case MicroInternalOp::kLoadDpLowFromFetch:
        regs_.DP = static_cast<uint16_t>((regs_.DP & 0xFF00U) | fetch_data_);
        break;
      case MicroInternalOp::kLoadDpHighFromFetchUpdateNz: {
        const uint16_t high = static_cast<uint16_t>(static_cast<uint16_t>(fetch_data_) << 8U);
        regs_.DP = static_cast<uint16_t>(high | (regs_.DP & 0x00FFU));
        regs_.P.Z = (regs_.DP == 0U);
        regs_.P.N = (regs_.DP & 0x8000U) != 0U;
        break;
      }
      case MicroInternalOp::kLoadXHighFromFetchUpdateNz:
        OpLoadXHighUpdateNz();
        break;
      case MicroInternalOp::kLoadYHighFromFetchUpdateNz:
        OpLoadYHighUpdateNz();
        break;
      case MicroInternalOp::kAluAdc8FromFetch:
        OpAluAdc8(fetch_data_);
        break;
      case MicroInternalOp::kAluSbc8FromFetch:
        OpAluSbc8(fetch_data_);
        break;
      case MicroInternalOp::kAluAnd8FromFetch:
        OpAluAnd8(fetch_data_);
        break;
      case MicroInternalOp::kAluOra8FromFetch:
        OpAluOra8(fetch_data_);
        break;
      case MicroInternalOp::kAluEor8FromFetch:
        OpAluEor8(fetch_data_);
        break;
      case MicroInternalOp::kAluCmp8FromFetch:
        DoCompare8(static_cast<uint8_t>(regs_.A), fetch_data_);
        break;
      case MicroInternalOp::kAluCpx8FromFetch:
        DoCompare8(static_cast<uint8_t>(regs_.X), fetch_data_);
        break;
      case MicroInternalOp::kAluCpy8FromFetch:
        DoCompare8(static_cast<uint8_t>(regs_.Y), fetch_data_);
        break;
      case MicroInternalOp::kAluBit8ImmFromFetch: {
        const uint8_t result = static_cast<uint8_t>(regs_.A) & fetch_data_;
        regs_.P.Z = (result == 0U);
        break;
      }
      case MicroInternalOp::kAluAdc16FromFetch:
        OpAluAdc16(AluOperand16FromFetch());
        break;
      case MicroInternalOp::kAluSbc16FromFetch:
        OpAluSbc16(AluOperand16FromFetch());
        break;
      case MicroInternalOp::kAluAnd16FromFetch:
        OpAluAnd16(AluOperand16FromFetch());
        break;
      case MicroInternalOp::kAluOra16FromFetch:
        OpAluOra16(AluOperand16FromFetch());
        break;
      case MicroInternalOp::kAluEor16FromFetch:
        OpAluEor16(AluOperand16FromFetch());
        break;
      case MicroInternalOp::kAluCmp16FromFetch:
        DoCompare16(regs_.A, AluOperand16FromFetch());
        break;
      case MicroInternalOp::kAluCpx16FromFetch:
        DoCompare16(regs_.X, AluOperand16FromFetch());
        break;
      case MicroInternalOp::kAluCpy16FromFetch:
        DoCompare16(regs_.Y, AluOperand16FromFetch());
        break;
      case MicroInternalOp::kAluBit16ImmFromFetch: {
        const uint16_t result = static_cast<uint16_t>(regs_.A & AluOperand16FromFetch());
        regs_.P.Z = (result == 0U);
        break;
      }
    }
  }
  void FinishInstruction();
  void DrainSkippedMicroOpsSlow();
  // Fast guard: needs_drain_ is set at instruction-entry time iff the
  // instruction has any conditional rules (rule_count > 1). For the common
  // case of unconditional instructions this is a single-load branch.
  void DrainSkippedMicroOps() {
    if (!needs_drain_) return;
    DrainSkippedMicroOpsSlow();
  }
  void RecordFault(Fault::Type type, uint8_t opcode, SnesAddrT opcode_address);
  [[nodiscard]] bool EvaluateTimingRule(const TimingRuleExpr& rule) const;

  [[nodiscard]] uint8_t ReadResetVectorByte(SnesAddrT addr);

  [[nodiscard]] [[gnu::always_inline]] inline SnesAddrT PcAddr() const {
    return (static_cast<uint32_t>(regs_.PBR) << 16U) | static_cast<uint32_t>(regs_.PC);
  }

  // Issue a Plan/Follow pair against the system bus at local_time_+cycle_time.
  // Returns the follow result; caller interprets outcome.
  [[nodiscard]] BusFollowResult PlanAndFollow(SnesAddrT addr, BusAccessType type, uint8_t data,
                                              TimeMasterDeltaT cycle_time);

  // Slow path for BusRead/BusWrite — handles unmapped pages, MMIO, scheduled
  // accesses, and devices without a fast pointer (e.g. test mocks). Defined
  // out-of-line in cpu.cpp.
  [[nodiscard]] TickResult BusReadSlow(SnesAddrT addr, TimeMasterDeltaT cycle_time);
  [[nodiscard]] TickResult BusWriteSlow(SnesAddrT addr, uint8_t data, TimeMasterDeltaT cycle_time);

  // Execute a bus read. Reason is kContinue on inline completion
  // (fetch_data_ updated); a stop reason when the access blocks.
  [[nodiscard]] TickResult BusRead(SnesAddrT addr, TimeMasterDeltaT cycle_time) {
    uint8_t data;
    if (system_bus_raw_ != nullptr && system_bus_raw_->TryFastRead(addr, data)) {
      fetch_data_ = data;
      return TickResult{0, TickStopReason::kContinue};
    }
    return BusReadSlow(addr, cycle_time);
  }

  // Execute a bus write. Reason is kContinue on inline completion, or a stop
  // reason when the access blocks.
  [[nodiscard]] TickResult BusWrite(SnesAddrT addr, uint8_t data, TimeMasterDeltaT cycle_time) {
    if (system_bus_raw_ != nullptr && system_bus_raw_->TryFastWrite(addr, data)) {
      return TickResult{0, TickStopReason::kContinue};
    }
    return BusWriteSlow(addr, data, cycle_time);
  }
};

}  // namespace pupsnes
