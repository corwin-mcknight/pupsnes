#include "pupsnes/hw/5a22/cpu.h"

#include <array>
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
// Opcode table
// All 256 opcodes default to NOP timing: 1 remaining internal cycle.
// Group-init helpers populate specific opcodes by instruction family.
// ---------------------------------------------------------------------------

namespace {

void InitMiscOpcodes(std::array<InstructionEntry, 256>& table) noexcept {
    // 0xEA  NOP  — 2 cycles; explicit for clarity (same as default).
    table[0xEA].remaining_op_count = 1;
    table[0xEA].ops[0] = {MicroBusAction::kNone, MicroInternalOp::kNone};

    // 0x80  BRA rel8  — 3 cycles.
    // Remaining ops: fetch signed displacement, then apply it to the already incremented PC.
    table[0x80].remaining_op_count = 2;
    table[0x80].ops[0] = {MicroBusAction::kFetchPc, MicroInternalOp::kNone};
    table[0x80].ops[1] = {MicroBusAction::kNone, MicroInternalOp::kBranchRelative8};

    // 0x8F  STA long  — 5 cycles in the subset we currently model.
    // Remaining ops: fetch address lo/hi/bank, then write A low to the resolved address.
    table[0x8F].remaining_op_count = 4;
    table[0x8F].ops[0] = {MicroBusAction::kFetchPc, MicroInternalOp::kSetAddrLowFromFetch};
    table[0x8F].ops[1] = {MicroBusAction::kFetchPc, MicroInternalOp::kSetAddrHighFromFetch};
    table[0x8F].ops[2] = {MicroBusAction::kFetchPc, MicroInternalOp::kSetAddrBankFromFetch};
    table[0x8F].ops[3] = {MicroBusAction::kWriteA8Addr, MicroInternalOp::kNone};
}

void InitLoadOpcodes(std::array<InstructionEntry, 256>& table) noexcept {
    // 0xA9  LDA #imm  — 2 cycles (8-bit accumulator mode).
    // Remaining op: fetch immediate byte and load into A, update N/Z.
    table[0xA9].remaining_op_count = 1;
    table[0xA9].ops[0] = {MicroBusAction::kFetchPc, MicroInternalOp::kLoadALowUpdateNz};
}

}  // namespace

const std::array<InstructionEntry, 256> CPU::kOpcodeTable = []() noexcept {
    std::array<InstructionEntry, 256> table{};

    // Default: 2-cycle instruction (opcode fetch + 1 internal cycle, no bus action).
    for (auto& entry : table) {
        entry.remaining_op_count = 1;
        entry.ops[0] = {MicroBusAction::kNone, MicroInternalOp::kNone};
    }

    InitMiscOpcodes(table);
    InitLoadOpcodes(table);

    return table;
}();

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
    current_instr_ = nullptr;
    local_time_ = (snes_ != nullptr) ? snes_->GetMasterTime() : 0;

    const uint8_t vector_lo = ReadResetVectorByte(0x00FFFCU);
    const uint8_t vector_hi = ReadResetVectorByte(0x00FFFDU);

    regs_.PBR = 0;
    regs_.PC = static_cast<uint16_t>(static_cast<uint16_t>(vector_hi) << 8U) | vector_lo;
}

void CPU::OpLoadALowUpdateNz() {
    regs_.A = static_cast<uint16_t>((regs_.A & 0xFF00U) | fetch_data_);
    regs_.P.Z = (static_cast<uint8_t>(regs_.A) == 0U);
    regs_.P.N = (regs_.A & 0x0080U) != 0U;
}

void CPU::OpBranchRelative8() {
    const int8_t displacement = static_cast<int8_t>(fetch_data_);
    regs_.PC = static_cast<uint16_t>(regs_.PC + displacement);
}

void CPU::OpSetAddrLowFromFetch() { addr_ = (addr_ & 0xFFFF00U) | static_cast<uint32_t>(fetch_data_); }

void CPU::OpSetAddrHighFromFetch() { addr_ = (addr_ & 0xFF00FFU) | (static_cast<uint32_t>(fetch_data_) << 8U); }

void CPU::OpSetAddrBankFromFetch() { addr_ = (addr_ & 0x00FFFFU) | (static_cast<uint32_t>(fetch_data_) << 16U); }

