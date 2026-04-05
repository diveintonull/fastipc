#include <fastipc/unix_domain_socket_transport.hpp>

#include <array>
#include <cerrno>
#include <dirent.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <linux/memfd.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;
using fastipc::BackpressurePolicy;
using fastipc::Deadline;
using fastipc::SendOptions;
using fastipc::StatusCode;
using fastipc::UnixDomainSocketConfig;
using fastipc::UnixDomainSocketListener;
using fastipc::UnixDomainSocketTransport;

bool Check(bool condition, const char* expression, int line) {
  if (condition) {
    return true;
  }
  std::cerr << "line " << line << ": check failed: " << expression << '\n';
  return false;
}

#define CHECK(expression)                                                     \
  do {                                                                        \
    if (!Check(static_cast<bool>(expression), #expression, __LINE__)) {        \
      return 1;                                                               \
    }                                                                         \
  } while (false)

std::string SocketPath(std::string_view test_name) {
  return "/tmp/fastipc_uds_" + std::string(test_name) + "_" +
         std::to_string(::getpid()) + ".sock";
}

struct ConnectedPair {
  UnixDomainSocketConfig config;
  std::unique_ptr<UnixDomainSocketListener> listener;
  std::unique_ptr<UnixDomainSocketTransport> client;
  std::unique_ptr<UnixDomainSocketTransport> server;
};

std::unique_ptr<ConnectedPair> MakePair(
    std::string_view test_name, std::uint32_t max_message_size = 4096U,
    std::uint32_t socket_buffer_bytes = 0U) {
  auto pair = std::make_unique<ConnectedPair>();
  pair->config.path = SocketPath(test_name);
  pair->config.max_message_size = max_message_size;
  pair->config.permissions = 0600;
  pair->config.socket_buffer_bytes = socket_buffer_bytes;

  auto listener_result = UnixDomainSocketListener::Bind(pair->config);
  if (!listener_result) {
    std::cerr << "bind failed: " << listener_result.status().ToString() << '\n';
    return nullptr;
  }
  pair->listener = std::move(listener_result).take_value();

  auto client_result =
      UnixDomainSocketTransport::Connect(pair->config, Deadline::After(500ms));
  if (!client_result) {
    std::cerr << "connect failed: " << client_result.status().ToString()
              << '\n';
    return nullptr;
  }
  pair->client = std::move(client_result).take_value();

  auto server_result = pair->listener->Accept(Deadline::After(500ms));
  if (!server_result) {
    std::cerr << "accept failed: " << server_result.status().ToString() << '\n';
    return nullptr;
  }
  pair->server = std::move(server_result).take_value();
  return pair;
}

class ScopedFd {
 public:
  explicit ScopedFd(int descriptor) : descriptor_(descriptor) {}
  ~ScopedFd() {
    if (descriptor_ >= 0) {
      static_cast<void>(::close(descriptor_));
    }
  }

  ScopedFd(const ScopedFd&) = delete;
  ScopedFd& operator=(const ScopedFd&) = delete;

  [[nodiscard]] int get() const noexcept { return descriptor_; }

 private:
  int descriptor_;
};

std::size_t OpenDescriptorCount() {
  DIR* directory = ::opendir("/proc/self/fd");
  if (directory == nullptr) {
    return std::numeric_limits<std::size_t>::max();
  }

  std::size_t count = 0;
  while (const dirent* entry = ::readdir(directory)) {
    if (entry->d_name[0] != '.') {
      ++count;
    }
  }
  static_cast<void>(::closedir(directory));
  return count;
}

struct RawWireHeader {
  std::uint32_t magic{0x43504946U};
  std::uint16_t version{1U};
  std::uint16_t flags{0U};
  std::uint32_t payload_bytes{0U};
  std::uint32_t reserved{0U};
};
static_assert(sizeof(RawWireHeader) == 16U);

int RoundTripUsesTransportSeam() {
  auto pair = MakePair("roundtrip", 1024U);
  CHECK(pair != nullptr);
  CHECK(pair->listener->path() == pair->config.path);

  const std::array<std::byte, 4> request{
      std::byte{0x10}, std::byte{0x20}, std::byte{0x30}, std::byte{0x40}};
  CHECK(pair->client
            ->Send(request, SendOptions{BackpressurePolicy::Block,
                                        Deadline::After(200ms)})
            .ok());
  std::array<std::byte, 1024> destination{};
  const auto received =
      pair->server->Receive(destination, Deadline::After(200ms));
  CHECK(received.ok());
  CHECK(received.value() == request.size());
  for (std::size_t index = 0; index < request.size(); ++index) {
    CHECK(destination[index] == request[index]);
  }

  const std::array<std::byte, 2> reply{
      std::byte{0xA5}, std::byte{0x5A}};
  CHECK(pair->server
            ->Send(reply, SendOptions{BackpressurePolicy::Block,
                                      Deadline::After(200ms)})
            .ok());
  const auto reply_received =
      pair->client->Receive(destination, Deadline::After(200ms));
  CHECK(reply_received.ok());
  CHECK(reply_received.value() == reply.size());
  CHECK(destination[0] == reply[0]);
  CHECK(destination[1] == reply[1]);

  CHECK(pair->client->Stats().sent_messages == 1U);
  CHECK(pair->client->Stats().received_messages == 1U);
  pair->listener->Close();
  CHECK(::access(pair->config.path.c_str(), F_OK) != 0);
  return 0;
}

int ReceiveUsesAbsoluteDeadline() {
  auto pair = MakePair("receive_deadline");
  CHECK(pair != nullptr);

  std::array<std::byte, 1> destination{};
  const auto started = Deadline::Clock::now();
  const auto received =
      pair->server->Receive(destination, Deadline::After(25ms));
  const auto elapsed = Deadline::Clock::now() - started;
  CHECK(!received.ok());
  CHECK(received.status().code() == StatusCode::Timeout);
  CHECK(elapsed >= 5ms);
  CHECK(elapsed < 2s);
  CHECK(pair->server->Stats().receive_timeouts == 1U);
  return 0;
}

int SmallBufferPreservesPacketAndEmptyMessages() {
  auto pair = MakePair("buffer");
  CHECK(pair != nullptr);

  const std::array<std::byte, 4> message{
      std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04}};
  CHECK(pair->client
            ->Send(message, SendOptions{BackpressurePolicy::Block,
                                        Deadline::After(200ms)})
            .ok());

  std::array<std::byte, 2> small{};
  const auto too_small =
      pair->server->Receive(small, Deadline::After(200ms));
  CHECK(!too_small.ok());
  CHECK(too_small.status().code() == StatusCode::BufferTooSmall);
  CHECK(pair->server->Stats().received_messages == 0U);

  std::array<std::byte, 4> exact{};
  const auto preserved =
      pair->server->Receive(exact, Deadline::After(200ms));
  CHECK(preserved.ok());
  CHECK(preserved.value() == message.size());
  CHECK(exact == message);

  const std::span<const std::byte> empty_message;
  CHECK(pair->client
            ->Send(empty_message,
                    SendOptions{BackpressurePolicy::Block,
                                Deadline::After(200ms)})
            .ok());
  std::span<std::byte> empty_destination;
  const auto empty =
      pair->server->Receive(empty_destination, Deadline::After(200ms));
  CHECK(empty.ok());
  CHECK(empty.value() == 0U);
  CHECK(pair->server->Stats().received_messages == 2U);
  return 0;
}

