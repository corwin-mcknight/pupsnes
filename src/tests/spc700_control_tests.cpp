#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <initializer_list>
#include <vector>

#include "pupsnes/hw/apu/spc700.h"

namespace {

using pupsnes::Spc700;
using pupsnes::Spc700Bus;

// Bus schedules follow the independently audited ares SPC700 implementation:
// https://github.com/ares-emulator/ares/blob/master/ares/component/processor/spc700/instructions.cpp
// Literal byte counts and base cycles were transcribed from Anomie's opcode
// table, with conditional branches configured not taken in the decode sweep:
// https://github.com/gilligan/snesdev/blob/master/docs/spc700.txt
// EF/FF use the first fetch/read/idle triplet of ares's repeating halt loop.
constexpr std::array<uint8_t, 256> kInstructionBytes = {
    1, 1, 2, 3, 2, 3, 1, 2, 2, 3, 3, 2, 3, 1, 3, 1,  // 0x
    2, 1, 2, 3, 2, 3, 3, 2, 3, 1, 2, 2, 1, 1, 3, 3,  // 1x
    1, 1, 2, 3, 2, 3, 1, 2, 2, 3, 3, 2, 3, 1, 3, 2,  // 2x
    2, 1, 2, 3, 2, 3, 3, 2, 3, 1, 2, 2, 1, 1, 2, 3,  // 3x
    1, 1, 2, 3, 2, 3, 1, 2, 2, 3, 3, 2, 3, 1, 3, 2,  // 4x
    2, 1, 2, 3, 2, 3, 3, 2, 3, 1, 2, 2, 1, 1, 3, 3,  // 5x
    1, 1, 2, 3, 2, 3, 1, 2, 2, 3, 3, 2, 3, 1, 3, 1,  // 6x
    2, 1, 2, 3, 2, 3, 3, 2, 3, 1, 2, 2, 1, 1, 2, 1,  // 7x
    1, 1, 2, 3, 2, 3, 1, 2, 2, 3, 3, 2, 3, 2, 1, 3,  // 8x
    2, 1, 2, 3, 2, 3, 3, 2, 3, 1, 2, 2, 1, 1, 1, 1,  // 9x
    1, 1, 2, 3, 2, 3, 1, 2, 2, 3, 3, 2, 3, 2, 1, 1,  // Ax
    2, 1, 2, 3, 2, 3, 3, 2, 3, 1, 2, 2, 1, 1, 1, 1,  // Bx
    1, 1, 2, 3, 2, 3, 1, 2, 2, 3, 3, 2, 3, 2, 1, 1,  // Cx
    2, 1, 2, 3, 2, 3, 3, 2, 2, 2, 2, 2, 1, 1, 3, 1,  // Dx
    1, 1, 2, 3, 2, 3, 1, 2, 2, 3, 3, 2, 3, 1, 1, 1,  // Ex
    2, 1, 2, 3, 2, 3, 3, 2, 2, 2, 3, 2, 1, 1, 2, 1,  // Fx
};
constexpr std::array<uint8_t, 256> kInstructionCycles = {
    2, 8, 4, 5, 3, 4, 3, 6, 2, 6, 5, 4, 5, 4, 6,  8,  // 0x
    2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 6, 5, 2, 2, 4,  6,  // 1x
    2, 8, 4, 5, 3, 4, 3, 6, 2, 6, 5, 4, 5, 4, 5,  4,  // 2x
    2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 6, 5, 2, 2, 3,  8,  // 3x
    2, 8, 4, 5, 3, 4, 3, 6, 2, 6, 4, 4, 5, 4, 6,  6,  // 4x
    2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 4, 5, 2, 2, 4,  3,  // 5x
    2, 8, 4, 5, 3, 4, 3, 6, 2, 6, 4, 4, 5, 4, 5,  5,  // 6x
    2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 5, 5, 2, 2, 3,  6,  // 7x
    2, 8, 4, 5, 3, 4, 3, 6, 2, 6, 5, 4, 5, 2, 4,  5,  // 8x
    2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 5, 5, 2, 2, 12, 5,  // 9x
    3, 8, 4, 5, 3, 4, 3, 6, 2, 6, 4, 4, 5, 2, 4,  4,  // Ax
    2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 5, 5, 2, 2, 3,  4,  // Bx
    3, 8, 4, 5, 4, 5, 4, 7, 2, 5, 6, 4, 5, 2, 4,  9,  // Cx
    2, 8, 4, 5, 5, 6, 6, 7, 4, 5, 5, 5, 2, 2, 6,  3,  // Dx
    2, 8, 4, 5, 3, 4, 3, 6, 2, 4, 5, 3, 4, 3, 4,  3,  // Ex
    2, 8, 4, 5, 4, 5, 5, 6, 3, 4, 5, 4, 2, 2, 4,  3,  // Fx
};

struct Access {
  uint64_t cycle;
  uint16_t address;
  uint8_t value;
  bool write;
  bool operator==(const Access&) const = default;
};

struct RecordingBus final : Spc700Bus {
  uint8_t Read(uint16_t address) override {
    accesses.push_back({cycle, address, memory[address], false});
    return memory[address];
  }
  void Write(uint16_t address, uint8_t value) override {
    accesses.push_back({cycle, address, value, true});
    memory[address] = value;
  }

