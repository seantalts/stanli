#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../tools/benchmark_timer.hpp"
#include <cassert>

struct FakeClock {
  using duration = std::chrono::nanoseconds;
  using time_point = std::chrono::time_point<FakeClock, duration>;
  static inline int64_t ticks = 0;
  static inline uint64_t reads = 0;
  static time_point now() { ++reads; return time_point(duration(ticks)); }
};

int main() {
  // Fast operations must warm past 1,000 calls, with amortized clock reads.
  auto fast = [] { FakeClock::ticks += 1; };
  auto warm = stanli_benchmark::window<FakeClock>(fast, 20000, 1, true);
  assert(warm.elapsed_ns >= 20000);
  assert(warm.iterations >= 20000);
  assert(warm.batch > 1);
  assert(FakeClock::reads * 100 < warm.iterations);
  auto measured = stanli_benchmark::window<FakeClock>(fast, 30000, warm.batch);
  assert(measured.elapsed_ns == static_cast<int64_t>(measured.iterations));
  assert(measured.elapsed_ns >= 30000);

  // A single evaluation can exceed the window. Report the real elapsed time.
  auto slow = [] { FakeClock::ticks += 500'000'000; };
  auto overshoot = stanli_benchmark::window<FakeClock>(slow, 200'000'000, 1, true);
  assert(overshoot.iterations == 1);
  assert(overshoot.elapsed_ns == 500'000'000);
  assert(overshoot.batch == 1);
  bool rejected = false;
  try { stanli_benchmark::window<FakeClock>(fast, 0); }
  catch (const std::invalid_argument&) { rejected = true; }
  assert(rejected);
}
