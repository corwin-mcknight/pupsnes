#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <initializer_list>
#include <vector>

#include "pupsnes/hw/apu/spc700.h"

using pupsnes::Spc700;
using pupsnes::Spc700Bus;

namespace {

// Independent instruction schedules follow the ares SPC700 reference:
// https://github.com/ares-emulator/ares/tree/master/ares/component/processor/spc700
// Each omitted cycle is idle; operand reads and dummy reads remain observable.
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
    cpu.Reset(Spc700::State{.a = 0x81, .x = 3, .y = 4, .sp = 0x44, .psw = 0x7F, .pc = 0x0200});
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

struct ShiftResult {
  uint8_t value;
  uint8_t flags;
};

ShiftResult Shift(unsigned operation, uint8_t value, uint8_t flags) {
  // ASL, LSR, ROL, ROR: arithmetic division/remainders express the bit
  // leaving each end independently of the core's shifts and bit masks.
  const unsigned carry = flags % 2;
  const bool left = operation == 0 || operation == 2;
  const unsigned incoming = operation == 2 ? carry : (operation == 3 ? carry * 128 : 0);
  const uint8_t result =
      static_cast<uint8_t>(left ? static_cast<unsigned>(value) * 2 + incoming : value / 2 + incoming);
  const bool outgoing = left ? value >= 128 : value % 2 != 0;
  const uint8_t expected_flags =
      static_cast<uint8_t>((flags & 0x7C) | (outgoing ? 1 : 0) | (result == 0 ? 2 : 0) | (result >= 128 ? 0x80 : 0));
  return {result, expected_flags};
}

}  // namespace

