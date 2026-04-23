#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace fastipc::detail {

inline constexpr std::array<char, 8> kMpmcMagic{
    'F', 'I', 'P', 'C', 'M', 'P', 'M', 'C'};
inline constexpr std::uint16_t kMpmcLayoutMajor = 1U;
inline constexpr std::uint16_t kMpmcLayoutMinor = 0U;
inline constexpr std::uint32_t kMpmcEndianMarker = 0x01020304U;
inline constexpr std::uint32_t kMpmcInitInitializing = 1U;
inline constexpr std::uint32_t kMpmcInitReady = 2U;
inline constexpr std::size_t kMpmcCacheLine = 64U;

struct alignas(kMpmcCacheLine) MpmcSegmentHeader {
  std::array<char, 8> magic{};
  std::uint16_t version_major{0U};
  std::uint16_t version_minor{0U};
  std::uint32_t header_bytes{0U};
  std::uint64_t segment_bytes{0U};
  std::uint64_t created_monotonic_ns{0U};
  std::uint32_t endian_marker{0U};
  std::uint32_t init_state{0U};
  std::array<std::byte, 24> reserved{};
};
static_assert(sizeof(MpmcSegmentHeader) == kMpmcCacheLine);

struct alignas(kMpmcCacheLine) MpmcQueueConfig {
  std::uint32_t capacity{0U};
  std::uint32_t max_message_size{0U};
  std::uint32_t slot_stride{0U};
  std::uint32_t slots_offset{0U};
  std::array<std::byte, 48> reserved{};
};
static_assert(sizeof(MpmcQueueConfig) == kMpmcCacheLine);

struct alignas(kMpmcCacheLine) MpmcEnqueueCursor {
  std::uint64_t position{0U};
  std::uint32_t data_epoch{0U};
  std::uint32_t reserved_word{0U};
  std::uint64_t sent_messages{0U};
  std::uint64_t dropped_messages{0U};
  std::uint64_t timeout_count{0U};
  std::array<std::byte, 24> reserved{};
};
static_assert(sizeof(MpmcEnqueueCursor) == kMpmcCacheLine);

struct alignas(kMpmcCacheLine) MpmcDequeueCursor {
  std::uint64_t position{0U};
  std::uint32_t space_epoch{0U};
  std::uint32_t reserved_word{0U};
  std::uint64_t received_messages{0U};
  std::uint64_t timeout_count{0U};
  std::uint64_t corrupt_messages{0U};
  std::uint64_t buffer_too_small_count{0U};
  std::array<std::byte, 16> reserved{};
};
static_assert(sizeof(MpmcDequeueCursor) == kMpmcCacheLine);

struct alignas(kMpmcCacheLine) MpmcSharedLayout {
  MpmcSegmentHeader header;
  MpmcQueueConfig queue;
  MpmcEnqueueCursor enqueue;
  MpmcDequeueCursor dequeue;
};
static_assert(sizeof(MpmcSharedLayout) == 4U * kMpmcCacheLine);
static_assert(std::is_standard_layout_v<MpmcSharedLayout>);

struct alignas(kMpmcCacheLine) MpmcSlotHeader {
  std::uint64_t sequence{0U};
  std::uint32_t length{0U};
  std::uint32_t reserved_word{0U};
  std::array<std::byte, 48> reserved{};
};
static_assert(sizeof(MpmcSlotHeader) == kMpmcCacheLine);

[[nodiscard]] constexpr std::size_t MpmcAlignUp(
    std::size_t value, std::size_t alignment) noexcept {
  return (value + alignment - 1U) & ~(alignment - 1U);
}

[[nodiscard]] inline MpmcSlotHeader* MpmcSlotAt(
    MpmcSharedLayout* layout, std::uint64_t position) noexcept {
  auto* base = reinterpret_cast<std::byte*>(layout);
  const auto index =
      position &
      static_cast<std::uint64_t>(layout->queue.capacity - 1U);
  const auto offset =
      static_cast<std::size_t>(layout->queue.slots_offset) +
      static_cast<std::size_t>(index) *
          static_cast<std::size_t>(layout->queue.slot_stride);
  return reinterpret_cast<MpmcSlotHeader*>(base + offset);
}

[[nodiscard]] inline const MpmcSlotHeader* MpmcSlotAt(
    const MpmcSharedLayout* layout,
    std::uint64_t position) noexcept {
  return MpmcSlotAt(
      const_cast<MpmcSharedLayout*>(layout), position);
}

[[nodiscard]] inline std::byte* MpmcSlotPayload(
    MpmcSlotHeader* slot) noexcept {
  return reinterpret_cast<std::byte*>(slot) +
         sizeof(MpmcSlotHeader);
}

}  // namespace fastipc::detail
