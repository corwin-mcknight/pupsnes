#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <initializer_list>
#include <vector>

#include "pupsnes/hw/apu/spc700.h"

using pupsnes::Spc700;
using pupsnes::Spc700Bus;

namespace {

// Bus schedules are independently specified from the ares SPC700 reference:
// https://github.com/ares-emulator/ares/tree/master/ares/component/processor/spc700
// Arithmetic expectations below use signed ranges and nibble carry/borrow,
// rather than the core's bitwise ALU implementation. Missing cycles are idle.
struct Access {
  uint64_t cycle;
  uint16_t address;
  uint8_t value;
  bool write;
  bool operator==(const Access&) const = default;
};

struct RecordingBus final : Spc700Bus {
  uint8_t Read(uint16_t address) override {
    if (record) {
      accesses.push_back({cycle, address, memory[address], false});
    }
    return memory[address];
  }
  void Write(uint16_t address, uint8_t value) override {
    if (record) {
      accesses.push_back({cycle, address, value, true});
    }
    memory[address] = value;
  }

  std::array<uint8_t, 65536> memory{};
  std::vector<Access> accesses;
  uint64_t cycle = 0;
  bool record = true;
};

struct Fixture {
  Fixture(std::initializer_list<uint8_t> program) {
    uint16_t address = 0x0200;
    for (const uint8_t byte : program) {
      bus.memory[address++] = byte;
    }
    cpu.Reset(Spc700::State{.a = 0, .x = 3, .y = 4, .sp = 0x44, .psw = 0x7F, .pc = 0x0200});
  }

  void Tick(unsigned count = 1) {
    for (unsigned i = 0; i < count; ++i) {
      bus.cycle = cpu.GetState().cycles + 1;
      const auto access_count = bus.accesses.size();
      cpu.TickCycle();
      REQUIRE(cpu.GetState().cycles == bus.cycle);
      REQUIRE(bus.accesses.size() - access_count <= 1);
      REQUIRE_FALSE(cpu.GetState().faulted);
    }
  }

  void NextFetch(uint16_t pc, uint64_t cycle) {
    REQUIRE(cpu.GetState().pc == pc);
    Tick();
    REQUIRE(bus.accesses.back() == Access{cycle, pc, bus.memory[pc], false});
    REQUIRE(cpu.GetState().pc == static_cast<uint16_t>(pc + 1));
  }

  RecordingBus bus;
  Spc700 cpu{bus};
};

using Register = uint8_t Spc700::State::*;

int SignedByte(unsigned value) { return value < 128 ? static_cast<int>(value) : static_cast<int>(value) - 256; }

struct AluResult {
  uint8_t value;
  uint8_t flags;
};

AluResult ByteArithmetic(bool subtract, unsigned lhs, unsigned rhs, unsigned carry, uint8_t initial_flags) {
  const int full = subtract ? static_cast<int>(lhs) - static_cast<int>(rhs) - static_cast<int>(1 - carry)
                            : static_cast<int>(lhs + rhs + carry);
  const int signed_full = subtract ? SignedByte(lhs) - SignedByte(rhs) - static_cast<int>(1 - carry)
                                   : SignedByte(lhs) + SignedByte(rhs) + static_cast<int>(carry);
  const bool half_carry = subtract ? (lhs % 16 >= rhs % 16 + 1 - carry) : (lhs % 16 + rhs % 16 + carry >= 16);
  const bool carry_out = subtract ? full >= 0 : full >= 256;
  const uint8_t result = static_cast<uint8_t>(full);
  const auto flags =
      static_cast<uint8_t>((initial_flags & 0x34) | (carry_out ? 1 : 0) | (result == 0 ? 2 : 0) | (half_carry ? 8 : 0) |
                           (signed_full < -128 || signed_full > 127 ? 0x40 : 0) | (result >= 128 ? 0x80 : 0));
  return {result, flags};
}

}  // namespace

TEST_CASE("SPC700 ADC and SBC match every byte pair carry input and preserved flag state",
          "[unit][apu][spc700][spc700_arithmetic]") {
  Fixture f{};
  f.bus.record = false;
  uint64_t checked = 0;
  for (const bool subtract : {false, true}) {
    f.bus.memory[0x0200] = subtract ? 0xA8 : 0x88;
    for (const uint8_t flag_seed : std::initializer_list<uint8_t>{0x00, 0xFE}) {
      for (unsigned carry = 0; carry < 2; ++carry) {
        for (unsigned lhs = 0; lhs < 256; ++lhs) {
          for (unsigned rhs = 0; rhs < 256; ++rhs) {
            const uint8_t flags = static_cast<uint8_t>(flag_seed | carry);
            f.bus.memory[0x0201] = static_cast<uint8_t>(rhs);
            f.cpu.Reset(Spc700::State{
                .a = static_cast<uint8_t>(lhs), .x = 0x32, .y = 0x54, .sp = 0x76, .psw = flags, .pc = 0x0200});
            f.cpu.TickCycle();
            f.cpu.TickCycle();
            const AluResult expected = ByteArithmetic(subtract, lhs, rhs, carry, flags);
            const auto& state = f.cpu.GetState();
            if (state.a != expected.value || state.psw != expected.flags || state.faulted || state.pc != 0x0202 ||
                state.x != 0x32 || state.y != 0x54 || state.sp != 0x76 || state.cycles != 2) {
              CAPTURE(subtract, lhs, rhs, carry, flags, expected.value, expected.flags);
              REQUIRE(state.a == expected.value);
              REQUIRE(state.psw == expected.flags);
              REQUIRE_FALSE(state.faulted);
              REQUIRE(state.pc == 0x0202);
              REQUIRE(state.x == 0x32);
              REQUIRE(state.y == 0x54);
              REQUIRE(state.sp == 0x76);
              REQUIRE(state.cycles == 2);
            }
            ++checked;
          }
        }
      }
    }
  }
  REQUIRE(checked == 524288);
}

