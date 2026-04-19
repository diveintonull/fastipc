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
using detail::SlotOwnerRole;
using detail::SlotState;

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

template <typename T>
[[nodiscard]] bool AtomicCompareExchange(
    T* address, T* expected, T desired, int success_order,
    int failure_order) noexcept {
  return __atomic_compare_exchange_n(
      address, expected, desired, false, success_order, failure_order);
}

[[nodiscard]] constexpr std::uint32_t StateValue(SlotState state) noexcept {
  return static_cast<std::uint32_t>(state);
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

[[nodiscard]] EndpointLiveness InspectEndpointForWait(
    const EndpointMetadata& metadata,
    std::chrono::milliseconds peer_timeout,
    std::atomic<std::uint64_t>& next_identity_probe_ns) {
  const auto pid = AtomicLoad(&metadata.pid, __ATOMIC_ACQUIRE);
  if (pid == 0) {
    return EndpointLiveness::Vacant;
  }

  const auto heartbeat =
      AtomicLoad(&metadata.heartbeat_monotonic_ns, __ATOMIC_ACQUIRE);
  const auto now = MonotonicNanoseconds();
  const auto timeout_count =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          peer_timeout).count();
  const auto timeout_ns = static_cast<std::uint64_t>(timeout_count);
  const bool heartbeat_expired =
      heartbeat == 0U ||
      (now > heartbeat && now - heartbeat > timeout_ns);
  const bool identity_probe_due =
      now >= next_identity_probe_ns.load(std::memory_order_relaxed);
  if (!heartbeat_expired && !identity_probe_due) {
    return EndpointLiveness::Alive;
  }

  const auto start_ticks =
      AtomicLoad(&metadata.process_start_ticks, __ATOMIC_ACQUIRE);
  if (!ProcessIdentityAlive(pid, start_ticks)) {
    return EndpointLiveness::ProcessDead;
  }
  if (heartbeat_expired) {
    return EndpointLiveness::HeartbeatExpired;
  }

  const auto probe_interval_count =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          HeartbeatInterval(peer_timeout)).count();
  next_identity_probe_ns.store(
      now + static_cast<std::uint64_t>(probe_interval_count),
      std::memory_order_relaxed);
  return EndpointLiveness::Alive;
}

