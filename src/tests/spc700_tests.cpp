#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <initializer_list>
#include <vector>

#include "pupsnes/hw/apu/spc700.h"

using pupsnes::Spc700;
using pupsnes::Spc700Bus;

namespace {

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
  Fixture(std::initializer_list<uint8_t> program) {
    uint16_t address = 0x0200;
    for (const uint8_t byte : program) {
      bus.memory[address++] = byte;
    }
    cpu.Reset(Spc700::State{.pc = 0x0200});
  }

  void Tick(unsigned count = 1) {
    for (unsigned i = 0; i < count; ++i) {
      bus.cycle = cpu.GetState().cycles + 1;
      const auto access_count = bus.accesses.size();
      cpu.TickCycle();
      REQUIRE(cpu.GetState().cycles == bus.cycle);
      REQUIRE(bus.accesses.size() - access_count <= 1);
    }
  }

  RecordingBus bus;
  Spc700 cpu{bus};
};

}  // namespace

TEST_CASE("SPC700 reset seeds the IPL entry and discards a pending write", "[unit][apu][spc700]") {
  Fixture f{0x8F, 0xAA, 0xF4};
  f.Tick(4);
  REQUIRE(f.bus.memory[0xF4] == 0);

  f.cpu.Reset();
  const auto& state = f.cpu.GetState();
  REQUIRE(state.pc == 0xFFC0);
  REQUIRE(state.a == 0);
  REQUIRE(state.x == 0);
  REQUIRE(state.y == 0);
  REQUIRE(state.sp == 0);
  REQUIRE(state.psw == 0);
  REQUIRE(state.cycles == 0);
  REQUIRE_FALSE(state.faulted);
  REQUIRE_FALSE(state.stopped);
  f.Tick();
  REQUIRE(f.bus.accesses.back() == Access{1, 0xFFC0, 0, false});
  REQUIRE(f.bus.memory[0xF4] == 0);
}

TEST_CASE("SPC700 immediate loads and implied operations complete on cycle two", "[unit][apu][spc700]") {
  struct Example {
    uint8_t opcode;
    uint8_t operand;
    uint8_t a;
    uint8_t x;
    uint8_t y;
    uint8_t sp;
    uint8_t psw;
  };
  // Initial A=$FF, X=$00, Y=$7F, SP=$44, PSW=$7F. Non-N/Z flags
  // survive every operation here; MOV SP,X preserves all flags.
  constexpr std::array kCases{
      Example{0xCD, 0x80, 0xFF, 0x80, 0x7F, 0x44, 0xFD}, Example{0xE8, 0x00, 0x00, 0x00, 0x7F, 0x44, 0x7F},
      Example{0x1D, 0x00, 0xFF, 0xFF, 0x7F, 0x44, 0xFD}, Example{0xBC, 0x00, 0x00, 0x00, 0x7F, 0x44, 0x7F},
      Example{0xFC, 0x00, 0xFF, 0x00, 0x80, 0x44, 0xFD}, Example{0xBD, 0x00, 0xFF, 0x00, 0x7F, 0x00, 0x7F},
      Example{0xDD, 0x00, 0x7F, 0x00, 0x7F, 0x44, 0x7D}, Example{0x5D, 0x00, 0xFF, 0xFF, 0x7F, 0x44, 0xFD},
      Example{0x00, 0x00, 0xFF, 0x00, 0x7F, 0x44, 0x7F},
  };
  for (const auto& example : kCases) {
    CAPTURE(example.opcode);
    Fixture f{example.opcode, example.operand};
    f.cpu.Reset(Spc700::State{.a = 0xFF, .x = 0, .y = 0x7F, .sp = 0x44, .psw = 0x7F, .pc = 0x0200});
    f.Tick();
    REQUIRE(f.cpu.GetState().a == 0xFF);
    REQUIRE(f.cpu.GetState().x == 0);
    REQUIRE(f.cpu.GetState().y == 0x7F);
    REQUIRE(f.cpu.GetState().sp == 0x44);
    REQUIRE(f.cpu.GetState().psw == 0x7F);
    f.Tick();
    const auto& state = f.cpu.GetState();
    REQUIRE(state.a == example.a);
    REQUIRE(state.x == example.x);
    REQUIRE(state.y == example.y);
    REQUIRE(state.sp == example.sp);
    REQUIRE(state.psw == example.psw);
    REQUIRE_FALSE(state.faulted);
    const bool immediate = example.opcode == 0xCD || example.opcode == 0xE8;
    REQUIRE(state.pc == (immediate ? 0x0202 : 0x0201));
    REQUIRE(f.bus.accesses ==
            std::vector<Access>{{1, 0x0200, example.opcode, false}, {2, 0x0201, example.operand, false}});
  }
}