int OneMegabyteMessageUsesLargePayloadPath() {
  constexpr std::uint32_t message_size = 1024U * 1024U;
  auto pair = MakePair("one_megabyte", message_size);
  CHECK(pair != nullptr);

  std::vector<std::byte> message(message_size);
  for (std::size_t index = 0; index < message.size(); ++index) {
    message[index] = static_cast<std::byte>(index % 251U);
  }

  const auto descriptors_before = OpenDescriptorCount();
  CHECK(descriptors_before !=
        std::numeric_limits<std::size_t>::max());
  CHECK(pair->client
            ->Send(message, SendOptions{BackpressurePolicy::Block,
                                        Deadline::After(2s)})
            .ok());
  CHECK(OpenDescriptorCount() == descriptors_before);

  std::array<std::byte, 64> small{};
  for (std::size_t attempt = 0; attempt < 4U; ++attempt) {
    const auto too_small =
        pair->server->Receive(small, Deadline::After(2s));
    CHECK(!too_small.ok());
    CHECK(too_small.status().code() ==
          StatusCode::BufferTooSmall);
    CHECK(OpenDescriptorCount() == descriptors_before);
  }

  std::vector<std::byte> destination(message_size);
  const auto received =
      pair->server->Receive(destination, Deadline::After(2s));
  CHECK(received.ok());
  CHECK(received.value() == message.size());
  CHECK(destination == message);
  CHECK(OpenDescriptorCount() == descriptors_before);
  return 0;
}

