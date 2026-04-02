#include <fastipc/shared_memory_transport.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <span>
#include <string>
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

int SharedMemoryTransportsExchangeOneMessage() {
  using namespace std::chrono_literals;
  using fastipc::BackpressurePolicy;
  using fastipc::ChannelConfig;
  using fastipc::Deadline;
  using fastipc::SendOptions;
  using fastipc::SharedMemoryTransport;

  ChannelConfig config;
  config.name = "fastipc_transport_" + std::to_string(::getpid());
  config.slot_count = 4;
  config.max_message_size = 64;
  config.unlink_on_owner_close = true;

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

}  // namespace

int main() {
  return SharedMemoryTransportsExchangeOneMessage();
}
