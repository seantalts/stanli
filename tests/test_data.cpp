// JSON data loading (CmdStan conventions): scalars, arrays, nested arrays,
// and the same map built from a Stan var_context instead.
#include <stanli/data.hpp>

#include <stan/io/dump.hpp>

#include <cmath>
#include <cstdio>
#include <limits>
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

  // Match CmdStan's streaming reader on malformed arrays and value types.
  for (const char* bad :
       {"{\"x\":[[1,2],[3]]}", "{\"x\":[1,[2]]}", "{\"x\":true}",
        "{\"x\":null}", "{\"x\":1} garbage"}) {
    bool rejected = false;
    try {
      DataMap::from_json(bad);
    } catch (const std::exception&) {
      rejected = true;
    }
    check(rejected, "malformed JSON data rejected");
  }

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

  // Empty leaves retain their zero-width dimension.  Stan models commonly
  // receive array[N] vector[0] when an optional predictor count is zero.
  DataMap z = DataMap::from_json(R"({
    "Z2": [[], [], []],
    "Z3": [[[], []], [[], []]]
  })");
  check(z.at("Z2").dims == std::vector<int64_t>({3, 0}) &&
            z.at("Z2").r.empty() && z.at("Z2").i.empty(),
        "JSON zero-width matrix shape");
  check(z.at("Z3").dims == std::vector<int64_t>({2, 2, 0}) &&
            z.at("Z3").r.empty() && z.at("Z3").i.empty(),
        "JSON nested zero-width shape");

  // Non-finite reals, in both spellings CmdStan's JSON reader accepts: the
  // bare tokens rapidjson's kParseNanAndInfFlag allows, and the quoted forms
  // json_data_handler::string() maps.
  DataMap nf = DataMap::from_json(R"({
    "a": Infinity, "b": -Infinity, "c": NaN, "d": Inf, "e": -Inf,
    "qa": "Infinity", "qb": "-Infinity", "qc": "NaN", "qd": "Inf",
    "qe": "-Inf"
  })");
  const double inf = std::numeric_limits<double>::infinity();
  for (const std::string& p : {std::string(""), std::string("q")}) {
    check(!nf.at(p + "a").is_int && nf.at(p + "a").r[0] == inf, p + "a +Inf");
    check(nf.at(p + "b").r[0] == -inf, p + "b -Inf");
    check(std::isnan(nf.at(p + "c").r[0]), p + "c NaN");
    check(nf.at(p + "d").r[0] == inf, p + "d Inf");
    check(nf.at(p + "e").r[0] == -inf, p + "e -Inf");
  }

  DataMap nfa = DataMap::from_json(R"({
    "v": [1, Infinity, 3],
    "M": [[1.0, Infinity], [NaN, 4.0]],
    "A": [[[1, -Infinity]]],
    "Info": 7, "NaNny": [1, 2]
  })");
  check(!nfa.at("v").is_int && nfa.at("v").r[1] == inf && nfa.at("v").r[2] == 3,
        "Infinity demotes an otherwise-int array to real");
  check(nfa.at("v").i.empty(), "no int mirror for a non-finite array");
  check(nfa.at("M").dims == std::vector<int64_t>({2, 2}) &&
            nfa.at("M").r[2] == inf && std::isnan(nfa.at("M").r[1]),
        "non-finite matrix stays column-major");
  check(nfa.at("A").dims == std::vector<int64_t>({1, 1, 2}) &&
            nfa.at("A").r[1] == -inf,
        "non-finite N-D array");
  check(nfa.at("Info").is_int && nfa.at("Info").i[0] == 7 &&
            nfa.at("NaNny").is_int,
        "token spellings inside keys are not values");

  bool bad_string = false;
  try {
    (void)DataMap::from_json(R"({"s": "hello"})");
  } catch (const std::exception& e) {
    bad_string = std::string(e.what()).find("s") != std::string::npos;
  }
  check(bad_string, "a non-token string is still rejected");

  bool bad_null = false;
  try {
    (void)DataMap::from_json(R"({"s": null})");
  } catch (const std::exception&) {
    bad_null = true;
  }
  check(bad_null, "null is still rejected");

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
