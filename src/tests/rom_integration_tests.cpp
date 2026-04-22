#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "pupsnes/hw/5a22/cpu.h"
#include "pupsnes/hw/cartridge.h"
#include "pupsnes/hw/scheduler.h"
#include "pupsnes/hw/snes.h"
#include "pupsnes/hw/wram.h"

using namespace pupsnes;  // NOLINT(google-build-using-namespace)

namespace {

enum class RomStatus : uint8_t {
  kPass = 0,
  kFail = 1,
  kHarnessError = 2,
};

enum class GoalObservationKind : uint8_t {
  kCpuA8 = 0,
  kCpuPc = 1,
  kWramByte = 2,
  kCpuSp = 3,
  kCpuDbr = 4,
  kCpuPbr = 5,
};

struct GoalSpec {
  std::string name;
  std::string description;
  GoalObservationKind kind = GoalObservationKind::kCpuA8;
  uint32_t address = 0;
  uint32_t expected_value = 0;
};

struct RomScenario {
  std::string rom_name;
  std::string title;
  std::string purpose;
  std::string why_expected_to_fail;
  TimeMasterT cycle_budget = 0;
  RomStatus expected_current_status = RomStatus::kFail;
  std::optional<uint8_t> initial_dbr;
  std::vector<GoalSpec> goals;
};

struct GoalResult {
  GoalSpec spec;
  bool matched = false;
  uint32_t actual_value = 0;
};

struct RomExecutionSnapshot {
  TimeMasterT cpu_time = 0;
  TimeMasterT master_time = 0;
  CPU::Regs cpu_regs{};
  uint8_t cpu_micro_op_index = 0;
};

struct RomExecutionResult {
  RomScenario scenario;
  RomStatus status = RomStatus::kHarnessError;
  std::string failure_summary;
  std::size_t matched_goals = 0;
  std::vector<GoalResult> goal_results;
  RomExecutionSnapshot snapshot{};
};

std::vector<uint8_t> ReadBinaryFile(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  REQUIRE(stream.good());

  return std::vector<uint8_t>(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

std::string Trim(std::string_view text) {
  std::size_t start = 0;
  while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])) != 0) {
    start++;
  }

  std::size_t end = text.size();
  while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
    end--;
  }

  return std::string(text.substr(start, end - start));
}

std::string StripCommentAndTrim(const std::string& line) {
  const std::size_t comment_pos = line.find('#');
  const std::string_view without_comment =
      (comment_pos == std::string::npos) ? std::string_view(line) : std::string_view(line).substr(0, comment_pos);
  return Trim(without_comment);
}

std::string ParseQuotedString(const std::string& value, const std::filesystem::path& path, int line_number) {
  if (value.size() < 2 || value.front() != '"' || value.back() != '"') {
    std::ostringstream out;
    out << path << ":" << line_number << ": expected quoted string value";
    throw std::runtime_error(out.str());
  }
  return value.substr(1, value.size() - 2);
}

uint32_t ParseUnsignedValue(const std::string& value, const std::filesystem::path& path, int line_number) {
  try {
    std::size_t parsed_chars = 0;
    const int base = (value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) ? 16 : 10;
    const uint64_t parsed = std::stoul(value, &parsed_chars, base);
    if (parsed_chars != value.size()) {
      throw std::invalid_argument("trailing characters");
    }
    return static_cast<uint32_t>(parsed);
  } catch (const std::exception&) {
    std::ostringstream out;
    out << path << ":" << line_number << ": invalid numeric value '" << value << "'";
    throw std::runtime_error(out.str());
  }
}

RomStatus ParseRomStatus(const std::string& value, const std::filesystem::path& path, int line_number) {
  if (value == "pass") {
    return RomStatus::kPass;
  }
  if (value == "fail") {
    return RomStatus::kFail;
  }
  if (value == "harness_error") {
    return RomStatus::kHarnessError;
  }

  std::ostringstream out;
  out << path << ":" << line_number << ": unsupported status '" << value << "'";
  throw std::runtime_error(out.str());
}

