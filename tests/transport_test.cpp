#include <fastipc/shared_memory_transport.hpp>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <signal.h>
#include <sys/wait.h>
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

int ProducerRestartAdvancesGenerationAndRestoresFlow() {
  using namespace std::chrono_literals;
  using fastipc::BackpressurePolicy;
  using fastipc::Deadline;
  using fastipc::SendOptions;
  using fastipc::SharedMemoryTransport;

  auto config = ConfigFor("fastipc_producer_restart");
  config.unlink_on_owner_close = false;

  auto first_result = SharedMemoryTransport::CreateProducer(config);
  CHECK(first_result.ok());
  auto first_producer = std::move(first_result).take_value();
  auto consumer_result = SharedMemoryTransport::OpenConsumer(config);
  CHECK(consumer_result.ok());
  auto consumer = std::move(consumer_result).take_value();

  const auto first_generation = first_producer->generation();
  CHECK(first_generation != 0);
  first_producer->Close();

  auto restart_config = config;
  restart_config.unlink_on_owner_close = true;
  auto restarted_result =
      SharedMemoryTransport::CreateProducer(restart_config);
  CHECK(restarted_result.ok());
  auto restarted_producer = std::move(restarted_result).take_value();
  CHECK(restarted_producer->generation() > first_generation);

  const std::array<std::byte, 2> payload{
      std::byte{0x55}, std::byte{0xAA}};
  CHECK(restarted_producer
            ->Send(payload, SendOptions{BackpressurePolicy::Block,
                                        Deadline::After(200ms)})
            .ok());

  std::array<std::byte, 64> destination{};
  const auto receive =
      consumer->Receive(destination, Deadline::After(200ms));
  CHECK(receive.ok());
  CHECK(receive.value() == payload.size());
  CHECK(destination[0] == payload[0]);
  CHECK(destination[1] == payload[1]);
  CHECK(consumer->generation() == restarted_producer->generation());
  return 0;
}

int DuplicateProducerCannotUnlinkLiveChannel() {
  using fastipc::SharedMemoryTransport;
  using fastipc::StatusCode;

  auto config = ConfigFor("fastipc_duplicate_producer");
  auto producer_result = SharedMemoryTransport::CreateProducer(config);
  CHECK(producer_result.ok());
  auto producer = std::move(producer_result).take_value();

  auto conflicting_config = config;
  conflicting_config.unlink_on_owner_close = true;
  const auto conflict =
      SharedMemoryTransport::CreateProducer(conflicting_config);
  CHECK(!conflict.ok());
  CHECK(conflict.status().code() == StatusCode::RoleConflict);

  auto consumer_result = SharedMemoryTransport::OpenConsumer(config);
  CHECK(consumer_result.ok());
  auto consumer = std::move(consumer_result).take_value();
  producer->Close();
  consumer->Close();
  return 0;
}

int KilledProducerIsDetectedAndReclaimed() {
  using namespace std::chrono_literals;
  using fastipc::BackpressurePolicy;
  using fastipc::Deadline;
  using fastipc::SendOptions;
  using fastipc::SharedMemoryTransport;
  using fastipc::StatusCode;

  auto config = ConfigFor("fastipc_killed_producer");
  config.unlink_on_owner_close = false;
  config.peer_timeout = 80ms;

  int ready_pipe[2]{-1, -1};
  CHECK(::pipe(ready_pipe) == 0);
  const pid_t child = ::fork();
  CHECK(child >= 0);
  if (child == 0) {
    ::close(ready_pipe[0]);
    auto producer_result = SharedMemoryTransport::CreateProducer(config);
    if (!producer_result.ok()) {
      ::_exit(10);
    }
    auto producer = std::move(producer_result).take_value();
    const char ready = 'R';
    if (::write(ready_pipe[1], &ready, 1) != 1) {
      ::_exit(11);
    }
    for (;;) {
      ::pause();
    }
  }

  ::close(ready_pipe[1]);
  char ready = 0;
  ssize_t bytes_read = -1;
  do {
    bytes_read = ::read(ready_pipe[0], &ready, 1);
  } while (bytes_read < 0 && errno == EINTR);
  ::close(ready_pipe[0]);
  CHECK(bytes_read == 1);
  CHECK(ready == 'R');

  auto consumer_result = SharedMemoryTransport::OpenConsumer(config);
  CHECK(consumer_result.ok());
  auto consumer = std::move(consumer_result).take_value();
  const auto first_generation = consumer->generation();

  CHECK(::kill(child, SIGKILL) == 0);
  int child_status = 0;
  CHECK(::waitpid(child, &child_status, 0) == child);
  CHECK(WIFSIGNALED(child_status));

  std::array<std::byte, 64> destination{};
  const auto dead_receive =
      consumer->Receive(destination, Deadline::After(2s));
  CHECK(!dead_receive.ok());
  CHECK(dead_receive.status().code() == StatusCode::PeerDead);

  auto restart_config = config;
  restart_config.unlink_on_owner_close = true;
  auto restarted_result =
      SharedMemoryTransport::CreateProducer(restart_config);
  CHECK(restarted_result.ok());
  auto producer = std::move(restarted_result).take_value();
  CHECK(producer->generation() > first_generation);

  const std::array<std::byte, 1> payload{std::byte{0xC7}};
  CHECK(producer
            ->Send(payload, SendOptions{BackpressurePolicy::Block,
                                        Deadline::After(200ms)})
            .ok());
  const auto receive =
      consumer->Receive(destination, Deadline::After(200ms));
  CHECK(receive.ok());
  CHECK(receive.value() == payload.size());
  CHECK(destination[0] == payload[0]);
  return 0;
}

