#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <initializer_list>
#include <vector>

#include "pupsnes/hw/apu/spc700.h"

using pupsnes::Spc700;
using pupsnes::Spc700Bus;

namespace {

// Independent bus schedules follow the ares SPC700 instruction reference:
// https://github.com/ares-emulator/ares/blob/master/ares/component/processor/spc700/instructions.cpp
// Cycle one is the opcode fetch; omitted cycle numbers are idle cycles.
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
    cpu.Reset(Spc700::State{.a = 0x11, .x = 3, .y = 4, .sp = 0x44, .psw = 0xDF, .pc = 0x0200});
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

struct RegisterOpcode {
  uint8_t opcode;
  Register reg;
};

}  // namespace

TEST_CASE("SPC700 register MOVs update NZ except when writing SP", "[unit][apu][spc700][spc700_transfer]") {
  struct Transfer {
    uint8_t opcode;
    Register source;
    Register target;
  };
  constexpr std::array kCases{
      Transfer{0x7D, &Spc700::State::x, &Spc700::State::a}, Transfer{0x9D, &Spc700::State::sp, &Spc700::State::x},
      Transfer{0xFD, &Spc700::State::a, &Spc700::State::y}, Transfer{0xBD, &Spc700::State::x, &Spc700::State::sp},
      Transfer{0x5D, &Spc700::State::a, &Spc700::State::x}, Transfer{0xDD, &Spc700::State::y, &Spc700::State::a},
  };
  for (const auto& example : kCases) {
    for (const uint8_t value : std::initializer_list<uint8_t>{0x00, 0x7F, 0x80}) {
      for (const uint8_t flags : std::initializer_list<uint8_t>{0x00, 0xFF}) {
        CAPTURE(example.opcode, value, flags);
        Fixture f{example.opcode, 0x00};
        auto initial = f.cpu.GetState();
        initial.*example.source = value;
        initial.psw = flags;
        f.cpu.Reset(initial);
        f.Tick();
        REQUIRE(f.cpu.GetState().*example.target == initial.*example.target);
        REQUIRE(f.cpu.GetState().psw == flags);
        f.Tick();
        REQUIRE(f.cpu.GetState().*example.target == value);
        REQUIRE(f.cpu.GetState().*example.source == value);
        const uint8_t expected_flags =
            example.target == &Spc700::State::sp
                ? flags
                : static_cast<uint8_t>((flags & 0x7D) | (value == 0 ? 2 : 0) | (value & 0x80));
        REQUIRE(f.cpu.GetState().psw == expected_flags);
        REQUIRE(f.bus.accesses == std::vector<Access>{{1, 0x0200, example.opcode, false}, {2, 0x0201, 0, false}});
        f.NextFetch(0x0201, 3);
      }
    }
  }
}

TEST_CASE("SPC700 MOV Y immediate samples and sets flags on cycle two", "[unit][apu][spc700][spc700_transfer]") {
  for (const uint8_t value : std::initializer_list<uint8_t>{0x00, 0x7F, 0x80}) {
    CAPTURE(value);
    Fixture f{0x8D, 0x42};
    f.Tick();
    REQUIRE(f.cpu.GetState().y == 4);
    REQUIRE(f.cpu.GetState().psw == 0xDF);
    f.bus.memory[0x0201] = value;
    f.Tick();
    REQUIRE(f.cpu.GetState().y == value);
    REQUIRE(f.cpu.GetState().psw == (0x5D | (value == 0 ? 2 : 0) | (value & 0x80)));
    REQUIRE(f.bus.accesses == std::vector<Access>{{1, 0x0200, 0x8D, false}, {2, 0x0201, value, false}});
    f.NextFetch(0x0202, 3);
  }
}

