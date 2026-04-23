#include <fastipc/mpmc_shared_memory_transport.hpp>

#include "mpmc_shared_memory_layout.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <fcntl.h>
#include <linux/futex.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace fastipc {
namespace {

using detail::MpmcSharedLayout;
using detail::MpmcSlotHeader;

static_assert(__atomic_always_lock_free(sizeof(std::uint32_t), nullptr));
static_assert(__atomic_always_lock_free(sizeof(std::uint64_t), nullptr));

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

  constexpr std::int64_t kNanosecondsPerSecond = 1'000'000'000;
  const auto remaining_count = remaining.count();
  const auto added_seconds =
      remaining_count / kNanosecondsPerSecond;
  const auto added_nanoseconds =
      remaining_count % kNanosecondsPerSecond;
  const auto combined_nanoseconds =
      static_cast<std::int64_t>(kernel_now.tv_nsec) +
      added_nanoseconds;

  timespec absolute{};
  absolute.tv_sec = static_cast<time_t>(
      static_cast<std::int64_t>(kernel_now.tv_sec) +
      added_seconds +
      combined_nanoseconds / kNanosecondsPerSecond);
  absolute.tv_nsec = static_cast<long>(
      combined_nanoseconds % kNanosecondsPerSecond);
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
  const long result = ::syscall(
      SYS_futex, epoch, FUTEX_WAIT_BITSET, expected, timeout,
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
      ::syscall(SYS_futex, epoch, FUTEX_WAKE, INT_MAX,
                nullptr, nullptr, 0));
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

[[nodiscard]] std::uint64_t MonotonicNanoseconds() noexcept {
  const auto now =
      std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          now)
          .count());
}

