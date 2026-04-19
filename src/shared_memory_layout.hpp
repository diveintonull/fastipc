#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace fastipc::detail {

inline constexpr std::array<char, 8> kMagic{
    'F', 'A', 'S', 'T', 'I', 'P', 'C', '\0'};
inline constexpr std::uint16_t kLayoutMajor = 2;
inline constexpr std::uint16_t kLayoutMinor = 0;
inline constexpr std::uint32_t kEndianMarker = 0x01020304U;
inline constexpr std::uint32_t kInitInitializing = 1;
inline constexpr std::uint32_t kInitReady = 2;
inline constexpr std::size_t kCacheLine = 64;

struct alignas(kCacheLine) SegmentHeader {
  std::array<char, 8> magic{};
  std::uint16_t version_major{0};
  std::uint16_t version_minor{0};
  std::uint32_t header_bytes{0};
  std::uint64_t segment_bytes{0};
  std::uint64_t generation{0};
  std::uint64_t created_monotonic_ns{0};
  std::uint32_t endian_marker{0};
  std::uint32_t init_state{0};
  std::array<std::byte, 16> reserved{};
};
static_assert(sizeof(SegmentHeader) == kCacheLine);

struct alignas(kCacheLine) EndpointMetadata {
  std::int32_t pid{0};
  std::uint32_t state{0};
  std::uint64_t process_start_ticks{0};
  std::uint64_t generation{0};
  std::uint64_t role_token{0};
  std::uint64_t heartbeat_monotonic_ns{0};
  std::uint64_t operation_sequence{0};
  std::array<std::byte, 16> reserved{};
};
static_assert(sizeof(EndpointMetadata) == kCacheLine);

struct alignas(kCacheLine) QueueConfig {
  std::uint32_t slot_count{0};
  std::uint32_t max_message_size{0};
  std::uint32_t slot_stride{0};
  std::uint32_t slots_offset{0};
  std::array<std::byte, 48> reserved{};
};
static_assert(sizeof(QueueConfig) == kCacheLine);

struct alignas(kCacheLine) ProducerCursor {
  std::uint64_t head{0};
  std::uint32_t data_epoch{0};
  std::uint32_t reserved_word{0};
  std::uint64_t sent_messages{0};
  std::uint64_t dropped_messages{0};
  std::uint64_t timeout_count{0};
  std::uint64_t zero_copy_loans{0};
  std::uint64_t zero_copy_publishes{0};
  std::uint64_t reclaimed_loans{0};
};
static_assert(sizeof(ProducerCursor) == kCacheLine);

struct alignas(kCacheLine) ConsumerCursor {
  std::uint64_t tail{0};
  std::uint32_t space_epoch{0};
  std::uint32_t reserved_word{0};
  std::uint64_t received_messages{0};
  std::uint64_t timeout_count{0};
  std::uint64_t corrupt_messages{0};
  std::uint64_t zero_copy_takes{0};
  std::uint64_t zero_copy_releases{0};
  std::uint64_t reclaimed_loans{0};
};
static_assert(sizeof(ConsumerCursor) == kCacheLine);

struct alignas(kCacheLine) SharedLayout {
  SegmentHeader header;
  EndpointMetadata producer;
  EndpointMetadata consumer;
  QueueConfig queue;
  ProducerCursor producer_cursor;
  ConsumerCursor consumer_cursor;
};
static_assert(sizeof(SharedLayout) == 6 * kCacheLine);
static_assert(std::is_standard_layout_v<SharedLayout>);

enum class SlotState : std::uint32_t {
  Free = 0U,
  ProducerClaiming = 1U,
  ClaimedByProducer = 2U,
  Published = 3U,
  ConsumerTaking = 4U,
  LoanedToConsumer = 5U,
};

enum class SlotOwnerRole : std::uint32_t {
  None = 0U,
  Producer = 1U,
  Consumer = 2U,
};

struct alignas(kCacheLine) SlotHeader {
  std::uint32_t state{
      static_cast<std::uint32_t>(SlotState::Free)};
  std::uint32_t length{0};
  std::uint64_t sequence{0};
  std::uint64_t chunk_generation{0};
  std::uint64_t owner_generation{0};
  std::int32_t owner_pid{0};
  std::uint32_t owner_role{
      static_cast<std::uint32_t>(SlotOwnerRole::None)};
  std::uint64_t owner_process_start_ticks{0};
  std::uint64_t owner_role_token{0};
  std::uint64_t owner_channel_generation{0};
};
static_assert(sizeof(SlotHeader) == kCacheLine);

[[nodiscard]] constexpr std::size_t AlignUp(std::size_t value,
                                            std::size_t alignment) noexcept {
  return (value + alignment - 1U) & ~(alignment - 1U);
}

}  // namespace fastipc::detail
