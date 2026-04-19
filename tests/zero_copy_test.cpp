#include <fastipc/shared_memory_transport.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

std::atomic<bool> g_track_allocations{false};
std::atomic<std::size_t> g_tracked_allocations{0U};

}  // namespace

void* operator new(std::size_t size) {
  if (g_track_allocations.load(std::memory_order_relaxed)) {
    g_tracked_allocations.fetch_add(1U, std::memory_order_relaxed);
  }
  if (void* allocation = std::malloc(size == 0U ? 1U : size)) {
    return allocation;
  }
  throw std::bad_alloc();
}

void operator delete(void* allocation) noexcept {
  std::free(allocation);
}

void operator delete(void* allocation, std::size_t) noexcept {
  std::free(allocation);
}

void* operator new[](std::size_t size) {
  return ::operator new(size);
}

void operator delete[](void* allocation) noexcept {
  ::operator delete(allocation);
}

void operator delete[](void* allocation, std::size_t) noexcept {
  ::operator delete(allocation);
}

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

fastipc::ChannelConfig ConfigFor(std::string_view suffix) {
  fastipc::ChannelConfig config;
  config.name =
      "fastipc_zero_copy_" + std::to_string(::getpid()) + "_" +
      std::to_string(
          g_channel_sequence.fetch_add(1U, std::memory_order_relaxed)) +
      "_" + std::string(suffix);
  config.slot_count = 2U;
  config.max_message_size = 1024U * 1024U;
  config.active_spin_count = 32U;
  config.peer_timeout = 100ms;
  config.unlink_on_owner_close = true;
  return config;
}

int LoanIsInvisibleUntilPublishAndSamplePinsChunk() {
  auto config = ConfigFor("lifecycle");
  auto producer_result =
      fastipc::SharedMemoryTransport::CreateProducer(config);
  CHECK(producer_result);
  auto producer = std::move(producer_result).take_value();
  auto consumer_result =
      fastipc::SharedMemoryTransport::OpenConsumer(config);
  CHECK(consumer_result);
  auto consumer = std::move(consumer_result).take_value();

  auto loan_result = producer->Loan(
      4U, {fastipc::BackpressurePolicy::Block,
           fastipc::Deadline::After(100ms)});
  CHECK(loan_result);
  auto loan = std::move(loan_result).take_value();
  CHECK(loan.size() == 4U);
  auto writable = loan.Data();
  CHECK(writable.size() == 4U);
  writable[0] = std::byte{0x11};
  writable[1] = std::byte{0x22};
  writable[2] = std::byte{0x33};
  writable[3] = std::byte{0x44};

  const auto before_publish =
      consumer->Take(fastipc::Deadline::Immediate());
  CHECK(!before_publish);
  CHECK(before_publish.status().code() == fastipc::StatusCode::Timeout);
  CHECK(loan.Publish());
  CHECK(!loan);

  auto sample_result =
      consumer->Take(fastipc::Deadline::After(100ms));
  CHECK(sample_result);
  auto sample = std::move(sample_result).take_value();
  CHECK(sample.size() == 4U);
  const auto readable = sample.Data();
  CHECK(readable[0] == std::byte{0x11});
  CHECK(readable[1] == std::byte{0x22});
  CHECK(readable[2] == std::byte{0x33});
  CHECK(readable[3] == std::byte{0x44});

  auto second_result = producer->Loan(
      1U, {fastipc::BackpressurePolicy::Block,
           fastipc::Deadline::After(100ms)});
  CHECK(second_result);
  auto second = std::move(second_result).take_value();
  second.Data()[0] = std::byte{0x55};
  CHECK(second.Publish());

  const auto exhausted = producer->Loan(
      1U, {fastipc::BackpressurePolicy::Timeout,
           fastipc::Deadline::After(20ms)});
  CHECK(!exhausted);
  CHECK(exhausted.status().code() == fastipc::StatusCode::Timeout);

  CHECK(sample.Release());
  auto third_result = producer->Loan(
      1U, {fastipc::BackpressurePolicy::Block,
           fastipc::Deadline::After(100ms)});
  CHECK(third_result);
  auto third = std::move(third_result).take_value();
  CHECK(third.Abandon());

  const auto producer_stats = producer->Stats();
  const auto consumer_stats = consumer->Stats();
  CHECK(producer_stats.zero_copy_loans == 3U);
  CHECK(producer_stats.zero_copy_publishes == 2U);
  CHECK(consumer_stats.zero_copy_takes == 1U);
  CHECK(consumer_stats.zero_copy_releases == 1U);
  return 0;
}