  std::array<uint8_t, 65536> memory{};
  std::vector<Access> accesses;
  uint64_t cycle = 0;
};

struct Fixture {
  Fixture() { cpu.Reset(Spc700::State{.a = 0x11, .x = 2, .y = 3, .sp = 0x44, .psw = 0xE5, .pc = 0x0200}); }

  void Put(uint16_t address, std::initializer_list<uint8_t> bytes) {
    for (const auto byte : bytes) {
      bus.memory[address++] = byte;
    }
  }

  void Tick(unsigned count = 1) {
    for (unsigned i = 0; i < count; ++i) {
      bus.cycle = cpu.GetState().cycles + 1;
      const auto before = bus.accesses.size();
      cpu.TickCycle();
      REQUIRE(cpu.GetState().cycles == bus.cycle);
      REQUIRE(bus.accesses.size() - before <= 1);
      REQUIRE_FALSE(cpu.GetState().faulted);
    }
  }

  void NextFetch(uint16_t address, uint64_t cycle) {
    REQUIRE(cpu.GetState().pc == address);
    const auto before = bus.accesses.size();
    Tick();
    REQUIRE(bus.accesses.size() == before + 1);
    REQUIRE(bus.accesses.back() == Access{cycle, address, bus.memory[address], false});
    REQUIRE(cpu.GetState().pc == static_cast<uint16_t>(address + 1));
  }

  RecordingBus bus;
  Spc700 cpu{bus};
};

struct ConditionalBranch {
  uint8_t opcode;
  uint8_t flag;
  bool when_set;
};
constexpr std::array kConditionalBranches = {
    ConditionalBranch{0x10, 0x80, false}, ConditionalBranch{0x30, 0x80, true},  ConditionalBranch{0x50, 0x40, false},
    ConditionalBranch{0x70, 0x40, true},  ConditionalBranch{0x90, 0x01, false}, ConditionalBranch{0xB0, 0x01, true},
    ConditionalBranch{0xD0, 0x02, false}, ConditionalBranch{0xF0, 0x02, true},
};

uint16_t BranchTarget(uint16_t after_operand, uint8_t displacement) {
  const int signed_displacement = displacement < 0x80 ? displacement : static_cast<int>(displacement) - 256;
  return static_cast<uint16_t>(static_cast<int>(after_operand) + signed_displacement);
}

}  // namespace

TEST_CASE("SPC700 all 256 opcodes execute to their independently specified first boundary",
          "[unit][apu][spc700][decode]") {
  for (unsigned code = 0; code < 256; ++code) {
    DYNAMIC_SECTION("opcode " << code) {
      const auto opcode = static_cast<uint8_t>(code);
      Fixture f;
      auto state = Spc700::State{.a = 1, .x = 1, .y = 1, .sp = 0x80, .pc = 0x4000};
      f.Put(0x4000, {opcode, 0x20, 0x20});
      uint16_t expected_pc = static_cast<uint16_t>(0x4000 + kInstructionBytes[code]);

      for (const auto& branch : kConditionalBranches) {
        if (opcode == branch.opcode) {
          state.psw = branch.when_set ? 0 : branch.flag;
          f.bus.memory[0x4001] = 0;
        }
      }
      if (opcode == 0x2F || opcode == 0xFE) {
        f.bus.memory[0x4001] = 0;
      } else if ((opcode & 0x0F) == 0x03) {
        // Each BBC sees its bit set; each BBS sees it clear.
        f.bus.memory[0x20] = (opcode & 0x10) != 0 ? static_cast<uint8_t>(1U << (opcode >> 5U)) : 0;
        f.bus.memory[0x4002] = 0;
      } else if (opcode == 0x2E || opcode == 0xDE) {
        f.bus.memory[opcode == 0x2E ? 0x20 : 0x21] = state.a;
        f.bus.memory[0x4002] = 0;
      } else if (opcode == 0x6E) {
        f.bus.memory[0x20] = 1;
        f.bus.memory[0x4002] = 0;
      }

      if ((opcode & 0x0F) == 0x01) {
        const auto vector = static_cast<uint16_t>(0xFFDE - 2 * (opcode >> 4U));
        f.Put(vector, {0x34, 0x12});
        expected_pc = 0x1234;
      } else {
        switch (opcode) {
          case 0x0F:
            f.Put(0xFFDE, {0x34, 0x12});
            expected_pc = 0x1234;
            break;
          case 0x1F:
            f.Put(0x2021, {0x34, 0x12});
            expected_pc = 0x1234;
            break;
          case 0x3F:
          case 0x5F: expected_pc = 0x2020; break;
          case 0x4F: expected_pc = 0xFF20; break;
          case 0x6F:
            f.Put(0x0181, {0x34, 0x12});
            expected_pc = 0x1234;
            break;
          case 0x7F:
            f.Put(0x0181, {0x00, 0x34, 0x12});
            expected_pc = 0x1234;
            break;
          default: break;
        }
      }
      f.cpu.Reset(state);
      REQUIRE(kInstructionBytes[code] >= 1);
      REQUIRE(kInstructionBytes[code] <= 3);
      REQUIRE(kInstructionCycles[code] >= 2);
      REQUIRE(kInstructionCycles[code] <= 12);
      f.Tick(kInstructionCycles[code]);
      REQUIRE(f.cpu.GetState().pc == expected_pc);
      REQUIRE(f.cpu.GetState().sleeping == (opcode == 0xEF));
      REQUIRE(f.cpu.GetState().stopped == (opcode == 0xFF));
      if (opcode != 0xEF && opcode != 0xFF) {
        f.NextFetch(expected_pc, kInstructionCycles[code] + 1U);
      }
    }
  }
}

