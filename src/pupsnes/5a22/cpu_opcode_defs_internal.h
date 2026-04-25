#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "pupsnes/5a22/cpu_internal.h"
#include "pupsnes/hw/5a22/cpu.h"

namespace pupsnes::opcode_defs_internal {

namespace micro_op_params {
// Packing helpers for MicroOp::params. Populated in subsequent refactor
// steps as each enum group collapses into a parameterized category.

// Register-to-register transfers (kTransferReg): src in low nibble, dst in high
// nibble. Both nibbles reference the Reg enum defined in cpu.h.
inline constexpr uint8_t PackTransfer(Reg src, Reg dst) {
  return static_cast<uint8_t>(static_cast<uint8_t>(src) | (static_cast<uint8_t>(dst) << 4U));
}
inline constexpr Reg UnpackTransferSrc(uint8_t params) { return static_cast<Reg>(params & 0x0FU); }
inline constexpr Reg UnpackTransferDst(uint8_t params) { return static_cast<Reg>((params >> 4U) & 0x0FU); }

// Inc/dec registers (kIncDecReg): bit 0 = decrement, bits [4:1] = Reg (A/X/Y only).
inline constexpr uint8_t PackIncDec(Reg reg, bool decrement) {
  return static_cast<uint8_t>((decrement ? 1U : 0U) | (static_cast<uint32_t>(reg) << 1U));
}
inline constexpr Reg UnpackIncDecReg(uint8_t params) { return static_cast<Reg>((params >> 1U) & 0x0FU); }
inline constexpr bool UnpackIncDecDecrement(uint8_t params) { return (params & 0x01U) != 0U; }

// Flag set/clear (kSetFlag): bit 0 = value (1 = set, 0 = clear), bits [4:1] = Flag.
// Only C, D, I, V are used by flag-op opcodes; other Flag values are unused.
inline constexpr uint8_t PackSetFlag(Flag flag, bool value) {
  return static_cast<uint8_t>((value ? 1U : 0U) | (static_cast<uint32_t>(flag) << 1U));
}
inline constexpr Flag UnpackSetFlagFlag(uint8_t params) { return static_cast<Flag>((params >> 1U) & 0x0FU); }
inline constexpr bool UnpackSetFlagValue(uint8_t params) { return (params & 0x01U) != 0U; }

// Branch-condition setter (kSetBranchTakenCond): bits [3:0] = BranchCond
// (kAlways, kZ/kNotZ, kC/kNotC, kN/kNotN, kV/kNotV). Dispatch lives in
// the kSetBranchTakenCond case in ExecuteInternalOp in cpu.cpp.
inline constexpr uint8_t PackBranchCond(BranchCond cond) {
  return static_cast<uint8_t>(static_cast<uint32_t>(cond) & 0x0FU);
}
inline constexpr BranchCond UnpackBranchCond(uint8_t params) { return static_cast<BranchCond>(params & 0x0FU); }

// Register load from fetch_data_ (kLoadReg): bits [3:0] = Reg (A/X/Y only),
// bits [5:4] = ByteSel (kLow = 0, kHigh = 1), bit 6 = update_nz, bit 7 =
// post_inc_addr. Dispatch lives in the kLoadReg case in ExecuteInternalOp in
// cpu.cpp. On kHigh, update_nz must be true (that's the only high-byte
// variant the concrete helpers implement); the builder enforces this.
// post_inc_addr advances addr_ by 1 (24-bit wrap) after the register update
// and is used by multi-byte reads from an effective address to set up the
// next byte's bus cycle.
inline constexpr uint8_t PackLoadReg(Reg reg, ByteSel byte_sel, bool update_nz, bool post_inc_addr = false) {
  return static_cast<uint8_t>((static_cast<uint32_t>(reg) & 0x0FU) | ((static_cast<uint32_t>(byte_sel) & 0x03U) << 4U) |
                              ((update_nz ? 1U : 0U) << 6U) | ((post_inc_addr ? 1U : 0U) << 7U));
}
inline constexpr Reg UnpackLoadRegReg(uint8_t params) { return static_cast<Reg>(params & 0x0FU); }
inline constexpr ByteSel UnpackLoadRegByteSel(uint8_t params) { return static_cast<ByteSel>((params >> 4U) & 0x03U); }
inline constexpr bool UnpackLoadRegNz(uint8_t params) { return (params & 0x40U) != 0U; }
inline constexpr bool UnpackLoadRegPostIncAddr(uint8_t params) { return (params & 0x80U) != 0U; }

// Push bus action (kPushStack): bits [3:0] = PushSrc (15 variants — A8/AHigh,
// X8/XHigh, Y8/YHigh, Pcl/Pch/Pbr, Dbr, P, DpLow/DpHigh, AddrLow/AddrHigh).
// Dispatch lives in the kPushStack case in PerformBusAction in cpu.cpp.
inline constexpr uint8_t PackPushStack(PushSrc src) { return static_cast<uint8_t>(static_cast<uint32_t>(src) & 0x0FU); }
inline constexpr PushSrc UnpackPushStack(uint8_t params) { return static_cast<PushSrc>(params & 0x0FU); }

// Write bus action (kWriteRegByte): bits [2:0] = WriteSrc (kFetchData/kA/kX/
// kY/kZero), bit 3 = ByteSel (0 = kLow, 1 = kHigh). For the generic fetch_data
// writer, WriteSrc::kFetchData + ByteSel::kLow is the canonical encoding (the
// ByteSel is ignored for fetch_data and kZero). Dispatch lives in the
// kWriteRegByte case in PerformBusAction in cpu.cpp. Bit 4 is reserved for
// kModifyAddr's decrement flag (shared-slot encoding); keep it clear here.
inline constexpr uint8_t PackWriteAddr(WriteSrc src, ByteSel byte_sel) {
  return static_cast<uint8_t>((static_cast<uint32_t>(src) & 0x07U) | ((static_cast<uint32_t>(byte_sel) & 0x01U) << 3U));
}
inline constexpr WriteSrc UnpackWriteAddrSrc(uint8_t params) { return static_cast<WriteSrc>(params & 0x07U); }
inline constexpr ByteSel UnpackWriteAddrByteSel(uint8_t params) { return static_cast<ByteSel>((params >> 3U) & 0x01U); }

// ALU immediate (kAlu8Imm / kAlu16Imm): bits [3:0] = AluOp (10 variants — Adc,
// Sbc, And, Ora, Eor, Cmp, Cpx, Cpy, Bit, BitMem). Bit 4 (kAlu16Imm only) = low byte
// source: 0 = addr_[7:0] (immediate path, stashed by a prior
// kSetAddrByteFromFetch(kLow)); 1 = addr_scratch_[7:0] (memory path, stashed
// by a prior kStashIndirectLow on the low-byte read).  Dispatch lives in the
// kAlu8Imm / kAlu16Imm cases in ExecuteInternalOp in cpu.cpp. Width selection
// is baked into the enum variant (8 vs 16) because the two paths differ in
// operand plumbing: 8-bit consumes fetch_data_ only; 16-bit consumes the
// low-byte source (addr_ or addr_scratch_) plus fetch_data_ (high).
inline constexpr uint8_t PackAluOp(AluOp op, bool low_from_scratch = false) {
  return static_cast<uint8_t>((static_cast<uint32_t>(op) & 0x0FU) | (low_from_scratch ? 0x10U : 0x00U));
}
inline constexpr AluOp UnpackAluOp(uint8_t params) { return static_cast<AluOp>(params & 0x0FU); }
inline constexpr bool UnpackAluLowFromScratch(uint8_t params) { return (params & 0x10U) != 0U; }

// Set addr byte from fetch (kSetAddrByteFromFetch): bits [1:0] = ByteSel
// (kLow/kHigh/kBank), bits [3:2] = BankSrc. BankSrc is only meaningful with
// byte_sel == kHigh (bank byte is set simultaneously with the high byte). The
// four variants mirror the four ways the bank byte can be populated alongside
// a high-byte PC fetch:
//   kLeave (0) — bank unchanged (used when the bank will be explicitly fetched
//     in a later cycle, e.g. absolute long).
//   kDbr   (1) — bank = DBR (used by absolute for ALU/load/store operands).
//   kPbr   (2) — bank = PBR (used by (abs,X) indirect: pointer lives in the
//     program bank).
//   kZero  (3) — bank = 0 (used by (abs) / [abs] indirect: pointer always in
//     bank 0).
// The old from_dbr overload is retained for call-site compatibility and maps
// to kLeave/kDbr.
inline constexpr uint8_t PackSetAddrByte(ByteSel byte_sel, BankSrc bank_src) {
  return static_cast<uint8_t>((static_cast<uint32_t>(byte_sel) & 0x03U) |
                              ((static_cast<uint32_t>(bank_src) & 0x03U) << 2U));
}
inline constexpr uint8_t PackSetAddrByte(ByteSel byte_sel, bool from_dbr) {
  return PackSetAddrByte(byte_sel, from_dbr ? BankSrc::kDbr : BankSrc::kLeave);
}
inline constexpr ByteSel UnpackSetAddrByteSel(uint8_t params) { return static_cast<ByteSel>(params & 0x03U); }
inline constexpr BankSrc UnpackSetAddrBankSrc(uint8_t params) { return static_cast<BankSrc>((params >> 2U) & 0x03U); }
inline constexpr bool UnpackSetAddrFromDbr(uint8_t params) { return UnpackSetAddrBankSrc(params) == BankSrc::kDbr; }

// Modify addr (kModifyAddr): bit 4 = decrement flag (0 = +1, 1 = -1). Encoded
// so the default (bit 4 = 0) is increment, which is also what results when
// kModifyAddr shares its CycleSlotSpec::params byte with a WriteRegByte in
// the same slot: WriteRegByte's packing occupies bits [3:0], leaving bit 4
// at 0 so the address advances after the write. No ModifyAddr decrement call
// site exists today; the plan reserves the encoding for future use.
inline constexpr uint8_t PackModifyAddr(bool increment) { return static_cast<uint8_t>(increment ? 0U : (1U << 4U)); }
inline constexpr bool UnpackModifyAddrIncrement(uint8_t params) { return (params & 0x10U) == 0U; }

// Modify SP (kModifySp): bit 5 = increment flag (0 = -1, 1 = +1). Encoded so
// the default (bit 5 = 0) is decrement, which is what results when kModifySp
// shares its CycleSlotSpec::params byte with a PushStack in the same slot:
// PushStack's PushSrc packing occupies bits [3:0], leaving bit 5 at 0 so SP
// decrements after the push. Standalone kModifySp increment slots must set
// bit 5.
inline constexpr uint8_t PackModifySp(bool increment) { return static_cast<uint8_t>(increment ? (1U << 5U) : 0U); }
inline constexpr bool UnpackModifySpIncrement(uint8_t params) { return (params & 0x20U) != 0U; }

// Modify PC (kModifyPc): bit 4 = decrement flag (0 = +1, 1 = -1). No sharing
// constraint today — kModifyPc always appears with bus_action = kNone — so
// the default encoding could be either polarity. We mirror kModifyAddr
// (default = increment) for consistency.
inline constexpr uint8_t PackModifyPc(bool increment) { return static_cast<uint8_t>(increment ? 0U : (1U << 4U)); }
inline constexpr bool UnpackModifyPcIncrement(uint8_t params) { return (params & 0x10U) == 0U; }

// Branch relative (kBranchRelative): bit 0 = wide (1 = 16-bit displacement
// from addr_, 0 = signed 8-bit from fetch_data_).
inline constexpr uint8_t PackBranchRelative(bool wide) { return static_cast<uint8_t>(wide ? 1U : 0U); }
inline constexpr bool UnpackBranchRelativeWide(uint8_t params) { return (params & 0x01U) != 0U; }

// Fused kLoadAddrByteAndSetPc: bits [1:0] = ByteSel, bit [2] = with_pbr.
// Used for JMP abs (kHigh, false) and JML (kBank, true).
inline constexpr uint8_t PackLoadAddrByteAndSetPc(ByteSel byte_sel, bool with_pbr) {
  return static_cast<uint8_t>((static_cast<uint32_t>(byte_sel) & 0x03U) | ((with_pbr ? 1U : 0U) << 2U));
}
inline constexpr ByteSel UnpackLoadAddrByteAndSetPcSel(uint8_t params) { return static_cast<ByteSel>(params & 0x03U); }
inline constexpr bool UnpackLoadAddrByteAndSetPcWithPbr(uint8_t params) { return (params & 0x04U) != 0U; }

// Add index to addr (kAddIndexToAddr): bits [3:0] = Reg (kX or kY), bit [4] =
// bank_wrap. When bank_wrap is 1 (default), addr_ is masked to 16 bits after
// the add (bank forced to 0) — used by direct-page-indexed addressing. When 0,
// the add is 24-bit and carry can propagate into the bank byte — used by
// absolute-indexed addressing where the effective address is DBR:(abs + idx).
inline constexpr uint8_t PackAddIndex(Reg reg, bool bank_wrap = true) {
  return static_cast<uint8_t>((static_cast<uint32_t>(reg) & 0x0FU) | (bank_wrap ? 0x10U : 0x00U));
}
inline constexpr Reg UnpackAddIndex(uint8_t params) { return static_cast<Reg>(params & 0x0FU); }
inline constexpr bool UnpackAddIndexBankWrap(uint8_t params) { return (params & 0x10U) != 0U; }

// Form addr from scratch (kFormAddrFromScratchBank): bit 0 = with_y_add. When
// set, after assembling addr_ = bank:(scratch high:low), Y is added with
// 24-bit carry into the bank byte. Used by [dp],Y addressing to fold the
// index add into the bank-fetch cycle (matches Bruce Clark's "7-m+w" count).
inline constexpr uint8_t PackFormAddrFromScratchBank(bool with_y_add = false) {
  return static_cast<uint8_t>(with_y_add ? 0x01U : 0x00U);
}
inline constexpr bool UnpackFormAddrFromScratchBankWithYAdd(uint8_t params) { return (params & 0x01U) != 0U; }

// Mask status (kMaskStatus): bit 0 = or_bits (1 = SEP/OR, 0 = REP/AND-NOT).
// Both paths preserve E-mode forcing of M/X back to 1.
inline constexpr uint8_t PackMaskStatus(bool or_bits) { return static_cast<uint8_t>(or_bits ? 1U : 0U); }
inline constexpr bool UnpackMaskStatusOr(uint8_t params) { return (params & 0x01U) != 0U; }

// Memory RMW (kRmwMem): bits [2:0] = RmwOp (kAsl/kLsr/kRol/kRor/kInc/kDec).
// Width follows regs_.P.M at runtime — the op dispatches 8-bit vs 16-bit
// dynamically rather than through a packed width bit. See kRmwMem dispatch in
// ExecuteInternalOp in cpu.cpp.
inline constexpr uint8_t PackRmw(RmwOp op) { return static_cast<uint8_t>(static_cast<uint32_t>(op) & 0x07U); }
inline constexpr RmwOp UnpackRmwOp(uint8_t params) { return static_cast<RmwOp>(params & 0x07U); }

// Halt CPU (kHaltCpu): bit 0 = is_stp (1 = STP, 0 = WAI). The two map to
// HaltState::kStp / HaltState::kWai; STP is only cleared by Reset while WAI
// wakes on any interrupt pin assertion.
inline constexpr uint8_t PackHaltCpu(bool is_stp) { return static_cast<uint8_t>(is_stp ? 1U : 0U); }

// Interrupt vector selection (kSetInterruptVector): bits [2:0] carry the
// InterruptKind enum. BRK=0, COP=1, NMI=2, IRQ=3, ABORT=4.
inline constexpr uint8_t PackSetInterruptVector(InterruptKind kind) {
  return static_cast<uint8_t>(static_cast<uint32_t>(kind) & 0x07U);
}
}  // namespace micro_op_params

struct CycleSlotSpec {
  MicroBusAction bus_action = MicroBusAction::kNone;
  MicroInternalOp internal_op = MicroInternalOp::kNone;
  TimingRuleExpr rule{};
  std::string_view label{};
  uint8_t params = 0;
};

struct CycleFragment {
  uint8_t cycle_count = 0;
  bool overflowed = false;
  std::array<CycleSlotSpec, kMaxRemainingOps> cycles{};
};

enum class OpcodeSpecDisposition : uint8_t {
  kImplemented = 0,
  kUnimplemented = 1,
};

struct OpcodeSpec {
  uint8_t opcode = 0;
  OpcodeSpecDisposition disposition = OpcodeSpecDisposition::kUnimplemented;
  std::string_view mnemonic = "???";
  std::string_view addressing_mode{};
  uint8_t cycle_count = 0;
  bool overflowed = false;
  std::array<CycleSlotSpec, kMaxRemainingOps> cycles{};
};

struct OpcodeMetadata {
  bool implemented = false;
  std::string_view mnemonic = "???";
  std::string_view addressing_mode{};
  uint8_t cycle_count = 0;
  std::array<std::string_view, kMaxRemainingOps> cycle_labels{};
};

struct OpcodeArtifacts {
  std::array<InstructionEntry, 256> execution_table{};
  std::array<OpcodeMetadata, 256> metadata_table{};
};

constexpr TimingRuleExpr Always() {
  TimingRuleExpr expr{};
  expr.node_count = 1;
  expr.root_index = 0;
  expr.nodes[0].op = TimingRuleOp::kAlways;
  return expr;
}

constexpr TimingRuleExpr Condition(TimingCondition condition) {
  TimingRuleExpr expr{};
  expr.node_count = 1;
  expr.root_index = 0;
  expr.nodes[0].op = TimingRuleOp::kCondition;
  expr.nodes[0].condition = condition;
  return expr;
}

constexpr TimingRuleNode ShiftTimingRuleNode(const TimingRuleNode& node, uint8_t offset) {
  TimingRuleNode shifted = node;
  switch (node.op) {
    case TimingRuleOp::kNot: shifted.lhs = static_cast<uint8_t>(node.lhs + offset); break;
    case TimingRuleOp::kAllOf:
    case TimingRuleOp::kAnyOf:
      shifted.lhs = static_cast<uint8_t>(node.lhs + offset);
      shifted.rhs = static_cast<uint8_t>(node.rhs + offset);
      break;
    case TimingRuleOp::kAlways:
    case TimingRuleOp::kCondition: break;
  }
  return shifted;
}

constexpr TimingRuleExpr Not(const TimingRuleExpr& expr) {
  TimingRuleExpr out{};
  out.node_count = static_cast<uint8_t>(expr.node_count + 1U);
  out.root_index = expr.node_count;

  const uint8_t copy_count = (expr.node_count < kMaxTimingRuleNodes) ? expr.node_count : kMaxTimingRuleNodes;
  for (uint8_t i = 0; i < copy_count; ++i) {
    out.nodes[i] = expr.nodes[i];
  }
  if (out.root_index < kMaxTimingRuleNodes) {
    out.nodes[out.root_index].op = TimingRuleOp::kNot;
    out.nodes[out.root_index].lhs = expr.root_index;
  }
  return out;
}

constexpr TimingRuleExpr MergeRules(TimingRuleOp op, const TimingRuleExpr& lhs, const TimingRuleExpr& rhs) {
  TimingRuleExpr out{};
  const uint8_t rhs_offset = lhs.node_count;
  out.node_count = static_cast<uint8_t>(lhs.node_count + rhs.node_count + 1U);
  out.root_index = static_cast<uint8_t>(lhs.node_count + rhs.node_count);

  for (uint8_t i = 0; i < lhs.node_count && i < kMaxTimingRuleNodes; ++i) {
    out.nodes[i] = lhs.nodes[i];
  }
  for (uint8_t i = 0; i < rhs.node_count && static_cast<uint8_t>(rhs_offset + i) < kMaxTimingRuleNodes; ++i) {
    out.nodes[rhs_offset + i] = ShiftTimingRuleNode(rhs.nodes[i], rhs_offset);
  }
  if (out.root_index < kMaxTimingRuleNodes) {
    out.nodes[out.root_index].op = op;
    out.nodes[out.root_index].lhs = lhs.root_index;
    out.nodes[out.root_index].rhs = static_cast<uint8_t>(rhs.root_index + rhs_offset);
  }
  return out;
}

constexpr TimingRuleExpr AllOf(const TimingRuleExpr& lhs, const TimingRuleExpr& rhs) {
  return MergeRules(TimingRuleOp::kAllOf, lhs, rhs);
}

constexpr TimingRuleExpr AnyOf(const TimingRuleExpr& lhs, const TimingRuleExpr& rhs) {
  return MergeRules(TimingRuleOp::kAnyOf, lhs, rhs);
}

constexpr CycleSlotSpec FetchPc(MicroInternalOp internal_op = MicroInternalOp::kNone, TimingRuleExpr rule = Always(),
                                std::string_view label = {}) {
  return CycleSlotSpec{MicroBusAction::kFetchPc, internal_op, rule, label};
}

constexpr CycleSlotSpec ReadAddr(MicroInternalOp internal_op = MicroInternalOp::kNone, TimingRuleExpr rule = Always(),
                                 std::string_view label = {}) {
  return CycleSlotSpec{MicroBusAction::kReadAddr, internal_op, rule, label};
}

// Parameterized write of a register (or fetch_data_) byte to the effective
// address (addr_). Packs (WriteSrc, ByteSel) into CycleSlotSpec::params for
// the unified kWriteRegByte bus action. Dispatch lives in the kWriteRegByte case in PerformBusAction
// in cpu.cpp. For the generic fetch_data writer, pass
// WriteSrc::kFetchData + ByteSel::kLow (ByteSel is ignored in that case).
constexpr CycleSlotSpec WriteRegByte(WriteSrc src, ByteSel byte_sel,
                                     MicroInternalOp internal_op = MicroInternalOp::kNone,
                                     TimingRuleExpr rule = Always(), std::string_view label = {}) {
  return CycleSlotSpec{
      MicroBusAction::kWriteRegByte, internal_op, rule, label, micro_op_params::PackWriteAddr(src, byte_sel),
  };
}

// Parameterized push to the stack. Packs PushSrc into CycleSlotSpec::params
// for the unified kPushStack bus action. Dispatch lives in the kPushStack case in PerformBusAction
// in cpu.cpp. internal_op defaults to kNone so callers can opt in (typically
// kModifySp with decrement=0 to sequence SP after the write) at the emit site.
constexpr CycleSlotSpec PushReg(PushSrc src, MicroInternalOp internal_op = MicroInternalOp::kNone,
                                TimingRuleExpr rule = Always(), std::string_view label = {}) {
  return CycleSlotSpec{
      MicroBusAction::kPushStack, internal_op, rule, label, micro_op_params::PackPushStack(src),
  };
}

constexpr CycleSlotSpec PullStack(MicroInternalOp internal_op = MicroInternalOp::kNone, TimingRuleExpr rule = Always(),
                                  std::string_view label = {}) {
  return CycleSlotSpec{MicroBusAction::kPullStack, internal_op, rule, label};
}

constexpr CycleSlotSpec Internal(MicroInternalOp internal_op = MicroInternalOp::kNone, TimingRuleExpr rule = Always(),
                                 std::string_view label = {}) {
  return CycleSlotSpec{MicroBusAction::kNone, internal_op, rule, label};
}

// Shorthand for the common idle-cycle slot: kNone bus action, kNone internal op,
// always-fires, "internal" label. Appears 27× across the opcode definitions.
constexpr CycleSlotSpec Idle(std::string_view label = "internal") {
  return CycleSlotSpec{MicroBusAction::kNone, MicroInternalOp::kNone, Always(), label};
}

// Parameterized register-to-register transfer. Packs (src, dst) into
// CycleSlotSpec::params for the unified kTransferReg internal op. Width and
// flag semantics of each concrete pair live in the kTransferReg case in ExecuteInternalOp in cpu.cpp.
constexpr CycleSlotSpec TransferReg(Reg src, Reg dst, TimingRuleExpr rule = Always(), std::string_view label = {}) {
  return CycleSlotSpec{
      MicroBusAction::kNone, MicroInternalOp::kTransferReg, rule, label, micro_op_params::PackTransfer(src, dst),
  };
}

// Parameterized register inc/dec. Packs (reg, decrement) into
// CycleSlotSpec::params for the unified kIncDecReg internal op. Width and flag
// semantics for each register live in the kIncDecReg case in ExecuteInternalOp in cpu.cpp.
constexpr CycleSlotSpec IncDecReg(Reg reg, bool decrement, TimingRuleExpr rule = Always(),
                                  std::string_view label = {}) {
  return CycleSlotSpec{
      MicroBusAction::kNone, MicroInternalOp::kIncDecReg, rule, label, micro_op_params::PackIncDec(reg, decrement),
  };
}

// Parameterized flag set/clear. Packs (flag, value) into CycleSlotSpec::params
// for the unified kSetFlag internal op. Dispatch lives in the kSetFlag case in ExecuteInternalOp in
// cpu.cpp. Only C, D, I, V are emitted by real opcodes (CLC/SEC/CLI/SEI/CLV/
// CLD/SED).
constexpr CycleSlotSpec SetFlag(Flag flag, bool value, TimingRuleExpr rule = Always(), std::string_view label = {}) {
  return CycleSlotSpec{
      MicroBusAction::kNone, MicroInternalOp::kSetFlag, rule, label, micro_op_params::PackSetFlag(flag, value),
  };
}

// Parameterized load of a register byte from fetch_data_ (FetchPc bus action).
// Packs (reg, byte_sel, update_nz) into CycleSlotSpec::params for the unified
// kLoadReg internal op. Dispatch lives in the kLoadReg case in ExecuteInternalOp in cpu.cpp. Used by
// LDA/LDX/LDY immediate and any other opcode that completes a register via a
// PC-side fetch.
constexpr CycleSlotSpec LoadRegFromFetch(Reg reg, ByteSel byte_sel, bool update_nz, TimingRuleExpr rule = Always(),
                                         std::string_view label = {}) {
  return CycleSlotSpec{
      MicroBusAction::kFetchPc,
      MicroInternalOp::kLoadReg,
      rule,
      label,
      micro_op_params::PackLoadReg(reg, byte_sel, update_nz),
  };
}

// Parameterized load of a register byte from a pre-incremented stack pull
// (kPreIncPullStack bus action). Same param encoding as LoadRegFromFetch;
// used by PLA/PLX/PLY.
constexpr CycleSlotSpec PullPreIncLoadReg(Reg reg, ByteSel byte_sel, bool update_nz, TimingRuleExpr rule = Always(),
                                          std::string_view label = {}) {
  return CycleSlotSpec{
      MicroBusAction::kPreIncPullStack,
      MicroInternalOp::kLoadReg,
      rule,
      label,
      micro_op_params::PackLoadReg(reg, byte_sel, update_nz),
  };
}

// Parameterized 8-bit ALU immediate. Emits a kFetchPc bus action paired with
// the unified kAlu8Imm internal op; packs AluOp into CycleSlotSpec::params for
// the kAlu8Imm case in ExecuteInternalOp in cpu.cpp. Used by ADC/SBC/AND/ORA/EOR/CMP/CPX/CPY/BIT
// immediate when the controlling flag (M for A, X for X/Y-compares) is 1.
constexpr CycleSlotSpec AluImm8(AluOp op, TimingRuleExpr rule = Always(), std::string_view label = {}) {
  return CycleSlotSpec{
      MicroBusAction::kFetchPc, MicroInternalOp::kAlu8Imm, rule, label, micro_op_params::PackAluOp(op),
  };
}

// Parameterized 16-bit ALU immediate. Emits a kFetchPc bus action paired with
// the unified kAlu16Imm internal op; packs AluOp into CycleSlotSpec::params for
// the kAlu16Imm case in ExecuteInternalOp in cpu.cpp. Consumes addr_[7:0] as the operand low byte
// (stashed by a prior kSetAddrLowFromFetch) and fetch_data_ as the high byte.
constexpr CycleSlotSpec AluImm16(AluOp op, TimingRuleExpr rule = Always(), std::string_view label = {}) {
  return CycleSlotSpec{
      MicroBusAction::kFetchPc, MicroInternalOp::kAlu16Imm, rule, label, micro_op_params::PackAluOp(op),
  };
}

// Parameterized "set addr byte from fetch". Wraps the kSetAddrByteFromFetch
// internal op. byte_sel chooses which byte of addr_ is replaced; from_dbr is
// meaningful only when byte_sel == kHigh and triggers the old
// kSetAddrHighFromFetchAndBankFromDbr behavior (high byte from fetch + bank
// from DBR).
constexpr CycleSlotSpec SetAddrByte(ByteSel byte_sel, bool from_dbr = false, TimingRuleExpr rule = Always(),
                                    std::string_view label = {}) {
  return CycleSlotSpec{
      MicroBusAction::kNone,
      MicroInternalOp::kSetAddrByteFromFetch,
      rule,
      label,
      micro_op_params::PackSetAddrByte(byte_sel, from_dbr),
  };
}

// Parameterized kModifyAddr. Defaults to increment; decrement is reserved.
constexpr CycleSlotSpec ModifyAddr(bool increment = true, TimingRuleExpr rule = Always(), std::string_view label = {}) {
  return CycleSlotSpec{
      MicroBusAction::kNone, MicroInternalOp::kModifyAddr, rule, label, micro_op_params::PackModifyAddr(increment),
  };
}

// Parameterized kModifySp. increment = true for pull-path SP advance,
// false for push-path SP retreat.
constexpr CycleSlotSpec ModifySp(bool increment, TimingRuleExpr rule = Always(), std::string_view label = {}) {
  return CycleSlotSpec{
      MicroBusAction::kNone, MicroInternalOp::kModifySp, rule, label, micro_op_params::PackModifySp(increment),
  };
}

// Parameterized kModifyPc. Defaults to increment.
constexpr CycleSlotSpec ModifyPc(bool increment = true, TimingRuleExpr rule = Always(), std::string_view label = {}) {
  return CycleSlotSpec{
      MicroBusAction::kNone, MicroInternalOp::kModifyPc, rule, label, micro_op_params::PackModifyPc(increment),
  };
}

// Parameterized branch-relative apply. wide = true selects the 16-bit
// displacement stashed in addr_[15:0] (BRL); wide = false applies the signed
// 8-bit displacement in fetch_data_ and tracks branch_page_crossed.
constexpr CycleSlotSpec BranchRelative(bool wide, TimingRuleExpr rule = Always(), std::string_view label = {}) {
  return CycleSlotSpec{
      MicroBusAction::kNone, MicroInternalOp::kBranchRelative, rule, label, micro_op_params::PackBranchRelative(wide),
  };
}

// Parameterized fused "set addr byte from fetch + set PC from addr". Used by
// JMP absolute (byte_sel = kHigh, with_pbr = false) and JML absolute long
// (byte_sel = kBank, with_pbr = true). Emits a kFetchPc bus action paired
// with the kLoadAddrByteAndSetPc internal op.
constexpr CycleSlotSpec LoadAddrByteAndSetPc(ByteSel byte_sel, bool with_pbr, TimingRuleExpr rule = Always(),
                                             std::string_view label = {}) {
  return CycleSlotSpec{
      MicroBusAction::kFetchPc,
      MicroInternalOp::kLoadAddrByteAndSetPc,
      rule,
      label,
      micro_op_params::PackLoadAddrByteAndSetPc(byte_sel, with_pbr),
  };
}

// Parameterized REP/SEP (kMaskStatus). or_bits = true for SEP (P |= fetch),
// false for REP (P &= ~fetch). Both paths preserve E-mode forcing of M/X.
constexpr CycleSlotSpec MaskStatus(bool or_bits, TimingRuleExpr rule = Always(), std::string_view label = {}) {
  return CycleSlotSpec{
      MicroBusAction::kNone, MicroInternalOp::kMaskStatus, rule, label, micro_op_params::PackMaskStatus(or_bits),
  };
}

// Parameterized branch-displacement fetch. Reads the signed 8-bit displacement
// from PBR:PC (advancing PC) and evaluates the branch condition in the same
// cycle, setting timing_context_.branch_taken via the kSetBranchTakenCond case in ExecuteInternalOp. Packs the
// BranchCond into CycleSlotSpec::params for the unified kSetBranchTakenCond
// internal op. Used by every conditional branch opcode via BranchSequence;
// BRL has its own long-form sequence.
constexpr CycleSlotSpec FetchPcBranchTest(BranchCond cond, std::string_view label = "fetch displacement") {
  return CycleSlotSpec{
      MicroBusAction::kFetchPc,
      MicroInternalOp::kSetBranchTakenCond,
      Always(),
      label,
      micro_op_params::PackBranchCond(cond),
  };
}

struct CycleFragmentBuilder {
  CycleFragment fragment{};

