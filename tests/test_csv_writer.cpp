#include "../tools/csv_writer.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

int main() {
  using Limits = std::numeric_limits<double>;
  std::vector<double> values{0.0,
                             -0.0,
                             Limits::min(),
                             Limits::max(),
                             Limits::denorm_min(),
                             Limits::lowest(),
                             Limits::infinity(),
                             -Limits::infinity(),
                             Limits::quiet_NaN(),
                             -Limits::quiet_NaN()};
  // Decimal notation boundaries and adjacent representable values.
  for (double x : {1e-5, 1e-4, 1e16, 1e17, 1.0}) {
    values.push_back(x);
    values.push_back(std::nextafter(x, 0.0));
    values.push_back(std::nextafter(x, Limits::infinity()));
  }
  std::mt19937_64 rng(891);
  for (int i = 0; i < 100000; ++i) {
    const uint64_t bits = rng();
    double x;
    std::memcpy(&x, &bits, sizeof(x));
    values.push_back(x);
  }
  std::FILE* file = std::tmpfile();
  if (!file) return 1;
  stanli::tooling::CsvWriter csv(file);
  std::string expected;
  char legacy[64];
  for (size_t i = 0; i < values.size(); ++i) {
    csv.value(values[i]);
    if (i % 13) expected += ',';
    std::snprintf(legacy, sizeof(legacy), "%.17g", values[i]);
    expected += legacy;
    if (i % 13 == 12 || i + 1 == values.size()) {
      csv.end_row();
      expected += '\n';
    }
  }
  csv.flush();
  // Explicit flushing may happen at any field boundary, including mid-row.
  csv.value(-0.0);
  csv.flush();
  csv.value(0.0);
  csv.end_row();
  csv.flush();
  expected += "-0,0\n";
  std::rewind(file);
  std::string actual(expected.size(), '\0');
  const auto count = std::fread(actual.data(), 1, actual.size(), file);
  const bool pass =
      count == expected.size() && actual == expected && std::fgetc(file) == EOF;
  std::fclose(file);
  if (!pass) {
    size_t i = 0;
    while (i < actual.size() && actual[i] == expected[i]) ++i;
    std::fprintf(stderr,
                 "CSV differs from legacy %%.17g at byte %zu: got %.80s "
                 "expected %.80s\n",
                 i, actual.c_str() + i, expected.c_str() + i);
  }
  return pass ? 0 : 1;
}
