#include <fastipc/status.hpp>

#include <array>
#include <string>

namespace fastipc {
namespace {

constexpr std::array<const char*, 19> kStatusNames{
    "ok",
    "invalid argument",
    "already exists",
    "not found",
    "permission denied",
    "layout mismatch",
    "role conflict",
    "message too large",
    "buffer too small",
    "would block",
    "timeout",
    "dropped",
    "peer unavailable",
    "peer dead",
    "stale generation",
    "corrupt data",
    "closed",
    "I/O error",
    "unsupported",
};

}  // namespace

std::string Status::ToString() const {
  const auto index = static_cast<std::size_t>(code_);
  std::string text =
      index < kStatusNames.size() ? kStatusNames[index] : "unknown status";
  if (!detail_.empty()) {
    text += ": ";
    text += detail_;
  }
  if (native_error_ != 0) {
    text += " (errno=" + std::to_string(native_error_) + ")";
  }
  return text;
}

}  // namespace fastipc
