#include "chaos/chaos_runner.hpp"

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

struct Options {
  fastipc::chaos::RunnerConfig config;
  std::string output_path;
  bool self_test{false};
  bool help{false};
};

[[nodiscard]] std::string_view OptionValue(
    int& index, int argc, char** argv, std::string_view argument,
    std::string_view option) {
  const std::string prefix = std::string(option) + "=";
  if (argument.starts_with(prefix)) {
    return argument.substr(prefix.size());
  }
  if (argument == option && index + 1 < argc) {
    ++index;
    return argv[index];
  }
  throw std::invalid_argument(
      "missing value for " + std::string(option));
}

[[nodiscard]] std::uint64_t ParseUnsigned(
    std::string_view value, std::string_view name) {
  std::uint64_t parsed = 0U;
  const auto result =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (result.ec != std::errc{} ||
      result.ptr != value.data() + value.size()) {
    throw std::invalid_argument(
        std::string(name) + " must be an unsigned integer");
  }
  return parsed;
}

[[nodiscard]] double ParseDouble(
    std::string_view value, std::string_view name) {
  double parsed = 0.0;
  const auto result =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (result.ec != std::errc{} ||
      result.ptr != value.data() + value.size()) {
    throw std::invalid_argument(
        std::string(name) + " must be a number");
  }
  return parsed;
}

template <typename T>
[[nodiscard]] T CheckedCast(
    std::uint64_t value, std::string_view name) {
  if (value > static_cast<std::uint64_t>(
                  std::numeric_limits<T>::max())) {
    throw std::invalid_argument(std::string(name) + " is too large");
  }
  return static_cast<T>(value);
}

