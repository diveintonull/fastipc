#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include <fastipc/deadline.hpp>
#include <fastipc/status.hpp>

namespace fastipc {

enum class BackpressurePolicy : std::uint8_t {
  Block,
  Timeout,
  Drop,
};

struct SendOptions {
  BackpressurePolicy policy{BackpressurePolicy::Block};
  Deadline deadline{Deadline::Infinite()};
};

struct TransportStats {
  std::uint64_t sent_messages{0};
  std::uint64_t received_messages{0};
  std::uint64_t dropped_messages{0};
  std::uint64_t send_timeouts{0};
  std::uint64_t receive_timeouts{0};
  std::uint64_t corrupt_messages{0};
  std::uint64_t zero_copy_loans{0};
  std::uint64_t zero_copy_publishes{0};
  std::uint64_t zero_copy_takes{0};
  std::uint64_t zero_copy_releases{0};
  std::uint64_t producer_loan_reclaims{0};
  std::uint64_t consumer_loan_reclaims{0};
};

class Transport {
 public:
  virtual ~Transport() = default;

  virtual Status Send(std::span<const std::byte> message,
                      SendOptions options) = 0;
  virtual Result<std::size_t> Receive(std::span<std::byte> destination,
                                      Deadline deadline) = 0;
  [[nodiscard]] virtual TransportStats Stats() const noexcept = 0;
  virtual void Close() noexcept = 0;
};

}  // namespace fastipc
