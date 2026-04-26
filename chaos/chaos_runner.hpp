#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include <fastipc/status.hpp>

namespace fastipc::chaos {

enum class Operation : std::uint8_t {
  KillProducer,
  KillConsumer,
  RestartProducer,
  RestartConsumer,
  SlowConsumer,
  QueuePressure,
  Timeout,
  DelayWakeup,
};

[[nodiscard]] std::string_view ToString(Operation operation) noexcept;

[[nodiscard]] std::vector<Operation> GenerateOperationPlan(
    std::uint64_t seed, std::size_t operation_count);

enum class DeliveryClassification : std::uint8_t {
  Ok,
  Duplicate,
  Unexpected,
};

class SequenceLedger {
 public:
  [[nodiscard]] std::uint64_t NextCandidate() const noexcept;
  [[nodiscard]] bool RecordPublished(std::uint64_t sequence);
  [[nodiscard]] DeliveryClassification RecordConsumed(
      std::uint64_t observed_sequence,
      std::uint64_t expected_sequence) noexcept;

  [[nodiscard]] std::size_t outstanding_count() const noexcept {
    return outstanding_.size();
  }
  [[nodiscard]] std::size_t maximum_outstanding_count() const noexcept {
    return maximum_outstanding_count_;
  }

 private:
  std::deque<std::uint64_t> outstanding_;
  std::uint64_t last_published_sequence_{0U};
  std::uint64_t last_consumed_sequence_{0U};
  std::size_t maximum_outstanding_count_{0U};
};

struct RunnerConfig {
  std::uint64_t seed{20260821U};
  std::size_t minimum_operations{16U};
  std::chrono::milliseconds minimum_duration{0};
  std::uint32_t slot_count{2U};
  std::size_t payload_size{256U};
  std::chrono::milliseconds peer_timeout{50};
  std::chrono::milliseconds command_timeout{1500};
  std::chrono::milliseconds delay{20};
  std::size_t latency_window{32U};
  std::optional<std::int64_t> max_memory_growth_kib;
  std::optional<double> max_p99_drift_us;
  std::string run_id;
};

[[nodiscard]] Status ValidateConfig(const RunnerConfig& config);

class Runner {
 public:
  explicit Runner(RunnerConfig config);
  ~Runner();

  Runner(const Runner&) = delete;
  Runner& operator=(const Runner&) = delete;
  Runner(Runner&&) noexcept;
  Runner& operator=(Runner&&) noexcept;

  [[nodiscard]] Status Run(
      std::ostream& output, std::ostream* mirror = nullptr);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace fastipc::chaos