TEST_CASE("SPC700 all accumulator and memory CMP ADC SBC modes follow their bus schedules",
          "[unit][apu][spc700][spc700_arithmetic]") {
  struct Mode {
    const char* name;
    std::array<uint8_t, 3> opcodes;  // CMP, ADC, SBC
    uint16_t next_pc;
    unsigned cycles;
    bool memory_target;
    uint16_t target;
    std::vector<Access> reads;
  };
  const std::array modes{
      Mode{"direct", {0x64, 0x84, 0xA4}, 0x0202, 3, false, 0, {{2, 0x0201, 0xF4, false}, {3, 0x01F4, 1, false}}},
      Mode{"absolute",
           {0x65, 0x85, 0xA5},
           0x0203,
           4,
           false,
           0,
           {{2, 0x0201, 0xFE, false}, {3, 0x0202, 0xFF, false}, {4, 0xFFFE, 1, false}}},
      Mode{"indirect X", {0x66, 0x86, 0xA6}, 0x0201, 3, false, 0, {{2, 0x0201, 0, false}, {3, 0x0103, 1, false}}},
      Mode{"indexed indirect",
           {0x67, 0x87, 0xA7},
           0x0202,
           6,
           false,
           0,
           {{2, 0x0201, 0xFC, false}, {4, 0x01FF, 0x56, false}, {5, 0x0100, 0x34, false}, {6, 0x3456, 1, false}}},
      Mode{"immediate", {0x68, 0x88, 0xA8}, 0x0202, 2, false, 0, {{2, 0x0201, 1, false}}},
      Mode{"direct direct",
           {0x69, 0x89, 0xA9},
           0x0203,
           6,
           true,
           0x0110,
           {{2, 0x0201, 0xFF, false}, {3, 0x01FF, 1, false}, {4, 0x0202, 0x10, false}, {5, 0x0110, 0, false}}},
      Mode{
          "indexed direct", {0x74, 0x94, 0xB4}, 0x0202, 4, false, 0, {{2, 0x0201, 0xFE, false}, {4, 0x0101, 1, false}}},
      Mode{"absolute X",
           {0x75, 0x95, 0xB5},
           0x0203,
           5,
           false,
           0,
           {{2, 0x0201, 0xFE, false}, {3, 0x0202, 0xFF, false}, {5, 0x0001, 1, false}}},
      Mode{"absolute Y",
           {0x76, 0x96, 0xB6},
           0x0203,
           5,
           false,
           0,
           {{2, 0x0201, 0xFE, false}, {3, 0x0202, 0xFF, false}, {5, 0x0002, 1, false}}},
      Mode{"indirect indexed",
           {0x77, 0x97, 0xB7},
           0x0202,
           6,
           false,
           0,
           {{2, 0x0201, 0xFF, false}, {4, 0x01FF, 0xFE, false}, {5, 0x0100, 0xFF, false}, {6, 0x0002, 1, false}}},
      Mode{"direct immediate",
           {0x78, 0x98, 0xB8},
           0x0203,
           5,
           true,
           0x01FF,
           {{2, 0x0201, 1, false}, {3, 0x0202, 0xFF, false}, {4, 0x01FF, 0, false}}},
      Mode{"indirect X indirect Y",
           {0x79, 0x99, 0xB9},
           0x0201,
           5,
           true,
           0x0103,
           {{2, 0x0201, 0, false}, {3, 0x0104, 1, false}, {4, 0x0103, 0, false}}},
  };
  constexpr std::array<uint8_t, 3> kFlags{0xFC, 0x34, 0xB4};
  constexpr std::array<uint8_t, 3> kResults{0, 2, 0xFF};
  for (const auto& mode : modes) {
    for (unsigned operation = 0; operation < 3; ++operation) {
      for (const uint8_t page_flag : std::initializer_list<uint8_t>{0x00, 0x20}) {
        CAPTURE(mode.name, operation, page_flag);
        const uint8_t opcode = mode.opcodes[operation];
        Fixture f{opcode};
        auto initial = f.cpu.GetState();
        initial.psw = static_cast<uint8_t>(0x5F | page_flag);
        f.cpu.Reset(initial);
        // The literal table specifies P=1; only its direct-page accesses move.
        auto reads = mode.reads;
        for (Access& access : reads) {
          if (page_flag == 0 && access.address >= 0x0100 && access.address <= 0x01FF) {
            access.address = static_cast<uint16_t>(access.address - 0x0100);
          }
          f.bus.memory[access.address] = access.value;
        }
        const uint16_t target = static_cast<uint16_t>(mode.target - (mode.memory_target && page_flag == 0 ? 0x100 : 0));
        const uint8_t expected_flags = static_cast<uint8_t>((kFlags[operation] & 0xDF) | page_flag);
        const unsigned compute_cycle = static_cast<unsigned>(reads.back().cycle);
        f.Tick(compute_cycle - 1);
        REQUIRE(f.cpu.GetState().a == 0);
        REQUIRE(f.cpu.GetState().psw == initial.psw);
        f.Tick();
        REQUIRE(f.cpu.GetState().psw == expected_flags);
        REQUIRE(f.cpu.GetState().a == (mode.memory_target || operation == 0 ? 0 : kResults[operation]));
        if (mode.memory_target) {
          REQUIRE(f.bus.memory[target] == 0);  // Result is pending until the following write cycle.
        }
        f.Tick(mode.cycles - compute_cycle);
        std::vector<Access> expected{{1, 0x0200, opcode, false}};
        expected.insert(expected.end(), reads.begin(), reads.end());
        if (mode.memory_target && operation != 0) {
          expected.push_back({mode.cycles, target, kResults[operation], true});
          REQUIRE(f.bus.memory[target] == kResults[operation]);
        }
        REQUIRE(f.cpu.GetState().psw == expected_flags);
        REQUIRE(f.cpu.GetState().x == 3);
        REQUIRE(f.cpu.GetState().y == 4);
        REQUIRE(f.cpu.GetState().sp == 0x44);
        REQUIRE(f.bus.accesses == expected);
        f.NextFetch(mode.next_pc, mode.cycles + 1);
      }
    }
  }
}