TEST_CASE("SPC700 every flag branch preserves flags and wraps signed displacements", "[unit][apu][spc700][control]") {
  for (const auto& branch : kConditionalBranches) {
    for (const bool take : {false, true}) {
      for (const uint16_t start : std::initializer_list<uint16_t>{0x0001, 0xFFFD}) {
        for (const uint8_t displacement : std::initializer_list<uint8_t>{0x00, 0x7F, 0x80, 0xFF}) {
          CAPTURE(branch.opcode, take, start, displacement);
          Fixture f;
          const bool flag_set = take == branch.when_set;
          const auto flags = static_cast<uint8_t>((0x2D & ~branch.flag) | (flag_set ? branch.flag : 0));
          f.Put(start, {branch.opcode, displacement});
          f.cpu.Reset(Spc700::State{.psw = flags, .pc = start});
          const auto after = static_cast<uint16_t>(start + 2);
          f.Tick(2);
          REQUIRE(f.cpu.GetState().pc == after);
          if (take) {
            f.Tick();
            REQUIRE(f.cpu.GetState().pc == after);
            f.Tick();
          }
          const auto target = take ? BranchTarget(after, displacement) : after;
          REQUIRE(f.cpu.GetState().pc == target);
          REQUIRE(f.cpu.GetState().psw == flags);
          REQUIRE(f.bus.accesses == std::vector<Access>{{1, start, branch.opcode, false},
                                                        {2, static_cast<uint16_t>(start + 1), displacement, false}});
          f.NextFetch(target, take ? 5 : 3);
        }
      }
    }
  }
}

TEST_CASE("SPC700 BRA always takes exactly four cycles even with a zero displacement", "[unit][apu][spc700][control]") {
  for (const uint16_t start : std::initializer_list<uint16_t>{0x0001, 0xFFFD}) {
    for (const uint8_t displacement : std::initializer_list<uint8_t>{0x00, 0x7F, 0x80, 0xFF}) {
      CAPTURE(start, displacement);
      Fixture f;
      f.Put(start, {0x2F, displacement});
      f.cpu.Reset(Spc700::State{.psw = 0xFF, .pc = start});
      const auto after = static_cast<uint16_t>(start + 2);
      f.Tick(3);
      REQUIRE(f.cpu.GetState().pc == after);
      f.Tick();
      REQUIRE(f.cpu.GetState().psw == 0xFF);
      REQUIRE(f.bus.accesses.size() == 2);
      f.NextFetch(BranchTarget(after, displacement), 5);
    }
  }
}