TEST_CASE("SPC700 absolute MOV loads read on cycle four and preserve non-NZ flags",
          "[unit][apu][spc700][spc700_transfer]") {
  constexpr std::array kCases{RegisterOpcode{0xE5, &Spc700::State::a}, RegisterOpcode{0xE9, &Spc700::State::x},
                              RegisterOpcode{0xEC, &Spc700::State::y}};
  for (const auto& example : kCases) {
    for (const uint8_t value : std::initializer_list<uint8_t>{0x00, 0x7F, 0x80}) {
      CAPTURE(example.opcode, value);
      Fixture f{example.opcode, 0xFF, 0xFF};
      const auto initial = f.cpu.GetState();
      f.bus.memory[0xFFFF] = 0x42;
      f.Tick(3);
      REQUIRE(f.cpu.GetState().*example.reg == initial.*example.reg);
      REQUIRE(f.cpu.GetState().psw == initial.psw);
      f.bus.memory[0xFFFF] = value;
      f.Tick();
      REQUIRE(f.cpu.GetState().*example.reg == value);
      REQUIRE(f.cpu.GetState().psw == (0x5D | (value == 0 ? 2 : 0) | (value & 0x80)));
      REQUIRE(f.bus.accesses == std::vector<Access>{{1, 0x0200, example.opcode, false},
                                                    {2, 0x0201, 0xFF, false},
                                                    {3, 0x0202, 0xFF, false},
                                                    {4, 0xFFFF, value, false}});
      f.NextFetch(0x0203, 5);
    }
  }
}

TEST_CASE("SPC700 absolute MOV stores perform a dummy read before cycle-five writes",
          "[unit][apu][spc700][spc700_transfer]") {
  constexpr std::array kCases{RegisterOpcode{0xC5, &Spc700::State::a}, RegisterOpcode{0xC9, &Spc700::State::x},
                              RegisterOpcode{0xCC, &Spc700::State::y}};
  for (const auto& example : kCases) {
    CAPTURE(example.opcode);
    Fixture f{example.opcode, 0xFF, 0xFF};
    const auto initial = f.cpu.GetState();
    const uint8_t value = initial.*example.reg;
    f.bus.memory[0xFFFF] = 0xA5;
    f.Tick(4);
    REQUIRE(f.bus.memory[0xFFFF] == 0xA5);
    f.Tick();
    REQUIRE(f.bus.memory[0xFFFF] == value);
    REQUIRE(f.cpu.GetState().psw == initial.psw);
    REQUIRE(f.bus.accesses == std::vector<Access>{{1, 0x0200, example.opcode, false},
                                                  {2, 0x0201, 0xFF, false},
                                                  {3, 0x0202, 0xFF, false},
                                                  {4, 0xFFFF, 0xA5, false},
                                                  {5, 0xFFFF, value, true}});
    f.NextFetch(0x0203, 6);
  }
}

TEST_CASE("SPC700 direct MOV X load and store use the selected page", "[unit][apu][spc700][spc700_transfer]") {
  for (const uint8_t page_flag : std::initializer_list<uint8_t>{0x00, 0x20}) {
    for (const uint8_t opcode : std::initializer_list<uint8_t>{0xF8, 0xD8}) {
      CAPTURE(page_flag, opcode);
      Fixture f{opcode, 0xFF};
      auto initial = f.cpu.GetState();
      initial.psw = static_cast<uint8_t>(0xDF | page_flag);
      f.cpu.Reset(initial);
      const uint16_t address = page_flag == 0 ? 0x00FF : 0x01FF;
      f.bus.memory[address] = 0x80;
      f.Tick(2);
      REQUIRE(f.cpu.GetState().x == 3);
      f.Tick();
      if (opcode == 0xF8) {
        REQUIRE(f.cpu.GetState().x == 0x80);
        REQUIRE(f.cpu.GetState().psw == (0xDD | page_flag));
        REQUIRE(f.bus.accesses ==
                std::vector<Access>{{1, 0x0200, opcode, false}, {2, 0x0201, 0xFF, false}, {3, address, 0x80, false}});
        f.NextFetch(0x0202, 4);
      } else {
        REQUIRE(f.bus.memory[address] == 0x80);
        f.Tick();
        REQUIRE(f.bus.memory[address] == 3);
        REQUIRE(f.cpu.GetState().psw == initial.psw);
        REQUIRE(f.bus.accesses == std::vector<Access>{{1, 0x0200, opcode, false},
                                                      {2, 0x0201, 0xFF, false},
                                                      {3, address, 0x80, false},
                                                      {4, address, 3, true}});
        f.NextFetch(0x0202, 5);
      }
    }
  }
}