TEST_CASE("SPC700 OR AND and EOR match every byte pair and preserve all non-NZ flags",
          "[unit][apu][spc700][spc700_logic]") {
  Fixture f{};
  f.bus.record = false;
  uint64_t checked = 0;
  constexpr std::array<uint8_t, 3> kOpcodes{0x08, 0x28, 0x48};
  for (unsigned operation = 0; operation < 3; ++operation) {
    f.bus.memory[0x0200] = kOpcodes[operation];
    for (const uint8_t flags : std::initializer_list<uint8_t>{0x00, 0xFF}) {
      for (unsigned lhs = 0; lhs < 256; ++lhs) {
        for (unsigned rhs = 0; rhs < 256; ++rhs) {
          f.bus.memory[0x0201] = static_cast<uint8_t>(rhs);
          f.cpu.Reset(Spc700::State{
              .a = static_cast<uint8_t>(lhs), .x = 0x32, .y = 0x54, .sp = 0x76, .psw = flags, .pc = 0x0200});
          f.cpu.TickCycle();
          f.cpu.TickCycle();
          const uint8_t result =
              static_cast<uint8_t>(operation == 0 ? lhs | rhs : (operation == 1 ? lhs & rhs : lhs ^ rhs));
          const uint8_t expected_flags =
              static_cast<uint8_t>((flags & 0x7D) | (result == 0 ? 2 : 0) | (result >= 128 ? 0x80 : 0));
          const auto& state = f.cpu.GetState();
          if (state.a != result || state.psw != expected_flags || state.faulted || state.pc != 0x0202 ||
              state.x != 0x32 || state.y != 0x54 || state.sp != 0x76 || state.cycles != 2) {
            CAPTURE(operation, lhs, rhs, flags, result, expected_flags);
            REQUIRE(state.a == result);
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
  }
  REQUIRE(checked == 393216);
}

TEST_CASE("SPC700 all logical addressing modes preserve cycle schedules and wrapping on either direct page",
          "[unit][apu][spc700][spc700_logic]") {
  struct Mode {
    const char* name;
    std::array<uint8_t, 3> opcodes;  // OR, AND, EOR
    uint16_t next_pc;
    unsigned cycles;
    bool memory_target;
    uint16_t target;
    std::vector<Access> reads;
  };
  const std::array modes{
      Mode{"direct", {0x04, 0x24, 0x44}, 0x0202, 3, false, 0, {{2, 0x0201, 0xF4, false}, {3, 0x01F4, 0x0F, false}}},
      Mode{"absolute",
           {0x05, 0x25, 0x45},
           0x0203,
           4,
           false,
           0,
           {{2, 0x0201, 0xFE, false}, {3, 0x0202, 0xFF, false}, {4, 0xFFFE, 0x0F, false}}},
      Mode{"indirect X", {0x06, 0x26, 0x46}, 0x0201, 3, false, 0, {{2, 0x0201, 0, false}, {3, 0x0103, 0x0F, false}}},
      Mode{"indexed indirect",
           {0x07, 0x27, 0x47},
           0x0202,
           6,
           false,
           0,
           {{2, 0x0201, 0xFC, false}, {4, 0x01FF, 0x56, false}, {5, 0x0100, 0x34, false}, {6, 0x3456, 0x0F, false}}},
      Mode{"immediate", {0x08, 0x28, 0x48}, 0x0202, 2, false, 0, {{2, 0x0201, 0x0F, false}}},
      Mode{"direct direct",
           {0x09, 0x29, 0x49},
           0x0203,
           6,
           true,
           0x0110,
           {{2, 0x0201, 0xFF, false}, {3, 0x01FF, 0x0F, false}, {4, 0x0202, 0x10, false}, {5, 0x0110, 0x81, false}}},
      Mode{"indexed direct",
           {0x14, 0x34, 0x54},
           0x0202,
           4,
           false,
           0,
           {{2, 0x0201, 0xFE, false}, {4, 0x0101, 0x0F, false}}},
      Mode{"absolute X",
           {0x15, 0x35, 0x55},
           0x0203,
           5,
           false,
           0,
           {{2, 0x0201, 0xFE, false}, {3, 0x0202, 0xFF, false}, {5, 0x0001, 0x0F, false}}},
      Mode{"absolute Y",
           {0x16, 0x36, 0x56},
           0x0203,
           5,
           false,
           0,
           {{2, 0x0201, 0xFE, false}, {3, 0x0202, 0xFF, false}, {5, 0x0002, 0x0F, false}}},
      Mode{"indirect indexed",
           {0x17, 0x37, 0x57},
           0x0202,
           6,
           false,
           0,
           {{2, 0x0201, 0xFF, false}, {4, 0x01FF, 0xFE, false}, {5, 0x0100, 0xFF, false}, {6, 0x0002, 0x0F, false}}},
      Mode{"direct immediate",
           {0x18, 0x38, 0x58},
           0x0203,
           5,
           true,
           0x01FF,
           {{2, 0x0201, 0x0F, false}, {3, 0x0202, 0xFF, false}, {4, 0x01FF, 0x81, false}}},
      Mode{"indirect X indirect Y",
           {0x19, 0x39, 0x59},
           0x0201,
           5,
           true,
           0x0103,
           {{2, 0x0201, 0, false}, {3, 0x0104, 0x0F, false}, {4, 0x0103, 0x81, false}}},
  };
  constexpr std::array<uint8_t, 3> kFlags{0xFD, 0x7D, 0xFD};
  constexpr std::array<uint8_t, 3> kResults{0x8F, 0x01, 0x8E};
  for (const auto& mode : modes) {
    for (unsigned operation = 0; operation < 3; ++operation) {
      for (const uint8_t page_flag : std::initializer_list<uint8_t>{0x00, 0x20}) {
        CAPTURE(mode.name, operation, page_flag);
        const uint8_t opcode = mode.opcodes[operation];
        Fixture f{opcode};
        auto initial = f.cpu.GetState();
        initial.psw = static_cast<uint8_t>(0x5F | page_flag);
        f.cpu.Reset(initial);
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
        REQUIRE(f.cpu.GetState().a == initial.a);
        REQUIRE(f.cpu.GetState().psw == initial.psw);
        f.Tick();
        REQUIRE(f.cpu.GetState().psw == expected_flags);
        REQUIRE(f.cpu.GetState().a == (mode.memory_target ? initial.a : kResults[operation]));
        if (mode.memory_target) {
          REQUIRE(f.bus.memory[target] == 0x81);
        }
        f.Tick(mode.cycles - compute_cycle);
        std::vector<Access> expected{{1, 0x0200, opcode, false}};
        expected.insert(expected.end(), reads.begin(), reads.end());
        if (mode.memory_target) {
          expected.push_back({mode.cycles, target, kResults[operation], true});
          REQUIRE(f.bus.memory[target] == kResults[operation]);
        }
        REQUIRE(f.cpu.GetState().psw == expected_flags);
        REQUIRE(f.cpu.GetState().x == initial.x);
        REQUIRE(f.cpu.GetState().y == initial.y);
        REQUIRE(f.cpu.GetState().sp == initial.sp);
        REQUIRE(f.bus.accesses == expected);
        f.NextFetch(mode.next_pc, mode.cycles + 1);
      }
    }
  }
}

TEST_CASE("SPC700 memory logical instructions latch both operands before their write",
          "[unit][apu][spc700][spc700_logic]") {
  constexpr std::array<uint8_t, 3> kOpcodes{0x09, 0x29, 0x49};
  constexpr std::array<uint8_t, 3> kResults{0x8F, 0x01, 0x8E};
  constexpr std::array<uint8_t, 3> kFlags{0xFD, 0x7D, 0xFD};
  for (unsigned operation = 0; operation < 3; ++operation) {
    CAPTURE(operation);
    Fixture f{kOpcodes[operation], 0xFF, 0x10};
    f.bus.memory[0x01FF] = 0x0F;
    f.bus.memory[0x0110] = 0x81;
    f.Tick(3);
    f.bus.memory[0x01FF] = 0xFF;
    f.Tick(2);
    REQUIRE(f.bus.memory[0x0110] == 0x81);
    REQUIRE(f.cpu.GetState().psw == kFlags[operation]);
    f.bus.memory[0x0110] = 0x66;
    f.Tick();
    REQUIRE(f.bus.memory[0x0110] == kResults[operation]);
    REQUIRE(f.cpu.GetState().psw == kFlags[operation]);
    REQUIRE(f.bus.accesses == std::vector<Access>{{1, 0x0200, kOpcodes[operation], false},
                                                  {2, 0x0201, 0xFF, false},
                                                  {3, 0x01FF, 0x0F, false},
                                                  {4, 0x0202, 0x10, false},
                                                  {5, 0x0110, 0x81, false},
                                                  {6, 0x0110, kResults[operation], true}});
    f.NextFetch(0x0203, 7);
  }
}

TEST_CASE("SPC700 shifts and rotates match every accumulator byte and carry input",
          "[unit][apu][spc700][spc700_logic]") {
  Fixture f{};
  f.bus.record = false;
  constexpr std::array<uint8_t, 4> kOpcodes{0x1C, 0x5C, 0x3C, 0x7C};
  uint64_t checked = 0;
  for (unsigned operation = 0; operation < 4; ++operation) {
    f.bus.memory[0x0200] = kOpcodes[operation];
    for (unsigned value = 0; value < 256; ++value) {
      for (unsigned carry = 0; carry < 2; ++carry) {
        for (const uint8_t seed : std::initializer_list<uint8_t>{0x00, 0xFE}) {
          const uint8_t flags = static_cast<uint8_t>(seed | carry);
          f.cpu.Reset(Spc700::State{
              .a = static_cast<uint8_t>(value), .x = 0x32, .y = 0x54, .sp = 0x76, .psw = flags, .pc = 0x0200});
          f.cpu.TickCycle();
          f.cpu.TickCycle();
          const auto expected = Shift(operation, static_cast<uint8_t>(value), flags);
          const auto& state = f.cpu.GetState();
          if (state.a != expected.value || state.psw != expected.flags || state.faulted || state.pc != 0x0201 ||
              state.x != 0x32 || state.y != 0x54 || state.sp != 0x76 || state.cycles != 2) {
            CAPTURE(operation, value, carry, flags, expected.value, expected.flags);
            REQUIRE(state.a == expected.value);
            REQUIRE(state.psw == expected.flags);
            REQUIRE_FALSE(state.faulted);
            REQUIRE(state.pc == 0x0201);
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
  REQUIRE(checked == 4096);
}

TEST_CASE("SPC700 all shift and rotate addressing modes update flags at the write cycle",
          "[unit][apu][spc700][spc700_logic]") {
  struct Mode {
    std::array<uint8_t, 4> opcodes;  // ASL, LSR, ROL, ROR
    unsigned cycles;
    uint16_t next_pc;
    std::vector<Access> fetches;
    uint16_t address;
    bool direct_page;
  };
  const std::array modes{
      Mode{{0x1C, 0x5C, 0x3C, 0x7C}, 2, 0x0201, {{2, 0x0201, 0, false}}, 0, false},
      Mode{{0x0B, 0x4B, 0x2B, 0x6B}, 4, 0x0202, {{2, 0x0201, 0xFF, false}}, 0x00FF, true},
      Mode{{0x0C, 0x4C, 0x2C, 0x6C}, 5, 0x0203, {{2, 0x0201, 0xFF, false}, {3, 0x0202, 0xFF, false}}, 0xFFFF, false},
      Mode{{0x1B, 0x5B, 0x3B, 0x7B}, 5, 0x0202, {{2, 0x0201, 0xFE, false}}, 0x0001, true},
  };
  for (const auto& mode : modes) {
    for (unsigned operation = 0; operation < 4; ++operation) {
      for (const uint8_t old_value : std::initializer_list<uint8_t>{0x00, 0x01, 0x7F, 0x80, 0x81, 0xFF}) {
        for (const uint8_t flags : std::initializer_list<uint8_t>{0x00, 0x01, 0xFE, 0xFF}) {
          CAPTURE(mode.opcodes[operation], old_value, flags);
          Fixture f{mode.opcodes[operation]};
          for (const Access& access : mode.fetches) {
            f.bus.memory[access.address] = access.value;
          }
          auto initial = f.cpu.GetState();
          initial.psw = flags;
          const bool accumulator = mode.cycles == 2;
          const uint16_t address =
              static_cast<uint16_t>(mode.address | (mode.direct_page && (flags & 0x20) != 0 ? 0x100 : 0));
          if (accumulator) {
            initial.a = old_value;
          } else {
            f.bus.memory[address] = old_value;
          }
          f.cpu.Reset(initial);
          f.Tick(mode.cycles - 1);
          REQUIRE(f.cpu.GetState().psw == flags);
          REQUIRE(f.cpu.GetState().a == initial.a);
          if (!accumulator) {
            REQUIRE(f.bus.memory[address] == old_value);
            f.bus.memory[address] = 0x55;
          }
          f.Tick();
          const auto expected_result = Shift(operation, old_value, flags);
          REQUIRE(f.cpu.GetState().psw == expected_result.flags);
          REQUIRE(f.cpu.GetState().x == initial.x);
          REQUIRE(f.cpu.GetState().y == initial.y);
          REQUIRE(f.cpu.GetState().sp == initial.sp);
          std::vector<Access> expected{{1, 0x0200, mode.opcodes[operation], false}};
          expected.insert(expected.end(), mode.fetches.begin(), mode.fetches.end());
          if (accumulator) {
            REQUIRE(f.cpu.GetState().a == expected_result.value);
          } else {
            REQUIRE(f.cpu.GetState().a == initial.a);
            REQUIRE(f.bus.memory[address] == expected_result.value);
            expected.push_back({mode.cycles - 1, address, old_value, false});
            expected.push_back({mode.cycles, address, expected_result.value, true});
          }
          REQUIRE(f.bus.accesses == expected);
          f.NextFetch(mode.next_pc, mode.cycles + 1);
        }
      }
    }
  }
}

TEST_CASE("SPC700 carry-bit logic decodes all bit positions and preserves its read versus idle flag timing",
          "[unit][apu][spc700][spc700_logic]") {
  constexpr std::array<uint8_t, 5> kOpcodes{0x0A, 0x2A, 0x4A, 0x6A, 0x8A};
  for (unsigned operation = 0; operation < 5; ++operation) {
    for (const uint16_t address : std::initializer_list<uint16_t>{0x0000, 0x1FFF}) {
      for (unsigned bit = 0; bit < 8; ++bit) {
        for (const bool set : {false, true}) {
          for (const bool carry : {false, true}) {
            for (const uint8_t seed : std::initializer_list<uint8_t>{0x00, 0xFE}) {
              CAPTURE(operation, address, bit, set, carry, seed);
              const uint8_t low = static_cast<uint8_t>(address);
              const uint8_t high = static_cast<uint8_t>((address >> 8) | (bit << 5));
              const uint8_t data = set ? static_cast<uint8_t>(1U << bit) : static_cast<uint8_t>(~(1U << bit));
              const uint8_t flags = static_cast<uint8_t>(seed | (carry ? 1 : 0));
              Fixture f{kOpcodes[operation], low, high};
              auto initial = f.cpu.GetState();
              initial.psw = flags;
              f.cpu.Reset(initial);
              f.bus.memory[address] = data;
              f.Tick(3);
              REQUIRE(f.cpu.GetState().psw == flags);
              f.Tick();
              const unsigned cycles = operation == 2 || operation == 3 ? 4 : 5;
              if (cycles == 5) {
                REQUIRE(f.cpu.GetState().psw == flags);
                f.bus.memory[address] = static_cast<uint8_t>(~data);
                f.Tick();
              }
              const bool expected_carry = operation == 0   ? carry || set
                                          : operation == 1 ? carry || !set
                                          : operation == 2 ? carry && set
                                          : operation == 3 ? carry && !set
                                                           : carry != set;
              REQUIRE(f.cpu.GetState().psw == (seed | (expected_carry ? 1 : 0)));
              REQUIRE(f.cpu.GetState().a == initial.a);
              REQUIRE(f.cpu.GetState().x == initial.x);
              REQUIRE(f.cpu.GetState().y == initial.y);
              REQUIRE(f.bus.accesses == std::vector<Access>{{1, 0x0200, kOpcodes[operation], false},
                                                            {2, 0x0201, low, false},
                                                            {3, 0x0202, high, false},
                                                            {4, address, data, false}});
              f.NextFetch(0x0203, cycles + 1);
            }
          }
        }
      }
    }
  }
}

TEST_CASE("SPC700 NOT1 toggles only the selected bit of the latched thirteen-bit-addressed byte",
          "[unit][apu][spc700][spc700_logic]") {
  for (const uint16_t address : std::initializer_list<uint16_t>{0x0000, 0x1FFF}) {
    for (unsigned bit = 0; bit < 8; ++bit) {
      for (const uint8_t old_value : std::initializer_list<uint8_t>{0x00, 0xFF, 0xA5}) {
        for (const uint8_t flags : std::initializer_list<uint8_t>{0x00, 0xFF}) {
          CAPTURE(address, bit, old_value, flags);
          const uint8_t low = static_cast<uint8_t>(address);
          const uint8_t high = static_cast<uint8_t>((address >> 8) | (bit << 5));
          Fixture f{0xEA, low, high};
          auto initial = f.cpu.GetState();
          initial.psw = flags;
          f.cpu.Reset(initial);
          f.bus.memory[address] = old_value;
          f.Tick(4);
          REQUIRE(f.bus.memory[address] == old_value);
          REQUIRE(f.cpu.GetState().psw == flags);
          f.bus.memory[address] = 0x55;
          f.Tick();
          const uint8_t value = static_cast<uint8_t>(old_value ^ (1U << bit));
          REQUIRE(f.bus.memory[address] == value);
          REQUIRE(f.cpu.GetState().psw == flags);
          REQUIRE(f.cpu.GetState().a == initial.a);
          REQUIRE(f.cpu.GetState().x == initial.x);
          REQUIRE(f.cpu.GetState().y == initial.y);
          REQUIRE(f.bus.accesses == std::vector<Access>{{1, 0x0200, 0xEA, false},
                                                        {2, 0x0201, low, false},
                                                        {3, 0x0202, high, false},
                                                        {4, address, old_value, false},
                                                        {5, address, value, true}});
          f.NextFetch(0x0203, 6);
        }
      }
    }
  }
}

TEST_CASE("SPC700 SET1 and CLR1 cover all opcode bit positions and preserve flags on both direct pages",
          "[unit][apu][spc700][spc700_logic]") {
  for (const bool set : {false, true}) {
    for (unsigned bit = 0; bit < 8; ++bit) {
      for (const uint8_t old_value : std::initializer_list<uint8_t>{0x00, 0xFF, 0xA5}) {
        for (const uint8_t flags : std::initializer_list<uint8_t>{0xDF, 0xFF}) {
          CAPTURE(set, bit, old_value, flags);
          const uint8_t opcode = static_cast<uint8_t>((bit << 5) | (set ? 0x02 : 0x12));
          const uint16_t address = flags == 0xDF ? 0x00FF : 0x01FF;
          Fixture f{opcode, 0xFF};
          auto initial = f.cpu.GetState();
          initial.psw = flags;
          f.cpu.Reset(initial);
          f.bus.memory[address] = old_value;
          f.Tick(3);
          REQUIRE(f.bus.memory[address] == old_value);
          REQUIRE(f.cpu.GetState().psw == flags);
          f.bus.memory[address] = 0x55;
          f.Tick();
          const uint8_t value = static_cast<uint8_t>(set ? old_value | (1U << bit) : old_value & ~(1U << bit));
          REQUIRE(f.bus.memory[address] == value);
          REQUIRE(f.cpu.GetState().psw == flags);
          REQUIRE(f.cpu.GetState().a == initial.a);
          REQUIRE(f.cpu.GetState().x == initial.x);
          REQUIRE(f.cpu.GetState().y == initial.y);
          REQUIRE(f.bus.accesses == std::vector<Access>{{1, 0x0200, opcode, false},
                                                        {2, 0x0201, 0xFF, false},
                                                        {3, address, old_value, false},
                                                        {4, address, value, true}});
          f.NextFetch(0x0202, 5);
        }
      }
    }
  }
}

TEST_CASE("SPC700 TSET1 and TCLR1 compare the first read then perform a distinct dummy read before writing",
          "[unit][apu][spc700][spc700_logic]") {
  struct Operands {
    uint8_t a;
    uint8_t memory;
  };
  constexpr std::array kOperands{Operands{0x00, 0x00}, Operands{0x00, 0x01}, Operands{0x80, 0x01}, Operands{0x7F, 0x80},
                                 Operands{0x55, 0x55}, Operands{0xFF, 0x80}, Operands{0x0F, 0xF0}};
  for (const uint8_t opcode : std::initializer_list<uint8_t>{0x0E, 0x4E}) {
    for (const auto& operands : kOperands) {
      for (const uint8_t flags : std::initializer_list<uint8_t>{0x00, 0xFF}) {
        CAPTURE(opcode, operands.a, operands.memory, flags);
        Fixture f{opcode, 0xFF, 0xFF};
        auto initial = f.cpu.GetState();
        initial.a = operands.a;
        initial.psw = flags;
        f.cpu.Reset(initial);
        f.bus.memory[0xFFFF] = operands.memory;
        f.Tick(3);
        REQUIRE(f.cpu.GetState().psw == flags);
        f.Tick();
        const uint8_t difference = static_cast<uint8_t>(static_cast<int>(operands.a) - operands.memory);
        const uint8_t expected_flags =
            static_cast<uint8_t>((flags & 0x7D) | (difference == 0 ? 2 : 0) | (difference & 0x80));
        REQUIRE(f.cpu.GetState().psw == expected_flags);
        REQUIRE(f.bus.memory[0xFFFF] == operands.memory);
        f.bus.memory[0xFFFF] = 0xAA;
        f.Tick();
        REQUIRE(f.bus.memory[0xFFFF] == 0xAA);
        REQUIRE(f.cpu.GetState().psw == expected_flags);
        f.bus.memory[0xFFFF] = 0x55;
        f.Tick();
        const uint8_t result =
            static_cast<uint8_t>(opcode == 0x0E ? operands.memory | operands.a : operands.memory & ~operands.a);
        REQUIRE(f.bus.memory[0xFFFF] == result);
        REQUIRE(f.cpu.GetState().psw == expected_flags);
        REQUIRE(f.cpu.GetState().a == initial.a);
        REQUIRE(f.cpu.GetState().x == initial.x);
        REQUIRE(f.cpu.GetState().y == initial.y);
        REQUIRE(f.bus.accesses == std::vector<Access>{{1, 0x0200, opcode, false},
                                                      {2, 0x0201, 0xFF, false},
                                                      {3, 0x0202, 0xFF, false},
                                                      {4, 0xFFFF, operands.memory, false},
                                                      {5, 0xFFFF, 0xAA, false},
                                                      {6, 0xFFFF, result, true}});
        f.NextFetch(0x0203, 7);
      }
    }
  }
}

TEST_CASE("SPC700 XCN exchanges every pair of nibbles and preserves non-NZ flags",
          "[unit][apu][spc700][spc700_logic]") {
  Fixture f{0x9F};
  f.bus.record = false;
  uint64_t checked = 0;
  for (unsigned value = 0; value < 256; ++value) {
    for (const uint8_t flags : std::initializer_list<uint8_t>{0x00, 0xFF}) {
      f.cpu.Reset(Spc700::State{
          .a = static_cast<uint8_t>(value), .x = 0x32, .y = 0x54, .sp = 0x76, .psw = flags, .pc = 0x0200});
      for (unsigned cycle = 0; cycle < 5; ++cycle) {
        f.cpu.TickCycle();
      }
      const uint8_t result = static_cast<uint8_t>((value % 16) * 16 + value / 16);
      const uint8_t expected_flags =
          static_cast<uint8_t>((flags & 0x7D) | (result == 0 ? 2 : 0) | (result >= 128 ? 0x80 : 0));
      const auto& state = f.cpu.GetState();
      if (state.a != result || state.psw != expected_flags || state.faulted || state.pc != 0x0201 || state.x != 0x32 ||
          state.y != 0x54 || state.sp != 0x76 || state.cycles != 5) {
        CAPTURE(value, flags, result, expected_flags);
        REQUIRE(state.a == result);
        REQUIRE(state.psw == expected_flags);
        REQUIRE_FALSE(state.faulted);
        REQUIRE(state.pc == 0x0201);
        REQUIRE(state.x == 0x32);
        REQUIRE(state.y == 0x54);
        REQUIRE(state.sp == 0x76);
        REQUIRE(state.cycles == 5);
      }
      ++checked;
    }
  }
  REQUIRE(checked == 512);
}

TEST_CASE("SPC700 XCN changes A and NZ only on its fifth cycle", "[unit][apu][spc700][spc700_logic]") {
  for (const uint8_t value : std::initializer_list<uint8_t>{0x00, 0x0F, 0xF0, 0x80, 0xFF, 0x55}) {
    for (const uint8_t flags : std::initializer_list<uint8_t>{0x00, 0xFF}) {
      CAPTURE(value, flags);
      Fixture f{0x9F, 0x00};
      auto initial = f.cpu.GetState();
      initial.a = value;
      initial.psw = flags;
      f.cpu.Reset(initial);
      f.Tick(4);
      REQUIRE(f.cpu.GetState().a == value);
      REQUIRE(f.cpu.GetState().psw == flags);
      f.Tick();
      const uint8_t result = static_cast<uint8_t>((value % 16) * 16 + value / 16);
      REQUIRE(f.cpu.GetState().a == result);
      REQUIRE(f.cpu.GetState().psw == ((flags & 0x7D) | (result == 0 ? 2 : 0) | (result & 0x80)));
      REQUIRE(f.bus.accesses == std::vector<Access>{{1, 0x0200, 0x9F, false}, {2, 0x0201, 0, false}});
      f.NextFetch(0x0201, 6);
    }
  }
}

TEST_CASE("SPC700 flag instructions preserve unrelated flags and retain their two or three cycle timing",
          "[unit][apu][spc700][spc700_logic]") {
  struct Example {
    uint8_t opcode;
    unsigned cycles;
    uint8_t clear;
    uint8_t set;
    uint8_t toggle;
  };
  constexpr std::array kCases{Example{0x60, 2, 0x01, 0, 0}, Example{0x80, 2, 0, 0x01, 0}, Example{0xED, 3, 0, 0, 0x01},
                              Example{0xE0, 2, 0x48, 0, 0}, Example{0xA0, 3, 0, 0x04, 0}, Example{0xC0, 3, 0x04, 0, 0},
                              Example{0x20, 2, 0x20, 0, 0}, Example{0x40, 2, 0, 0x20, 0}};
  for (const auto& example : kCases) {
    for (const uint8_t flags : std::initializer_list<uint8_t>{0x00, 0xFF, 0x55, 0xAA}) {
      CAPTURE(example.opcode, flags);
      Fixture f{example.opcode, 0x00};
      auto initial = f.cpu.GetState();
      initial.psw = flags;
      f.cpu.Reset(initial);
      f.Tick(example.cycles - 1);
      REQUIRE(f.cpu.GetState().psw == flags);
      f.Tick();
      const uint8_t expected_flags = static_cast<uint8_t>(((flags & ~example.clear) | example.set) ^ example.toggle);
      REQUIRE(f.cpu.GetState().psw == expected_flags);
      REQUIRE(f.cpu.GetState().a == initial.a);
      REQUIRE(f.cpu.GetState().x == initial.x);
      REQUIRE(f.cpu.GetState().y == initial.y);
      REQUIRE(f.cpu.GetState().sp == initial.sp);
      REQUIRE(f.bus.accesses == std::vector<Access>{{1, 0x0200, example.opcode, false}, {2, 0x0201, 0, false}});
      f.NextFetch(0x0201, example.cycles + 1);
    }
  }
}