[[nodiscard]] Options ParseOptions(int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--help" || argument == "-h") {
      options.help = true;
      continue;
    }
    if (argument == "--self-test") {
      options.self_test = true;
      continue;
    }
    if (argument == "--seed" || argument.starts_with("--seed=")) {
      options.config.seed = ParseUnsigned(
          OptionValue(index, argc, argv, argument, "--seed"), "seed");
      continue;
    }
    if (argument == "--operations" ||
        argument.starts_with("--operations=")) {
      options.config.minimum_operations = CheckedCast<std::size_t>(
          ParseUnsigned(
              OptionValue(index, argc, argv, argument, "--operations"),
              "operations"),
          "operations");
      continue;
    }
    if (argument == "--duration-ms" ||
        argument.starts_with("--duration-ms=")) {
      options.config.minimum_duration = std::chrono::milliseconds(
          CheckedCast<std::chrono::milliseconds::rep>(
              ParseUnsigned(
                  OptionValue(
                      index, argc, argv, argument, "--duration-ms"),
                  "duration-ms"),
              "duration-ms"));
      continue;
    }
    if (argument == "--payload" ||
        argument.starts_with("--payload=")) {
      options.config.payload_size = CheckedCast<std::size_t>(
          ParseUnsigned(
              OptionValue(index, argc, argv, argument, "--payload"),
              "payload"),
          "payload");
      continue;
    }
    if (argument == "--slot-count" ||
        argument.starts_with("--slot-count=")) {
      options.config.slot_count = CheckedCast<std::uint32_t>(
          ParseUnsigned(
              OptionValue(index, argc, argv, argument, "--slot-count"),
              "slot-count"),
          "slot-count");
      continue;
    }
    if (argument == "--peer-timeout-ms" ||
        argument.starts_with("--peer-timeout-ms=")) {
      options.config.peer_timeout = std::chrono::milliseconds(
          CheckedCast<std::chrono::milliseconds::rep>(
              ParseUnsigned(
                  OptionValue(
                      index, argc, argv, argument, "--peer-timeout-ms"),
                  "peer-timeout-ms"),
              "peer-timeout-ms"));
      continue;
    }
    if (argument == "--command-timeout-ms" ||
        argument.starts_with("--command-timeout-ms=")) {
      options.config.command_timeout = std::chrono::milliseconds(
          CheckedCast<std::chrono::milliseconds::rep>(
              ParseUnsigned(
                  OptionValue(
                      index, argc, argv, argument,
                      "--command-timeout-ms"),
                  "command-timeout-ms"),
              "command-timeout-ms"));
      continue;
    }
    if (argument == "--delay-ms" ||
        argument.starts_with("--delay-ms=")) {
      options.config.delay = std::chrono::milliseconds(
          CheckedCast<std::chrono::milliseconds::rep>(
              ParseUnsigned(
                  OptionValue(index, argc, argv, argument, "--delay-ms"),
                  "delay-ms"),
              "delay-ms"));
      continue;
    }
    if (argument == "--latency-window" ||
        argument.starts_with("--latency-window=")) {
      options.config.latency_window = CheckedCast<std::size_t>(
          ParseUnsigned(
              OptionValue(
                  index, argc, argv, argument, "--latency-window"),
              "latency-window"),
          "latency-window");
      continue;
    }
    if (argument == "--max-memory-growth-kib" ||
        argument.starts_with("--max-memory-growth-kib=")) {
      options.config.max_memory_growth_kib =
          CheckedCast<std::int64_t>(
              ParseUnsigned(
                  OptionValue(
                      index, argc, argv, argument,
                      "--max-memory-growth-kib"),
                  "max-memory-growth-kib"),
              "max-memory-growth-kib");
      continue;
    }
    if (argument == "--max-p99-drift-us" ||
        argument.starts_with("--max-p99-drift-us=")) {
      options.config.max_p99_drift_us = ParseDouble(
          OptionValue(
              index, argc, argv, argument, "--max-p99-drift-us"),
          "max-p99-drift-us");
      continue;
    }
    if (argument == "--run-id" ||
        argument.starts_with("--run-id=")) {
      options.config.run_id = std::string(
          OptionValue(index, argc, argv, argument, "--run-id"));
      continue;
    }
    if (argument == "--output" ||
        argument.starts_with("--output=")) {
      options.output_path = std::string(
          OptionValue(index, argc, argv, argument, "--output"));
      continue;
    }
    throw std::invalid_argument(
        "unknown argument: " + std::string(argument));
  }

  if (options.self_test) {
    options.config.minimum_operations = 8U;
    options.config.minimum_duration = std::chrono::milliseconds::zero();
    options.config.slot_count = 2U;
    options.config.payload_size = 64U;
    options.config.peer_timeout = std::chrono::milliseconds(30);
    options.config.command_timeout = std::chrono::milliseconds(1500);
    options.config.delay = std::chrono::milliseconds(10);
    options.config.latency_window = 4U;
  }
  return options;
}

void PrintUsage() {
  std::cout
      << "usage: fastipc_chaos_runner [--seed N] [--operations N] "
         "[--duration-ms MS] [--payload BYTES] [--slot-count N] "
         "[--peer-timeout-ms MS] [--command-timeout-ms MS] "
         "[--delay-ms MS] [--latency-window N] "
         "[--max-memory-growth-kib N] [--max-p99-drift-us US] "
         "[--run-id ID] [--output FILE] [--self-test]\n";
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto options = ParseOptions(argc, argv);
    if (options.help) {
      PrintUsage();
      return 0;
    }

    fastipc::chaos::Runner runner(options.config);
    if (options.output_path.empty()) {
      const auto status = runner.Run(std::cout);
      if (!status) {
        std::cerr << status.ToString() << '\n';
        return status.code() == fastipc::StatusCode::InvalidArgument ? 2 : 1;
      }
      return 0;
    }

    std::ofstream output(options.output_path, std::ios::out | std::ios::trunc);
    if (!output) {
      std::cerr << "failed to open output: " << options.output_path << '\n';
      return 2;
    }
    const auto status = runner.Run(output, &std::cout);
    output.flush();
    if (!output) {
      std::cerr << "failed to flush output: " << options.output_path << '\n';
      return 2;
    }
    if (!status) {
      std::cerr << status.ToString() << '\n';
      return status.code() == fastipc::StatusCode::InvalidArgument ? 2 : 1;
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    PrintUsage();
    return 2;
  }
}
