#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <fastipc/transport.hpp>

namespace fastipc {

struct MpmcChannelConfig {
  std::string name;
  std::uint32_t capacity{64U};
  std::uint32_t max_message_size{4096U};
  std::uint32_t active_spin_count{256U};
  std::uint32_t permissions{0600U};
  bool unlink_on_owner_close{false};
};

class MpmcSharedMemoryTransport final : public Transport {
 public:
  [[nodiscard]] static Result<
      std::unique_ptr<MpmcSharedMemoryTransport>>
  Create(const MpmcChannelConfig& config);

  [[nodiscard]] static Result<
      std::unique_ptr<MpmcSharedMemoryTransport>>
  Open(const MpmcChannelConfig& config);

  ~MpmcSharedMemoryTransport() override;

  MpmcSharedMemoryTransport(const MpmcSharedMemoryTransport&) = delete;
  MpmcSharedMemoryTransport& operator=(
      const MpmcSharedMemoryTransport&) = delete;
  MpmcSharedMemoryTransport(MpmcSharedMemoryTransport&&) = delete;
  MpmcSharedMemoryTransport& operator=(
      MpmcSharedMemoryTransport&&) = delete;

  Status Send(std::span<const std::byte> message,
              SendOptions options) override;
  Result<std::size_t> Receive(
      std::span<std::byte> destination,
      Deadline deadline) override;
  [[nodiscard]] TransportStats Stats() const noexcept override;
  void Close() noexcept override;

 private:
  struct Impl;

  [[nodiscard]] static Result<std::unique_ptr<Impl>> CreateImpl(
      const MpmcChannelConfig& config);
  [[nodiscard]] static Result<std::unique_ptr<Impl>> OpenImpl(
      const MpmcChannelConfig& config);
  explicit MpmcSharedMemoryTransport(
      std::shared_ptr<Impl> impl);

  std::shared_ptr<Impl> impl_;
};

}  // namespace fastipc
