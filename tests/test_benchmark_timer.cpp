#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../tools/benchmark_timer.hpp"
#include <cassert>
#include <sstream>

struct FakeClock {
  using duration = std::chrono::nanoseconds;
  using time_point = std::chrono::time_point<FakeClock, duration>;
  static inline int64_t ticks = 0;
  static inline uint64_t reads = 0;
  static time_point now() {
    ++reads;
    return time_point(duration(ticks));
  }
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
  auto overshoot =
      stanli_benchmark::window<FakeClock>(slow, 200'000'000, 1, true);
  assert(overshoot.iterations == 1);
  assert(overshoot.elapsed_ns == 500'000'000);
  assert(overshoot.batch == 1);
  bool rejected = false;
  try {
    stanli_benchmark::window<FakeClock>(fast, 0);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  assert(rejected);

  assert(stanli_benchmark::splitmix64(0) == 0xE220A8397B1DCDAFULL);
  const auto origin = stanli_benchmark::evaluation_point(0, 9);
  assert(origin.size() == 9);
  for (int64_t i = 0; i < 9; ++i)
    assert(origin[i] == 0.1 + 0.05 * (i % 7) - 0.15 * (i % 3));
  const auto narrow = stanli_benchmark::evaluation_point(3, 4);
  const auto wide = stanli_benchmark::evaluation_point(3, 8);
  for (int64_t i = 0; i < 4; ++i) {
    assert(narrow[i] == wide[i]);
    assert(narrow[i] >= -2 && narrow[i] < 2);
  }
  assert(narrow != stanli_benchmark::evaluation_point(4, 4));
  assert(narrow == stanli_benchmark::evaluation_point(3, 4));

  const auto field = [](const std::string& text, const std::string& point,
                        const std::string& key) {
    const auto start = text.find(point);
    assert(start != std::string::npos);
    const auto at = text.find(key, start);
    assert(at != std::string::npos);
    return std::stoll(text.substr(at + key.size()));
  };
  const auto count = [](const std::string& text, const std::string& what) {
    int64_t found = 0;
    for (auto at = text.find(what); at != std::string::npos;
         at = text.find(what, at + 1))
      ++found;
    return found;
  };
  {
    stanli_benchmark::Options options;
    options.points = 4;
    options.warmup_ns = 50'000;
    options.measure_ns = 100'000;
    std::vector<double> q;
    const auto set = [&](const std::vector<double>& x) { q = x; };
    const auto is = [&](int k) { return q == stanli_benchmark::evaluation_point(k, 3); };
    const auto one = [&] {
      if (is(3)) throw std::domain_error("rejected");
      FakeClock::ticks += is(1) ? 200 : 100;
    };
    const auto values = [&]() -> std::vector<double> {
      return {is(2) ? std::nan("") : -1.5, 2.0, 3.0};
    };
    std::ostringstream out;
    stanli_benchmark::timed<FakeClock>(options, 3, set, one, values, out);
    const auto text = out.str();
    assert(text.find("\"protocol\":\"stanli-gradient-v3\"") !=
           std::string::npos);
    assert(text.find("\"warmup_iterations\":") != std::string::npos);
    assert(text.find("\"point\":0,\"finite\":true,\"values\":[-1.5,2,3]") !=
           std::string::npos);
    assert(text.find("\"point\":2,\"finite\":false}") != std::string::npos);
    assert(text.find("\"point\":3,\"finite\":false}") != std::string::npos);
    assert(count(text, "\"iterations\":") == 2);
    assert(count(text, "\"elapsed_ns\":") == 2);
    for (const char* point : {"\"point\":0", "\"point\":1"})
      assert(field(text, point, "\"elapsed_ns\":") >= 100'000);
    const auto cheap = field(text, "\"point\":0", "\"elapsed_ns\":") /
                       field(text, "\"point\":0", "\"iterations\":");
    const auto dear = field(text, "\"point\":1", "\"elapsed_ns\":") /
                      field(text, "\"point\":1", "\"iterations\":");
    assert(cheap == 100 && dear == 200);
  }
  {
    stanli_benchmark::Options options;
    options.points = 2;
    const auto set = [](const std::vector<double>&) {};
    const auto one = [] { FakeClock::ticks += 1; };
    const auto values = []() -> std::vector<double> { return {std::nan("")}; };
    std::ostringstream out;
    bool failed = false;
    try {
      stanli_benchmark::timed<FakeClock>(options, 1, set, one, values, out);
    } catch (const std::runtime_error&) {
      failed = true;
    }
    assert(failed);
    assert(out.str().empty());
  }
  {
    char program[] = "driver";
    char warm[] = "--warmup-ms";
    char warm_value[] = "7";
    char pts[] = "--points";
    char pts_value[] = "3";
    char* argv[] = {program, warm, warm_value, pts, pts_value};
    const auto options = stanli_benchmark::options(5, argv, 1);
    assert(options.warmup_ns == 7'000'000);
    assert(options.measure_ns == 40'000'000);
    assert(options.points == 3);
    const auto defaults = stanli_benchmark::options(1, argv, 1);
    assert(defaults.warmup_ns == 20'000'000 && defaults.points == 10);
    char zero[] = "0";
    char* bad[] = {program, pts, zero};
    bool rejected_points = false;
    try {
      stanli_benchmark::options(3, bad, 1);
    } catch (const std::invalid_argument&) {
      rejected_points = true;
    }
    assert(rejected_points);
  }
}