int ExistingPathIsNeverUnlinkedByFailedBind() {
  UnixDomainSocketConfig config;
  config.path = SocketPath("collision");

  auto first_result = UnixDomainSocketListener::Bind(config);
  CHECK(first_result.ok());
  auto first = std::move(first_result).take_value();
  CHECK(::access(config.path.c_str(), F_OK) == 0);

  const auto second = UnixDomainSocketListener::Bind(config);
  CHECK(!second.ok());
  CHECK(second.status().code() == StatusCode::AlreadyExists);
  CHECK(::access(config.path.c_str(), F_OK) == 0);

  auto client_result =
      UnixDomainSocketTransport::Connect(config, Deadline::After(200ms));
  CHECK(client_result.ok());
  auto client = std::move(client_result).take_value();
  auto server_result = first->Accept(Deadline::After(200ms));
  CHECK(server_result.ok());
  auto server = std::move(server_result).take_value();

  const std::array<std::byte, 1> message{std::byte{0x7F}};
  CHECK(client
            ->Send(message, SendOptions{BackpressurePolicy::Block,
                                        Deadline::After(200ms)})
            .ok());
  std::array<std::byte, 1> destination{};
  CHECK(server->Receive(destination, Deadline::After(200ms)).ok());
  CHECK(destination == message);

  first->Close();
  CHECK(::access(config.path.c_str(), F_OK) != 0);
  return 0;
}

int ListenerOnlyUnlinksItsOwnFilesystemEntry() {
  UnixDomainSocketConfig config;
  config.path = SocketPath("path_identity");

  auto original_result = UnixDomainSocketListener::Bind(config);
  CHECK(original_result.ok());
  auto original = std::move(original_result).take_value();
  CHECK(::unlink(config.path.c_str()) == 0);

  auto replacement_result = UnixDomainSocketListener::Bind(config);
  CHECK(replacement_result.ok());
  auto replacement = std::move(replacement_result).take_value();
  CHECK(::access(config.path.c_str(), F_OK) == 0);

  original->Close();
  CHECK(::access(config.path.c_str(), F_OK) == 0);
  replacement->Close();
  CHECK(::access(config.path.c_str(), F_OK) != 0);
  return 0;
}

int PeerDisconnectIsTyped() {
  auto pair = MakePair("peer_dead");
  CHECK(pair != nullptr);

  pair->client->Close();
  std::array<std::byte, 1> destination{};
  const auto received =
      pair->server->Receive(destination, Deadline::After(200ms));
  CHECK(!received.ok());
  CHECK(received.status().code() == StatusCode::PeerDead);
  return 0;
}

int CloseInterruptsBlockedReceive() {
  auto pair = MakePair("close_interrupt");
  CHECK(pair != nullptr);

  auto blocked = std::async(std::launch::async, [&pair] {
    std::array<std::byte, 1> destination{};
    const auto received =
        pair->server->Receive(destination, Deadline::After(2s));
    return received.ok() ? StatusCode::Ok : received.status().code();
  });
  std::this_thread::sleep_for(20ms);
  CHECK(blocked.wait_for(0ms) == std::future_status::timeout);

  pair->server->Close();
  CHECK(blocked.wait_for(500ms) == std::future_status::ready);
  CHECK(blocked.get() == StatusCode::Closed);
  return 0;
}

int DropAndTimeoutPoliciesBoundBackpressure() {
  constexpr std::uint32_t message_size = 4096U;
  auto pair = MakePair("backpressure", message_size, 4096U);
  CHECK(pair != nullptr);

  const std::vector<std::byte> message(message_size, std::byte{0x6B});
  bool observed_drop = false;
  for (std::size_t attempt = 0; attempt < 10'000U; ++attempt) {
    const auto sent =
        pair->client->Send(message,
                           SendOptions{BackpressurePolicy::Drop,
                                       Deadline::Immediate()});
    if (sent.code() == StatusCode::Dropped) {
      observed_drop = true;
      break;
    }
    CHECK(sent.ok());
  }
  CHECK(observed_drop);
  CHECK(pair->client->Stats().sent_messages > 0U);
  CHECK(pair->client->Stats().dropped_messages == 1U);

  const auto timed_out =
      pair->client->Send(message,
                         SendOptions{BackpressurePolicy::Timeout,
                                     Deadline::After(20ms)});
  CHECK(timed_out.code() == StatusCode::Timeout);
  CHECK(pair->client->Stats().send_timeouts == 1U);
  return 0;
}