TEST_CASE("SPC700 CMP X and Y cover immediate direct and absolute addressing without modifying registers",
          "[unit][apu][spc700][spc700_arithmetic]") {
  struct Mode {
    uint8_t opcode;
    Register target;
    uint16_t next_pc;
    std::vector<Access> reads;
  };
  const std::array modes{
      Mode{0xC8, &Spc700::State::x, 0x0202, {{2, 0x0201, 0x80, false}}},
      Mode{0x3E, &Spc700::State::x, 0x0202, {{2, 0x0201, 0xFF, false}, {3, 0x01FF, 0x80, false}}},
      Mode{0x1E,
           &Spc700::State::x,
           0x0203,
           {{2, 0x0201, 0xFE, false}, {3, 0x0202, 0xFF, false}, {4, 0xFFFE, 0x80, false}}},
      Mode{0xAD, &Spc700::State::y, 0x0202, {{2, 0x0201, 0x80, false}}},
      Mode{0x7E, &Spc700::State::y, 0x0202, {{2, 0x0201, 0xFF, false}, {3, 0x01FF, 0x80, false}}},
      Mode{0x5E,
           &Spc700::State::y,
           0x0203,
           {{2, 0x0201, 0xFE, false}, {3, 0x0202, 0xFF, false}, {4, 0xFFFE, 0x80, false}}},
  };
  for (const auto& mode : modes) {
    for (const uint8_t lhs : std::initializer_list<uint8_t>{0x00, 0x7F, 0x80, 0xFF}) {
      CAPTURE(mode.opcode, lhs);
      Fixture f{mode.opcode};
      auto initial = f.cpu.GetState();
      initial.*mode.target = lhs;
      f.cpu.Reset(initial);
      for (const Access& access : mode.reads) {
        f.bus.memory[access.address] = access.value;
      }
      const auto cycles = static_cast<unsigned>(mode.reads.back().cycle);
      f.Tick(cycles - 1);
      REQUIRE(f.cpu.GetState().psw == initial.psw);
      f.Tick();
      const uint8_t difference = static_cast<uint8_t>(lhs - 0x80);
      REQUIRE(f.cpu.GetState().psw == (0x7C | (lhs >= 0x80 ? 1 : 0) | (difference == 0 ? 2 : 0) | (difference & 0x80)));
      REQUIRE(f.cpu.GetState().a == initial.a);
      REQUIRE(f.cpu.GetState().x == initial.x);
      REQUIRE(f.cpu.GetState().y == initial.y);
      std::vector<Access> expected{{1, 0x0200, mode.opcode, false}};
      expected.insert(expected.end(), mode.reads.begin(), mode.reads.end());
      REQUIRE(f.bus.accesses == expected);
      f.NextFetch(mode.next_pc, cycles + 1);
    }
  }
}

