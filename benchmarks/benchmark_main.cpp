#include "benchmark_runner.hpp"

#include <charconv>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using fastipc::benchmark::AccessPattern;
using fastipc::benchmark::CaseConfig;
using fastipc::benchmark::TransportKind;

struct Options {
  std::vector<TransportKind> transports{
      TransportKind::FastIpcCopy,
      TransportKind::FastIpcZeroCopy,
      TransportKind::UnixDomainSocket,
      TransportKind::Pipe};
  std::vector<AccessPattern> access_patterns{
      AccessPattern::TransportOnly, AccessPattern::TouchMemory};
  std::vector<std::size_t> payload_sizes =
      fastipc::benchmark::RequiredPayloadSizes();
  std::optional<std::size_t> iterations;
  std::optional<std::size_t> warmup_iterations;
  bool self_test{false};
};

std::size_t ParsePositiveSize(std::string_view text) {
  if (text.empty()) {
    throw std::invalid_argument("numeric option is empty");
  }

  std::size_t multiplier = 1U;
  const char suffix = text.back();
  if (suffix == 'K' || suffix == 'k') {
    multiplier = 1024U;
    text.remove_suffix(1U);
  } else if (suffix == 'M' || suffix == 'm') {
    multiplier = 1024U * 1024U;
    text.remove_suffix(1U);
  }

  std::size_t value = 0U;
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} ||
      parsed.ptr != text.data() + text.size() || value == 0U ||
      value > std::numeric_limits<std::size_t>::max() / multiplier) {
    throw std::invalid_argument("invalid positive size: " +
                                std::string(text));
  }
  return value * multiplier;
}

std::string_view OptionValue(std::string_view argument,
                             std::string_view prefix) {
  if (!argument.starts_with(prefix)) {
    return {};
  }
  return argument.substr(prefix.size());
}

void PrintHelp() {
  std::cout
      << "Usage: fastipc_benchmark [options]\n"
      << "  --self-test                 run 4 transports x 2 modes x 2 sizes\n"
      << "  --transport=NAME            fastipc_copy, fastipc_zero_copy, "
         "unix_domain_socket, pipe, or all\n"
      << "  --access=NAME               transport_only, touch_memory, or all\n"
      << "  --payload=BYTES             one size; K and M suffixes work\n"
      << "  --iterations=COUNT          measured round trips per case\n"
      << "  --warmup=COUNT              warmup round trips per case\n"
      << "  --help                      show this message\n";
}

Options ParseOptions(int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--self-test") {
      options.self_test = true;
      continue;
    }
    if (argument == "--help") {
      PrintHelp();
      std::exit(0);
    }

    if (const auto value =
            OptionValue(argument, "--transport=");
        !value.empty()) {
      if (value == "all") {
        options.transports = {
            TransportKind::FastIpcCopy,
            TransportKind::FastIpcZeroCopy,
            TransportKind::UnixDomainSocket,
            TransportKind::Pipe};
      } else {
        const auto transport =
            fastipc::benchmark::ParseTransport(value);
        if (!transport) {
          throw std::invalid_argument(
              "unknown transport: " + std::string(value));
        }
        options.transports = {*transport};
      }
      continue;
    }

    if (const auto value = OptionValue(argument, "--access=");
        !value.empty()) {
      if (value == "all") {
        options.access_patterns = {
            AccessPattern::TransportOnly,
            AccessPattern::TouchMemory};
      } else {
        const auto access_pattern =
            fastipc::benchmark::ParseAccessPattern(value);
        if (!access_pattern) {
          throw std::invalid_argument(
              "unknown access pattern: " + std::string(value));
        }
        options.access_patterns = {*access_pattern};
      }
      continue;
    }

    if (const auto value = OptionValue(argument, "--payload=");
        !value.empty()) {
      options.payload_sizes = {ParsePositiveSize(value)};
      continue;
    }

    if (const auto value =
            OptionValue(argument, "--iterations=");
        !value.empty()) {
      options.iterations = ParsePositiveSize(value);
      continue;
    }

    if (const auto value = OptionValue(argument, "--warmup=");
        !value.empty()) {
      options.warmup_iterations = ParsePositiveSize(value);
      continue;
    }

    throw std::invalid_argument(
        "unknown option: " + std::string(argument));
  }

  if (options.self_test) {
    options.transports = {
        TransportKind::FastIpcCopy,
        TransportKind::FastIpcZeroCopy,
        TransportKind::UnixDomainSocket,
        TransportKind::Pipe};
    options.access_patterns = {
        AccessPattern::TransportOnly, AccessPattern::TouchMemory};
    options.payload_sizes = {64U, 1024U * 1024U};
    options.iterations = 3U;
    options.warmup_iterations = 1U;
  }
  return options;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto options = ParseOptions(argc, argv);
    std::cout << fastipc::benchmark::ToJson(
                     fastipc::benchmark::CaptureEnvironment())
              << std::endl;

    std::uint64_t case_id = 1U;
    for (const auto transport : options.transports) {
      for (const auto access_pattern : options.access_patterns) {
        for (const auto payload_bytes : options.payload_sizes) {
          CaseConfig config;
          config.transport = transport;
          config.access_pattern = access_pattern;
          config.payload_bytes = payload_bytes;
          config.iterations =
              options.iterations.value_or(
                  fastipc::benchmark::DefaultIterations(payload_bytes));
          config.warmup_iterations =
              options.warmup_iterations.value_or(
                  fastipc::benchmark::DefaultWarmupIterations(
                      config.iterations));
          config.case_id = case_id++;

          auto result = fastipc::benchmark::RunCase(config);
          if (!result) {
            std::cerr << "benchmark case failed: transport="
                      << fastipc::benchmark::TransportName(transport)
                      << " access="
                      << fastipc::benchmark::AccessPatternName(access_pattern)
                      << " payload=" << payload_bytes << " status="
                      << result.status().ToString() << '\n';
            return 1;
          }
          std::cout << fastipc::benchmark::ToJson(result.value())
                    << std::endl;
        }
      }
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "fastipc_benchmark: " << error.what() << '\n';
    return 2;
  }
}