TEST_CASE("SPC700 BBS and BBC test all bits in the selected page using the latched read",
          "[unit][apu][spc700][control]") {
  for (unsigned bit = 0; bit < 8; ++bit) {
    for (const bool branch_on_set : {false, true}) {
      for (const bool take : {false, true}) {
        for (const uint8_t page : std::initializer_list<uint8_t>{0, 0x20}) {
          for (const uint8_t displacement : std::initializer_list<uint8_t>{0x7F, 0x80}) {
            CAPTURE(bit, branch_on_set, take, page, displacement);
            Fixture f;
            const auto opcode = static_cast<uint8_t>((bit << 5U) | (branch_on_set ? 0x03 : 0x13));
            const auto mask = static_cast<uint8_t>(1U << bit);
            const auto value = static_cast<uint8_t>((0xA5 & ~mask) | (take == branch_on_set ? mask : 0));
            const auto flags = static_cast<uint8_t>(0xDD | page);
            const uint16_t address = page == 0 ? 0x0020 : 0x0120;
            f.Put(0xFFFC, {opcode, 0x20, displacement});
            f.cpu.Reset(Spc700::State{.psw = flags, .pc = 0xFFFC});
            f.bus.memory[address] = value;
            f.bus.memory[address ^ 0x0100] = static_cast<uint8_t>(value ^ mask);
            f.Tick(3);
            REQUIRE(f.bus.accesses.back() == Access{3, address, value, false});
            REQUIRE(f.cpu.GetState().pc == 0xFFFE);
            f.bus.memory[address] = static_cast<uint8_t>(value ^ mask);
            f.Tick(2);
            REQUIRE(f.cpu.GetState().pc == 0xFFFF);
            if (take) {
              f.Tick();
              REQUIRE(f.cpu.GetState().pc == 0xFFFF);
              f.Tick();
            }
            const auto target = take ? BranchTarget(0xFFFF, displacement) : uint16_t{0xFFFF};
            REQUIRE(f.cpu.GetState().psw == flags);
            REQUIRE(f.bus.accesses == std::vector<Access>{{1, 0xFFFC, opcode, false},
                                                          {2, 0xFFFD, 0x20, false},
                                                          {3, address, value, false},
                                                          {5, 0xFFFE, displacement, false}});
            f.NextFetch(target, take ? 8 : 6);
          }
        }
      }
    }
  }
}

TEST_CASE("SPC700 CBNE forms wrap direct indexing and compare latched data without changing flags",
          "[unit][apu][spc700][control]") {
  for (const bool indexed : {false, true}) {
    for (const bool take : {false, true}) {
      for (const uint8_t page : std::initializer_list<uint8_t>{0, 0x20}) {
        CAPTURE(indexed, take, page);
        Fixture f;
        const auto opcode = static_cast<uint8_t>(indexed ? 0xDE : 0x2E);
        const auto flags = static_cast<uint8_t>(0xC3 | page);
        const auto address = static_cast<uint16_t>((page == 0 ? 0x0000 : 0x0100) | (indexed ? 0x01 : 0xFF));
        const auto value = static_cast<uint8_t>(take ? 0x7F : 0x7E);
        const unsigned read_cycle = indexed ? 4 : 3;
        const unsigned base_cycles = indexed ? 6 : 5;
        f.Put(0xFFFD, {opcode, 0xFF, 0x80});
        f.cpu.Reset(Spc700::State{.a = 0x7E, .x = 2, .psw = flags, .pc = 0xFFFD});
        f.bus.memory[address] = value;
        f.Tick(read_cycle);
        REQUIRE(f.bus.accesses.back() == Access{read_cycle, address, value, false});
        f.bus.memory[address] = static_cast<uint8_t>(take ? 0x7E : 0x7F);
        f.Tick(base_cycles - read_cycle);
        REQUIRE(f.cpu.GetState().pc == 0);
        if (take) {
          f.Tick();
          REQUIRE(f.cpu.GetState().pc == 0);
          f.Tick();
        }
        REQUIRE(f.cpu.GetState().a == 0x7E);
        REQUIRE(f.cpu.GetState().x == 2);
        REQUIRE(f.cpu.GetState().psw == flags);
        REQUIRE(f.bus.accesses == std::vector<Access>{{1, 0xFFFD, opcode, false},
                                                      {2, 0xFFFE, 0xFF, false},
                                                      {read_cycle, address, value, false},
                                                      {base_cycles, 0xFFFF, 0x80, false}});
        f.NextFetch(take ? 0xFF80 : 0x0000, base_cycles + (take ? 3 : 1));
      }
    }
  }
}

