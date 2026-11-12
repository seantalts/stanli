// Assertions shared by the C++ tests. Each check counts into `failures`
// and prints one FAIL line; main() ends with `return finish("test_x");`.
#ifndef STANLI_TESTS_EXPECT_HPP
#define STANLI_TESTS_EXPECT_HPP

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>

inline int failures = 0;

inline bool same_bits(double a, double b) {
  return std::memcmp(&a, &b, sizeof(double)) == 0;
}

inline bool both_nan(double a, double b) {
  return std::isnan(a) && std::isnan(b);
}

// Representable doubles between a and b. Values of opposite sign, and any
// NaN, are infinitely far apart; equal values (so +0 and -0) are 0 apart.
inline uint64_t ulp_distance(double a, double b) {
  if (a == b) return 0;
  if (std::isnan(a) || std::isnan(b) || std::signbit(a) != std::signbit(b))
    return std::numeric_limits<uint64_t>::max();
  uint64_t x, y;
  std::memcpy(&x, &a, sizeof x);
  std::memcpy(&y, &b, sizeof y);
  return x > y ? x - y : y - x;
}

inline void report_mismatch(const std::string& what, double got, double want,
                            const char* why) {
  ++failures;
  const uint64_t d = ulp_distance(got, want);
  if (d == std::numeric_limits<uint64_t>::max())
    std::printf("FAIL %s got %.17g want %.17g (%s)\n", what.c_str(), got, want,
                why);
  else
    std::printf("FAIL %s got %.17g want %.17g (%s, %llu ulp)\n", what.c_str(),
                got, want, why, (unsigned long long)d);
}

// Bitwise: +0 and -0 differ, and a NaN never matches.
inline void expect_eq(const std::string& what, double got, double want) {
  if (std::isnan(got) || std::isnan(want))
    report_mismatch(what, got, want, "NaN");
  else if (!same_bits(got, want))
    report_mismatch(what, got, want, "bits differ");
}

// Bitwise, except that any NaN matches any NaN.
inline void expect_eq_or_both_nan(const std::string& what, double got,
                                  double want) {
  if (!same_bits(got, want) && !both_nan(got, want))
    report_mismatch(what, got, want, "bits differ");
}

inline void expect_ulp(const std::string& what, double got, double want,
                       uint64_t max_ulp) {
  if (ulp_distance(got, want) > max_ulp)
    report_mismatch(what, got, want, "over ulp budget");
}

// |got - want| within `max_ulp` spacings of the double at |scale|. Unlike
// ulp_distance this does not count representable values, so it is the
// looser check across a binade boundary below |scale|.
inline void expect_ulp_of(const std::string& what, double got, double want,
                          double scale, int64_t max_ulp) {
  if (got == want) return;
  const double s = std::abs(scale);
  const double spacing = std::max(std::nextafter(s, 1e308) - s,
                                  std::numeric_limits<double>::denorm_min());
  if (std::isfinite(got) && std::isfinite(want) &&
      std::abs(got - want) <= (double)max_ulp * spacing)
    return;
  ++failures;
  std::printf("FAIL %s got %.17g want %.17g (%.1f ulp of %g, budget %lld)\n",
              what.c_str(), got, want, std::abs(got - want) / spacing, scale,
              (long long)max_ulp);
}

inline void expect_true(const std::string& what, bool ok) {
  if (ok) return;
  ++failures;
  std::printf("FAIL %s\n", what.c_str());
}

inline std::string slurp(const std::string& path) {
  std::ifstream f(path);
  if (!f) throw std::runtime_error("cannot open " + path);
  std::ostringstream out;
  out << f.rdbuf();
  return out.str();
}

inline int finish(const char* name) {
  if (failures == 0) {
    std::printf("%s OK\n", name);
    return 0;
  }
  std::printf("%s: %d failures\n", name, failures);
  return 1;
}

#endif