TEST_CASE("SPC700 memory arithmetic latches both operands before its final write",
          "[unit][apu][spc700][spc700_arithmetic]") {
  for (const uint8_t opcode : std::initializer_list<uint8_t>{0x89, 0xA9}) {
    CAPTURE(opcode);
    Fixture f{opcode, 0xFF, 0x10};
    f.bus.memory[0x01FF] = 0x01;
    f.bus.memory[0x0110] = 0x7F;
    f.Tick(3);
    f.bus.memory[0x01FF] = 0x55;
    f.Tick(2);
    const uint8_t flags = opcode == 0x89 ? 0xFC : 0x3D;
    const uint8_t result = opcode == 0x89 ? 0x81 : 0x7E;
    REQUIRE(f.cpu.GetState().psw == flags);
    REQUIRE(f.bus.memory[0x0110] == 0x7F);
    f.bus.memory[0x0110] = 0x66;
    f.Tick();
    REQUIRE(f.bus.memory[0x0110] == result);
    REQUIRE(f.cpu.GetState().psw == flags);
    REQUIRE(f.bus.accesses == std::vector<Access>{{1, 0x0200, opcode, false},
                                                  {2, 0x0201, 0xFF, false},
                                                  {3, 0x01FF, 1, false},
                                                  {4, 0x0202, 0x10, false},
                                                  {5, 0x0110, 0x7F, false},
                                                  {6, 0x0110, result, true}});
    f.NextFetch(0x0203, 7);
  }
}

TEST_CASE("SPC700 all byte INC and DEC forms preserve non-NZ flags and change memory on their write cycle",
          "[unit][apu][spc700][spc700_arithmetic]") {
  struct Mode {
    std::array<uint8_t, 2> opcodes;  // INC, DEC
    Register target;
    unsigned cycles;
    uint16_t next_pc;
    std::vector<Access> fetches;
    uint16_t address;
    bool direct_page;
  };
  const std::array modes{
      Mode{{0xBC, 0x9C}, &Spc700::State::a, 2, 0x0201, {{2, 0x0201, 0, false}}, 0, false},
      Mode{{0x3D, 0x1D}, &Spc700::State::x, 2, 0x0201, {{2, 0x0201, 0, false}}, 0, false},
      Mode{{0xFC, 0xDC}, &Spc700::State::y, 2, 0x0201, {{2, 0x0201, 0, false}}, 0, false},
      Mode{{0xAB, 0x8B}, nullptr, 4, 0x0202, {{2, 0x0201, 0xFF, false}}, 0x00FF, true},
      Mode{{0xAC, 0x8C}, nullptr, 5, 0x0203, {{2, 0x0201, 0xFF, false}, {3, 0x0202, 0xFF, false}}, 0xFFFF, false},
      Mode{{0xBB, 0x9B}, nullptr, 5, 0x0202, {{2, 0x0201, 0xFE, false}}, 0x0001, true},
  };
  for (const auto& mode : modes) {
    for (unsigned operation = 0; operation < 2; ++operation) {
      for (const uint8_t old_value : std::initializer_list<uint8_t>{0x00, 0x01, 0x7F, 0x80, 0xFF}) {
        for (const uint8_t flags : std::initializer_list<uint8_t>{0x00, 0xFF}) {
          CAPTURE(mode.opcodes[operation], old_value, flags);
          Fixture f{mode.opcodes[operation]};
          for (const Access& access : mode.fetches) {
            f.bus.memory[access.address] = access.value;
          }
          auto initial = f.cpu.GetState();
          initial.psw = flags;
          const uint16_t address =
              static_cast<uint16_t>(mode.address | (mode.direct_page && flags == 0xFF ? 0x100 : 0));
          if (mode.target != nullptr) {
            initial.*mode.target = old_value;
          } else {
            f.bus.memory[address] = old_value;
          }
          f.cpu.Reset(initial);
          f.Tick(mode.cycles - 1);
          REQUIRE(f.cpu.GetState().psw == flags);
          if (mode.target != nullptr) {
            REQUIRE(f.cpu.GetState().*mode.target == old_value);
          } else {
            REQUIRE(f.bus.memory[address] == old_value);
            f.bus.memory[address] = 0x55;  // The result must use the earlier read.
          }
          f.Tick();
          const uint8_t value = static_cast<uint8_t>(static_cast<int>(old_value) + (operation == 0 ? 1 : -1));
          REQUIRE(f.cpu.GetState().psw == ((flags & 0x7D) | (value == 0 ? 2 : 0) | (value & 0x80)));
          std::vector<Access> expected{{1, 0x0200, mode.opcodes[operation], false}};
          expected.insert(expected.end(), mode.fetches.begin(), mode.fetches.end());
          if (mode.target != nullptr) {
            REQUIRE(f.cpu.GetState().*mode.target == value);
          } else {
            REQUIRE(f.bus.memory[address] == value);
            expected.push_back({mode.cycles - 1, address, old_value, false});
            expected.push_back({mode.cycles, address, value, true});
            REQUIRE(f.cpu.GetState().a == initial.a);
            REQUIRE(f.cpu.GetState().x == initial.x);
            REQUIRE(f.cpu.GetState().y == initial.y);
          }
          REQUIRE(f.bus.accesses == expected);
          f.NextFetch(mode.next_pc, mode.cycles + 1);
        }
      }
    }
  }
}

