#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fastipc/status.hpp>

namespace fastipc::benchmark {

enum class TransportKind : std::uint8_t {
  SharedMemory,
  UnixDomainSocket,
  Pipe,
};

struct CaseConfig {
  TransportKind transport{TransportKind::SharedMemory};
  std::size_t payload_bytes{64U};
  std::size_t iterations{100U};
  std::size_t warmup_iterations{10U};
  std::chrono::milliseconds operation_timeout{5000};
  std::uint64_t case_id{0U};
};

struct BenchmarkResult {
  std::string transport;
  std::string transport_mode;
  std::size_t payload_bytes{0U};
  std::size_t iterations{0U};
  std::size_t warmup_iterations{0U};
  double wall_time_ms{0.0};
  double round_trips_per_second{0.0};
  double messages_per_second{0.0};
  double payload_mib_per_second{0.0};
  double p50_us{0.0};
  double p95_us{0.0};
  double p99_us{0.0};
  double user_cpu_ms{0.0};
  double system_cpu_ms{0.0};
  double cpu_time_ms{0.0};
  double cpu_utilization_percent{0.0};
  std::int64_t voluntary_context_switches{0};
  std::int64_t involuntary_context_switches{0};
  std::int64_t parent_peak_rss_kib{0};
  std::int64_t child_peak_rss_kib{0};
};

struct Environment {
  std::string timestamp_utc;
  std::string hostname;
  std::string kernel_release;
  std::string machine;
  std::string cpu_model;
  std::string compiler;
  std::string build_type;
  std::string source_revision;
  std::int64_t logical_cpus{0};
  std::int64_t page_size_bytes{0};
  std::int64_t memory_total_kib{0};
};

[[nodiscard]] const char* TransportName(TransportKind transport) noexcept;
[[nodiscard]] std::optional<TransportKind> ParseTransport(
    std::string_view name) noexcept;
[[nodiscard]] std::vector<std::size_t> RequiredPayloadSizes();
[[nodiscard]] std::size_t DefaultIterations(std::size_t payload_bytes);
[[nodiscard]] std::size_t DefaultWarmupIterations(
    std::size_t iterations);

[[nodiscard]] Result<BenchmarkResult> RunCase(const CaseConfig& config);
[[nodiscard]] Environment CaptureEnvironment();
[[nodiscard]] std::string ToJson(const Environment& environment);
[[nodiscard]] std::string ToJson(const BenchmarkResult& result);

}  // namespace fastipc::benchmark