TEST_CASE("SPC700 indexed direct MOV loads wrap inside either selected direct page",
          "[unit][apu][spc700][spc700_transfer]") {
  struct Load {
    uint8_t opcode;
    Register target;
    uint8_t offset;
  };
  constexpr std::array kCases{Load{0xF4, &Spc700::State::a, 1}, Load{0xF9, &Spc700::State::x, 2},
                              Load{0xFB, &Spc700::State::y, 1}};
  for (const auto& example : kCases) {
    for (const uint8_t page_flag : std::initializer_list<uint8_t>{0x00, 0x20}) {
      for (const uint8_t value : std::initializer_list<uint8_t>{0x00, 0x7F, 0x80}) {
        CAPTURE(example.opcode, page_flag, value);
        Fixture f{example.opcode, 0xFE};
        auto initial = f.cpu.GetState();
        initial.psw = static_cast<uint8_t>(0xDF | page_flag);
        f.cpu.Reset(initial);
        const uint16_t address = static_cast<uint16_t>((page_flag == 0 ? 0 : 0x100) | example.offset);
        f.bus.memory[address] = value;
        f.Tick(3);
        REQUIRE(f.cpu.GetState().*example.target == initial.*example.target);
        REQUIRE(f.cpu.GetState().psw == initial.psw);
        f.Tick();
        REQUIRE(f.cpu.GetState().*example.target == value);
        REQUIRE(f.cpu.GetState().psw == (0x5D | page_flag | (value == 0 ? 2 : 0) | (value & 0x80)));
        REQUIRE(f.bus.accesses == std::vector<Access>{{1, 0x0200, example.opcode, false},
                                                      {2, 0x0201, 0xFE, false},
                                                      {4, address, value, false}});
        f.NextFetch(0x0202, 5);
      }
    }
  }
}

TEST_CASE("SPC700 indexed direct MOV stores idle before dummy reads and wrap in the direct page",
          "[unit][apu][spc700][spc700_transfer]") {
  struct Store {
    uint8_t opcode;
    uint8_t value;
    uint8_t offset;
  };
  constexpr std::array kCases{Store{0xD4, 0x11, 1}, Store{0xD9, 3, 2}, Store{0xDB, 4, 1}};
  for (const auto& example : kCases) {
    for (const uint8_t page_flag : std::initializer_list<uint8_t>{0x00, 0x20}) {
      CAPTURE(example.opcode, page_flag);
      Fixture f{example.opcode, 0xFE};
      auto initial = f.cpu.GetState();
      initial.psw = static_cast<uint8_t>(0xDF | page_flag);
      f.cpu.Reset(initial);
      const uint16_t address = static_cast<uint16_t>((page_flag == 0 ? 0 : 0x100) | example.offset);
      f.bus.memory[address] = 0x80;
      f.Tick(4);
      REQUIRE(f.bus.memory[address] == 0x80);
      f.Tick();
      REQUIRE(f.bus.memory[address] == example.value);
      REQUIRE(f.cpu.GetState().psw == initial.psw);
      REQUIRE(f.bus.accesses == std::vector<Access>{{1, 0x0200, example.opcode, false},
                                                    {2, 0x0201, 0xFE, false},
                                                    {4, address, 0x80, false},
                                                    {5, address, example.value, true}});
      f.NextFetch(0x0202, 6);
    }
  }
}