TEST_CASE("SPC700 INCW and DECW wrap within the direct page and defer word NZ until the high-byte write",
          "[unit][apu][spc700][spc700_arithmetic]") {
  for (const uint8_t opcode : std::initializer_list<uint8_t>{0x3A, 0x1A}) {
    for (const uint16_t old_word : std::initializer_list<uint16_t>{0x0000, 0x00FF, 0x7FFF, 0x8000, 0xFFFF}) {
      for (const uint8_t flags : std::initializer_list<uint8_t>{0x00, 0xFF}) {
        CAPTURE(opcode, old_word, flags);
        Fixture f{opcode, 0xFF};
        auto initial = f.cpu.GetState();
        initial.psw = flags;
        f.cpu.Reset(initial);
        const uint16_t low_address = flags == 0 ? 0x00FF : 0x01FF;
        const uint16_t high_address = flags == 0 ? 0x0000 : 0x0100;
        const uint8_t old_low = static_cast<uint8_t>(old_word);
        const uint8_t old_high = static_cast<uint8_t>(old_word >> 8);
        const uint16_t word = static_cast<uint16_t>(static_cast<int>(old_word) + (opcode == 0x3A ? 1 : -1));
        const uint8_t low = static_cast<uint8_t>(word);
        const uint8_t high = static_cast<uint8_t>(word >> 8);
        f.bus.memory[low_address] = old_low;
        f.bus.memory[high_address] = old_high;
        f.Tick(3);
        REQUIRE(f.bus.memory[low_address] == old_low);
        REQUIRE(f.cpu.GetState().psw == flags);
        f.Tick();
        REQUIRE(f.bus.memory[low_address] == low);
        REQUIRE(f.bus.memory[high_address] == old_high);
        REQUIRE(f.cpu.GetState().psw == flags);
        f.Tick();
        REQUIRE(f.bus.memory[high_address] == old_high);
        REQUIRE(f.cpu.GetState().psw == flags);
        f.Tick();
        REQUIRE(f.bus.memory[high_address] == high);
        REQUIRE(f.cpu.GetState().psw == ((flags & 0x7D) | (word == 0 ? 2 : 0) | (word >= 0x8000 ? 0x80 : 0)));
        REQUIRE(f.cpu.GetState().a == initial.a);
        REQUIRE(f.cpu.GetState().x == initial.x);
        REQUIRE(f.cpu.GetState().y == initial.y);
        REQUIRE(f.bus.accesses == std::vector<Access>{{1, 0x0200, opcode, false},
                                                      {2, 0x0201, 0xFF, false},
                                                      {3, low_address, old_low, false},
                                                      {4, low_address, low, true},
                                                      {5, high_address, old_high, false},
                                                      {6, high_address, high, true}});
        f.NextFetch(0x0202, 7);
      }
    }
  }
}

TEST_CASE("SPC700 word arithmetic reads a wrapping pointer and computes full-word flags on the high-byte read",
          "[unit][apu][spc700][spc700_arithmetic]") {
  struct Operands {
    uint16_t lhs;
    uint16_t rhs;
  };
  constexpr std::array kOperands{
      Operands{0x0000, 0x0000}, Operands{0xFFFF, 0x0001}, Operands{0x7FFF, 0x0001}, Operands{0x8000, 0x0001},
      Operands{0x0FFF, 0x0001}, Operands{0x1000, 0x0001}, Operands{0x0000, 0xFFFF}, Operands{0x8000, 0xFFFF},
      Operands{0x00FF, 0x0001}, Operands{0x0001, 0x0001}, Operands{0x00FF, 0x0100},
  };
  for (const uint8_t opcode : std::initializer_list<uint8_t>{0x7A, 0x9A, 0x5A}) {
    for (const auto& operands : kOperands) {
      for (const uint8_t flags : std::initializer_list<uint8_t>{0x00, 0xFF}) {
        CAPTURE(opcode, operands.lhs, operands.rhs, flags);
        Fixture f{opcode, 0xFF};
        auto initial = f.cpu.GetState();
        initial.a = static_cast<uint8_t>(operands.lhs);
        initial.y = static_cast<uint8_t>(operands.lhs >> 8);
        initial.psw = flags;
        f.cpu.Reset(initial);
        const uint16_t low_address = flags == 0 ? 0x00FF : 0x01FF;
        const uint16_t high_address = flags == 0 ? 0x0000 : 0x0100;
        const uint8_t low = static_cast<uint8_t>(operands.rhs);
        const uint8_t high = static_cast<uint8_t>(operands.rhs >> 8);
        f.bus.memory[low_address] = low;
        f.bus.memory[high_address] = high;
        const bool subtract = opcode != 0x7A;
        const bool compare = opcode == 0x5A;
        const unsigned cycles = compare ? 4 : 5;
        f.Tick(cycles - 1);
        REQUIRE(f.cpu.GetState().a == initial.a);
        REQUIRE(f.cpu.GetState().y == initial.y);
        REQUIRE(f.cpu.GetState().psw == flags);
        f.bus.memory[low_address] = 0x55;  // Low byte was already sampled on cycle three.
        f.Tick();
        const int full =
            subtract ? static_cast<int>(operands.lhs) - operands.rhs : static_cast<int>(operands.lhs) + operands.rhs;
        const uint16_t result = static_cast<uint16_t>(full);
        const bool carry = subtract ? full >= 0 : full >= 65536;
        uint8_t expected_flags = static_cast<uint8_t>((flags & (compare ? 0x7C : 0x34)) | (carry ? 1 : 0) |
                                                      (result == 0 ? 2 : 0) | (result >= 0x8000 ? 0x80 : 0));
        if (!compare) {
          const int signed_lhs = operands.lhs < 0x8000 ? operands.lhs : static_cast<int>(operands.lhs) - 65536;
          const int signed_rhs = operands.rhs < 0x8000 ? operands.rhs : static_cast<int>(operands.rhs) - 65536;
          const int signed_result = subtract ? signed_lhs - signed_rhs : signed_lhs + signed_rhs;
          const bool half =
              subtract ? operands.lhs % 4096 >= operands.rhs % 4096 : operands.lhs % 4096 + operands.rhs % 4096 >= 4096;
          expected_flags = static_cast<uint8_t>(expected_flags | (half ? 8 : 0) |
                                                (signed_result < -32768 || signed_result > 32767 ? 0x40 : 0));
        }
        REQUIRE(f.cpu.GetState().psw == expected_flags);
        REQUIRE(f.cpu.GetState().a == (compare ? initial.a : static_cast<uint8_t>(result)));
        REQUIRE(f.cpu.GetState().y == (compare ? initial.y : static_cast<uint8_t>(result >> 8)));
        REQUIRE(f.cpu.GetState().x == initial.x);
        REQUIRE(f.bus.accesses == std::vector<Access>{{1, 0x0200, opcode, false},
                                                      {2, 0x0201, 0xFF, false},
                                                      {3, low_address, low, false},
                                                      {cycles, high_address, high, false}});
        f.NextFetch(0x0202, cycles + 1);
      }
    }
  }
}

