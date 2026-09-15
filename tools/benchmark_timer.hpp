#pragma once

// Timing policy shared by the two benchmark drivers. No model-size heuristic:
// warm for an elapsed-time window, calibrating batches to amortize clock reads,
// then measure whole batches until the requested measurement window expires.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

namespace stanli_benchmark {
struct Window {
  uint64_t iterations = 0;
  int64_t elapsed_ns = 0;
  uint64_t batch = 1;
};

template <class Clock = std::chrono::steady_clock, class F>
Window window(F&& one, int64_t duration_ns, uint64_t batch = 1,
              bool calibrate = false) {
  if (duration_ns <= 0 || batch == 0)
    throw std::invalid_argument(
        "benchmark duration and batch must be positive");
  Window result;
  const auto start = Clock::now();
  auto previous = start;
  do {
    for (uint64_t i = 0; i < batch; ++i) one();
    result.iterations += batch;
    const auto now = Clock::now();
    const auto batch_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - previous)
            .count();
    result.elapsed_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - start)
            .count();
    previous = now;
    // About one clock read per millisecond, even for nanosecond-scale models.
    // A slow evaluation remains a batch of one; the observed overshoot stays
    // in the result rather than being truncated to the requested duration.
    if (calibrate && batch_ns < 1'000'000 && batch < (1u << 20)) batch *= 2;
  } while (result.elapsed_ns < duration_ns);
  result.batch = batch;
  return result;
}

struct Options {
  int64_t warmup_ns = 200'000'000;
  int64_t measure_ns = 250'000'000;
};

inline Options options(int argc, char** argv, int first) {
  Options result;
  for (int i = first; i < argc; i += 2) {
    const std::string flag(argv[i]);
    if (i + 1 >= argc || (flag != "--warmup-ms" && flag != "--measure-ms"))
      throw std::invalid_argument("expected --warmup-ms N or --measure-ms N");
    size_t consumed = 0;
    const std::string value(argv[i + 1]);
    const double ms = std::stod(value, &consumed);
    if (consumed != value.size() || !std::isfinite(ms) || ms < 1 || ms > 60'000)
      throw std::invalid_argument(
          "benchmark windows must be 1..60000 milliseconds");
    (flag == "--warmup-ms" ? result.warmup_ns : result.measure_ns) =
        static_cast<int64_t>(ms * 1e6);
  }
  return result;
}

template <class G>
void output(const Window& warm, const Window& measured, double lp,
            const G& grad) {
  if (!std::isfinite(lp)) throw std::runtime_error("non-finite log density");
  for (int64_t i = 0; i < static_cast<int64_t>(grad.size()); ++i)
    if (!std::isfinite(grad[i]))
      throw std::runtime_error("non-finite gradient");
  std::cout << std::setprecision(17)
            << "{\"protocol\":\"stanli-gradient-v2\",\"iterations\":"
            << measured.iterations << ",\"elapsed_ns\":" << measured.elapsed_ns
            << ",\"batch\":" << measured.batch
            << ",\"warmup_iterations\":" << warm.iterations
            << ",\"warmup_elapsed_ns\":" << warm.elapsed_ns << ",\"values\":["
            << lp;
  for (int64_t i = 0; i < static_cast<int64_t>(grad.size()); ++i)
    std::cout << ',' << grad[i];
  std::cout << "]}" << std::endl;
}
}  // namespace stanli_benchmark
