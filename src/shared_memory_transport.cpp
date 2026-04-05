#include <fastipc/shared_memory_transport.hpp>

#include "shared_memory_layout.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <ctime>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <linux/futex.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace fastipc {
namespace {

using detail::EndpointMetadata;
using detail::SharedLayout;
using detail::SlotHeader;

static_assert(__atomic_always_lock_free(sizeof(std::uint32_t), nullptr));
static_assert(__atomic_always_lock_free(sizeof(std::uint64_t), nullptr));

enum class Role : std::uint8_t {
  Producer,
  Consumer,
};

std::atomic<std::uint64_t> g_role_token_counter{1};

template <typename T>
[[nodiscard]] T AtomicLoad(const T* address, int order) noexcept {
  return __atomic_load_n(address, order);
}

template <typename T>
void AtomicStore(T* address, T value, int order) noexcept {
  __atomic_store_n(address, value, order);
}

template <typename T>
T AtomicFetchAdd(T* address, T value, int order) noexcept {
  return __atomic_fetch_add(address, value, order);
}

enum class FutexWaitResult : std::uint8_t {
  Woken,
  ValueChanged,
  Interrupted,
  TimedOut,
  Error,
};

struct FutexWaitOutcome {
  FutexWaitResult result{FutexWaitResult::Error};
  int error{0};
};

[[nodiscard]] timespec ToMonotonicAbsoluteTime(
    const Deadline& deadline) noexcept {
  timespec kernel_now{};
  static_cast<void>(::clock_gettime(CLOCK_MONOTONIC, &kernel_now));

  auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(
      deadline.time_point() - Deadline::Clock::now());
  if (remaining < std::chrono::nanoseconds::zero()) {
    remaining = std::chrono::nanoseconds::zero();
  }

  constexpr std::int64_t nanoseconds_per_second = 1'000'000'000;
  const auto remaining_count = remaining.count();
  const auto added_seconds = remaining_count / nanoseconds_per_second;
  const auto added_nanoseconds = remaining_count % nanoseconds_per_second;
  const auto combined_nanoseconds =
      static_cast<std::int64_t>(kernel_now.tv_nsec) + added_nanoseconds;

  timespec absolute{};
  absolute.tv_sec = static_cast<time_t>(
      static_cast<std::int64_t>(kernel_now.tv_sec) + added_seconds +
      combined_nanoseconds / nanoseconds_per_second);
  absolute.tv_nsec =
      static_cast<long>(combined_nanoseconds % nanoseconds_per_second);
  return absolute;
}

[[nodiscard]] FutexWaitOutcome FutexWait(
    std::uint32_t* epoch, std::uint32_t expected,
    const Deadline& deadline) noexcept {
  timespec absolute{};
  const timespec* timeout = nullptr;
  if (!deadline.infinite()) {
    absolute = ToMonotonicAbsoluteTime(deadline);
    timeout = &absolute;
  }

  errno = 0;
  const long result =
      ::syscall(SYS_futex, epoch, FUTEX_WAIT_BITSET, expected, timeout,
                nullptr, FUTEX_BITSET_MATCH_ANY);
  if (result == 0) {
    return {FutexWaitResult::Woken, 0};
  }

  const int error = errno;
  switch (error) {
    case EAGAIN:
      return {FutexWaitResult::ValueChanged, error};
    case EINTR:
      return {FutexWaitResult::Interrupted, error};
    case ETIMEDOUT:
      return {FutexWaitResult::TimedOut, error};
    default:
      return {FutexWaitResult::Error, error};
  }
}

void FutexWake(std::uint32_t* epoch) noexcept {
  static_cast<void>(
      ::syscall(SYS_futex, epoch, FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0));
}

[[nodiscard]] std::uint64_t MonotonicNanoseconds() noexcept {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

[[nodiscard]] std::uint64_t GenerateRoleToken() noexcept {
  const auto sequence =
      g_role_token_counter.fetch_add(1U, std::memory_order_relaxed);
  auto token = MonotonicNanoseconds() ^
               (static_cast<std::uint64_t>(
                    static_cast<std::uint32_t>(::getpid()))
                << 32U) ^
               sequence;
  if (token == 0U) {
    token = sequence | 1U;
  }
  return token;
}

[[nodiscard]] std::chrono::milliseconds HeartbeatInterval(
    std::chrono::milliseconds peer_timeout) noexcept {
  using namespace std::chrono_literals;
  return std::clamp(peer_timeout / 4, 1ms, 250ms);
}

[[nodiscard]] std::uint64_t ProcessStartTicks(pid_t pid) {
  std::ifstream stream("/proc/" + std::to_string(pid) + "/stat");
  std::string line;
  if (!std::getline(stream, line)) {
    return 0;
  }
  const auto command_end = line.rfind(')');
  if (command_end == std::string::npos || command_end + 2U >= line.size()) {
    return 0;
  }

  std::istringstream fields(line.substr(command_end + 2U));
  std::string field;
  for (int field_number = 3; field_number <= 22; ++field_number) {
    if (!(fields >> field)) {
      return 0;
    }
    if (field_number == 22) {
      try {
        return std::stoull(field);
      } catch (...) {
        return 0;
      }
    }
  }
  return 0;
}

[[nodiscard]] bool ProcessIdentityAlive(
    std::int32_t pid, std::uint64_t expected_start_ticks) {
  if (pid <= 0 || expected_start_ticks == 0U) {
    return false;
  }

  errno = 0;
  if (::kill(static_cast<pid_t>(pid), 0) != 0 && errno != EPERM) {
    return false;
  }
  return ProcessStartTicks(static_cast<pid_t>(pid)) == expected_start_ticks;
}

enum class EndpointLiveness : std::uint8_t {
  Vacant,
  Alive,
  ProcessDead,
  HeartbeatExpired,
};

[[nodiscard]] EndpointLiveness InspectEndpoint(
    const EndpointMetadata& metadata,
    std::chrono::milliseconds peer_timeout) {
  const auto pid = AtomicLoad(&metadata.pid, __ATOMIC_ACQUIRE);
  if (pid == 0) {
    return EndpointLiveness::Vacant;
  }
  const auto start_ticks =
      AtomicLoad(&metadata.process_start_ticks, __ATOMIC_ACQUIRE);
  if (!ProcessIdentityAlive(pid, start_ticks)) {
    return EndpointLiveness::ProcessDead;
  }

  const auto heartbeat =
      AtomicLoad(&metadata.heartbeat_monotonic_ns, __ATOMIC_ACQUIRE);
  const auto now = MonotonicNanoseconds();
  const auto timeout_count =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          peer_timeout).count();
  const auto timeout_ns = static_cast<std::uint64_t>(timeout_count);
  if (heartbeat == 0U ||
      (now > heartbeat && now - heartbeat > timeout_ns)) {
    return EndpointLiveness::HeartbeatExpired;
  }
  return EndpointLiveness::Alive;
}

[[nodiscard]] Status PeerStatus(
    const EndpointMetadata& metadata,
    std::chrono::milliseconds peer_timeout) {
  switch (InspectEndpoint(metadata, peer_timeout)) {
    case EndpointLiveness::Vacant:
      return Status(StatusCode::PeerUnavailable,
                    "the peer role is not currently owned");
    case EndpointLiveness::ProcessDead:
      return Status(StatusCode::PeerDead,
                    "the peer process identity is no longer alive");
    case EndpointLiveness::HeartbeatExpired:
      return Status(StatusCode::PeerDead,
                    "the peer heartbeat lease expired");
    case EndpointLiveness::Alive:
      return Status::Ok();
  }
  return Status(StatusCode::PeerDead, "unknown peer liveness state");
}

[[nodiscard]] Deadline ProbeDeadline(
    const Deadline& requested,
    std::chrono::milliseconds peer_timeout) noexcept {
  const auto probe_time = Deadline::Clock::now() + peer_timeout;
  if (!requested.infinite() && requested.time_point() <= probe_time) {
    return requested;
  }
  return Deadline::At(probe_time);
}

[[nodiscard]] Status ErrnoStatus(StatusCode fallback, std::string detail) {
  const int error = errno;
  StatusCode code = fallback;
  if (error == EACCES || error == EPERM) {
    code = StatusCode::PermissionDenied;
  } else if (error == ENOENT) {
    code = StatusCode::NotFound;
  } else if (error == EEXIST) {
    code = StatusCode::AlreadyExists;
  }
  return Status(code, std::move(detail), error);
}

[[nodiscard]] Result<std::string> CanonicalName(std::string_view name) {
  if (name.empty() || name.size() > 200U) {
    return Status(StatusCode::InvalidArgument,
                  "channel name must contain 1 to 200 characters");
  }
  for (const char character : name) {
    const bool valid =
        (character >= 'a' && character <= 'z') ||
        (character >= 'A' && character <= 'Z') ||
        (character >= '0' && character <= '9') || character == '_' ||
        character == '-' || character == '.';
    if (!valid) {
      return Status(StatusCode::InvalidArgument,
                    "channel name contains an unsupported character");
    }
  }
  return std::string("/") + std::string(name);
}

[[nodiscard]] Status ValidatePeerTimeout(const ChannelConfig& config) {
  if (config.peer_timeout <= std::chrono::milliseconds::zero()) {
    return Status(StatusCode::InvalidArgument,
                  "peer_timeout must be positive");
  }
  return Status::Ok();
}

struct Geometry {
  std::size_t segment_bytes{0};
  std::uint32_t slot_stride{0};
  std::uint32_t slots_offset{0};
};

[[nodiscard]] Result<Geometry> ComputeGeometry(const ChannelConfig& config) {
  if (config.slot_count < 2U) {
    return Status(StatusCode::InvalidArgument,
                  "slot_count must be at least two");
  }
  if (config.max_message_size == 0U ||
      config.max_message_size > 16U * 1024U * 1024U) {
    return Status(StatusCode::InvalidArgument,
                  "max_message_size must be between 1 and 16 MiB");
  }
  if ((config.permissions & ~0777U) != 0U) {
    return Status(StatusCode::InvalidArgument,
                  "permissions contain bits outside POSIX mode 0777");
  }

  const std::size_t raw_stride =
      sizeof(SlotHeader) + static_cast<std::size_t>(config.max_message_size);
  const std::size_t aligned_stride =
      detail::AlignUp(raw_stride, detail::kCacheLine);
  if (aligned_stride > std::numeric_limits<std::uint32_t>::max()) {
    return Status(StatusCode::InvalidArgument, "slot stride overflows layout");
  }
  constexpr std::size_t slots_offset =
      detail::AlignUp(sizeof(SharedLayout), detail::kCacheLine);
  if (static_cast<std::size_t>(config.slot_count) >
      (std::numeric_limits<std::size_t>::max() - slots_offset) /
          aligned_stride) {
    return Status(StatusCode::InvalidArgument, "segment size overflows");
  }
  return Geometry{
      slots_offset + static_cast<std::size_t>(config.slot_count) *
                         aligned_stride,
      static_cast<std::uint32_t>(aligned_stride),
      static_cast<std::uint32_t>(slots_offset)};
}

[[nodiscard]] Status ValidateLayout(const SharedLayout& layout,
                                    std::size_t mapped_bytes,
                                    const ChannelConfig& expected) {
  const std::uint32_t state =
      AtomicLoad(&layout.header.init_state, __ATOMIC_ACQUIRE);
  if (state != detail::kInitReady) {
    return Status(StatusCode::LayoutMismatch,
                  "segment initialization is incomplete");
  }
  if (layout.header.magic != detail::kMagic) {
    return Status(StatusCode::LayoutMismatch, "shared-memory magic differs");
  }
  if (layout.header.version_major != detail::kLayoutMajor ||
      layout.header.version_minor != detail::kLayoutMinor) {
    return Status(StatusCode::LayoutMismatch,
                  "shared-memory layout version is incompatible");
  }
  if (layout.header.endian_marker != detail::kEndianMarker) {
    return Status(StatusCode::LayoutMismatch, "byte order is incompatible");
  }
  if (layout.header.header_bytes != sizeof(SharedLayout) ||
      layout.header.segment_bytes != mapped_bytes) {
    return Status(StatusCode::LayoutMismatch,
                  "layout size does not match the mapped object");
  }
  if (layout.queue.slot_count != expected.slot_count ||
      layout.queue.max_message_size != expected.max_message_size) {
    return Status(StatusCode::LayoutMismatch,
                  "channel geometry differs from caller configuration");
  }

  const std::size_t expected_end =
      static_cast<std::size_t>(layout.queue.slots_offset) +
      static_cast<std::size_t>(layout.queue.slot_count) *
          static_cast<std::size_t>(layout.queue.slot_stride);
  if (layout.queue.slots_offset < sizeof(SharedLayout) ||
      layout.queue.slot_stride <
          sizeof(SlotHeader) + layout.queue.max_message_size ||
      expected_end != mapped_bytes) {
    return Status(StatusCode::LayoutMismatch,
                  "queue offsets or stride are invalid");
  }
  return Status::Ok();
}

[[nodiscard]] SlotHeader* SlotAt(SharedLayout* layout,
                                 std::uint64_t cursor) noexcept {
  auto* base = reinterpret_cast<std::byte*>(layout);
  const auto index =
      cursor % static_cast<std::uint64_t>(layout->queue.slot_count);
  const auto offset =
      static_cast<std::size_t>(layout->queue.slots_offset) +
      static_cast<std::size_t>(index) *
          static_cast<std::size_t>(layout->queue.slot_stride);
  return reinterpret_cast<SlotHeader*>(base + offset);
}

}  // namespace