TEST_CASE("SPC700 MUL completes after nine cycles with NZ based only on the high product byte",
          "[unit][apu][spc700][spc700_arithmetic]") {
  for (const uint8_t a : std::initializer_list<uint8_t>{0x00, 0x01, 0x0F, 0x80, 0xFF}) {
    for (const uint8_t y : std::initializer_list<uint8_t>{0x00, 0x01, 0x10, 0x80, 0xFF}) {
      for (const uint8_t flags : std::initializer_list<uint8_t>{0x00, 0xFF}) {
        CAPTURE(a, y, flags);
        Fixture f{0xCF, 0x00};
        f.cpu.Reset(Spc700::State{.a = a, .x = 0x42, .y = y, .sp = 0x76, .psw = flags, .pc = 0x0200});
        f.Tick(8);
        REQUIRE(f.cpu.GetState().a == a);
        REQUIRE(f.cpu.GetState().y == y);
        REQUIRE(f.cpu.GetState().psw == flags);
        f.Tick();
        const unsigned product = static_cast<unsigned>(a) * y;
        const auto high = static_cast<uint8_t>(product >> 8);
        REQUIRE(f.cpu.GetState().a == static_cast<uint8_t>(product));
        REQUIRE(f.cpu.GetState().y == high);
        REQUIRE(f.cpu.GetState().x == 0x42);
        REQUIRE(f.cpu.GetState().sp == 0x76);
        REQUIRE(f.cpu.GetState().psw == ((flags & 0x7D) | (high == 0 ? 2 : 0) | (high & 0x80)));
        REQUIRE(f.bus.accesses == std::vector<Access>{{1, 0x0200, 0xCF, false}, {2, 0x0201, 0, false}});
        f.NextFetch(0x0201, 10);
      }
    }
  }
}

TEST_CASE("SPC700 DIV defines ordinary overflow and zero-divisor results after twelve cycles",
          "[unit][apu][spc700][spc700_arithmetic]") {
  struct Example {
    uint8_t a;
    uint8_t y;
    uint8_t x;
    uint8_t quotient;
    uint8_t remainder;
    uint8_t hvnz;
  };
  // Literal boundary vectors include both sides of Y=2*X, a nine-bit
  // quotient, X=0, and H comparisons that differ from the overflow flag.
  constexpr std::array kCases{
      Example{0x80, 0x00, 0x10, 0x08, 0x00, 0x08}, Example{0x34, 0x12, 0x10, 0x23, 0x04, 0x48},
      Example{0x56, 0x20, 0x10, 0xFF, 0x66, 0xC8}, Example{0x00, 0xFF, 0x00, 0x00, 0x00, 0x4A},
      Example{0x37, 0x12, 0x00, 0xED, 0x37, 0xC8}, Example{0xFF, 0xFF, 0x01, 0x01, 0xFE, 0x48},
      Example{0x00, 0x01, 0x02, 0x80, 0x00, 0x80}, Example{0x00, 0x00, 0xFF, 0x00, 0x00, 0x02},
      Example{0xFF, 0xFF, 0xFF, 0x01, 0x00, 0x48}, Example{0x00, 0x80, 0xFF, 0x80, 0x80, 0x80},
      Example{0x00, 0x80, 0x40, 0xFF, 0x40, 0xC8}, Example{0x00, 0x7F, 0x40, 0xFC, 0x00, 0xC8},
  };
  for (const auto& example : kCases) {
    for (const uint8_t flags : std::initializer_list<uint8_t>{0x00, 0xFF}) {
      CAPTURE(example.a, example.y, example.x, flags);
      Fixture f{0x9E, 0x00};
      f.cpu.Reset(
          Spc700::State{.a = example.a, .x = example.x, .y = example.y, .sp = 0x76, .psw = flags, .pc = 0x0200});
      f.Tick(11);
      REQUIRE(f.cpu.GetState().a == example.a);
      REQUIRE(f.cpu.GetState().y == example.y);
      REQUIRE(f.cpu.GetState().psw == flags);
      f.Tick();
      REQUIRE(f.cpu.GetState().a == example.quotient);
      REQUIRE(f.cpu.GetState().y == example.remainder);
      REQUIRE(f.cpu.GetState().x == example.x);
      REQUIRE(f.cpu.GetState().sp == 0x76);
      REQUIRE(f.cpu.GetState().psw == ((flags & 0x35) | example.hvnz));
      REQUIRE(f.bus.accesses == std::vector<Access>{{1, 0x0200, 0x9E, false}, {2, 0x0201, 0, false}});
      f.NextFetch(0x0201, 13);
    }
  }
}

