#include <array>
#include <catch2/catch_test_macros.hpp>

#include "pupsnes/debugger/disasm.h"
#include "pupsnes/hw/5a22/opcode_metadata.h"
#include "pupsnes/hw/cartridge.h"
#include "pupsnes/hw/snes.h"

using namespace pupsnes;            // NOLINT(google-build-using-namespace)
using namespace pupsnes::debugger;  // NOLINT(google-build-using-namespace)

namespace {

std::array<uint8_t, Cartridge::kLoROMWindowSize> MakeRom() {
  std::array<uint8_t, Cartridge::kLoROMWindowSize> rom{};
  rom.fill(0xEA);
  rom[0x0000] = 0xA9;
  rom[0x0001] = 0x34;
  rom[0x0002] = 0x12;
  rom[0x0003] = 0xA2;
  rom[0x0004] = 0x78;
  rom[0x0005] = 0x56;
  rom[0x0006] = 0x8F;
  rom[0x0007] = 0x00;
  rom[0x0008] = 0x20;
  rom[0x0009] = 0x7E;
  rom[0x7FFC] = 0x00;
  rom[0x7FFD] = 0x80;
  return rom;
}

}  // namespace

TEST_CASE("Public opcode metadata exposes stable mnemonic and length information", "[unit][debugger]") {
  const auto& table = GetOpcodeMetadataTable();
  CpuFlags emulation{};
  emulation.E = true;
  emulation.M = true;
  emulation.X = true;

  CpuFlags native_16 = emulation;
  native_16.E = false;
  native_16.M = false;
  native_16.X = false;

  for (std::size_t opcode = 0; opcode < 256; ++opcode) {
    const auto opcode_u8 = static_cast<uint8_t>(opcode);
    const auto& metadata = GetOpcodeMetadata(opcode_u8);
    REQUIRE(metadata.mnemonic == table[opcode].mnemonic);
    REQUIRE(ComputeInstructionLength(metadata, emulation) >= 1);
    REQUIRE(ComputeInstructionLength(metadata, native_16) >= 1);
    REQUIRE(ComputeInstructionLength(metadata, native_16) <= 4);
  }

  REQUIRE(GetOpcodeMetadata(0xEA).mnemonic == "NOP");
  REQUIRE(ComputeInstructionLength(GetOpcodeMetadata(0xEA), emulation) == 1);
  REQUIRE(GetOpcodeMetadata(0x8F).mnemonic == "STA");
  REQUIRE(ComputeInstructionLength(GetOpcodeMetadata(0x8F), emulation) == 4);
  REQUIRE(ComputeInstructionLength(GetOpcodeMetadata(0xA9), emulation) == 2);
  REQUIRE(ComputeInstructionLength(GetOpcodeMetadata(0xA9), native_16) == 3);
  REQUIRE(ComputeInstructionLength(GetOpcodeMetadata(0xA2), emulation) == 2);
  REQUIRE(ComputeInstructionLength(GetOpcodeMetadata(0xA2), native_16) == 3);
}

TEST_CASE("Disassembler formats width-dependent and long instructions", "[unit][debugger]") {
  SNES snes;
  const auto rom = MakeRom();
  snes.LoadLoRom(rom);
  snes.Reset();

  CpuFlags accumulator_16 = snes.GetCpu().GetRegs().P;
  accumulator_16.E = false;
  accumulator_16.M = false;

  CpuFlags index_16 = accumulator_16;
  index_16.X = false;

  const DisassembledInstruction lda = DisassembleInstruction(snes, 0x008000, accumulator_16);
  REQUIRE(lda.text == "LDA #$1234");
  REQUIRE(lda.length == 3);

  const DisassembledInstruction ldx = DisassembleInstruction(snes, 0x008003, index_16);
  REQUIRE(ldx.text == "LDX #$5678");
  REQUIRE(ldx.length == 3);

  const DisassembledInstruction sta = DisassembleInstruction(snes, 0x008006, accumulator_16);
  REQUIRE(sta.text == "STA $7E:2000");
  REQUIRE(sta.length == 4);
}