int RaiiReturnsUnpublishedAndConsumedChunks() {
  auto config = ConfigFor("raii");
  auto producer_result =
      fastipc::SharedMemoryTransport::CreateProducer(config);
  CHECK(producer_result);
  auto producer = std::move(producer_result).take_value();
  auto consumer_result =
      fastipc::SharedMemoryTransport::OpenConsumer(config);
  CHECK(consumer_result);
  auto consumer = std::move(consumer_result).take_value();

  {
    auto abandoned_result = producer->Loan(
        16U, {fastipc::BackpressurePolicy::Block,
              fastipc::Deadline::After(100ms)});
    CHECK(abandoned_result);
    auto abandoned = std::move(abandoned_result).take_value();
    abandoned.Data()[0] = std::byte{0xA1};
  }

  auto published_result = producer->Loan(
      16U, {fastipc::BackpressurePolicy::Block,
            fastipc::Deadline::After(100ms)});
  CHECK(published_result);
  auto published = std::move(published_result).take_value();
  published.Data()[0] = std::byte{0xB2};
  CHECK(published.Publish());

  {
    auto sample_result =
        consumer->Take(fastipc::Deadline::After(100ms));
    CHECK(sample_result);
    auto sample = std::move(sample_result).take_value();
    CHECK(sample.Data()[0] == std::byte{0xB2});
  }

  for (std::uint8_t value = 1U; value <= 2U; ++value) {
    auto loan_result = producer->Loan(
        1U, {fastipc::BackpressurePolicy::Block,
             fastipc::Deadline::After(100ms)});
    CHECK(loan_result);
    auto loan = std::move(loan_result).take_value();
    loan.Data()[0] = static_cast<std::byte>(value);
    CHECK(loan.Publish());
  }
  const auto full = producer->Loan(
      1U, {fastipc::BackpressurePolicy::Drop,
           fastipc::Deadline::Infinite()});
  CHECK(!full);
  CHECK(full.status().code() == fastipc::StatusCode::Dropped);
  return 0;
}

int CopyAndLoanApisInteroperate() {
  auto config = ConfigFor("copy_interop");
  auto producer_result =
      fastipc::SharedMemoryTransport::CreateProducer(config);
  CHECK(producer_result);
  auto producer = std::move(producer_result).take_value();
  auto consumer_result =
      fastipc::SharedMemoryTransport::OpenConsumer(config);
  CHECK(consumer_result);
  auto consumer = std::move(consumer_result).take_value();

  const std::array<std::byte, 3U> first{
      std::byte{0x10}, std::byte{0x20}, std::byte{0x30}};
  CHECK(producer->Send(
      first, {fastipc::BackpressurePolicy::Block,
              fastipc::Deadline::After(100ms)}));
  auto first_sample =
      consumer->Take(fastipc::Deadline::After(100ms));
  CHECK(first_sample);
  CHECK(first_sample.value().Data().size() == first.size());
  CHECK(first_sample.value().Data()[2] == first[2]);
  CHECK(first_sample.value().Release());

  auto loan_result = producer->Loan(
      3U, {fastipc::BackpressurePolicy::Block,
           fastipc::Deadline::After(100ms)});
  CHECK(loan_result);
  auto loan = std::move(loan_result).take_value();
  loan.Data()[0] = std::byte{0x41};
  loan.Data()[1] = std::byte{0x42};
  loan.Data()[2] = std::byte{0x43};
  CHECK(loan.Publish());

  std::array<std::byte, 2U> too_small_destination{};
  const auto rejected = consumer->Receive(
      too_small_destination, fastipc::Deadline::After(100ms));
  CHECK(!rejected);
  CHECK(rejected.status().code() ==
        fastipc::StatusCode::BufferTooSmall);

  std::array<std::byte, 8U> destination{};
  const auto received =
      consumer->Receive(destination, fastipc::Deadline::After(100ms));
  CHECK(received);
  CHECK(received.value() == 3U);
  CHECK(destination[0] == std::byte{0x41});
  CHECK(destination[2] == std::byte{0x43});
  return 0;
}

