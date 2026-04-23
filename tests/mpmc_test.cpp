#include <fastipc/mpmc_shared_memory_transport.hpp>

#include "mpmc_shared_memory_layout.hpp"

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace std::chrono_literals;

namespace {

#define CHECK(condition)                                                     \
  do {                                                                       \
    if (!(condition)) {                                                      \
      std::cerr << __FILE__ << ':' << __LINE__                               \
                << ": check failed: " #condition << '\n';                  \
      return 1;                                                              \
    }                                                                        \
  } while (false)

std::atomic<std::uint64_t> g_channel_sequence{1U};

fastipc::MpmcChannelConfig ConfigFor(std::string_view suffix) {
  fastipc::MpmcChannelConfig config;
  config.name =
      "fastipc_mpmc_" + std::to_string(::getpid()) + "_" +
      std::to_string(
          g_channel_sequence.fetch_add(1U, std::memory_order_relaxed)) +
      "_" + std::string(suffix);
  config.capacity = 64U;
  config.max_message_size = 64U;
  config.active_spin_count = 32U;
  config.unlink_on_owner_close = true;
  return config;
}

struct Message {
  std::uint64_t id{0U};
  std::uint64_t checksum{0U};
};

constexpr std::uint64_t kChecksumSalt = 0x9E3779B97F4A7C15ULL;

Message MakeMessage(std::uint64_t id) {
  return {id, id ^ kChecksumSalt};
}

std::span<const std::byte> BytesOf(const Message& message) {
  return {reinterpret_cast<const std::byte*>(&message), sizeof(message)};
}

bool IsValid(const Message& message, std::uint64_t upper_bound) {
  return message.id < upper_bound &&
         message.checksum == (message.id ^ kChecksumSalt);
}

int ConfigurationAndBasicQueueSemantics() {
  auto invalid_capacity = ConfigFor("invalid_capacity");
  invalid_capacity.capacity = 3U;
  auto invalid_result =
      fastipc::MpmcSharedMemoryTransport::Create(invalid_capacity);
  CHECK(!invalid_result);
  CHECK(invalid_result.status().code() ==
        fastipc::StatusCode::InvalidArgument);

  auto excessive_segment = ConfigFor("excessive_segment");
  excessive_segment.capacity = 1U << 20U;
  excessive_segment.max_message_size = 4096U;
  auto excessive_result =
      fastipc::MpmcSharedMemoryTransport::Create(excessive_segment);
  CHECK(!excessive_result);
  CHECK(excessive_result.status().code() ==
        fastipc::StatusCode::InvalidArgument);

  auto config = ConfigFor("basic");
  config.capacity = 2U;
  auto owner_result = fastipc::MpmcSharedMemoryTransport::Create(config);
  CHECK(owner_result);
  auto owner = std::move(owner_result).take_value();

  auto mismatch = config;
  mismatch.capacity = 4U;
  auto mismatch_result =
      fastipc::MpmcSharedMemoryTransport::Open(mismatch);
  CHECK(!mismatch_result);
  CHECK(mismatch_result.status().code() ==
        fastipc::StatusCode::LayoutMismatch);

  auto peer_result = fastipc::MpmcSharedMemoryTransport::Open(config);
  CHECK(peer_result);
  auto peer = std::move(peer_result).take_value();

  const auto first = MakeMessage(1U);
  const auto second = MakeMessage(2U);
  const auto third = MakeMessage(3U);
  CHECK(owner->Send(
      BytesOf(first), {fastipc::BackpressurePolicy::Block,
                       fastipc::Deadline::After(100ms)}));
  CHECK(peer->Send(
      BytesOf(second), {fastipc::BackpressurePolicy::Block,
                        fastipc::Deadline::After(100ms)}));

  const auto dropped = owner->Send(
      BytesOf(third), {fastipc::BackpressurePolicy::Drop,
                       fastipc::Deadline::Infinite()});
  CHECK(!dropped);
  CHECK(dropped.code() == fastipc::StatusCode::Dropped);

  const auto timeout = owner->Send(
      BytesOf(third), {fastipc::BackpressurePolicy::Timeout,
                       fastipc::Deadline::After(20ms)});
  CHECK(!timeout);
  CHECK(timeout.code() == fastipc::StatusCode::Timeout);

  Message received{};
  auto first_receive = peer->Receive(
      {reinterpret_cast<std::byte*>(&received), sizeof(received)},
      fastipc::Deadline::After(100ms));
  CHECK(first_receive);
  CHECK(first_receive.value() == sizeof(Message));
  CHECK(received.id == first.id);

  auto second_receive = owner->Receive(
      {reinterpret_cast<std::byte*>(&received), sizeof(received)},
      fastipc::Deadline::After(100ms));
  CHECK(second_receive);
  CHECK(received.id == second.id);

  const auto empty = peer->Receive(
      {reinterpret_cast<std::byte*>(&received), sizeof(received)},
      fastipc::Deadline::Immediate());
  CHECK(!empty);
  CHECK(empty.status().code() == fastipc::StatusCode::Timeout);

  std::array<std::byte, 65U> oversized{};
  const auto too_large = owner->Send(
      oversized, {fastipc::BackpressurePolicy::Block,
                  fastipc::Deadline::After(100ms)});
  CHECK(!too_large);
  CHECK(too_large.code() == fastipc::StatusCode::MessageTooLarge);

  CHECK(owner->Send(
      BytesOf(third), {fastipc::BackpressurePolicy::Block,
                       fastipc::Deadline::After(100ms)}));
  std::array<std::byte, sizeof(Message) - 1U> too_small{};
  const auto buffer_error = peer->Receive(
      too_small, fastipc::Deadline::After(100ms));
  CHECK(!buffer_error);
  CHECK(buffer_error.status().code() ==
        fastipc::StatusCode::BufferTooSmall);
  const auto progressed = peer->Receive(
      {reinterpret_cast<std::byte*>(&received), sizeof(received)},
      fastipc::Deadline::Immediate());
  CHECK(!progressed);
  CHECK(progressed.status().code() == fastipc::StatusCode::Timeout);

  const auto stats = owner->Stats();
  CHECK(stats.sent_messages == 3U);
  CHECK(stats.received_messages == 2U);
  CHECK(stats.dropped_messages == 2U);
  CHECK(stats.send_timeouts == 1U);
  CHECK(stats.receive_timeouts >= 2U);
  return 0;
}

int FutexWaitersWakeOnDataAndSpace() {
  auto config = ConfigFor("futex_wakeup");
  config.capacity = 2U;
  auto owner_result = fastipc::MpmcSharedMemoryTransport::Create(config);
  CHECK(owner_result);
  auto owner = std::move(owner_result).take_value();

  Message received{};
  std::atomic<bool> receive_ok{false};
  std::jthread consumer([&] {
    auto result = owner->Receive(
        {reinterpret_cast<std::byte*>(&received), sizeof(received)},
        fastipc::Deadline::After(1s));
    receive_ok.store(
        result && result.value() == sizeof(Message),
        std::memory_order_release);
  });
  std::this_thread::sleep_for(20ms);
  const auto first = MakeMessage(7U);
  CHECK(owner->Send(
      BytesOf(first), {fastipc::BackpressurePolicy::Block,
                       fastipc::Deadline::After(1s)}));
  consumer.join();
  CHECK(receive_ok.load(std::memory_order_acquire));
  CHECK(received.id == first.id);

  const auto second = MakeMessage(8U);
  const auto third = MakeMessage(9U);
  const auto fourth = MakeMessage(10U);
  CHECK(owner->Send(
      BytesOf(second), {fastipc::BackpressurePolicy::Block,
                        fastipc::Deadline::After(1s)}));
  CHECK(owner->Send(
      BytesOf(third), {fastipc::BackpressurePolicy::Block,
                       fastipc::Deadline::After(1s)}));

  std::atomic<bool> send_ok{false};
  std::jthread producer([&] {
    const auto status = owner->Send(
        BytesOf(fourth), {fastipc::BackpressurePolicy::Block,
                          fastipc::Deadline::After(1s)});
    send_ok.store(status.ok(), std::memory_order_release);
  });
  std::this_thread::sleep_for(20ms);
  auto result = owner->Receive(
      {reinterpret_cast<std::byte*>(&received), sizeof(received)},
      fastipc::Deadline::After(1s));
  CHECK(result);
  producer.join();
  CHECK(send_ok.load(std::memory_order_acquire));
  return 0;
}

int ThreadedManyToManyIsExactlyOnce() {
  constexpr std::uint64_t kProducerCount = 4U;
  constexpr std::uint64_t kConsumerCount = 4U;
  constexpr std::uint64_t kMessagesPerProducer = 2000U;
  constexpr std::uint64_t kTotalMessages =
      kProducerCount * kMessagesPerProducer;

  auto config = ConfigFor("threaded_exactly_once");
  config.capacity = 256U;
  auto owner_result = fastipc::MpmcSharedMemoryTransport::Create(config);
  CHECK(owner_result);
  auto owner = std::move(owner_result).take_value();

  std::vector<std::atomic<std::uint32_t>> seen(kTotalMessages);
  std::atomic<std::uint64_t> consumed{0U};
  std::atomic<std::uint64_t> producers_finished{0U};
  std::atomic<std::uint32_t> failures{0U};
  std::barrier start(static_cast<std::ptrdiff_t>(
      kProducerCount + kConsumerCount));
  std::vector<std::jthread> threads;
  threads.reserve(kProducerCount + kConsumerCount);

  for (std::uint64_t producer_id = 0U;
       producer_id < kProducerCount; ++producer_id) {
    threads.emplace_back([&, producer_id] {
      start.arrive_and_wait();
      for (std::uint64_t offset = 0U;
           offset < kMessagesPerProducer; ++offset) {
        const auto message = MakeMessage(
            producer_id * kMessagesPerProducer + offset);
        const auto status = owner->Send(
            BytesOf(message),
            {fastipc::BackpressurePolicy::Block,
             fastipc::Deadline::After(5s)});
        if (!status) {
          failures.fetch_add(1U, std::memory_order_relaxed);
          producers_finished.fetch_add(1U, std::memory_order_release);
          return;
        }
      }
      producers_finished.fetch_add(1U, std::memory_order_release);
    });
  }

  for (std::uint64_t consumer_id = 0U;
       consumer_id < kConsumerCount; ++consumer_id) {
    threads.emplace_back([&, consumer_id] {
      static_cast<void>(consumer_id);
      start.arrive_and_wait();
      while (consumed.load(std::memory_order_acquire) < kTotalMessages) {
        Message message{};
        auto result = owner->Receive(
            {reinterpret_cast<std::byte*>(&message), sizeof(message)},
            fastipc::Deadline::After(250ms));
        if (!result) {
          if (result.status().code() == fastipc::StatusCode::Timeout) {
            if (consumed.load(std::memory_order_acquire) >=
                kTotalMessages) {
              return;
            }
            if (producers_finished.load(std::memory_order_acquire) <
                kProducerCount) {
              continue;
            }
          }
          failures.fetch_add(1U, std::memory_order_relaxed);
          return;
        }
        if (result.value() != sizeof(Message) ||
            !IsValid(message, kTotalMessages)) {
          failures.fetch_add(1U, std::memory_order_relaxed);
          return;
        }
        seen[message.id].fetch_add(1U, std::memory_order_relaxed);
        consumed.fetch_add(1U, std::memory_order_release);
      }
    });
  }

  threads.clear();
  CHECK(failures.load(std::memory_order_relaxed) == 0U);
  CHECK(consumed.load(std::memory_order_relaxed) == kTotalMessages);
  for (const auto& count : seen) {
    CHECK(count.load(std::memory_order_relaxed) == 1U);
  }
  return 0;
}

struct SharedVerification {
  std::uint64_t total_received{0U};
  std::uint64_t errors{0U};
};

int CrossProcessManyToManyIsExactlyOnce() {
  constexpr std::uint64_t kProducerCount = 3U;
  constexpr std::uint64_t kConsumerCount = 3U;
  constexpr std::uint64_t kMessagesPerProducer = 500U;
  constexpr std::uint64_t kTotalMessages =
      kProducerCount * kMessagesPerProducer;
  constexpr std::uint64_t kStopId = UINT64_MAX;

  auto config = ConfigFor("process_exactly_once");
  config.capacity = 256U;
  auto owner_result = fastipc::MpmcSharedMemoryTransport::Create(config);
  CHECK(owner_result);
  auto owner = std::move(owner_result).take_value();

  const std::size_t verification_bytes =
      sizeof(SharedVerification) +
      static_cast<std::size_t>(kTotalMessages) * sizeof(std::uint32_t);
  void* verification_mapping = ::mmap(
      nullptr, verification_bytes, PROT_READ | PROT_WRITE,
      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  CHECK(verification_mapping != MAP_FAILED);
  std::memset(verification_mapping, 0, verification_bytes);
  auto* verification =
      static_cast<SharedVerification*>(verification_mapping);
  auto* counts = reinterpret_cast<std::uint32_t*>(
      static_cast<std::byte*>(verification_mapping) +
      sizeof(SharedVerification));

  std::array<pid_t, kConsumerCount> consumers{};
  for (std::size_t index = 0U; index < consumers.size(); ++index) {
    const pid_t child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
      auto endpoint_result =
          fastipc::MpmcSharedMemoryTransport::Open(config);
      if (!endpoint_result) {
        ::_exit(20);
      }
      auto endpoint = std::move(endpoint_result).take_value();
      for (;;) {
        Message message{};
        auto result = endpoint->Receive(
            {reinterpret_cast<std::byte*>(&message), sizeof(message)},
            fastipc::Deadline::After(5s));
        if (!result) {
          __atomic_fetch_add(&verification->errors,
                             std::uint64_t{1}, __ATOMIC_RELAXED);
          ::_exit(21);
        }
        if (message.id == kStopId) {
          ::_exit(0);
        }
        if (result.value() != sizeof(Message) ||
            !IsValid(message, kTotalMessages)) {
          __atomic_fetch_add(&verification->errors,
                             std::uint64_t{1}, __ATOMIC_RELAXED);
          ::_exit(22);
        }
        __atomic_fetch_add(&counts[message.id],
                           std::uint32_t{1}, __ATOMIC_RELAXED);
        __atomic_fetch_add(&verification->total_received,
                           std::uint64_t{1}, __ATOMIC_RELAXED);
      }
    }
    consumers[index] = child;
  }

  std::array<pid_t, kProducerCount> producers{};
  for (std::size_t index = 0U; index < producers.size(); ++index) {
    const pid_t child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
      auto endpoint_result =
          fastipc::MpmcSharedMemoryTransport::Open(config);
      if (!endpoint_result) {
        ::_exit(30);
      }
      auto endpoint = std::move(endpoint_result).take_value();
      for (std::uint64_t offset = 0U;
           offset < kMessagesPerProducer; ++offset) {
        const auto message = MakeMessage(
            static_cast<std::uint64_t>(index) *
                kMessagesPerProducer +
            offset);
        const auto status = endpoint->Send(
            BytesOf(message),
            {fastipc::BackpressurePolicy::Block,
             fastipc::Deadline::After(5s)});
        if (!status) {
          ::_exit(31);
        }
      }
      ::_exit(0);
    }
    producers[index] = child;
  }

