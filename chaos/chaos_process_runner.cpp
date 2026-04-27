#include "chaos/chaos_runner.hpp"

#include <fastipc/shared_memory_transport.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <ostream>
#include <poll.h>
#include <signal.h>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>
#include <unistd.h>

#ifndef FASTIPC_CHAOS_BUILD_TYPE
#define FASTIPC_CHAOS_BUILD_TYPE "unknown"
#endif

#ifndef FASTIPC_CHAOS_COMPILER
#define FASTIPC_CHAOS_COMPILER "unknown"
#endif

#ifndef FASTIPC_CHAOS_SOURCE_REVISION
#define FASTIPC_CHAOS_SOURCE_REVISION "unknown"
#endif

namespace fastipc::chaos {
namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr std::size_t kSequenceBytes = sizeof(std::uint64_t);
constexpr std::size_t kChecksumBytes = sizeof(std::uint64_t);
constexpr std::size_t kMinimumPayloadBytes =
    kSequenceBytes + kChecksumBytes;
constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

enum class EndpointRole : std::uint32_t {
  Producer = 1U,
  Consumer = 2U,
};

enum class CommandKind : std::uint32_t {
  Stop = 1U,
  Publish = 2U,
  LoanAndHold = 3U,
  TakeAndRelease = 4U,
  TakeAndHold = 5U,
  TakeDelayRelease = 6U,
  Stats = 7U,
};

enum class ReportEvent : std::uint32_t {
  Ready = 1U,
  Completed = 2U,
  LoanHeld = 3U,
  SampleHeld = 4U,
};

struct WireCommand {
  std::uint32_t kind{0U};
  std::uint32_t policy{0U};
  std::uint32_t timeout_ms{0U};
  std::uint32_t delay_ms{0U};
  std::uint64_t sequence{0U};
  std::uint64_t payload_size{0U};
};

struct WireReport {
  std::uint32_t event{0U};
  std::uint32_t status_code{0U};
  std::uint32_t checksum_match{1U};
  std::uint32_t reserved{0U};
  std::uint64_t sequence{0U};
  std::uint64_t elapsed_ns{0U};
  TransportStats stats{};
};

static_assert(std::is_trivially_copyable_v<WireCommand>);
static_assert(std::is_trivially_copyable_v<WireReport>);

[[nodiscard]] std::uint64_t DurationNanoseconds(
    Clock::time_point start, Clock::time_point finish) noexcept {
  const auto count =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          finish - start)
          .count();
  return count <= 0 ? 0U : static_cast<std::uint64_t>(count);
}

void StoreLittleEndian64(
    std::span<std::byte> destination, std::uint64_t value) noexcept {
  for (std::size_t index = 0U; index < sizeof(value); ++index) {
    destination[index] =
        static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
  }
}

[[nodiscard]] std::uint64_t LoadLittleEndian64(
    std::span<const std::byte> source) noexcept {
  std::uint64_t value = 0U;
  for (std::size_t index = 0U; index < sizeof(value); ++index) {
    value |= static_cast<std::uint64_t>(
                 std::to_integer<std::uint8_t>(source[index]))
             << (index * 8U);
  }
  return value;
}

[[nodiscard]] std::uint64_t Checksum(
    std::span<const std::byte> bytes) noexcept {
  std::uint64_t hash = kFnvOffset;
  for (const auto value : bytes) {
    hash ^= std::to_integer<std::uint8_t>(value);
    hash *= kFnvPrime;
  }
  return hash;
}

void FillPayload(
    std::span<std::byte> payload, std::uint64_t sequence) noexcept {
  StoreLittleEndian64(payload.first(kSequenceBytes), sequence);
  for (std::size_t index = kSequenceBytes;
       index < payload.size() - kChecksumBytes; ++index) {
    const auto value =
        static_cast<std::uint8_t>(
            (sequence * 131U + index * 17U) & 0xFFU);
    payload[index] = static_cast<std::byte>(value);
  }
  const auto checksum =
      Checksum(std::span<const std::byte>(
          payload.data(), payload.size() - kChecksumBytes));
  StoreLittleEndian64(
      payload.last(kChecksumBytes), checksum);
}

struct PayloadValidation {
  std::uint64_t sequence{0U};
  bool checksum_match{false};
};

[[nodiscard]] PayloadValidation ValidatePayload(
    std::span<const std::byte> payload) noexcept {
  if (payload.size() < kMinimumPayloadBytes) {
    return {};
  }
  const auto sequence =
      LoadLittleEndian64(payload.first(kSequenceBytes));
  const auto expected =
      LoadLittleEndian64(payload.last(kChecksumBytes));
  const auto observed =
      Checksum(payload.first(payload.size() - kChecksumBytes));
  return PayloadValidation{sequence, expected == observed};
}

[[nodiscard]] bool WriteFull(
    int descriptor, const void* source, std::size_t size) noexcept {
  const auto* bytes = static_cast<const std::byte*>(source);
  std::size_t written = 0U;
  while (written < size) {
    const auto result =
        ::write(descriptor, bytes + written, size - written);
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (result == 0) {
      return false;
    }
    written += static_cast<std::size_t>(result);
  }
  return true;
}

[[nodiscard]] bool ReadFull(
    int descriptor, void* destination, std::size_t size) noexcept {
  auto* bytes = static_cast<std::byte*>(destination);
  std::size_t read_bytes = 0U;
  while (read_bytes < size) {
    const auto result =
        ::read(descriptor, bytes + read_bytes, size - read_bytes);
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (result == 0) {
      return false;
    }
    read_bytes += static_cast<std::size_t>(result);
  }
  return true;
}

[[nodiscard]] Status ReadFullFor(
    int descriptor, void* destination, std::size_t size,
    std::chrono::milliseconds timeout) noexcept {
  const auto deadline = Clock::now() + timeout;
  auto* bytes = static_cast<std::byte*>(destination);
  std::size_t read_bytes = 0U;
  while (read_bytes < size) {
    const auto now = Clock::now();
    if (now >= deadline) {
      return Status(StatusCode::Timeout, "endpoint report timed out");
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - now);
    const auto remaining_count =
        std::max<std::int64_t>(1, remaining.count());
    const auto timeout_ms =
        remaining_count > std::numeric_limits<int>::max()
            ? std::numeric_limits<int>::max()
            : static_cast<int>(remaining_count);
    pollfd descriptor_state{};
    descriptor_state.fd = descriptor;
    descriptor_state.events = POLLIN;
    int poll_result = 0;
    do {
      poll_result = ::poll(&descriptor_state, 1, timeout_ms);
    } while (poll_result < 0 && errno == EINTR);
    if (poll_result == 0) {
      return Status(StatusCode::Timeout, "endpoint report timed out");
    }
    if (poll_result < 0) {
      return Status(
          StatusCode::IoError, "polling endpoint report failed", errno);
    }
    const auto result =
        ::read(descriptor, bytes + read_bytes, size - read_bytes);
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      return Status(
          StatusCode::IoError, "reading endpoint report failed", errno);
    }
    if (result == 0) {
      return Status(
          StatusCode::PeerDead, "endpoint report pipe closed");
    }
    read_bytes += static_cast<std::size_t>(result);
  }
  return Status::Ok();
}