int SteadyStateLoanTakeDoesNotAllocate() {
  auto config = ConfigFor("steady_state_allocation");
  auto producer_result =
      fastipc::SharedMemoryTransport::CreateProducer(config);
  CHECK(producer_result);
  auto producer = std::move(producer_result).take_value();
  auto consumer_result =
      fastipc::SharedMemoryTransport::OpenConsumer(config);
  CHECK(consumer_result);
  auto consumer = std::move(consumer_result).take_value();

  auto warmup_loan_result = producer->Loan(
      64U, {fastipc::BackpressurePolicy::Block,
            fastipc::Deadline::After(100ms)});
  CHECK(warmup_loan_result);
  auto warmup_loan = std::move(warmup_loan_result).take_value();
  CHECK(warmup_loan.Publish());
  auto warmup_sample_result =
      consumer->Take(fastipc::Deadline::After(100ms));
  CHECK(warmup_sample_result);
  auto warmup_sample =
      std::move(warmup_sample_result).take_value();
  CHECK(warmup_sample.Release());

  g_tracked_allocations.store(0U, std::memory_order_relaxed);
  g_track_allocations.store(true, std::memory_order_release);
  auto loan_result = producer->Loan(
      64U, {fastipc::BackpressurePolicy::Block,
            fastipc::Deadline::After(100ms)});
  CHECK(loan_result);
  auto loan = std::move(loan_result).take_value();
  loan.Data()[0] = std::byte{0x5A};
  CHECK(loan.Publish());
  auto sample_result =
      consumer->Take(fastipc::Deadline::After(100ms));
  CHECK(sample_result);
  auto sample = std::move(sample_result).take_value();
  CHECK(sample.Data()[0] == std::byte{0x5A});
  CHECK(sample.Release());
  g_track_allocations.store(false, std::memory_order_release);

  CHECK(g_tracked_allocations.load(std::memory_order_relaxed) == 0U);
  return 0;
}