  for (const pid_t producer : producers) {
    int status = 0;
    CHECK(::waitpid(producer, &status, 0) == producer);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
  }

  const Message stop{kStopId, 0U};
  for (std::size_t index = 0U; index < consumers.size(); ++index) {
    CHECK(owner->Send(
        BytesOf(stop), {fastipc::BackpressurePolicy::Block,
                        fastipc::Deadline::After(5s)}));
  }
  for (const pid_t consumer : consumers) {
    int status = 0;
    CHECK(::waitpid(consumer, &status, 0) == consumer);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
  }

  CHECK(__atomic_load_n(&verification->errors, __ATOMIC_RELAXED) == 0U);
  CHECK(__atomic_load_n(
            &verification->total_received, __ATOMIC_RELAXED) ==
        kTotalMessages);
  for (std::uint64_t index = 0U; index < kTotalMessages; ++index) {
    CHECK(__atomic_load_n(&counts[index], __ATOMIC_RELAXED) == 1U);
  }
  CHECK(::munmap(verification_mapping, verification_bytes) == 0);
  return 0;
}

int AbandonedReservationBlocksHeadWithoutFakeRecovery() {
  auto config = ConfigFor("abandoned_reservation");
  config.capacity = 4U;
  auto owner_result = fastipc::MpmcSharedMemoryTransport::Create(config);
  CHECK(owner_result);
  auto owner = std::move(owner_result).take_value();

  const pid_t child = ::fork();
  CHECK(child >= 0);
  if (child == 0) {
    const std::string object_name = "/" + config.name;
    const int descriptor = ::shm_open(object_name.c_str(), O_RDWR, 0);
    if (descriptor < 0) {
      ::_exit(40);
    }
    struct stat object_stat {};
    if (::fstat(descriptor, &object_stat) != 0) {
      ::_exit(41);
    }
    void* mapping = ::mmap(
        nullptr, static_cast<std::size_t>(object_stat.st_size),
        PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
    if (mapping == MAP_FAILED) {
      ::_exit(42);
    }
    auto* layout =
        static_cast<fastipc::detail::MpmcSharedLayout*>(mapping);
    std::uint64_t expected = 0U;
    const bool reserved = __atomic_compare_exchange_n(
        &layout->enqueue.position, &expected, std::uint64_t{1}, false,
        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    ::_exit(reserved ? 0 : 43);
  }

  int child_status = 0;
  CHECK(::waitpid(child, &child_status, 0) == child);
  CHECK(WIFEXITED(child_status));
  CHECK(WEXITSTATUS(child_status) == 0);

  const auto later = MakeMessage(1U);
  CHECK(owner->Send(
      BytesOf(later), {fastipc::BackpressurePolicy::Block,
                       fastipc::Deadline::After(100ms)}));

  Message received{};
  const auto blocked = owner->Receive(
      {reinterpret_cast<std::byte*>(&received), sizeof(received)},
      fastipc::Deadline::After(30ms));
  CHECK(!blocked);
  CHECK(blocked.status().code() == fastipc::StatusCode::Timeout);
  CHECK(owner->Stats().sent_messages == 1U);
  CHECK(owner->Stats().received_messages == 0U);
  return 0;
}

struct NamedTest {
  std::string_view name;
  int (*run)();
};

constexpr std::array kTests{
    NamedTest{"basic", ConfigurationAndBasicQueueSemantics},
    NamedTest{"futex_wakeup", FutexWaitersWakeOnDataAndSpace},
    NamedTest{"threaded_exactly_once", ThreadedManyToManyIsExactlyOnce},
    NamedTest{"process_exactly_once", CrossProcessManyToManyIsExactlyOnce},
    NamedTest{"abandoned_reservation",
              AbandonedReservationBlocksHeadWithoutFakeRecovery},
};

}  // namespace

int main(int argc, char** argv) {
  if (argc == 3 && std::string_view(argv[1]) == "--case") {
    const std::string_view requested(argv[2]);
    for (const auto& test : kTests) {
      if (test.name == requested) {
        return test.run();
      }
    }
    std::cerr << "unknown test case: " << requested << '\n';
    return 2;
  }
  if (argc != 1) {
    std::cerr << "usage: fastipc_mpmc_tests [--case NAME]\n";
    return 2;
  }
  for (const auto& test : kTests) {
    const int result = test.run();
    if (result != 0) {
      std::cerr << "test case failed: " << test.name << '\n';
      return result;
    }
  }
  return 0;
}