[[nodiscard]] Status ErrnoStatus(
    int error, StatusCode fallback, std::string detail) {
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

[[nodiscard]] Result<std::string> CanonicalName(
    std::string_view name) {
  if (name.empty() || name.size() > 200U) {
    return Status(
        StatusCode::InvalidArgument,
        "MPMC channel name must contain 1 to 200 characters");
  }
  for (const char character : name) {
    const bool valid =
        (character >= 'a' && character <= 'z') ||
        (character >= 'A' && character <= 'Z') ||
        (character >= '0' && character <= '9') ||
        character == '_' || character == '-' || character == '.';
    if (!valid) {
      return Status(
          StatusCode::InvalidArgument,
          "MPMC channel name contains an unsupported character");
    }
  }
  return std::string("/") + std::string(name);
}

struct Geometry {
  std::size_t segment_bytes{0U};
  std::uint32_t slot_stride{0U};
  std::uint32_t slots_offset{0U};
};

[[nodiscard]] Result<Geometry> ComputeGeometry(
    const MpmcChannelConfig& config) {
  if (config.capacity < 2U ||
      (config.capacity & (config.capacity - 1U)) != 0U) {
    return Status(
        StatusCode::InvalidArgument,
        "MPMC capacity must be a power of two and at least two");
  }
  constexpr std::uint32_t kMaximumCapacity = 1U << 20U;
  if (config.capacity > kMaximumCapacity) {
    return Status(
        StatusCode::InvalidArgument,
        "MPMC capacity must not exceed 1048576");
  }
  constexpr std::uint32_t kMaximumMessageSize =
      16U * 1024U * 1024U;
  if (config.max_message_size == 0U ||
      config.max_message_size > kMaximumMessageSize) {
    return Status(
        StatusCode::InvalidArgument,
        "MPMC max_message_size must be between 1 and 16 MiB");
  }
  constexpr std::uint32_t kMaximumActiveSpinCount = 65'536U;
  if (config.active_spin_count > kMaximumActiveSpinCount) {
    return Status(
        StatusCode::InvalidArgument,
        "MPMC active_spin_count must not exceed 65536");
  }
  if ((config.permissions & ~0777U) != 0U) {
    return Status(
        StatusCode::InvalidArgument,
        "MPMC permissions contain bits outside POSIX mode 0777");
  }

  const std::size_t raw_stride =
      sizeof(MpmcSlotHeader) +
      static_cast<std::size_t>(config.max_message_size);
  const std::size_t aligned_stride = detail::MpmcAlignUp(
      raw_stride, detail::kMpmcCacheLine);
  if (aligned_stride >
      std::numeric_limits<std::uint32_t>::max()) {
    return Status(
        StatusCode::InvalidArgument,
        "MPMC slot stride overflows the shared layout");
  }

  constexpr std::size_t slots_offset = detail::MpmcAlignUp(
      sizeof(MpmcSharedLayout), detail::kMpmcCacheLine);
  if (static_cast<std::size_t>(config.capacity) >
      (std::numeric_limits<std::size_t>::max() - slots_offset) /
          aligned_stride) {
    return Status(
        StatusCode::InvalidArgument,
        "MPMC segment size overflows size_t");
  }
  const std::size_t segment_bytes =
      slots_offset +
      static_cast<std::size_t>(config.capacity) * aligned_stride;
  constexpr std::size_t kMaximumSegmentBytes =
      512U * 1024U * 1024U;
  if (segment_bytes > kMaximumSegmentBytes) {
    return Status(
        StatusCode::InvalidArgument,
        "MPMC segment must not exceed 512 MiB");
  }
  if (segment_bytes >
      static_cast<std::size_t>(
          std::numeric_limits<off_t>::max())) {
    return Status(
        StatusCode::InvalidArgument,
        "MPMC segment size exceeds off_t");
  }

  return Geometry{
      segment_bytes,
      static_cast<std::uint32_t>(aligned_stride),
      static_cast<std::uint32_t>(slots_offset)};
}

[[nodiscard]] Status ValidateLayout(
    const MpmcSharedLayout& layout, std::size_t mapped_bytes,
    const MpmcChannelConfig& expected) {
  const auto init_state =
      AtomicLoad(&layout.header.init_state, __ATOMIC_ACQUIRE);
  if (init_state != detail::kMpmcInitReady) {
    return Status(
        StatusCode::LayoutMismatch,
        "MPMC shared-memory initialization is incomplete");
  }
  if (layout.header.magic != detail::kMpmcMagic) {
    return Status(
        StatusCode::LayoutMismatch,
        "MPMC shared-memory magic differs");
  }
  if (layout.header.version_major !=
          detail::kMpmcLayoutMajor ||
      layout.header.version_minor !=
          detail::kMpmcLayoutMinor) {
    return Status(
        StatusCode::LayoutMismatch,
        "MPMC shared-memory layout version is incompatible");
  }
  if (layout.header.endian_marker !=
      detail::kMpmcEndianMarker) {
    return Status(
        StatusCode::LayoutMismatch,
        "MPMC shared-memory byte order is incompatible");
  }
  if (layout.header.header_bytes !=
          sizeof(MpmcSharedLayout) ||
      layout.header.segment_bytes != mapped_bytes) {
    return Status(
        StatusCode::LayoutMismatch,
        "MPMC shared-memory size differs");
  }
  if (layout.queue.capacity != expected.capacity ||
      layout.queue.max_message_size !=
          expected.max_message_size) {
    return Status(
        StatusCode::LayoutMismatch,
        "MPMC channel geometry differs from caller configuration");
  }

  auto geometry_result = ComputeGeometry(expected);
  if (!geometry_result) {
    return geometry_result.status();
  }
  const auto& geometry = geometry_result.value();
  if (layout.queue.slot_stride != geometry.slot_stride ||
      layout.queue.slots_offset != geometry.slots_offset ||
      geometry.segment_bytes != mapped_bytes) {
    return Status(
        StatusCode::LayoutMismatch,
        "MPMC slot offsets or stride are invalid");
  }
  return Status::Ok();
}

struct Reservation {
  std::uint64_t position{0U};
  MpmcSlotHeader* slot{nullptr};
};

[[nodiscard]] std::int64_t SequenceDifference(
    std::uint64_t actual, std::uint64_t expected) noexcept {
  return static_cast<std::int64_t>(actual - expected);
}

[[nodiscard]] bool TryReserveEnqueue(
    MpmcSharedLayout& layout, Reservation* reservation) noexcept {
  auto position =
      AtomicLoad(&layout.enqueue.position, __ATOMIC_RELAXED);
  for (;;) {
    auto* slot = detail::MpmcSlotAt(&layout, position);
    const auto sequence =
        AtomicLoad(&slot->sequence, __ATOMIC_ACQUIRE);
    const auto difference =
        SequenceDifference(sequence, position);
    if (difference == 0) {
      auto expected = position;
      if (AtomicCompareExchange(
              &layout.enqueue.position, &expected,
              position + 1U, __ATOMIC_RELAXED,
              __ATOMIC_RELAXED)) {
        *reservation = {position, slot};
        return true;
      }
      position = expected;
      continue;
    }
    if (difference < 0) {
      return false;
    }
    position =
        AtomicLoad(&layout.enqueue.position, __ATOMIC_RELAXED);
  }
}

[[nodiscard]] bool TryReserveDequeue(
    MpmcSharedLayout& layout, Reservation* reservation) noexcept {
  auto position =
      AtomicLoad(&layout.dequeue.position, __ATOMIC_RELAXED);
  for (;;) {
    auto* slot = detail::MpmcSlotAt(&layout, position);
    const auto sequence =
        AtomicLoad(&slot->sequence, __ATOMIC_ACQUIRE);
    const auto expected_sequence = position + 1U;
    const auto difference =
        SequenceDifference(sequence, expected_sequence);
    if (difference == 0) {
      auto expected = position;
      if (AtomicCompareExchange(
              &layout.dequeue.position, &expected,
              position + 1U, __ATOMIC_RELAXED,
              __ATOMIC_RELAXED)) {
        *reservation = {position, slot};
        return true;
      }
      position = expected;
      continue;
    }
    if (difference < 0) {
      return false;
    }
    position =
        AtomicLoad(&layout.dequeue.position, __ATOMIC_RELAXED);
  }
}

}  // namespace