[[nodiscard]] WireReport ReportFor(
    ReportEvent event, const Status& status,
    Clock::time_point start = Clock::now()) noexcept {
  WireReport report;
  report.event = static_cast<std::uint32_t>(event);
  report.status_code = static_cast<std::uint32_t>(status.code());
  report.elapsed_ns = DurationNanoseconds(start, Clock::now());
  return report;
}

[[nodiscard]] Deadline DeadlineAfter(std::uint32_t timeout_ms) noexcept {
  return Deadline::After(std::chrono::milliseconds(timeout_ms));
}

[[nodiscard]] BackpressurePolicy PolicyFrom(
    std::uint32_t raw) noexcept {
  switch (raw) {
    case static_cast<std::uint32_t>(BackpressurePolicy::Block):
      return BackpressurePolicy::Block;
    case static_cast<std::uint32_t>(BackpressurePolicy::Timeout):
      return BackpressurePolicy::Timeout;
    case static_cast<std::uint32_t>(BackpressurePolicy::Drop):
      return BackpressurePolicy::Drop;
    default:
      return BackpressurePolicy::Block;
  }
}

[[nodiscard]] int ProducerMain(
    const ChannelConfig& config, int command_fd, int report_fd) {
  auto transport_result =
      SharedMemoryTransport::CreateProducer(config);
  if (!transport_result) {
    const auto report = ReportFor(
        ReportEvent::Ready, transport_result.status());
    static_cast<void>(WriteFull(report_fd, &report, sizeof(report)));
    return 20;
  }
  auto transport = std::move(transport_result).take_value();
  auto ready = ReportFor(ReportEvent::Ready, Status::Ok());
  ready.stats = transport->Stats();
  if (!WriteFull(report_fd, &ready, sizeof(ready))) {
    return 21;
  }

  for (;;) {
    WireCommand command;
    if (!ReadFull(command_fd, &command, sizeof(command))) {
      return 22;
    }
    const auto start = Clock::now();
    const auto kind = static_cast<CommandKind>(command.kind);
    if (kind == CommandKind::Stop) {
      transport->Close();
      auto report =
          ReportFor(ReportEvent::Completed, Status::Ok(), start);
      report.stats = transport->Stats();
      static_cast<void>(WriteFull(report_fd, &report, sizeof(report)));
      return 0;
    }
    if (kind == CommandKind::Stats) {
      auto report =
          ReportFor(ReportEvent::Completed, Status::Ok(), start);
      report.stats = transport->Stats();
      if (!WriteFull(report_fd, &report, sizeof(report))) {
        return 23;
      }
      continue;
    }
    if (kind != CommandKind::Publish &&
        kind != CommandKind::LoanAndHold) {
      const auto report = ReportFor(
          ReportEvent::Completed,
          Status(StatusCode::InvalidArgument,
                 "producer received an invalid command"),
          start);
      if (!WriteFull(report_fd, &report, sizeof(report))) {
        return 24;
      }
      continue;
    }

    auto loan_result = transport->Loan(
        static_cast<std::size_t>(command.payload_size),
        SendOptions{PolicyFrom(command.policy),
                    DeadlineAfter(command.timeout_ms)});
    if (!loan_result) {
      auto report = ReportFor(
          ReportEvent::Completed, loan_result.status(), start);
      report.sequence = command.sequence;
      report.stats = transport->Stats();
      if (!WriteFull(report_fd, &report, sizeof(report))) {
        return 25;
      }
      continue;
    }
    auto loan = std::move(loan_result).take_value();
    FillPayload(loan.Data(), command.sequence);
    if (kind == CommandKind::LoanAndHold) {
      auto report =
          ReportFor(ReportEvent::LoanHeld, Status::Ok(), start);
      report.sequence = command.sequence;
      report.stats = transport->Stats();
      if (!WriteFull(report_fd, &report, sizeof(report))) {
        return 26;
      }
      for (;;) {
        ::pause();
      }
    }

    const auto publish_status = loan.Publish();
    auto report =
        ReportFor(ReportEvent::Completed, publish_status, start);
    report.sequence = command.sequence;
    report.stats = transport->Stats();
    if (!WriteFull(report_fd, &report, sizeof(report))) {
      return 27;
    }
  }
}

[[nodiscard]] int ConsumerMain(
    const ChannelConfig& config, int command_fd, int report_fd) {
  auto transport_result =
      SharedMemoryTransport::OpenConsumer(config);
  if (!transport_result) {
    const auto report = ReportFor(
        ReportEvent::Ready, transport_result.status());
    static_cast<void>(WriteFull(report_fd, &report, sizeof(report)));
    return 30;
  }
  auto transport = std::move(transport_result).take_value();
  auto ready = ReportFor(ReportEvent::Ready, Status::Ok());
  ready.stats = transport->Stats();
  if (!WriteFull(report_fd, &ready, sizeof(ready))) {
    return 31;
  }

  for (;;) {
    WireCommand command;
    if (!ReadFull(command_fd, &command, sizeof(command))) {
      return 32;
    }
    const auto start = Clock::now();
    const auto kind = static_cast<CommandKind>(command.kind);
    if (kind == CommandKind::Stop) {
      transport->Close();
      auto report =
          ReportFor(ReportEvent::Completed, Status::Ok(), start);
      report.stats = transport->Stats();
      static_cast<void>(WriteFull(report_fd, &report, sizeof(report)));
      return 0;
    }
    if (kind == CommandKind::Stats) {
      auto report =
          ReportFor(ReportEvent::Completed, Status::Ok(), start);
      report.stats = transport->Stats();
      if (!WriteFull(report_fd, &report, sizeof(report))) {
        return 33;
      }
      continue;
    }
    if (kind != CommandKind::TakeAndRelease &&
        kind != CommandKind::TakeAndHold &&
        kind != CommandKind::TakeDelayRelease) {
      const auto report = ReportFor(
          ReportEvent::Completed,
          Status(StatusCode::InvalidArgument,
                 "consumer received an invalid command"),
          start);
      if (!WriteFull(report_fd, &report, sizeof(report))) {
        return 34;
      }
      continue;
    }

    auto sample_result =
        transport->Take(DeadlineAfter(command.timeout_ms));
    if (!sample_result) {
      auto report = ReportFor(
          ReportEvent::Completed, sample_result.status(), start);
      report.stats = transport->Stats();
      if (!WriteFull(report_fd, &report, sizeof(report))) {
        return 35;
      }
      continue;
    }
    auto sample = std::move(sample_result).take_value();
    const auto validation = ValidatePayload(sample.Data());
    const auto validation_status =
        validation.checksum_match
            ? Status::Ok()
            : Status(StatusCode::CorruptData,
                     "payload checksum did not match");
    if (kind == CommandKind::TakeAndHold) {
      auto report = ReportFor(
          ReportEvent::SampleHeld, validation_status, start);
      report.sequence = validation.sequence;
      report.checksum_match = validation.checksum_match ? 1U : 0U;
      report.stats = transport->Stats();
      if (!WriteFull(report_fd, &report, sizeof(report))) {
        return 36;
      }
      for (;;) {
        ::pause();
      }
    }

    if (kind == CommandKind::TakeDelayRelease) {
      std::this_thread::sleep_for(
          std::chrono::milliseconds(command.delay_ms));
    }
    auto status = validation_status;
    const auto release_status = sample.Release();
    if (status && !release_status) {
      status = release_status;
    }
    auto report =
        ReportFor(ReportEvent::Completed, status, start);
    report.sequence = validation.sequence;
    report.checksum_match = validation.checksum_match ? 1U : 0U;
    report.stats = transport->Stats();
    if (!WriteFull(report_fd, &report, sizeof(report))) {
      return 37;
    }
  }
}

