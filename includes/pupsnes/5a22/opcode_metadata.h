#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "pupsnes/hw/5a22/cpu.h"

namespace pupsnes {

enum class OpcodeAddressingMode : uint8_t {
  kUnknown = 0,
  kImplied = 1,
  kImmediateAccumulator = 2,
  kImmediateIndex = 3,
  kAbsolute = 4,
  kAbsoluteLong = 5,
  kRelative8 = 6,
  kImmediateByte = 7,
  kRelative16 = 8,
  kDirectPage = 9,
  kDirectPageIndexedX = 10,
  kDirectPageIndexedY = 11,
  kStackRelative = 12,
  kAbsoluteIndexedX = 13,
  kAbsoluteIndexedY = 14,
  kDirectIndirect = 15,
  kDirectIndirectLong = 16,
  kAbsoluteLongIndexedX = 17,
  kDirectIndirectIndexedY = 18,
  kDirectIndirectLongIndexedY = 19,
  kDirectIndexedIndirectX = 20,
  kAbsoluteIndirect = 21,
  kAbsoluteIndirectLong = 22,
  kAbsoluteIndexedIndirectX = 23,
  kStackRelativeIndirectIndexedY = 24,
};

enum class OpcodeImplementationStatus : uint8_t {
  kImplemented = 0,
  kUnimplemented = 1,
};

struct OpcodeMetadataView {
  std::string_view mnemonic = "???";
  OpcodeAddressingMode addressing_mode = OpcodeAddressingMode::kUnknown;
  OpcodeImplementationStatus implementation_status = OpcodeImplementationStatus::kUnimplemented;
  uint8_t base_length = 1;
  bool accumulator_width_dependent = false;
  bool index_width_dependent = false;
};

[[nodiscard]] const std::array<OpcodeMetadataView, 256>& GetOpcodeMetadataTable();
[[nodiscard]] const OpcodeMetadataView& GetOpcodeMetadata(uint8_t opcode);
[[nodiscard]] uint8_t ComputeInstructionLength(const OpcodeMetadataView& metadata, const CpuFlags& flags);
[[nodiscard]] std::string_view GetAddressingModeName(OpcodeAddressingMode mode);

}  // namespace pupsnes