struct MpmcSharedMemoryTransport::Impl {
  int descriptor{-1};
  void* mapping{MAP_FAILED};
  std::size_t mapped_bytes{0U};
  MpmcSharedLayout* layout{nullptr};
  std::string object_name;
  bool owner{false};
  bool unlink_on_owner_close{false};
  std::uint32_t active_spin_count{0U};
  std::atomic<bool> closed{false};

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

  void WakeData() noexcept {
    AtomicFetchAdd(
        &layout->enqueue.data_epoch, std::uint32_t{1U},
        __ATOMIC_RELEASE);
    FutexWake(&layout->enqueue.data_epoch);
  }

  void WakeSpace() noexcept {
    AtomicFetchAdd(
        &layout->dequeue.space_epoch, std::uint32_t{1U},
        __ATOMIC_RELEASE);
    FutexWake(&layout->dequeue.space_epoch);
  }

  void Close() noexcept {
    if (closed.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    if (layout != nullptr) {
      WakeData();
      WakeSpace();
    }
    if (owner && unlink_on_owner_close &&
        !object_name.empty()) {
      static_cast<void>(::shm_unlink(object_name.c_str()));
    }
  }
};

Result<std::unique_ptr<MpmcSharedMemoryTransport::Impl>>
MpmcSharedMemoryTransport::CreateImpl(
    const MpmcChannelConfig& config) {
  auto name_result = CanonicalName(config.name);
  if (!name_result) {
    return name_result.status();
  }
  auto geometry_result = ComputeGeometry(config);
  if (!geometry_result) {
    return geometry_result.status();
  }

  const std::string object_name =
      std::move(name_result).take_value();
  const auto geometry =
      std::move(geometry_result).take_value();
  const int descriptor = ::shm_open(
      object_name.c_str(),
      O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC,
      static_cast<mode_t>(config.permissions));
  if (descriptor < 0) {
    return ErrnoStatus(
        errno, StatusCode::IoError,
        "failed to create MPMC shared-memory object");
  }

  if (::ftruncate(
          descriptor,
          static_cast<off_t>(geometry.segment_bytes)) != 0) {
    const int error = errno;
    static_cast<void>(::close(descriptor));
    static_cast<void>(::shm_unlink(object_name.c_str()));
    return ErrnoStatus(
        error, StatusCode::IoError,
        "failed to size MPMC shared-memory object");
  }

  void* mapping = ::mmap(
      nullptr, geometry.segment_bytes,
      PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
  if (mapping == MAP_FAILED) {
    const int error = errno;
    static_cast<void>(::close(descriptor));
    static_cast<void>(::shm_unlink(object_name.c_str()));
    return ErrnoStatus(
        error, StatusCode::IoError,
        "failed to map MPMC shared-memory object");
  }

  auto impl = std::make_unique<Impl>();
  impl->descriptor = descriptor;
  impl->mapping = mapping;
  impl->mapped_bytes = geometry.segment_bytes;
  impl->layout = static_cast<MpmcSharedLayout*>(mapping);
  impl->object_name = object_name;
  impl->owner = true;
  impl->unlink_on_owner_close =
      config.unlink_on_owner_close;
  impl->active_spin_count = config.active_spin_count;

  std::memset(mapping, 0, geometry.segment_bytes);
  auto& layout = *impl->layout;
  AtomicStore(
      &layout.header.init_state,
      detail::kMpmcInitInitializing, __ATOMIC_RELAXED);
  layout.header.magic = detail::kMpmcMagic;
  layout.header.version_major = detail::kMpmcLayoutMajor;
  layout.header.version_minor = detail::kMpmcLayoutMinor;
  layout.header.header_bytes =
      static_cast<std::uint32_t>(sizeof(MpmcSharedLayout));
  layout.header.segment_bytes = geometry.segment_bytes;
  layout.header.created_monotonic_ns = MonotonicNanoseconds();
  layout.header.endian_marker = detail::kMpmcEndianMarker;
  layout.queue.capacity = config.capacity;
  layout.queue.max_message_size = config.max_message_size;
  layout.queue.slot_stride = geometry.slot_stride;
  layout.queue.slots_offset = geometry.slots_offset;

  for (std::uint64_t index = 0U;
       index < static_cast<std::uint64_t>(config.capacity);
       ++index) {
    auto* slot = detail::MpmcSlotAt(&layout, index);
    slot->sequence = index;
  }
  AtomicStore(
      &layout.header.init_state,
      detail::kMpmcInitReady, __ATOMIC_RELEASE);
  return impl;
}

Result<std::unique_ptr<MpmcSharedMemoryTransport::Impl>>
MpmcSharedMemoryTransport::OpenImpl(
    const MpmcChannelConfig& config) {
  auto name_result = CanonicalName(config.name);
  if (!name_result) {
    return name_result.status();
  }
  auto geometry_result = ComputeGeometry(config);
  if (!geometry_result) {
    return geometry_result.status();
  }

  const std::string object_name =
      std::move(name_result).take_value();
  const auto expected_geometry =
      std::move(geometry_result).take_value();
  const int descriptor =
      ::shm_open(object_name.c_str(), O_RDWR | O_CLOEXEC, 0);
  if (descriptor < 0) {
    const int error = errno;
    const StatusCode fallback =
        error == ENOENT ? StatusCode::PeerUnavailable
                        : StatusCode::IoError;
    return ErrnoStatus(
        error, fallback,
        "failed to open MPMC shared-memory object");
  }

  struct stat object_stat {};
  if (::fstat(descriptor, &object_stat) != 0) {
    const int error = errno;
    static_cast<void>(::close(descriptor));
    return ErrnoStatus(
        error, StatusCode::IoError,
        "failed to inspect MPMC shared-memory object");
  }
  if (object_stat.st_size <
      static_cast<off_t>(sizeof(MpmcSharedLayout))) {
    static_cast<void>(::close(descriptor));
    return Status(
        StatusCode::LayoutMismatch,
        "MPMC shared-memory object is smaller than its header");
  }
  if (object_stat.st_size !=
      static_cast<off_t>(expected_geometry.segment_bytes)) {
    static_cast<void>(::close(descriptor));
    return Status(
        StatusCode::LayoutMismatch,
        "MPMC shared-memory object size differs from configuration");
  }

  const auto mapped_bytes =
      expected_geometry.segment_bytes;
  void* mapping = ::mmap(
      nullptr, mapped_bytes, PROT_READ | PROT_WRITE,
      MAP_SHARED, descriptor, 0);
  if (mapping == MAP_FAILED) {
    const int error = errno;
    static_cast<void>(::close(descriptor));
    return ErrnoStatus(
        error, StatusCode::IoError,
        "failed to map MPMC shared-memory object");
  }

  auto impl = std::make_unique<Impl>();
  impl->descriptor = descriptor;
  impl->mapping = mapping;
  impl->mapped_bytes = mapped_bytes;
  impl->layout = static_cast<MpmcSharedLayout*>(mapping);
  impl->object_name = object_name;
  impl->active_spin_count = config.active_spin_count;

  const auto validation =
      ValidateLayout(*impl->layout, mapped_bytes, config);
  if (!validation) {
    impl->closed.store(
        true, std::memory_order_relaxed);
    return validation;
  }
  return impl;
}

MpmcSharedMemoryTransport::MpmcSharedMemoryTransport(
    std::shared_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

MpmcSharedMemoryTransport::~MpmcSharedMemoryTransport() {
  Close();
}

Result<std::unique_ptr<MpmcSharedMemoryTransport>>
MpmcSharedMemoryTransport::Create(
    const MpmcChannelConfig& config) {
  auto impl_result = CreateImpl(config);
  if (!impl_result) {
    return impl_result.status();
  }
  auto owned = std::move(impl_result).take_value();
  return std::unique_ptr<MpmcSharedMemoryTransport>(
      new MpmcSharedMemoryTransport(
          std::shared_ptr<Impl>(std::move(owned))));
}

Result<std::unique_ptr<MpmcSharedMemoryTransport>>
MpmcSharedMemoryTransport::Open(
    const MpmcChannelConfig& config) {
  auto impl_result = OpenImpl(config);
  if (!impl_result) {
    return impl_result.status();
  }
  auto owned = std::move(impl_result).take_value();
  return std::unique_ptr<MpmcSharedMemoryTransport>(
      new MpmcSharedMemoryTransport(
          std::shared_ptr<Impl>(std::move(owned))));
}

Status MpmcSharedMemoryTransport::Send(
    std::span<const std::byte> message,
    SendOptions options) {
  if (!impl_ ||
      impl_->closed.load(std::memory_order_acquire)) {
    return Status(StatusCode::Closed);
  }
  auto& layout = *impl_->layout;
  if (message.size() > layout.queue.max_message_size) {
    return Status(StatusCode::MessageTooLarge);
  }

  Reservation reservation;
  std::uint32_t spins_remaining =
      impl_->active_spin_count;
  for (;;) {
    if (impl_->closed.load(std::memory_order_acquire)) {
      return Status(StatusCode::Closed);
    }
    if (TryReserveEnqueue(layout, &reservation)) {
      break;
    }

    if (options.policy == BackpressurePolicy::Drop) {
      AtomicFetchAdd(
          &layout.enqueue.dropped_messages,
          std::uint64_t{1U}, __ATOMIC_RELAXED);
      return Status(
          StatusCode::Dropped,
          "MPMC queue is full");
    }
    if (options.deadline.expired()) {
      AtomicFetchAdd(
          &layout.enqueue.timeout_count,
          std::uint64_t{1U}, __ATOMIC_RELAXED);
      return Status(
          StatusCode::Timeout,
          "MPMC enqueue deadline expired");
    }
    if (spins_remaining > 0U) {
      --spins_remaining;
      CpuRelax();
      continue;
    }

    const auto observed_epoch =
        AtomicLoad(
            &layout.dequeue.space_epoch, __ATOMIC_ACQUIRE);
    if (TryReserveEnqueue(layout, &reservation)) {
      break;
    }
    if (impl_->closed.load(std::memory_order_acquire)) {
      return Status(StatusCode::Closed);
    }
    if (options.deadline.expired()) {
      AtomicFetchAdd(
          &layout.enqueue.timeout_count,
          std::uint64_t{1U}, __ATOMIC_RELAXED);
      return Status(
          StatusCode::Timeout,
          "MPMC enqueue deadline expired");
    }

    const auto wait = FutexWait(
        &layout.dequeue.space_epoch, observed_epoch,
        options.deadline);
    if (wait.result == FutexWaitResult::TimedOut) {
      AtomicFetchAdd(
          &layout.enqueue.timeout_count,
          std::uint64_t{1U}, __ATOMIC_RELAXED);
      return Status(
          StatusCode::Timeout,
          "MPMC enqueue deadline expired");
    }
    if (wait.result == FutexWaitResult::Error) {
      return Status(
          StatusCode::IoError,
          "MPMC enqueue futex wait failed", wait.error);
    }
    spins_remaining = impl_->active_spin_count;
  }

  reservation.slot->length =
      static_cast<std::uint32_t>(message.size());
  if (!message.empty()) {
    std::memcpy(
        detail::MpmcSlotPayload(reservation.slot),
        message.data(), message.size());
  }
  AtomicStore(
      &reservation.slot->sequence,
      reservation.position + 1U, __ATOMIC_RELEASE);
  AtomicFetchAdd(
      &layout.enqueue.sent_messages,
      std::uint64_t{1U}, __ATOMIC_RELAXED);
  impl_->WakeData();
  return Status::Ok();
}

Result<std::size_t> MpmcSharedMemoryTransport::Receive(
    std::span<std::byte> destination,
    Deadline deadline) {
  if (!impl_ ||
      impl_->closed.load(std::memory_order_acquire)) {
    return Status(StatusCode::Closed);
  }
  auto& layout = *impl_->layout;

  Reservation reservation;
  std::uint32_t spins_remaining =
      impl_->active_spin_count;
  for (;;) {
    if (impl_->closed.load(std::memory_order_acquire)) {
      return Status(StatusCode::Closed);
    }
    if (TryReserveDequeue(layout, &reservation)) {
      break;
    }
    if (deadline.expired()) {
      AtomicFetchAdd(
          &layout.dequeue.timeout_count,
          std::uint64_t{1U}, __ATOMIC_RELAXED);
      return Status(
          StatusCode::Timeout,
          "MPMC dequeue deadline expired");
    }
    if (spins_remaining > 0U) {
      --spins_remaining;
      CpuRelax();
      continue;
    }

    const auto observed_epoch =
        AtomicLoad(
            &layout.enqueue.data_epoch, __ATOMIC_ACQUIRE);
    if (TryReserveDequeue(layout, &reservation)) {
      break;
    }
    if (impl_->closed.load(std::memory_order_acquire)) {
      return Status(StatusCode::Closed);
    }
    if (deadline.expired()) {
      AtomicFetchAdd(
          &layout.dequeue.timeout_count,
          std::uint64_t{1U}, __ATOMIC_RELAXED);
      return Status(
          StatusCode::Timeout,
          "MPMC dequeue deadline expired");
    }

    const auto wait = FutexWait(
        &layout.enqueue.data_epoch, observed_epoch, deadline);
    if (wait.result == FutexWaitResult::TimedOut) {
      AtomicFetchAdd(
          &layout.dequeue.timeout_count,
          std::uint64_t{1U}, __ATOMIC_RELAXED);
      return Status(
          StatusCode::Timeout,
          "MPMC dequeue deadline expired");
    }
    if (wait.result == FutexWaitResult::Error) {
      return Status(
          StatusCode::IoError,
          "MPMC dequeue futex wait failed", wait.error);
    }
    spins_remaining = impl_->active_spin_count;
  }

  const auto length =
      static_cast<std::size_t>(reservation.slot->length);
  Status failure = Status::Ok();
  if (length > layout.queue.max_message_size) {
    AtomicFetchAdd(
        &layout.dequeue.corrupt_messages,
        std::uint64_t{1U}, __ATOMIC_RELAXED);
    failure = Status(
        StatusCode::CorruptData,
        "MPMC slot length exceeds configured maximum");
  } else if (length > destination.size()) {
    AtomicFetchAdd(
        &layout.dequeue.buffer_too_small_count,
        std::uint64_t{1U}, __ATOMIC_RELAXED);
    failure = Status(
        StatusCode::BufferTooSmall,
        "MPMC destination is too small; reserved message was discarded");
  } else {
    if (length > 0U) {
      std::memcpy(
          destination.data(),
          detail::MpmcSlotPayload(reservation.slot), length);
    }
    AtomicFetchAdd(
        &layout.dequeue.received_messages,
        std::uint64_t{1U}, __ATOMIC_RELAXED);
  }

  AtomicStore(
      &reservation.slot->sequence,
      reservation.position +
          static_cast<std::uint64_t>(layout.queue.capacity),
      __ATOMIC_RELEASE);
  impl_->WakeSpace();

  if (!failure) {
    return failure;
  }
  return length;
}

TransportStats MpmcSharedMemoryTransport::Stats() const noexcept {
  TransportStats stats;
  if (!impl_ || impl_->layout == nullptr) {
    return stats;
  }

  const auto& layout = *impl_->layout;
  stats.sent_messages = AtomicLoad(
      &layout.enqueue.sent_messages, __ATOMIC_RELAXED);
  stats.received_messages = AtomicLoad(
      &layout.dequeue.received_messages, __ATOMIC_RELAXED);
  stats.dropped_messages =
      AtomicLoad(
          &layout.enqueue.dropped_messages, __ATOMIC_RELAXED) +
      AtomicLoad(
          &layout.dequeue.buffer_too_small_count,
          __ATOMIC_RELAXED);
  stats.send_timeouts = AtomicLoad(
      &layout.enqueue.timeout_count, __ATOMIC_RELAXED);
  stats.receive_timeouts = AtomicLoad(
      &layout.dequeue.timeout_count, __ATOMIC_RELAXED);
  stats.corrupt_messages = AtomicLoad(
      &layout.dequeue.corrupt_messages, __ATOMIC_RELAXED);
  return stats;
}

void MpmcSharedMemoryTransport::Close() noexcept {
  if (impl_) {
    impl_->Close();
  }
}

}  // namespace fastipc