TEST_CASE("SPC700 direct loads sample their operand on the third cycle", "[unit][apu][spc700]") {
  for (const uint8_t opcode : std::initializer_list<uint8_t>{0xE4, 0xEB}) {
    CAPTURE(opcode);
    Fixture f{opcode, 0xF4};
    f.bus.memory[0xF4] = 0x11;
    f.Tick(2);
    REQUIRE(f.cpu.GetState().a == 0);
    REQUIRE(f.cpu.GetState().y == 0);
    f.bus.memory[0xF4] = 0x81;
    f.Tick();
    REQUIRE((opcode == 0xE4 ? f.cpu.GetState().a : f.cpu.GetState().y) == 0x81);
    REQUIRE(f.cpu.GetState().psw == 0x80);
    REQUIRE(f.bus.accesses.back() == Access{3, 0x00F4, 0x81, false});
  }
}

TEST_CASE("SPC700 direct and indirect X stores retain their dummy reads", "[unit][apu][spc700]") {
  for (const uint8_t opcode : std::initializer_list<uint8_t>{0xC4, 0xCB, 0xC6}) {
    CAPTURE(opcode);
    Fixture f{opcode, 0xF4};
    f.cpu.Reset(Spc700::State{.a = 0xAA, .x = 0xF4, .y = 0xBB, .psw = 0x43, .pc = 0x0200});
    f.bus.memory[0xF4] = 0x11;
    f.Tick(3);
    REQUIRE(f.bus.memory[0xF4] == 0x11);
    REQUIRE(f.bus.accesses.back() == Access{3, 0x00F4, 0x11, false});
    f.Tick();
    const uint8_t written = opcode == 0xCB ? 0xBB : 0xAA;
    REQUIRE(f.bus.memory[0xF4] == written);
    REQUIRE(f.bus.accesses.back() == Access{4, 0x00F4, written, true});
    REQUIRE(f.cpu.GetState().psw == 0x43);
    REQUIRE(f.cpu.GetState().pc == (opcode == 0xC6 ? 0x0201 : 0x0202));
  }
}

TEST_CASE("SPC700 immediate store has no write effect until its fifth cycle", "[unit][apu][spc700]") {
  Fixture f{0x8F, 0xAA, 0xF4};
  f.cpu.Reset(Spc700::State{.psw = 0xC3, .pc = 0x0200});
  f.bus.memory[0xF4] = 0x17;
  f.Tick(4);
  REQUIRE(f.bus.memory[0xF4] == 0x17);
  f.Tick();
  REQUIRE(f.bus.memory[0xF4] == 0xAA);
  REQUIRE(f.cpu.GetState().psw == 0xC3);
  REQUIRE(f.bus.accesses == std::vector<Access>{{1, 0x0200, 0x8F, false},
                                                {2, 0x0201, 0xAA, false},
                                                {3, 0x0202, 0xF4, false},
                                                {4, 0x00F4, 0x17, false},
                                                {5, 0x00F4, 0xAA, true}});
}