TEST_CASE("SPC700 absolute indexed MOV loads wrap sixteen-bit addresses without extra cycles",
          "[unit][apu][spc700][spc700_transfer]") {
  for (const uint8_t opcode : std::initializer_list<uint8_t>{0xF5, 0xF6}) {
    for (const uint8_t value : std::initializer_list<uint8_t>{0x00, 0x7F, 0x80}) {
      CAPTURE(opcode, value);
      Fixture f{opcode, 0xFE, 0xFF};
      const uint16_t address = opcode == 0xF5 ? 1 : 2;
      f.bus.memory[address] = value;
      f.Tick(4);
      REQUIRE(f.cpu.GetState().a == 0x11);
      REQUIRE(f.cpu.GetState().psw == 0xDF);
      f.Tick();
      REQUIRE(f.cpu.GetState().a == value);
      REQUIRE(f.cpu.GetState().psw == (0x5D | (value == 0 ? 2 : 0) | (value & 0x80)));
      REQUIRE(f.bus.accesses == std::vector<Access>{{1, 0x0200, opcode, false},
                                                    {2, 0x0201, 0xFE, false},
                                                    {3, 0x0202, 0xFF, false},
                                                    {5, address, value, false}});
      f.NextFetch(0x0203, 6);
    }
  }
}

TEST_CASE("SPC700 absolute indexed MOV stores idle then read and write the wrapped target",
          "[unit][apu][spc700][spc700_transfer]") {
  for (const uint8_t opcode : std::initializer_list<uint8_t>{0xD5, 0xD6}) {
    CAPTURE(opcode);
    Fixture f{opcode, 0xFE, 0xFF};
    const uint16_t address = opcode == 0xD5 ? 1 : 2;
    f.bus.memory[address] = 0x80;
    f.Tick(5);
    REQUIRE(f.bus.memory[address] == 0x80);
    f.Tick();
    REQUIRE(f.bus.memory[address] == 0x11);
    REQUIRE(f.cpu.GetState().psw == 0xDF);
    REQUIRE(f.bus.accesses == std::vector<Access>{{1, 0x0200, opcode, false},
                                                  {2, 0x0201, 0xFE, false},
                                                  {3, 0x0202, 0xFF, false},
                                                  {5, address, 0x80, false},
                                                  {6, address, 0x11, true}});
    f.NextFetch(0x0203, 7);
  }
}

TEST_CASE("SPC700 MOV A indirect X reads the selected page after a dummy PC read",
          "[unit][apu][spc700][spc700_transfer]") {
  for (const uint8_t page_flag : std::initializer_list<uint8_t>{0x00, 0x20}) {
    CAPTURE(page_flag);
    Fixture f{0xE6, 0x00};
    auto initial = f.cpu.GetState();
    initial.x = 0xFF;
    initial.psw = static_cast<uint8_t>(0xDF | page_flag);
    f.cpu.Reset(initial);
    const uint16_t address = page_flag == 0 ? 0x00FF : 0x01FF;
    f.bus.memory[address] = 0;
    f.Tick(2);
    REQUIRE(f.cpu.GetState().a == 0x11);
    REQUIRE(f.cpu.GetState().psw == initial.psw);
    f.Tick();
    REQUIRE(f.cpu.GetState().a == 0);
    REQUIRE(f.cpu.GetState().x == 0xFF);
    REQUIRE(f.cpu.GetState().psw == (0x5F | page_flag));
    REQUIRE(f.bus.accesses ==
            std::vector<Access>{{1, 0x0200, 0xE6, false}, {2, 0x0201, 0, false}, {3, address, 0, false}});
    f.NextFetch(0x0201, 4);
  }
}