TEST_CASE("SPC700 DBNZ direct writes its latched decrement before fetching the branch operand",
          "[unit][apu][spc700][control]") {
  for (const uint8_t initial : std::initializer_list<uint8_t>{0, 1, 2}) {
    for (const uint8_t page : std::initializer_list<uint8_t>{0, 0x20}) {
      CAPTURE(initial, page);
      Fixture f;
      const auto flags = static_cast<uint8_t>(0xDD | page);
      const uint16_t address = page == 0 ? 0x00FF : 0x01FF;
      const auto decremented = static_cast<uint8_t>(initial - 1);
      const bool take = decremented != 0;
      f.Put(0xFFFD, {0x6E, 0xFF, 0x80});
      f.cpu.Reset(Spc700::State{.psw = flags, .pc = 0xFFFD});
      f.bus.memory[address] = initial;
      f.Tick(3);
      REQUIRE(f.bus.memory[address] == initial);
      REQUIRE(f.cpu.GetState().psw == flags);
      f.bus.memory[address] = 0x99;
      f.Tick();
      REQUIRE(f.bus.memory[address] == decremented);
      REQUIRE(f.cpu.GetState().pc == 0xFFFF);
      f.Tick();
      REQUIRE(f.cpu.GetState().pc == 0);
      if (take) {
        f.Tick(2);
      }
      REQUIRE(f.cpu.GetState().psw == flags);
      REQUIRE(f.bus.accesses == std::vector<Access>{{1, 0xFFFD, 0x6E, false},
                                                    {2, 0xFFFE, 0xFF, false},
                                                    {3, address, initial, false},
                                                    {4, address, decremented, true},
                                                    {5, 0xFFFF, 0x80, false}});
      f.NextFetch(take ? 0xFF80 : 0x0000, take ? 8 : 6);
    }
  }
}

TEST_CASE("SPC700 DBNZ Y decrements on displacement fetch and preserves every status bit",
          "[unit][apu][spc700][control]") {
  for (const uint8_t initial : std::initializer_list<uint8_t>{0, 1, 2}) {
    CAPTURE(initial);
    Fixture f;
    const auto decremented = static_cast<uint8_t>(initial - 1);
    const bool take = decremented != 0;
    f.Put(0xFFFE, {0xFE, 0x80});
    f.cpu.Reset(Spc700::State{.y = initial, .psw = 0xFF, .pc = 0xFFFE});
    f.Tick(3);
    REQUIRE(f.cpu.GetState().y == initial);
    REQUIRE(f.cpu.GetState().pc == 0xFFFF);
    f.Tick();
    REQUIRE(f.cpu.GetState().y == decremented);
    REQUIRE(f.cpu.GetState().pc == 0);
    if (take) {
      f.Tick(2);
    }
    REQUIRE(f.cpu.GetState().psw == 0xFF);
    REQUIRE(f.bus.accesses ==
            std::vector<Access>{{1, 0xFFFE, 0xFE, false}, {2, 0xFFFF, 0x80, false}, {4, 0xFFFF, 0x80, false}});
    f.NextFetch(take ? 0xFF80 : 0x0000, take ? 7 : 5);
  }
}

TEST_CASE("SPC700 JMP absolute latches its little-endian operand across PC wrap", "[unit][apu][spc700][control]") {
  Fixture f;
  f.Put(0xFFFE, {0x5F, 0x34, 0x12});
  f.cpu.Reset(Spc700::State{.psw = 0xE5, .pc = 0xFFFE});
  f.Tick(2);
  REQUIRE(f.cpu.GetState().pc == 0);
  f.bus.memory[0xFFFF] = 0xAB;
  f.Tick();
  REQUIRE(f.cpu.GetState().psw == 0xE5);
  REQUIRE(f.bus.accesses ==
          std::vector<Access>{{1, 0xFFFE, 0x5F, false}, {2, 0xFFFF, 0x34, false}, {3, 0x0000, 0x12, false}});
  f.NextFetch(0x1234, 4);
}

TEST_CASE("SPC700 JMP indirect X wraps its pointer and retains the low-byte sample", "[unit][apu][spc700][control]") {
  for (const uint8_t flags : std::initializer_list<uint8_t>{0xC5, 0xE5}) {
    CAPTURE(flags);
    Fixture f;
    f.Put(0x0200, {0x1F, 0xFE, 0xFF});
    f.cpu.Reset(Spc700::State{.x = 1, .psw = flags, .pc = 0x0200});
    f.bus.memory[0xFFFF] = 0x34;
    f.bus.memory[0x0000] = 0x12;
    f.Tick(5);
    REQUIRE(f.cpu.GetState().pc == 0x0203);
    f.bus.memory[0xFFFF] = 0xAB;
    f.bus.memory[0x0000] = 0x56;
    f.Tick();
    REQUIRE(f.cpu.GetState().psw == flags);
    REQUIRE(f.bus.accesses == std::vector<Access>{{1, 0x0200, 0x1F, false},
                                                  {2, 0x0201, 0xFE, false},
                                                  {3, 0x0202, 0xFF, false},
                                                  {5, 0xFFFF, 0x34, false},
                                                  {6, 0x0000, 0x56, false}});
    f.NextFetch(0x5634, 7);
  }
}