TEST_CASE("SPC700 compares set carry for no borrow and preserve their operands", "[unit][apu][spc700]") {
  struct Example {
    uint8_t lhs;
    uint8_t rhs;
    uint8_t flags;
  };
  constexpr std::array kCases{Example{0x42, 0x42, 0x03}, Example{0x80, 0x01, 0x01}, Example{0x00, 0x01, 0x80},
                              Example{0xFF, 0x01, 0x81}};
  for (const uint8_t opcode : std::initializer_list<uint8_t>{0x64, 0x68, 0x78, 0x7E}) {
    for (const auto& example : kCases) {
      CAPTURE(opcode, example.lhs, example.rhs);
      Fixture f{};
      f.cpu.Reset(Spc700::State{.a = example.lhs, .y = example.lhs, .psw = 0x5C, .pc = 0x0200});
      f.bus.memory[0x0200] = opcode;
      f.bus.memory[0x0201] = opcode == 0x68 || opcode == 0x78 ? example.rhs : 0xF4;
      f.bus.memory[0x0202] = 0xF4;
      f.bus.memory[0xF4] = opcode == 0x78 ? example.lhs : example.rhs;
      const unsigned compare_cycle = opcode == 0x78 ? 4 : (opcode == 0x68 ? 2 : 3);
      f.Tick(compare_cycle - 1);
      REQUIRE(f.cpu.GetState().psw == 0x5C);
      f.Tick();
      REQUIRE(f.cpu.GetState().psw == (0x5C | example.flags));
      REQUIRE(f.cpu.GetState().a == example.lhs);
      REQUIRE(f.cpu.GetState().y == example.lhs);
      REQUIRE(f.bus.memory[0xF4] == (opcode == 0x78 ? example.lhs : example.rhs));
      if (opcode == 0x78) {
        const auto reads = f.bus.accesses.size();
        f.Tick();
        REQUIRE(f.bus.accesses.size() == reads);  // CMP dp,#imm ends with an idle cycle.
      }
    }
  }
}

TEST_CASE("SPC700 relative branches have two or four cycles without a page penalty", "[unit][apu][spc700]") {
  for (const uint8_t opcode : std::initializer_list<uint8_t>{0x10, 0xD0, 0x2F}) {
    for (const bool take : {false, true}) {
      if (opcode == 0x2F && !take) {
        continue;
      }
      CAPTURE(opcode, take);
      Fixture f{};
      f.bus.memory[0xFFFD] = opcode;
      f.bus.memory[0xFFFE] = 0x02;
      f.cpu.Reset(Spc700::State{.psw = static_cast<uint8_t>(take ? 0x00 : 0x82), .pc = 0xFFFD});
      f.Tick(2);
      REQUIRE(f.cpu.GetState().pc == 0xFFFF);
      if (take) {
        f.Tick();
        REQUIRE(f.cpu.GetState().pc == 0xFFFF);
        f.Tick();
        REQUIRE(f.cpu.GetState().pc == 0x0001);
        REQUIRE(f.bus.accesses.size() == 2);
      }
      f.Tick();
      REQUIRE(f.bus.accesses.back() == Access{take ? 5U : 3U, static_cast<uint16_t>(take ? 1 : 0xFFFF), 0, false});
    }
  }

  Fixture backwards{0x2F, 0xFC};
  backwards.Tick(4);
  REQUIRE(backwards.cpu.GetState().pc == 0x01FE);
}

TEST_CASE("SPC700 direct-page selection affects data and preserves all other flags", "[unit][apu][spc700]") {
  Fixture f{0x40, 0xE4, 0xF4, 0xC6, 0x20, 0xE4, 0xF4};
  f.cpu.Reset(Spc700::State{.x = 0xF5, .psw = 0x41, .pc = 0x0200});
  f.bus.memory[0x00F4] = 0x12;
  f.bus.memory[0x01F4] = 0x34;
  f.Tick(2);
  REQUIRE(f.cpu.GetState().psw == 0x61);
  f.Tick(3);
  REQUIRE(f.cpu.GetState().a == 0x34);
  f.Tick(4);
  REQUIRE(f.bus.memory[0x01F5] == 0x34);
  REQUIRE(f.bus.memory[0x00F5] == 0);
  f.Tick(2);
  REQUIRE(f.cpu.GetState().psw == 0x41);
  f.Tick(3);
  REQUIRE(f.cpu.GetState().a == 0x12);
}