int KilledConsumerIsDetectedAndReclaimed() {
  using namespace std::chrono_literals;
  using fastipc::BackpressurePolicy;
  using fastipc::Deadline;
  using fastipc::SendOptions;
  using fastipc::SharedMemoryTransport;
  using fastipc::StatusCode;

  auto config = ConfigFor("fastipc_killed_consumer");
  config.peer_timeout = 80ms;

  int start_pipe[2]{-1, -1};
  int ready_pipe[2]{-1, -1};
  CHECK(::pipe(start_pipe) == 0);
  CHECK(::pipe(ready_pipe) == 0);
  const pid_t child = ::fork();
  CHECK(child >= 0);
  if (child == 0) {
    ::close(start_pipe[1]);
    ::close(ready_pipe[0]);
    char start_signal = 0;
    if (::read(start_pipe[0], &start_signal, 1) != 1) {
      ::_exit(20);
    }
    auto consumer_result = SharedMemoryTransport::OpenConsumer(config);
    if (!consumer_result.ok()) {
      ::_exit(21);
    }
    auto consumer = std::move(consumer_result).take_value();
    const char ready = 'R';
    if (::write(ready_pipe[1], &ready, 1) != 1) {
      ::_exit(22);
    }
    for (;;) {
      ::pause();
    }
  }

  ::close(start_pipe[0]);
  ::close(ready_pipe[1]);
  auto producer_result = SharedMemoryTransport::CreateProducer(config);
  CHECK(producer_result.ok());
  auto producer = std::move(producer_result).take_value();
  const char start_signal = 'S';
  CHECK(::write(start_pipe[1], &start_signal, 1) == 1);
  ::close(start_pipe[1]);

  char ready = 0;
  CHECK(::read(ready_pipe[0], &ready, 1) == 1);
  ::close(ready_pipe[0]);
  CHECK(ready == 'R');

  CHECK(::kill(child, SIGKILL) == 0);
  int child_status = 0;
  CHECK(::waitpid(child, &child_status, 0) == child);
  CHECK(WIFSIGNALED(child_status));

  for (std::uint8_t value = 0; value < config.slot_count; ++value) {
    const std::array<std::byte, 1> payload{static_cast<std::byte>(value)};
    CHECK(producer
              ->Send(payload, SendOptions{BackpressurePolicy::Block,
                                          Deadline::After(200ms)})
              .ok());
  }

  const std::array<std::byte, 1> blocked_payload{std::byte{0xEE}};
  const auto dead_send =
      producer->Send(blocked_payload,
                     SendOptions{BackpressurePolicy::Block,
                                 Deadline::After(2s)});
  CHECK(!dead_send.ok());
  CHECK(dead_send.code() == StatusCode::PeerDead);

  auto replacement_result = SharedMemoryTransport::OpenConsumer(config);
  CHECK(replacement_result.ok());
  auto replacement = std::move(replacement_result).take_value();
  CHECK(replacement->generation() == producer->generation());

  std::array<std::byte, 64> destination{};
  for (std::uint8_t value = 0; value < config.slot_count; ++value) {
    const auto received =
        replacement->Receive(destination, Deadline::After(200ms));
    CHECK(received.ok());
    CHECK(received.value() == 1);
    CHECK(destination[0] == static_cast<std::byte>(value));
  }

  CHECK(producer
            ->Send(blocked_payload,
                   SendOptions{BackpressurePolicy::Block,
                               Deadline::After(200ms)})
            .ok());
  const auto restored =
      replacement->Receive(destination, Deadline::After(200ms));
  CHECK(restored.ok());
  CHECK(destination[0] == blocked_payload[0]);
  return 0;
}