struct SharedMemoryTransport::Impl {
  int descriptor{-1};
  void* mapping{MAP_FAILED};
  std::size_t mapped_bytes{0};
  SharedLayout* layout{nullptr};
  Role role{Role::Producer};
  std::string object_name;
  bool owner{false};
  bool role_claimed{false};
  bool unlink_on_owner_close{false};
  std::atomic<bool> closed{false};
  std::chrono::milliseconds peer_timeout{1000};
  std::uint64_t process_start_ticks{0};
  std::uint64_t role_token{0};
  std::atomic<std::uint64_t> observed_generation{0};
  std::mutex heartbeat_mutex;
  std::condition_variable heartbeat_cv;
  std::jthread heartbeat_thread;

  ~Impl() { Close(); }

  [[nodiscard]] EndpointMetadata& RoleMetadata() noexcept {
    return role == Role::Producer ? layout->producer : layout->consumer;
  }

  [[nodiscard]] bool OwnsRole() const noexcept {
    if (layout == nullptr || !role_claimed) {
      return false;
    }
    const auto& metadata =
        role == Role::Producer ? layout->producer : layout->consumer;
    return AtomicLoad(&metadata.pid, __ATOMIC_ACQUIRE) ==
               static_cast<std::int32_t>(::getpid()) &&
           AtomicLoad(&metadata.process_start_ticks, __ATOMIC_ACQUIRE) ==
               process_start_ticks &&
           AtomicLoad(&metadata.role_token, __ATOMIC_ACQUIRE) == role_token;
  }

