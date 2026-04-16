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

uint8_t CpuFlags::toByte() const {
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

void CpuFlags::fromByte(uint8_t p, bool emulation_mode) {
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

void initMiscOpcodes(std::array<InstructionEntry, 256>& table) noexcept {
    // 0xEA  NOP  — 2 cycles; explicit for clarity (same as default).
    table[0xEA].remaining_op_count = 1;
    table[0xEA].ops[0] = {MicroBusAction::None, MicroInternalOp::None};

    // 0x80  BRA rel8  — 3 cycles.
    // Remaining ops: fetch signed displacement, then apply it to the already incremented PC.
    table[0x80].remaining_op_count = 2;
    table[0x80].ops[0] = {MicroBusAction::FetchPC, MicroInternalOp::None};
    table[0x80].ops[1] = {MicroBusAction::None, MicroInternalOp::BranchRelative8};

    // 0x8F  STA long  — 5 cycles in the subset we currently model.
    // Remaining ops: fetch address lo/hi/bank, then write A low to the resolved address.
    table[0x8F].remaining_op_count = 4;
    table[0x8F].ops[0] = {MicroBusAction::FetchPC, MicroInternalOp::SetAddrLowFromFetch};
    table[0x8F].ops[1] = {MicroBusAction::FetchPC, MicroInternalOp::SetAddrHighFromFetch};
    table[0x8F].ops[2] = {MicroBusAction::FetchPC, MicroInternalOp::SetAddrBankFromFetch};
    table[0x8F].ops[3] = {MicroBusAction::WriteA8Addr, MicroInternalOp::None};
}

void initLoadOpcodes(std::array<InstructionEntry, 256>& table) noexcept {
    // 0xA9  LDA #imm  — 2 cycles (8-bit accumulator mode).
    // Remaining op: fetch immediate byte and load into A, update N/Z.
    table[0xA9].remaining_op_count = 1;
    table[0xA9].ops[0] = {MicroBusAction::FetchPC, MicroInternalOp::LoadALow_UpdateNZ};
}

}  // namespace

const std::array<InstructionEntry, 256> CPU::kOpcodeTable = []() noexcept {
    std::array<InstructionEntry, 256> table{};

    // Default: 2-cycle instruction (opcode fetch + 1 internal cycle, no bus action).
    for (auto& entry : table) {
        entry.remaining_op_count = 1;
        entry.ops[0] = {MicroBusAction::None, MicroInternalOp::None};
    }

    initMiscOpcodes(table);
    initLoadOpcodes(table);

    return table;
}();

// ---------------------------------------------------------------------------
// CPU
// ---------------------------------------------------------------------------

CPU::CPU(SNES* snes) : Device(snes) {}

void CPU::reset() {
    regs_ = Regs{};
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
    local_time = (snes != nullptr) ? snes->getMasterTime() : 0;

    const uint8_t vector_lo = readResetVectorByte(0x00FFFCU);
    const uint8_t vector_hi = readResetVectorByte(0x00FFFDU);

    regs_.PBR = 0;
    regs_.PC = static_cast<uint16_t>(static_cast<uint16_t>(vector_hi) << 8U) | vector_lo;
}

void CPU::opLoadALow_UpdateNZ() {
    regs_.A = static_cast<uint16_t>((regs_.A & 0xFF00U) | fetch_data_);
    regs_.P.Z = (static_cast<uint8_t>(regs_.A) == 0U);
    regs_.P.N = (regs_.A & 0x0080U) != 0U;
}

void CPU::opBranchRelative8() {
    const int8_t displacement = static_cast<int8_t>(fetch_data_);
    regs_.PC = static_cast<uint16_t>(regs_.PC + displacement);
}

void CPU::opSetAddrLowFromFetch() { addr_ = (addr_ & 0xFFFF00U) | static_cast<uint32_t>(fetch_data_); }

void CPU::opSetAddrHighFromFetch() { addr_ = (addr_ & 0xFF00FFU) | (static_cast<uint32_t>(fetch_data_) << 8U); }

void CPU::opSetAddrBankFromFetch() { addr_ = (addr_ & 0x00FFFFU) | (static_cast<uint32_t>(fetch_data_) << 16U); }

void CPU::executeInternalOp(MicroInternalOp op) {
    switch (op) {
        case MicroInternalOp::None:
            break;
        case MicroInternalOp::LoadALow_UpdateNZ:
            opLoadALow_UpdateNZ();
            break;
        case MicroInternalOp::BranchRelative8:
            opBranchRelative8();
            break;
        case MicroInternalOp::SetAddrLowFromFetch:
            opSetAddrLowFromFetch();
            break;
        case MicroInternalOp::SetAddrHighFromFetch:
            opSetAddrHighFromFetch();
            break;
        case MicroInternalOp::SetAddrBankFromFetch:
            opSetAddrBankFromFetch();
            break;
    }
}

uint8_t CPU::readResetVectorByte(snes_addr_t addr) {
    if (snes == nullptr || snes->system_bus == nullptr) {
        return 0xFFU;
    }

    auto plan = snes->system_bus->plan(addr, BusAccessType::Read);
    auto result = snes->system_bus->follow(plan, local_time, device_id_);
    if (result.outcome == BusPlanOutcome::ScheduledComplete) {
        throw std::logic_error("CPU reset vector fetch cannot block on asynchronous bus access");
    }

    return result.data;
}

std::optional<TickResult> CPU::busRead(snes_addr_t addr, time_master_delta_t cycle_time) {
    auto plan = snes->system_bus->plan(addr, BusAccessType::Read);
    auto result = snes->system_bus->follow(plan, local_time + cycle_time, device_id_);

    if (result.outcome == BusPlanOutcome::ScheduledComplete) {
        return TickResult{cycle_time, TickStopReason::BlockedOnToken, result.token};
    }

    fetch_data_ = result.data;
    return std::nullopt;
}

std::optional<TickResult> CPU::busWrite(snes_addr_t addr, uint8_t data, time_master_delta_t cycle_time) {
    auto plan = snes->system_bus->plan(addr, BusAccessType::Write, data);
    auto result = snes->system_bus->follow(plan, local_time + cycle_time, device_id_);

    if (result.outcome == BusPlanOutcome::ScheduledComplete) {
        return TickResult{cycle_time, TickStopReason::BlockedOnToken, result.token};
    }

    return std::nullopt;
}

snes_addr_t CPU::pcAddr() const { return (static_cast<uint32_t>(regs_.PBR) << 16U) | static_cast<uint32_t>(regs_.PC); }

TickResult CPU::tick(time_master_delta_t budget) {
    time_master_delta_t cycle_time = 0;

    while (cycle_time < budget) {
        if (micro_op_index_ == 0) {
            // Cycle 0 of every instruction: fetch opcode from PBR:PC.
            if (auto blocked = busRead(pcAddr(), cycle_time)) {
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
                case MicroBusAction::FetchPC: {
                    if (auto blocked = busRead(pcAddr(), cycle_time)) {
                        return *blocked;
                    }
                    regs_.PC = static_cast<uint16_t>(regs_.PC + 1U);
                    break;
                }
                case MicroBusAction::ReadAddr: {
                    if (auto blocked = busRead(addr_, cycle_time)) {
                        return *blocked;
                    }
                    break;
                }
                case MicroBusAction::WriteAddr: {
                    if (auto blocked = busWrite(addr_, fetch_data_, cycle_time)) {
                        return *blocked;
                    }
                    break;
                }
                case MicroBusAction::WriteA8Addr: {
                    if (auto blocked = busWrite(addr_, static_cast<uint8_t>(regs_.A), cycle_time)) {
                        return *blocked;
                    }
                    break;
                }
                case MicroBusAction::None:
                    break;
            }

            executeInternalOp(mop.internal_op);
            micro_op_index_++;
        }

        cycle_time++;
    }

    return {cycle_time, TickStopReason::BudgetExhausted};
}

void CPU::onEvent(const SchedulerEvent& /*event*/) {
    // CommitComplete/WakeSample do not currently require CPU-side mutation.
    // The scheduler wakes blocked CPU runs by replacing the authoritative Run wake.
}

}  // namespace pupsnes