TEST_CASE("SPC700 INC direct wraps and changes flags on the write cycle", "[unit][apu][spc700]") {
  Fixture f{0xAB, 0x01};
  f.cpu.Reset(Spc700::State{.psw = 0xE1, .pc = 0x0200});
  f.bus.memory[0x0101] = 0xFF;
  f.Tick(3);
  REQUIRE(f.bus.memory[0x0101] == 0xFF);
  REQUIRE(f.cpu.GetState().psw == 0xE1);
  f.Tick();
  REQUIRE(f.bus.memory[0x0101] == 0);
  REQUIRE(f.cpu.GetState().psw == 0x63);
  REQUIRE(f.bus.accesses.back() == Access{4, 0x0101, 0, true});
}

TEST_CASE("SPC700 MOVW reads wrap within the selected direct page and use word flags", "[unit][apu][spc700]") {
  for (const uint8_t page_flag : std::initializer_list<uint8_t>{0x00, 0x20}) {
    for (const uint16_t word : std::initializer_list<uint16_t>{0x0000, 0x0080, 0x8000}) {
      CAPTURE(page_flag, word);
      Fixture f{0xBA, 0xFF};
      const uint16_t base = page_flag == 0 ? 0x0000 : 0x0100;
      f.bus.memory[base | 0xFF] = static_cast<uint8_t>(word);
      f.bus.memory[base] = static_cast<uint8_t>(word >> 8U);
      f.cpu.Reset(Spc700::State{.a = 0x11, .y = 0x22, .psw = static_cast<uint8_t>(page_flag | 1), .pc = 0x0200});
      f.Tick(4);
      REQUIRE(f.cpu.GetState().a == 0x11);
      REQUIRE(f.cpu.GetState().y == 0x22);
      REQUIRE(f.bus.accesses.size() == 3);
      // Low byte is latched; the high byte is not read until the fifth cycle.
      f.bus.memory[base | 0xFF] = 0x55;
      f.Tick();
      REQUIRE(f.cpu.GetState().a == static_cast<uint8_t>(word));
      REQUIRE(f.cpu.GetState().y == static_cast<uint8_t>(word >> 8U));
      REQUIRE(f.cpu.GetState().psw == (page_flag | 1 | (word == 0 ? 2 : 0) | (word == 0x8000 ? 0x80 : 0)));
      REQUIRE(f.bus.accesses.back() == Access{5, base, static_cast<uint8_t>(word >> 8U), false});
    }
  }
}

TEST_CASE("SPC700 MOVW stores wrap and expose their two writes on separate cycles", "[unit][apu][spc700]") {
  for (const uint8_t page_flag : std::initializer_list<uint8_t>{0x00, 0x20}) {
    CAPTURE(page_flag);
    Fixture f{0xDA, 0xFF};
    const uint16_t base = page_flag == 0 ? 0x0000 : 0x0100;
    f.cpu.Reset(Spc700::State{.a = 0x34, .y = 0x12, .psw = static_cast<uint8_t>(page_flag | 0x83), .pc = 0x0200});
    f.Tick(3);
    REQUIRE(f.bus.memory[base | 0xFF] == 0);
    REQUIRE(f.bus.memory[base] == 0);
    REQUIRE(f.bus.accesses.back() == Access{3, static_cast<uint16_t>(base | 0xFF), 0, false});
    f.Tick();
    REQUIRE(f.bus.memory[base | 0xFF] == 0x34);
    REQUIRE(f.bus.memory[base] == 0);
    f.Tick();
    REQUIRE(f.bus.memory[base] == 0x12);
    REQUIRE(f.cpu.GetState().psw == (page_flag | 0x83));
    REQUIRE(f.bus.accesses.back() == Access{5, base, 0x12, true});
  }
}

