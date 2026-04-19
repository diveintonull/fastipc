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

class SharedMemoryTransport;

class PublisherLoan {
 public:
  PublisherLoan() = default;
  ~PublisherLoan();

  PublisherLoan(const PublisherLoan&) = delete;
  PublisherLoan& operator=(const PublisherLoan&) = delete;
  PublisherLoan(PublisherLoan&& other) noexcept;
  PublisherLoan& operator=(PublisherLoan&& other) noexcept;

  [[nodiscard]] std::span<std::byte> Data() noexcept;
  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] explicit operator bool() const noexcept;

  Status Publish() noexcept;
  Status Abandon() noexcept;

 private:
  PublisherLoan(
      std::shared_ptr<void> transport, void* slot,
      std::size_t payload_size, std::uint64_t cursor,
      std::uint64_t chunk_generation,
      std::uint64_t owner_generation,
      std::uint64_t owner_role_token,
      std::uint64_t owner_channel_generation);

  std::shared_ptr<void> transport_;
  void* slot_{nullptr};
  std::size_t payload_size_{0U};
  std::uint64_t cursor_{0U};
  std::uint64_t chunk_generation_{0U};
  std::uint64_t owner_generation_{0U};
  std::uint64_t owner_role_token_{0U};
  std::uint64_t owner_channel_generation_{0U};
  bool active_{false};
  friend class SharedMemoryTransport;
};

class SubscriberSample {
 public:
  SubscriberSample() = default;
  ~SubscriberSample();

  SubscriberSample(const SubscriberSample&) = delete;
  SubscriberSample& operator=(const SubscriberSample&) = delete;
  SubscriberSample(SubscriberSample&& other) noexcept;
  SubscriberSample& operator=(SubscriberSample&& other) noexcept;

  [[nodiscard]] std::span<const std::byte> Data() const noexcept;
  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] explicit operator bool() const noexcept;

  Status Release() noexcept;

 private:
  SubscriberSample(
      std::shared_ptr<void> transport, void* slot,
      std::size_t payload_size, std::uint64_t cursor,
      std::uint64_t chunk_generation,
      std::uint64_t owner_generation,
      std::uint64_t owner_role_token,
      std::uint64_t owner_channel_generation);
  Status Requeue() noexcept;

  std::shared_ptr<void> transport_;
  void* slot_{nullptr};
  std::size_t payload_size_{0U};
  std::uint64_t cursor_{0U};
  std::uint64_t chunk_generation_{0U};
  std::uint64_t owner_generation_{0U};
  std::uint64_t owner_role_token_{0U};
  std::uint64_t owner_channel_generation_{0U};
  bool active_{false};
  friend class SharedMemoryTransport;
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
  [[nodiscard]] Result<PublisherLoan> Loan(
      std::size_t size, SendOptions options = {});
  [[nodiscard]] Result<SubscriberSample> Take(
      Deadline deadline = Deadline::Infinite());
  [[nodiscard]] TransportStats Stats() const noexcept override;
  void Close() noexcept override;

  [[nodiscard]] std::uint64_t generation() const noexcept;

 private:
  struct Impl;
  friend class PublisherLoan;
  friend class SubscriberSample;
  [[nodiscard]] static Result<std::unique_ptr<Impl>> CreateProducerImpl(
      const ChannelConfig& config);
  [[nodiscard]] static Result<std::unique_ptr<Impl>> OpenConsumerImpl(
      const ChannelConfig& config);

  explicit SharedMemoryTransport(std::shared_ptr<Impl> impl);

  std::shared_ptr<Impl> impl_;
};

}  // namespace fastipc
