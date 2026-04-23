#include "pupsnes/debugger/disasm.h"

#include <format>
#include <string>

#include "debugger/ui_utils.h"

namespace pupsnes::debugger {

namespace {

SnesAddrT WrapAddress(SnesAddrT address) { return address & 0x00FFFFFFU; }

std::string FormatOperand(const OpcodeMetadataView& metadata, SnesAddrT pc, uint8_t length,
                          const std::array<uint8_t, 4>& bytes) {
  switch (metadata.addressing_mode) {
    case OpcodeAddressingMode::kImplied:
    case OpcodeAddressingMode::kUnknown: return "";
    case OpcodeAddressingMode::kImmediateAccumulator:
    case OpcodeAddressingMode::kImmediateIndex:
      if (length >= 3) {
        return std::format("#${:04X}", static_cast<unsigned>(bytes[1] | (static_cast<uint16_t>(bytes[2]) << 8U)));
      }
      return std::format("#${:02X}", static_cast<unsigned>(bytes[1]));
    case OpcodeAddressingMode::kImmediateByte: return std::format("#${:02X}", static_cast<unsigned>(bytes[1]));
    case OpcodeAddressingMode::kAbsolute:
      return std::format("${:04X}", static_cast<unsigned>(bytes[1] | (static_cast<uint16_t>(bytes[2]) << 8U)));
    case OpcodeAddressingMode::kAbsoluteLong:
      return std::format("${:02X}:{:04X}", static_cast<unsigned>(bytes[3]),
                         static_cast<unsigned>(bytes[1] | (static_cast<uint16_t>(bytes[2]) << 8U)));
    case OpcodeAddressingMode::kRelative8: {
      const int8_t displacement = static_cast<int8_t>(bytes[1]);
      const uint16_t next_pc = static_cast<uint16_t>((pc + 2U) & 0xFFFFU);
      const uint16_t target = static_cast<uint16_t>(next_pc + displacement);
      return std::format("${:04X}", static_cast<unsigned>(target));
    }
    case OpcodeAddressingMode::kRelative16: {
      const int16_t displacement =
          static_cast<int16_t>(static_cast<uint16_t>(bytes[1] | (static_cast<uint16_t>(bytes[2]) << 8U)));
      const uint16_t next_pc = static_cast<uint16_t>((pc + 3U) & 0xFFFFU);
      const uint16_t target = static_cast<uint16_t>(next_pc + displacement);
      return std::format("${:04X}", static_cast<unsigned>(target));
    }
    case OpcodeAddressingMode::kDirectPage: return std::format("${:02X}", static_cast<unsigned>(bytes[1]));
    case OpcodeAddressingMode::kDirectPageIndexedX: return std::format("${:02X},X", static_cast<unsigned>(bytes[1]));
    case OpcodeAddressingMode::kDirectPageIndexedY: return std::format("${:02X},Y", static_cast<unsigned>(bytes[1]));
    case OpcodeAddressingMode::kStackRelative: return std::format("${:02X},S", static_cast<unsigned>(bytes[1]));
    case OpcodeAddressingMode::kAbsoluteIndexedX:
      return std::format("${:04X},X", static_cast<unsigned>(bytes[1] | (static_cast<uint16_t>(bytes[2]) << 8U)));
    case OpcodeAddressingMode::kAbsoluteIndexedY:
      return std::format("${:04X},Y", static_cast<unsigned>(bytes[1] | (static_cast<uint16_t>(bytes[2]) << 8U)));
    case OpcodeAddressingMode::kDirectIndirect: return std::format("(${:02X})", static_cast<unsigned>(bytes[1]));
    case OpcodeAddressingMode::kDirectIndirectLong: return std::format("[${:02X}]", static_cast<unsigned>(bytes[1]));
    case OpcodeAddressingMode::kDirectIndirectIndexedY:
      return std::format("(${:02X}),Y", static_cast<unsigned>(bytes[1]));
    case OpcodeAddressingMode::kDirectIndirectLongIndexedY:
      return std::format("[${:02X}],Y", static_cast<unsigned>(bytes[1]));
    case OpcodeAddressingMode::kDirectIndexedIndirectX:
      return std::format("(${:02X},X)", static_cast<unsigned>(bytes[1]));
    case OpcodeAddressingMode::kAbsoluteLongIndexedX:
      return std::format("${:02X}:{:04X},X", static_cast<unsigned>(bytes[3]),
                         static_cast<unsigned>(bytes[1] | (static_cast<uint16_t>(bytes[2]) << 8U)));
    case OpcodeAddressingMode::kAbsoluteIndirect:
      return std::format("(${:04X})", static_cast<unsigned>(bytes[1] | (static_cast<uint16_t>(bytes[2]) << 8U)));
    case OpcodeAddressingMode::kAbsoluteIndirectLong:
      return std::format("[${:04X}]", static_cast<unsigned>(bytes[1] | (static_cast<uint16_t>(bytes[2]) << 8U)));
    case OpcodeAddressingMode::kAbsoluteIndexedIndirectX:
      return std::format("(${:04X},X)", static_cast<unsigned>(bytes[1] | (static_cast<uint16_t>(bytes[2]) << 8U)));
    case OpcodeAddressingMode::kStackRelativeIndirectIndexedY:
      return std::format("(${:02X},S),Y", static_cast<unsigned>(bytes[1]));
    case OpcodeAddressingMode::kBlockMove:
      return std::format("#${:02X},#${:02X}", static_cast<unsigned>(bytes[2]), static_cast<unsigned>(bytes[1]));
  }
  return "";
}

}  // namespace

DisassembledInstruction DisassembleInstructionRaw(const SNES& snes, SnesAddrT pc, const CpuFlags& flags) {
  DisassembledInstruction out{};
  out.pc = WrapAddress(pc);

  const DebugReadResult opcode_read = snes.GetSystemBus().DebugRead(out.pc);
  out.complete = opcode_read.ok;
  out.opcode = opcode_read.ok ? opcode_read.value : 0xFFU;
  out.bytes[0] = out.opcode;
  out.byte_count = 1;

  const OpcodeMetadataView& metadata = GetOpcodeMetadata(out.opcode);
  out.length = ComputeInstructionLength(metadata, flags);

  for (uint8_t i = 1; i < out.length && i < out.bytes.size(); ++i) {
    const DebugReadResult operand = snes.GetSystemBus().DebugRead(WrapAddress(out.pc + i));
    if (!operand.ok) {
      out.complete = false;
      break;
    }
    out.bytes[i] = operand.value;
    out.byte_count = i + 1U;
  }

  return out;
}

std::string FormatDisassembly(const DisassembledInstruction& out) {
  const OpcodeMetadataView& metadata = GetOpcodeMetadata(out.opcode);
  if (metadata.implementation_status == OpcodeImplementationStatus::kUnimplemented) {
    return std::format("??? ; {}", FormatAddress24(out.pc));
  }

  std::string text(metadata.mnemonic);
  const std::string operand_text = FormatOperand(metadata, out.pc, out.length, out.bytes);
  if (!operand_text.empty()) {
    text += " ";
    text += operand_text;
  }
  if (!out.complete) {
    text += " ; debug-read-failed";
  }
  return text;
}

DisassembledInstruction DisassembleInstruction(const SNES& snes, SnesAddrT pc, const CpuFlags& flags) {
  DisassembledInstruction out = DisassembleInstructionRaw(snes, pc, flags);
  out.text = FormatDisassembly(out);
  return out;
}

}  // namespace pupsnes::debugger