TEST_CASE("SPC700 CALL pushes the next PC high then low and commits its destination on cycle eight",
          "[unit][apu][spc700][control]") {
  for (const uint8_t flags : std::initializer_list<uint8_t>{0xC5, 0xE5}) {
    CAPTURE(flags);
    Fixture f;
    f.Put(0xFFFE, {0x3F, 0x34, 0x12});
    f.cpu.Reset(Spc700::State{.sp = 0x00, .psw = flags, .pc = 0xFFFE});
    f.Tick(4);
    REQUIRE(f.cpu.GetState().pc == 0x0001);
    REQUIRE(f.cpu.GetState().sp == 0x00);
    f.Tick();
    REQUIRE(f.cpu.GetState().sp == 0xFF);
    REQUIRE(f.bus.accesses.back() == Access{5, 0x0100, 0x00, true});
    f.Tick();
    REQUIRE(f.cpu.GetState().sp == 0xFE);
    REQUIRE(f.bus.accesses.back() == Access{6, 0x01FF, 0x01, true});
    f.Tick();
    REQUIRE(f.cpu.GetState().pc == 0x0001);
    f.Tick();
    REQUIRE(f.cpu.GetState().psw == flags);
    REQUIRE(f.bus.accesses == std::vector<Access>{{1, 0xFFFE, 0x3F, false},
                                                  {2, 0xFFFF, 0x34, false},
                                                  {3, 0x0000, 0x12, false},
                                                  {5, 0x0100, 0x00, true},
                                                  {6, 0x01FF, 0x01, true}});
    f.NextFetch(0x1234, 9);
  }
}

TEST_CASE("SPC700 PCALL forms a page-FF destination and pushes the wrapped return PC", "[unit][apu][spc700][control]") {
  for (const uint8_t flags : std::initializer_list<uint8_t>{0xC5, 0xE5}) {
    CAPTURE(flags);
    Fixture f;
    f.Put(0xFFFF, {0x4F, 0xA7});
    f.cpu.Reset(Spc700::State{.sp = 0, .psw = flags, .pc = 0xFFFF});
    f.Tick(3);
    REQUIRE(f.cpu.GetState().pc == 1);
    REQUIRE(f.cpu.GetState().sp == 0);
    f.Tick();
    REQUIRE(f.cpu.GetState().sp == 0xFF);
    f.Tick();
    REQUIRE(f.cpu.GetState().sp == 0xFE);
    REQUIRE(f.cpu.GetState().pc == 1);
    f.Tick();
    REQUIRE(f.cpu.GetState().psw == flags);
    REQUIRE(f.bus.accesses ==
            std::vector<Access>{
                {1, 0xFFFF, 0x4F, false}, {2, 0x0000, 0xA7, false}, {4, 0x0100, 0x00, true}, {5, 0x01FF, 0x01, true}});
    f.NextFetch(0xFFA7, 7);
  }
}

TEST_CASE("SPC700 all sixteen TCALL vectors descend from FFDE and latch each vector byte",
          "[unit][apu][spc700][control]") {
  for (unsigned number = 0; number < 16; ++number) {
    for (const uint8_t flags : std::initializer_list<uint8_t>{0xC5, 0xE5}) {
      CAPTURE(number, flags);
      Fixture f;
      const auto opcode = static_cast<uint8_t>((number << 4U) | 0x01);
      const auto vector = static_cast<uint16_t>(0xFFDE - 2 * number);
      f.Put(0xABCD, {opcode, 0x00});
      f.Put(vector, {0x34, 0x12});
      f.cpu.Reset(Spc700::State{.sp = 0, .psw = flags, .pc = 0xABCD});
      f.Tick(3);
      REQUIRE(f.cpu.GetState().sp == 0);
      f.Tick();
      REQUIRE(f.cpu.GetState().sp == 0xFF);
      REQUIRE(f.bus.memory[0x0100] == 0xAB);
      f.Tick();
      REQUIRE(f.cpu.GetState().sp == 0xFE);
      REQUIRE(f.bus.memory[0x01FF] == 0xCE);
      f.Tick(2);
      REQUIRE(f.cpu.GetState().pc == 0xABCE);
      REQUIRE(f.bus.accesses.back() == Access{7, vector, 0x34, false});
      f.bus.memory[vector] = 0xAB;
      f.bus.memory[vector + 1] = 0x56;
      f.Tick();
      REQUIRE(f.cpu.GetState().psw == flags);
      REQUIRE(f.bus.accesses == std::vector<Access>{{1, 0xABCD, opcode, false},
                                                    {2, 0xABCE, 0x00, false},
                                                    {4, 0x0100, 0xAB, true},
                                                    {5, 0x01FF, 0xCE, true},
                                                    {7, vector, 0x34, false},
                                                    {8, static_cast<uint16_t>(vector + 1), 0x56, false}});
      f.NextFetch(0x5634, 9);
    }
  }
}