class EndpointProcess {
 public:
  EndpointProcess() = default;
  ~EndpointProcess() { ForceKill(); }

  EndpointProcess(const EndpointProcess&) = delete;
  EndpointProcess& operator=(const EndpointProcess&) = delete;

  [[nodiscard]] Status Start(
      EndpointRole role, const ChannelConfig& config,
      std::chrono::milliseconds timeout) {
    if (running_) {
      return Status(
          StatusCode::AlreadyExists, "endpoint process is already running");
    }
    int command_pipe[2]{-1, -1};
    int report_pipe[2]{-1, -1};
    if (::pipe(command_pipe) != 0) {
      return Status(
          StatusCode::IoError, "creating command pipe failed", errno);
    }
    if (::pipe(report_pipe) != 0) {
      const auto native_error = errno;
      static_cast<void>(::close(command_pipe[0]));
      static_cast<void>(::close(command_pipe[1]));
      return Status(
          StatusCode::IoError, "creating report pipe failed", native_error);
    }

    const auto child = ::fork();
    if (child < 0) {
      const auto native_error = errno;
      static_cast<void>(::close(command_pipe[0]));
      static_cast<void>(::close(command_pipe[1]));
      static_cast<void>(::close(report_pipe[0]));
      static_cast<void>(::close(report_pipe[1]));
      return Status(
          StatusCode::IoError, "forking endpoint failed", native_error);
    }
    if (child == 0) {
      static_cast<void>(::close(command_pipe[1]));
      static_cast<void>(::close(report_pipe[0]));
      const int exit_code =
          role == EndpointRole::Producer
              ? ProducerMain(config, command_pipe[0], report_pipe[1])
              : ConsumerMain(config, command_pipe[0], report_pipe[1]);
      ::_exit(exit_code);
    }

    static_cast<void>(::close(command_pipe[0]));
    static_cast<void>(::close(report_pipe[1]));
    pid_ = child;
    command_fd_ = command_pipe[1];
    report_fd_ = report_pipe[0];
    running_ = true;
    role_ = role;

    const auto ready_result = Receive(timeout);
    if (!ready_result) {
      const auto failure = ready_result.status();
      ForceKill();
      return failure;
    }
    const auto ready = ready_result.value();
    if (ready.event != static_cast<std::uint32_t>(ReportEvent::Ready)) {
      ForceKill();
      return Status(
          StatusCode::CorruptData,
          "endpoint emitted a non-ready startup report");
    }
    const auto startup_status = StatusFrom(ready, "endpoint startup failed");
    if (!startup_status) {
      ForceKill();
      return startup_status;
    }
    return Status::Ok();
  }

  [[nodiscard]] Status Send(const WireCommand& command) noexcept {
    if (!running_) {
      return Status(StatusCode::Closed, "endpoint process is not running");
    }
    if (!WriteFull(command_fd_, &command, sizeof(command))) {
      return Status(
          StatusCode::PeerDead, "endpoint command pipe closed", errno);
    }
    return Status::Ok();
  }

  [[nodiscard]] Result<WireReport> Receive(
      std::chrono::milliseconds timeout) noexcept {
    if (!running_) {
      return Status(StatusCode::Closed, "endpoint process is not running");
    }
    WireReport report;
    const auto status =
        ReadFullFor(report_fd_, &report, sizeof(report), timeout);
    if (!status) {
      return status;
    }
    if (report.status_code >
        static_cast<std::uint32_t>(StatusCode::Unsupported)) {
      return Status(
          StatusCode::CorruptData,
          "endpoint report contained an invalid status code");
    }
    return report;
  }

  [[nodiscard]] Status Stop(
      std::chrono::milliseconds timeout) noexcept {
    if (!running_) {
      return Status::Ok();
    }
    WireCommand command;
    command.kind = static_cast<std::uint32_t>(CommandKind::Stop);
    const auto send_status = Send(command);
    if (!send_status) {
      ForceKill();
      return send_status;
    }
    const auto report_result = Receive(timeout);
    if (!report_result) {
      const auto failure = report_result.status();
      ForceKill();
      return failure;
    }
    const auto report = report_result.value();
    const auto report_status =
        StatusFrom(report, "endpoint stop failed");
    int child_status = 0;
    while (::waitpid(pid_, &child_status, 0) < 0) {
      if (errno != EINTR) {
        const auto native_error = errno;
        CloseDescriptors();
        running_ = false;
        pid_ = -1;
        return Status(
            StatusCode::IoError, "waiting for endpoint failed",
            native_error);
      }
    }
    CloseDescriptors();
    running_ = false;
    pid_ = -1;
    if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
      return Status(
          StatusCode::PeerDead,
          "endpoint did not exit cleanly after stop");
    }
    return report_status;
  }

  [[nodiscard]] Status Kill() noexcept {
    if (!running_) {
      return Status(StatusCode::Closed, "endpoint process is not running");
    }
    if (::kill(pid_, SIGKILL) != 0 && errno != ESRCH) {
      return Status(
          StatusCode::IoError, "killing endpoint failed", errno);
    }
    int child_status = 0;
    while (::waitpid(pid_, &child_status, 0) < 0) {
      if (errno != EINTR) {
        const auto native_error = errno;
        CloseDescriptors();
        running_ = false;
        pid_ = -1;
        return Status(
            StatusCode::IoError, "waiting for killed endpoint failed",
            native_error);
      }
    }
    CloseDescriptors();
    running_ = false;
    pid_ = -1;
    if (!WIFSIGNALED(child_status) ||
        WTERMSIG(child_status) != SIGKILL) {
      return Status(
          StatusCode::CorruptData,
          "endpoint did not terminate from SIGKILL");
    }
    return Status::Ok();
  }

  [[nodiscard]] pid_t pid() const noexcept { return pid_; }
  [[nodiscard]] bool running() const noexcept { return running_; }
  [[nodiscard]] EndpointRole role() const noexcept { return role_; }

 private:
  [[nodiscard]] static Status StatusFrom(
      const WireReport& report, std::string_view detail) {
    const auto code = static_cast<StatusCode>(report.status_code);
    return code == StatusCode::Ok
               ? Status::Ok()
               : Status(code, std::string(detail));
  }

  void CloseDescriptors() noexcept {
    if (command_fd_ >= 0) {
      static_cast<void>(::close(command_fd_));
      command_fd_ = -1;
    }
    if (report_fd_ >= 0) {
      static_cast<void>(::close(report_fd_));
      report_fd_ = -1;
    }
  }

  void ForceKill() noexcept {
    if (running_) {
      static_cast<void>(::kill(pid_, SIGKILL));
      while (::waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {
      }
    }
    CloseDescriptors();
    running_ = false;
    pid_ = -1;
  }

  pid_t pid_{-1};
  int command_fd_{-1};
  int report_fd_{-1};
  bool running_{false};
  EndpointRole role_{EndpointRole::Producer};
};