  [[nodiscard]] Status StartHeartbeat() {
    const auto interval = HeartbeatInterval(peer_timeout);
    try {
      heartbeat_thread = std::jthread(
          [this, interval](std::stop_token stop_token) {
            for (;;) {
              if (stop_token.stop_requested() ||
                  closed.load(std::memory_order_acquire) ||
                  !OwnsRole()) {
                return;
              }
              AtomicStore(&RoleMetadata().heartbeat_monotonic_ns,
                          MonotonicNanoseconds(), __ATOMIC_RELEASE);

              std::unique_lock lock(heartbeat_mutex);
              heartbeat_cv.wait_for(lock, interval, [this, &stop_token] {
                return stop_token.stop_requested() ||
                       closed.load(std::memory_order_acquire);
              });
            }
          });
    } catch (const std::system_error& error) {
      return Status(StatusCode::IoError,
                    "failed to start endpoint heartbeat thread",
                    error.code().value());
    }
    return Status::Ok();
  }

  void Close() noexcept {
    if (closed.exchange(true, std::memory_order_acq_rel)) {
      return;
    }

    heartbeat_thread.request_stop();
    heartbeat_cv.notify_all();
    if (heartbeat_thread.joinable()) {
      heartbeat_thread.join();
    }

    bool may_unlink = owner && !role_claimed;
    if (layout != nullptr && role_claimed) {
      const bool owns_role = OwnsRole();
      if (owns_role) {
        auto& metadata = RoleMetadata();
        AtomicStore(&metadata.state, std::uint32_t{0}, __ATOMIC_RELAXED);
        AtomicStore(&metadata.pid, std::int32_t{0}, __ATOMIC_RELEASE);
        may_unlink = owner;
      }

      auto* epoch = role == Role::Producer
                        ? &layout->producer_cursor.data_epoch
                        : &layout->consumer_cursor.space_epoch;
      AtomicFetchAdd(epoch, std::uint32_t{1}, __ATOMIC_RELEASE);
      FutexWake(epoch);
    }

    if (mapping != MAP_FAILED) {
      ::munmap(mapping, mapped_bytes);
      mapping = MAP_FAILED;
      layout = nullptr;
    }
    if (descriptor >= 0) {
      ::close(descriptor);
      descriptor = -1;
    }
    if (may_unlink && unlink_on_owner_close && !object_name.empty()) {
      ::shm_unlink(object_name.c_str());
    }
  }
};

