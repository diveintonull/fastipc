#include "benchmark_runner.hpp"

#include <fastipc/deadline.hpp>
#include <fastipc/shared_memory_transport.hpp>
#include <fastipc/transport.hpp>
#include <fastipc/unix_domain_socket_transport.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <poll.h>
#include <signal.h>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <sys/wait.h>
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

namespace fastipc::benchmark {
namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr std::size_t kMebibyte = 1024U * 1024U;
constexpr std::uint64_t kTargetTransferredBytes = 64U * kMebibyte;
constexpr std::size_t kMinimumIterations = 100U;
constexpr std::size_t kMaximumIterations = 20'000U;

class UniqueFd {
 public:
  UniqueFd() = default;
  explicit UniqueFd(int descriptor) noexcept
      : descriptor_(descriptor) {}

  ~UniqueFd() { Reset(); }

  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;

  UniqueFd(UniqueFd&& other) noexcept
      : descriptor_(std::exchange(other.descriptor_, -1)) {}

  UniqueFd& operator=(UniqueFd&& other) noexcept {
    if (this != &other) {
      Reset(std::exchange(other.descriptor_, -1));
    }
    return *this;
  }

  [[nodiscard]] int get() const noexcept { return descriptor_; }

  void Reset(int replacement = -1) noexcept {
    if (descriptor_ >= 0) {
      static_cast<void>(::close(descriptor_));
    }
    descriptor_ = replacement;
  }

