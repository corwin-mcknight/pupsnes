#include <array>
#include <catch2/catch_test_macros.hpp>

#include "pupsnes/5a22/opcode_metadata.h"
#include "pupsnes/debugger/disasm.h"
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