[[nodiscard]] std::string EscapeJson(std::string_view value) {
  std::ostringstream output;
  for (const char character : value) {
    switch (character) {
      case '"':
        output << "\\\"";
        break;
      case '\\':
        output << "\\\\";
        break;
      case '\n':
        output << "\\n";
        break;
      case '\r':
        output << "\\r";
        break;
      case '\t':
        output << "\\t";
        break;
      default:
        output << character;
        break;
    }
  }
  return output.str();
}

[[nodiscard]] std::string UtcNow() {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm utc{};
  gmtime_r(&time, &utc);
  std::ostringstream output;
  output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
  return output.str();
}

[[nodiscard]] std::optional<std::uint64_t> CurrentRssKiB(
    pid_t process_id) {
  const std::string path =
      "/proc/" + std::to_string(process_id) + "/statm";
  std::ifstream input(path);
  std::uint64_t total_pages = 0U;
  std::uint64_t resident_pages = 0U;
  if (!(input >> total_pages >> resident_pages)) {
    return std::nullopt;
  }
  static_cast<void>(total_pages);
  const auto page_size = ::sysconf(_SC_PAGESIZE);
  if (page_size <= 0) {
    return std::nullopt;
  }
  return resident_pages *
         static_cast<std::uint64_t>(page_size) / 1024U;
}

[[nodiscard]] double Percentile(
    std::span<const double> observations, double quantile) {
  if (observations.empty()) {
    return 0.0;
  }
  std::vector<double> sorted(observations.begin(), observations.end());
  std::sort(sorted.begin(), sorted.end());
  const auto rank =
      static_cast<std::size_t>(
          std::ceil(quantile * static_cast<double>(sorted.size())));
  const auto index =
      std::min(sorted.size() - 1U, std::max<std::size_t>(1U, rank) - 1U);
  return sorted[index];
}

[[nodiscard]] std::uint32_t MillisecondsToWire(
    std::chrono::milliseconds value) noexcept {
  const auto count = value.count();
  if (count <= 0) {
    return 0U;
  }
  if (count > std::numeric_limits<std::uint32_t>::max()) {
    return std::numeric_limits<std::uint32_t>::max();
  }
  return static_cast<std::uint32_t>(count);
}

[[nodiscard]] bool IsRecoveryOperation(Operation operation) noexcept {
  return operation == Operation::KillProducer ||
         operation == Operation::KillConsumer ||
         operation == Operation::RestartProducer ||
         operation == Operation::RestartConsumer;
}

struct Counters {
  std::uint64_t crash_count{0U};
  std::uint64_t restart_count{0U};
  std::uint64_t recovery_count{0U};
  std::uint64_t checksum_mismatches{0U};
  std::uint64_t duplicate_messages{0U};
  std::uint64_t unexpected_messages{0U};
  std::uint64_t expected_timeouts{0U};
  std::uint64_t unexpected_timeouts{0U};
  std::uint64_t expected_drops{0U};
  std::uint64_t operation_failures{0U};
  std::uint64_t cleanup_failures{0U};
};

}  // namespace

struct Runner::Impl {
  explicit Impl(RunnerConfig runner_config)
      : config(std::move(runner_config)),
        latency_windows(config.latency_window) {}

  ~Impl() {
    static_cast<void>(consumer.Stop(config.command_timeout));
    static_cast<void>(producer.Stop(config.command_timeout));
    UnlinkChannel();
  }

  [[nodiscard]] Status Execute(
      EndpointProcess& endpoint, const WireCommand& command,
      WireReport& report) {
    const auto send_status = endpoint.Send(command);
    if (!send_status) {
      return send_status;
    }
    const auto timeout =
        config.command_timeout + config.delay + config.peer_timeout;
    const auto report_result = endpoint.Receive(timeout);
    if (!report_result) {
      return report_result.status();
    }
    report = report_result.value();
    return Status::Ok();
  }

  [[nodiscard]] Status ExpectCode(
      const WireReport& report, StatusCode expected,
      std::string_view context) {
    const auto observed =
        static_cast<StatusCode>(report.status_code);
    if (observed == expected) {
      return Status::Ok();
    }
    if (observed == StatusCode::Timeout &&
        expected != StatusCode::Timeout) {
      ++counters.unexpected_timeouts;
    }
    return Status(
        observed,
        std::string(context) + ": expected " +
            Status(expected).ToString() + ", observed " +
            Status(observed).ToString());
  }

  [[nodiscard]] WireCommand PublishCommand(
      std::uint64_t sequence, BackpressurePolicy policy,
      std::chrono::milliseconds timeout,
      CommandKind kind = CommandKind::Publish) const noexcept {
    WireCommand command;
    command.kind = static_cast<std::uint32_t>(kind);
    command.policy = static_cast<std::uint32_t>(policy);
    command.timeout_ms = MillisecondsToWire(timeout);
    command.sequence = sequence;
    command.payload_size =
        static_cast<std::uint64_t>(config.payload_size);
    return command;
  }

  [[nodiscard]] WireCommand TakeCommand(
      CommandKind kind, std::chrono::milliseconds timeout,
      std::chrono::milliseconds delay = 0ms) const noexcept {
    WireCommand command;
    command.kind = static_cast<std::uint32_t>(kind);
    command.timeout_ms = MillisecondsToWire(timeout);
    command.delay_ms = MillisecondsToWire(delay);
    return command;
  }

  [[nodiscard]] Status StartProducer(bool restart) {
    const auto status = producer.Start(
        EndpointRole::Producer, channel, config.command_timeout);
    if (status && restart) {
      ++counters.restart_count;
    }
    return status;
  }

  [[nodiscard]] Status StartConsumer(bool restart) {
    const auto status = consumer.Start(
        EndpointRole::Consumer, channel, config.command_timeout);
    if (status && restart) {
      ++counters.restart_count;
    }
    return status;
  }

  [[nodiscard]] std::uint64_t NextSequence() const noexcept {
    return sequence_ledger.NextCandidate();
  }

  [[nodiscard]] Status TrackPublished(std::uint64_t sequence) {
    if (!sequence_ledger.RecordPublished(sequence)) {
      ++counters.unexpected_messages;
      return Status(
          StatusCode::CorruptData,
          "publisher sequence was not contiguous");
    }
    return Status::Ok();
  }