int ProducerCrashAfterLoanIsReclaimed() {
  auto config = ConfigFor("producer_crash");
  config.unlink_on_owner_close = false;
  int ready_pipe[2]{-1, -1};
  CHECK(::pipe(ready_pipe) == 0);

  const pid_t child = ::fork();
  CHECK(child >= 0);
  if (child == 0) {
    ::close(ready_pipe[0]);
    auto producer_result =
        fastipc::SharedMemoryTransport::CreateProducer(config);
    if (!producer_result) {
      ::_exit(20);
    }
    auto producer = std::move(producer_result).take_value();
    auto loan_result = producer->Loan(
        32U, {fastipc::BackpressurePolicy::Block,
              fastipc::Deadline::After(100ms)});
    if (!loan_result) {
      ::_exit(21);
    }
    auto loan = std::move(loan_result).take_value();
    loan.Data()[0] = std::byte{0xDE};
    const char ready = 'R';
    if (::write(ready_pipe[1], &ready, 1) != 1) {
      ::_exit(22);
    }
    for (;;) {
      ::pause();
    }
  }

  ::close(ready_pipe[1]);
  char ready = 0;
  CHECK(::read(ready_pipe[0], &ready, 1) == 1);
  ::close(ready_pipe[0]);
  CHECK(ready == 'R');
  auto consumer_result =
      fastipc::SharedMemoryTransport::OpenConsumer(config);
  CHECK(consumer_result);
  auto consumer = std::move(consumer_result).take_value();

  CHECK(::kill(child, SIGKILL) == 0);
  int child_status = 0;
  CHECK(::waitpid(child, &child_status, 0) == child);
  CHECK(WIFSIGNALED(child_status));

  auto replacement_config = config;
  replacement_config.unlink_on_owner_close = true;
  auto replacement_result =
      fastipc::SharedMemoryTransport::CreateProducer(replacement_config);
  CHECK(replacement_result);
  auto replacement = std::move(replacement_result).take_value();
  auto loan_result = replacement->Loan(
      32U, {fastipc::BackpressurePolicy::Block,
            fastipc::Deadline::After(200ms)});
  CHECK(loan_result);
  auto loan = std::move(loan_result).take_value();
  loan.Data()[0] = std::byte{0xAD};
  CHECK(loan.Publish());

  auto sample_result =
      consumer->Take(fastipc::Deadline::After(200ms));
  CHECK(sample_result);
  auto sample = std::move(sample_result).take_value();
  CHECK(sample.Data()[0] == std::byte{0xAD});
  CHECK(sample.Release());
  CHECK(replacement->Stats().producer_loan_reclaims >= 1U);
  return 0;
}

int ConsumerCrashHoldingSampleIsRedelivered() {
  auto config = ConfigFor("consumer_crash");
  int ready_pipe[2]{-1, -1};
  int start_pipe[2]{-1, -1};
  CHECK(::pipe(ready_pipe) == 0);
  CHECK(::pipe(start_pipe) == 0);
  const pid_t child = ::fork();
  CHECK(child >= 0);
  if (child == 0) {
    ::close(ready_pipe[0]);
    ::close(start_pipe[1]);
    char start = 0;
    if (::read(start_pipe[0], &start, 1) != 1 || start != 'S') {
      ::_exit(29);
    }
    ::close(start_pipe[0]);

    auto consumer_result =
        fastipc::SharedMemoryTransport::OpenConsumer(config);
    if (!consumer_result) {
      ::_exit(30);
    }
    auto consumer = std::move(consumer_result).take_value();
    auto sample_result =
        consumer->Take(fastipc::Deadline::After(200ms));
    if (!sample_result ||
        sample_result.value().Data()[1] != std::byte{0xFF}) {
      ::_exit(31);
    }
    auto sample = std::move(sample_result).take_value();
    const char ready = 'R';
    if (::write(ready_pipe[1], &ready, 1) != 1) {
      ::_exit(32);
    }
    for (;;) {
      ::pause();
    }
  }

  ::close(ready_pipe[1]);
  ::close(start_pipe[0]);
  auto producer_result =
      fastipc::SharedMemoryTransport::CreateProducer(config);
  CHECK(producer_result);
  auto producer = std::move(producer_result).take_value();

  const std::array<std::byte, 4U> payload{
      std::byte{0xC0}, std::byte{0xFF},
      std::byte{0xEE}, std::byte{0x01}};
  CHECK(producer->Send(
      payload, {fastipc::BackpressurePolicy::Block,
                fastipc::Deadline::After(100ms)}));

  const char start = 'S';
  CHECK(::write(start_pipe[1], &start, 1) == 1);
  ::close(start_pipe[1]);
  char ready = 0;
  CHECK(::read(ready_pipe[0], &ready, 1) == 1);
  ::close(ready_pipe[0]);
  CHECK(ready == 'R');
  CHECK(::kill(child, SIGKILL) == 0);
  int child_status = 0;
  CHECK(::waitpid(child, &child_status, 0) == child);
  CHECK(WIFSIGNALED(child_status));

  auto replacement_result =
      fastipc::SharedMemoryTransport::OpenConsumer(config);
  CHECK(replacement_result);
  auto replacement = std::move(replacement_result).take_value();
  auto sample_result =
      replacement->Take(fastipc::Deadline::After(200ms));
  CHECK(sample_result);
  auto sample = std::move(sample_result).take_value();
  CHECK(sample.Data().size() == payload.size());
  CHECK(sample.Data()[0] == payload[0]);
  CHECK(sample.Data()[3] == payload[3]);
  CHECK(sample.Release());
  CHECK(replacement->Stats().consumer_loan_reclaims >= 1U);
  return 0;
}