TEST_CASE("SPC700 RET pulls a wrapped little-endian PC without touching flags", "[unit][apu][spc700][control]") {
  for (const uint8_t flags : std::initializer_list<uint8_t>{0xC5, 0xE5}) {
    CAPTURE(flags);
    Fixture f;
    f.Put(0x0200, {0x6F, 0x00});
    f.bus.memory[0x01FF] = 0x34;
    f.bus.memory[0x0100] = 0x12;
    f.cpu.Reset(Spc700::State{.sp = 0xFE, .psw = flags, .pc = 0x0200});
    f.Tick(3);
    REQUIRE(f.cpu.GetState().sp == 0xFE);
    f.Tick();
    REQUIRE(f.cpu.GetState().sp == 0xFF);
    REQUIRE(f.cpu.GetState().pc == 0x0201);
    f.bus.memory[0x01FF] = 0xAB;
    f.bus.memory[0x0100] = 0x56;
    f.Tick();
    REQUIRE(f.cpu.GetState().sp == 0);
    REQUIRE(f.cpu.GetState().psw == flags);
    REQUIRE(f.bus.accesses == std::vector<Access>{{1, 0x0200, 0x6F, false},
                                                  {2, 0x0201, 0x00, false},
                                                  {4, 0x01FF, 0x34, false},
                                                  {5, 0x0100, 0x56, false}});
    f.NextFetch(0x5634, 6);
  }
}

TEST_CASE("SPC700 RETI restores PSW before pulling PC and keeps the stack in page one",
          "[unit][apu][spc700][control]") {
  for (const uint8_t initial_flags : std::initializer_list<uint8_t>{0xC5, 0xE5}) {
    CAPTURE(initial_flags);
    Fixture f;
    const auto restored_flags = static_cast<uint8_t>(initial_flags == 0xC5 ? 0xE3 : 0xD7);
    f.Put(0x0200, {0x7F, 0x00});
    f.bus.memory[0x01FF] = restored_flags;
    f.Put(0x0100, {0x34, 0x12});
    f.cpu.Reset(Spc700::State{.sp = 0xFE, .psw = initial_flags, .pc = 0x0200});
    f.Tick(3);
    REQUIRE(f.cpu.GetState().psw == initial_flags);
    REQUIRE(f.cpu.GetState().sp == 0xFE);
    f.Tick();
    REQUIRE(f.cpu.GetState().psw == restored_flags);
    REQUIRE(f.cpu.GetState().sp == 0xFF);
    REQUIRE(f.cpu.GetState().pc == 0x0201);
    f.Tick();
    REQUIRE(f.cpu.GetState().sp == 0);
    REQUIRE(f.cpu.GetState().pc == 0x0201);
    f.bus.memory[0x0100] = 0xAB;
    f.bus.memory[0x0101] = 0x56;
    f.Tick();
    REQUIRE(f.cpu.GetState().sp == 1);
    REQUIRE(f.cpu.GetState().psw == restored_flags);
    REQUIRE(f.bus.accesses == std::vector<Access>{{1, 0x0200, 0x7F, false},
                                                  {2, 0x0201, 0x00, false},
                                                  {4, 0x01FF, restored_flags, false},
                                                  {5, 0x0100, 0x34, false},
                                                  {6, 0x0101, 0x56, false}});
    f.NextFetch(0x5634, 7);
  }
}

