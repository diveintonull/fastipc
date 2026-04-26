#include "chaos/chaos_runner.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace {

bool Check(bool condition, const char* expression, int line) {
  if (condition) {
    return true;
  }
  std::cerr << "line " << line << ": check failed: " << expression << '\n';
  return false;
}

#define CHECK(expression)                                                     \
  do {                                                                        \
    if (!Check(static_cast<bool>(expression), #expression, __LINE__)) {        \
      return 1;                                                               \
    }                                                                         \
  } while (false)

int DeterministicPlanCoversEveryOperation() {
  constexpr std::uint64_t kSeed = 20260821U;
  const auto first = fastipc::chaos::GenerateOperationPlan(kSeed, 24U);
  const auto repeated = fastipc::chaos::GenerateOperationPlan(kSeed, 24U);
  const auto different =
      fastipc::chaos::GenerateOperationPlan(kSeed + 1U, 24U);

  CHECK(first.size() == 24U);
  CHECK(first == repeated);
  CHECK(first != different);

  std::array<bool, 8U> seen{};
  for (std::size_t index = 0U; index < 8U; ++index) {
    const auto operation_index = static_cast<std::size_t>(first[index]);
    CHECK(operation_index < seen.size());
    CHECK(!seen[operation_index]);
    seen[operation_index] = true;
  }
  for (const bool present : seen) {
    CHECK(present);
  }
  return 0;
}

std::size_t CountOccurrences(
    std::string_view text, std::string_view needle) {
  std::size_t count = 0U;
  std::size_t offset = 0U;
  while ((offset = text.find(needle, offset)) != std::string_view::npos) {
    ++count;
    offset += needle.size();
  }
  return count;
}

int SeededRunProducesExactSummary() {
  fastipc::chaos::RunnerConfig config;
  config.seed = 20260821U;
  config.minimum_operations = 8U;
  config.minimum_duration = std::chrono::milliseconds::zero();
  config.slot_count = 2U;
  config.payload_size = 64U;
  config.peer_timeout = std::chrono::milliseconds(30);
  config.command_timeout = std::chrono::milliseconds(1500);
  config.delay = std::chrono::milliseconds(10);
  config.latency_window = 4U;
  config.run_id = "ctest-summary";

  std::ostringstream evidence;
  fastipc::chaos::Runner runner(config);
  const auto status = runner.Run(evidence);
  CHECK(status.ok());

  const std::string output = evidence.str();
  CHECK(CountOccurrences(output, "\"record_type\":\"environment\"") == 1U);
  CHECK(CountOccurrences(output, "\"record_type\":\"operation\"") == 8U);
  CHECK(CountOccurrences(output, "\"record_type\":\"summary\"") == 1U);
  CHECK(CountOccurrences(output, "\"operation\":\"KillProducer\"") == 1U);
  CHECK(CountOccurrences(output, "\"operation\":\"KillConsumer\"") == 1U);
  CHECK(CountOccurrences(output, "\"operation\":\"RestartProducer\"") == 1U);
  CHECK(CountOccurrences(output, "\"operation\":\"RestartConsumer\"") == 1U);
  CHECK(CountOccurrences(output, "\"operation\":\"SlowConsumer\"") == 1U);
  CHECK(CountOccurrences(output, "\"operation\":\"QueuePressure\"") == 1U);
  CHECK(CountOccurrences(output, "\"operation\":\"Timeout\"") == 1U);
  CHECK(CountOccurrences(output, "\"operation\":\"DelayWakeup\"") == 1U);
  CHECK(output.find("\"status\":\"passed\"") != std::string::npos);
  CHECK(output.find("\"operation_count\":8") != std::string::npos);
  CHECK(output.find("\"crash_count\":2") != std::string::npos);
  CHECK(output.find("\"restart_count\":4") != std::string::npos);
  CHECK(output.find("\"recovery_count\":4") != std::string::npos);
  CHECK(output.find("\"checksum_mismatches\":0") != std::string::npos);
  CHECK(output.find("\"lost_messages\":0") != std::string::npos);
  CHECK(output.find("\"duplicate_messages\":0") != std::string::npos);
  CHECK(output.find("\"unexpected_messages\":0") != std::string::npos);
  CHECK(output.find("\"unexpected_timeouts\":0") != std::string::npos);
  CHECK(output.find("\"operation_failures\":0") != std::string::npos);
  CHECK(output.find("\"cleanup_failures\":0") != std::string::npos);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--seeded-summary") {
    return SeededRunProducesExactSummary();
  }
  return DeterministicPlanCoversEveryOperation();
}
