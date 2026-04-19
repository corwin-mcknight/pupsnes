#pragma once

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
  kIncDecReg,                           // reg += 1 or reg -= 1; width/flag semantics per reg; params
                                        // packs decrement flag in bit 0 and Reg (A/X/Y) in bits [4:1]
                                        // (see micro_op_params::PackIncDec)
  kSetFlag,                             // P.<flag> = value; params packs value in bit 0 and Flag
                                        // (C/D/I/V only) in bits [4:1] (see micro_op_params::PackSetFlag)
  kRepFromFetch,                        // P &= ~fetch_data_ (E=1 forces M,X back to 1)
  kSepFromFetch,                        // P |= fetch_data_ (E=1 forces M,X to 1)
  kExchangeCarryEmulation,              // swap C and E; on E=1 force M,X=1, XH/YH=0, SH=$01
  kTransferReg,                         // dst = src; width/flag semantics per (src,dst) pair; params
                                        // packs src in [3:0] and dst in [7:4] (see micro_op_params::PackTransfer)
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

// Typed enums for MicroOp::params packing. Populated in subsequent refactor
// steps as each enum group collapses into a parameterized category. For now
// they exist alongside MicroInternalOp; no opcode uses them yet.
enum class Reg : uint8_t { kA, kX, kY, kSp, kDp, kDbr, kP, kPcl, kPch, kPbr };
enum class Width : uint8_t { kByMFlag, kByXFlag, kForce8, kForce16 };
enum class ByteSel : uint8_t { kLow, kHigh, kBank };
enum class Flag : uint8_t { kC, kD, kI, kV, kZ, kN, kM, kX };
enum class BranchCond : uint8_t { kAlways, kZ, kNotZ, kC, kNotC, kN, kNotN, kV, kNotV };
enum class AluOp : uint8_t { kAdc, kSbc, kAnd, kOra, kEor, kCmp, kCpx, kCpy, kBit };
enum class WriteSrc : uint8_t { kFetchData, kA, kX, kY };
enum class PushSrc : uint8_t {
  kA,
  kX,
  kY,
  kPcl,
  kPch,
  kPbr,
  kDbr,
  kP,
  kDpLow,
  kDpHigh,
  kAddrLow,
  kAddrHigh,
};

// Maximum micro-ops remaining after the opcode fetch (longest 65C816
// instruction = 7 cycles). Used by MicroOpRecord-based debugger observers;
// the matching InstructionEntry::ops size lives in cpu_internal.h.
inline constexpr uint8_t kMaxRemainingOps = 7;

// Forward declaration only — the InstructionEntry layout lives in
// src/pupsnes/5a22/cpu_internal.h. CPU::current_instr_ holds a pointer to
// entries in the opcode table defined alongside cpu.cpp; no caller of cpu.h
// needs the complete type.
struct InstructionEntry;

enum class MicroOpStatus : uint8_t {
  kPending = 0,
  kExecuted = 1,
  kSkipped = 2,
};

struct MicroOpRecord {
  uint8_t index = 0;  // 0 = opcode fetch, 1..N = remaining ops (1-based)
  MicroBusAction bus_action = MicroBusAction::kNone;
  MicroInternalOp internal_op = MicroInternalOp::kNone;
  uint8_t params = 0;
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

  struct StepResult {
    bool consumed_cycle = false;
    TickResult stop{0, TickStopReason::kContinue};
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

  [[nodiscard]] bool ShouldFetchInstruction() const { return micro_op_index_ == 0; }

  // Fetch/execute pipeline. All definitions in cpu.cpp.
  [[nodiscard]] StepResult FetchOpcode(TimeMasterDeltaT cycle_time);
  [[nodiscard]] StepResult ExecuteMicroOp(TimeMasterDeltaT cycle_time);
  [[nodiscard]] TickResult PerformBusAction(MicroBusAction action, [[maybe_unused]] uint8_t params,
                                            TimeMasterDeltaT cycle_time);

  // Internal-op dispatch. The switch and every op body live in cpu.cpp; we
  // keep only the declaration here so that adding a new MicroInternalOp
  // variant does not trigger a recompile of every TU that includes cpu.h.
  void ExecuteInternalOp(MicroInternalOp op, [[maybe_unused]] uint8_t params);

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

  [[nodiscard]] bool EvaluateTimingRule(uint32_t truth_table) const;

  [[nodiscard]] uint8_t ReadResetVectorByte(SnesAddrT addr);

  // Issue a Plan/Follow pair against the system bus at local_time_+cycle_time.
  // Returns the follow result; caller interprets outcome.
  [[nodiscard]] BusFollowResult PlanAndFollow(SnesAddrT addr, BusAccessType type, uint8_t data,
                                              TimeMasterDeltaT cycle_time);

  // Bus read/write. Fast-path hits system_bus_raw_->TryFastRead/Write; slow
  // path handles unmapped pages, MMIO, scheduled accesses, and test mocks.
  [[nodiscard]] TickResult BusRead(SnesAddrT addr, TimeMasterDeltaT cycle_time);
  [[nodiscard]] TickResult BusWrite(SnesAddrT addr, uint8_t data, TimeMasterDeltaT cycle_time);
  [[nodiscard]] TickResult BusReadSlow(SnesAddrT addr, TimeMasterDeltaT cycle_time);
  [[nodiscard]] TickResult BusWriteSlow(SnesAddrT addr, uint8_t data, TimeMasterDeltaT cycle_time);
};

}  // namespace pupsnes