TEST_CASE("SPC700 indirect Y store wraps its direct pointer and sixteen-bit target", "[unit][apu][spc700]") {
  Fixture f{0xD7, 0xFF};
  f.cpu.Reset(Spc700::State{.a = 0xA5, .y = 2, .psw = 0x20, .pc = 0x0200});
  f.bus.memory[0x01FF] = 0xFF;
  f.bus.memory[0x0100] = 0xFF;
  f.bus.memory[0x0001] = 0x19;
  f.Tick(6);
  REQUIRE(f.bus.memory[0x0001] == 0x19);
  REQUIRE(f.bus.accesses.size() == 5);
  f.Tick();
  REQUIRE(f.bus.memory[0x0001] == 0xA5);
  REQUIRE(f.cpu.GetState().psw == 0x20);
  REQUIRE(f.bus.accesses == std::vector<Access>{{1, 0x0200, 0xD7, false},
                                                {2, 0x0201, 0xFF, false},
                                                {3, 0x01FF, 0xFF, false},
                                                {4, 0x0100, 0xFF, false},
                                                {6, 0x0001, 0x19, false},
                                                {7, 0x0001, 0xA5, true}});
}

TEST_CASE("SPC700 indirect X jump wraps pointer reads without direct-page selection", "[unit][apu][spc700]") {
  Fixture f{0x1F, 0xFE, 0xFF};
  f.cpu.Reset(Spc700::State{.x = 1, .psw = 0x20, .pc = 0x0200});
  f.bus.memory[0xFFFF] = 0x34;
  f.bus.memory[0x0000] = 0x12;
  f.Tick(5);
  REQUIRE(f.cpu.GetState().pc == 0x0203);
  f.Tick();
  REQUIRE(f.cpu.GetState().pc == 0x1234);
  REQUIRE(f.cpu.GetState().psw == 0x20);
  REQUIRE(f.bus.accesses == std::vector<Access>{{1, 0x0200, 0x1F, false},
                                                {2, 0x0201, 0xFE, false},
                                                {3, 0x0202, 0xFF, false},
                                                {5, 0xFFFF, 0x34, false},
                                                {6, 0x0000, 0x12, false}});
}

TEST_CASE("SPC700 explicit fault state prevents execution until reset", "[unit][apu][spc700]") {
  Fixture f{0x8F, 0xAA, 0xF4};
  // All opcodes are valid. Seed a fault to exercise the defensive halt path.
  f.cpu.Reset(Spc700::State{.pc = 0x0200, .faulted = true, .fault_opcode = 0x8F, .fault_pc = 0x0200});
  f.Tick();
  REQUIRE(f.cpu.GetState().faulted);
  REQUIRE(f.cpu.GetState().fault_opcode == 0x8F);
  REQUIRE(f.cpu.GetState().fault_pc == 0x0200);
  REQUIRE(f.cpu.GetState().pc == 0x0200);
  f.Tick(20);
  REQUIRE(f.bus.accesses.empty());
  REQUIRE(f.bus.memory[0xF4] == 0);
  REQUIRE(f.cpu.GetState().cycles == 21);
  f.cpu.Reset();
  REQUIRE_FALSE(f.cpu.GetState().faulted);
}

TEST_CASE("SPC700 STOP prevents later instruction effects until reset", "[unit][apu][spc700]") {
  Fixture f{0xFF, 0x8F, 0xAA, 0xF4};
  f.Tick(2);
  REQUIRE_FALSE(f.cpu.GetState().stopped);
  f.Tick();
  REQUIRE(f.cpu.GetState().stopped);
  REQUIRE_FALSE(f.cpu.GetState().faulted);
  f.Tick(20);
  REQUIRE(f.bus.accesses.size() == 12);
  for (std::size_t index = 2; index < f.bus.accesses.size(); ++index) {
    REQUIRE(f.bus.accesses[index] == Access{2 * index, 0x0201, 0x8F, false});
  }
  REQUIRE(f.bus.memory[0xF4] == 0);
  REQUIRE(f.cpu.GetState().pc == 0x0201);
  REQUIRE(f.cpu.GetState().cycles == 23);
  f.cpu.Reset();
  REQUIRE_FALSE(f.cpu.GetState().stopped);
}