int GracefulCloseWakesInfinitePeerWait() {
  using namespace std::chrono_literals;
  using fastipc::Deadline;
  using fastipc::SharedMemoryTransport;
  using fastipc::StatusCode;

  const auto config = ConfigFor("fastipc_close_wake");
  auto producer_result = SharedMemoryTransport::CreateProducer(config);
  CHECK(producer_result.ok());
  auto producer = std::move(producer_result).take_value();
  auto consumer_result = SharedMemoryTransport::OpenConsumer(config);
  CHECK(consumer_result.ok());
  auto consumer = std::move(consumer_result).take_value();

  std::atomic<int> observed_code{-1};
  std::jthread waiting_consumer([&consumer, &observed_code] {
    std::array<std::byte, 64> destination{};
    const auto result = consumer->Receive(destination, Deadline::Infinite());
    observed_code.store(static_cast<int>(result.status().code()),
                        std::memory_order_relaxed);
  });
  std::this_thread::sleep_for(20ms);
  producer->Close();
  waiting_consumer.join();

  CHECK(observed_code.load(std::memory_order_relaxed) ==
        static_cast<int>(StatusCode::PeerUnavailable));
  return 0;
}

int StoppedProducerExpiresAndOldGenerationIsFenced() {
  using namespace std::chrono_literals;
  using fastipc::BackpressurePolicy;
  using fastipc::Deadline;
  using fastipc::SendOptions;
  using fastipc::SharedMemoryTransport;
  using fastipc::StatusCode;

  auto config = ConfigFor("fastipc_stopped_producer");
  config.unlink_on_owner_close = false;
  config.peer_timeout = 100ms;

  int ready_pipe[2]{-1, -1};
  int command_pipe[2]{-1, -1};
  int result_pipe[2]{-1, -1};
  CHECK(::pipe(ready_pipe) == 0);
  CHECK(::pipe(command_pipe) == 0);
  CHECK(::pipe(result_pipe) == 0);
  const pid_t child = ::fork();
  CHECK(child >= 0);
  if (child == 0) {
    ::close(ready_pipe[0]);
    ::close(command_pipe[1]);
    ::close(result_pipe[0]);
    auto producer_result = SharedMemoryTransport::CreateProducer(config);
    if (!producer_result.ok()) {
      ::_exit(30);
    }
    auto producer = std::move(producer_result).take_value();
    const char ready = 'R';
    if (::write(ready_pipe[1], &ready, 1) != 1) {
      ::_exit(31);
    }

    char command = 0;
    if (::read(command_pipe[0], &command, 1) != 1) {
      ::_exit(32);
    }
    const std::array<std::byte, 1> stale_payload{std::byte{0x51}};
    const auto stale_send =
        producer->Send(stale_payload,
                       SendOptions{BackpressurePolicy::Block,
                                   Deadline::After(200ms)});
    const auto code = static_cast<std::uint8_t>(stale_send.code());
    if (::write(result_pipe[1], &code, sizeof(code)) !=
        static_cast<ssize_t>(sizeof(code))) {
      ::_exit(33);
    }
    producer->Close();
    ::_exit(0);
  }

  ::close(ready_pipe[1]);
  ::close(command_pipe[0]);
  ::close(result_pipe[1]);
  char ready = 0;
  CHECK(::read(ready_pipe[0], &ready, 1) == 1);
  ::close(ready_pipe[0]);
  CHECK(ready == 'R');

  auto consumer_result = SharedMemoryTransport::OpenConsumer(config);
  CHECK(consumer_result.ok());
  auto consumer = std::move(consumer_result).take_value();
  const auto first_generation = consumer->generation();

  CHECK(::kill(child, SIGSTOP) == 0);
  int stop_status = 0;
  CHECK(::waitpid(child, &stop_status, WUNTRACED) == child);
  CHECK(WIFSTOPPED(stop_status));
  std::this_thread::sleep_for(300ms);

  auto replacement_config = config;
  replacement_config.unlink_on_owner_close = true;
  auto replacement_result =
      SharedMemoryTransport::CreateProducer(replacement_config);
  if (!replacement_result.ok()) {
    static_cast<void>(::kill(child, SIGKILL));
    static_cast<void>(::waitpid(child, nullptr, 0));
  }
  CHECK(replacement_result.ok());
  auto replacement = std::move(replacement_result).take_value();
  CHECK(replacement->generation() > first_generation);

  const std::array<std::byte, 1> fresh_payload{std::byte{0xA4}};
  CHECK(replacement
            ->Send(fresh_payload,
                   SendOptions{BackpressurePolicy::Block,
                               Deadline::After(200ms)})
            .ok());
  std::array<std::byte, 64> destination{};
  const auto fresh_receive =
      consumer->Receive(destination, Deadline::After(200ms));
  CHECK(fresh_receive.ok());
  CHECK(destination[0] == fresh_payload[0]);

  CHECK(::kill(child, SIGCONT) == 0);
  const char command = 'S';
  CHECK(::write(command_pipe[1], &command, 1) == 1);
  ::close(command_pipe[1]);

  std::uint8_t stale_code = 0;
  CHECK(::read(result_pipe[0], &stale_code, sizeof(stale_code)) ==
        static_cast<ssize_t>(sizeof(stale_code)));
  ::close(result_pipe[0]);
  int child_status = 0;
  CHECK(::waitpid(child, &child_status, 0) == child);
  CHECK(WIFEXITED(child_status));
  CHECK(WEXITSTATUS(child_status) == 0);
  CHECK(stale_code ==
        static_cast<std::uint8_t>(StatusCode::StaleGeneration));
  return 0;
}