GoalObservationKind ParseGoalKind(const std::string& value, const std::filesystem::path& path, int line_number) {
  if (value == "cpu_a8") {
    return GoalObservationKind::kCpuA8;
  }
  if (value == "cpu_pc") {
    return GoalObservationKind::kCpuPc;
  }
  if (value == "wram_byte") {
    return GoalObservationKind::kWramByte;
  }
  if (value == "cpu_sp") {
    return GoalObservationKind::kCpuSp;
  }
  if (value == "cpu_dbr") {
    return GoalObservationKind::kCpuDbr;
  }
  if (value == "cpu_pbr") {
    return GoalObservationKind::kCpuPbr;
  }

  std::ostringstream out;
  out << path << ":" << line_number << ": unsupported goal kind '" << value << "'";
  throw std::runtime_error(out.str());
}

void ApplyScenarioField(RomScenario& scenario, const std::string& key, const std::string& value,
                        const std::filesystem::path& path, int line_number) {
  if (key == "rom_name") {
    scenario.rom_name = ParseQuotedString(value, path, line_number);
    return;
  }
  if (key == "title") {
    scenario.title = ParseQuotedString(value, path, line_number);
    return;
  }
  if (key == "purpose") {
    scenario.purpose = ParseQuotedString(value, path, line_number);
    return;
  }
  if (key == "why_expected_to_fail") {
    scenario.why_expected_to_fail = ParseQuotedString(value, path, line_number);
    return;
  }
  if (key == "cycle_budget") {
    scenario.cycle_budget = ParseUnsignedValue(value, path, line_number);
    return;
  }
  if (key == "expected_current_status") {
    scenario.expected_current_status = ParseRomStatus(ParseQuotedString(value, path, line_number), path, line_number);
    return;
  }
  if (key == "initial_dbr") {
    scenario.initial_dbr = static_cast<uint8_t>(ParseUnsignedValue(value, path, line_number));
    return;
  }

  std::ostringstream out;
  out << path << ":" << line_number << ": unknown scenario field '" << key << "'";
  throw std::runtime_error(out.str());
}

void ApplyGoalField(GoalSpec& goal, const std::string& key, const std::string& value, const std::filesystem::path& path,
                    int line_number) {
  if (key == "name") {
    goal.name = ParseQuotedString(value, path, line_number);
    return;
  }
  if (key == "description") {
    goal.description = ParseQuotedString(value, path, line_number);
    return;
  }
  if (key == "kind") {
    goal.kind = ParseGoalKind(ParseQuotedString(value, path, line_number), path, line_number);
    return;
  }
  if (key == "address") {
    goal.address = ParseUnsignedValue(value, path, line_number);
    return;
  }
  if (key == "expected_value") {
    goal.expected_value = ParseUnsignedValue(value, path, line_number);
    return;
  }

  std::ostringstream out;
  out << path << ":" << line_number << ": unknown goal field '" << key << "'";
  throw std::runtime_error(out.str());
}

void ValidateScenario(const RomScenario& scenario, const std::filesystem::path& path) {
  if (scenario.rom_name.empty()) {
    throw std::runtime_error(path.string() + ": rom_name is required");
  }
  if (scenario.title.empty()) {
    throw std::runtime_error(path.string() + ": title is required");
  }
  if (scenario.purpose.empty()) {
    throw std::runtime_error(path.string() + ": purpose is required");
  }
  if (scenario.cycle_budget == 0) {
    throw std::runtime_error(path.string() + ": cycle_budget must be greater than zero");
  }
  if (scenario.goals.empty()) {
    throw std::runtime_error(path.string() + ": at least one [goal] section is required");
  }

  for (const GoalSpec& goal : scenario.goals) {
    if (goal.name.empty()) {
      throw std::runtime_error(path.string() + ": goal name is required");
    }
    if (goal.description.empty()) {
      throw std::runtime_error(path.string() + ": goal description is required");
    }
  }
}