  constexpr CycleFragmentBuilder& Then(const CycleSlotSpec& cycle) {
    if (fragment.cycle_count >= kMaxRemainingOps) {
      fragment.overflowed = true;
      return *this;
    }
    fragment.cycles[fragment.cycle_count++] = cycle;
    return *this;
  }

  constexpr CycleFragment Build() const { return fragment; }
};

constexpr CycleFragmentBuilder Fragment() { return CycleFragmentBuilder{}; }

struct OpcodeSpecBuilder {
  OpcodeSpec spec{};

  constexpr OpcodeSpecBuilder(uint8_t opcode, std::string_view mnemonic, std::string_view addressing_mode) {
    spec.opcode = opcode;
    spec.disposition = OpcodeSpecDisposition::kImplemented;
    spec.mnemonic = mnemonic;
    spec.addressing_mode = addressing_mode;
  }

  constexpr OpcodeSpecBuilder& Then(const CycleSlotSpec& cycle) {
    if (spec.cycle_count >= kMaxRemainingOps) {
      spec.overflowed = true;
      return *this;
    }
    spec.cycles[spec.cycle_count++] = cycle;
    return *this;
  }

  constexpr OpcodeSpecBuilder& Then(const CycleFragment& fragment) {
    spec.overflowed = spec.overflowed || fragment.overflowed;
    for (uint8_t i = 0; i < fragment.cycle_count; ++i) {
      Then(fragment.cycles[i]);
    }
    return *this;
  }