  [[nodiscard]] Status VerifyHeld(
      const WireReport& report, std::uint64_t expected_sequence) {
    if (report.checksum_match == 0U) {
      ++counters.checksum_mismatches;
      return Status(
          StatusCode::CorruptData,
          "held sample checksum mismatch");
    }
    if (report.sequence != expected_sequence) {
      ++counters.unexpected_messages;
      return Status(
          StatusCode::CorruptData,
          "held sample sequence mismatch");
    }
    return Status::Ok();
  }

  [[nodiscard]] Status VerifyConsumed(
      const WireReport& report,
      std::uint64_t expected_sequence) {
    if (report.checksum_match == 0U) {
      ++counters.checksum_mismatches;
      return Status(
          StatusCode::CorruptData,
          "consumed payload checksum mismatch");
    }
    const auto classification = sequence_ledger.RecordConsumed(
        report.sequence, expected_sequence);
    switch (classification) {
      case DeliveryClassification::Ok:
        return Status::Ok();
      case DeliveryClassification::Duplicate:
        ++counters.duplicate_messages;
        return Status(
            StatusCode::CorruptData,
            "sequence was delivered more than once");
      case DeliveryClassification::Unexpected:
        ++counters.unexpected_messages;
        return Status(
            StatusCode::CorruptData,
            "consumed sequence was unpublished or out of FIFO order");
    }
    return Status(StatusCode::CorruptData, "unknown delivery classification");
  }

  [[nodiscard]] Status Publish(
      std::uint64_t sequence, BackpressurePolicy policy,
      std::chrono::milliseconds timeout,
      WireReport* observed_report = nullptr) {
    WireReport report;
    const auto execute_status = Execute(
        producer, PublishCommand(sequence, policy, timeout), report);
    if (!execute_status) {
      return execute_status;
    }
    const auto status =
        ExpectCode(report, StatusCode::Ok, "publish");
    if (!status) {
      return status;
    }
    const auto tracking_status = TrackPublished(sequence);
    if (!tracking_status) {
      return tracking_status;
    }
    if (observed_report != nullptr) {
      *observed_report = report;
    }
    return Status::Ok();
  }

  [[nodiscard]] Status Consume(
      std::uint64_t expected_sequence,
      std::chrono::milliseconds timeout,
      WireReport* observed_report = nullptr) {
    WireReport report;
    const auto execute_status = Execute(
        consumer,
        TakeCommand(CommandKind::TakeAndRelease, timeout),
        report);
    if (!execute_status) {
      return execute_status;
    }
    const auto status =
        ExpectCode(report, StatusCode::Ok, "consume");
    if (!status) {
      return status;
    }
    const auto verification_status =
        VerifyConsumed(report, expected_sequence);
    if (!verification_status) {
      return verification_status;
    }
    if (observed_report != nullptr) {
      *observed_report = report;
    }
    return Status::Ok();
  }

  [[nodiscard]] Status Stats(
      EndpointProcess& endpoint, TransportStats& stats) {
    WireCommand command;
    command.kind = static_cast<std::uint32_t>(CommandKind::Stats);
    WireReport report;
    const auto execute_status = Execute(endpoint, command, report);
    if (!execute_status) {
      return execute_status;
    }
    const auto status =
        ExpectCode(report, StatusCode::Ok, "stats");
    if (!status) {
      return status;
    }
    stats = report.stats;
    return Status::Ok();
  }

  [[nodiscard]] Status KillProducer() {
    const auto sequence = NextSequence();
    WireReport held;
    const auto execute_status = Execute(
        producer,
        PublishCommand(
            sequence, BackpressurePolicy::Block,
            config.command_timeout, CommandKind::LoanAndHold),
        held);
    if (!execute_status) {
      return execute_status;
    }
    auto status =
        ExpectCode(held, StatusCode::Ok, "producer loan hold");
    if (!status) {
      return status;
    }
    if (held.event !=
            static_cast<std::uint32_t>(ReportEvent::LoanHeld) ||
        held.sequence != sequence) {
      return Status(
          StatusCode::CorruptData,
          "producer did not report the expected held loan");
    }
    status = producer.Kill();
    if (!status) {
      return status;
    }
    ++counters.crash_count;
    return StartProducer(true);
  }

  [[nodiscard]] Status KillConsumer() {
    const auto sequence = NextSequence();
    auto status = Publish(
        sequence, BackpressurePolicy::Block, config.command_timeout);
    if (!status) {
      return status;
    }
    WireReport held;
    status = Execute(
        consumer,
        TakeCommand(
            CommandKind::TakeAndHold, config.command_timeout),
        held);
    if (!status) {
      return status;
    }
    status =
        ExpectCode(held, StatusCode::Ok, "consumer sample hold");
    if (!status) {
      return status;
    }
    if (held.event !=
        static_cast<std::uint32_t>(ReportEvent::SampleHeld)) {
      return Status(
          StatusCode::CorruptData,
          "consumer did not report the expected held sample");
    }
    status = VerifyHeld(held, sequence);
    if (!status) {
      return status;
    }
    status = consumer.Kill();
    if (!status) {
      return status;
    }
    ++counters.crash_count;
    status = StartConsumer(true);
    if (!status) {
      return status;
    }
    return Consume(sequence, config.command_timeout);
  }

  [[nodiscard]] Status RestartProducer() {
    auto status = producer.Stop(config.command_timeout);
    if (!status) {
      return status;
    }
    return StartProducer(true);
  }

  [[nodiscard]] Status RestartConsumer() {
    auto status = consumer.Stop(config.command_timeout);
    if (!status) {
      return status;
    }
    return StartConsumer(true);
  }

  [[nodiscard]] Status SlowConsumer() {
    std::vector<std::uint64_t> sequences;
    sequences.reserve(config.slot_count + 1U);
    for (std::uint32_t index = 0U;
         index < config.slot_count; ++index) {
      const auto sequence = NextSequence();
      const auto status = Publish(
          sequence, BackpressurePolicy::Block,
          config.command_timeout);
      if (!status) {
        return status;
      }
      sequences.push_back(sequence);
    }

    const auto delayed_take = TakeCommand(
        CommandKind::TakeDelayRelease,
        config.command_timeout, config.delay);
    auto status = consumer.Send(delayed_take);
    if (!status) {
      return status;
    }

    const auto blocked_sequence = NextSequence();
    WireReport publish_report;
    status = Execute(
        producer,
        PublishCommand(
            blocked_sequence, BackpressurePolicy::Block,
            config.command_timeout),
        publish_report);
    if (!status) {
      return status;
    }
    status = ExpectCode(
        publish_report, StatusCode::Ok,
        "slow-consumer blocked publish");
    if (!status) {
      return status;
    }
    status = TrackPublished(blocked_sequence);
    if (!status) {
      return status;
    }
    sequences.push_back(blocked_sequence);

    const auto delayed_report_result =
        consumer.Receive(config.command_timeout + config.delay);
    if (!delayed_report_result) {
      return delayed_report_result.status();
    }
    const auto delayed_report = delayed_report_result.value();
    status = ExpectCode(
        delayed_report, StatusCode::Ok,
        "slow-consumer delayed take");
    if (!status) {
      return status;
    }
    status = VerifyConsumed(delayed_report, sequences.front());
    if (!status) {
      return status;
    }
    for (std::size_t index = 1U;
         index < sequences.size(); ++index) {
      status = Consume(sequences[index], config.command_timeout);
      if (!status) {
        return status;
      }
    }
    return Status::Ok();
  }

