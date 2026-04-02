#pragma once

#include <chrono>
#include <cstdint>
#include <limits>

namespace fastipc {

class Deadline {
 public:
  using Clock = std::chrono::steady_clock;
  using TimePoint = Clock::time_point;

  [[nodiscard]] static Deadline Infinite() noexcept {
    return Deadline(true, TimePoint::max());
  }

  [[nodiscard]] static Deadline Immediate() noexcept {
    return Deadline(false, Clock::now());
  }

  template <typename Rep, typename Period>
  [[nodiscard]] static Deadline After(
      std::chrono::duration<Rep, Period> duration) noexcept {
    const auto now = Clock::now();
    const auto converted =
        std::chrono::duration_cast<Clock::duration>(duration);
    if (converted <= Clock::duration::zero()) {
      return Deadline(false, now);
    }
    if (TimePoint::max() - now < converted) {
      return Infinite();
    }
    return Deadline(false, now + converted);
  }

  [[nodiscard]] static Deadline At(TimePoint time_point) noexcept {
    return Deadline(false, time_point);
  }

  [[nodiscard]] bool infinite() const noexcept { return infinite_; }
  [[nodiscard]] bool expired() const noexcept {
    return !infinite_ && Clock::now() >= time_point_;
  }
  [[nodiscard]] TimePoint time_point() const noexcept { return time_point_; }

 private:
  Deadline(bool infinite, TimePoint time_point) noexcept
      : infinite_(infinite), time_point_(time_point) {}

  bool infinite_{false};
  TimePoint time_point_{};
};

}  // namespace fastipc