TEST_CASE("SPC700 BRK pushes the pre-break PSW before setting B and clearing I on vector completion",
          "[unit][apu][spc700][control]") {
  for (const uint8_t flags : std::initializer_list<uint8_t>{0x45, 0x65, 0xD7, 0xF7}) {
    CAPTURE(flags);
    Fixture f;
    f.Put(0x1234, {0x0F, 0xA5});
    f.Put(0xFFDE, {0x56, 0x78});
    f.cpu.Reset(Spc700::State{.a = 0x11, .x = 0x22, .y = 0x33, .sp = 1, .psw = flags, .pc = 0x1234});
    f.Tick(2);
    REQUIRE(f.cpu.GetState().pc == 0x1235);  // BRK's dummy read is not an operand fetch.
    REQUIRE(f.cpu.GetState().sp == 1);
    f.Tick();
    REQUIRE(f.cpu.GetState().sp == 0);
    REQUIRE(f.bus.memory[0x0101] == 0x12);
    f.Tick();
    REQUIRE(f.cpu.GetState().sp == 0xFF);
    REQUIRE(f.bus.memory[0x0100] == 0x35);
    f.Tick();
    REQUIRE(f.cpu.GetState().sp == 0xFE);
    REQUIRE(f.bus.memory[0x01FF] == flags);
    f.Tick(2);
    REQUIRE(f.cpu.GetState().psw == flags);
    REQUIRE(f.cpu.GetState().pc == 0x1235);
    f.Tick();
    REQUIRE(f.cpu.GetState().psw == static_cast<uint8_t>((flags | 0x10) & ~0x04));
    REQUIRE(f.cpu.GetState().a == 0x11);
    REQUIRE(f.cpu.GetState().x == 0x22);
    REQUIRE(f.cpu.GetState().y == 0x33);
    REQUIRE(f.bus.accesses == std::vector<Access>{{1, 0x1234, 0x0F, false},
                                                  {2, 0x1235, 0xA5, false},
                                                  {3, 0x0101, 0x12, true},
                                                  {4, 0x0100, 0x35, true},
                                                  {5, 0x01FF, flags, true},
                                                  {7, 0xFFDE, 0x56, false},
                                                  {8, 0xFFDF, 0x78, false}});
    f.NextFetch(0x7856, 9);
  }
}

TEST_CASE("SPC700 SLEEP and STOP keep separate halt state and repeat PC reads until reset",
          "[unit][apu][spc700][control]") {
  for (const uint8_t opcode : std::initializer_list<uint8_t>{0xEF, 0xFF}) {
    CAPTURE(opcode);
    Fixture f;
    f.Put(0x0200, {opcode, 0x8F, 0xAA, 0xF4});
    f.cpu.Reset(Spc700::State{.a = 0x11, .x = 0x22, .y = 0x33, .sp = 0x44, .psw = 0xE5, .pc = 0x0200});
    f.Tick(2);
    REQUIRE_FALSE(f.cpu.GetState().sleeping);
    REQUIRE_FALSE(f.cpu.GetState().stopped);
    f.Tick();
    REQUIRE(f.cpu.GetState().sleeping == (opcode == 0xEF));
    REQUIRE(f.cpu.GetState().stopped == (opcode == 0xFF));
    f.Tick(5);
    REQUIRE(f.bus.accesses == std::vector<Access>{{1, 0x0200, opcode, false},
                                                  {2, 0x0201, 0x8F, false},
                                                  {4, 0x0201, 0x8F, false},
                                                  {6, 0x0201, 0x8F, false},
                                                  {8, 0x0201, 0x8F, false}});
    f.bus.memory[0x00F4] = 0xCC;
    f.bus.memory[0x0201] = 0x00;
    f.Tick(4);
    REQUIRE(f.cpu.GetState().pc == 0x0201);
    REQUIRE(f.cpu.GetState().a == 0x11);
    REQUIRE(f.cpu.GetState().x == 0x22);
    REQUIRE(f.cpu.GetState().y == 0x33);
    REQUIRE(f.cpu.GetState().sp == 0x44);
    REQUIRE(f.cpu.GetState().psw == 0xE5);
    REQUIRE(f.cpu.GetState().sleeping == (opcode == 0xEF));
    REQUIRE(f.cpu.GetState().stopped == (opcode == 0xFF));
    REQUIRE(f.bus.memory[0xF4] == 0xCC);
    REQUIRE(f.bus.accesses.back() == Access{12, 0x0201, 0x00, false});

    f.Put(0xFFC0, {0xE8, 0x42});
    f.cpu.Reset();
    REQUIRE_FALSE(f.cpu.GetState().sleeping);
    REQUIRE_FALSE(f.cpu.GetState().stopped);
    f.Tick(2);
    REQUIRE(f.cpu.GetState().a == 0x42);
    REQUIRE(f.cpu.GetState().pc == 0xFFC2);
  }
}

TEST_CASE("SPC700 resetting into either halted state starts with the PC read phase", "[unit][apu][spc700][control]") {
  for (const bool sleeping : {false, true}) {
    CAPTURE(sleeping);
    Fixture f;
    f.Put(0x0200, {0x8F, 0xAA, 0xF4});
    f.cpu.Reset(Spc700::State{.pc = 0x0200, .stopped = !sleeping, .sleeping = sleeping});
    f.Tick(4);
    REQUIRE(f.cpu.GetState().pc == 0x0200);
    REQUIRE(f.cpu.GetState().sleeping == sleeping);
    REQUIRE(f.cpu.GetState().stopped == !sleeping);
    REQUIRE(f.bus.memory[0xF4] == 0);
    REQUIRE(f.bus.accesses == std::vector<Access>{{1, 0x0200, 0x8F, false}, {3, 0x0200, 0x8F, false}});
  }
}