  [[nodiscard]] Status QueuePressure() {
    TransportStats before;
    auto status = Stats(producer, before);
    if (!status) {
      return status;
    }

    std::vector<std::uint64_t> queued;
    queued.reserve(config.slot_count);
    for (std::uint32_t index = 0U;
         index < config.slot_count; ++index) {
      const auto sequence = NextSequence();
      status = Publish(
          sequence, BackpressurePolicy::Block,
          config.command_timeout);
      if (!status) {
        return status;
      }
      queued.push_back(sequence);
    }

    WireReport dropped;
    status = Execute(
        producer,
        PublishCommand(
            NextSequence(), BackpressurePolicy::Drop, 0ms),
        dropped);
    if (!status) {
      return status;
    }
    ++counters.expected_drops;
    status = ExpectCode(
        dropped, StatusCode::Dropped, "queue-pressure drop");
    if (!status) {
      return status;
    }

    WireReport timed_out;
    status = Execute(
        producer,
        PublishCommand(
            NextSequence(), BackpressurePolicy::Timeout,
            config.delay),
        timed_out);
    if (!status) {
      return status;
    }
    ++counters.expected_timeouts;
    status = ExpectCode(
        timed_out, StatusCode::Timeout,
        "queue-pressure timeout");
    if (!status) {
      return status;
    }

    TransportStats after;
    status = Stats(producer, after);
    if (!status) {
      return status;
    }
    if (after.dropped_messages <= before.dropped_messages ||
        after.send_timeouts <= before.send_timeouts) {
      return Status(
          StatusCode::CorruptData,
          "queue-pressure counters did not advance");
    }

    for (const auto sequence : queued) {
      status = Consume(sequence, config.command_timeout);
      if (!status) {
        return status;
      }
    }
    return Status::Ok();
  }

  [[nodiscard]] Status Timeout() {
    TransportStats before;
    auto status = Stats(consumer, before);
    if (!status) {
      return status;
    }
    WireReport report;
    status = Execute(
        consumer,
        TakeCommand(CommandKind::TakeAndRelease, config.delay),
        report);
    if (!status) {
      return status;
    }
    ++counters.expected_timeouts;
    status =
        ExpectCode(report, StatusCode::Timeout, "empty receive timeout");
    if (!status) {
      return status;
    }
    TransportStats after;
    status = Stats(consumer, after);
    if (!status) {
      return status;
    }
    if (after.receive_timeouts <= before.receive_timeouts) {
      return Status(
          StatusCode::CorruptData,
          "receive timeout counter did not advance");
    }
    return Status::Ok();
  }

  [[nodiscard]] Status DelayWakeup() {
    const auto sequence = NextSequence();
    const auto take = TakeCommand(
        CommandKind::TakeAndRelease, config.command_timeout);
    auto status = consumer.Send(take);
    if (!status) {
      return status;
    }
    std::this_thread::sleep_for(config.delay);
    status = Publish(
        sequence, BackpressurePolicy::Block,
        config.command_timeout);
    if (!status) {
      return status;
    }
    const auto report_result =
        consumer.Receive(config.command_timeout);
    if (!report_result) {
      return report_result.status();
    }
    const auto report = report_result.value();
    status =
        ExpectCode(report, StatusCode::Ok, "delayed wakeup take");
    if (!status) {
      return status;
    }
    return VerifyConsumed(report, sequence);
  }

  [[nodiscard]] Status Perform(Operation operation) {
    switch (operation) {
      case Operation::KillProducer:
        return KillProducer();
      case Operation::KillConsumer:
        return KillConsumer();
      case Operation::RestartProducer:
        return RestartProducer();
      case Operation::RestartConsumer:
        return RestartConsumer();
      case Operation::SlowConsumer:
        return SlowConsumer();
      case Operation::QueuePressure:
        return QueuePressure();
      case Operation::Timeout:
        return Timeout();
      case Operation::DelayWakeup:
        return DelayWakeup();
    }
    return Status(
        StatusCode::InvalidArgument, "unknown chaos operation");
  }

  [[nodiscard]] Result<double> RecoveryProbe() {
    if (sequence_ledger.outstanding_count() != 0U) {
      return Status(
          StatusCode::CorruptData,
          "operation left messages outstanding before recovery probe");
    }
    const auto sequence = NextSequence();
    const auto start = Clock::now();
    auto status = Publish(
        sequence, BackpressurePolicy::Block,
        config.command_timeout);
    if (!status) {
      return status;
    }
    status = Consume(sequence, config.command_timeout);
    if (!status) {
      return status;
    }
    const auto elapsed =
        std::chrono::duration<double, std::micro>(
            Clock::now() - start)
            .count();
    return elapsed;
  }

  [[nodiscard]] std::optional<std::uint64_t> TotalRssKiB() const {
    const auto parent = CurrentRssKiB(::getpid());
    const auto producer_rss =
        producer.running()
            ? CurrentRssKiB(producer.pid())
            : std::optional<std::uint64_t>{0U};
    const auto consumer_rss =
        consumer.running()
            ? CurrentRssKiB(consumer.pid())
            : std::optional<std::uint64_t>{0U};
    if (!parent || !producer_rss || !consumer_rss) {
      return std::nullopt;
    }
    return *parent + *producer_rss + *consumer_rss;
  }

  void WriteLine(
      std::string_view line, std::ostream& output,
      std::ostream* mirror) {
    output << line << '\n';
    output.flush();
    if (mirror != nullptr && mirror != &output) {
      *mirror << line << '\n';
      mirror->flush();
    }
  }