namespace {

DisassembledInstruction MakeRel8(SnesAddrT pc, uint8_t opcode, uint8_t disp) {
  DisassembledInstruction out{};
  out.pc = pc;
  out.opcode = opcode;
  out.length = 2;
  out.byte_count = 2;
  out.bytes[0] = opcode;
  out.bytes[1] = disp;
  out.complete = true;
  return out;
}

DisassembledInstruction MakeRel16(SnesAddrT pc, uint8_t opcode, uint16_t disp) {
  DisassembledInstruction out{};
  out.pc = pc;
  out.opcode = opcode;
  out.length = 3;
  out.byte_count = 3;
  out.bytes[0] = opcode;
  out.bytes[1] = static_cast<uint8_t>(disp & 0xFFU);
  out.bytes[2] = static_cast<uint8_t>((disp >> 8U) & 0xFFU);
  out.complete = true;
  return out;
}

}  // namespace

TEST_CASE("GetBranchTarget returns nullopt for non-branch opcodes", "[unit][debugger]") {
  DisassembledInstruction nop{};
  nop.pc = 0x008000;
  nop.opcode = 0xEA;
  nop.length = 1;
  nop.byte_count = 1;
  nop.bytes[0] = 0xEA;
  nop.complete = true;
  REQUIRE_FALSE(GetBranchTarget(nop).has_value());

  // PER (0x62) also uses Relative16 but is not a branch.
  DisassembledInstruction per = MakeRel16(0x008000, 0x62, 0x0010);
  REQUIRE_FALSE(GetBranchTarget(per).has_value());
}

TEST_CASE("GetBranchTarget computes forward rel8 branch target", "[unit][debugger]") {
  // BEQ +$05 from $00:8000 -> next PC = $8002, target = $8007
  const auto instr = MakeRel8(0x008000, 0xF0, 0x05);
  const auto target = GetBranchTarget(instr);
  REQUIRE(target.has_value());
  REQUIRE(*target == 0x008007U);
}

TEST_CASE("GetBranchTarget computes backward rel8 branch target", "[unit][debugger]") {
  // BNE -$03 from $00:8010 -> next PC = $8012, disp = 0xFD (-3), target = $800F
  const auto instr = MakeRel8(0x008010, 0xD0, 0xFD);
  const auto target = GetBranchTarget(instr);
  REQUIRE(target.has_value());
  REQUIRE(*target == 0x00800FU);
}

TEST_CASE("GetBranchTarget covers all rel8 branch opcodes", "[unit][debugger]") {
  const std::array<uint8_t, 9> ops{0x10, 0x30, 0x50, 0x70, 0x80, 0x90, 0xB0, 0xD0, 0xF0};
  for (const uint8_t op : ops) {
    const auto instr = MakeRel8(0x008100, op, 0x00);
    const auto target = GetBranchTarget(instr);
    REQUIRE(target.has_value());
    REQUIRE(*target == 0x008102U);
  }
}

TEST_CASE("GetBranchTarget computes rel16 BRL target with backward jump", "[unit][debugger]") {
  // BRL -$0010 from $00:8100 -> next PC = $8103, disp = 0xFFF0 (-16), target = $80F3
  const auto instr = MakeRel16(0x008100, 0x82, 0xFFF0);
  const auto target = GetBranchTarget(instr);
  REQUIRE(target.has_value());
  REQUIRE(*target == 0x0080F3U);
}

TEST_CASE("GetBranchTarget stays within source bank", "[unit][debugger]") {
  // BEQ +$00 from $7E:0000 should target $7E:0002 (bank preserved).
  const auto instr = MakeRel8(0x7E0000, 0xF0, 0x00);
  const auto target = GetBranchTarget(instr);
  REQUIRE(target.has_value());
  REQUIRE(*target == 0x7E0002U);
}

TEST_CASE("GetBranchTarget wraps within bank on rel8 underflow", "[unit][debugger]") {
  // BRA -$10 from $00:8005 -> next PC = $8007, disp = 0xF0 (-16), target = $7FF7 (same bank).
  const auto instr = MakeRel8(0x008005, 0x80, 0xF0);
  const auto target = GetBranchTarget(instr);
  REQUIRE(target.has_value());
  REQUIRE(*target == 0x007FF7U);
}

TEST_CASE("GetBranchTarget returns nullopt for incomplete instruction", "[unit][debugger]") {
  auto instr = MakeRel8(0x008000, 0xF0, 0x05);
  instr.complete = false;
  REQUIRE_FALSE(GetBranchTarget(instr).has_value());
}