TEST_CASE("SPC700 indexed indirect MOV wraps the pointer index and high byte in either direct page",
          "[unit][apu][spc700][spc700_transfer]") {
  for (const uint8_t opcode : std::initializer_list<uint8_t>{0xC7, 0xE7}) {
    for (const uint8_t page_flag : std::initializer_list<uint8_t>{0x00, 0x20}) {
      for (const uint8_t operand : std::initializer_list<uint8_t>{0xFC, 0xFE}) {
        CAPTURE(opcode, page_flag, operand);
        Fixture f{opcode, operand};
        auto initial = f.cpu.GetState();
        initial.psw = static_cast<uint8_t>(0xDF | page_flag);
        f.cpu.Reset(initial);
        const uint16_t base = page_flag == 0 ? 0 : 0x100;
        // X=3: $FC+X gives $FF/$00; $FE+X gives $01/$02.
        const uint16_t low = static_cast<uint16_t>(base | (operand == 0xFC ? 0xFF : 1));
        const uint16_t high = static_cast<uint16_t>(base | (operand == 0xFC ? 0 : 2));
        f.bus.memory[low] = 0x56;
        f.bus.memory[high] = 0x34;
        f.bus.memory[0x3456] = 0x80;
        f.Tick(5);
        REQUIRE(f.cpu.GetState().a == 0x11);
        REQUIRE(f.cpu.GetState().psw == initial.psw);
        // Pointer bytes are latched independently of the later data access.
        f.bus.memory[low] = 0;
        f.bus.memory[high] = 0;
        f.Tick();
        std::vector<Access> expected{{1, 0x0200, opcode, false},
                                     {2, 0x0201, operand, false},
                                     {4, low, 0x56, false},
                                     {5, high, 0x34, false},
                                     {6, 0x3456, 0x80, false}};
        if (opcode == 0xE7) {
          REQUIRE(f.cpu.GetState().a == 0x80);
          REQUIRE(f.cpu.GetState().psw == (0xDD | page_flag));
          REQUIRE(f.bus.accesses == expected);
          f.NextFetch(0x0202, 7);
        } else {
          REQUIRE(f.bus.memory[0x3456] == 0x80);
          f.Tick();
          REQUIRE(f.bus.memory[0x3456] == 0x11);
          REQUIRE(f.cpu.GetState().psw == initial.psw);
          expected.push_back({7, 0x3456, 0x11, true});
          REQUIRE(f.bus.accesses == expected);
          f.NextFetch(0x0202, 8);
        }
      }
    }
  }
}

TEST_CASE("SPC700 indirect indexed MOV read wraps both its page pointer and absolute target",
          "[unit][apu][spc700][spc700_transfer]") {
  for (const uint8_t page_flag : std::initializer_list<uint8_t>{0x00, 0x20}) {
    CAPTURE(page_flag);
    Fixture f{0xF7, 0xFF};
    auto initial = f.cpu.GetState();
    initial.psw = static_cast<uint8_t>(0xDF | page_flag);
    f.cpu.Reset(initial);
    const uint16_t low = page_flag == 0 ? 0x00FF : 0x01FF;
    const uint16_t high = page_flag == 0 ? 0x0000 : 0x0100;
    f.bus.memory[low] = 0xFE;
    f.bus.memory[high] = 0xFF;
    f.bus.memory[2] = 0;
    f.Tick(5);
    REQUIRE(f.cpu.GetState().a == 0x11);
    REQUIRE(f.cpu.GetState().psw == initial.psw);
    f.Tick();
    REQUIRE(f.cpu.GetState().a == 0);
    REQUIRE(f.cpu.GetState().psw == (0x5F | page_flag));
    REQUIRE(f.bus.accesses == std::vector<Access>{{1, 0x0200, 0xF7, false},
                                                  {2, 0x0201, 0xFF, false},
                                                  {4, low, 0xFE, false},
                                                  {5, high, 0xFF, false},
                                                  {6, 2, 0, false}});
    f.NextFetch(0x0202, 7);
  }
}