  constexpr OpcodeSpec Build() const { return spec; }
};

constexpr OpcodeSpecBuilder Opcode(uint8_t opcode, std::string_view mnemonic, std::string_view addressing_mode) {
  return OpcodeSpecBuilder(opcode, mnemonic, addressing_mode);
}

template <typename T, std::size_t N, std::size_t M>
constexpr auto ConcatArrays(const std::array<T, N>& lhs, const std::array<T, M>& rhs) {
  std::array<T, N + M> out{};
  for (std::size_t i = 0; i < N; ++i) {
    out[i] = lhs[i];
  }
  for (std::size_t i = 0; i < M; ++i) {
    out[N + i] = rhs[i];
  }
  return out;
}

// Variadic flatten over std::array. Folds left so that a single call
// `ConcatAll(a, b, c, d, ...)` replaces a tree of nested ConcatArrays.
template <typename A>
constexpr auto ConcatAll(const A& a) {
  return a;
}
template <typename A, typename B, typename... Rest>
constexpr auto ConcatAll(const A& a, const B& b, const Rest&... rest) {
  return ConcatAll(ConcatArrays(a, b), rest...);
}

constexpr bool ValidateTimingRule(const TimingRuleExpr& rule) {
  if (rule.node_count == 0) {
    return true;
  }
  if (rule.node_count > kMaxTimingRuleNodes || rule.root_index >= rule.node_count) {
    return false;
  }

  for (uint8_t i = 0; i < rule.node_count; ++i) {
    const TimingRuleNode& node = rule.nodes[i];
    switch (node.op) {
      case TimingRuleOp::kAlways:
      case TimingRuleOp::kCondition: break;
      case TimingRuleOp::kNot:
        if (node.lhs >= i) {
          return false;
        }
        break;
      case TimingRuleOp::kAllOf:
      case TimingRuleOp::kAnyOf:
        if (node.lhs >= i || node.rhs >= i) {
          return false;
        }
        break;
    }
  }
  return true;
}

constexpr bool ValidateOpcodeSpec(const OpcodeSpec& spec) {
  if (spec.disposition != OpcodeSpecDisposition::kImplemented) {
    return spec.cycle_count == 0 && !spec.overflowed;
  }
  if (spec.cycle_count == 0 || spec.cycle_count > kMaxRemainingOps || spec.overflowed) {
    return false;
  }
  for (uint8_t i = 0; i < spec.cycle_count; ++i) {
    if (!ValidateTimingRule(spec.cycles[i].rule)) {
      return false;
    }
  }
  return true;
}

template <std::size_t N>
constexpr bool ValidateUniqueOpcodes(const std::array<OpcodeSpec, N>& specs) {
  std::array<bool, 256> seen{};
  for (const OpcodeSpec& spec : specs) {
    if (seen[spec.opcode]) {
      return false;
    }
    seen[spec.opcode] = true;
  }
  return true;
}

constexpr bool EvaluateRuleForBits(const TimingRuleExpr& rule, uint32_t bits) {
  if (rule.node_count == 0) {
    return true;
  }
  std::array<bool, kMaxTimingRuleNodes> values{};
  for (uint8_t i = 0; i < rule.node_count; ++i) {
    const TimingRuleNode& node = rule.nodes[i];
    switch (node.op) {
      case TimingRuleOp::kAlways: values[i] = true; break;
      case TimingRuleOp::kCondition: values[i] = ((bits >> static_cast<uint8_t>(node.condition)) & 1U) != 0U; break;
      case TimingRuleOp::kNot: values[i] = !values[node.lhs]; break;
      case TimingRuleOp::kAllOf: values[i] = values[node.lhs] && values[node.rhs]; break;
      case TimingRuleOp::kAnyOf: values[i] = values[node.lhs] || values[node.rhs]; break;
    }
  }
  return values[rule.root_index];
}

constexpr uint32_t ComputeTimingRuleTruthTable(const TimingRuleExpr& rule) {
  uint32_t table = 0;
  constexpr uint32_t kCombinations = 1U << kTimingConditionCount;
  for (uint32_t bits = 0; bits < kCombinations; ++bits) {
    if (EvaluateRuleForBits(rule, bits)) {
      table |= (1U << bits);
    }
  }
  return table;
}

constexpr uint8_t CountUniqueRules(const OpcodeSpec& spec) {
  std::array<uint32_t, kMaxInstructionRules> unique_tables{};
  uint8_t count = 1;
  unique_tables[0] = ComputeTimingRuleTruthTable(Always());

  for (uint8_t i = 0; i < spec.cycle_count; ++i) {
    const uint32_t table = ComputeTimingRuleTruthTable(spec.cycles[i].rule);
    bool found = false;
    for (uint8_t j = 0; j < count; ++j) {
      if (unique_tables[j] == table) {
        found = true;
        break;
      }
    }
    if (found) {
      continue;
    }
    if (count >= kMaxInstructionRules) {
      return static_cast<uint8_t>(kMaxInstructionRules + 1U);
    }
    unique_tables[count++] = table;
  }

  return count;
}

template <std::size_t N>
constexpr bool ValidateOpcodeSpecs(const std::array<OpcodeSpec, N>& specs) {
  if (!ValidateUniqueOpcodes(specs)) {
    return false;
  }
  for (const OpcodeSpec& spec : specs) {
    if (!ValidateOpcodeSpec(spec)) {
      return false;
    }
    if (CountUniqueRules(spec) > kMaxInstructionRules) {
      return false;
    }
  }
  return true;
}

constexpr uint8_t FindRuleIndex(const InstructionEntry& entry, uint32_t truth_table) {
  for (uint8_t i = 0; i < entry.rule_count; ++i) {
    if (entry.rules[i] == truth_table) {
      return i;
    }
  }
  return entry.rule_count;
}

constexpr bool AddressingModeUsesDpPenalty(std::string_view mode) {
  return mode == "direct page" || mode == "direct page indexed X" || mode == "direct page indexed Y";
}

constexpr InstructionEntry LowerOpcode(const OpcodeSpec& spec) {
  InstructionEntry entry{};
  if (spec.disposition != OpcodeSpecDisposition::kImplemented) {
    return entry;
  }

  entry.disposition = InstructionDisposition::kImplemented;
  entry.remaining_op_count = spec.cycle_count;
  entry.uses_dp_penalty = AddressingModeUsesDpPenalty(spec.addressing_mode);
  entry.rule_count = 1;
  entry.rules[0] = ComputeTimingRuleTruthTable(Always());

  for (uint8_t i = 0; i < spec.cycle_count; ++i) {
    const CycleSlotSpec& slot = spec.cycles[i];
    const uint32_t table = ComputeTimingRuleTruthTable(slot.rule);
    uint8_t rule_index = FindRuleIndex(entry, table);
    if (rule_index == entry.rule_count) {
      entry.rules[entry.rule_count++] = table;
    }
    entry.ops[i] = MicroOp{slot.bus_action, slot.internal_op, slot.params, rule_index};
  }

  return entry;
}

constexpr OpcodeMetadata LowerMetadata(const OpcodeSpec& spec) {
  OpcodeMetadata metadata{};
  metadata.implemented = (spec.disposition == OpcodeSpecDisposition::kImplemented);
  metadata.mnemonic = spec.mnemonic;
  metadata.addressing_mode = spec.addressing_mode;
  metadata.cycle_count = spec.cycle_count;
  for (uint8_t i = 0; i < spec.cycle_count; ++i) {
    metadata.cycle_labels[i] = spec.cycles[i].label;
  }
  return metadata;
}

template <std::size_t N>
consteval OpcodeArtifacts BuildOpcodeArtifacts(const std::array<OpcodeSpec, N>& specs) noexcept {
  OpcodeArtifacts artifacts{};

  for (OpcodeMetadata& metadata : artifacts.metadata_table) {
    metadata = OpcodeMetadata{};
  }
  for (InstructionEntry& entry : artifacts.execution_table) {
    entry = InstructionEntry{};
  }

  for (const OpcodeSpec& spec : specs) {
    artifacts.execution_table[spec.opcode] = LowerOpcode(spec);
    artifacts.metadata_table[spec.opcode] = LowerMetadata(spec);
  }

  return artifacts;
}

extern const OpcodeArtifacts kOpcodeArtifacts;

// Synthetic HW interrupt entries (NMI / IRQ / ABORT). Not in the opcode
// table; dispatched by CPU::TickToTarget when the instruction-boundary
// interrupt sampler decides to deliver. Returns nullptr for SW kinds
// (BRK / COP) which have their own entries in the opcode table.
const InstructionEntry* HwInterruptEntryFor(InterruptKind kind);

}  // namespace pupsnes::opcode_defs_internal
