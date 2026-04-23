#include <fastipc/mpmc_shared_memory_transport.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <sys/resource.h>
#include <sys/utsname.h>
#include <unistd.h>

#ifndef FASTIPC_BUILD_TYPE
#define FASTIPC_BUILD_TYPE "unknown"
#endif

#ifndef FASTIPC_COMPILER
#define FASTIPC_COMPILER "unknown"
#endif

#ifndef FASTIPC_SOURCE_REVISION
#define FASTIPC_SOURCE_REVISION "unknown"
#endif

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr std::uint32_t kSchemaVersion = 1U;
constexpr std::size_t kMebibyte = 1024U * 1024U;
constexpr std::size_t kMaximumTrials = 100U;
constexpr std::size_t kMaximumParticipants = 64U;
constexpr std::size_t kMaximumTotalMessages = 100'000'000U;
constexpr std::uint64_t kChecksumSalt = 0xD6E8FEB86659FD93ULL;

struct Topology {
  std::size_t producers{1U};
  std::size_t consumers{1U};
};

struct Options {
  std::vector<Topology> topologies{
      {1U, 1U}, {2U, 2U}, {4U, 4U}};
  std::vector<std::size_t> payload_sizes{
      64U, 1024U, 64U * 1024U};
  std::optional<std::size_t> messages_per_producer;
  std::optional<std::size_t> warmup_messages_per_producer;
  std::size_t trials{1U};
  bool self_test{false};
};

struct WireHeader {
  std::uint64_t id{0U};
  std::uint64_t sent_monotonic_ns{0U};
  std::uint64_t checksum{0U};
};
static_assert(sizeof(WireHeader) == 24U);

struct ResourceSample {
  rusage usage{};
};

struct ContentionResult {
  std::string run_id;
  std::uint64_t case_id{0U};
  std::size_t trial{0U};
  std::size_t producers{0U};
  std::size_t consumers{0U};
  std::size_t payload_bytes{0U};
  std::size_t messages_per_producer{0U};
  std::size_t warmup_messages_per_producer{0U};
  std::uint64_t expected_messages{0U};
  std::uint64_t sent_messages{0U};
  std::uint64_t received_messages{0U};
  std::uint64_t missing_messages{0U};
  std::uint64_t duplicate_messages{0U};
  std::uint64_t checksum_errors{0U};
  std::uint64_t queue_send_timeouts{0U};
  std::uint64_t queue_receive_timeouts{0U};
  double wall_time_ms{0.0};
  double messages_per_second{0.0};
  double payload_mib_per_second{0.0};
  double p50_us{0.0};
  double p95_us{0.0};
  double p99_us{0.0};
  double p99_9_us{0.0};
  double max_us{0.0};
  double user_cpu_ms{0.0};
  double system_cpu_ms{0.0};
  double cpu_time_ms{0.0};
  double cpu_utilization_percent{0.0};
  std::int64_t voluntary_context_switches{0};
  std::int64_t involuntary_context_switches{0};
  std::int64_t peak_rss_kib{0};
};

[[nodiscard]] std::uint64_t MonotonicNanoseconds() noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          Clock::now().time_since_epoch())
          .count());
}

[[nodiscard]] std::string EscapeJson(std::string_view value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (const char raw_character : value) {
    const auto character = static_cast<unsigned char>(raw_character);
    switch (character) {
      case '"':
        escaped += "\\\"";
        break;
      case '\\':
        escaped += "\\\\";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      case '\t':
        escaped += "\\t";
        break;
      default:
        if (character < 0x20U) {
          std::ostringstream encoded;
          encoded << "\\u" << std::hex << std::setw(4)
                  << std::setfill('0')
                  << static_cast<unsigned int>(character);
          escaped += encoded.str();
        } else {
          escaped.push_back(static_cast<char>(character));
        }
        break;
    }
  }
  return escaped;
}