TEST_CASE("SPC700 postincrement MOV load wraps X at its read then sets NZ on the trailing idle",
          "[unit][apu][spc700][spc700_transfer]") {
  for (const uint8_t page_flag : std::initializer_list<uint8_t>{0x00, 0x20}) {
    for (const uint8_t value : std::initializer_list<uint8_t>{0x00, 0x7F, 0x80}) {
      CAPTURE(page_flag, value);
      Fixture f{0xBF, 0x00};
      auto initial = f.cpu.GetState();
      initial.x = 0xFF;
      initial.psw = static_cast<uint8_t>(0xDF | page_flag);
      f.cpu.Reset(initial);
      const uint16_t address = page_flag == 0 ? 0x00FF : 0x01FF;
      f.bus.memory[address] = value;
      f.Tick(2);
      REQUIRE(f.cpu.GetState().a == 0x11);
      REQUIRE(f.cpu.GetState().x == 0xFF);
      f.Tick();
      REQUIRE(f.cpu.GetState().a == value);
      REQUIRE(f.cpu.GetState().x == 0);
      REQUIRE(f.cpu.GetState().psw == initial.psw);
      f.bus.memory[address] = 0x42;
      f.Tick();
      REQUIRE(f.cpu.GetState().a == value);
      REQUIRE(f.cpu.GetState().psw == (0x5D | page_flag | (value == 0 ? 2 : 0) | (value & 0x80)));
      REQUIRE(f.bus.accesses ==
              std::vector<Access>{{1, 0x0200, 0xBF, false}, {2, 0x0201, 0, false}, {3, address, value, false}});
      f.NextFetch(0x0201, 5);
    }
  }
}

TEST_CASE("SPC700 postincrement MOV store has no destination dummy read and wraps X on cycle four",
          "[unit][apu][spc700][spc700_transfer]") {
  for (const uint8_t page_flag : std::initializer_list<uint8_t>{0x00, 0x20}) {
    CAPTURE(page_flag);
    Fixture f{0xAF, 0x00};
    auto initial = f.cpu.GetState();
    initial.x = 0xFF;
    initial.psw = static_cast<uint8_t>(0xDF | page_flag);
    f.cpu.Reset(initial);
    const uint16_t address = page_flag == 0 ? 0x00FF : 0x01FF;
    f.bus.memory[address] = 0x80;
    f.Tick(3);
    REQUIRE(f.bus.memory[address] == 0x80);
    REQUIRE(f.cpu.GetState().x == 0xFF);
    f.Tick();
    REQUIRE(f.bus.memory[address] == 0x11);
    REQUIRE(f.cpu.GetState().x == 0);
    REQUIRE(f.cpu.GetState().psw == initial.psw);
    REQUIRE(f.bus.accesses ==
            std::vector<Access>{{1, 0x0200, 0xAF, false}, {2, 0x0201, 0, false}, {4, address, 0x11, true}});
    f.NextFetch(0x0201, 5);
  }
}

TEST_CASE("SPC700 direct-to-direct MOV fetches the source first and writes without a destination dummy read",
          "[unit][apu][spc700][spc700_transfer]") {
  for (const uint8_t page_flag : std::initializer_list<uint8_t>{0x00, 0x20}) {
    CAPTURE(page_flag);
    Fixture f{0xFA, 0xFF, 0x10};
    auto initial = f.cpu.GetState();
    initial.psw = static_cast<uint8_t>(0xDF | page_flag);
    f.cpu.Reset(initial);
    const uint16_t source = page_flag == 0 ? 0x00FF : 0x01FF;
    const uint16_t target = page_flag == 0 ? 0x0010 : 0x0110;
    f.bus.memory[source] = 0;
    f.bus.memory[target] = 0x80;
    f.Tick(3);
    REQUIRE(f.bus.accesses.back() == Access{3, source, 0, false});
    REQUIRE(f.cpu.GetState().pc == 0x0202);
    f.bus.memory[source] = 0x42;
    f.Tick();
    REQUIRE(f.bus.memory[target] == 0x80);
    f.Tick();
    REQUIRE(f.bus.memory[target] == 0);
    REQUIRE(f.cpu.GetState().a == initial.a);
    REQUIRE(f.cpu.GetState().x == initial.x);
    REQUIRE(f.cpu.GetState().y == initial.y);
    REQUIRE(f.cpu.GetState().psw == initial.psw);
    REQUIRE(f.bus.accesses == std::vector<Access>{{1, 0x0200, 0xFA, false},
                                                  {2, 0x0201, 0xFF, false},
                                                  {3, source, 0, false},
                                                  {4, 0x0202, 0x10, false},
                                                  {5, target, 0, true}});
    f.NextFetch(0x0203, 6);
  }
}