int OutstandingHandlesKeepMappingAlive() {
  auto config = ConfigFor("close_with_handles");
  config.unlink_on_owner_close = false;
  auto producer_result =
      fastipc::SharedMemoryTransport::CreateProducer(config);
  CHECK(producer_result);
  auto producer = std::move(producer_result).take_value();
  auto consumer_result =
      fastipc::SharedMemoryTransport::OpenConsumer(config);
  CHECK(consumer_result);
  auto consumer = std::move(consumer_result).take_value();

  auto stale_result = producer->Loan(
      1U, {fastipc::BackpressurePolicy::Block,
           fastipc::Deadline::After(100ms)});
  CHECK(stale_result);
  auto stale = std::move(stale_result).take_value();
  stale.Data()[0] = std::byte{0x11};
  producer.reset();
  CHECK(stale.Data()[0] == std::byte{0x11});

  auto replacement_config = config;
  replacement_config.unlink_on_owner_close = true;
  auto replacement_result =
      fastipc::SharedMemoryTransport::CreateProducer(replacement_config);
  CHECK(replacement_result);
  auto replacement = std::move(replacement_result).take_value();

  const auto stale_publish = stale.Publish();
  CHECK(!stale_publish);
  CHECK(stale_publish.code() == fastipc::StatusCode::Closed);

  auto current_result = replacement->Loan(
      1U, {fastipc::BackpressurePolicy::Block,
           fastipc::Deadline::After(200ms)});
  CHECK(current_result);
  auto current = std::move(current_result).take_value();
  current.Data()[0] = std::byte{0xAD};

  CHECK(current.Publish());

  auto sample_result =
      consumer->Take(fastipc::Deadline::After(200ms));
  CHECK(sample_result);
  auto sample = std::move(sample_result).take_value();
  CHECK(sample.Data()[0] == std::byte{0xAD});
  consumer.reset();
  CHECK(sample.Data()[0] == std::byte{0xAD});
  const auto release = sample.Release();
  CHECK(!release);
  CHECK(release.code() == fastipc::StatusCode::StaleGeneration);
  return 0;
}

