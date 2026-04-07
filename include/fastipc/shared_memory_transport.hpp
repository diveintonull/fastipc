#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include <fastipc/transport.hpp>

namespace fastipc {

struct ChannelConfig {
  std::string name;
  std::uint32_t slot_count{64};
  std::uint32_t max_message_size{4096};
  std::uint32_t active_spin_count{256};
  std::uint32_t permissions{0600};
  bool unlink_on_owner_close{false};
  std::chrono::milliseconds peer_timeout{1000};
};

class SharedMemoryTransport final : public Transport {
 public:
  [[nodiscard]] static Result<std::unique_ptr<SharedMemoryTransport>>
  CreateProducer(const ChannelConfig& config);

  [[nodiscard]] static Result<std::unique_ptr<SharedMemoryTransport>>
  OpenConsumer(const ChannelConfig& config);

  ~SharedMemoryTransport() override;

  SharedMemoryTransport(const SharedMemoryTransport&) = delete;
  SharedMemoryTransport& operator=(const SharedMemoryTransport&) = delete;
  SharedMemoryTransport(SharedMemoryTransport&&) = delete;
  SharedMemoryTransport& operator=(SharedMemoryTransport&&) = delete;

  Status Send(std::span<const std::byte> message,
              SendOptions options) override;
  Result<std::size_t> Receive(std::span<std::byte> destination,
                              Deadline deadline) override;
  [[nodiscard]] TransportStats Stats() const noexcept override;
  void Close() noexcept override;

  [[nodiscard]] std::uint64_t generation() const noexcept;

 private:
  struct Impl;
  [[nodiscard]] static Result<std::unique_ptr<Impl>> CreateProducerImpl(
      const ChannelConfig& config);
  [[nodiscard]] static Result<std::unique_ptr<Impl>> OpenConsumerImpl(
      const ChannelConfig& config);

  explicit SharedMemoryTransport(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

}  // namespace fastipc