TEST_CASE("SPC700 MOV1 carry load decodes every bit and preserves the other seven flags",
          "[unit][apu][spc700][spc700_transfer]") {
  for (const uint16_t address : std::initializer_list<uint16_t>{0x0000, 0x1FFF}) {
    for (unsigned bit = 0; bit < 8; ++bit) {
      for (const bool set : {false, true}) {
        CAPTURE(address, bit, set);
        const uint8_t low = static_cast<uint8_t>(address);
        const uint8_t high = static_cast<uint8_t>((address >> 8) | (bit << 5));
        const uint8_t value = set ? static_cast<uint8_t>(1U << bit) : static_cast<uint8_t>(~(1U << bit));
        Fixture f{0xAA, low, high};
        auto initial = f.cpu.GetState();
        initial.psw = set ? 0xFE : 0xFF;
        f.cpu.Reset(initial);
        f.bus.memory[address] = value;
        f.Tick(3);
        REQUIRE(f.cpu.GetState().psw == initial.psw);
        f.Tick();
        REQUIRE(f.cpu.GetState().psw == (set ? 0xFF : 0xFE));
        REQUIRE(f.bus.memory[address] == value);
        REQUIRE(f.bus.accesses == std::vector<Access>{{1, 0x0200, 0xAA, false},
                                                      {2, 0x0201, low, false},
                                                      {3, 0x0202, high, false},
                                                      {4, address, value, false}});
        f.NextFetch(0x0203, 5);
      }
    }
  }
}

TEST_CASE("SPC700 MOV1 carry store preserves other bits and flags with read-idle-write timing",
          "[unit][apu][spc700][spc700_transfer]") {
  for (const uint16_t address : std::initializer_list<uint16_t>{0x0000, 0x1FFF}) {
    for (unsigned bit = 0; bit < 8; ++bit) {
      for (const bool set : {false, true}) {
        CAPTURE(address, bit, set);
        const uint8_t low = static_cast<uint8_t>(address);
        const uint8_t high = static_cast<uint8_t>((address >> 8) | (bit << 5));
        const uint8_t old_value = set ? static_cast<uint8_t>(~(1U << bit)) : static_cast<uint8_t>(1U << bit);
        const uint8_t new_value = set ? 0xFF : 0;
        Fixture f{0xCA, low, high};
        auto initial = f.cpu.GetState();
        initial.psw = set ? 0xFF : 0xFE;
        f.cpu.Reset(initial);
        f.bus.memory[address] = old_value;
        f.Tick(4);
        REQUIRE(f.bus.memory[address] == old_value);
        // The whole byte comes from the cycle-four read, including preserved bits.
        f.bus.memory[address] = 0x55;
        f.Tick();
        REQUIRE(f.bus.memory[address] == 0x55);
        f.Tick();
        REQUIRE(f.bus.memory[address] == new_value);
        REQUIRE(f.cpu.GetState().psw == initial.psw);
        REQUIRE(f.bus.accesses == std::vector<Access>{{1, 0x0200, 0xCA, false},
                                                      {2, 0x0201, low, false},
                                                      {3, 0x0202, high, false},
                                                      {4, address, old_value, false},
                                                      {6, address, new_value, true}});
        f.NextFetch(0x0203, 7);
      }
    }
  }
}