RomScenario LoadScenarioSpec(const std::filesystem::path& path) {
  std::ifstream stream(path);
  if (!stream.good()) {
    throw std::runtime_error("unable to open ROM spec: " + path.string());
  }

  RomScenario scenario{};
  std::optional<GoalSpec> current_goal;
  std::string line;
  int line_number = 0;

  while (std::getline(stream, line)) {
    line_number++;
    const std::string trimmed = StripCommentAndTrim(line);
    if (trimmed.empty()) {
      continue;
    }

    if (trimmed == "[goal]") {
      if (current_goal.has_value()) {
        scenario.goals.push_back(*current_goal);
      }
      current_goal = GoalSpec{};
      continue;
    }

    const std::size_t equals_pos = trimmed.find('=');
    if (equals_pos == std::string::npos) {
      std::ostringstream out;
      out << path << ":" << line_number << ": expected key = value entry";
      throw std::runtime_error(out.str());
    }

    const std::string key = Trim(std::string_view(trimmed).substr(0, equals_pos));
    const std::string value = Trim(std::string_view(trimmed).substr(equals_pos + 1));
    if (key.empty() || value.empty()) {
      std::ostringstream out;
      out << path << ":" << line_number << ": malformed key/value entry";
      throw std::runtime_error(out.str());
    }

    if (current_goal.has_value()) {
      ApplyGoalField(*current_goal, key, value, path, line_number);
    } else {
      ApplyScenarioField(scenario, key, value, path, line_number);
    }
  }

  if (current_goal.has_value()) {
    scenario.goals.push_back(*current_goal);
  }

  ValidateScenario(scenario, path);
  return scenario;
}

std::vector<RomScenario> LoadScenarioSpecs() {
  const std::filesystem::path spec_dir = std::filesystem::path(PUPSNES_TEST_ROM_METADATA_DIR);
  std::vector<std::filesystem::path> spec_paths;

  for (const auto& entry : std::filesystem::recursive_directory_iterator(spec_dir)) {
    if (entry.is_regular_file() && entry.path().extension() == ".romspec") {
      spec_paths.push_back(entry.path());
    }
  }

  std::sort(spec_paths.begin(), spec_paths.end());

  std::vector<RomScenario> scenarios;
  scenarios.reserve(spec_paths.size());
  for (const std::filesystem::path& spec_path : spec_paths) {
    scenarios.push_back(LoadScenarioSpec(spec_path));
  }

  return scenarios;
}

uint32_t ReadObservationValue(const GoalSpec& goal, const CPU& cpu, const WRAM& wram) {
  switch (goal.kind) {
    case GoalObservationKind::kCpuA8: return static_cast<uint8_t>(cpu.GetRegs().A);
    case GoalObservationKind::kCpuPc: return cpu.GetRegs().PC;
    case GoalObservationKind::kWramByte: return wram.Peek(goal.address);
    case GoalObservationKind::kCpuSp: return cpu.GetRegs().SP;
    case GoalObservationKind::kCpuDbr: return cpu.GetRegs().DBR;
    case GoalObservationKind::kCpuPbr: return cpu.GetRegs().PBR;
  }

  return 0;
}

std::string ObservationLabel(const GoalSpec& goal) {
  switch (goal.kind) {
    case GoalObservationKind::kCpuA8: return "cpu.a.low";
    case GoalObservationKind::kCpuPc: return "cpu.pc";
    case GoalObservationKind::kWramByte: {
      std::ostringstream out;
      out << "wram[$" << std::hex << std::uppercase << goal.address << "]";
      return out.str();
    }
    case GoalObservationKind::kCpuSp: return "cpu.sp";
    case GoalObservationKind::kCpuDbr: return "cpu.dbr";
    case GoalObservationKind::kCpuPbr: return "cpu.pbr";
  }

  return "unknown";
}

std::string FormatHex(uint32_t value) {
  std::ostringstream out;
  out << "0x" << std::hex << std::uppercase << value;
  return out.str();
}

std::string StatusLabel(RomStatus status) {
  switch (status) {
    case RomStatus::kPass: return "pass";
    case RomStatus::kFail: return "fail";
    case RomStatus::kHarnessError: return "harness_error";
  }

  return "unknown";
}

std::string SummarizeResult(const RomExecutionResult& result) {
  std::ostringstream out;
  out << "ROM " << result.scenario.rom_name << " (" << result.scenario.title << ")\n";
  out << "purpose: " << result.scenario.purpose << "\n";
  out << "cycle budget: " << result.scenario.cycle_budget << "\n";
  out << "expected current status: " << StatusLabel(result.scenario.expected_current_status) << "\n";
  out << "observed status: " << StatusLabel(result.status) << "\n";
  out << "matched goals: " << result.matched_goals << "/" << result.goal_results.size() << "\n";
  out << "cpu time: " << result.snapshot.cpu_time << ", master time: " << result.snapshot.master_time << "\n";
  out << "cpu.a.low: " << FormatHex(static_cast<uint8_t>(result.snapshot.cpu_regs.A))
      << ", cpu.pc: " << FormatHex(result.snapshot.cpu_regs.PC)
      << ", micro-op index: " << static_cast<unsigned>(result.snapshot.cpu_micro_op_index) << "\n";
  if (!result.failure_summary.empty()) {
    out << "summary: " << result.failure_summary << "\n";
  }
  for (const GoalResult& goal : result.goal_results) {
    out << (goal.matched ? "[pass] " : "[fail] ") << goal.spec.name << ": " << goal.spec.description << " | "
        << ObservationLabel(goal.spec) << " expected " << FormatHex(goal.spec.expected_value) << ", got "
        << FormatHex(goal.actual_value) << "\n";
  }
  return out.str();
}