int IdleHeartbeatsPreventFalseTakeover() {
  using namespace std::chrono_literals;
  using fastipc::SharedMemoryTransport;
  using fastipc::StatusCode;

  auto config = ConfigFor("fastipc_idle_heartbeat");
  config.peer_timeout = 200ms;
  auto producer_result = SharedMemoryTransport::CreateProducer(config);
  CHECK(producer_result.ok());
  auto producer = std::move(producer_result).take_value();
  auto consumer_result = SharedMemoryTransport::OpenConsumer(config);
  CHECK(consumer_result.ok());
  auto consumer = std::move(consumer_result).take_value();

  std::this_thread::sleep_for(600ms);
  const auto duplicate_producer =
      SharedMemoryTransport::CreateProducer(config);
  CHECK(!duplicate_producer.ok());
  CHECK(duplicate_producer.status().code() == StatusCode::RoleConflict);
  const auto duplicate_consumer =
      SharedMemoryTransport::OpenConsumer(config);
  CHECK(!duplicate_consumer.ok());
  CHECK(duplicate_consumer.status().code() == StatusCode::RoleConflict);
  return 0;
}

int StoppedConsumerExpiresAndOldRoleIsFenced() {
  using namespace std::chrono_literals;
  using fastipc::BackpressurePolicy;
  using fastipc::Deadline;
  using fastipc::SendOptions;
  using fastipc::SharedMemoryTransport;
  using fastipc::StatusCode;

  auto config = ConfigFor("fastipc_stopped_consumer");
  config.peer_timeout = 100ms;

  int start_pipe[2]{-1, -1};
  int ready_pipe[2]{-1, -1};
  int command_pipe[2]{-1, -1};
  int result_pipe[2]{-1, -1};
  CHECK(::pipe(start_pipe) == 0);
  CHECK(::pipe(ready_pipe) == 0);
  CHECK(::pipe(command_pipe) == 0);
  CHECK(::pipe(result_pipe) == 0);
  const pid_t child = ::fork();
  CHECK(child >= 0);
  if (child == 0) {
    ::close(start_pipe[1]);
    ::close(ready_pipe[0]);
    ::close(command_pipe[1]);
    ::close(result_pipe[0]);
    char start_signal = 0;
    if (::read(start_pipe[0], &start_signal, 1) != 1) {
      ::_exit(40);
    }
    auto consumer_result = SharedMemoryTransport::OpenConsumer(config);
    if (!consumer_result.ok()) {
      ::_exit(41);
    }
    auto consumer = std::move(consumer_result).take_value();
    const char ready = 'R';
    if (::write(ready_pipe[1], &ready, 1) != 1) {
      ::_exit(42);
    }
    char command = 0;
    if (::read(command_pipe[0], &command, 1) != 1) {
      ::_exit(43);
    }

    std::array<std::byte, 64> destination{};
    const auto stale_receive =
        consumer->Receive(destination, Deadline::After(200ms));
    const auto code =
        static_cast<std::uint8_t>(stale_receive.status().code());
    if (::write(result_pipe[1], &code, sizeof(code)) !=
        static_cast<ssize_t>(sizeof(code))) {
      ::_exit(44);
    }
    consumer->Close();
    ::_exit(0);
  }

  ::close(start_pipe[0]);
  ::close(ready_pipe[1]);
  ::close(command_pipe[0]);
  ::close(result_pipe[1]);
  auto producer_result = SharedMemoryTransport::CreateProducer(config);
  CHECK(producer_result.ok());
  auto producer = std::move(producer_result).take_value();
  const char start_signal = 'S';
  CHECK(::write(start_pipe[1], &start_signal, 1) == 1);
  ::close(start_pipe[1]);

  char ready = 0;
  CHECK(::read(ready_pipe[0], &ready, 1) == 1);
  ::close(ready_pipe[0]);
  CHECK(ready == 'R');

  CHECK(::kill(child, SIGSTOP) == 0);
  int stop_status = 0;
  CHECK(::waitpid(child, &stop_status, WUNTRACED) == child);
  CHECK(WIFSTOPPED(stop_status));
  std::this_thread::sleep_for(300ms);

  auto replacement_result = SharedMemoryTransport::OpenConsumer(config);
  if (!replacement_result.ok()) {
    static_cast<void>(::kill(child, SIGKILL));
    static_cast<void>(::waitpid(child, nullptr, 0));
  }
  CHECK(replacement_result.ok());
  auto replacement = std::move(replacement_result).take_value();
  CHECK(replacement->generation() == producer->generation());

  const std::array<std::byte, 1> payload{std::byte{0xB6}};
  CHECK(producer
            ->Send(payload, SendOptions{BackpressurePolicy::Block,
                                        Deadline::After(200ms)})
            .ok());
  std::array<std::byte, 64> destination{};
  const auto receive =
      replacement->Receive(destination, Deadline::After(200ms));
  CHECK(receive.ok());
  CHECK(destination[0] == payload[0]);

  CHECK(::kill(child, SIGCONT) == 0);
  const char command = 'R';
  CHECK(::write(command_pipe[1], &command, 1) == 1);
  ::close(command_pipe[1]);
  std::uint8_t stale_code = 0;
  CHECK(::read(result_pipe[0], &stale_code, sizeof(stale_code)) ==
        static_cast<ssize_t>(sizeof(stale_code)));
  ::close(result_pipe[0]);

  int child_status = 0;
  CHECK(::waitpid(child, &child_status, 0) == child);
  CHECK(WIFEXITED(child_status));
  CHECK(WEXITSTATUS(child_status) == 0);
  CHECK(stale_code ==
        static_cast<std::uint8_t>(StatusCode::StaleGeneration));
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
  if (const int result = EpochProtocolDoesNotLoseWakeups(); result != 0) {
    return result;
  }
  if (const int result = ProducerRestartAdvancesGenerationAndRestoresFlow();
      result != 0) {
    return result;
  }
  if (const int result = DuplicateProducerCannotUnlinkLiveChannel();
      result != 0) {
    return result;
  }
  if (const int result = KilledProducerIsDetectedAndReclaimed();
      result != 0) {
    return result;
  }
  if (const int result = KilledConsumerIsDetectedAndReclaimed();
      result != 0) {
    return result;
  }
  if (const int result = GracefulCloseWakesInfinitePeerWait();
      result != 0) {
    return result;
  }
  if (const int result = StoppedProducerExpiresAndOldGenerationIsFenced();
      result != 0) {
    return result;
  }
  if (const int result = IdleHeartbeatsPreventFalseTakeover();
      result != 0) {
    return result;
  }
  return StoppedConsumerExpiresAndOldRoleIsFenced();
}