[[nodiscard]] Status PeerStatus(
    const EndpointMetadata& metadata,
    std::chrono::milliseconds peer_timeout,
    std::atomic<std::uint64_t>& next_identity_probe_ns) {
  switch (InspectEndpointForWait(
      metadata, peer_timeout, next_identity_probe_ns)) {
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
  const auto probe_time =
      Deadline::Clock::now() + HeartbeatInterval(peer_timeout);
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

[[nodiscard]] Status ValidateChannelConfig(const ChannelConfig& config) {
  if (config.peer_timeout <= std::chrono::milliseconds::zero()) {
    return Status(StatusCode::InvalidArgument,
                  "peer_timeout must be positive");
  }
  constexpr std::uint32_t maximum_active_spin_count = 65'536U;
  if (config.active_spin_count > maximum_active_spin_count) {
    return Status(StatusCode::InvalidArgument,
                  "active_spin_count must not exceed 65536");
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

[[nodiscard]] std::byte* SlotPayload(SlotHeader* slot) noexcept {
  return reinterpret_cast<std::byte*>(slot) + sizeof(SlotHeader);
}

[[nodiscard]] SlotState LoadSlotState(
    const SlotHeader& slot) noexcept {
  return static_cast<SlotState>(
      AtomicLoad(&slot.state, __ATOMIC_ACQUIRE));
}

[[nodiscard]] bool TransitionSlot(
    SlotHeader& slot, SlotState expected_state,
    SlotState desired_state) noexcept {
  auto expected = StateValue(expected_state);
  return AtomicCompareExchange(
      &slot.state, &expected, StateValue(desired_state),
      __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

[[nodiscard]] bool TryAdvanceCursor(
    std::uint64_t* cursor, std::uint64_t expected) noexcept {
  return AtomicCompareExchange(
      cursor, &expected, expected + 1U,
      __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

[[nodiscard]] std::uint64_t NextNonZero(
    std::uint64_t value) noexcept {
  ++value;
  return value == 0U ? 1U : value;
}

[[nodiscard]] bool SlotMatches(
    const SlotHeader& slot, SlotState expected_state,
    std::uint64_t chunk_generation,
    std::uint64_t owner_generation,
    std::uint64_t owner_role_token,
    std::uint64_t owner_channel_generation) noexcept {
  if (LoadSlotState(slot) != expected_state) {
    return false;
  }
  return slot.chunk_generation == chunk_generation &&
         slot.owner_generation == owner_generation &&
         slot.owner_role_token == owner_role_token &&
         slot.owner_channel_generation == owner_channel_generation;
}

[[nodiscard]] bool SlotOwnerProcessAlive(
    const SlotHeader& slot, std::int32_t fallback_pid,
    std::uint64_t fallback_start_ticks) {
  const auto pid =
      slot.owner_pid != 0 ? slot.owner_pid : fallback_pid;
  const auto start_ticks =
      slot.owner_process_start_ticks != 0U
          ? slot.owner_process_start_ticks
          : fallback_start_ticks;
  return ProcessIdentityAlive(pid, start_ticks);
}

[[nodiscard]] bool SlotOwnedByLiveEndpoint(
    const SlotHeader& slot, const EndpointMetadata& metadata,
    SlotOwnerRole expected_role) {
  if (slot.owner_role != static_cast<std::uint32_t>(expected_role) ||
      !ProcessIdentityAlive(
          slot.owner_pid, slot.owner_process_start_ticks)) {
    return false;
  }
  const auto metadata_pid =
      AtomicLoad(&metadata.pid, __ATOMIC_ACQUIRE);
  if (metadata_pid != slot.owner_pid) {
    return false;
  }
  return AtomicLoad(
             &metadata.process_start_ticks,
             __ATOMIC_ACQUIRE) ==
             slot.owner_process_start_ticks &&
         AtomicLoad(&metadata.role_token, __ATOMIC_ACQUIRE) ==
             slot.owner_role_token &&
         AtomicLoad(&metadata.state, __ATOMIC_ACQUIRE) != 0U;
}

void CpuRelax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
  __asm__ __volatile__("yield");
#else
  std::atomic_signal_fence(std::memory_order_acq_rel);
#endif
}

[[nodiscard]] bool SpinForSpace(
    SharedLayout& layout, std::uint32_t spin_count,
    std::uint64_t* head) noexcept {
  for (std::uint32_t iteration = 0; iteration < spin_count; ++iteration) {
    CpuRelax();
    *head = AtomicLoad(&layout.producer_cursor.head, __ATOMIC_RELAXED);
    const auto tail =
        AtomicLoad(&layout.consumer_cursor.tail, __ATOMIC_ACQUIRE);
    if (*head - tail < layout.queue.slot_count) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool SpinForData(
    SharedLayout& layout, std::uint32_t spin_count,
    std::uint64_t* tail) noexcept {
  for (std::uint32_t iteration = 0; iteration < spin_count; ++iteration) {
    CpuRelax();
    *tail = AtomicLoad(&layout.consumer_cursor.tail, __ATOMIC_RELAXED);
    const auto head =
        AtomicLoad(&layout.producer_cursor.head, __ATOMIC_ACQUIRE);
    if (*tail != head) {
      return true;
    }
  }
  return false;
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
  bool close_complete{false};
  std::chrono::milliseconds peer_timeout{1000};
  std::uint32_t active_spin_count{0};
  std::uint64_t process_start_ticks{0};
  std::uint64_t role_token{0};
  std::int32_t replaced_process_id{0};
  std::uint64_t replaced_process_start_ticks{0U};
  std::atomic<std::uint64_t> observed_generation{0};
  std::atomic<std::uint64_t> next_peer_identity_probe_ns{0};
  std::mutex zero_copy_mutex;
  std::mutex operation_mutex;
  std::condition_variable operation_cv;
  std::size_t active_operations{0U};
  std::mutex heartbeat_mutex;
  std::condition_variable heartbeat_cv;
  std::jthread heartbeat_thread;

  class OperationLease {
   public:
    explicit OperationLease(Impl& owner) : owner_(&owner) {
      std::lock_guard lock(owner_->operation_mutex);
      if (!owner_->closed.load(std::memory_order_relaxed)) {
        ++owner_->active_operations;
        acquired_ = true;
      }
    }

    ~OperationLease() {
      if (!acquired_) {
        return;
      }
      bool notify = false;
      {
        std::lock_guard lock(owner_->operation_mutex);
        --owner_->active_operations;
        notify = owner_->active_operations == 0U;
      }
      if (notify) {
        owner_->operation_cv.notify_all();
      }
    }

    OperationLease(const OperationLease&) = delete;
    OperationLease& operator=(const OperationLease&) = delete;

    [[nodiscard]] bool acquired() const noexcept { return acquired_; }

   private:
    Impl* owner_;
    bool acquired_{false};
  };

  ~Impl() {
    Close();
    if (mapping != MAP_FAILED) {
      static_cast<void>(::munmap(mapping, mapped_bytes));
      mapping = MAP_FAILED;
      layout = nullptr;
    }
    if (descriptor >= 0) {
      static_cast<void>(::close(descriptor));
      descriptor = -1;
    }
  }

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

  void WakeData() noexcept {
    AtomicFetchAdd(&layout->producer_cursor.data_epoch,
                   std::uint32_t{1}, __ATOMIC_RELEASE);
    FutexWake(&layout->producer_cursor.data_epoch);
  }

  void WakeSpace() noexcept {
    AtomicFetchAdd(&layout->consumer_cursor.space_epoch,
                   std::uint32_t{1}, __ATOMIC_RELEASE);
    FutexWake(&layout->consumer_cursor.space_epoch);
  }

  [[nodiscard]] Status RecoverProducerHeadSlot() {
    const auto head =
        AtomicLoad(&layout->producer_cursor.head, __ATOMIC_RELAXED);
    auto& slot = *SlotAt(layout, head);
    const auto state = LoadSlotState(slot);
    switch (state) {
      case SlotState::Free:
        return Status::Ok();
      case SlotState::Published:
        if (slot.sequence != head + 1U ||
            slot.length > layout->queue.max_message_size) {
          return Status(StatusCode::CorruptData,
                        "published chunk cannot complete cursor recovery");
        }
        if (!TryAdvanceCursor(
                &layout->producer_cursor.head, head)) {
          return Status::Ok();
        }
        AtomicFetchAdd(&layout->producer_cursor.sent_messages,
                       std::uint64_t{1}, __ATOMIC_RELAXED);
        AtomicFetchAdd(&layout->producer_cursor.zero_copy_publishes,
                       std::uint64_t{1}, __ATOMIC_RELAXED);
        AtomicFetchAdd(&layout->producer_cursor.reclaimed_loans,
                       std::uint64_t{1}, __ATOMIC_RELAXED);
        WakeData();
        return Status::Ok();
      case SlotState::ProducerClaiming:
      case SlotState::ClaimedByProducer: {
        if (SlotOwnerProcessAlive(
                slot, replaced_process_id,
                replaced_process_start_ticks)) {
          return Status(StatusCode::WouldBlock,
                        "previous producer still owns an unfinished loan");
        }
        if (!TransitionSlot(slot, state, SlotState::Free)) {
          return Status(StatusCode::WouldBlock,
                        "producer loan state changed during recovery");
        }
        AtomicFetchAdd(&layout->producer_cursor.reclaimed_loans,
                       std::uint64_t{1}, __ATOMIC_RELAXED);
        WakeSpace();
        return Status::Ok();
      }
      case SlotState::ConsumerTaking:
      case SlotState::LoanedToConsumer:
        return Status(StatusCode::WouldBlock,
                      "consumer still pins the next producer chunk");
    }
    return Status(StatusCode::CorruptData,
                  "unknown producer slot lifecycle state");
  }

  [[nodiscard]] Status RecoverConsumerTailSlot() {
    const auto tail =
        AtomicLoad(&layout->consumer_cursor.tail, __ATOMIC_RELAXED);
    const auto head =
        AtomicLoad(&layout->producer_cursor.head, __ATOMIC_ACQUIRE);
    if (tail == head) {
      return Status::Ok();
    }
    auto& slot = *SlotAt(layout, tail);
    const auto state = LoadSlotState(slot);
    switch (state) {
      case SlotState::Published:
        return Status::Ok();
      case SlotState::ConsumerTaking: {
        if (SlotOwnerProcessAlive(
                slot, replaced_process_id,
                replaced_process_start_ticks)) {
          return Status(StatusCode::WouldBlock,
                        "previous consumer is still taking the sample");
        }
        [[fallthrough]];
      }
      case SlotState::LoanedToConsumer:
        if (state == SlotState::LoanedToConsumer &&
            SlotOwnedByLiveEndpoint(
                slot, layout->consumer,
                SlotOwnerRole::Consumer)) {
          return Status(StatusCode::WouldBlock,
                        "previous consumer still owns the sample");
        }
        if (!TransitionSlot(slot, state, SlotState::Published)) {
          return Status(StatusCode::WouldBlock,
                        "consumer loan state changed during recovery");
        }
        AtomicFetchAdd(&layout->consumer_cursor.reclaimed_loans,
                       std::uint64_t{1}, __ATOMIC_RELAXED);
        WakeData();
        return Status::Ok();
      case SlotState::Free:
        if (!TryAdvanceCursor(&layout->consumer_cursor.tail, tail)) {
          return Status::Ok();
        }
        AtomicFetchAdd(&layout->consumer_cursor.received_messages,
                       std::uint64_t{1}, __ATOMIC_RELAXED);
        AtomicFetchAdd(&layout->consumer_cursor.zero_copy_releases,
                       std::uint64_t{1}, __ATOMIC_RELAXED);
        AtomicFetchAdd(&layout->consumer_cursor.reclaimed_loans,
                       std::uint64_t{1}, __ATOMIC_RELAXED);
        WakeSpace();
        return Status::Ok();
      case SlotState::ProducerClaiming:
      case SlotState::ClaimedByProducer:
        return Status(StatusCode::CorruptData,
                      "head exposed an unpublished producer chunk");
    }
    return Status(StatusCode::CorruptData,
                  "unknown consumer slot lifecycle state");
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
    {
      std::unique_lock lock(operation_mutex);
      if (closed.exchange(true, std::memory_order_acq_rel)) {
        operation_cv.wait(lock, [this] { return close_complete; });
        return;
      }
    }

    heartbeat_thread.request_stop();
    heartbeat_cv.notify_all();
    if (heartbeat_thread.joinable()) {
      heartbeat_thread.join();
    }

    if (layout != nullptr) {
      AtomicFetchAdd(&layout->producer_cursor.data_epoch,
                     std::uint32_t{1}, __ATOMIC_RELEASE);
      AtomicFetchAdd(&layout->consumer_cursor.space_epoch,
                     std::uint32_t{1}, __ATOMIC_RELEASE);
      FutexWake(&layout->producer_cursor.data_epoch);
      FutexWake(&layout->consumer_cursor.space_epoch);
    }

    std::unique_lock operation_lock(operation_mutex);
    operation_cv.wait(
        operation_lock, [this] { return active_operations == 0U; });

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

    if (descriptor >= 0) {
      ::close(descriptor);
      descriptor = -1;
    }
    if (may_unlink && unlink_on_owner_close && !object_name.empty()) {
      ::shm_unlink(object_name.c_str());
    }
    close_complete = true;
    operation_lock.unlock();
    operation_cv.notify_all();
  }
};

[[nodiscard]] Result<std::unique_ptr<SharedMemoryTransport::Impl>>
SharedMemoryTransport::CreateProducerImpl(const ChannelConfig& config) {
  const auto timeout_validation = ValidateChannelConfig(config);
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
  impl->active_spin_count = config.active_spin_count;

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
    impl->replaced_process_id =
        AtomicLoad(&metadata.pid, __ATOMIC_ACQUIRE);
    impl->replaced_process_start_ticks =
        AtomicLoad(&metadata.process_start_ticks,
                   __ATOMIC_ACQUIRE);

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
  impl->active_spin_count = config.active_spin_count;

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
  const auto timeout_validation = ValidateChannelConfig(config);
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
  impl->active_spin_count = config.active_spin_count;

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
  impl->replaced_process_id =
      AtomicLoad(&metadata.pid, __ATOMIC_ACQUIRE);
  impl->replaced_process_start_ticks =
      AtomicLoad(&metadata.process_start_ticks,
                 __ATOMIC_ACQUIRE);

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

SharedMemoryTransport::SharedMemoryTransport(std::shared_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

SharedMemoryTransport::~SharedMemoryTransport() {
  Close();
}

Result<std::unique_ptr<SharedMemoryTransport>>
SharedMemoryTransport::CreateProducer(const ChannelConfig& config) {
  auto impl = CreateProducerImpl(config);
  if (!impl) {
    return impl.status();
  }
  std::unique_ptr<Impl> owned =
      std::move(impl).take_value();
  return std::unique_ptr<SharedMemoryTransport>(
      new SharedMemoryTransport(
          std::shared_ptr<Impl>(std::move(owned))));
}

Result<std::unique_ptr<SharedMemoryTransport>>
SharedMemoryTransport::OpenConsumer(const ChannelConfig& config) {
  auto impl = OpenConsumerImpl(config);
  if (!impl) {
    return impl.status();
  }
  std::unique_ptr<Impl> owned =
      std::move(impl).take_value();
  return std::unique_ptr<SharedMemoryTransport>(
      new SharedMemoryTransport(
          std::shared_ptr<Impl>(std::move(owned))));
}

PublisherLoan::PublisherLoan(
    std::shared_ptr<void> transport, void* slot,
    std::size_t payload_size, std::uint64_t cursor,
    std::uint64_t chunk_generation,
    std::uint64_t owner_generation,
    std::uint64_t owner_role_token,
    std::uint64_t owner_channel_generation)
    : transport_(std::move(transport)),
      slot_(slot),
      payload_size_(payload_size),
      cursor_(cursor),
      chunk_generation_(chunk_generation),
      owner_generation_(owner_generation),
      owner_role_token_(owner_role_token),
      owner_channel_generation_(owner_channel_generation),
      active_(true) {}

PublisherLoan::~PublisherLoan() {
  static_cast<void>(Abandon());
}

PublisherLoan::PublisherLoan(PublisherLoan&& other) noexcept
    : transport_(std::move(other.transport_)),
      slot_(std::exchange(other.slot_, nullptr)),
      payload_size_(std::exchange(other.payload_size_, 0U)),
      cursor_(std::exchange(other.cursor_, 0U)),
      chunk_generation_(
          std::exchange(other.chunk_generation_, 0U)),
      owner_generation_(
          std::exchange(other.owner_generation_, 0U)),
      owner_role_token_(
          std::exchange(other.owner_role_token_, 0U)),
      owner_channel_generation_(
          std::exchange(other.owner_channel_generation_, 0U)),
      active_(std::exchange(other.active_, false)) {}

PublisherLoan& PublisherLoan::operator=(PublisherLoan&& other) noexcept {
  if (this != &other) {
    static_cast<void>(Abandon());
    transport_ = std::move(other.transport_);
    slot_ = std::exchange(other.slot_, nullptr);
    payload_size_ = std::exchange(other.payload_size_, 0U);
    cursor_ = std::exchange(other.cursor_, 0U);
    chunk_generation_ =
        std::exchange(other.chunk_generation_, 0U);
    owner_generation_ =
        std::exchange(other.owner_generation_, 0U);
    owner_role_token_ =
        std::exchange(other.owner_role_token_, 0U);
    owner_channel_generation_ =
        std::exchange(other.owner_channel_generation_, 0U);
    active_ = std::exchange(other.active_, false);
  }
  return *this;
}

std::span<std::byte> PublisherLoan::Data() noexcept {
  if (!transport_ || !active_ || slot_ == nullptr) {
    return {};
  }
  return {SlotPayload(static_cast<SlotHeader*>(slot_)),
          payload_size_};
}

std::size_t PublisherLoan::size() const noexcept {
  return transport_ && active_ ? payload_size_ : 0U;
}

PublisherLoan::operator bool() const noexcept {
  return transport_ && active_;
}

Status PublisherLoan::Abandon() noexcept {
  if (!transport_ || !active_) {
    return Status::Ok();
  }
  auto& transport =
      *static_cast<SharedMemoryTransport::Impl*>(transport_.get());
  auto& slot = *static_cast<SlotHeader*>(slot_);
  Status result = Status::Ok();
  if (!SlotMatches(
          slot, SlotState::ClaimedByProducer,
          chunk_generation_, owner_generation_,
          owner_role_token_, owner_channel_generation_) ||
      !TransitionSlot(
          slot, SlotState::ClaimedByProducer,
          SlotState::Free)) {
    result = Status(StatusCode::StaleGeneration);
  } else {
    transport.WakeSpace();
  }
  active_ = false;
  slot_ = nullptr;
  transport_.reset();
  return result;
}

Status PublisherLoan::Publish() noexcept {
  if (!transport_ || !active_) {
    return Status(StatusCode::Closed);
  }
  auto& transport =
      *static_cast<SharedMemoryTransport::Impl*>(transport_.get());
  auto& slot = *static_cast<SlotHeader*>(slot_);
  if (transport.closed.load(std::memory_order_acquire)) {
    static_cast<void>(Abandon());
    return Status(StatusCode::Closed);
  }
  const auto current_generation =
      AtomicLoad(&transport.layout->header.generation,
                 __ATOMIC_ACQUIRE);
  if (!transport.OwnsRole() ||
      transport.role != Role::Producer ||
      current_generation != owner_channel_generation_ ||
      current_generation !=
          transport.observed_generation.load(
              std::memory_order_acquire)) {
    static_cast<void>(Abandon());
    return Status(StatusCode::StaleGeneration);
  }
  const auto head = AtomicLoad(
      &transport.layout->producer_cursor.head,
      __ATOMIC_RELAXED);
  if (head != cursor_ ||
      !SlotMatches(
          slot, SlotState::ClaimedByProducer,
          chunk_generation_, owner_generation_,
          owner_role_token_, owner_channel_generation_)) {
    static_cast<void>(Abandon());
    return Status(StatusCode::StaleGeneration);
  }
  if (!TransitionSlot(
          slot, SlotState::ClaimedByProducer,
          SlotState::Published)) {
    active_ = false;
    slot_ = nullptr;
    transport_.reset();
    return Status(StatusCode::StaleGeneration);
  }

  if (!TryAdvanceCursor(
          &transport.layout->producer_cursor.head, cursor_)) {
    active_ = false;
    slot_ = nullptr;
    transport_.reset();
    return Status(StatusCode::StaleGeneration);
  }
  transport.WakeData();
  AtomicFetchAdd(
      &transport.layout->producer_cursor.sent_messages,
      std::uint64_t{1}, __ATOMIC_RELAXED);
  AtomicFetchAdd(
      &transport.layout->producer_cursor.zero_copy_publishes,
      std::uint64_t{1}, __ATOMIC_RELAXED);
  AtomicFetchAdd(
      &transport.layout->producer.operation_sequence,
      std::uint64_t{1}, __ATOMIC_RELAXED);
  active_ = false;
  slot_ = nullptr;
  transport_.reset();
  return Status::Ok();
}

SubscriberSample::SubscriberSample(
    std::shared_ptr<void> transport, void* slot,
    std::size_t payload_size, std::uint64_t cursor,
    std::uint64_t chunk_generation,
    std::uint64_t owner_generation,
    std::uint64_t owner_role_token,
    std::uint64_t owner_channel_generation)
    : transport_(std::move(transport)),
      slot_(slot),
      payload_size_(payload_size),
      cursor_(cursor),
      chunk_generation_(chunk_generation),
      owner_generation_(owner_generation),
      owner_role_token_(owner_role_token),
      owner_channel_generation_(owner_channel_generation),
      active_(true) {}

SubscriberSample::~SubscriberSample() {
  static_cast<void>(Release());
}

SubscriberSample::SubscriberSample(
    SubscriberSample&& other) noexcept
    : transport_(std::move(other.transport_)),
      slot_(std::exchange(other.slot_, nullptr)),
      payload_size_(std::exchange(other.payload_size_, 0U)),
      cursor_(std::exchange(other.cursor_, 0U)),
      chunk_generation_(
          std::exchange(other.chunk_generation_, 0U)),
      owner_generation_(
          std::exchange(other.owner_generation_, 0U)),
      owner_role_token_(
          std::exchange(other.owner_role_token_, 0U)),
      owner_channel_generation_(
          std::exchange(other.owner_channel_generation_, 0U)),
      active_(std::exchange(other.active_, false)) {}

SubscriberSample& SubscriberSample::operator=(
    SubscriberSample&& other) noexcept {
  if (this != &other) {
    static_cast<void>(Release());
    transport_ = std::move(other.transport_);
    slot_ = std::exchange(other.slot_, nullptr);
    payload_size_ = std::exchange(other.payload_size_, 0U);
    cursor_ = std::exchange(other.cursor_, 0U);
    chunk_generation_ =
        std::exchange(other.chunk_generation_, 0U);
    owner_generation_ =
        std::exchange(other.owner_generation_, 0U);
    owner_role_token_ =
        std::exchange(other.owner_role_token_, 0U);
    owner_channel_generation_ =
        std::exchange(other.owner_channel_generation_, 0U);
    active_ = std::exchange(other.active_, false);
  }
  return *this;
}

std::span<const std::byte> SubscriberSample::Data() const noexcept {
  if (!transport_ || !active_ || slot_ == nullptr) {
    return {};
  }
  return {SlotPayload(static_cast<SlotHeader*>(slot_)),
          payload_size_};
}

std::size_t SubscriberSample::size() const noexcept {
  return transport_ && active_ ? payload_size_ : 0U;
}

SubscriberSample::operator bool() const noexcept {
  return transport_ && active_;
}

Status SubscriberSample::Release() noexcept {
  if (!transport_ || !active_) {
    return Status::Ok();
  }
  auto& transport =
      *static_cast<SharedMemoryTransport::Impl*>(transport_.get());
  auto& slot = *static_cast<SlotHeader*>(slot_);
  const bool endpoint_is_current =
      !transport.closed.load(std::memory_order_acquire) &&
      transport.role == Role::Consumer &&
      transport.OwnsRole();
  const auto tail = AtomicLoad(
      &transport.layout->consumer_cursor.tail,
      __ATOMIC_RELAXED);
  if (tail != cursor_ ||
      !SlotMatches(
          slot, SlotState::LoanedToConsumer,
          chunk_generation_, owner_generation_,
          owner_role_token_, owner_channel_generation_) ||
      !TransitionSlot(
          slot, SlotState::LoanedToConsumer,
          SlotState::Free)) {
    active_ = false;
    slot_ = nullptr;
    transport_.reset();
    return Status(StatusCode::StaleGeneration);
  }

  const bool advanced = TryAdvanceCursor(
      &transport.layout->consumer_cursor.tail, cursor_);
  if (advanced) {
    transport.WakeSpace();
    AtomicFetchAdd(
        &transport.layout->consumer_cursor.received_messages,
        std::uint64_t{1}, __ATOMIC_RELAXED);
    AtomicFetchAdd(
        &transport.layout->consumer_cursor.zero_copy_releases,
        std::uint64_t{1}, __ATOMIC_RELAXED);
    AtomicFetchAdd(
        &transport.layout->consumer.operation_sequence,
        std::uint64_t{1}, __ATOMIC_RELAXED);
  }
  active_ = false;
  slot_ = nullptr;
  transport_.reset();
  return endpoint_is_current && advanced
             ? Status::Ok()
             : Status(StatusCode::StaleGeneration);
}

Status SubscriberSample::Requeue() noexcept {
  if (!transport_ || !active_) {
    return Status(StatusCode::Closed);
  }
  auto& transport =
      *static_cast<SharedMemoryTransport::Impl*>(transport_.get());
  auto& slot = *static_cast<SlotHeader*>(slot_);
  if (!SlotMatches(
          slot, SlotState::LoanedToConsumer,
          chunk_generation_, owner_generation_,
          owner_role_token_, owner_channel_generation_) ||
      !TransitionSlot(
          slot, SlotState::LoanedToConsumer,
          SlotState::Published)) {
    active_ = false;
    slot_ = nullptr;
    transport_.reset();
    return Status(StatusCode::StaleGeneration);
  }
  transport.WakeData();
  active_ = false;
  slot_ = nullptr;
  transport_.reset();
  return Status::Ok();
}

Result<PublisherLoan> SharedMemoryTransport::Loan(
    std::size_t size, SendOptions options) {
  if (!impl_) {
    return Status(StatusCode::Closed);
  }
  Impl::OperationLease operation(*impl_);
  if (!operation.acquired()) {
    return Status(StatusCode::Closed);
  }
  std::lock_guard zero_copy_lock(impl_->zero_copy_mutex);
  if (impl_->role != Role::Producer) {
    return Status(StatusCode::RoleConflict,
                  "consumer endpoint cannot loan");
  }
  if (!impl_->OwnsRole()) {
    return Status(StatusCode::StaleGeneration,
                  "producer role was reclaimed by another endpoint");
  }
  if (size > impl_->layout->queue.max_message_size) {
    return Status(StatusCode::MessageTooLarge);
  }

  auto& layout = *impl_->layout;
  for (;;) {
    if (impl_->closed.load(std::memory_order_acquire)) {
      return Status(StatusCode::Closed);
    }
    const auto current_generation =
        AtomicLoad(&layout.header.generation, __ATOMIC_ACQUIRE);
    if (!impl_->OwnsRole() ||
        current_generation !=
            impl_->observed_generation.load(
                std::memory_order_acquire)) {
      return Status(StatusCode::StaleGeneration);
    }

    const auto head =
        AtomicLoad(&layout.producer_cursor.head,
                   __ATOMIC_RELAXED);
    const auto tail =
        AtomicLoad(&layout.consumer_cursor.tail,
                   __ATOMIC_ACQUIRE);
    if (head - tail >= layout.queue.slot_count) {
      if (options.policy == BackpressurePolicy::Drop) {
        AtomicFetchAdd(
            &layout.producer_cursor.dropped_messages,
            std::uint64_t{1}, __ATOMIC_RELAXED);
        return Status(StatusCode::Dropped);
      }
      if (options.deadline.expired()) {
        AtomicFetchAdd(
            &layout.producer_cursor.timeout_count,
            std::uint64_t{1}, __ATOMIC_RELAXED);
        return Status(StatusCode::Timeout);
      }
      std::uint64_t spun_head = head;
      if (SpinForSpace(
              layout, impl_->active_spin_count,
              &spun_head)) {
        continue;
      }
      const auto peer_status = PeerStatus(
          layout.consumer, impl_->peer_timeout,
          impl_->next_peer_identity_probe_ns);
      if (!peer_status) {
        return peer_status;
      }
      const auto expected_epoch = AtomicLoad(
          &layout.consumer_cursor.space_epoch,
          __ATOMIC_ACQUIRE);
      const auto rechecked_head = AtomicLoad(
          &layout.producer_cursor.head, __ATOMIC_RELAXED);
      const auto rechecked_tail = AtomicLoad(
          &layout.consumer_cursor.tail, __ATOMIC_ACQUIRE);
      if (rechecked_head - rechecked_tail <
          layout.queue.slot_count) {
        continue;
      }
      const auto wait = FutexWait(
          &layout.consumer_cursor.space_epoch,
          expected_epoch,
          ProbeDeadline(options.deadline,
                        impl_->peer_timeout));
      if (wait.result == FutexWaitResult::Error) {
        return Status(
            StatusCode::IoError,
            "futex wait for chunk space failed",
            wait.error);
      }
      continue;
    }

    auto& slot = *SlotAt(&layout, head);
    if (LoadSlotState(slot) != SlotState::Free) {
      const auto recovery = impl_->RecoverProducerHeadSlot();
      if (!recovery) {
        return recovery;
      }
      if (AtomicLoad(&layout.producer_cursor.head,
                     __ATOMIC_RELAXED) != head) {
        continue;
      }
      if (LoadSlotState(slot) != SlotState::Free) {
        continue;
      }
    }
    if (!impl_->OwnsRole() ||
        AtomicLoad(&layout.header.generation,
                   __ATOMIC_ACQUIRE) != current_generation) {
      return Status(StatusCode::StaleGeneration);
    }
    if (!TransitionSlot(
            slot, SlotState::Free,
            SlotState::ProducerClaiming)) {
      continue;
    }

    const auto chunk_generation =
        NextNonZero(slot.chunk_generation);
    const auto owner_generation =
        NextNonZero(slot.owner_generation);
    slot.length = static_cast<std::uint32_t>(size);
    slot.sequence = head + 1U;
    slot.chunk_generation = chunk_generation;
    slot.owner_generation = owner_generation;
    slot.owner_pid = static_cast<std::int32_t>(::getpid());
    slot.owner_role =
        static_cast<std::uint32_t>(SlotOwnerRole::Producer);
    slot.owner_process_start_ticks =
        impl_->process_start_ticks;
    slot.owner_role_token = impl_->role_token;
    slot.owner_channel_generation = current_generation;
    AtomicStore(&slot.state,
                StateValue(SlotState::ClaimedByProducer),
                __ATOMIC_RELEASE);

    AtomicFetchAdd(
        &layout.producer_cursor.zero_copy_loans,
        std::uint64_t{1}, __ATOMIC_RELAXED);
    AtomicFetchAdd(
        &layout.producer.operation_sequence,
        std::uint64_t{1}, __ATOMIC_RELAXED);
    return PublisherLoan(
        impl_, &slot, size, head, chunk_generation,
        owner_generation, impl_->role_token,
        current_generation);
  }
}

Result<SubscriberSample> SharedMemoryTransport::Take(
    Deadline deadline) {
  if (!impl_) {
    return Status(StatusCode::Closed);
  }
  Impl::OperationLease operation(*impl_);
  if (!operation.acquired()) {
    return Status(StatusCode::Closed);
  }
  std::lock_guard zero_copy_lock(impl_->zero_copy_mutex);
  if (impl_->role != Role::Consumer) {
    return Status(StatusCode::RoleConflict,
                  "producer endpoint cannot take");
  }
  if (!impl_->OwnsRole()) {
    return Status(StatusCode::StaleGeneration,
                  "consumer role was reclaimed by another endpoint");
  }

  auto& layout = *impl_->layout;
  for (;;) {
    if (impl_->closed.load(std::memory_order_acquire)) {
      return Status(StatusCode::Closed);
    }
    const auto current_generation =
        AtomicLoad(&layout.header.generation, __ATOMIC_ACQUIRE);
    if (current_generation !=
        impl_->observed_generation.load(
            std::memory_order_acquire)) {
      impl_->observed_generation.store(
          current_generation, std::memory_order_release);
      AtomicStore(&layout.consumer.generation,
                  current_generation, __ATOMIC_RELEASE);
    }

    const auto tail =
        AtomicLoad(&layout.consumer_cursor.tail,
                   __ATOMIC_RELAXED);
    const auto head =
        AtomicLoad(&layout.producer_cursor.head,
                   __ATOMIC_ACQUIRE);
    if (tail == head) {
      if (deadline.expired()) {
        AtomicFetchAdd(
            &layout.consumer_cursor.timeout_count,
            std::uint64_t{1}, __ATOMIC_RELAXED);
        return Status(StatusCode::Timeout);
      }
      std::uint64_t spun_tail = tail;
      if (SpinForData(
              layout, impl_->active_spin_count,
              &spun_tail)) {
        continue;
      }
      const auto peer_status = PeerStatus(
          layout.producer, impl_->peer_timeout,
          impl_->next_peer_identity_probe_ns);
      if (!peer_status) {
        return peer_status;
      }
      const auto expected_epoch = AtomicLoad(
          &layout.producer_cursor.data_epoch,
          __ATOMIC_ACQUIRE);
      const auto rechecked_tail = AtomicLoad(
          &layout.consumer_cursor.tail, __ATOMIC_RELAXED);
      const auto rechecked_head = AtomicLoad(
          &layout.producer_cursor.head, __ATOMIC_ACQUIRE);
      if (rechecked_tail != rechecked_head) {
        continue;
      }
      const auto wait = FutexWait(
          &layout.producer_cursor.data_epoch,
          expected_epoch,
          ProbeDeadline(deadline, impl_->peer_timeout));
      if (wait.result == FutexWaitResult::Error) {
        return Status(
            StatusCode::IoError,
            "futex wait for published chunk failed",
            wait.error);
      }
      continue;
    }

    const auto recovery = impl_->RecoverConsumerTailSlot();
    if (!recovery) {
      return recovery;
    }
    if (AtomicLoad(&layout.consumer_cursor.tail,
                   __ATOMIC_RELAXED) != tail) {
      continue;
    }
    auto& slot = *SlotAt(&layout, tail);
    if (LoadSlotState(slot) != SlotState::Published) {
      continue;
    }
    if (slot.sequence != tail + 1U ||
        slot.length > layout.queue.max_message_size) {
      AtomicFetchAdd(
          &layout.consumer_cursor.corrupt_messages,
          std::uint64_t{1}, __ATOMIC_RELAXED);
      return Status(
          StatusCode::CorruptData,
          "published chunk metadata is invalid");
    }
    if (!TransitionSlot(
            slot, SlotState::Published,
            SlotState::ConsumerTaking)) {
      continue;
    }

    const auto owner_generation =
        NextNonZero(slot.owner_generation);
    slot.owner_generation = owner_generation;
    slot.owner_pid = static_cast<std::int32_t>(::getpid());
    slot.owner_role =
        static_cast<std::uint32_t>(SlotOwnerRole::Consumer);
    slot.owner_process_start_ticks =
        impl_->process_start_ticks;
    slot.owner_role_token = impl_->role_token;
    slot.owner_channel_generation = current_generation;
    AtomicStore(&slot.state,
                StateValue(SlotState::LoanedToConsumer),
                __ATOMIC_RELEASE);

    AtomicFetchAdd(
        &layout.consumer_cursor.zero_copy_takes,
        std::uint64_t{1}, __ATOMIC_RELAXED);
    AtomicFetchAdd(
        &layout.consumer.operation_sequence,
        std::uint64_t{1}, __ATOMIC_RELAXED);
    return SubscriberSample(
        impl_, &slot, slot.length, tail,
        slot.chunk_generation, owner_generation,
        impl_->role_token, current_generation);
  }
}

Status SharedMemoryTransport::Send(std::span<const std::byte> message,
                                   SendOptions options) {
  auto loan_result = Loan(message.size(), options);
  if (!loan_result) {
    return loan_result.status();
  }
  auto loan = std::move(loan_result).take_value();
  auto destination = loan.Data();
  if (!message.empty()) {
    std::memcpy(
        destination.data(), message.data(), message.size());
  }
  return loan.Publish();

}

Result<std::size_t> SharedMemoryTransport::Receive(
    std::span<std::byte> destination, Deadline deadline) {
  auto sample_result = Take(deadline);
  if (!sample_result) {
    return sample_result.status();
  }
  auto sample = std::move(sample_result).take_value();
  const auto loaned_payload = sample.Data();
  if (destination.size() < loaned_payload.size()) {
    const auto requeue_status = sample.Requeue();
    if (!requeue_status) {
      return requeue_status;
    }
    return Status(StatusCode::BufferTooSmall);
  }
  if (!loaned_payload.empty()) {
    std::memcpy(
        destination.data(), loaned_payload.data(),
        loaned_payload.size());
  }
  const auto received_length = loaned_payload.size();
  const auto release_status = sample.Release();
  if (!release_status) {
    return release_status;
  }
  return received_length;

}

TransportStats SharedMemoryTransport::Stats() const noexcept {
  TransportStats stats;
  if (!impl_) {
    return stats;
  }
  Impl::OperationLease operation(*impl_);
  if (!operation.acquired() || impl_->layout == nullptr) {
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
  stats.zero_copy_loans =
      AtomicLoad(&layout.producer_cursor.zero_copy_loans,
                 __ATOMIC_RELAXED);
  stats.zero_copy_publishes =
      AtomicLoad(&layout.producer_cursor.zero_copy_publishes,
                 __ATOMIC_RELAXED);
  stats.zero_copy_takes =
      AtomicLoad(&layout.consumer_cursor.zero_copy_takes,
                 __ATOMIC_RELAXED);
  stats.zero_copy_releases =
      AtomicLoad(&layout.consumer_cursor.zero_copy_releases,
                 __ATOMIC_RELAXED);
  stats.producer_loan_reclaims =
      AtomicLoad(&layout.producer_cursor.reclaimed_loans,
                 __ATOMIC_RELAXED);
  stats.consumer_loan_reclaims =
      AtomicLoad(&layout.consumer_cursor.reclaimed_loans,
                 __ATOMIC_RELAXED);
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