RomExecutionResult RunScenario(const RomScenario& scenario) {
  RomExecutionResult result{};
  result.scenario = scenario;

  try {
    const std::filesystem::path rom_path = std::filesystem::path(PUPSNES_TEST_ROM_DIR) / (scenario.rom_name + ".sfc");
    const std::vector<uint8_t> rom_bytes = ReadBinaryFile(rom_path);

    SNES snes;
    WRAM& wram = snes.GetWram();
    CPU& cpu = snes.GetCpu();

    snes.LoadLoRom(rom_bytes);

    snes.Reset();
    if (scenario.initial_dbr.has_value()) {
      CPU::Regs regs = cpu.GetRegs();
      regs.DBR = *scenario.initial_dbr;
      cpu.SetRegs(regs);
    }

    static_cast<void>(cpu.TickToTarget(scenario.cycle_budget));

    // Under strict no-overshoot the CPU stops at the last micro-op boundary
    // that fits within the budget, so cpu_time may be slightly below
    // cycle_budget.  A time of zero (no progress at all) is a harness error.
    result.snapshot.cpu_time = cpu.GetTime();
    result.snapshot.master_time = snes.GetMasterTime();
    result.snapshot.cpu_regs = cpu.GetRegs();
    result.snapshot.cpu_micro_op_index = cpu.GetMicroOpIndex();

    if (result.snapshot.cpu_time == 0 && scenario.cycle_budget > 0) {
      result.status = RomStatus::kHarnessError;
      result.failure_summary = "ROM run did not reach the configured cycle budget";
    } else {
      for (const GoalSpec& goal : scenario.goals) {
        GoalResult goal_result{};
        goal_result.spec = goal;
        goal_result.actual_value = ReadObservationValue(goal, cpu, wram);
        goal_result.matched = goal_result.actual_value == goal.expected_value;
        if (goal_result.matched) {
          result.matched_goals++;
        }
        result.goal_results.push_back(goal_result);
      }

      if (result.matched_goals == result.goal_results.size()) {
        result.status = RomStatus::kPass;
        result.failure_summary = "all goals satisfied";
      } else {
        result.status = RomStatus::kFail;
        result.failure_summary = scenario.why_expected_to_fail;
      }
    }
  } catch (const std::exception& ex) {
    result.status = RomStatus::kHarnessError;
    result.failure_summary = ex.what();
  }

  return result;
}

}  // namespace

TEST_CASE(
    "ROM integration harness records current bring-up progress for each test "
    "ROM",
    "[integration][rom]") {
  const std::vector<RomScenario> scenarios = LoadScenarioSpecs();
  REQUIRE_FALSE(scenarios.empty());

  for (const RomScenario& scenario : scenarios) {
    const RomExecutionResult result = RunScenario(scenario);
    INFO(SummarizeResult(result));

    REQUIRE(result.status == scenario.expected_current_status);
    REQUIRE_FALSE(result.goal_results.empty());
    // Under strict no-overshoot the CPU stops at the last micro-op boundary
    // that fits within the budget, so cpu_time <= cycle_budget.  It must be
    // within 12 master cycles (one worst-case bus access) of the budget.
    REQUIRE(result.snapshot.cpu_time <= scenario.cycle_budget);
    REQUIRE(result.snapshot.cpu_time + 12 >= scenario.cycle_budget);
    if (scenario.expected_current_status == RomStatus::kPass) {
      REQUIRE(result.matched_goals == result.goal_results.size());
    } else {
      REQUIRE(result.matched_goals < result.goal_results.size());
    }
  }
}
