#include <fastipc/unix_domain_socket_transport.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <fcntl.h>
#include <linux/memfd.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

namespace fastipc {
namespace {

constexpr std::uint32_t kFrameMagic = 0x43504946U;
constexpr std::uint16_t kFrameVersion = 1U;
constexpr std::uint16_t kFrameFlagMemfd = 1U;
constexpr std::uint16_t kKnownFrameFlags = kFrameFlagMemfd;
constexpr std::uint32_t kInlineMessageLimit = 64U * 1024U;
constexpr std::uint32_t kMaximumMessageSize = 16U * 1024U * 1024U;
constexpr int kRequiredMemfdSeals =
    F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE;

struct WireHeader {
  std::uint32_t magic{kFrameMagic};
  std::uint16_t version{kFrameVersion};
  std::uint16_t flags{0};
  std::uint32_t payload_bytes{0};
  std::uint32_t reserved{0};
};
static_assert(sizeof(WireHeader) == 16);

struct SocketAddress {
  sockaddr_un address{};
  socklen_t length{0};
};

struct PathIdentity {
  dev_t device{};
  ino_t inode{};
  bool valid{false};
};

[[nodiscard]] bool ReadSocketPathIdentity(
    const std::string& path, PathIdentity* identity) noexcept {
  struct stat state {};
  if (::lstat(path.c_str(), &state) != 0) {
    return false;
  }
  if (!S_ISSOCK(state.st_mode)) {
    errno = EINVAL;
    return false;
  }
  identity->device = state.st_dev;
  identity->inode = state.st_ino;
  identity->valid = true;
  return true;
}

[[nodiscard]] bool PathMatchesIdentity(
    const std::string& path, const PathIdentity& identity) noexcept {
  if (!identity.valid) {
    return false;
  }
  struct stat state {};
  return ::lstat(path.c_str(), &state) == 0 &&
         S_ISSOCK(state.st_mode) && state.st_dev == identity.device &&
         state.st_ino == identity.inode;
}

void UnlinkIfSameSocket(const std::string& path,
                        const PathIdentity& identity) noexcept {
  if (PathMatchesIdentity(path, identity)) {
    static_cast<void>(::unlink(path.c_str()));
  }
}

[[nodiscard]] Status SocketStatus(StatusCode fallback, std::string detail,
                                  int error = errno) {
  StatusCode code = fallback;
  if (error == EACCES || error == EPERM) {
    code = StatusCode::PermissionDenied;
  } else if (error == EADDRINUSE) {
    code = StatusCode::AlreadyExists;
  }
  return Status(code, std::move(detail), error);
}

[[nodiscard]] Status ValidateConfig(const UnixDomainSocketConfig& config) {
  if (config.path.empty() || config.path.front() != '/') {
    return Status(StatusCode::InvalidArgument,
                  "Unix-domain socket path must be absolute");
  }
  sockaddr_un address{};
  if (config.path.size() >= sizeof(address.sun_path)) {
    return Status(StatusCode::InvalidArgument,
                  "Unix-domain socket path exceeds sun_path");
  }
  if (config.max_message_size == 0U ||
      config.max_message_size > kMaximumMessageSize) {
    return Status(StatusCode::InvalidArgument,
                  "max_message_size must be between 1 and 16 MiB");
  }
  if ((config.permissions & ~0777U) != 0U) {
    return Status(StatusCode::InvalidArgument,
                  "permissions contain bits outside POSIX mode 0777");
  }
  if (config.socket_buffer_bytes >
      static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
    return Status(StatusCode::InvalidArgument,
                  "socket_buffer_bytes exceeds the kernel integer range");
  }
  return Status::Ok();
}

[[nodiscard]] SocketAddress MakeAddress(std::string_view path) {
  SocketAddress result;
  result.address.sun_family = AF_UNIX;
  std::memcpy(result.address.sun_path, path.data(), path.size());
  result.address.sun_path[path.size()] = '\0';
  result.length = static_cast<socklen_t>(
      offsetof(sockaddr_un, sun_path) + path.size() + 1U);
  return result;
}

[[nodiscard]] int CreateSocket() noexcept {
  return ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
}

[[nodiscard]] Status ConfigureSocket(
    int descriptor, const UnixDomainSocketConfig& config) {
  if (config.socket_buffer_bytes == 0U) {
    return Status::Ok();
  }
  const int bytes = static_cast<int>(config.socket_buffer_bytes);
  if (::setsockopt(descriptor, SOL_SOCKET, SO_SNDBUF, &bytes,
                   sizeof(bytes)) != 0) {
    return SocketStatus(StatusCode::IoError,
                        "failed to set Unix socket send buffer");
  }
  if (::setsockopt(descriptor, SOL_SOCKET, SO_RCVBUF, &bytes,
                   sizeof(bytes)) != 0) {
    return SocketStatus(StatusCode::IoError,
                        "failed to set Unix socket receive buffer");
  }
  return Status::Ok();
}

[[nodiscard]] int PollTimeoutMilliseconds(const Deadline& deadline) noexcept {
  if (deadline.infinite()) {
    return -1;
  }
  const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(
      deadline.time_point() - Deadline::Clock::now());
  if (remaining <= std::chrono::nanoseconds::zero()) {
    return 0;
  }

  constexpr std::int64_t nanoseconds_per_millisecond = 1'000'000;
  const auto rounded =
      (remaining.count() + nanoseconds_per_millisecond - 1) /
      nanoseconds_per_millisecond;
  return static_cast<int>(std::min<std::int64_t>(rounded, INT_MAX));
}

[[nodiscard]] Status WaitFor(int descriptor, short events,
                             const Deadline& deadline) {
  for (;;) {
    pollfd candidate{};
    candidate.fd = descriptor;
    candidate.events = events;
    const int result =
        ::poll(&candidate, 1, PollTimeoutMilliseconds(deadline));
    if (result > 0) {
      if ((candidate.revents & events) != 0) {
        return Status::Ok();
      }
      if ((candidate.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        return Status(StatusCode::PeerDead,
                      "Unix-domain socket peer disconnected");
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
    return SocketStatus(StatusCode::IoError,
                        "poll on Unix-domain socket failed");
  }
}

[[nodiscard]] Status PeerSocketError(std::string detail, int error) {
  if (error == EPIPE || error == ECONNRESET || error == ENOTCONN ||
      error == ECONNABORTED) {
    return Status(StatusCode::PeerDead, std::move(detail), error);
  }
  return SocketStatus(StatusCode::IoError, std::move(detail), error);
}

class UniqueDescriptor {
 public:
  UniqueDescriptor() = default;
  explicit UniqueDescriptor(int descriptor) noexcept
      : descriptor_(descriptor) {}
  ~UniqueDescriptor() { Reset(); }

  UniqueDescriptor(const UniqueDescriptor&) = delete;
  UniqueDescriptor& operator=(const UniqueDescriptor&) = delete;

  UniqueDescriptor(UniqueDescriptor&& other) noexcept
      : descriptor_(std::exchange(other.descriptor_, -1)) {}

  UniqueDescriptor& operator=(UniqueDescriptor&& other) noexcept {
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

[[nodiscard]] Status CreateSealedPayload(
    std::span<const std::byte> payload,
    UniqueDescriptor* result) {
  const int descriptor = static_cast<int>(
      ::syscall(SYS_memfd_create, "fastipc-payload",
                MFD_CLOEXEC | MFD_ALLOW_SEALING));
  if (descriptor < 0) {
    return SocketStatus(StatusCode::IoError,
                        "failed to create memfd payload");
  }

  UniqueDescriptor owner(descriptor);
  if (::ftruncate(descriptor, static_cast<off_t>(payload.size())) != 0) {
    return SocketStatus(StatusCode::IoError,
                        "failed to size memfd payload");
  }

  std::size_t written = 0;
  while (written < payload.size()) {
    const ssize_t count =
        ::pwrite(descriptor, payload.data() + written,
                 payload.size() - written,
                 static_cast<off_t>(written));
    if (count > 0) {
      written += static_cast<std::size_t>(count);
      continue;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    return SocketStatus(StatusCode::IoError,
                        "failed to populate memfd payload",
                        count < 0 ? errno : EIO);
  }

  if (::fcntl(descriptor, F_ADD_SEALS, kRequiredMemfdSeals) != 0) {
    return SocketStatus(StatusCode::IoError,
                        "failed to seal memfd payload");
  }

  *result = std::move(owner);
  return Status::Ok();
}

[[nodiscard]] Status ReadSealedPayload(
    int descriptor, std::uint32_t expected_size,
    std::span<std::byte> destination) {
  struct stat state {};
  if (::fstat(descriptor, &state) != 0 || !S_ISREG(state.st_mode) ||
      state.st_size < 0 ||
      static_cast<std::uint64_t>(state.st_size) != expected_size) {
    return Status(StatusCode::CorruptData,
                  "received payload descriptor has the wrong size or type");
  }

  const int seals = ::fcntl(descriptor, F_GET_SEALS);
  if (seals < 0 ||
      (seals & kRequiredMemfdSeals) != kRequiredMemfdSeals) {
    return Status(StatusCode::CorruptData,
                  "received payload descriptor is not sealed");
  }

  std::size_t read_bytes = 0;
  while (read_bytes < expected_size) {
    const ssize_t count =
        ::pread(descriptor, destination.data() + read_bytes,
                static_cast<std::size_t>(expected_size) - read_bytes,
                static_cast<off_t>(read_bytes));
    if (count > 0) {
      read_bytes += static_cast<std::size_t>(count);
      continue;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    return Status(StatusCode::CorruptData,
                  "failed to read sealed payload descriptor",
                  count < 0 ? errno : EIO);
  }
  return Status::Ok();
}

struct ReceivedControl {
  UniqueDescriptor descriptor;
  std::size_t descriptor_count{0};
  bool unexpected_control{false};
};

[[nodiscard]] ReceivedControl ParseReceivedControl(
    msghdr* message) noexcept {
  ReceivedControl result;
  for (cmsghdr* control = CMSG_FIRSTHDR(message); control != nullptr;
       control = CMSG_NXTHDR(message, control)) {
    if (control->cmsg_level != SOL_SOCKET ||
        control->cmsg_type != SCM_RIGHTS ||
        control->cmsg_len < CMSG_LEN(0)) {
      result.unexpected_control = true;
      continue;
    }

    const std::size_t payload_bytes =
        control->cmsg_len - CMSG_LEN(0);
    if (payload_bytes % sizeof(int) != 0U) {
      result.unexpected_control = true;
      continue;
    }

    const std::size_t count = payload_bytes / sizeof(int);
    const auto* descriptors =
        reinterpret_cast<const int*>(CMSG_DATA(control));
    for (std::size_t index = 0; index < count; ++index) {
      ++result.descriptor_count;
      if (result.descriptor.get() < 0) {
        result.descriptor.Reset(descriptors[index]);
      } else {
        static_cast<void>(::close(descriptors[index]));
      }
    }
  }
  return result;
}

void DiscardPacket(int descriptor) noexcept {
  std::byte discard{};
  iovec vector{};
  vector.iov_base = &discard;
  vector.iov_len = sizeof(discard);
  alignas(cmsghdr)
      std::array<std::byte, CMSG_SPACE(sizeof(int))> control{};
  msghdr packet{};
  packet.msg_iov = &vector;
  packet.msg_iovlen = 1;
  packet.msg_control = control.data();
  packet.msg_controllen = control.size();
  static_cast<void>(
      ::recvmsg(descriptor, &packet,
                MSG_DONTWAIT | MSG_TRUNC | MSG_CMSG_CLOEXEC));
  auto received_control = ParseReceivedControl(&packet);
  static_cast<void>(received_control);
}

}  // namespace

struct UnixDomainSocketTransport::Impl {
  int descriptor{-1};
  std::uint32_t max_message_size{0};
  std::atomic<bool> closed{false};
  std::atomic<std::uint64_t> sent_messages{0};
  std::atomic<std::uint64_t> received_messages{0};
  std::atomic<std::uint64_t> dropped_messages{0};
  std::atomic<std::uint64_t> send_timeouts{0};
  std::atomic<std::uint64_t> receive_timeouts{0};
  std::atomic<std::uint64_t> corrupt_messages{0};

  ~Impl() {
    Close();
    if (descriptor >= 0) {
      static_cast<void>(::close(descriptor));
      descriptor = -1;
    }
  }

  void Close() noexcept {
    if (closed.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    if (descriptor >= 0) {
      static_cast<void>(::shutdown(descriptor, SHUT_RDWR));
    }
  }
};

struct UnixDomainSocketListener::Impl {
  int descriptor{-1};
  std::string path;
  UnixDomainSocketConfig config;
  PathIdentity path_identity;
  bool owns_path{false};
  bool closed{false};

  ~Impl() { Close(); }

  void Close() noexcept {
    if (closed) {
      return;
    }
    closed = true;
    if (descriptor >= 0) {
      static_cast<void>(::close(descriptor));
      descriptor = -1;
    }
    if (owns_path && config.unlink_on_listener_close && !path.empty()) {
      UnlinkIfSameSocket(path, path_identity);
      owns_path = false;
    }
  }
};

UnixDomainSocketTransport::UnixDomainSocketTransport(
    std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

UnixDomainSocketTransport::~UnixDomainSocketTransport() = default;

Result<std::unique_ptr<UnixDomainSocketTransport>>
UnixDomainSocketTransport::Connect(const UnixDomainSocketConfig& config,
                                   Deadline deadline) {
  const auto validation = ValidateConfig(config);
  if (!validation) {
    return validation;
  }

  const int descriptor = CreateSocket();
  if (descriptor < 0) {
    return SocketStatus(StatusCode::IoError,
                        "failed to create Unix-domain socket");
  }
  const auto configured = ConfigureSocket(descriptor, config);
  if (!configured) {
    static_cast<void>(::close(descriptor));
    return configured;
  }

  const auto socket_address = MakeAddress(config.path);
  for (;;) {
    const int result =
        ::connect(
            descriptor,
            reinterpret_cast<const sockaddr*>(
                &socket_address.address),
            socket_address.length);
    if (result == 0 || (result != 0 && errno == EISCONN)) {
      break;
    }

    const int error = errno;
    if (error == ENOENT || error == ECONNREFUSED) {
      static_cast<void>(::close(descriptor));
      return Status(StatusCode::PeerUnavailable,
                    "Unix-domain socket listener is unavailable",
                    error);
    }
    const bool pending =
        error == EINPROGRESS || error == EALREADY ||
        error == EAGAIN;
    if (!pending) {
      static_cast<void>(::close(descriptor));
      return PeerSocketError(
          "Unix-domain socket connect failed", error);
    }
    if (deadline.expired()) {
      static_cast<void>(::close(descriptor));
      return Status(StatusCode::Timeout);
    }

    const auto wait = WaitFor(descriptor, POLLOUT, deadline);
    if (!wait) {
      static_cast<void>(::close(descriptor));
      return wait;
    }

    int socket_error = 0;
    socklen_t error_size = sizeof(socket_error);
    if (::getsockopt(descriptor, SOL_SOCKET, SO_ERROR,
                     &socket_error, &error_size) != 0) {
      const auto status =
          SocketStatus(
              StatusCode::IoError,
              "failed to read Unix socket connect status");
      static_cast<void>(::close(descriptor));
      return status;
    }
    if (socket_error != 0 && socket_error != EINPROGRESS &&
        socket_error != EALREADY && socket_error != EAGAIN) {
      static_cast<void>(::close(descriptor));
      if (socket_error == ENOENT ||
          socket_error == ECONNREFUSED) {
        return Status(
            StatusCode::PeerUnavailable,
            "Unix-domain socket listener is unavailable",
            socket_error);
      }
      return PeerSocketError(
          "Unix-domain socket connect failed", socket_error);
    }

    sockaddr_un peer{};
    socklen_t peer_size = sizeof(peer);
    if (::getpeername(
            descriptor, reinterpret_cast<sockaddr*>(&peer),
            &peer_size) == 0) {
      break;
    }
    const int peer_error = errno;
    if (peer_error != ENOTCONN) {
      const auto status =
          SocketStatus(
              StatusCode::IoError,
              "failed to verify Unix socket connection",
              peer_error);
      static_cast<void>(::close(descriptor));
      return status;
    }
  }

  auto impl = std::make_unique<Impl>();
  impl->descriptor = descriptor;
  impl->max_message_size = config.max_message_size;
  return std::unique_ptr<UnixDomainSocketTransport>(
      new UnixDomainSocketTransport(std::move(impl)));
}

Status UnixDomainSocketTransport::Send(
    std::span<const std::byte> message, SendOptions options) {
  if (!impl_ || impl_->closed.load(std::memory_order_acquire)) {
    return Status(StatusCode::Closed);
  }
  if (message.size() > impl_->max_message_size) {
    return Status(StatusCode::MessageTooLarge);
  }

  const bool uses_memfd = message.size() > kInlineMessageLimit;
  UniqueDescriptor payload_descriptor;
  if (uses_memfd) {
    const auto prepared =
        CreateSealedPayload(message, &payload_descriptor);
    if (!prepared) {
      return prepared;
    }
  }

  WireHeader header;
  header.flags = uses_memfd ? kFrameFlagMemfd : 0U;
  header.payload_bytes =
      static_cast<std::uint32_t>(message.size());

  std::array<iovec, 2> vectors{};
  vectors[0].iov_base = &header;
  vectors[0].iov_len = sizeof(header);
  vectors[1].iov_base =
      const_cast<std::byte*>(message.data());
  vectors[1].iov_len = uses_memfd ? 0U : message.size();

  alignas(cmsghdr)
      std::array<std::byte, CMSG_SPACE(sizeof(int))> control{};
  msghdr packet{};
  packet.msg_iov = vectors.data();
  packet.msg_iovlen = uses_memfd ? 1U : vectors.size();
  if (uses_memfd) {
    packet.msg_control = control.data();
    packet.msg_controllen = control.size();
    cmsghdr* rights = CMSG_FIRSTHDR(&packet);
    rights->cmsg_level = SOL_SOCKET;
    rights->cmsg_type = SCM_RIGHTS;
    rights->cmsg_len = CMSG_LEN(sizeof(int));
    const int descriptor = payload_descriptor.get();
    std::memcpy(CMSG_DATA(rights), &descriptor, sizeof(descriptor));
  }

  const auto expected_bytes =
      sizeof(header) + (uses_memfd ? 0U : message.size());
  for (;;) {
    const ssize_t sent =
        ::sendmsg(impl_->descriptor, &packet,
                  MSG_DONTWAIT | MSG_NOSIGNAL);
    if (sent == static_cast<ssize_t>(expected_bytes)) {
      impl_->sent_messages.fetch_add(1U,
                                     std::memory_order_relaxed);
      return Status::Ok();
    }
    if (sent >= 0) {
      return Status(StatusCode::IoError,
                    "Unix SOCK_SEQPACKET performed a partial send");
    }

    const int error = errno;
    if (impl_->closed.load(std::memory_order_acquire)) {
      return Status(StatusCode::Closed);
    }
    if (error == EINTR) {
      continue;
    }
    if (error == EMSGSIZE) {
      return Status(StatusCode::MessageTooLarge,
                    "inline payload exceeds the kernel packet limit",
                    error);
    }
    if (error != EAGAIN && error != EWOULDBLOCK) {
      return PeerSocketError("Unix-domain socket send failed", error);
    }
    if (options.policy == BackpressurePolicy::Drop) {
      impl_->dropped_messages.fetch_add(1U,
                                        std::memory_order_relaxed);
      return Status(StatusCode::Dropped);
    }
    if (options.deadline.expired()) {
      impl_->send_timeouts.fetch_add(1U,
                                     std::memory_order_relaxed);
      return Status(StatusCode::Timeout);
    }

    const auto wait =
        WaitFor(impl_->descriptor, POLLOUT, options.deadline);
    if (!wait) {
      if (impl_->closed.load(std::memory_order_acquire)) {
        return Status(StatusCode::Closed);
      }
      if (wait.code() == StatusCode::Timeout) {
        impl_->send_timeouts.fetch_add(
            1U, std::memory_order_relaxed);
      }
      return wait;
    }
  }
}

Result<std::size_t>
UnixDomainSocketTransport::Receive(
    std::span<std::byte> destination, Deadline deadline) {
  if (!impl_ || impl_->closed.load(std::memory_order_acquire)) {
    return Status(StatusCode::Closed);
  }

  for (;;) {
    WireHeader peeked_header{};
    iovec peek_vector{};
    peek_vector.iov_base = &peeked_header;
    peek_vector.iov_len = sizeof(peeked_header);
    msghdr peek_packet{};
    peek_packet.msg_iov = &peek_vector;
    peek_packet.msg_iovlen = 1;

    const ssize_t packet_bytes =
        ::recvmsg(impl_->descriptor, &peek_packet,
                  MSG_DONTWAIT | MSG_PEEK | MSG_TRUNC);
    if (packet_bytes > 0) {
      const auto packet_size =
          static_cast<std::size_t>(packet_bytes);
      const bool uses_memfd =
          (peeked_header.flags & kFrameFlagMemfd) != 0U;
      const std::size_t expected_packet_size =
          sizeof(WireHeader) +
          (uses_memfd
               ? 0U
               : static_cast<std::size_t>(
                     peeked_header.payload_bytes));
      const bool malformed =
          packet_size < sizeof(WireHeader) ||
          peeked_header.magic != kFrameMagic ||
          peeked_header.version != kFrameVersion ||
          (peeked_header.flags & ~kKnownFrameFlags) != 0U ||
          peeked_header.reserved != 0U ||
          peeked_header.payload_bytes >
              impl_->max_message_size ||
          (!uses_memfd &&
           peeked_header.payload_bytes > kInlineMessageLimit) ||
          packet_size != expected_packet_size;
      if (malformed) {
        DiscardPacket(impl_->descriptor);
        impl_->corrupt_messages.fetch_add(
            1U, std::memory_order_relaxed);
        return Status(StatusCode::CorruptData,
                      "Unix-domain socket frame header is malformed");
      }
      if (destination.size() <
          peeked_header.payload_bytes) {
        return Status(StatusCode::BufferTooSmall);
      }

      WireHeader received_header{};
      std::array<iovec, 2> vectors{};
      vectors[0].iov_base = &received_header;
      vectors[0].iov_len = sizeof(received_header);
      vectors[1].iov_base = destination.data();
      vectors[1].iov_len =
          uses_memfd ? 0U : peeked_header.payload_bytes;
      alignas(cmsghdr)
          std::array<std::byte, CMSG_SPACE(sizeof(int))>
              control{};
      msghdr packet{};
      packet.msg_iov = vectors.data();
      packet.msg_iovlen = uses_memfd ? 1U : vectors.size();
      packet.msg_control = control.data();
      packet.msg_controllen = control.size();
      const ssize_t received =
          ::recvmsg(impl_->descriptor, &packet,
                    MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
      if (received < 0) {
        if (impl_->closed.load(std::memory_order_acquire)) {
          return Status(StatusCode::Closed);
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK ||
            errno == EINTR) {
          continue;
        }
        return PeerSocketError(
            "Unix-domain socket receive failed", errno);
      }

      auto received_control =
          ParseReceivedControl(&packet);
      if (impl_->closed.load(std::memory_order_acquire)) {
        return Status(StatusCode::Closed);
      }
      const bool control_is_valid =
          (packet.msg_flags & MSG_CTRUNC) == 0 &&
          !received_control.unexpected_control &&
          received_control.descriptor_count ==
              (uses_memfd ? 1U : 0U);
      if (received != packet_bytes ||
          received_header.magic != peeked_header.magic ||
          received_header.version !=
              peeked_header.version ||
          received_header.flags != peeked_header.flags ||
          received_header.payload_bytes !=
              peeked_header.payload_bytes ||
          received_header.reserved != 0U ||
          (packet.msg_flags & MSG_TRUNC) != 0 ||
          !control_is_valid) {
        impl_->corrupt_messages.fetch_add(
            1U, std::memory_order_relaxed);
        return Status(
            StatusCode::CorruptData,
            "Unix-domain socket frame changed while receiving");
      }

      if (uses_memfd) {
        const auto copied =
            ReadSealedPayload(
                received_control.descriptor.get(),
                received_header.payload_bytes, destination);
        if (!copied) {
          impl_->corrupt_messages.fetch_add(
              1U, std::memory_order_relaxed);
          return copied;
        }
      }

      impl_->received_messages.fetch_add(
          1U, std::memory_order_relaxed);
      return static_cast<std::size_t>(
          received_header.payload_bytes);
    }
    if (packet_bytes == 0) {
      if (impl_->closed.load(std::memory_order_acquire)) {
        return Status(StatusCode::Closed);
      }
      return Status(StatusCode::PeerDead,
                    "Unix-domain socket peer closed");
    }

    const int error = errno;
    if (impl_->closed.load(std::memory_order_acquire)) {
      return Status(StatusCode::Closed);
    }
    if (error == EINTR) {
      continue;
    }
    if (error != EAGAIN && error != EWOULDBLOCK) {
      return PeerSocketError(
          "Unix-domain socket receive failed", error);
    }
    if (deadline.expired()) {
      impl_->receive_timeouts.fetch_add(
          1U, std::memory_order_relaxed);
      return Status(StatusCode::Timeout);
    }

    const auto wait =
        WaitFor(impl_->descriptor, POLLIN, deadline);
    if (!wait) {
      if (impl_->closed.load(std::memory_order_acquire)) {
        return Status(StatusCode::Closed);
      }
      if (wait.code() == StatusCode::Timeout) {
        impl_->receive_timeouts.fetch_add(
            1U, std::memory_order_relaxed);
      }
      return wait;
    }
  }
}

TransportStats UnixDomainSocketTransport::Stats() const noexcept {
  TransportStats stats;
  if (!impl_) {
    return stats;
  }
  stats.sent_messages =
      impl_->sent_messages.load(std::memory_order_relaxed);
  stats.received_messages =
      impl_->received_messages.load(std::memory_order_relaxed);
  stats.dropped_messages =
      impl_->dropped_messages.load(std::memory_order_relaxed);
  stats.send_timeouts =
      impl_->send_timeouts.load(std::memory_order_relaxed);
  stats.receive_timeouts =
      impl_->receive_timeouts.load(std::memory_order_relaxed);
  stats.corrupt_messages =
      impl_->corrupt_messages.load(std::memory_order_relaxed);
  return stats;
}

void UnixDomainSocketTransport::Close() noexcept {
  if (impl_) {
    impl_->Close();
  }
}

UnixDomainSocketListener::UnixDomainSocketListener(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

UnixDomainSocketListener::~UnixDomainSocketListener() = default;

Result<std::unique_ptr<UnixDomainSocketListener>>
UnixDomainSocketListener::Bind(const UnixDomainSocketConfig& config) {
  const auto validation = ValidateConfig(config);
  if (!validation) {
    return validation;
  }

  const int descriptor = CreateSocket();
  if (descriptor < 0) {
    return SocketStatus(StatusCode::IoError,
                        "failed to create Unix-domain listener");
  }
  const auto configured = ConfigureSocket(descriptor, config);
  if (!configured) {
    static_cast<void>(::close(descriptor));
    return configured;
  }

  const auto socket_address = MakeAddress(config.path);
  if (::bind(descriptor,
             reinterpret_cast<const sockaddr*>(&socket_address.address),
             socket_address.length) != 0) {
    const auto status =
        SocketStatus(StatusCode::IoError,
                     "failed to bind Unix-domain listener");
    static_cast<void>(::close(descriptor));
    return status;
  }

  PathIdentity path_identity;
  if (!ReadSocketPathIdentity(config.path, &path_identity)) {
    const auto status =
        SocketStatus(StatusCode::IoError,
                     "failed to identify Unix-domain socket path");
    static_cast<void>(::close(descriptor));
    return status;
  }
  if (::chmod(config.path.c_str(),
              static_cast<mode_t>(config.permissions)) != 0) {
    const auto status =
        SocketStatus(StatusCode::IoError,
                     "failed to set Unix-domain socket permissions");
    static_cast<void>(::close(descriptor));
    UnlinkIfSameSocket(config.path, path_identity);
    return status;
  }
  if (::listen(descriptor, 16) != 0) {
    const auto status =
        SocketStatus(StatusCode::IoError,
                     "failed to listen on Unix-domain socket");
    static_cast<void>(::close(descriptor));
    UnlinkIfSameSocket(config.path, path_identity);
    return status;
  }

  auto impl = std::make_unique<Impl>();
  impl->descriptor = descriptor;
  impl->path = config.path;
  impl->config = config;
  impl->path_identity = path_identity;
  impl->owns_path = true;
  return std::unique_ptr<UnixDomainSocketListener>(
      new UnixDomainSocketListener(std::move(impl)));
}

Result<std::unique_ptr<UnixDomainSocketTransport>>
UnixDomainSocketListener::Accept(Deadline deadline) {
  if (!impl_ || impl_->closed) {
    return Status(StatusCode::Closed);
  }

  for (;;) {
    const int accepted =
        ::accept4(impl_->descriptor, nullptr, nullptr,
                  SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (accepted >= 0) {
      const auto configured = ConfigureSocket(accepted, impl_->config);
      if (!configured) {
        static_cast<void>(::close(accepted));
        return configured;
      }
      auto transport_impl =
          std::make_unique<UnixDomainSocketTransport::Impl>();
      transport_impl->descriptor = accepted;
      transport_impl->max_message_size =
          impl_->config.max_message_size;
      return std::unique_ptr<UnixDomainSocketTransport>(
          new UnixDomainSocketTransport(std::move(transport_impl)));
    }

    const int error = errno;
    if (error == EINTR) {
      continue;
    }
    if (error != EAGAIN && error != EWOULDBLOCK) {
      return SocketStatus(StatusCode::IoError,
                          "failed to accept Unix-domain connection",
                          error);
    }
    if (deadline.expired()) {
      return Status(StatusCode::Timeout);
    }
    const auto wait = WaitFor(impl_->descriptor, POLLIN, deadline);
    if (!wait) {
      return wait;
    }
  }
}

std::string_view UnixDomainSocketListener::path() const noexcept {
  return impl_ ? std::string_view(impl_->path) : std::string_view{};
}

void UnixDomainSocketListener::Close() noexcept {
  if (impl_) {
    impl_->Close();
  }
}

}  // namespace fastipc