[[nodiscard]] std::string TimestampUtc() {
  const std::time_t now = std::time(nullptr);
  std::tm utc{};
  static_cast<void>(::gmtime_r(&now, &utc));
  std::array<char, 32U> buffer{};
  static_cast<void>(std::strftime(
      buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%SZ", &utc));
  return buffer.data();
}

[[nodiscard]] std::string Hostname() {
  std::array<char, 256U> buffer{};
  if (::gethostname(buffer.data(), buffer.size()) != 0) {
    return "unknown";
  }
  buffer.back() = '\0';
  return buffer.data();
}

[[nodiscard]] std::string CpuModel() {
  std::ifstream cpuinfo("/proc/cpuinfo");
  std::string line;
  while (std::getline(cpuinfo, line)) {
    constexpr std::string_view prefix = "model name";
    if (!line.starts_with(prefix)) {
      continue;
    }
    const auto separator = line.find(':');
    if (separator == std::string::npos) {
      continue;
    }
    const auto first = line.find_first_not_of(" \t", separator + 1U);
    return first == std::string::npos
               ? "unknown"
               : line.substr(first);
  }
  return "unknown";
}

[[nodiscard]] std::string RunId(
    std::string_view timestamp) {
  return std::string(timestamp) + "-" +
         std::to_string(::getpid()) + "-" +
         std::to_string(MonotonicNanoseconds()) + "-" +
         FASTIPC_SOURCE_REVISION;
}

[[nodiscard]] std::string EnvironmentJson(
    std::string_view run_id, std::string_view timestamp) {
  utsname system{};
  const bool have_uname = ::uname(&system) == 0;
  const long logical_cpus = ::sysconf(_SC_NPROCESSORS_ONLN);
  const long page_size = ::sysconf(_SC_PAGESIZE);

  std::ostringstream output;
  output << "{\"type\":\"environment\","
         << "\"schema_version\":" << kSchemaVersion << ','
         << "\"benchmark\":\"fastipc_mpmc_contention\","
         << "\"run_id\":\"" << EscapeJson(run_id) << "\","
         << "\"timestamp_utc\":\"" << EscapeJson(timestamp)
         << "\","
         << "\"hostname\":\"" << EscapeJson(Hostname()) << "\","
         << "\"kernel_release\":\""
         << EscapeJson(have_uname ? system.release : "unknown")
         << "\","
         << "\"machine\":\""
         << EscapeJson(have_uname ? system.machine : "unknown")
         << "\","
         << "\"cpu_model\":\"" << EscapeJson(CpuModel()) << "\","
         << "\"logical_cpus\":" << logical_cpus << ','
         << "\"page_size_bytes\":" << page_size << ','
         << "\"compiler\":\"" << EscapeJson(FASTIPC_COMPILER)
         << "\","
         << "\"build_type\":\"" << EscapeJson(FASTIPC_BUILD_TYPE)
         << "\","
         << "\"source_revision\":\""
         << EscapeJson(FASTIPC_SOURCE_REVISION) << "\"}";
  return output.str();
}

[[nodiscard]] double TimevalSeconds(
    const timeval& value) noexcept {
  return static_cast<double>(value.tv_sec) +
         static_cast<double>(value.tv_usec) / 1'000'000.0;
}

[[nodiscard]] fastipc::Status CaptureResource(
    ResourceSample* sample) {
  if (::getrusage(RUSAGE_SELF, &sample->usage) != 0) {
    return fastipc::Status(
        fastipc::StatusCode::IoError,
        "getrusage failed", errno);
  }
  return fastipc::Status::Ok();
}

[[nodiscard]] std::uint64_t NearestRank(
    const std::vector<std::uint64_t>& sorted,
    std::size_t numerator, std::size_t denominator) {
  const auto rank =
      (numerator * sorted.size() + denominator - 1U) /
      denominator;
  return sorted[std::max<std::size_t>(1U, rank) - 1U];
}

void FillPayload(
    std::span<std::byte> payload, std::uint64_t id) {
  for (std::size_t offset = sizeof(WireHeader);
       offset < payload.size(); ++offset) {
    const auto value =
        static_cast<unsigned char>((id + offset * 131U) % 251U);
    payload[offset] = static_cast<std::byte>(value);
  }

  WireHeader header;
  header.id = id;
  header.sent_monotonic_ns = MonotonicNanoseconds();
  header.checksum =
      header.id ^ header.sent_monotonic_ns ^ kChecksumSalt;
  std::memcpy(payload.data(), &header, sizeof(header));
}

[[nodiscard]] bool ValidatePayload(
    std::span<const std::byte> payload,
    std::uint64_t upper_bound, WireHeader* header) {
  if (payload.size() < sizeof(WireHeader)) {
    return false;
  }
  std::memcpy(header, payload.data(), sizeof(*header));
  if (header->id >= upper_bound ||
      header->checksum !=
          (header->id ^ header->sent_monotonic_ns ^
           kChecksumSalt)) {
    return false;
  }
  for (std::size_t offset = sizeof(WireHeader);
       offset < payload.size(); ++offset) {
    const auto value =
        static_cast<unsigned char>(
            (header->id + offset * 131U) % 251U);
    if (payload[offset] != static_cast<std::byte>(value)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::size_t NextPowerOfTwo(
    std::size_t value) {
  std::size_t result = 2U;
  while (result < value) {
    result *= 2U;
  }
  return result;
}

[[nodiscard]] std::size_t PreviousPowerOfTwo(
    std::size_t value) {
  std::size_t result = 1U;
  while (result <= value / 2U) {
    result *= 2U;
  }
  return std::max<std::size_t>(2U, result);
}

[[nodiscard]] fastipc::Result<ContentionResult> RunContention(
    std::string_view run_id, std::uint64_t case_id,
    std::size_t trial, const Topology& topology,
    std::size_t payload_bytes,
    std::size_t messages_per_producer,
    std::size_t warmup_messages_per_producer) {
  if (payload_bytes < sizeof(WireHeader)) {
    return fastipc::Status(
        fastipc::StatusCode::InvalidArgument,
        "MPMC contention payload must fit the wire header");
  }
  if (topology.producers == 0U || topology.consumers == 0U ||
      topology.producers > kMaximumParticipants ||
      topology.consumers > kMaximumParticipants) {
    return fastipc::Status(
        fastipc::StatusCode::InvalidArgument,
        "MPMC contention participant count is out of range");
  }
  if (messages_per_producer == 0U ||
      topology.producers >
          kMaximumTotalMessages / messages_per_producer) {
    return fastipc::Status(
        fastipc::StatusCode::InvalidArgument,
        "MPMC contention message count is out of range");
  }

  const std::size_t total_messages =
      topology.producers * messages_per_producer;
  const std::size_t desired_capacity = std::min<std::size_t>(
      4096U, std::max<std::size_t>(
                 64U, total_messages / 8U));
  constexpr std::size_t kMaximumQueueBytes =
      128U * 1024U * 1024U;
  const auto maximum_capacity = PreviousPowerOfTwo(
      std::max<std::size_t>(
          2U, kMaximumQueueBytes /
                  (payload_bytes + 64U)));
  const auto capacity = std::min(
      NextPowerOfTwo(desired_capacity), maximum_capacity);

  static std::atomic<std::uint64_t> channel_sequence{1U};
  fastipc::MpmcChannelConfig config;
  config.name =
      "fastipc_mpmc_benchmark_" + std::to_string(::getpid()) +
      "_" + std::to_string(channel_sequence.fetch_add(
                  1U, std::memory_order_relaxed));
  config.capacity = static_cast<std::uint32_t>(capacity);
  config.max_message_size =
      static_cast<std::uint32_t>(payload_bytes);
  config.active_spin_count = 256U;
  config.unlink_on_owner_close = true;

  auto transport_result =
      fastipc::MpmcSharedMemoryTransport::Create(config);
  if (!transport_result) {
    return transport_result.status();
  }
  auto transport = std::move(transport_result).take_value();

  std::vector<std::atomic<std::uint32_t>> seen(total_messages);
  std::vector<std::atomic<std::uint64_t>> latency_ns(total_messages);
  for (auto& value : seen) {
    value.store(0U, std::memory_order_relaxed);
  }
  for (auto& value : latency_ns) {
    value.store(0U, std::memory_order_relaxed);
  }

  std::atomic<std::uint64_t> consumed{0U};
  std::atomic<std::size_t> producers_finished{0U};
  std::atomic<int> failure{0};
  std::barrier start(static_cast<std::ptrdiff_t>(
      topology.producers + topology.consumers + 1U));
  std::vector<std::jthread> workers;
  workers.reserve(topology.producers + topology.consumers);

  auto fail = [&](int code) {
    int expected = 0;
    if (failure.compare_exchange_strong(
            expected, code, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
      transport->Close();
    }
  };

  for (std::size_t producer_index = 0U;
       producer_index < topology.producers;
       ++producer_index) {
    workers.emplace_back([&, producer_index] {
      std::vector<std::byte> payload(payload_bytes);
      start.arrive_and_wait();
      for (std::size_t offset = 0U;
           offset < messages_per_producer; ++offset) {
        if (failure.load(std::memory_order_acquire) != 0) {
          break;
        }
        const auto id =
            static_cast<std::uint64_t>(
                producer_index * messages_per_producer + offset);
        FillPayload(payload, id);
        const auto status = transport->Send(
            payload,
            {fastipc::BackpressurePolicy::Block,
             fastipc::Deadline::After(5s)});
        if (!status) {
          fail(10);
          break;
        }
      }
      producers_finished.fetch_add(
          1U, std::memory_order_release);
    });
  }

  for (std::size_t consumer_index = 0U;
       consumer_index < topology.consumers;
       ++consumer_index) {
    workers.emplace_back([&, consumer_index] {
      static_cast<void>(consumer_index);
      std::vector<std::byte> payload(payload_bytes);
      start.arrive_and_wait();
      for (;;) {
        if (failure.load(std::memory_order_acquire) != 0) {
          return;
        }
        if (consumed.load(std::memory_order_acquire) >=
            total_messages) {
          return;
        }

        auto received = transport->Receive(
            payload, fastipc::Deadline::After(100ms));
        if (!received) {
          if (consumed.load(std::memory_order_acquire) >=
              total_messages) {
            return;
          }
          if (received.status().code() ==
                  fastipc::StatusCode::Timeout &&
              producers_finished.load(std::memory_order_acquire) <
                  topology.producers) {
            continue;
          }
          fail(20);
          return;
        }
        if (received.value() != payload_bytes) {
          fail(21);
          return;
        }

        WireHeader header;
        if (!ValidatePayload(
                payload, static_cast<std::uint64_t>(total_messages),
                &header)) {
          fail(22);
          return;
        }
        const auto now = MonotonicNanoseconds();
        const auto elapsed =
            now > header.sent_monotonic_ns
                ? now - header.sent_monotonic_ns
                : 1U;
        seen[header.id].fetch_add(
            1U, std::memory_order_relaxed);
        latency_ns[header.id].store(
            elapsed, std::memory_order_relaxed);
        const auto completed = consumed.fetch_add(
                                   1U, std::memory_order_acq_rel) +
                               1U;
        if (completed == total_messages) {
          transport->Close();
          return;
        }
      }
    });
  }

  ResourceSample before;
  const auto before_status = CaptureResource(&before);
  if (!before_status) {
    fail(30);
  }
  const auto wall_started = Clock::now();
  start.arrive_and_wait();
  workers.clear();
  const auto wall_finished = Clock::now();

  ResourceSample after;
  const auto after_status = CaptureResource(&after);
  if (!after_status) {
    return after_status;
  }
  if (failure.load(std::memory_order_acquire) != 0) {
    return fastipc::Status(
        fastipc::StatusCode::CorruptData,
        "MPMC contention worker failed with code " +
            std::to_string(
                failure.load(std::memory_order_relaxed)));
  }

  std::uint64_t missing_messages = 0U;
  std::uint64_t duplicate_messages = 0U;
  std::vector<std::uint64_t> sorted_latency;
  sorted_latency.reserve(total_messages);
  for (std::size_t index = 0U;
       index < total_messages; ++index) {
    const auto count =
        seen[index].load(std::memory_order_relaxed);
    if (count == 0U) {
      ++missing_messages;
    } else if (count > 1U) {
      duplicate_messages +=
          static_cast<std::uint64_t>(count - 1U);
    }
    const auto latency =
        latency_ns[index].load(std::memory_order_relaxed);
    if (latency > 0U) {
      sorted_latency.push_back(latency);
    }
  }
  if (missing_messages != 0U ||
      duplicate_messages != 0U ||
      sorted_latency.size() != total_messages) {
    return fastipc::Status(
        fastipc::StatusCode::CorruptData,
        "MPMC contention exact-once validation failed");
  }

  std::sort(sorted_latency.begin(), sorted_latency.end());
  const std::chrono::duration<double> wall_duration =
      wall_finished - wall_started;
  const double wall_seconds = wall_duration.count();
  if (wall_seconds <= 0.0) {
    return fastipc::Status(
        fastipc::StatusCode::IoError,
        "MPMC contention measured an empty duration");
  }

  const double user_seconds =
      TimevalSeconds(after.usage.ru_utime) -
      TimevalSeconds(before.usage.ru_utime);
  const double system_seconds =
      TimevalSeconds(after.usage.ru_stime) -
      TimevalSeconds(before.usage.ru_stime);
  const double cpu_seconds = user_seconds + system_seconds;
  const auto stats = transport->Stats();

  ContentionResult result;
  result.run_id = std::string(run_id);
  result.case_id = case_id;
  result.trial = trial;
  result.producers = topology.producers;
  result.consumers = topology.consumers;
  result.payload_bytes = payload_bytes;
  result.messages_per_producer = messages_per_producer;
  result.warmup_messages_per_producer =
      warmup_messages_per_producer;
  result.expected_messages =
      static_cast<std::uint64_t>(total_messages);
  result.sent_messages = stats.sent_messages;
  result.received_messages = stats.received_messages;
  result.missing_messages = missing_messages;
  result.duplicate_messages = duplicate_messages;
  result.queue_send_timeouts = stats.send_timeouts;
  result.queue_receive_timeouts = stats.receive_timeouts;
  result.wall_time_ms = wall_seconds * 1000.0;
  result.messages_per_second =
      static_cast<double>(total_messages) / wall_seconds;
  result.payload_mib_per_second =
      static_cast<double>(total_messages) *
      static_cast<double>(payload_bytes) /
      (static_cast<double>(kMebibyte) * wall_seconds);
  result.p50_us = static_cast<double>(
      NearestRank(sorted_latency, 50U, 100U)) /
      1000.0;
  result.p95_us = static_cast<double>(
      NearestRank(sorted_latency, 95U, 100U)) /
      1000.0;
  result.p99_us = static_cast<double>(
      NearestRank(sorted_latency, 99U, 100U)) /
      1000.0;
  result.p99_9_us = static_cast<double>(
      NearestRank(sorted_latency, 999U, 1000U)) /
      1000.0;
  result.max_us =
      static_cast<double>(sorted_latency.back()) / 1000.0;
  result.user_cpu_ms = user_seconds * 1000.0;
  result.system_cpu_ms = system_seconds * 1000.0;
  result.cpu_time_ms = cpu_seconds * 1000.0;
  result.cpu_utilization_percent =
      cpu_seconds / wall_seconds * 100.0;
  result.voluntary_context_switches =
      static_cast<std::int64_t>(after.usage.ru_nvcsw) -
      static_cast<std::int64_t>(before.usage.ru_nvcsw);
  result.involuntary_context_switches =
      static_cast<std::int64_t>(after.usage.ru_nivcsw) -
      static_cast<std::int64_t>(before.usage.ru_nivcsw);
  result.peak_rss_kib =
      static_cast<std::int64_t>(after.usage.ru_maxrss);
  return result;
}

[[nodiscard]] std::string ResultJson(
    const ContentionResult& result) {
  std::ostringstream output;
  output << std::fixed << std::setprecision(3)
         << "{\"type\":\"result\","
         << "\"schema_version\":" << kSchemaVersion << ','
         << "\"benchmark\":\"fastipc_mpmc_contention\","
         << "\"run_id\":\"" << EscapeJson(result.run_id) << "\","
         << "\"case_id\":" << result.case_id << ','
         << "\"trial\":" << result.trial << ','
         << "\"status\":\"ok\","
         << "\"queue\":\"bounded_mpmc_sequence_futex_copy\","
         << "\"access_pattern\":\"touch_memory\","
         << "\"producers\":" << result.producers << ','
         << "\"consumers\":" << result.consumers << ','
         << "\"payload_bytes\":" << result.payload_bytes << ','
         << "\"messages_per_producer\":"
         << result.messages_per_producer << ','
         << "\"warmup_messages_per_producer\":"
         << result.warmup_messages_per_producer << ','
         << "\"expected_messages\":"
         << result.expected_messages << ','
         << "\"sent_messages\":" << result.sent_messages << ','
         << "\"received_messages\":"
         << result.received_messages << ','
         << "\"missing_messages\":"
         << result.missing_messages << ','
         << "\"duplicate_messages\":"
         << result.duplicate_messages << ','
         << "\"checksum_errors\":"
         << result.checksum_errors << ','
         << "\"queue_send_timeouts\":"
         << result.queue_send_timeouts << ','
         << "\"queue_receive_timeouts\":"
         << result.queue_receive_timeouts << ','
         << "\"wall_time_ms\":" << result.wall_time_ms << ','
         << "\"messages_per_second\":"
         << result.messages_per_second << ','
         << "\"payload_mib_per_second\":"
         << result.payload_mib_per_second << ','
         << "\"p50_us\":" << result.p50_us << ','
         << "\"p95_us\":" << result.p95_us << ','
         << "\"p99_us\":" << result.p99_us << ','
         << "\"p99_9_us\":" << result.p99_9_us << ','
         << "\"max_us\":" << result.max_us << ','
         << "\"user_cpu_ms\":" << result.user_cpu_ms << ','
         << "\"system_cpu_ms\":" << result.system_cpu_ms << ','
         << "\"cpu_time_ms\":" << result.cpu_time_ms << ','
         << "\"cpu_utilization_percent\":"
         << result.cpu_utilization_percent << ','
         << "\"voluntary_context_switches\":"
         << result.voluntary_context_switches << ','
         << "\"involuntary_context_switches\":"
         << result.involuntary_context_switches << ','
         << "\"peak_rss_kib\":" << result.peak_rss_kib
         << '}';
  return output.str();
}

[[nodiscard]] std::size_t ParsePositiveSize(
    std::string_view text) {
  if (text.empty()) {
    throw std::invalid_argument("numeric option is empty");
  }
  std::size_t multiplier = 1U;
  const char suffix = text.back();
  if (suffix == 'K' || suffix == 'k') {
    multiplier = 1024U;
    text.remove_suffix(1U);
  } else if (suffix == 'M' || suffix == 'm') {
    multiplier = 1024U * 1024U;
    text.remove_suffix(1U);
  }

  std::size_t value = 0U;
  const auto parsed = std::from_chars(
      text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} ||
      parsed.ptr != text.data() + text.size() ||
      value == 0U ||
      value > std::numeric_limits<std::size_t>::max() /
                  multiplier) {
    throw std::invalid_argument(
        "invalid positive size: " + std::string(text));
  }
  return value * multiplier;
}

[[nodiscard]] Topology ParseTopology(
    std::string_view text) {
  const auto separator = text.find('x');
  if (separator == std::string_view::npos) {
    throw std::invalid_argument(
        "topology must use PRODUCERSxCONSUMERS");
  }
  const auto producers =
      ParsePositiveSize(text.substr(0U, separator));
  const auto consumers =
      ParsePositiveSize(text.substr(separator + 1U));
  if (producers > kMaximumParticipants ||
      consumers > kMaximumParticipants) {
    throw std::invalid_argument(
        "topology participant count must not exceed 64");
  }
  return {producers, consumers};
}

[[nodiscard]] std::string_view OptionValue(
    std::string_view argument, std::string_view prefix) {
  return argument.starts_with(prefix)
             ? argument.substr(prefix.size())
             : std::string_view{};
}

void PrintHelp() {
  std::cout
      << "Usage: fastipc_mpmc_benchmark [options]\n"
      << "  --self-test             run a short 1P1C and 2P2C schema check\n"
      << "  --topology=PxC          run one producer/consumer topology\n"
      << "  --payload=BYTES         run one payload size; K/M suffixes work\n"
      << "  --messages=COUNT        measured messages per producer\n"
      << "  --warmup=COUNT          warmup messages per producer\n"
      << "  --trials=COUNT          independent trials per logical case\n"
      << "  --help                  show this message\n";
}

[[nodiscard]] Options ParseOptions(
    int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--self-test") {
      options.self_test = true;
      continue;
    }
    if (argument == "--help") {
      PrintHelp();
      std::exit(0);
    }
    if (const auto value =
            OptionValue(argument, "--topology=");
        !value.empty()) {
      options.topologies = {ParseTopology(value)};
      continue;
    }
    if (const auto value =
            OptionValue(argument, "--payload=");
        !value.empty()) {
      options.payload_sizes = {ParsePositiveSize(value)};
      continue;
    }
    if (const auto value =
            OptionValue(argument, "--messages=");
        !value.empty()) {
      options.messages_per_producer =
          ParsePositiveSize(value);
      continue;
    }
    if (const auto value =
            OptionValue(argument, "--warmup=");
        !value.empty()) {
      options.warmup_messages_per_producer =
          ParsePositiveSize(value);
      continue;
    }
    if (const auto value =
            OptionValue(argument, "--trials=");
        !value.empty()) {
      options.trials = ParsePositiveSize(value);
      if (options.trials > kMaximumTrials) {
        throw std::invalid_argument(
            "trials must not exceed 100");
      }
      continue;
    }
    throw std::invalid_argument(
        "unknown option: " + std::string(argument));
  }

  if (options.self_test) {
    options.topologies = {{1U, 1U}, {2U, 2U}};
    options.payload_sizes = {64U};
    options.messages_per_producer = 200U;
    options.warmup_messages_per_producer = 20U;
    options.trials = 1U;
  }
  return options;
}

[[nodiscard]] std::size_t DefaultMessages(
    std::size_t payload_bytes) {
  constexpr std::size_t target_bytes =
      64U * 1024U * 1024U;
  return std::clamp(
      target_bytes / payload_bytes,
      std::size_t{1000U}, std::size_t{100'000U});
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto options = ParseOptions(argc, argv);
    const auto timestamp = TimestampUtc();
    const auto run_id = RunId(timestamp);
    std::cout << EnvironmentJson(run_id, timestamp)
              << std::endl;

    std::uint64_t case_id = 1U;
    for (const auto& topology : options.topologies) {
      for (const auto payload_bytes : options.payload_sizes) {
        if (payload_bytes < sizeof(WireHeader) ||
            payload_bytes > 16U * 1024U * 1024U) {
          throw std::invalid_argument(
              "payload must be between 24 bytes and 16 MiB");
        }
        const auto messages =
            options.messages_per_producer.value_or(
                DefaultMessages(payload_bytes));
        const auto warmup =
            options.warmup_messages_per_producer.value_or(
                std::max<std::size_t>(
                    1U, std::min<std::size_t>(
                            1000U, messages / 10U)));
        const auto logical_case_id = case_id++;

        for (std::size_t trial = 1U;
             trial <= options.trials; ++trial) {
          auto warmup_result = RunContention(
              run_id, logical_case_id, trial, topology,
              payload_bytes, warmup, 0U);
          if (!warmup_result) {
            std::cerr << "MPMC contention warmup failed: "
                      << warmup_result.status().ToString() << '\n';
            return 1;
          }

          auto result = RunContention(
              run_id, logical_case_id, trial, topology,
              payload_bytes, messages, warmup);
          if (!result) {
            std::cerr << "MPMC contention case failed: "
                      << result.status().ToString() << '\n';
            return 1;
          }
          std::cout << ResultJson(result.value())
                    << std::endl;
        }
      }
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "MPMC benchmark configuration error: "
              << error.what() << '\n';
    return 2;
  }
}
