// Public constrained-parameter names through the shipped C ABI.
//
// The compiler already owns CmdStan's naming rules through ParamView:
// scalars are bare, containers are indexed even at length one, and matrices
// carry row/column indices in column-major order. This test is intentionally
// at the ABI boundary so a second flattening policy in capi.cpp cannot drift.
#include <stanli/capi.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

std::string slurp(const char* path) {
  std::ifstream f(path);
  std::ostringstream out;
  out << f.rdbuf();
  return out.str();
}

void expect_eq(const char* what, const std::string& got,
               const std::string& want) {
  if (got == want) return;
  ++failures;
  std::printf("FAIL %s\n  got  %s\n  want %s\n", what, got.c_str(),
              want.c_str());
}

void expect_names(const char* fixture, const std::string& data,
                  const std::vector<std::string>& expected,
                  const std::vector<double>& q = {},
                  const std::vector<double>& expected_values = {}) {
  const std::string mir = slurp(fixture);
  char err[8192]{};
  stanli_model* model =
      stanli_model_new(mir.c_str(), data.c_str(), err, sizeof err);
  if (model == nullptr) {
    ++failures;
    std::printf("FAIL model construction for %s: %s\n", fixture, err);
    return;
  }

  const int64_t n = stanli_n_constrained(model);
  if (n != static_cast<int64_t>(expected.size())) {
    ++failures;
    std::printf("FAIL constrained count for %s: got %lld want %zu\n", fixture,
                static_cast<long long>(n), expected.size());
  }
  for (int64_t i = 0; i < n && i < static_cast<int64_t>(expected.size()); ++i) {
    const char* name = stanli_constrained_name(model, i);
    expect_eq((std::string(fixture) + " constrained name " + std::to_string(i))
                  .c_str(),
              name == nullptr ? "<null>" : name, expected[(size_t)i]);
  }
  if (!q.empty()) {
    std::vector<double> values((size_t)n);
    if (stanli_n_unconstrained(model) != static_cast<int64_t>(q.size()) ||
        stanli_constrain(model, q.data(), values.data()) != 0) {
      ++failures;
      std::printf("FAIL constrain for %s\n", fixture);
    } else if (values != expected_values) {
      ++failures;
      std::printf("FAIL constrained value order for %s\n", fixture);
      for (size_t i = 0; i < values.size(); ++i)
        if (values[i] != expected_values[i])
          std::printf("  [%zu] got %.17g want %.17g\n", i, values[i],
                      expected_values[i]);
    }
  }
  stanli_model_free(model);
}

}  // namespace

int main() {
  expect_names("tests/fixtures/wanames.tmir.sexp", "{}",
               {
                   "s",
                   "v.1",
                   "M.1.1",
                   "M.2.1",
                   "M.1.2",
                   "M.2.2",
                   "M.1.3",
                   "M.2.3",
               });

  // Arrays are outer-major, while each matrix element is column-major.
  // This is the shape a flat "name.1 ... name.N" policy cannot represent.
  expect_names(
      "tests/fixtures/amatwa.tmir.sexp", slurp("tests/fixtures/amatwa.json"),
      {"m.1.1.1", "m.2.1.1", "m.1.2.1", "m.2.2.1", "m.1.1.2", "m.2.1.2",
       "m.1.2.2", "m.2.2.2", "m.1.1.3", "m.2.1.3", "m.1.2.3", "m.2.2.3"},
      {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11},
      {0, 6, 1, 7, 2, 8, 3, 9, 4, 10, 5, 11});

  if (failures == 0) std::printf("test_capi OK\n");
  return failures == 0 ? 0 : 1;
}