void CPU::ExecuteInternalOp(MicroInternalOp op) {
    switch (op) {
        case MicroInternalOp::kNone:
            break;
        case MicroInternalOp::kLoadALowUpdateNz:
            OpLoadALowUpdateNz();
            break;
        case MicroInternalOp::kBranchRelative8:
            OpBranchRelative8();
            break;
        case MicroInternalOp::kSetAddrLowFromFetch:
            OpSetAddrLowFromFetch();
            break;
        case MicroInternalOp::kSetAddrHighFromFetch:
            OpSetAddrHighFromFetch();
            break;
        case MicroInternalOp::kSetAddrBankFromFetch:
            OpSetAddrBankFromFetch();
            break;
    }
}

uint8_t CPU::ReadResetVectorByte(SnesAddrT addr) {
    if (snes_ == nullptr || snes_->system_bus == nullptr) {
        return 0xFFU;
    }

    auto plan = snes_->system_bus->Plan(addr, BusAccessType::kRead);
    auto result = snes_->system_bus->Follow(plan, local_time_, device_id_);
    if (result.outcome == BusPlanOutcome::kScheduledComplete) {
        throw std::logic_error("CPU reset vector fetch cannot block on asynchronous bus access");
    }

    return result.data;
}

std::optional<TickResult> CPU::BusRead(SnesAddrT addr, TimeMasterDeltaT cycle_time) {
    auto plan = snes_->system_bus->Plan(addr, BusAccessType::kRead);
    auto result = snes_->system_bus->Follow(plan, local_time_ + cycle_time, device_id_);

    if (result.outcome == BusPlanOutcome::kScheduledComplete) {
        return TickResult{cycle_time, TickStopReason::kBlockedOnToken, result.token};
    }

    fetch_data_ = result.data;
    return std::nullopt;
}

std::optional<TickResult> CPU::BusWrite(SnesAddrT addr, uint8_t data, TimeMasterDeltaT cycle_time) {
    auto plan = snes_->system_bus->Plan(addr, BusAccessType::kWrite, data);
    auto result = snes_->system_bus->Follow(plan, local_time_ + cycle_time, device_id_);

    if (result.outcome == BusPlanOutcome::kScheduledComplete) {
        return TickResult{cycle_time, TickStopReason::kBlockedOnToken, result.token};
    }

    return std::nullopt;
}

SnesAddrT CPU::PcAddr() const { return (static_cast<uint32_t>(regs_.PBR) << 16U) | static_cast<uint32_t>(regs_.PC); }

TickResult CPU::Tick(TimeMasterDeltaT budget) {
    TimeMasterDeltaT cycle_time = 0;

    while (cycle_time < budget) {
        if (micro_op_index_ == 0) {
            // Cycle 0 of every instruction: fetch opcode from PBR:PC.
            if (auto blocked = BusRead(PcAddr(), cycle_time)) {
                return *blocked;
            }
            regs_.PC = static_cast<uint16_t>(regs_.PC + 1U);

            current_instr_ = &kOpcodeTable[fetch_data_];
            micro_op_index_ = 1;
        } else {
            // Cycles 1..N: execute the opcode's remaining micro-ops.
            const uint8_t op_idx = static_cast<uint8_t>(micro_op_index_ - 1U);

            if (current_instr_ == nullptr || op_idx >= current_instr_->remaining_op_count) {
                current_instr_ = nullptr;
                micro_op_index_ = 0;
                continue;
            }

            const MicroOp& mop = current_instr_->ops[op_idx];

            switch (mop.bus_action) {
                case MicroBusAction::kFetchPc: {
                    if (auto blocked = BusRead(PcAddr(), cycle_time)) {
                        return *blocked;
                    }
                    regs_.PC = static_cast<uint16_t>(regs_.PC + 1U);
                    break;
                }
                case MicroBusAction::kReadAddr: {
                    if (auto blocked = BusRead(addr_, cycle_time)) {
                        return *blocked;
                    }
                    break;
                }
                case MicroBusAction::kWriteAddr: {
                    if (auto blocked = BusWrite(addr_, fetch_data_, cycle_time)) {
                        return *blocked;
                    }
                    break;
                }
                case MicroBusAction::kWriteA8Addr: {
                    if (auto blocked = BusWrite(addr_, static_cast<uint8_t>(regs_.A), cycle_time)) {
                        return *blocked;
                    }
                    break;
                }
                case MicroBusAction::kNone:
                    break;
            }

            ExecuteInternalOp(mop.internal_op);
            micro_op_index_++;
        }

        cycle_time++;
    }

    return {cycle_time, TickStopReason::kBudgetExhausted};
}

void CPU::OnEvent(const SchedulerEvent& /*event*/) {
    // CommitComplete/WakeSample do not currently require CPU-side mutation.
    // The scheduler wakes blocked CPU runs by replacing the authoritative Run wake.
}

}  // namespace pupsnes
