#include <catch2/catch_test_macros.hpp>

#include "pupsnes/hw/5a22/cpu.h"
#include "pupsnes/hw/cartridge.h"
#include "pupsnes/hw/scheduler.h"
#include "pupsnes/hw/snes.h"
#include "pupsnes/hw/wram.h"

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

using namespace pupsnes;

namespace {

enum class RomStatus : uint8_t {
    Pass = 0,
    Fail = 1,
    HarnessError = 2,
};

enum class GoalObservationKind : uint8_t {
    CpuA8 = 0,
    CpuPC = 1,
    WramByte = 2,
};

struct GoalSpec {
    std::string name;
    std::string description;
    GoalObservationKind kind = GoalObservationKind::CpuA8;
    uint32_t address = 0;
    uint32_t expected_value = 0;
};

struct RomScenario {
    std::string rom_name;
    std::string title;
    std::string purpose;
    std::string why_expected_to_fail;
    time_master_t cycle_budget = 0;
    RomStatus expected_current_status = RomStatus::Fail;
    std::vector<GoalSpec> goals;
};

struct GoalResult {
    GoalSpec spec;
    bool matched = false;
    uint32_t actual_value = 0;
};

struct RomExecutionSnapshot {
    time_master_t cpu_time = 0;
    time_master_t master_time = 0;
    CPU::Regs cpu_regs{};
    uint8_t cpu_micro_op_index = 0;
};

struct RomExecutionResult {
    RomScenario scenario;
    RomStatus status = RomStatus::HarnessError;
    std::string failure_summary;
    std::size_t matched_goals = 0;
    std::vector<GoalResult> goal_results;
    RomExecutionSnapshot snapshot{};
};

std::vector<uint8_t> readBinaryFile(const std::filesystem::path &path) {
    std::ifstream stream(path, std::ios::binary);
    REQUIRE(stream.good());

    return std::vector<uint8_t>(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

std::string trim(std::string_view text) {
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

std::string stripCommentAndTrim(const std::string &line) {
    const std::size_t comment_pos = line.find('#');
    const std::string_view without_comment =
        (comment_pos == std::string::npos) ? std::string_view(line) : std::string_view(line).substr(0, comment_pos);
    return trim(without_comment);
}

std::string parseQuotedString(const std::string &value, const std::filesystem::path &path, int line_number) {
    if (value.size() < 2 || value.front() != '"' || value.back() != '"') {
        std::ostringstream out;
        out << path << ":" << line_number << ": expected quoted string value";
        throw std::runtime_error(out.str());
    }
    return value.substr(1, value.size() - 2);
}

uint32_t parseUnsignedValue(const std::string &value, const std::filesystem::path &path, int line_number) {
    try {
        std::size_t parsed_chars = 0;
        const int base = (value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) ? 16 : 10;
        const unsigned long parsed = std::stoul(value, &parsed_chars, base);
        if (parsed_chars != value.size()) {
            throw std::invalid_argument("trailing characters");
        }
        return static_cast<uint32_t>(parsed);
    } catch (const std::exception &) {
        std::ostringstream out;
        out << path << ":" << line_number << ": invalid numeric value '" << value << "'";
        throw std::runtime_error(out.str());
    }
}

RomStatus parseRomStatus(const std::string &value, const std::filesystem::path &path, int line_number) {
    if (value == "pass") {
        return RomStatus::Pass;
    }
    if (value == "fail") {
        return RomStatus::Fail;
    }
    if (value == "harness_error") {
        return RomStatus::HarnessError;
    }

    std::ostringstream out;
    out << path << ":" << line_number << ": unsupported status '" << value << "'";
    throw std::runtime_error(out.str());
}

GoalObservationKind parseGoalKind(const std::string &value, const std::filesystem::path &path, int line_number) {
    if (value == "cpu_a8") {
        return GoalObservationKind::CpuA8;
    }
    if (value == "cpu_pc") {
        return GoalObservationKind::CpuPC;
    }
    if (value == "wram_byte") {
        return GoalObservationKind::WramByte;
    }

    std::ostringstream out;
    out << path << ":" << line_number << ": unsupported goal kind '" << value << "'";
    throw std::runtime_error(out.str());
}

void applyScenarioField(RomScenario &scenario, const std::string &key, const std::string &value,
                        const std::filesystem::path &path, int line_number) {
    if (key == "rom_name") {
        scenario.rom_name = parseQuotedString(value, path, line_number);
        return;
    }
    if (key == "title") {
        scenario.title = parseQuotedString(value, path, line_number);
        return;
    }
    if (key == "purpose") {
        scenario.purpose = parseQuotedString(value, path, line_number);
        return;
    }
    if (key == "why_expected_to_fail") {
        scenario.why_expected_to_fail = parseQuotedString(value, path, line_number);
        return;
    }
    if (key == "cycle_budget") {
        scenario.cycle_budget = parseUnsignedValue(value, path, line_number);
        return;
    }
    if (key == "expected_current_status") {
        scenario.expected_current_status = parseRomStatus(parseQuotedString(value, path, line_number), path, line_number);
        return;
    }

    std::ostringstream out;
    out << path << ":" << line_number << ": unknown scenario field '" << key << "'";
    throw std::runtime_error(out.str());
}

void applyGoalField(GoalSpec &goal, const std::string &key, const std::string &value, const std::filesystem::path &path,
                    int line_number) {
    if (key == "name") {
        goal.name = parseQuotedString(value, path, line_number);
        return;
    }
    if (key == "description") {
        goal.description = parseQuotedString(value, path, line_number);
        return;
    }
    if (key == "kind") {
        goal.kind = parseGoalKind(parseQuotedString(value, path, line_number), path, line_number);
        return;
    }
    if (key == "address") {
        goal.address = parseUnsignedValue(value, path, line_number);
        return;
    }
    if (key == "expected_value") {
        goal.expected_value = parseUnsignedValue(value, path, line_number);
        return;
    }

    std::ostringstream out;
    out << path << ":" << line_number << ": unknown goal field '" << key << "'";
    throw std::runtime_error(out.str());
}

void validateScenario(const RomScenario &scenario, const std::filesystem::path &path) {
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

    for (const GoalSpec &goal : scenario.goals) {
        if (goal.name.empty()) {
            throw std::runtime_error(path.string() + ": goal name is required");
        }
        if (goal.description.empty()) {
            throw std::runtime_error(path.string() + ": goal description is required");
        }
    }
}

RomScenario loadScenarioSpec(const std::filesystem::path &path) {
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
        const std::string trimmed = stripCommentAndTrim(line);
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

        const std::string key = trim(std::string_view(trimmed).substr(0, equals_pos));
        const std::string value = trim(std::string_view(trimmed).substr(equals_pos + 1));
        if (key.empty() || value.empty()) {
            std::ostringstream out;
            out << path << ":" << line_number << ": malformed key/value entry";
            throw std::runtime_error(out.str());
        }

        if (current_goal.has_value()) {
            applyGoalField(*current_goal, key, value, path, line_number);
        } else {
            applyScenarioField(scenario, key, value, path, line_number);
        }
    }

    if (current_goal.has_value()) {
        scenario.goals.push_back(*current_goal);
    }

    validateScenario(scenario, path);
    return scenario;
}

std::vector<RomScenario> loadScenarioSpecs() {
    const std::filesystem::path spec_dir = std::filesystem::path(PUPSNES_TEST_ROM_METADATA_DIR);
    std::vector<std::filesystem::path> spec_paths;

    for (const auto &entry : std::filesystem::recursive_directory_iterator(spec_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".romspec") {
            spec_paths.push_back(entry.path());
        }
    }

    std::sort(spec_paths.begin(), spec_paths.end());

    std::vector<RomScenario> scenarios;
    for (const std::filesystem::path &spec_path : spec_paths) {
        scenarios.push_back(loadScenarioSpec(spec_path));
    }

    return scenarios;
}

uint32_t readObservationValue(const GoalSpec &goal, const CPU &cpu, const WRAM &wram) {
    switch (goal.kind) {
    case GoalObservationKind::CpuA8:
        return static_cast<uint8_t>(cpu.regs().A);
    case GoalObservationKind::CpuPC:
        return cpu.regs().PC;
    case GoalObservationKind::WramByte:
        return wram.peek(goal.address);
    }

    return 0;
}

std::string observationLabel(const GoalSpec &goal) {
    switch (goal.kind) {
    case GoalObservationKind::CpuA8:
        return "cpu.a.low";
    case GoalObservationKind::CpuPC:
        return "cpu.pc";
    case GoalObservationKind::WramByte: {
        std::ostringstream out;
        out << "wram[$" << std::hex << std::uppercase << goal.address << "]";
        return out.str();
    }
    }

    return "unknown";
}

std::string formatHex(uint32_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << std::uppercase << value;
    return out.str();
}

std::string statusLabel(RomStatus status) {
    switch (status) {
    case RomStatus::Pass:
        return "pass";
    case RomStatus::Fail:
        return "fail";
    case RomStatus::HarnessError:
        return "harness_error";
    }

    return "unknown";
}

std::string summarizeResult(const RomExecutionResult &result) {
    std::ostringstream out;
    out << "ROM " << result.scenario.rom_name << " (" << result.scenario.title << ")\n";
    out << "purpose: " << result.scenario.purpose << "\n";
    out << "cycle budget: " << result.scenario.cycle_budget << "\n";
    out << "expected current status: " << statusLabel(result.scenario.expected_current_status) << "\n";
    out << "observed status: " << statusLabel(result.status) << "\n";
    out << "matched goals: " << result.matched_goals << "/" << result.goal_results.size() << "\n";
    out << "cpu time: " << result.snapshot.cpu_time << ", master time: " << result.snapshot.master_time << "\n";
    out << "cpu.a.low: " << formatHex(static_cast<uint8_t>(result.snapshot.cpu_regs.A))
        << ", cpu.pc: " << formatHex(result.snapshot.cpu_regs.PC)
        << ", micro-op index: " << static_cast<unsigned>(result.snapshot.cpu_micro_op_index) << "\n";
    if (!result.failure_summary.empty()) {
        out << "summary: " << result.failure_summary << "\n";
    }
    for (const GoalResult &goal : result.goal_results) {
        out << (goal.matched ? "[pass] " : "[fail] ") << goal.spec.name << ": " << goal.spec.description << " | "
            << observationLabel(goal.spec) << " expected " << formatHex(goal.spec.expected_value) << ", got "
            << formatHex(goal.actual_value) << "\n";
    }
    return out.str();
}

RomExecutionResult runScenario(const RomScenario &scenario) {
    RomExecutionResult result{};
    result.scenario = scenario;

    try {
        const std::filesystem::path rom_path = std::filesystem::path(PUPSNES_TEST_ROM_DIR) / (scenario.rom_name + ".sfc");
        const std::vector<uint8_t> rom_bytes = readBinaryFile(rom_path);

        SNES snes;
        Cartridge cartridge(&snes);
        WRAM wram(&snes);
        CPU cpu(&snes);

        cartridge.loadLoROM(rom_bytes);
        cartridge.mapLoROM(*snes.system_bus);
        wram.mapSystemBus(*snes.system_bus);

        cpu.reset();
        snes.scheduler->scheduleDeviceRun(&cpu, 0);
        snes.scheduler->scheduleEvent(scenario.cycle_budget, nullptr, SchedulerPhase::WakeSample, EventType::DeviceBoundary);

        constexpr std::size_t kMaxSchedulerSteps = 256;
        std::size_t steps = 0;
        while (cpu.getTime() < scenario.cycle_budget && steps < kMaxSchedulerSteps) {
            snes.scheduler->step();
            steps++;
        }

        if (cpu.getTime() < scenario.cycle_budget) {
            result.status = RomStatus::HarnessError;
            result.failure_summary = "ROM run did not reach the configured cycle budget";
        } else {
            result.snapshot.cpu_time = cpu.getTime();
            result.snapshot.master_time = snes.getMasterTime();
            result.snapshot.cpu_regs = cpu.regs();
            result.snapshot.cpu_micro_op_index = cpu.getMicroOpIndex();

            for (const GoalSpec &goal : scenario.goals) {
                GoalResult goal_result{};
                goal_result.spec = goal;
                goal_result.actual_value = readObservationValue(goal, cpu, wram);
                goal_result.matched = goal_result.actual_value == goal.expected_value;
                if (goal_result.matched) {
                    result.matched_goals++;
                }
                result.goal_results.push_back(goal_result);
            }

            if (result.matched_goals == result.goal_results.size()) {
                result.status = RomStatus::Pass;
                result.failure_summary = "all goals satisfied";
            } else {
                result.status = RomStatus::Fail;
                result.failure_summary = scenario.why_expected_to_fail;
            }
        }
    } catch (const std::exception &ex) {
        result.status = RomStatus::HarnessError;
        result.failure_summary = ex.what();
    }

    return result;
}

} // namespace

TEST_CASE("ROM integration harness records current bring-up progress for each test ROM", "[integration][rom]") {
    const std::vector<RomScenario> scenarios = loadScenarioSpecs();
    REQUIRE_FALSE(scenarios.empty());

    for (const RomScenario &scenario : scenarios) {
        const RomExecutionResult result = runScenario(scenario);
        INFO(summarizeResult(result));

        REQUIRE(result.status == scenario.expected_current_status);
        REQUIRE_FALSE(result.goal_results.empty());
        REQUIRE(result.snapshot.cpu_time == scenario.cycle_budget);
        if (scenario.expected_current_status == RomStatus::Pass) {
            REQUIRE(result.matched_goals == result.goal_results.size());
        } else {
            REQUIRE(result.matched_goals < result.goal_results.size());
        }
    }
}