TEST_CASE("SPC700 PUSH writes on cycle three and wraps the stack inside page one regardless of P",
          "[unit][apu][spc700][spc700_transfer]") {
  constexpr std::array kCases{RegisterOpcode{0x0D, &Spc700::State::psw}, RegisterOpcode{0x2D, &Spc700::State::a},
                              RegisterOpcode{0x4D, &Spc700::State::x}, RegisterOpcode{0x6D, &Spc700::State::y}};
  for (const auto& example : kCases) {
    for (const uint8_t flags : std::initializer_list<uint8_t>{0xDF, 0xFF}) {
      CAPTURE(example.opcode, flags);
      Fixture f{example.opcode, example.opcode, 0x00};
      auto initial = f.cpu.GetState();
      initial.sp = 0;
      initial.psw = flags;
      f.cpu.Reset(initial);
      const uint8_t value = initial.*example.reg;
      f.bus.memory[0x0100] = 0x80;
      f.bus.memory[0x01FF] = 0x80;
      f.Tick(2);
      REQUIRE(f.bus.memory[0x0100] == 0x80);
      REQUIRE(f.cpu.GetState().sp == 0);
      f.Tick();
      REQUIRE(f.bus.memory[0x0100] == value);
      REQUIRE(f.cpu.GetState().sp == 0xFF);
      f.Tick(3);  // idle, next opcode, dummy PC read
      REQUIRE(f.bus.memory[0x01FF] == 0x80);
      REQUIRE(f.cpu.GetState().sp == 0xFF);
      f.Tick(2);
      REQUIRE(f.bus.memory[0x01FF] == value);
      REQUIRE(f.cpu.GetState().sp == 0xFE);
      REQUIRE(f.cpu.GetState().psw == flags);
      REQUIRE(f.cpu.GetState().*example.reg == value);
      REQUIRE(f.bus.accesses == std::vector<Access>{{1, 0x0200, example.opcode, false},
                                                    {2, 0x0201, example.opcode, false},
                                                    {3, 0x0100, value, true},
                                                    {5, 0x0201, example.opcode, false},
                                                    {6, 0x0202, 0, false},
                                                    {7, 0x01FF, value, true}});
      f.NextFetch(0x0202, 9);
    }
  }
}

TEST_CASE("SPC700 POP reads on cycle four and preserves NZ except when restoring PSW",
          "[unit][apu][spc700][spc700_transfer]") {
  constexpr std::array kCases{RegisterOpcode{0x8E, &Spc700::State::psw}, RegisterOpcode{0xAE, &Spc700::State::a},
                              RegisterOpcode{0xCE, &Spc700::State::x}, RegisterOpcode{0xEE, &Spc700::State::y}};
  for (const auto& example : kCases) {
    for (const uint8_t flags : std::initializer_list<uint8_t>{0x5D, 0x7D, 0xDF, 0xFF}) {
      CAPTURE(example.opcode, flags);
      Fixture f{example.opcode, example.opcode, 0x00};
      auto initial = f.cpu.GetState();
      initial.sp = 0xFF;
      initial.psw = flags;
      f.cpu.Reset(initial);
      f.bus.memory[0x0100] = 0;
      f.bus.memory[0x0101] = 0x80;
      f.Tick(3);
      REQUIRE(f.cpu.GetState().sp == 0xFF);
      REQUIRE(f.cpu.GetState().*example.reg == initial.*example.reg);
      f.Tick();
      REQUIRE(f.cpu.GetState().sp == 0);
      REQUIRE(f.cpu.GetState().*example.reg == 0);
      REQUIRE(f.cpu.GetState().psw == (example.opcode == 0x8E ? 0 : flags));
      f.Tick(3);
      REQUIRE(f.cpu.GetState().sp == 0);
      REQUIRE(f.cpu.GetState().*example.reg == 0);
      f.Tick();
      REQUIRE(f.cpu.GetState().sp == 1);
      REQUIRE(f.cpu.GetState().*example.reg == 0x80);
      REQUIRE(f.cpu.GetState().psw == (example.opcode == 0x8E ? 0x80 : flags));
      REQUIRE(f.bus.accesses == std::vector<Access>{{1, 0x0200, example.opcode, false},
                                                    {2, 0x0201, example.opcode, false},
                                                    {4, 0x0100, 0, false},
                                                    {5, 0x0201, example.opcode, false},
                                                    {6, 0x0202, 0, false},
                                                    {8, 0x0101, 0x80, false}});
      f.NextFetch(0x0202, 9);
    }
  }
}