  void WriteEnvironment(
      std::ostream& output, std::ostream* mirror) {
    utsname system{};
    static_cast<void>(::uname(&system));
    char hostname[256]{};
    static_cast<void>(::gethostname(hostname, sizeof(hostname) - 1U));
    std::ostringstream line;
    line << "{\"schema_version\":1,"
         << "\"record_type\":\"environment\","
         << "\"runner\":\"fastipc_seeded_chaos\","
         << "\"run_id\":\"" << EscapeJson(config.run_id) << "\","
         << "\"timestamp_utc\":\"" << UtcNow() << "\","
         << "\"source_revision\":\""
         << FASTIPC_CHAOS_SOURCE_REVISION << "\","
         << "\"build_type\":\"" << FASTIPC_CHAOS_BUILD_TYPE << "\","
         << "\"compiler\":\"" << FASTIPC_CHAOS_COMPILER << "\","
         << "\"hostname\":\"" << EscapeJson(hostname) << "\","
         << "\"kernel_release\":\""
         << EscapeJson(system.release) << "\","
         << "\"machine\":\"" << EscapeJson(system.machine) << "\","
         << "\"seed\":" << config.seed << ','
         << "\"minimum_operations\":" << config.minimum_operations << ','
         << "\"minimum_duration_ms\":"
         << config.minimum_duration.count() << ','
         << "\"slot_count\":" << config.slot_count << ','
         << "\"payload_bytes\":" << config.payload_size << ','
         << "\"peer_timeout_ms\":" << config.peer_timeout.count() << ','
         << "\"command_timeout_ms\":"
         << config.command_timeout.count() << ','
         << "\"delay_ms\":" << config.delay.count() << ','
         << "\"latency_window\":" << config.latency_window << ','
         << "\"plan\":\"seeded_shuffled_cycles_of_all_operations\""
         << '}';
    WriteLine(line.str(), output, mirror);
  }

  void WriteOperation(
      std::size_t index, Operation operation, const Status& status,
      std::optional<double> probe_latency_us,
      std::ostream& output, std::ostream* mirror) {
    const auto rss = TotalRssKiB();
    std::ostringstream line;
    line << std::fixed << std::setprecision(3)
         << "{\"schema_version\":1,"
         << "\"record_type\":\"operation\","
         << "\"runner\":\"fastipc_seeded_chaos\","
         << "\"run_id\":\"" << EscapeJson(config.run_id) << "\","
         << "\"seed\":" << config.seed << ','
         << "\"operation_index\":" << index << ','
         << "\"operation\":\"" << ToString(operation) << "\","
         << "\"status\":\"" << (status ? "ok" : "failed") << "\","
         << "\"detail\":\""
         << EscapeJson(status ? "ok" : status.ToString()) << "\","
         << "\"probe_latency_us\":";
    if (probe_latency_us) {
      line << *probe_latency_us;
    } else {
      line << "null";
    }
    line << ",\"crash_count\":" << counters.crash_count
         << ",\"restart_count\":" << counters.restart_count
         << ",\"recovery_count\":" << counters.recovery_count
         << ",\"checksum_mismatches\":"
         << counters.checksum_mismatches
         << ",\"duplicate_messages\":"
         << counters.duplicate_messages
         << ",\"unexpected_timeouts\":"
         << counters.unexpected_timeouts
         << ",\"outstanding_messages\":"
         << sequence_ledger.outstanding_count()
         << ",\"total_rss_kib\":";
    if (rss) {
      line << *rss;
    } else {
      line << "null";
    }
    line << '}';
    WriteLine(line.str(), output, mirror);
  }

  void WriteSetupError(
      const Status& status, std::ostream& output,
      std::ostream* mirror) {
    std::ostringstream line;
    line << "{\"schema_version\":1,"
         << "\"record_type\":\"setup_error\","
         << "\"runner\":\"fastipc_seeded_chaos\","
         << "\"run_id\":\"" << EscapeJson(config.run_id) << "\","
         << "\"seed\":" << config.seed << ','
         << "\"detail\":\"" << EscapeJson(status.ToString()) << "\"}";
    WriteLine(line.str(), output, mirror);
  }

  void UnlinkChannel() noexcept {
    if (channel.name.empty()) {
      return;
    }
    const std::string object_name = "/" + channel.name;
    static_cast<void>(::shm_unlink(object_name.c_str()));
  }

  RunnerConfig config;
  ChannelConfig channel;
  EndpointProcess producer;
  EndpointProcess consumer;
  Counters counters;
  SequenceLedger sequence_ledger;
  LatencyWindows latency_windows;
  bool has_run{false};
};

Status ValidateConfig(const RunnerConfig& config) {
  if (config.minimum_operations == 0U &&
      config.minimum_duration <= 0ms) {
    return Status(
        StatusCode::InvalidArgument,
        "operations or duration must be positive");
  }
  if (config.slot_count < 2U) {
    return Status(
        StatusCode::InvalidArgument,
        "slot_count must be at least two");
  }
  if (config.payload_size < kMinimumPayloadBytes ||
      config.payload_size >
          std::numeric_limits<std::uint32_t>::max()) {
    return Status(
        StatusCode::InvalidArgument,
        "payload_size must be between 16 and UINT32_MAX");
  }
  if (config.peer_timeout <= 0ms ||
      config.command_timeout <= 0ms ||
      config.delay <= 0ms) {
    return Status(
        StatusCode::InvalidArgument,
        "timeouts and delay must be positive");
  }
  if (config.command_timeout <=
      config.peer_timeout + config.delay) {
    return Status(
        StatusCode::InvalidArgument,
        "command_timeout must exceed peer_timeout + delay");
  }
  if (config.latency_window == 0U) {
    return Status(
        StatusCode::InvalidArgument,
        "latency_window must be positive");
  }
  if (config.max_memory_growth_kib &&
      *config.max_memory_growth_kib < 0) {
    return Status(
        StatusCode::InvalidArgument,
        "max_memory_growth_kib must not be negative");
  }
  if (config.max_p99_drift_us &&
      (!std::isfinite(*config.max_p99_drift_us) ||
       *config.max_p99_drift_us < 0.0)) {
    return Status(
        StatusCode::InvalidArgument,
        "max_p99_drift_us must be finite and non-negative");
  }
  return Status::Ok();
}

