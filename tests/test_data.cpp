// JSON data loading (CmdStan conventions): scalars, arrays, nested arrays,
// and the same map built from a Stan var_context instead.
#include <stanli/data.hpp>

#include <stan/io/dump.hpp>

#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

static int failures = 0;
static void check(bool ok, const std::string& what) {
  if (!ok) {
    ++failures;
    std::printf("FAIL %s\n", what.c_str());
  }
}

int main() {
  using stanli::DataMap;

  DataMap d = DataMap::from_json_file("tests/fixtures/eight_schools.json");
  check(d.at("J").is_int && d.at("J").i[0] == 8, "J int 8");
  check(d.at("y").r.size() == 8 && d.at("y").r[0] == 28 && d.at("y").r[2] == -3,
        "y values");
  check(d.at("sigma").dims.size() == 1 && d.at("sigma").dims[0] == 8,
        "sigma dims");

  // Mixed int/real detection + matrices as nested arrays (row-major).
  DataMap m = DataMap::from_json(R"({
    "n": 3, "x": 2.5, "v": [1, 2, 3],
    "M": [[1.0, 2.0], [3.0, 4.0], [5.0, 6.0]],
    "yi": [0, 1, 1]
  })");
  check(m.at("n").is_int, "n is int");
  check(!m.at("x").is_int && m.at("x").r[0] == 2.5, "x real");
  check(m.at("v").is_int && m.at("v").i.size() == 3, "int array stays int");
  // Column-major: M = [[1,2],[3,4],[5,6]] stores as 1,3,5,2,4,6.
  check(m.at("M").dims.size() == 2 && m.at("M").dims[0] == 3 &&
            m.at("M").dims[1] == 2 && m.at("M").r.size() == 6 &&
            m.at("M").r[1] == 3.0 && m.at("M").r[3] == 2.0,
        "matrix column-major with dims");
  check(m.at("yi").is_int && m.at("yi").i[2] == 1, "yi int array");

  // A var_context keeps reals and ints under separate name lists, and its
  // flat values are already column-major -- the same layout the JSON reader
  // produces -- so the conversion copies rather than reorders.
  std::istringstream dumped(
      "n <- 4\n"
      "x <- 1.5\n"
      "rv <- c(0.5, -1.25, 3.75)\n"
      "iv <- c(7, -2, 0)\n"
      "R <- structure(c(1.5, 3.5, 5.5, 2.5, 4.5, 6.5), .Dim = c(3, 2))\n"
      "I <- structure(c(1, 4, 2, 5, 3, 6), .Dim = c(2, 3))\n"
      "RA <- structure(c(0.5, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5),"
      " .Dim = c(2, 2, 2))\n"
      "Z <- structure(c(), .Dim = c(2, 0, 3))\n");
  stan::io::dump ctx(dumped);
  DataMap c = DataMap::from_var_context(ctx);
  check(c.at("n").is_int && c.at("n").i == std::vector<int>{4} &&
            c.at("n").r == std::vector<double>{4.0} && c.at("n").dims.empty(),
        "var_context int scalar");
  check(!c.at("x").is_int && c.at("x").r == std::vector<double>{1.5} &&
            c.at("x").dims.empty(),
        "var_context real scalar");
  check(!c.at("rv").is_int && c.at("rv").dims == std::vector<int64_t>{3} &&
            c.at("rv").r == std::vector<double>({0.5, -1.25, 3.75}),
        "var_context real array");
  // Ints are usable as reals, the invariant set_int_array keeps.
  check(c.at("iv").is_int && c.at("iv").dims == std::vector<int64_t>{3} &&
            c.at("iv").i == std::vector<int>({7, -2, 0}) &&
            c.at("iv").r == std::vector<double>({7, -2, 0}),
        "var_context int array");
  check(c.at("R").dims == std::vector<int64_t>({3, 2}) &&
            c.at("R").r == std::vector<double>({1.5, 3.5, 5.5, 2.5, 4.5, 6.5}),
        "var_context matrix stays column-major");
  check(c.at("I").is_int && c.at("I").dims == std::vector<int64_t>({2, 3}) &&
            c.at("I").i == std::vector<int>({1, 4, 2, 5, 3, 6}),
        "var_context int matrix");
  check(!c.at("RA").is_int &&
            c.at("RA").dims == std::vector<int64_t>({2, 2, 2}) &&
            c.at("RA").r.size() == 8 && c.at("RA").r[7] == 7.5,
        "var_context 3-D real array");
  check(
      c.at("Z").dims == std::vector<int64_t>({2, 0, 3}) && c.at("Z").r.empty(),
      "var_context zero-sized array");
  check(!c.has("absent"), "var_context copies only what it holds");

  bool threw = false;
  try {
    (void)m.at("absent");
  } catch (const std::exception& e) {
    threw = std::string(e.what()).find("absent") != std::string::npos;
  }
  check(threw, "missing name reported");

  if (failures == 0) std::printf("test_data OK\n");
  return failures == 0 ? 0 : 1;
}
