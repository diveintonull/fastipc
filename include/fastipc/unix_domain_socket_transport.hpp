#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <fastipc/transport.hpp>

namespace fastipc {

struct UnixDomainSocketConfig {
  std::string path;
  std::uint32_t max_message_size{1024U * 1024U};
  std::uint32_t permissions{0600};
  std::uint32_t socket_buffer_bytes{0};
  bool unlink_on_listener_close{true};
};

class UnixDomainSocketTransport final : public Transport {
 public:
  [[nodiscard]] static Result<std::unique_ptr<UnixDomainSocketTransport>>
  Connect(const UnixDomainSocketConfig& config, Deadline deadline);

  ~UnixDomainSocketTransport() override;

  UnixDomainSocketTransport(const UnixDomainSocketTransport&) = delete;
  UnixDomainSocketTransport& operator=(const UnixDomainSocketTransport&) =
      delete;
  UnixDomainSocketTransport(UnixDomainSocketTransport&&) = delete;
  UnixDomainSocketTransport& operator=(UnixDomainSocketTransport&&) = delete;

  Status Send(std::span<const std::byte> message,
              SendOptions options) override;
  Result<std::size_t> Receive(std::span<std::byte> destination,
                              Deadline deadline) override;
  [[nodiscard]] TransportStats Stats() const noexcept override;
  void Close() noexcept override;

 private:
  struct Impl;
  explicit UnixDomainSocketTransport(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;

  friend class UnixDomainSocketListener;
};

class UnixDomainSocketListener final {
 public:
  [[nodiscard]] static Result<std::unique_ptr<UnixDomainSocketListener>>
  Bind(const UnixDomainSocketConfig& config);

  ~UnixDomainSocketListener();

  UnixDomainSocketListener(const UnixDomainSocketListener&) = delete;
  UnixDomainSocketListener& operator=(const UnixDomainSocketListener&) =
      delete;
  UnixDomainSocketListener(UnixDomainSocketListener&&) = delete;
  UnixDomainSocketListener& operator=(UnixDomainSocketListener&&) = delete;

  [[nodiscard]] Result<std::unique_ptr<UnixDomainSocketTransport>> Accept(
      Deadline deadline);
  [[nodiscard]] std::string_view path() const noexcept;
  void Close() noexcept;

 private:
  struct Impl;
  explicit UnixDomainSocketListener(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

}  // namespace fastipc
