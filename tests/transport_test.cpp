#include <fastipc/shared_memory_transport.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <unistd.h>

namespace {

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

fastipc::ChannelConfig ConfigFor(const std::string& test_name) {
  fastipc::ChannelConfig config;
  config.name = test_name + "_" + std::to_string(::getpid());
  config.slot_count = 4;
  config.max_message_size = 64;
  config.unlink_on_owner_close = true;
  return config;
}

int SharedMemoryTransportsExchangeOneMessage() {
  using namespace std::chrono_literals;
  using fastipc::BackpressurePolicy;
  using fastipc::Deadline;
  using fastipc::SendOptions;
  using fastipc::SharedMemoryTransport;

  const auto config = ConfigFor("fastipc_exchange");
  auto producer_result = SharedMemoryTransport::CreateProducer(config);
  CHECK(producer_result.ok());
  auto producer = std::move(producer_result).take_value();

  auto consumer_result = SharedMemoryTransport::OpenConsumer(config);
  CHECK(consumer_result.ok());
  auto consumer = std::move(consumer_result).take_value();

  const std::array<std::byte, 4> payload{
      std::byte{0x10}, std::byte{0x20}, std::byte{0x30}, std::byte{0x40}};
  const auto send_status = producer->Send(
      std::span<const std::byte>(payload),
      SendOptions{BackpressurePolicy::Block, Deadline::After(100ms)});
  CHECK(send_status.ok());

  std::array<std::byte, 64> destination{};
  auto receive_result =
      consumer->Receive(std::span<std::byte>(destination), Deadline::After(100ms));
  CHECK(receive_result.ok());
  CHECK(receive_result.value() == payload.size());

  for (std::size_t index = 0; index < payload.size(); ++index) {
    CHECK(destination[index] == payload[index]);
  }
  return 0;
}

int ConsumerSleepsUntilProducerPublishes() {
  using namespace std::chrono_literals;
  using fastipc::BackpressurePolicy;
  using fastipc::Deadline;
  using fastipc::SendOptions;
  using fastipc::SharedMemoryTransport;

  const auto config = ConfigFor("fastipc_wait_empty");
  auto producer_result = SharedMemoryTransport::CreateProducer(config);
  CHECK(producer_result.ok());
  auto producer = std::move(producer_result).take_value();

  auto consumer_result = SharedMemoryTransport::OpenConsumer(config);
  CHECK(consumer_result.ok());
  auto consumer = std::move(consumer_result).take_value();

  const std::array<std::byte, 3> payload{
      std::byte{0xA1}, std::byte{0xB2}, std::byte{0xC3}};
  std::jthread delayed_producer([&producer, &payload] {
    using namespace std::chrono_literals;
    std::this_thread::sleep_for(20ms);
    const auto status = producer->Send(
        std::span<const std::byte>(payload),
        SendOptions{BackpressurePolicy::Block, Deadline::After(200ms)});
    if (!status.ok()) {
      std::cerr << "delayed producer failed: " << status.ToString() << '\n';
    }
  });

  std::array<std::byte, 64> destination{};
  auto receive_result =
      consumer->Receive(std::span<std::byte>(destination), Deadline::After(500ms));
  CHECK(receive_result.ok());
  CHECK(receive_result.value() == payload.size());
  CHECK(destination[0] == payload[0]);
  CHECK(destination[1] == payload[1]);
  CHECK(destination[2] == payload[2]);
  return 0;
}

int ProducerSleepsUntilConsumerFreesSlot() {
  using namespace std::chrono_literals;
  using fastipc::BackpressurePolicy;
  using fastipc::Deadline;
  using fastipc::SendOptions;
  using fastipc::SharedMemoryTransport;

  auto config = ConfigFor("fastipc_wait_full");
  config.slot_count = 2;
  auto producer_result = SharedMemoryTransport::CreateProducer(config);
  CHECK(producer_result.ok());
  auto producer = std::move(producer_result).take_value();
  auto consumer_result = SharedMemoryTransport::OpenConsumer(config);
  CHECK(consumer_result.ok());
  auto consumer = std::move(consumer_result).take_value();

  const std::array<std::byte, 1> first{std::byte{0x01}};
  const std::array<std::byte, 1> second{std::byte{0x02}};
  const std::array<std::byte, 1> third{std::byte{0x03}};
  CHECK(producer
            ->Send(first, SendOptions{BackpressurePolicy::Block,
                                      Deadline::After(100ms)})
            .ok());
  CHECK(producer
            ->Send(second, SendOptions{BackpressurePolicy::Block,
                                       Deadline::After(100ms)})
            .ok());

  std::jthread delayed_consumer([&consumer] {
    using namespace std::chrono_literals;
    std::this_thread::sleep_for(20ms);
    std::array<std::byte, 64> destination{};
    const auto result =
        consumer->Receive(destination, Deadline::After(200ms));
    if (!result.ok()) {
      std::cerr << "delayed consumer failed: "
                << result.status().ToString() << '\n';
    }
  });

  CHECK(producer
            ->Send(third, SendOptions{BackpressurePolicy::Block,
                                      Deadline::After(500ms)})
            .ok());
  return 0;
}

int EmptyReceiveHonorsAbsoluteDeadline() {
  using namespace std::chrono_literals;
  using fastipc::Deadline;
  using fastipc::SharedMemoryTransport;
  using fastipc::StatusCode;

  const auto config = ConfigFor("fastipc_receive_timeout");
  auto producer_result = SharedMemoryTransport::CreateProducer(config);
  CHECK(producer_result.ok());
  auto producer = std::move(producer_result).take_value();
  auto consumer_result = SharedMemoryTransport::OpenConsumer(config);
  CHECK(consumer_result.ok());
  auto consumer = std::move(consumer_result).take_value();

  std::array<std::byte, 64> destination{};
  const auto started = std::chrono::steady_clock::now();
  const auto result =
      consumer->Receive(destination, Deadline::After(30ms));
  const auto elapsed = std::chrono::steady_clock::now() - started;

  CHECK(!result.ok());
  CHECK(result.status().code() == StatusCode::Timeout);
  CHECK(elapsed >= 10ms);
  CHECK(elapsed < 500ms);
  return 0;
}

int EpochProtocolDoesNotLoseWakeups() {
  using namespace std::chrono_literals;
  using fastipc::BackpressurePolicy;
  using fastipc::Deadline;
  using fastipc::SendOptions;
  using fastipc::SharedMemoryTransport;

  auto config = ConfigFor("fastipc_epoch_stress");
  config.slot_count = 2;
  auto producer_result = SharedMemoryTransport::CreateProducer(config);
  CHECK(producer_result.ok());
  auto producer = std::move(producer_result).take_value();
  auto consumer_result = SharedMemoryTransport::OpenConsumer(config);
  CHECK(consumer_result.ok());
  auto consumer = std::move(consumer_result).take_value();

  constexpr std::size_t message_count = 2000;
  std::size_t consumer_failures = 0;
  std::jthread consumer_thread([&consumer, &consumer_failures] {
    using namespace std::chrono_literals;
    std::array<std::byte, 64> destination{};
    for (std::size_t index = 0; index < message_count; ++index) {
      const auto result =
          consumer->Receive(destination, Deadline::After(2s));
      if (!result.ok() || result.value() != sizeof(std::uint64_t)) {
        ++consumer_failures;
        return;
      }
      std::uint64_t observed = 0;
      std::memcpy(&observed, destination.data(), sizeof(observed));
      if (observed != index) {
        ++consumer_failures;
        return;
      }
    }
  });

  for (std::uint64_t index = 0; index < message_count; ++index) {
    const auto* begin = reinterpret_cast<const std::byte*>(&index);
    const auto status = producer->Send(
        std::span<const std::byte>(begin, sizeof(index)),
        SendOptions{BackpressurePolicy::Block, Deadline::After(2s)});
    CHECK(status.ok());
  }
  consumer_thread.join();
  CHECK(consumer_failures == 0);
  return 0;
}

}  // namespace

int main() {
  if (const int result = SharedMemoryTransportsExchangeOneMessage();
      result != 0) {
    return result;
  }
  if (const int result = ConsumerSleepsUntilProducerPublishes(); result != 0) {
    return result;
  }
  if (const int result = ProducerSleepsUntilConsumerFreesSlot(); result != 0) {
    return result;
  }
  if (const int result = EmptyReceiveHonorsAbsoluteDeadline(); result != 0) {
    return result;
  }
  return EpochProtocolDoesNotLoseWakeups();
}
