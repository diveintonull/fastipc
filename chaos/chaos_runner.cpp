#include "chaos/chaos_runner.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <random>

namespace fastipc::chaos {

std::string_view ToString(Operation operation) noexcept {
  switch (operation) {
    case Operation::KillProducer:
      return "KillProducer";
    case Operation::KillConsumer:
      return "KillConsumer";
    case Operation::RestartProducer:
      return "RestartProducer";
    case Operation::RestartConsumer:
      return "RestartConsumer";
    case Operation::SlowConsumer:
      return "SlowConsumer";
    case Operation::QueuePressure:
      return "QueuePressure";
    case Operation::Timeout:
      return "Timeout";
    case Operation::DelayWakeup:
      return "DelayWakeup";
  }
  return "Unknown";
}

std::vector<Operation> GenerateOperationPlan(
    std::uint64_t seed, std::size_t operation_count) {
  constexpr std::array<Operation, 8U> kAllOperations{
      Operation::KillProducer,
      Operation::KillConsumer,
      Operation::RestartProducer,
      Operation::RestartConsumer,
      Operation::SlowConsumer,
      Operation::QueuePressure,
      Operation::Timeout,
      Operation::DelayWakeup,
  };

  std::mt19937_64 random(seed);
  std::vector<Operation> plan;
  plan.reserve(operation_count);
  while (plan.size() < operation_count) {
    auto cycle = kAllOperations;
    std::shuffle(cycle.begin(), cycle.end(), random);
    const auto remaining = operation_count - plan.size();
    const auto count = std::min(remaining, cycle.size());
    plan.insert(plan.end(), cycle.begin(),
                cycle.begin() + static_cast<std::ptrdiff_t>(count));
  }
  return plan;
}

std::uint64_t SequenceLedger::NextCandidate() const noexcept {
  if (last_published_sequence_ ==
      std::numeric_limits<std::uint64_t>::max()) {
    return 0U;
  }
  return last_published_sequence_ + 1U;
}

bool SequenceLedger::RecordPublished(std::uint64_t sequence) {
  if (sequence == 0U || sequence != NextCandidate()) {
    return false;
  }
  outstanding_.push_back(sequence);
  last_published_sequence_ = sequence;
  maximum_outstanding_count_ =
      std::max(maximum_outstanding_count_, outstanding_.size());
  return true;
}

DeliveryClassification SequenceLedger::RecordConsumed(
    std::uint64_t observed_sequence,
    std::uint64_t expected_sequence) noexcept {
  if (observed_sequence <= last_consumed_sequence_) {
    return DeliveryClassification::Duplicate;
  }
  if (observed_sequence != expected_sequence ||
      outstanding_.empty() ||
      outstanding_.front() != observed_sequence) {
    return DeliveryClassification::Unexpected;
  }
  outstanding_.pop_front();
  last_consumed_sequence_ = observed_sequence;
  return DeliveryClassification::Ok;
}

void LatencyWindows::Record(double sample) {
  ++sample_count_;
  if (baseline_.size() < capacity_) {
    baseline_.push_back(sample);
  }
  if (capacity_ == 0U) {
    return;
  }
  final_.push_back(sample);
  if (final_.size() > capacity_) {
    final_.pop_front();
  }
}

std::size_t LatencyWindows::ComparisonWindowSize() const noexcept {
  return std::min(capacity_, sample_count_ / 2U);
}

std::vector<double> LatencyWindows::Baseline() const {
  const auto count = ComparisonWindowSize();
  return std::vector<double>(
      baseline_.begin(),
      baseline_.begin() + static_cast<std::ptrdiff_t>(count));
}

std::vector<double> LatencyWindows::Final() const {
  const auto count = ComparisonWindowSize();
  return std::vector<double>(
      final_.end() - static_cast<std::ptrdiff_t>(count),
      final_.end());
}

}  // namespace fastipc::chaos