TEST_CASE("SPC700 DAA and DAS adjust valid and invalid BCD inputs on their final idle cycle",
          "[unit][apu][spc700][spc700_arithmetic]") {
  struct Example {
    uint8_t opcode;
    uint8_t a;
    uint8_t carry_half;
    uint8_t result;
    bool carry_out;
  };
  // These literal cases include invalid decimal nibbles, forced correction
  // through incoming C/H, and correction that wraps A through zero.
  constexpr std::array kCases{
      Example{0xDF, 0x00, 0x00, 0x00, false}, Example{0xDF, 0x09, 0x00, 0x09, false},
      Example{0xDF, 0x0A, 0x00, 0x10, false}, Example{0xDF, 0x0F, 0x00, 0x15, false},
      Example{0xDF, 0x10, 0x08, 0x16, false}, Example{0xDF, 0x99, 0x00, 0x99, false},
      Example{0xDF, 0x9A, 0x00, 0x00, true},  Example{0xDF, 0xA0, 0x00, 0x00, true},
      Example{0xDF, 0xFF, 0x00, 0x65, true},  Example{0xDF, 0x15, 0x01, 0x75, true},
      Example{0xDF, 0x99, 0x01, 0xF9, true},  Example{0xDF, 0x99, 0x08, 0x9F, false},
      Example{0xDF, 0xFF, 0x09, 0x65, true},  Example{0xDF, 0x94, 0x08, 0x9A, false},
      Example{0xBE, 0x00, 0x09, 0x00, true},  Example{0xBE, 0x00, 0x00, 0x9A, false},
      Example{0xBE, 0x00, 0x01, 0xFA, true},  Example{0xBE, 0x00, 0x08, 0xA0, false},
      Example{0xBE, 0x09, 0x09, 0x09, true},  Example{0xBE, 0x0A, 0x09, 0x04, true},
      Example{0xBE, 0x10, 0x01, 0x0A, true},  Example{0xBE, 0x99, 0x09, 0x99, true},
      Example{0xBE, 0x9A, 0x09, 0x34, false}, Example{0xBE, 0xA0, 0x09, 0x40, false},
      Example{0xBE, 0xFF, 0x09, 0x99, false}, Example{0xBE, 0xFF, 0x00, 0x99, false},
      Example{0xBE, 0x15, 0x08, 0xB5, false}, Example{0xBE, 0x99, 0x08, 0x39, false},
      Example{0xBE, 0x99, 0x01, 0x93, true},  Example{0xBE, 0x9F, 0x09, 0x39, false},
  };
  for (const auto& example : kCases) {
    for (const uint8_t preserved : std::initializer_list<uint8_t>{0x00, 0xF6}) {
      CAPTURE(example.opcode, example.a, example.carry_half, preserved);
      Fixture f{example.opcode, 0x00};
      const uint8_t flags = static_cast<uint8_t>(preserved | example.carry_half);
      f.cpu.Reset(Spc700::State{.a = example.a, .x = 0x32, .y = 0x54, .sp = 0x76, .psw = flags, .pc = 0x0200});
      f.Tick(2);
      REQUIRE(f.cpu.GetState().a == example.a);
      REQUIRE(f.cpu.GetState().psw == flags);
      f.Tick();
      REQUIRE(f.cpu.GetState().a == example.result);
      REQUIRE(f.cpu.GetState().x == 0x32);
      REQUIRE(f.cpu.GetState().y == 0x54);
      REQUIRE(f.cpu.GetState().sp == 0x76);
      REQUIRE(f.cpu.GetState().psw ==
              ((flags & 0x7C) | (example.carry_out ? 1 : 0) | (example.result == 0 ? 2 : 0) | (example.result & 0x80)));
      REQUIRE(f.bus.accesses == std::vector<Access>{{1, 0x0200, example.opcode, false}, {2, 0x0201, 0, false}});
      f.NextFetch(0x0201, 4);
    }
  }
}