int ProducerPausedTakeoverFencesStaleLoan() {
  auto config = ConfigFor("producer_paused_takeover");
  config.peer_timeout = 30ms;
  config.unlink_on_owner_close = false;
  int ready_pipe[2]{-1, -1};
  int result_pipe[2]{-1, -1};
  CHECK(::pipe(ready_pipe) == 0);
  CHECK(::pipe(result_pipe) == 0);

  const pid_t child = ::fork();
  CHECK(child >= 0);
  if (child == 0) {
    ::close(ready_pipe[0]);
    ::close(result_pipe[0]);
    auto producer_result =
        fastipc::SharedMemoryTransport::CreateProducer(config);
    if (!producer_result) {
      ::_exit(40);
    }
    auto producer = std::move(producer_result).take_value();
    auto loan_result = producer->Loan(
        1U, {fastipc::BackpressurePolicy::Block,
             fastipc::Deadline::After(100ms)});
    if (!loan_result) {
      ::_exit(41);
    }
    auto loan = std::move(loan_result).take_value();
    loan.Data()[0] = std::byte{0x11};
    const char ready = 'R';
    if (::write(ready_pipe[1], &ready, 1) != 1) {
      ::_exit(42);
    }
    ::raise(SIGSTOP);
    const auto status = loan.Publish();
    const auto code = static_cast<std::uint8_t>(status.code());
    producer.reset();
    if (::write(result_pipe[1], &code, sizeof(code)) !=
        static_cast<ssize_t>(sizeof(code))) {
      ::_exit(43);
    }
    ::_exit(0);
  }

  ::close(ready_pipe[1]);
  ::close(result_pipe[1]);
  char ready = 0;
  CHECK(::read(ready_pipe[0], &ready, 1) == 1);
  ::close(ready_pipe[0]);
  CHECK(ready == 'R');
  int stopped_status = 0;
  CHECK(::waitpid(child, &stopped_status, WUNTRACED) == child);
  CHECK(WIFSTOPPED(stopped_status));

  auto consumer_result =
      fastipc::SharedMemoryTransport::OpenConsumer(config);
  CHECK(consumer_result);
  auto consumer = std::move(consumer_result).take_value();
  std::this_thread::sleep_for(config.peer_timeout * 3);

  auto replacement_config = config;
  replacement_config.unlink_on_owner_close = true;
  auto replacement_result =
      fastipc::SharedMemoryTransport::CreateProducer(replacement_config);
  CHECK(replacement_result);
  auto replacement = std::move(replacement_result).take_value();
  const auto blocked = replacement->Loan(
      1U, {fastipc::BackpressurePolicy::Block,
           fastipc::Deadline::After(200ms)});
  CHECK(!blocked);
  CHECK(blocked.status().code() == fastipc::StatusCode::WouldBlock);

  CHECK(::kill(child, SIGCONT) == 0);
  std::uint8_t stale_code = 0U;
  CHECK(::read(result_pipe[0], &stale_code, sizeof(stale_code)) ==
        static_cast<ssize_t>(sizeof(stale_code)));
  ::close(result_pipe[0]);
  int child_status = 0;
  CHECK(::waitpid(child, &child_status, 0) == child);
  CHECK(WIFEXITED(child_status));
  CHECK(WEXITSTATUS(child_status) == 0);
  CHECK(stale_code ==
        static_cast<std::uint8_t>(fastipc::StatusCode::StaleGeneration));

  auto current_result = replacement->Loan(
      1U, {fastipc::BackpressurePolicy::Block,
           fastipc::Deadline::After(200ms)});
  CHECK(current_result);
  auto current = std::move(current_result).take_value();
  current.Data()[0] = std::byte{0xAD};
  CHECK(current.Data()[0] == std::byte{0xAD});
  CHECK(current.Publish());

  auto sample_result =
      consumer->Take(fastipc::Deadline::After(200ms));
  CHECK(sample_result);
  auto sample = std::move(sample_result).take_value();
  CHECK(sample.Data()[0] == std::byte{0xAD});
  CHECK(sample.Release());
  CHECK(replacement->Stats().producer_loan_reclaims == 0U);
  return 0;
}