Runner::Runner(RunnerConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

Runner::~Runner() = default;
Runner::Runner(Runner&&) noexcept = default;
Runner& Runner::operator=(Runner&&) noexcept = default;

Status Runner::Run(std::ostream& output, std::ostream* mirror) {
  if (impl_->has_run) {
    return Status(
        StatusCode::InvalidArgument,
        "a ChaosRunner instance can only run once");
  }
  impl_->has_run = true;
  const auto validation = ValidateConfig(impl_->config);
  if (!validation) {
    return validation;
  }
  if (impl_->config.run_id.empty()) {
    impl_->config.run_id =
        "chaos-" + std::to_string(::getpid()) + "-" +
        std::to_string(impl_->config.seed);
  }
  static_cast<void>(::signal(SIGPIPE, SIG_IGN));

  const auto unique_suffix =
      static_cast<std::uint64_t>(
          Clock::now().time_since_epoch().count());
  impl_->channel.name =
      "fastipc_chaos_" + std::to_string(::getpid()) + "_" +
      std::to_string(impl_->config.seed) + "_" +
      std::to_string(unique_suffix);
  impl_->channel.slot_count = impl_->config.slot_count;
  impl_->channel.max_message_size =
      static_cast<std::uint32_t>(impl_->config.payload_size);
  impl_->channel.active_spin_count = 32U;
  impl_->channel.permissions = 0600U;
  impl_->channel.unlink_on_owner_close = false;
  impl_->channel.peer_timeout = impl_->config.peer_timeout;

  impl_->WriteEnvironment(output, mirror);
  auto status = impl_->StartProducer(false);
  if (status) {
    status = impl_->StartConsumer(false);
  }
  if (!status) {
    impl_->WriteSetupError(status, output, mirror);
    return status;
  }

  const auto rss_start = impl_->TotalRssKiB();
  auto maximum_rss = rss_start;
  const auto run_start = Clock::now();
  OperationPlanGenerator plan(impl_->config.seed);

  std::size_t operation_index = 0U;
  while (operation_index < impl_->config.minimum_operations ||
         Clock::now() - run_start <
             impl_->config.minimum_duration) {
    const auto operation = plan.Next();
    status = impl_->Perform(operation);
    std::optional<double> probe_latency;
    if (status) {
      const auto probe_result = impl_->RecoveryProbe();
      if (!probe_result) {
        status = probe_result.status();
      } else {
        probe_latency = probe_result.value();
        impl_->latency_windows.Record(*probe_latency);
        if (IsRecoveryOperation(operation)) {
          ++impl_->counters.recovery_count;
        }
      }
    }
    ++operation_index;
    if (!status) {
      ++impl_->counters.operation_failures;
    }
    impl_->WriteOperation(
        operation_index, operation, status, probe_latency,
        output, mirror);
    const auto current_rss = impl_->TotalRssKiB();
    if (current_rss &&
        (!maximum_rss || *current_rss > *maximum_rss)) {
      maximum_rss = current_rss;
    }
    if (!status) {
      break;
    }
  }

  const auto run_finish = Clock::now();
  const auto rss_end = impl_->TotalRssKiB();
  const auto consumer_stop =
      impl_->consumer.Stop(impl_->config.command_timeout);
  const auto producer_stop =
      impl_->producer.Stop(impl_->config.command_timeout);
  if (!consumer_stop) {
    ++impl_->counters.cleanup_failures;
  }
  if (!producer_stop) {
    ++impl_->counters.cleanup_failures;
  }
  impl_->UnlinkChannel();

  const auto lost_messages =
      static_cast<std::uint64_t>(impl_->sequence_ledger.outstanding_count());
  const auto baseline_window = impl_->latency_windows.Baseline();
  const auto final_window = impl_->latency_windows.Final();
  double baseline_p99_us = 0.0;
  double final_p99_us = 0.0;
  if (!baseline_window.empty()) {
    baseline_p99_us = Percentile(baseline_window, 0.99);
    final_p99_us = Percentile(final_window, 0.99);
  }
  const auto p99_drift_us = final_p99_us - baseline_p99_us;
  std::optional<std::int64_t> memory_growth_kib;
  if (rss_start && rss_end) {
    memory_growth_kib =
        static_cast<std::int64_t>(*rss_end) -
        static_cast<std::int64_t>(*rss_start);
  }

  bool passed =
      impl_->counters.operation_failures == 0U &&
      impl_->counters.cleanup_failures == 0U &&
      impl_->counters.checksum_mismatches == 0U &&
      impl_->counters.duplicate_messages == 0U &&
      impl_->counters.unexpected_messages == 0U &&
      impl_->counters.unexpected_timeouts == 0U &&
      lost_messages == 0U;
  bool memory_threshold_exceeded = false;
  if (impl_->config.max_memory_growth_kib &&
      memory_growth_kib &&
      *memory_growth_kib >
          *impl_->config.max_memory_growth_kib) {
    memory_threshold_exceeded = true;
    passed = false;
  }
  bool p99_threshold_exceeded = false;
  if (impl_->config.max_p99_drift_us &&
      p99_drift_us > *impl_->config.max_p99_drift_us) {
    p99_threshold_exceeded = true;
    passed = false;
  }

  const auto duration_ms =
      std::chrono::duration<double, std::milli>(
          run_finish - run_start)
          .count();
  std::ostringstream summary;
  summary << std::fixed << std::setprecision(3)
          << "{\"schema_version\":1,"
          << "\"record_type\":\"summary\","
          << "\"runner\":\"fastipc_seeded_chaos\","
          << "\"run_id\":\""
          << EscapeJson(impl_->config.run_id) << "\","
          << "\"status\":\"" << (passed ? "passed" : "failed")
          << "\",\"seed\":" << impl_->config.seed
          << ",\"operation_count\":" << operation_index
          << ",\"crash_count\":" << impl_->counters.crash_count
          << ",\"restart_count\":" << impl_->counters.restart_count
          << ",\"recovery_count\":" << impl_->counters.recovery_count
          << ",\"checksum_mismatches\":"
          << impl_->counters.checksum_mismatches
          << ",\"lost_messages\":" << lost_messages
          << ",\"duplicate_messages\":"
          << impl_->counters.duplicate_messages
          << ",\"unexpected_messages\":"
          << impl_->counters.unexpected_messages
          << ",\"expected_timeouts\":"
          << impl_->counters.expected_timeouts
          << ",\"unexpected_timeouts\":"
          << impl_->counters.unexpected_timeouts
          << ",\"expected_drops\":"
          << impl_->counters.expected_drops
          << ",\"operation_failures\":"
          << impl_->counters.operation_failures
          << ",\"cleanup_failures\":"
          << impl_->counters.cleanup_failures
          << ",\"sequence_tracker_mode\":\"contiguous_fifo\""
          << ",\"maximum_outstanding_messages\":"
          << impl_->sequence_ledger.maximum_outstanding_count()
          << ",\"retained_plan_operations\":"
          << plan.retained_operation_count()
          << ",\"actual_duration_ms\":" << duration_ms
          << ",\"probe_samples\":"
          << impl_->latency_windows.sample_count()
          << ",\"retained_probe_samples\":"
          << impl_->latency_windows.retained_sample_count()
          << ",\"baseline_p99_us\":" << baseline_p99_us
          << ",\"final_p99_us\":" << final_p99_us
          << ",\"p99_drift_us\":" << p99_drift_us
          << ",\"p99_threshold_exceeded\":"
          << (p99_threshold_exceeded ? "true" : "false")
          << ",\"rss_start_kib\":";
  if (rss_start) {
    summary << *rss_start;
  } else {
    summary << "null";
  }
  summary << ",\"rss_end_kib\":";
  if (rss_end) {
    summary << *rss_end;
  } else {
    summary << "null";
  }
  summary << ",\"maximum_rss_kib\":";
  if (maximum_rss) {
    summary << *maximum_rss;
  } else {
    summary << "null";
  }
  summary << ",\"memory_growth_kib\":";
  if (memory_growth_kib) {
    summary << *memory_growth_kib;
  } else {
    summary << "null";
  }
  summary << ",\"memory_threshold_exceeded\":"
          << (memory_threshold_exceeded ? "true" : "false")
          << '}';
  impl_->WriteLine(summary.str(), output, mirror);

  if (!output || (mirror != nullptr && !*mirror)) {
    return Status(
        StatusCode::IoError, "writing chaos evidence failed");
  }
  if (!passed) {
    return Status(
        StatusCode::CorruptData,
        "seeded chaos invariants or configured thresholds failed");
  }
  return Status::Ok();
}

}  // namespace fastipc::chaos