TEST_CASE("SPC700 decimal adjustment reproduces all two-digit BCD additions and subtractions",
          "[unit][apu][spc700][spc700_arithmetic]") {
  Fixture f{};
  f.bus.record = false;
  uint64_t checked = 0;
  for (const bool subtract : {false, true}) {
    f.bus.memory[0x0200] = subtract ? 0xA8 : 0x88;
    f.bus.memory[0x0202] = subtract ? 0xBE : 0xDF;
    for (unsigned carry = 0; carry < 2; ++carry) {
      for (unsigned lhs = 0; lhs < 100; ++lhs) {
        for (unsigned rhs = 0; rhs < 100; ++rhs) {
          const uint8_t lhs_bcd = static_cast<uint8_t>((lhs / 10) * 16 + lhs % 10);
          const uint8_t rhs_bcd = static_cast<uint8_t>((rhs / 10) * 16 + rhs % 10);
          f.bus.memory[0x0201] = rhs_bcd;
          const uint8_t flags = static_cast<uint8_t>(0x34 | carry);
          f.cpu.Reset(Spc700::State{.a = lhs_bcd, .x = 0x32, .y = 0x54, .sp = 0x76, .psw = flags, .pc = 0x0200});
          for (unsigned cycle = 0; cycle < 5; ++cycle) {
            f.cpu.TickCycle();
          }
          const int decimal = subtract ? static_cast<int>(lhs) - static_cast<int>(rhs) - static_cast<int>(1 - carry)
                                       : static_cast<int>(lhs + rhs + carry);
          const unsigned wrapped = static_cast<unsigned>((decimal + 100) % 100);
          const uint8_t expected_a = static_cast<uint8_t>((wrapped / 10) * 16 + wrapped % 10);
          const bool carry_out = subtract ? decimal >= 0 : decimal >= 100;
          const AluResult binary = ByteArithmetic(subtract, lhs_bcd, rhs_bcd, carry, flags);
          const uint8_t expected_flags = static_cast<uint8_t>((binary.flags & 0x7C) | (carry_out ? 1 : 0) |
                                                              (expected_a == 0 ? 2 : 0) | (expected_a & 0x80));
          const auto& state = f.cpu.GetState();
          if (state.a != expected_a || state.psw != expected_flags || state.faulted || state.pc != 0x0203 ||
              state.x != 0x32 || state.y != 0x54 || state.sp != 0x76 || state.cycles != 5) {
            CAPTURE(subtract, lhs, rhs, carry, expected_a, expected_flags);
            REQUIRE(state.a == expected_a);
            REQUIRE(state.psw == expected_flags);
            REQUIRE_FALSE(state.faulted);
            REQUIRE(state.pc == 0x0203);
            REQUIRE(state.x == 0x32);
            REQUIRE(state.y == 0x54);
            REQUIRE(state.sp == 0x76);
            REQUIRE(state.cycles == 5);
          }
          ++checked;
        }
      }
    }
  }
  REQUIRE(checked == 40000);
}

TEST_CASE("SPC700 CMP matches every unsigned byte pair and preserves H V and control flags",
          "[unit][apu][spc700][spc700_arithmetic]") {
  Fixture f{0x68};
  f.bus.record = false;
  uint64_t checked = 0;
  for (const uint8_t flags : std::initializer_list<uint8_t>{0x00, 0xFF}) {
    for (unsigned lhs = 0; lhs < 256; ++lhs) {
      for (unsigned rhs = 0; rhs < 256; ++rhs) {
        f.bus.memory[0x0201] = static_cast<uint8_t>(rhs);
        f.cpu.Reset(Spc700::State{
            .a = static_cast<uint8_t>(lhs), .x = 0x32, .y = 0x54, .sp = 0x76, .psw = flags, .pc = 0x0200});
        f.cpu.TickCycle();
        f.cpu.TickCycle();
        const uint8_t difference = static_cast<uint8_t>(static_cast<int>(lhs) - static_cast<int>(rhs));
        const uint8_t expected_flags = static_cast<uint8_t>((flags & 0x7C) | (lhs >= rhs ? 1 : 0) |
                                                            (lhs == rhs ? 2 : 0) | (difference >= 128 ? 0x80 : 0));
        const auto& state = f.cpu.GetState();
        if (state.a != lhs || state.psw != expected_flags || state.faulted || state.pc != 0x0202 || state.x != 0x32 ||
            state.y != 0x54 || state.sp != 0x76 || state.cycles != 2) {
          CAPTURE(lhs, rhs, flags, expected_flags);
          REQUIRE(state.a == lhs);
          REQUIRE(state.psw == expected_flags);
          REQUIRE_FALSE(state.faulted);
          REQUIRE(state.pc == 0x0202);
          REQUIRE(state.x == 0x32);
          REQUIRE(state.y == 0x54);
          REQUIRE(state.sp == 0x76);
          REQUIRE(state.cycles == 2);
        }
        ++checked;
      }
    }
  }
  REQUIRE(checked == 131072);
}
