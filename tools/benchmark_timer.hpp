#pragma once

// Timing policy shared by the two benchmark drivers. No model-size heuristic:
// warm for an elapsed-time window, calibrating batches to amortize clock reads,
// then measure whole batches until the requested measurement window expires.
// Each evaluation point gets its own short calibration and measurement window.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

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

inline uint64_t splitmix64(uint64_t x) {
  x += 0x9E3779B97F4A7C15ULL;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
  return x ^ (x >> 31);
}

inline std::vector<double> evaluation_point(int point, int64_t n) {
  std::vector<double> q(static_cast<size_t>(n));
  for (int64_t i = 0; i < n; ++i) {
    if (point == 0) {
      q[i] = 0.1 + 0.05 * static_cast<double>(i % 7) -
             0.15 * static_cast<double>(i % 3);
      continue;
    }
    const uint64_t seed = splitmix64(static_cast<uint64_t>(point) *
                                     0x9E3779B97F4A7C15ULL) ^
                          static_cast<uint64_t>(i);
    const double u = static_cast<double>(splitmix64(seed) >> 11) * 0x1.0p-53;
    q[i] = -2.0 + 4.0 * u;
  }
  return q;
}

struct Options {
  int64_t warmup_ns = 20'000'000;
  int64_t measure_ns = 40'000'000;
  int points = 10;
};

inline Options options(int argc, char** argv, int first) {
  Options result;
  for (int i = first; i < argc; i += 2) {
    const std::string flag(argv[i]);
    if (i + 1 >= argc ||
        (flag != "--warmup-ms" && flag != "--measure-ms" && flag != "--points"))
      throw std::invalid_argument(
          "expected --warmup-ms N, --measure-ms N or --points N");
    size_t consumed = 0;
    const std::string value(argv[i + 1]);
    if (flag == "--points") {
      const long points = std::stol(value, &consumed);
      if (consumed != value.size() || points < 1 || points > 1000)
        throw std::invalid_argument("benchmark points must be 1..1000");
      result.points = static_cast<int>(points);
      continue;
    }
    const double ms = std::stod(value, &consumed);
    if (consumed != value.size() || !std::isfinite(ms) || ms < 1 || ms > 60'000)
      throw std::invalid_argument(
          "benchmark windows must be 1..60000 milliseconds");
    (flag == "--warmup-ms" ? result.warmup_ns : result.measure_ns) =
        static_cast<int64_t>(ms * 1e6);
  }
  return result;
}

constexpr int64_t calibration_ns = 2'000'000;

template <class Clock = std::chrono::steady_clock, class Set, class One,
          class Values>
void timed(const Options& opts, int64_t n, Set&& set, One&& one,
           Values&& values, std::ostream& out = std::cout) {
  struct Point {
    bool finite = false;
    std::vector<double> values;
    Window measured;
  };
  std::vector<Point> points(static_cast<size_t>(opts.points));
  std::vector<std::vector<double>> q(points.size());
  int first = -1;
  for (int k = 0; k < opts.points; ++k) {
    q[k] = evaluation_point(k, n);
    try {
      set(q[k]);
      one();
      points[k].values = values();
      points[k].finite =
          std::all_of(points[k].values.begin(), points[k].values.end(),
                      [](double v) { return std::isfinite(v); });
    } catch (const std::exception&) {
      points[k].finite = false;
    }
    if (points[k].finite && first < 0) first = k;
  }
  if (first < 0)
    throw std::runtime_error(
        "benchmark has no point with a finite density and gradient");
  set(q[first]);
  const auto warm = window<Clock>(one, opts.warmup_ns, 1, true);
  for (int k = 0; k < opts.points; ++k) {
    if (!points[k].finite) continue;
    set(q[k]);
    const auto fit = window<Clock>(one, calibration_ns, 1, true);
    points[k].measured = window<Clock>(one, opts.measure_ns, fit.batch);
  }
  out << std::setprecision(17) << "{\"protocol\":\"stanli-gradient-v3\""
      << ",\"warmup_iterations\":" << warm.iterations
      << ",\"warmup_elapsed_ns\":" << warm.elapsed_ns << ",\"points\":[";
  for (int k = 0; k < opts.points; ++k) {
    const auto& point = points[k];
    out << (k ? "," : "") << "{\"point\":" << k
        << ",\"finite\":" << (point.finite ? "true" : "false");
    if (point.finite) {
      out << ",\"values\":[";
      for (size_t i = 0; i < point.values.size(); ++i)
        out << (i ? "," : "") << point.values[i];
      out << "],\"iterations\":" << point.measured.iterations
          << ",\"elapsed_ns\":" << point.measured.elapsed_ns
          << ",\"batch\":" << point.measured.batch;
    }
    out << "}";
  }
  out << "]}" << std::endl;
}
}  // namespace stanli_benchmark