int MalformedFrameIsRejectedAndCounted() {
  UnixDomainSocketConfig config;
  config.path = SocketPath("malformed");
  config.max_message_size = 1024U;

  auto listener_result = UnixDomainSocketListener::Bind(config);
  CHECK(listener_result.ok());
  auto listener = std::move(listener_result).take_value();

  ScopedFd raw_client(
      ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0));
  CHECK(raw_client.get() >= 0);

  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, config.path.data(), config.path.size());
  address.sun_path[config.path.size()] = '\0';
  const auto address_length = static_cast<socklen_t>(
      offsetof(sockaddr_un, sun_path) + config.path.size() + 1U);
  CHECK(::connect(raw_client.get(),
                  reinterpret_cast<const sockaddr*>(&address),
                  address_length) == 0);

  auto server_result = listener->Accept(Deadline::After(200ms));
  CHECK(server_result.ok());
  auto server = std::move(server_result).take_value();

  const std::array<std::byte, 4> malformed{
      std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}, std::byte{0xEF}};
  CHECK(::send(raw_client.get(), malformed.data(), malformed.size(),
               MSG_NOSIGNAL) ==
        static_cast<ssize_t>(malformed.size()));

  std::array<std::byte, 1024> destination{};
  const auto received =
      server->Receive(destination, Deadline::After(200ms));
  CHECK(!received.ok());
  CHECK(received.status().code() == StatusCode::CorruptData);
  CHECK(server->Stats().corrupt_messages == 1U);
  return 0;
}

int UnsealedDescriptorIsRejected() {
  UnixDomainSocketConfig config;
  config.path = SocketPath("unsealed");
  config.max_message_size = 1024U;

  auto listener_result = UnixDomainSocketListener::Bind(config);
  CHECK(listener_result.ok());
  auto listener = std::move(listener_result).take_value();

  ScopedFd raw_client(
      ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0));
  CHECK(raw_client.get() >= 0);
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, config.path.data(),
              config.path.size());
  address.sun_path[config.path.size()] = '\0';
  const auto address_length = static_cast<socklen_t>(
      offsetof(sockaddr_un, sun_path) + config.path.size() + 1U);
  CHECK(::connect(raw_client.get(),
                  reinterpret_cast<const sockaddr*>(&address),
                  address_length) == 0);

  auto server_result = listener->Accept(Deadline::After(200ms));
  CHECK(server_result.ok());
  auto server = std::move(server_result).take_value();

  ScopedFd payload(static_cast<int>(
      ::syscall(SYS_memfd_create, "fastipc-unsealed",
                MFD_CLOEXEC | MFD_ALLOW_SEALING)));
  CHECK(payload.get() >= 0);
  constexpr std::array<std::byte, 4> contents{
      std::byte{0x01}, std::byte{0x02},
      std::byte{0x03}, std::byte{0x04}};
  CHECK(::ftruncate(payload.get(),
                    static_cast<off_t>(contents.size())) == 0);
  CHECK(::pwrite(payload.get(), contents.data(), contents.size(), 0) ==
        static_cast<ssize_t>(contents.size()));

  RawWireHeader header;
  header.flags = 1U;
  header.payload_bytes =
      static_cast<std::uint32_t>(contents.size());
  iovec vector{};
  vector.iov_base = &header;
  vector.iov_len = sizeof(header);
  alignas(cmsghdr)
      std::array<std::byte, CMSG_SPACE(sizeof(int))> control{};
  msghdr packet{};
  packet.msg_iov = &vector;
  packet.msg_iovlen = 1U;
  packet.msg_control = control.data();
  packet.msg_controllen = control.size();
  cmsghdr* rights = CMSG_FIRSTHDR(&packet);
  rights->cmsg_level = SOL_SOCKET;
  rights->cmsg_type = SCM_RIGHTS;
  rights->cmsg_len = CMSG_LEN(sizeof(int));
  const int descriptor = payload.get();
  std::memcpy(CMSG_DATA(rights), &descriptor, sizeof(descriptor));
  CHECK(::sendmsg(raw_client.get(), &packet, MSG_NOSIGNAL) ==
        static_cast<ssize_t>(sizeof(header)));

  std::array<std::byte, 4> destination{};
  const auto received =
      server->Receive(destination, Deadline::After(200ms));
  CHECK(!received.ok());
  CHECK(received.status().code() == StatusCode::CorruptData);
  CHECK(server->Stats().corrupt_messages == 1U);
  return 0;
}