 private:
  int descriptor_{-1};
};

struct PipePair {
  UniqueFd read_end;
  UniqueFd write_end;
};

[[nodiscard]] Result<PipePair> CreatePipe(int flags) {
  std::array<int, 2> descriptors{};
  if (::pipe2(descriptors.data(), flags) != 0) {
    return Status(StatusCode::IoError, "pipe2 failed", errno);
  }

  PipePair result;
  result.read_end.Reset(descriptors[0]);
  result.write_end.Reset(descriptors[1]);
  return result;
}

[[nodiscard]] bool WriteExactBlocking(
    int descriptor, const void* data, std::size_t size) noexcept {
  const auto* bytes = static_cast<const std::byte*>(data);
  std::size_t offset = 0U;
  while (offset < size) {
    const ssize_t written =
        ::write(descriptor, bytes + offset, size - offset);
    if (written > 0) {
      offset += static_cast<std::size_t>(written);
      continue;
    }
    if (written < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

[[nodiscard]] bool ReadExactBlocking(
    int descriptor, void* data, std::size_t size) noexcept {
  auto* bytes = static_cast<std::byte*>(data);
  std::size_t offset = 0U;
  while (offset < size) {
    const ssize_t count =
        ::read(descriptor, bytes + offset, size - offset);
    if (count > 0) {
      offset += static_cast<std::size_t>(count);
      continue;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

[[nodiscard]] int PollTimeoutMilliseconds(
    const Deadline& deadline) noexcept {
  if (deadline.infinite()) {
    return -1;
  }
  const auto remaining =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          deadline.time_point() - Deadline::Clock::now());
  if (remaining <= std::chrono::nanoseconds::zero()) {
    return 0;
  }

  constexpr std::int64_t nanoseconds_per_millisecond = 1'000'000;
  const auto rounded =
      (remaining.count() + nanoseconds_per_millisecond - 1) /
      nanoseconds_per_millisecond;
  return static_cast<int>(
      std::min<std::int64_t>(rounded, INT_MAX));
}

[[nodiscard]] Status WaitForFd(
    int descriptor, short events, const Deadline& deadline) {
  for (;;) {
    pollfd candidate{};
    candidate.fd = descriptor;
    candidate.events = events;
    const int result =
        ::poll(&candidate, 1,
               PollTimeoutMilliseconds(deadline));
    if (result > 0) {
      if ((candidate.revents & events) != 0) {
        return Status::Ok();
      }
      if ((candidate.revents &
           (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        return Status(StatusCode::PeerDead,
                      "benchmark peer closed its descriptor");
      }
      continue;
    }
    if (result == 0) {
      return Status(StatusCode::Timeout);
    }
    if (errno == EINTR) {
      if (deadline.expired()) {
        return Status(StatusCode::Timeout);
      }
      continue;
    }
    return Status(StatusCode::IoError,
                  "benchmark poll failed", errno);
  }
}

void StoreSequence(std::span<std::byte> message,
                   std::uint64_t sequence) noexcept {
  std::memcpy(message.data(), &sequence, sizeof(sequence));
}

[[nodiscard]] std::uint64_t LoadSequence(
    std::span<const std::byte> message) noexcept {
  std::uint64_t sequence = 0U;
  std::memcpy(&sequence, message.data(), sizeof(sequence));
  return sequence;
}

[[nodiscard]] std::byte PayloadByte(
    std::uint64_t sequence, std::size_t index) noexcept {
  return static_cast<std::byte>(
      (sequence + static_cast<std::uint64_t>(index) * 131U + 17U) %
      251U);
}

void PreparePayload(std::span<std::byte> message,
                    std::uint64_t sequence,
                    AccessPattern access_pattern) noexcept {
  StoreSequence(message, sequence);
  if (access_pattern != AccessPattern::TouchMemory) {
    return;
  }
  for (std::size_t index = sizeof(sequence);
       index < message.size(); ++index) {
    message[index] = PayloadByte(sequence, index);
  }
}

[[nodiscard]] Result<std::uint64_t> ValidatePayload(
    std::span<const std::byte> message,
    AccessPattern access_pattern) {
  if (message.size() < sizeof(std::uint64_t)) {
    return Status(StatusCode::CorruptData,
                  "benchmark message is shorter than its sequence");
  }
  const auto sequence = LoadSequence(message);
  if (access_pattern == AccessPattern::TouchMemory) {
    for (std::size_t index = sizeof(sequence);
         index < message.size(); ++index) {
      if (message[index] != PayloadByte(sequence, index)) {
        return Status(StatusCode::CorruptData,
                      "benchmark payload validation failed");
      }
    }
  }
  return sequence;
}

class Endpoint {
 public:
  virtual ~Endpoint() = default;

  virtual Status SendMessage(
      std::size_t payload_bytes, std::uint64_t sequence,
      AccessPattern access_pattern, Deadline deadline) = 0;
  virtual Result<std::uint64_t> ReceiveMessage(
      std::size_t payload_bytes, AccessPattern access_pattern,
      Deadline deadline) = 0;
  virtual void Close() noexcept = 0;
};

class BufferedEndpoint : public Endpoint {
 public:
  Status SendMessage(
      std::size_t payload_bytes, std::uint64_t sequence,
      AccessPattern access_pattern, Deadline deadline) final {
    outbound_buffer_.resize(payload_bytes);
    PreparePayload(outbound_buffer_, sequence, access_pattern);
    return SendBytes(outbound_buffer_, deadline);
  }

  Result<std::uint64_t> ReceiveMessage(
      std::size_t payload_bytes, AccessPattern access_pattern,
      Deadline deadline) final {
    inbound_buffer_.resize(payload_bytes);
    auto received = ReceiveBytes(inbound_buffer_, deadline);
    if (!received) {
      return received.status();
    }
    if (received.value() != payload_bytes) {
      return Status(StatusCode::CorruptData,
                    "benchmark endpoint received the wrong size");
    }
    return ValidatePayload(inbound_buffer_, access_pattern);
  }

 protected:
  virtual Status SendBytes(
      std::span<const std::byte> message, Deadline deadline) = 0;
  virtual Result<std::size_t> ReceiveBytes(
      std::span<std::byte> destination, Deadline deadline) = 0;

 private:
  std::vector<std::byte> outbound_buffer_;
  std::vector<std::byte> inbound_buffer_;
};

class TransportEndpoint final : public BufferedEndpoint {
 public:
  TransportEndpoint(std::shared_ptr<Transport> outbound,
                    std::shared_ptr<Transport> inbound)
      : outbound_(std::move(outbound)),
        inbound_(std::move(inbound)) {}

  [[nodiscard]] static std::unique_ptr<Endpoint> Duplex(
      std::unique_ptr<Transport> transport) {
    auto shared =
        std::shared_ptr<Transport>(std::move(transport));
    return std::make_unique<TransportEndpoint>(shared, shared);
  }

  [[nodiscard]] static std::unique_ptr<Endpoint> Split(
      std::unique_ptr<Transport> outbound,
      std::unique_ptr<Transport> inbound) {
    return std::make_unique<TransportEndpoint>(
        std::shared_ptr<Transport>(std::move(outbound)),
        std::shared_ptr<Transport>(std::move(inbound)));
  }

  void Close() noexcept override {
    outbound_->Close();
    if (inbound_.get() != outbound_.get()) {
      inbound_->Close();
    }
  }

 private:
  Status SendBytes(std::span<const std::byte> message,
                   Deadline deadline) override {
    return outbound_->Send(
        message,
        SendOptions{BackpressurePolicy::Block, deadline});
  }

  Result<std::size_t> ReceiveBytes(
      std::span<std::byte> destination,
      Deadline deadline) override {
    return inbound_->Receive(destination, deadline);
  }

  std::shared_ptr<Transport> outbound_;
  std::shared_ptr<Transport> inbound_;
};

class ZeroCopyEndpoint final : public Endpoint {
 public:
  ZeroCopyEndpoint(
      std::unique_ptr<SharedMemoryTransport> outbound,
      std::unique_ptr<SharedMemoryTransport> inbound)
      : outbound_(std::move(outbound)),
        inbound_(std::move(inbound)) {}

  [[nodiscard]] static std::unique_ptr<Endpoint> Split(
      std::unique_ptr<SharedMemoryTransport> outbound,
      std::unique_ptr<SharedMemoryTransport> inbound) {
    return std::make_unique<ZeroCopyEndpoint>(
        std::move(outbound), std::move(inbound));
  }

  Status SendMessage(
      std::size_t payload_bytes, std::uint64_t sequence,
      AccessPattern access_pattern, Deadline deadline) override {
    auto loan_result = outbound_->Loan(
        payload_bytes,
        SendOptions{BackpressurePolicy::Block, deadline});
    if (!loan_result) {
      return loan_result.status();
    }
    auto loan = std::move(loan_result).take_value();
    PreparePayload(loan.Data(), sequence, access_pattern);
    return loan.Publish();
  }

  Result<std::uint64_t> ReceiveMessage(
      std::size_t payload_bytes, AccessPattern access_pattern,
      Deadline deadline) override {
    auto sample_result = inbound_->Take(deadline);
    if (!sample_result) {
      return sample_result.status();
    }
    auto sample = std::move(sample_result).take_value();
    if (sample.size() != payload_bytes) {
      static_cast<void>(sample.Release());
      return Status(StatusCode::CorruptData,
                    "zero-copy endpoint received the wrong size");
    }
    auto validation =
        ValidatePayload(sample.Data(), access_pattern);
    const auto release = sample.Release();
    if (!release) {
      return release;
    }
    return validation;
  }

  void Close() noexcept override {
    outbound_->Close();
    inbound_->Close();
  }

 private:
  std::unique_ptr<SharedMemoryTransport> outbound_;
  std::unique_ptr<SharedMemoryTransport> inbound_;
};

class PipeEndpoint final : public BufferedEndpoint {
 public:
  PipeEndpoint(UniqueFd write_end, UniqueFd read_end)
      : write_end_(std::move(write_end)),
        read_end_(std::move(read_end)) {}

  void Close() noexcept override {
    write_end_.Reset();
    read_end_.Reset();
  }

 private:
  Status SendBytes(std::span<const std::byte> message,
                   Deadline deadline) override {
    std::size_t offset = 0U;
    while (offset < message.size()) {
      const ssize_t written =
          ::write(write_end_.get(), message.data() + offset,
                  message.size() - offset);
      if (written > 0) {
        offset += static_cast<std::size_t>(written);
        continue;
      }
      if (written < 0 && errno == EINTR) {
        continue;
      }
      if (written < 0 &&
          (errno == EAGAIN || errno == EWOULDBLOCK)) {
        const auto wait =
            WaitForFd(write_end_.get(), POLLOUT, deadline);
        if (!wait) {
          return wait;
        }
        continue;
      }
      if (written < 0 && errno == EPIPE) {
        return Status(StatusCode::PeerDead,
                      "benchmark pipe reader closed", errno);
      }
      return Status(StatusCode::IoError,
                    "benchmark pipe write failed",
                    written < 0 ? errno : EIO);
    }
    return Status::Ok();
  }

  Result<std::size_t> ReceiveBytes(
      std::span<std::byte> destination,
      Deadline deadline) override {
    std::size_t offset = 0U;
    while (offset < destination.size()) {
      const ssize_t count =
          ::read(read_end_.get(), destination.data() + offset,
                 destination.size() - offset);
      if (count > 0) {
        offset += static_cast<std::size_t>(count);
        continue;
      }
      if (count == 0) {
        return Status(StatusCode::PeerDead,
                      "benchmark pipe writer closed");
      }
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        const auto wait =
            WaitForFd(read_end_.get(), POLLIN, deadline);
        if (!wait) {
          return wait;
        }
        continue;
      }
      return Status(StatusCode::IoError,
                    "benchmark pipe read failed", errno);
    }
    return destination.size();
  }

  UniqueFd write_end_;
  UniqueFd read_end_;
};

struct ResourceSample {
  rusage usage{};
};

struct ChildMetrics {
  double user_cpu_seconds{0.0};
  double system_cpu_seconds{0.0};
  std::int64_t voluntary_context_switches{0};
  std::int64_t involuntary_context_switches{0};
};

[[nodiscard]] double TimevalSeconds(const timeval& value) noexcept {
  return static_cast<double>(value.tv_sec) +
         static_cast<double>(value.tv_usec) / 1'000'000.0;
}

[[nodiscard]] Status CaptureSelfResource(
    ResourceSample* sample) {
  if (::getrusage(RUSAGE_SELF, &sample->usage) != 0) {
    return Status(StatusCode::IoError,
                  "getrusage failed", errno);
  }
  return Status::Ok();
}

[[nodiscard]] ChildMetrics ResourceDelta(
    const ResourceSample& before,
    const ResourceSample& after) noexcept {
  ChildMetrics result;
  result.user_cpu_seconds =
      TimevalSeconds(after.usage.ru_utime) -
      TimevalSeconds(before.usage.ru_utime);
  result.system_cpu_seconds =
      TimevalSeconds(after.usage.ru_stime) -
      TimevalSeconds(before.usage.ru_stime);
  result.voluntary_context_switches =
      static_cast<std::int64_t>(after.usage.ru_nvcsw) -
      static_cast<std::int64_t>(before.usage.ru_nvcsw);
  result.involuntary_context_switches =
      static_cast<std::int64_t>(after.usage.ru_nivcsw) -
      static_cast<std::int64_t>(before.usage.ru_nivcsw);
  return result;
}

[[nodiscard]] Status EchoIterations(
    Endpoint& endpoint, std::size_t payload_bytes,
    std::size_t iterations, AccessPattern access_pattern,
    std::chrono::milliseconds operation_timeout) {
  for (std::size_t iteration = 0U;
       iteration < iterations; ++iteration) {
    const auto deadline = Deadline::After(operation_timeout);
    auto received = endpoint.ReceiveMessage(
        payload_bytes, access_pattern, deadline);
    if (!received) {
      return received.status();
    }
    const auto sent = endpoint.SendMessage(
        payload_bytes, received.value(), access_pattern, deadline);
    if (!sent) {
      return sent;
    }
  }
  return Status::Ok();
}

[[nodiscard]] int RunChildBenchmark(
    Endpoint& endpoint, int metrics_descriptor,
    const CaseConfig& config) {
  const auto warmup =
      EchoIterations(endpoint, config.payload_bytes,
                     config.warmup_iterations,
                     config.access_pattern,
                     config.operation_timeout);
  if (!warmup) {
    return 20;
  }

  ResourceSample before;
  const auto before_status = CaptureSelfResource(&before);
  if (!before_status) {
    return 21;
  }

  const auto measured =
      EchoIterations(endpoint, config.payload_bytes,
                     config.iterations,
                     config.access_pattern,
                     config.operation_timeout);
  if (!measured) {
    return 22;
  }

  ResourceSample after;
  const auto after_status = CaptureSelfResource(&after);
  if (!after_status) {
    return 23;
  }

  const auto metrics = ResourceDelta(before, after);
  if (!WriteExactBlocking(
          metrics_descriptor, &metrics, sizeof(metrics))) {
    return 24;
  }
  return 0;
}

[[nodiscard]] Status RoundTrip(
    Endpoint& endpoint, std::size_t payload_bytes,
    AccessPattern access_pattern, std::uint64_t sequence,
    std::chrono::milliseconds operation_timeout,
    std::chrono::nanoseconds* elapsed) {
  const auto deadline = Deadline::After(operation_timeout);
  const auto started =
      elapsed == nullptr ? Clock::time_point{} : Clock::now();

  const auto sent = endpoint.SendMessage(
      payload_bytes, sequence, access_pattern, deadline);
  if (!sent) {
    return sent;
  }
  auto received = endpoint.ReceiveMessage(
      payload_bytes, access_pattern, deadline);
  if (!received) {
    return received.status();
  }
  if (received.value() != sequence) {
    return Status(StatusCode::CorruptData,
                  "benchmark echo validation failed");
  }

  if (elapsed != nullptr) {
    *elapsed =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - started);
  }
  return Status::Ok();
}

void WaitForChild(pid_t child, rusage* usage = nullptr) noexcept {
  int status = 0;
  for (;;) {
    const pid_t result =
        usage == nullptr
            ? ::waitpid(child, &status, 0)
            : ::wait4(child, &status, 0, usage);
    if (result == child) {
      return;
    }
    if (result < 0 && errno == EINTR) {
      continue;
    }
    return;
  }
}

[[nodiscard]] std::uint64_t NearestRank(
    const std::vector<std::uint64_t>& sorted,
    std::size_t numerator, std::size_t denominator) {
  const std::size_t rank =
      (numerator * sorted.size() + denominator - 1U) /
      denominator;
  return sorted[std::max<std::size_t>(1U, rank) - 1U];
}

[[nodiscard]] Result<BenchmarkResult> RunParentBenchmark(
    Endpoint& endpoint, pid_t child,
    UniqueFd metrics_reader, const CaseConfig& config) {
  auto fail = [&](Status status) -> Result<BenchmarkResult> {
    endpoint.Close();
    WaitForChild(child);
    return status;
  };

  for (std::size_t iteration = 0U;
       iteration < config.warmup_iterations; ++iteration) {
    const auto status =
        RoundTrip(endpoint, config.payload_bytes,
                  config.access_pattern,
                  static_cast<std::uint64_t>(iteration + 1U),
                  config.operation_timeout, nullptr);
    if (!status) {
      return fail(status);
    }
  }

  ResourceSample parent_before;
  const auto before_status =
      CaptureSelfResource(&parent_before);
  if (!before_status) {
    return fail(before_status);
  }

  std::vector<std::uint64_t> latencies_ns;
  latencies_ns.reserve(config.iterations);
  const auto wall_started = Clock::now();
  for (std::size_t iteration = 0U;
       iteration < config.iterations; ++iteration) {
    std::chrono::nanoseconds elapsed{};
    const std::uint64_t sequence =
        static_cast<std::uint64_t>(
            config.warmup_iterations + iteration + 1U);
    const auto status =
        RoundTrip(endpoint, config.payload_bytes,
                  config.access_pattern, sequence,
                  config.operation_timeout, &elapsed);
    if (!status) {
      return fail(status);
    }
    latencies_ns.push_back(
        static_cast<std::uint64_t>(elapsed.count()));
  }
  const auto wall_finished = Clock::now();

  ResourceSample parent_after;
  const auto after_status =
      CaptureSelfResource(&parent_after);
  if (!after_status) {
    return fail(after_status);
  }

  ChildMetrics child_metrics;
  if (!ReadExactBlocking(metrics_reader.get(), &child_metrics,
                         sizeof(child_metrics))) {
    return fail(Status(
        StatusCode::IoError,
        "benchmark child did not return resource metrics"));
  }
  metrics_reader.Reset();

  int child_status = 0;
  rusage child_usage{};
  pid_t waited = -1;
  do {
    waited = ::wait4(child, &child_status, 0, &child_usage);
  } while (waited < 0 && errno == EINTR);
  if (waited != child || !WIFEXITED(child_status) ||
      WEXITSTATUS(child_status) != 0) {
    return Status(StatusCode::IoError,
                  "benchmark child exited unsuccessfully");
  }

  std::sort(latencies_ns.begin(), latencies_ns.end());
  const auto wall_duration =
      std::chrono::duration<double>(
          wall_finished - wall_started);
  const double wall_seconds = wall_duration.count();
  if (wall_seconds <= 0.0 || latencies_ns.empty()) {
    return Status(StatusCode::IoError,
                  "benchmark measured an empty duration");
  }

  const auto parent_delta =
      ResourceDelta(parent_before, parent_after);
  const double user_seconds =
      parent_delta.user_cpu_seconds +
      child_metrics.user_cpu_seconds;
  const double system_seconds =
      parent_delta.system_cpu_seconds +
      child_metrics.system_cpu_seconds;
  const double cpu_seconds = user_seconds + system_seconds;
  const double logical_messages =
      static_cast<double>(config.iterations) * 2.0;
  const double payload_bytes =
      logical_messages *
      static_cast<double>(config.payload_bytes);

  BenchmarkResult result;
  result.transport = TransportName(config.transport);
  if (config.transport == TransportKind::FastIpcCopy) {
    result.transport_mode = "spsc_ring_copy_futex";
  } else if (
      config.transport == TransportKind::FastIpcZeroCopy) {
    result.transport_mode = "spsc_ring_loan_futex";
  } else if (
      config.transport == TransportKind::UnixDomainSocket) {
    result.transport_mode =
        config.payload_bytes <= 64U * 1024U
            ? "seqpacket_inline"
            : "seqpacket_sealed_memfd";
  } else {
    result.transport_mode = "dual_nonblocking_pipe";
  }
  result.access_pattern = AccessPatternName(config.access_pattern);
  result.payload_bytes = config.payload_bytes;
  result.iterations = config.iterations;
  result.warmup_iterations = config.warmup_iterations;
  result.wall_time_ms = wall_seconds * 1000.0;
  result.round_trips_per_second =
      static_cast<double>(config.iterations) / wall_seconds;
  result.messages_per_second =
      logical_messages / wall_seconds;
  result.payload_mib_per_second =
      payload_bytes /
      (static_cast<double>(kMebibyte) * wall_seconds);
  result.p50_us =
      static_cast<double>(NearestRank(latencies_ns, 50U, 100U)) /
      1000.0;
  result.p95_us =
      static_cast<double>(NearestRank(latencies_ns, 95U, 100U)) /
      1000.0;
  result.p99_us =
      static_cast<double>(NearestRank(latencies_ns, 99U, 100U)) /
      1000.0;
  result.p99_9_us =
      static_cast<double>(NearestRank(latencies_ns, 999U, 1000U)) /
      1000.0;
  result.user_cpu_ms = user_seconds * 1000.0;
  result.system_cpu_ms = system_seconds * 1000.0;
  result.cpu_time_ms = cpu_seconds * 1000.0;
  result.cpu_utilization_percent =
      cpu_seconds / wall_seconds * 100.0;
  result.voluntary_context_switches =
      parent_delta.voluntary_context_switches +
      child_metrics.voluntary_context_switches;
  result.involuntary_context_switches =
      parent_delta.involuntary_context_switches +
      child_metrics.involuntary_context_switches;
  result.parent_peak_rss_kib =
      static_cast<std::int64_t>(
          parent_after.usage.ru_maxrss);
  result.child_peak_rss_kib =
      static_cast<std::int64_t>(child_usage.ru_maxrss);
  return result;
}

[[nodiscard]] ChannelConfig SharedConfig(
    std::string name, std::size_t payload_bytes) {
  ChannelConfig config;
  config.name = std::move(name);
  config.slot_count = 2U;
  config.max_message_size =
      static_cast<std::uint32_t>(payload_bytes);
  config.permissions = 0600;
  config.unlink_on_owner_close = true;
  config.peer_timeout = 10s;
  return config;
}

[[nodiscard]] std::unique_ptr<Endpoint> MakeSharedMemoryEndpoint(
    TransportKind transport,
    std::unique_ptr<SharedMemoryTransport> outbound,
    std::unique_ptr<SharedMemoryTransport> inbound) {
  if (transport == TransportKind::FastIpcZeroCopy) {
    return ZeroCopyEndpoint::Split(
        std::move(outbound), std::move(inbound));
  }
  return TransportEndpoint::Split(
      std::unique_ptr<Transport>(std::move(outbound)),
      std::unique_ptr<Transport>(std::move(inbound)));
}

[[nodiscard]] Result<BenchmarkResult> RunSharedMemoryCase(
    const CaseConfig& config) {
  auto parent_to_child_result = CreatePipe(O_CLOEXEC);
  if (!parent_to_child_result) {
    return parent_to_child_result.status();
  }
  auto child_to_parent_result = CreatePipe(O_CLOEXEC);
  if (!child_to_parent_result) {
    return child_to_parent_result.status();
  }
  auto metrics_result = CreatePipe(O_CLOEXEC);
  if (!metrics_result) {
    return metrics_result.status();
  }
  auto parent_to_child =
      std::move(parent_to_child_result).take_value();
  auto child_to_parent =
      std::move(child_to_parent_result).take_value();
  auto metrics = std::move(metrics_result).take_value();

  const std::string suffix =
      std::to_string(::getpid()) + "_" +
      std::to_string(config.case_id);
  const auto request_config =
      SharedConfig("fastipc_bench_req_" + suffix,
                   config.payload_bytes);
  const auto reply_config =
      SharedConfig("fastipc_bench_reply_" + suffix,
                   config.payload_bytes);

  const pid_t child = ::fork();
  if (child < 0) {
    return Status(StatusCode::IoError,
                  "fork failed for shared-memory benchmark",
                  errno);
  }

  if (child == 0) {
    parent_to_child.write_end.Reset();
    child_to_parent.read_end.Reset();
    metrics.read_end.Reset();

    const auto child_main = [&]() -> int {
      std::uint8_t start = 0U;
      if (!ReadExactBlocking(
              parent_to_child.read_end.get(), &start,
              sizeof(start)) ||
          start != 1U) {
        return 30;
      }
      parent_to_child.read_end.Reset();

      auto request =
          SharedMemoryTransport::OpenConsumer(request_config);
      if (!request) {
        const std::uint8_t failed = 0U;
        static_cast<void>(WriteExactBlocking(
            child_to_parent.write_end.get(), &failed,
            sizeof(failed)));
        return 31;
      }
      auto reply =
          SharedMemoryTransport::CreateProducer(reply_config);
      if (!reply) {
        const std::uint8_t failed = 0U;
        static_cast<void>(WriteExactBlocking(
            child_to_parent.write_end.get(), &failed,
            sizeof(failed)));
        return 32;
      }

      auto endpoint = MakeSharedMemoryEndpoint(
          config.transport,
          std::move(reply).take_value(),
          std::move(request).take_value());
      const std::uint8_t ready = 1U;
      if (!WriteExactBlocking(
              child_to_parent.write_end.get(), &ready,
              sizeof(ready))) {
        return 33;
      }
      child_to_parent.write_end.Reset();
      const int result =
          RunChildBenchmark(*endpoint, metrics.write_end.get(),
                            config);
      endpoint->Close();
      return result;
    };

    const int result = child_main();
    std::_Exit(result);
  }

  parent_to_child.read_end.Reset();
  child_to_parent.write_end.Reset();
  metrics.write_end.Reset();

  auto request =
      SharedMemoryTransport::CreateProducer(request_config);
  if (!request) {
    const std::uint8_t stop = 0U;
    static_cast<void>(WriteExactBlocking(
        parent_to_child.write_end.get(), &stop,
        sizeof(stop)));
    parent_to_child.write_end.Reset();
    WaitForChild(child);
    return request.status();
  }

  const std::uint8_t start = 1U;
  if (!WriteExactBlocking(parent_to_child.write_end.get(),
                          &start, sizeof(start))) {
    request.value()->Close();
    WaitForChild(child);
    return Status(StatusCode::IoError,
                  "failed to start shared-memory child");
  }
  parent_to_child.write_end.Reset();

  std::uint8_t ready = 0U;
  if (!ReadExactBlocking(child_to_parent.read_end.get(),
                         &ready, sizeof(ready)) ||
      ready != 1U) {
    request.value()->Close();
    WaitForChild(child);
    return Status(
        StatusCode::IoError,
        "shared-memory child failed during setup");
  }
  child_to_parent.read_end.Reset();

  auto reply =
      SharedMemoryTransport::OpenConsumer(reply_config);
  if (!reply) {
    request.value()->Close();
    WaitForChild(child);
    return reply.status();
  }

  auto endpoint = MakeSharedMemoryEndpoint(
      config.transport,
      std::move(request).take_value(),
      std::move(reply).take_value());
  auto result = RunParentBenchmark(
      *endpoint, child, std::move(metrics.read_end), config);
  endpoint->Close();
  return result;
}

[[nodiscard]] Result<BenchmarkResult> RunUnixSocketCase(
    const CaseConfig& config) {
  auto parent_to_child_result = CreatePipe(O_CLOEXEC);
  if (!parent_to_child_result) {
    return parent_to_child_result.status();
  }
  auto child_to_parent_result = CreatePipe(O_CLOEXEC);
  if (!child_to_parent_result) {
    return child_to_parent_result.status();
  }
  auto metrics_result = CreatePipe(O_CLOEXEC);
  if (!metrics_result) {
    return metrics_result.status();
  }
  auto parent_to_child =
      std::move(parent_to_child_result).take_value();
  auto child_to_parent =
      std::move(child_to_parent_result).take_value();
  auto metrics = std::move(metrics_result).take_value();

  UnixDomainSocketConfig socket_config;
  socket_config.path =
      "/tmp/fastipc_bench_" + std::to_string(::getpid()) +
      "_" + std::to_string(config.case_id) + ".sock";
  socket_config.max_message_size =
      static_cast<std::uint32_t>(config.payload_bytes);
  socket_config.permissions = 0600;

  const pid_t child = ::fork();
  if (child < 0) {
    return Status(StatusCode::IoError,
                  "fork failed for Unix-socket benchmark",
                  errno);
  }

  if (child == 0) {
    parent_to_child.write_end.Reset();
    child_to_parent.read_end.Reset();
    metrics.read_end.Reset();

    const auto child_main = [&]() -> int {
      std::uint8_t start = 0U;
      if (!ReadExactBlocking(
              parent_to_child.read_end.get(), &start,
              sizeof(start)) ||
          start != 1U) {
        return 40;
      }
      parent_to_child.read_end.Reset();

      auto connection = UnixDomainSocketTransport::Connect(
          socket_config,
          Deadline::After(config.operation_timeout));
      if (!connection) {
        const std::uint8_t failed = 0U;
        static_cast<void>(WriteExactBlocking(
            child_to_parent.write_end.get(), &failed,
            sizeof(failed)));
        return 41;
      }

      auto endpoint = TransportEndpoint::Duplex(
          std::unique_ptr<Transport>(
              std::move(connection).take_value()));
      const std::uint8_t ready = 1U;
      if (!WriteExactBlocking(
              child_to_parent.write_end.get(), &ready,
              sizeof(ready))) {
        return 42;
      }
      child_to_parent.write_end.Reset();
      const int result =
          RunChildBenchmark(*endpoint, metrics.write_end.get(),
                            config);
      endpoint->Close();
      return result;
    };

    const int result = child_main();
    std::_Exit(result);
  }

  parent_to_child.read_end.Reset();
  child_to_parent.write_end.Reset();
  metrics.write_end.Reset();

  auto listener = UnixDomainSocketListener::Bind(socket_config);
  if (!listener) {
    const std::uint8_t stop = 0U;
    static_cast<void>(WriteExactBlocking(
        parent_to_child.write_end.get(), &stop,
        sizeof(stop)));
    parent_to_child.write_end.Reset();
    WaitForChild(child);
    return listener.status();
  }

  const std::uint8_t start = 1U;
  if (!WriteExactBlocking(parent_to_child.write_end.get(),
                          &start, sizeof(start))) {
    listener.value()->Close();
    WaitForChild(child);
    return Status(StatusCode::IoError,
                  "failed to start Unix-socket child");
  }
  parent_to_child.write_end.Reset();

  std::uint8_t ready = 0U;
  if (!ReadExactBlocking(child_to_parent.read_end.get(),
                         &ready, sizeof(ready)) ||
      ready != 1U) {
    listener.value()->Close();
    WaitForChild(child);
    return Status(
        StatusCode::IoError,
        "Unix-socket child failed during setup");
  }
  child_to_parent.read_end.Reset();

  auto accepted =
      listener.value()->Accept(
          Deadline::After(config.operation_timeout));
  if (!accepted) {
    listener.value()->Close();
    static_cast<void>(::kill(child, SIGTERM));
    WaitForChild(child);
    return accepted.status();
  }

  auto endpoint = TransportEndpoint::Duplex(
      std::unique_ptr<Transport>(
          std::move(accepted).take_value()));
  auto result = RunParentBenchmark(
      *endpoint, child, std::move(metrics.read_end), config);
  endpoint->Close();
  listener.value()->Close();
  return result;
}

[[nodiscard]] Result<BenchmarkResult> RunPipeCase(
    const CaseConfig& config) {
  auto request_result =
      CreatePipe(O_CLOEXEC | O_NONBLOCK);
  if (!request_result) {
    return request_result.status();
  }
  auto reply_result =
      CreatePipe(O_CLOEXEC | O_NONBLOCK);
  if (!reply_result) {
    return reply_result.status();
  }
  auto metrics_result = CreatePipe(O_CLOEXEC);
  if (!metrics_result) {
    return metrics_result.status();
  }
  auto request = std::move(request_result).take_value();
  auto reply = std::move(reply_result).take_value();
  auto metrics = std::move(metrics_result).take_value();

  const pid_t child = ::fork();
  if (child < 0) {
    return Status(StatusCode::IoError,
                  "fork failed for pipe benchmark", errno);
  }

  if (child == 0) {
    request.write_end.Reset();
    reply.read_end.Reset();
    metrics.read_end.Reset();

    const auto child_main = [&]() -> int {
      auto endpoint = std::make_unique<PipeEndpoint>(
          std::move(reply.write_end),
          std::move(request.read_end));
      const int result =
          RunChildBenchmark(*endpoint, metrics.write_end.get(),
                            config);
      endpoint->Close();
      return result;
    };

    const int result = child_main();
    std::_Exit(result);
  }

  request.read_end.Reset();
  reply.write_end.Reset();
  metrics.write_end.Reset();

  auto endpoint = std::make_unique<PipeEndpoint>(
      std::move(request.write_end),
      std::move(reply.read_end));
  auto result = RunParentBenchmark(
      *endpoint, child, std::move(metrics.read_end), config);
  endpoint->Close();
  return result;
}

[[nodiscard]] std::string Trim(std::string text) {
  const auto first = text.find_first_not_of(" \t");
  if (first == std::string::npos) {
    return {};
  }
  const auto last = text.find_last_not_of(" \t\r\n");
  return text.substr(first, last - first + 1U);
}

[[nodiscard]] std::string ReadCpuModel() {
  std::ifstream cpu_info("/proc/cpuinfo");
  std::string line;
  while (std::getline(cpu_info, line)) {
    if (line.starts_with("model name")) {
      const auto separator = line.find(':');
      if (separator != std::string::npos) {
        return Trim(line.substr(separator + 1U));
      }
    }
  }
  return "unknown";
}

[[nodiscard]] std::int64_t ReadMemoryTotalKib() {
  std::ifstream memory_info("/proc/meminfo");
  std::string key;
  std::int64_t value = 0;
  std::string unit;
  while (memory_info >> key >> value >> unit) {
    if (key == "MemTotal:") {
      return value;
    }
  }
  return 0;
}

[[nodiscard]] std::string TimestampUtc() {
  const std::time_t now = std::time(nullptr);
  std::tm utc{};
  if (::gmtime_r(&now, &utc) == nullptr) {
    return "unknown";
  }
  std::array<char, 32> buffer{};
  if (std::strftime(buffer.data(), buffer.size(),
                    "%Y-%m-%dT%H:%M:%SZ", &utc) == 0U) {
    return "unknown";
  }
  return buffer.data();
}

[[nodiscard]] std::string JsonEscape(
    std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (const char character : value) {
    if (character == '"' || character == '\\') {
      result.push_back('\\');
      result.push_back(character);
    } else if (character == '\n') {
      result += "\\n";
    } else if (character == '\r') {
      result += "\\r";
    } else if (character == '\t') {
      result += "\\t";
    } else {
      result.push_back(character);
    }
  }
  return result;
}

}  // namespace

const char* TransportName(TransportKind transport) noexcept {
  switch (transport) {
    case TransportKind::FastIpcCopy:
      return "fastipc_copy";
    case TransportKind::FastIpcZeroCopy:
      return "fastipc_zero_copy";
    case TransportKind::UnixDomainSocket:
      return "unix_domain_socket";
    case TransportKind::Pipe:
      return "pipe";
  }
  return "unknown";
}

const char* AccessPatternName(
    AccessPattern access_pattern) noexcept {
  switch (access_pattern) {
    case AccessPattern::TransportOnly:
      return "transport_only";
    case AccessPattern::TouchMemory:
      return "touch_memory";
  }
  return "unknown";
}

std::optional<TransportKind> ParseTransport(
    std::string_view name) noexcept {
  if (name == "fastipc_copy" || name == "shared_memory" ||
      name == "shm") {
    return TransportKind::FastIpcCopy;
  }
  if (name == "fastipc_zero_copy" || name == "zero_copy" ||
      name == "shm_zero_copy") {
    return TransportKind::FastIpcZeroCopy;
  }
  if (name == "unix_domain_socket" || name == "uds") {
    return TransportKind::UnixDomainSocket;
  }
  if (name == "pipe") {
    return TransportKind::Pipe;
  }
  return std::nullopt;
}

std::optional<AccessPattern> ParseAccessPattern(
    std::string_view name) noexcept {
  if (name == "transport_only") {
    return AccessPattern::TransportOnly;
  }
  if (name == "touch_memory") {
    return AccessPattern::TouchMemory;
  }
  return std::nullopt;
}

std::vector<std::size_t> RequiredPayloadSizes() {
  return {64U, 256U, 1024U, 4U * 1024U,
          64U * 1024U, 1024U * 1024U};
}

std::size_t DefaultIterations(std::size_t payload_bytes) {
  if (payload_bytes == 0U) {
    return kMinimumIterations;
  }
  const std::uint64_t round_trip_bytes =
      2U * static_cast<std::uint64_t>(payload_bytes);
  const std::uint64_t target =
      kTargetTransferredBytes /
      std::max<std::uint64_t>(1U, round_trip_bytes);
  return static_cast<std::size_t>(
      std::clamp<std::uint64_t>(
          target, kMinimumIterations,
          kMaximumIterations));
}

std::size_t DefaultWarmupIterations(
    std::size_t iterations) {
  return std::clamp<std::size_t>(
      iterations / 20U, 5U, 100U);
}

Result<BenchmarkResult> RunCase(const CaseConfig& config) {
  if (config.payload_bytes < sizeof(std::uint64_t) ||
      config.payload_bytes >
          16U * static_cast<std::size_t>(kMebibyte) ||
      config.iterations == 0U) {
    return Status(
        StatusCode::InvalidArgument,
        "benchmark payload must be 8 B..16 MiB and iterations positive");
  }
  if (::signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
    return Status(StatusCode::IoError,
                  "failed to ignore SIGPIPE", errno);
  }

  switch (config.transport) {
    case TransportKind::FastIpcCopy:
    case TransportKind::FastIpcZeroCopy:
      return RunSharedMemoryCase(config);
    case TransportKind::UnixDomainSocket:
      return RunUnixSocketCase(config);
    case TransportKind::Pipe:
      return RunPipeCase(config);
  }
  return Status(StatusCode::Unsupported,
                "unknown benchmark transport");
}

Environment CaptureEnvironment() {
  Environment environment;
  environment.timestamp_utc = TimestampUtc();

  std::array<char, 256> hostname{};
  if (::gethostname(hostname.data(), hostname.size()) == 0) {
    hostname.back() = '\0';
    environment.hostname = hostname.data();
  } else {
    environment.hostname = "unknown";
  }

  utsname system{};
  if (::uname(&system) == 0) {
    environment.kernel_release = system.release;
    environment.machine = system.machine;
  } else {
    environment.kernel_release = "unknown";
    environment.machine = "unknown";
  }

  environment.cpu_model = ReadCpuModel();
  environment.compiler = FASTIPC_COMPILER;
  environment.build_type = FASTIPC_BUILD_TYPE;
  environment.source_revision = FASTIPC_SOURCE_REVISION;
  environment.logical_cpus =
      static_cast<std::int64_t>(
          ::sysconf(_SC_NPROCESSORS_ONLN));
  environment.page_size_bytes =
      static_cast<std::int64_t>(::sysconf(_SC_PAGESIZE));
  environment.memory_total_kib = ReadMemoryTotalKib();
  return environment;
}

std::string ToJson(const Environment& environment) {
  std::ostringstream output;
  output << "{\"type\":\"environment\""
         << ",\"timestamp_utc\":\""
         << JsonEscape(environment.timestamp_utc) << '"'
         << ",\"hostname\":\""
         << JsonEscape(environment.hostname) << '"'
         << ",\"kernel_release\":\""
         << JsonEscape(environment.kernel_release) << '"'
         << ",\"machine\":\""
         << JsonEscape(environment.machine) << '"'
         << ",\"cpu_model\":\""
         << JsonEscape(environment.cpu_model) << '"'
         << ",\"logical_cpus\":" << environment.logical_cpus
         << ",\"page_size_bytes\":"
         << environment.page_size_bytes
         << ",\"memory_total_kib\":"
         << environment.memory_total_kib
         << ",\"compiler\":\""
         << JsonEscape(environment.compiler) << '"'
         << ",\"build_type\":\""
         << JsonEscape(environment.build_type) << '"'
         << ",\"source_revision\":\""
         << JsonEscape(environment.source_revision) << '"'
         << ",\"clock\":\"std::chrono::steady_clock\""
         << ",\"methodology\":\"cross-process single-outstanding "
            "ping-pong; latency is RTT\""
         << ",\"quantile_method\":\"nearest-rank\""
         << '}';
  return output.str();
}

std::string ToJson(const BenchmarkResult& result) {
  std::ostringstream output;
  output << std::fixed << std::setprecision(3)
         << "{\"type\":\"result\""
         << ",\"transport\":\""
         << JsonEscape(result.transport) << '"'
         << ",\"transport_mode\":\""
         << JsonEscape(result.transport_mode) << '"'
         << ",\"access_pattern\":\""
         << JsonEscape(result.access_pattern) << '"'
         << ",\"payload_bytes\":" << result.payload_bytes
         << ",\"iterations\":" << result.iterations
         << ",\"warmup_iterations\":"
         << result.warmup_iterations
         << ",\"wall_time_ms\":" << result.wall_time_ms
         << ",\"round_trips_per_second\":"
         << result.round_trips_per_second
         << ",\"messages_per_second\":"
         << result.messages_per_second
         << ",\"payload_mib_per_second\":"
         << result.payload_mib_per_second
         << ",\"p50_us\":" << result.p50_us
         << ",\"p95_us\":" << result.p95_us
         << ",\"p99_us\":" << result.p99_us
         << ",\"p99_9_us\":" << result.p99_9_us
         << ",\"user_cpu_ms\":" << result.user_cpu_ms
         << ",\"system_cpu_ms\":" << result.system_cpu_ms
         << ",\"cpu_time_ms\":" << result.cpu_time_ms
         << ",\"cpu_utilization_percent\":"
         << result.cpu_utilization_percent
         << ",\"voluntary_context_switches\":"
         << result.voluntary_context_switches
         << ",\"involuntary_context_switches\":"
         << result.involuntary_context_switches
         << ",\"parent_peak_rss_kib\":"
         << result.parent_peak_rss_kib
         << ",\"child_peak_rss_kib\":"
         << result.child_peak_rss_kib
         << '}';
  return output.str();
}

}  // namespace fastipc::benchmark