[[nodiscard]] Result<std::unique_ptr<SharedMemoryTransport::Impl>>
SharedMemoryTransport::CreateProducerImpl(const ChannelConfig& config) {
  const auto timeout_validation = ValidatePeerTimeout(config);
  if (!timeout_validation) {
    return timeout_validation;
  }
  auto name_result = CanonicalName(config.name);
  if (!name_result) {
    return name_result.status();
  }
  auto geometry_result = ComputeGeometry(config);
  if (!geometry_result) {
    return geometry_result.status();
  }
  const auto geometry = geometry_result.value();
  const std::string object_name = std::move(name_result).take_value();

  bool created = true;
  int descriptor =
      ::shm_open(object_name.c_str(), O_CREAT | O_EXCL | O_RDWR,
                 static_cast<mode_t>(config.permissions));
  if (descriptor < 0 && errno == EEXIST) {
    created = false;
    descriptor = ::shm_open(object_name.c_str(), O_RDWR, 0);
  }
  if (descriptor < 0) {
    return ErrnoStatus(StatusCode::IoError,
                       "failed to create or open shared-memory object");
  }

  if (!created) {
    struct stat object_stat {};
    if (::fstat(descriptor, &object_stat) != 0 ||
        object_stat.st_size < static_cast<off_t>(sizeof(SharedLayout))) {
      const auto status =
          ErrnoStatus(StatusCode::LayoutMismatch,
                      "existing shared-memory object is smaller than header");
      ::close(descriptor);
      return status;
    }

    const auto mapped_bytes = static_cast<std::size_t>(object_stat.st_size);
    void* mapping = ::mmap(nullptr, mapped_bytes, PROT_READ | PROT_WRITE,
                           MAP_SHARED, descriptor, 0);
    if (mapping == MAP_FAILED) {
      const auto status =
          ErrnoStatus(StatusCode::IoError,
                      "failed to map existing shared-memory object");
      ::close(descriptor);
      return status;
    }

    auto impl = std::make_unique<SharedMemoryTransport::Impl>();
    impl->descriptor = descriptor;
    impl->mapping = mapping;
    impl->mapped_bytes = mapped_bytes;
    impl->layout = static_cast<SharedLayout*>(mapping);
    impl->role = Role::Producer;
    impl->object_name = object_name;
    impl->unlink_on_owner_close = config.unlink_on_owner_close;
    impl->peer_timeout = config.peer_timeout;

    const auto validation =
        ValidateLayout(*impl->layout, mapped_bytes, config);
    if (!validation) {
      return validation;
    }
    if (::flock(descriptor, LOCK_EX) != 0) {
      return ErrnoStatus(StatusCode::IoError,
                         "failed to lock producer role metadata");
    }

    auto& metadata = impl->layout->producer;
    const auto liveness =
        InspectEndpoint(metadata, config.peer_timeout);
    if (liveness == EndpointLiveness::Alive) {
      static_cast<void>(::flock(descriptor, LOCK_UN));
      return Status(StatusCode::RoleConflict,
                    "a live producer already owns this channel");
    }

    const auto previous_generation =
        AtomicLoad(&impl->layout->header.generation, __ATOMIC_ACQUIRE);
    std::uint64_t next_generation = previous_generation + 1U;
    if (next_generation == 0U) {
      next_generation = 1U;
    }
    AtomicStore(&impl->layout->header.generation, next_generation,
                __ATOMIC_RELEASE);

    const auto process_start_ticks = ProcessStartTicks(::getpid());
    const auto role_token = GenerateRoleToken();
    AtomicStore(&metadata.process_start_ticks, process_start_ticks,
                __ATOMIC_RELAXED);
    AtomicStore(&metadata.generation, next_generation, __ATOMIC_RELAXED);
    AtomicStore(&metadata.role_token, role_token, __ATOMIC_RELAXED);
    AtomicStore(&metadata.heartbeat_monotonic_ns, MonotonicNanoseconds(),
                __ATOMIC_RELAXED);
    AtomicStore(&metadata.operation_sequence, std::uint64_t{0},
                __ATOMIC_RELAXED);
    AtomicStore(&metadata.state, std::uint32_t{1}, __ATOMIC_RELAXED);
    AtomicStore(&metadata.pid, static_cast<std::int32_t>(::getpid()),
                __ATOMIC_RELEASE);

    impl->process_start_ticks = process_start_ticks;
    impl->role_token = role_token;
    impl->role_claimed = true;
    impl->owner = true;
    impl->observed_generation.store(next_generation,
                                    std::memory_order_relaxed);
    static_cast<void>(::flock(descriptor, LOCK_UN));

    AtomicFetchAdd(&impl->layout->producer_cursor.data_epoch,
                   std::uint32_t{1}, __ATOMIC_RELEASE);
    AtomicFetchAdd(&impl->layout->consumer_cursor.space_epoch,
                   std::uint32_t{1}, __ATOMIC_RELEASE);
    FutexWake(&impl->layout->producer_cursor.data_epoch);
    FutexWake(&impl->layout->consumer_cursor.space_epoch);
    const auto heartbeat_status = impl->StartHeartbeat();
    if (!heartbeat_status) {
      return heartbeat_status;
    }
    return impl;
  }

  if (::ftruncate(descriptor, static_cast<off_t>(geometry.segment_bytes)) != 0) {
    const auto status =
        ErrnoStatus(StatusCode::IoError, "failed to size shared-memory object");
    ::close(descriptor);
    ::shm_unlink(object_name.c_str());
    return status;
  }

  void* mapping = ::mmap(nullptr, geometry.segment_bytes,
                         PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
  if (mapping == MAP_FAILED) {
    const auto status =
        ErrnoStatus(StatusCode::IoError, "failed to map shared-memory object");
    ::close(descriptor);
    ::shm_unlink(object_name.c_str());
    return status;
  }

  auto impl = std::make_unique<SharedMemoryTransport::Impl>();
  impl->descriptor = descriptor;
  impl->mapping = mapping;
  impl->mapped_bytes = geometry.segment_bytes;
  impl->layout = static_cast<SharedLayout*>(mapping);
  impl->role = Role::Producer;
  impl->object_name = object_name;
  impl->owner = true;
  impl->unlink_on_owner_close = config.unlink_on_owner_close;
  impl->peer_timeout = config.peer_timeout;

  std::memset(mapping, 0, geometry.segment_bytes);
  auto& layout = *impl->layout;
  layout.header.magic = detail::kMagic;
  layout.header.version_major = detail::kLayoutMajor;
  layout.header.version_minor = detail::kLayoutMinor;
  layout.header.header_bytes = sizeof(SharedLayout);
  layout.header.segment_bytes = geometry.segment_bytes;
  layout.header.created_monotonic_ns = MonotonicNanoseconds();
  layout.header.generation =
      layout.header.created_monotonic_ns ^
      static_cast<std::uint64_t>(static_cast<std::uint32_t>(::getpid()));
  if (layout.header.generation == 0U) {
    layout.header.generation = 1U;
  }
  layout.header.endian_marker = detail::kEndianMarker;
  AtomicStore(&layout.header.init_state, detail::kInitInitializing,
              __ATOMIC_RELAXED);

  layout.queue.slot_count = config.slot_count;
  layout.queue.max_message_size = config.max_message_size;
  layout.queue.slot_stride = geometry.slot_stride;
  layout.queue.slots_offset = geometry.slots_offset;

  const auto process_start_ticks = ProcessStartTicks(::getpid());
  const auto role_token = GenerateRoleToken();
  layout.producer.process_start_ticks = process_start_ticks;
  layout.producer.generation = layout.header.generation;
  layout.producer.role_token = role_token;
  layout.producer.heartbeat_monotonic_ns = MonotonicNanoseconds();
  layout.producer.state = 1U;
  AtomicStore(&layout.producer.pid, static_cast<std::int32_t>(::getpid()),
              __ATOMIC_RELEASE);

  impl->process_start_ticks = process_start_ticks;
  impl->role_token = role_token;
  impl->role_claimed = true;
  impl->observed_generation.store(layout.header.generation,
                                  std::memory_order_relaxed);
  AtomicStore(&layout.header.init_state, detail::kInitReady, __ATOMIC_RELEASE);
  const auto heartbeat_status = impl->StartHeartbeat();
  if (!heartbeat_status) {
    return heartbeat_status;
  }
  return impl;
}

[[nodiscard]] Result<std::unique_ptr<SharedMemoryTransport::Impl>>
SharedMemoryTransport::OpenConsumerImpl(const ChannelConfig& config) {
  const auto timeout_validation = ValidatePeerTimeout(config);
  if (!timeout_validation) {
    return timeout_validation;
  }
  auto name_result = CanonicalName(config.name);
  if (!name_result) {
    return name_result.status();
  }
  const std::string object_name = std::move(name_result).take_value();
  const int descriptor = ::shm_open(object_name.c_str(), O_RDWR, 0);
  if (descriptor < 0) {
    const int error = errno;
    if (error == ENOENT) {
      return Status(StatusCode::PeerUnavailable,
                    "producer shared-memory object is unavailable", error);
    }
    return ErrnoStatus(StatusCode::PeerUnavailable,
                       "failed to open producer shared-memory object");
  }

  struct stat object_stat {};
  if (::fstat(descriptor, &object_stat) != 0 ||
      object_stat.st_size < static_cast<off_t>(sizeof(SharedLayout))) {
    const auto status =
        ErrnoStatus(StatusCode::LayoutMismatch,
                    "shared-memory object is smaller than the header");
    ::close(descriptor);
    return status;
  }
  const auto mapped_bytes = static_cast<std::size_t>(object_stat.st_size);
  void* mapping = ::mmap(nullptr, mapped_bytes, PROT_READ | PROT_WRITE,
                         MAP_SHARED, descriptor, 0);
  if (mapping == MAP_FAILED) {
    const auto status =
        ErrnoStatus(StatusCode::IoError, "failed to map shared-memory object");
    ::close(descriptor);
    return status;
  }

  auto impl = std::make_unique<SharedMemoryTransport::Impl>();
  impl->descriptor = descriptor;
  impl->mapping = mapping;
  impl->mapped_bytes = mapped_bytes;
  impl->layout = static_cast<SharedLayout*>(mapping);
  impl->role = Role::Consumer;
  impl->object_name = object_name;
  impl->peer_timeout = config.peer_timeout;

  const auto validation = ValidateLayout(*impl->layout, mapped_bytes, config);
  if (!validation) {
    return validation;
  }

  if (::flock(descriptor, LOCK_EX) != 0) {
    return ErrnoStatus(StatusCode::IoError,
                       "failed to lock shared-memory role metadata");
  }
  auto& metadata = impl->layout->consumer;
  const auto liveness =
      InspectEndpoint(metadata, config.peer_timeout);
  if (liveness == EndpointLiveness::Alive) {
    ::flock(descriptor, LOCK_UN);
    return Status(StatusCode::RoleConflict,
                  "a live consumer already owns this channel");
  }

  const auto process_start_ticks = ProcessStartTicks(::getpid());
  const auto role_token = GenerateRoleToken();
  AtomicStore(&metadata.process_start_ticks, process_start_ticks,
              __ATOMIC_RELAXED);
  AtomicStore(&metadata.generation, impl->layout->header.generation,
              __ATOMIC_RELAXED);
  AtomicStore(&metadata.role_token, role_token, __ATOMIC_RELAXED);
  AtomicStore(&metadata.heartbeat_monotonic_ns, MonotonicNanoseconds(),
              __ATOMIC_RELAXED);
  AtomicStore(&metadata.state, std::uint32_t{1}, __ATOMIC_RELAXED);
  AtomicStore(&metadata.pid, static_cast<std::int32_t>(::getpid()),
              __ATOMIC_RELEASE);
  impl->process_start_ticks = process_start_ticks;
  impl->role_token = role_token;
  impl->role_claimed = true;
  ::flock(descriptor, LOCK_UN);

  impl->observed_generation.store(impl->layout->header.generation,
                                  std::memory_order_relaxed);
  const auto heartbeat_status = impl->StartHeartbeat();
  if (!heartbeat_status) {
    return heartbeat_status;
  }
  return impl;
}

SharedMemoryTransport::SharedMemoryTransport(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

SharedMemoryTransport::~SharedMemoryTransport() = default;

Result<std::unique_ptr<SharedMemoryTransport>>
SharedMemoryTransport::CreateProducer(const ChannelConfig& config) {
  auto impl = CreateProducerImpl(config);
  if (!impl) {
    return impl.status();
  }
  return std::unique_ptr<SharedMemoryTransport>(
      new SharedMemoryTransport(std::move(impl).take_value()));
}

Result<std::unique_ptr<SharedMemoryTransport>>
SharedMemoryTransport::OpenConsumer(const ChannelConfig& config) {
  auto impl = OpenConsumerImpl(config);
  if (!impl) {
    return impl.status();
  }
  return std::unique_ptr<SharedMemoryTransport>(
      new SharedMemoryTransport(std::move(impl).take_value()));
}

Status SharedMemoryTransport::Send(std::span<const std::byte> message,
                                   SendOptions options) {
  if (!impl_ || impl_->closed.load(std::memory_order_acquire)) {
    return Status(StatusCode::Closed);
  }
  if (impl_->role != Role::Producer) {
    return Status(StatusCode::RoleConflict,
                  "consumer endpoint cannot send");
  }
  if (!impl_->OwnsRole()) {
    return Status(StatusCode::StaleGeneration,
                  "producer role was reclaimed by another endpoint");
  }

  auto& layout = *impl_->layout;
  const auto current_generation =
      AtomicLoad(&layout.header.generation, __ATOMIC_ACQUIRE);
  if (current_generation !=
      impl_->observed_generation.load(std::memory_order_acquire)) {
    return Status(StatusCode::StaleGeneration,
                  "producer was fenced by a newer channel generation");
  }
  if (message.size() > layout.queue.max_message_size) {
    return Status(StatusCode::MessageTooLarge);
  }

  std::uint64_t head = 0;
  for (;;) {
    head = AtomicLoad(&layout.producer_cursor.head, __ATOMIC_RELAXED);
    const auto tail =
        AtomicLoad(&layout.consumer_cursor.tail, __ATOMIC_ACQUIRE);
    if (head - tail < layout.queue.slot_count) {
      break;
    }

    if (options.policy == BackpressurePolicy::Drop) {
      AtomicFetchAdd(&layout.producer_cursor.dropped_messages,
                     std::uint64_t{1}, __ATOMIC_RELAXED);
      return Status(StatusCode::Dropped);
    }
    if (options.deadline.expired()) {
      AtomicFetchAdd(&layout.producer_cursor.timeout_count,
                     std::uint64_t{1}, __ATOMIC_RELAXED);
      return Status(StatusCode::Timeout);
    }
    const auto peer_status = PeerStatus(layout.consumer, impl_->peer_timeout);
    if (!peer_status) {
      return peer_status;
    }

    const auto expected_epoch =
        AtomicLoad(&layout.consumer_cursor.space_epoch, __ATOMIC_ACQUIRE);
    const auto rechecked_head =
        AtomicLoad(&layout.producer_cursor.head, __ATOMIC_RELAXED);
    const auto rechecked_tail =
        AtomicLoad(&layout.consumer_cursor.tail, __ATOMIC_ACQUIRE);
    if (rechecked_head - rechecked_tail < layout.queue.slot_count) {
      continue;
    }

    const auto wait =
        FutexWait(&layout.consumer_cursor.space_epoch, expected_epoch,
                  ProbeDeadline(options.deadline, impl_->peer_timeout));
    if (wait.result == FutexWaitResult::TimedOut) {
      continue;
    }
    if (wait.result == FutexWaitResult::Error) {
      return Status(StatusCode::IoError, "futex wait for queue space failed",
                    wait.error);
    }
  }

  SlotHeader* slot = SlotAt(&layout, head);
  slot->length = static_cast<std::uint32_t>(message.size());
  slot->sequence = head + 1U;
  auto* payload = reinterpret_cast<std::byte*>(slot) + sizeof(SlotHeader);
  if (!message.empty()) {
    std::memcpy(payload, message.data(), message.size());
  }

  AtomicStore(&layout.producer_cursor.head, head + 1U, __ATOMIC_RELEASE);
  AtomicFetchAdd(&layout.producer_cursor.data_epoch, std::uint32_t{1},
                 __ATOMIC_RELEASE);
  FutexWake(&layout.producer_cursor.data_epoch);
  AtomicFetchAdd(&layout.producer_cursor.sent_messages, std::uint64_t{1},
                 __ATOMIC_RELAXED);
  AtomicStore(&layout.producer.heartbeat_monotonic_ns, MonotonicNanoseconds(),
              __ATOMIC_RELEASE);
  AtomicFetchAdd(&layout.producer.operation_sequence, std::uint64_t{1},
                 __ATOMIC_RELAXED);
  return Status::Ok();
}

Result<std::size_t> SharedMemoryTransport::Receive(
    std::span<std::byte> destination, Deadline deadline) {
  if (!impl_ || impl_->closed.load(std::memory_order_acquire)) {
    return Status(StatusCode::Closed);
  }
  if (impl_->role != Role::Consumer) {
    return Status(StatusCode::RoleConflict,
                  "producer endpoint cannot receive");
  }
  if (!impl_->OwnsRole()) {
    return Status(StatusCode::StaleGeneration,
                  "consumer role was reclaimed by another endpoint");
  }

  auto& layout = *impl_->layout;
  const auto current_generation =
      AtomicLoad(&layout.header.generation, __ATOMIC_ACQUIRE);
  if (current_generation !=
      impl_->observed_generation.load(std::memory_order_acquire)) {
    impl_->observed_generation.store(current_generation,
                                     std::memory_order_release);
    AtomicStore(&layout.consumer.generation, current_generation,
                __ATOMIC_RELEASE);
  }
  std::uint64_t tail = 0;
  for (;;) {
    tail = AtomicLoad(&layout.consumer_cursor.tail, __ATOMIC_RELAXED);
    const auto head =
        AtomicLoad(&layout.producer_cursor.head, __ATOMIC_ACQUIRE);
    if (tail != head) {
      break;
    }
    if (deadline.expired()) {
      AtomicFetchAdd(&layout.consumer_cursor.timeout_count,
                     std::uint64_t{1}, __ATOMIC_RELAXED);
      return Status(StatusCode::Timeout);
    }
    const auto peer_status = PeerStatus(layout.producer, impl_->peer_timeout);
    if (!peer_status) {
      return peer_status;
    }

    const auto expected_epoch =
        AtomicLoad(&layout.producer_cursor.data_epoch, __ATOMIC_ACQUIRE);
    const auto rechecked_tail =
        AtomicLoad(&layout.consumer_cursor.tail, __ATOMIC_RELAXED);
    const auto rechecked_head =
        AtomicLoad(&layout.producer_cursor.head, __ATOMIC_ACQUIRE);
    if (rechecked_tail != rechecked_head) {
      continue;
    }

    const auto wait =
        FutexWait(&layout.producer_cursor.data_epoch, expected_epoch,
                  ProbeDeadline(deadline, impl_->peer_timeout));
    if (wait.result == FutexWaitResult::TimedOut) {
      continue;
    }
    if (wait.result == FutexWaitResult::Error) {
      return Status(StatusCode::IoError, "futex wait for queue data failed",
                    wait.error);
    }
  }

  SlotHeader* slot = SlotAt(&layout, tail);
  const std::uint32_t length = slot->length;
  if (length > layout.queue.max_message_size) {
    AtomicFetchAdd(&layout.consumer_cursor.corrupt_messages,
                   std::uint64_t{1}, __ATOMIC_RELAXED);
    return Status(StatusCode::CorruptData,
                  "slot length exceeds configured maximum");
  }
  if (destination.size() < length) {
    return Status(StatusCode::BufferTooSmall);
  }

  const auto* payload =
      reinterpret_cast<const std::byte*>(slot) + sizeof(SlotHeader);
  if (length != 0U) {
    std::memcpy(destination.data(), payload, length);
  }

  AtomicStore(&layout.consumer_cursor.tail, tail + 1U, __ATOMIC_RELEASE);
  AtomicFetchAdd(&layout.consumer_cursor.space_epoch, std::uint32_t{1},
                 __ATOMIC_RELEASE);
  FutexWake(&layout.consumer_cursor.space_epoch);
  AtomicFetchAdd(&layout.consumer_cursor.received_messages, std::uint64_t{1},
                 __ATOMIC_RELAXED);
  AtomicStore(&layout.consumer.heartbeat_monotonic_ns, MonotonicNanoseconds(),
              __ATOMIC_RELEASE);
  AtomicFetchAdd(&layout.consumer.operation_sequence, std::uint64_t{1},
                 __ATOMIC_RELAXED);
  return static_cast<std::size_t>(length);
}

TransportStats SharedMemoryTransport::Stats() const noexcept {
  TransportStats stats;
  if (!impl_ || impl_->closed.load(std::memory_order_acquire) ||
      impl_->layout == nullptr) {
    return stats;
  }
  const auto& layout = *impl_->layout;
  stats.sent_messages =
      AtomicLoad(&layout.producer_cursor.sent_messages, __ATOMIC_RELAXED);
  stats.received_messages =
      AtomicLoad(&layout.consumer_cursor.received_messages, __ATOMIC_RELAXED);
  stats.dropped_messages =
      AtomicLoad(&layout.producer_cursor.dropped_messages, __ATOMIC_RELAXED);
  stats.send_timeouts =
      AtomicLoad(&layout.producer_cursor.timeout_count, __ATOMIC_RELAXED);
  stats.receive_timeouts =
      AtomicLoad(&layout.consumer_cursor.timeout_count, __ATOMIC_RELAXED);
  stats.corrupt_messages =
      AtomicLoad(&layout.consumer_cursor.corrupt_messages, __ATOMIC_RELAXED);
  return stats;
}

void SharedMemoryTransport::Close() noexcept {
  if (impl_) {
    impl_->Close();
  }
}

std::uint64_t SharedMemoryTransport::generation() const noexcept {
  return impl_ ? impl_->observed_generation.load(std::memory_order_acquire)
               : 0U;
}

}  // namespace fastipc