int ConsumerPausedTakeoverFencesStaleSample() {
  auto config = ConfigFor("consumer_paused_takeover");
  config.peer_timeout = 30ms;
  int ready_pipe[2]{-1, -1};
  int result_pipe[2]{-1, -1};
  int start_pipe[2]{-1, -1};
  CHECK(::pipe(ready_pipe) == 0);
  CHECK(::pipe(result_pipe) == 0);
  CHECK(::pipe(start_pipe) == 0);
  const pid_t child = ::fork();
  CHECK(child >= 0);
  if (child == 0) {
    ::close(ready_pipe[0]);
    ::close(result_pipe[0]);
    ::close(start_pipe[1]);
    char start = 0;
    if (::read(start_pipe[0], &start, 1) != 1 || start != 'S') {
      ::_exit(49);
    }
    ::close(start_pipe[0]);

    auto consumer_result =
        fastipc::SharedMemoryTransport::OpenConsumer(config);
    if (!consumer_result) {
      ::_exit(50);
    }
    auto consumer = std::move(consumer_result).take_value();
    auto sample_result =
        consumer->Take(fastipc::Deadline::After(100ms));
    if (!sample_result) {
      ::_exit(51);
    }
    auto sample = std::move(sample_result).take_value();
    const char ready = 'R';
    if (::write(ready_pipe[1], &ready, 1) != 1) {
      ::_exit(52);
    }
    ::raise(SIGSTOP);
    const auto status = sample.Release();
    const auto code = static_cast<std::uint8_t>(status.code());
    consumer.reset();
    if (::write(result_pipe[1], &code, sizeof(code)) !=
        static_cast<ssize_t>(sizeof(code))) {
      ::_exit(53);
    }
    ::_exit(0);
  }

  ::close(ready_pipe[1]);
  ::close(result_pipe[1]);
  ::close(start_pipe[0]);
  auto producer_result =
      fastipc::SharedMemoryTransport::CreateProducer(config);
  CHECK(producer_result);
  auto producer = std::move(producer_result).take_value();
  const std::array<std::byte, 2U> payload{
      std::byte{0xBE}, std::byte{0xEF}};
  CHECK(producer->Send(
      payload, {fastipc::BackpressurePolicy::Block,
                fastipc::Deadline::After(100ms)}));

  const char start = 'S';
  CHECK(::write(start_pipe[1], &start, 1) == 1);
  ::close(start_pipe[1]);
  char ready = 0;
  CHECK(::read(ready_pipe[0], &ready, 1) == 1);
  ::close(ready_pipe[0]);
  CHECK(ready == 'R');
  int stopped_status = 0;
  CHECK(::waitpid(child, &stopped_status, WUNTRACED) == child);
  CHECK(WIFSTOPPED(stopped_status));
  std::this_thread::sleep_for(config.peer_timeout * 3);

  auto replacement_result =
      fastipc::SharedMemoryTransport::OpenConsumer(config);
  CHECK(replacement_result);
  auto replacement = std::move(replacement_result).take_value();
  auto current_result =
      replacement->Take(fastipc::Deadline::After(200ms));
  CHECK(current_result);
  auto current = std::move(current_result).take_value();
  CHECK(current.Data()[0] == payload[0]);
  CHECK(current.Data()[1] == payload[1]);

  CHECK(::kill(child, SIGCONT) == 0);
  std::uint8_t stale_code = 0U;
  CHECK(::read(result_pipe[0], &stale_code, sizeof(stale_code)) ==
        static_cast<ssize_t>(sizeof(stale_code)));
  ::close(result_pipe[0]);
  int child_status = 0;
  CHECK(::waitpid(child, &child_status, 0) == child);
  CHECK(WIFEXITED(child_status));
  CHECK(WEXITSTATUS(child_status) == 0);
  CHECK(stale_code ==
        static_cast<std::uint8_t>(fastipc::StatusCode::StaleGeneration));
  CHECK(current.Data()[0] == payload[0]);
  CHECK(current.Release());
  CHECK(replacement->Stats().consumer_loan_reclaims >= 1U);
  return 0;
}

struct NamedTest {
  std::string_view name;
  int (*run)();
};

constexpr std::array kTests{
    NamedTest{"lifecycle", LoanIsInvisibleUntilPublishAndSamplePinsChunk},
    NamedTest{"raii", RaiiReturnsUnpublishedAndConsumedChunks},
    NamedTest{"copy_interop", CopyAndLoanApisInteroperate},
    NamedTest{"steady_state_allocation",
              SteadyStateLoanTakeDoesNotAllocate},
    NamedTest{"producer_loan_crash", ProducerCrashAfterLoanIsReclaimed},
    NamedTest{"consumer_sample_crash",
              ConsumerCrashHoldingSampleIsRedelivered},
    NamedTest{"outstanding_handle_close",
              OutstandingHandlesKeepMappingAlive},
    NamedTest{"producer_paused_takeover",
              ProducerPausedTakeoverFencesStaleLoan},
    NamedTest{"consumer_paused_takeover",
              ConsumerPausedTakeoverFencesStaleSample},
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
    std::cerr << "usage: fastipc_zero_copy_tests [--case NAME]\n";
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