int BacklogPressureUsesConnectDeadline() {
  UnixDomainSocketConfig config;
  config.path = SocketPath("connect_backlog");

  auto listener_result = UnixDomainSocketListener::Bind(config);
  CHECK(listener_result.ok());
  auto listener = std::move(listener_result).take_value();

  std::vector<std::unique_ptr<UnixDomainSocketTransport>> pending;
  bool observed_timeout = false;
  for (std::size_t attempt = 0; attempt < 256U; ++attempt) {
    auto connection =
        UnixDomainSocketTransport::Connect(config,
                                           Deadline::Immediate());
    if (connection.ok()) {
      pending.push_back(std::move(connection).take_value());
      continue;
    }
    CHECK(connection.status().code() == StatusCode::Timeout);
    observed_timeout = true;
    break;
  }
  CHECK(observed_timeout);
  return 0;
}

int InvalidConfigMissingPeerAndAcceptTimeoutAreTyped() {
  UnixDomainSocketConfig relative;
  relative.path = "relative.sock";
  const auto invalid = UnixDomainSocketListener::Bind(relative);
  CHECK(!invalid.ok());
  CHECK(invalid.status().code() == StatusCode::InvalidArgument);

  UnixDomainSocketConfig missing;
  missing.path = SocketPath("missing");
  const auto unavailable =
      UnixDomainSocketTransport::Connect(missing, Deadline::Immediate());
  CHECK(!unavailable.ok());
  CHECK(unavailable.status().code() == StatusCode::PeerUnavailable);

  UnixDomainSocketConfig idle;
  idle.path = SocketPath("accept_timeout");
  auto listener_result = UnixDomainSocketListener::Bind(idle);
  CHECK(listener_result.ok());
  auto listener = std::move(listener_result).take_value();
  const auto accepted = listener->Accept(Deadline::After(20ms));
  CHECK(!accepted.ok());
  CHECK(accepted.status().code() == StatusCode::Timeout);
  return 0;
}

struct TestCase {
  const char* name;
  int (*run)();
};

}  // namespace

int main() {
  constexpr std::array tests{
      TestCase{"round trip", RoundTripUsesTransportSeam},
      TestCase{"receive deadline", ReceiveUsesAbsoluteDeadline},
      TestCase{"buffer preservation", SmallBufferPreservesPacketAndEmptyMessages},
      TestCase{"one megabyte", OneMegabyteMessageUsesLargePayloadPath},
      TestCase{"path collision", ExistingPathIsNeverUnlinkedByFailedBind},
      TestCase{"path identity", ListenerOnlyUnlinksItsOwnFilesystemEntry},
      TestCase{"peer disconnect", PeerDisconnectIsTyped},
      TestCase{"close interrupt", CloseInterruptsBlockedReceive},
      TestCase{"backpressure", DropAndTimeoutPoliciesBoundBackpressure},
      TestCase{"malformed frame", MalformedFrameIsRejectedAndCounted},
      TestCase{"unsealed descriptor", UnsealedDescriptorIsRejected},
      TestCase{"connect backlog", BacklogPressureUsesConnectDeadline},
      TestCase{"typed setup failures",
               InvalidConfigMissingPeerAndAcceptTimeoutAreTyped},
  };

  for (const auto& test : tests) {
    std::cout << "[ RUN      ] " << test.name << '\n';
    if (test.run() != 0) {
      std::cerr << "[  FAILED  ] " << test.name << '\n';
      return 1;
    }
    std::cout << "[       OK ] " << test.name << '\n';
  }
  return 0;
}
