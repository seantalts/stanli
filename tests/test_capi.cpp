// Public constrained-parameter names through the shipped C ABI.
//
// The compiler already owns CmdStan's naming rules through ParamView:
// scalars are bare, containers are indexed even at length one, and matrices
// carry row/column indices in column-major order. This test is intentionally
// at the ABI boundary so a second flattening policy in capi.cpp cannot drift.
#include "structured_array_oracles.hpp"

#include <stanli/capi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {
namespace oracle = structured_array_oracle;

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

void expect_near(const std::string& what, double got, double want) {
  const double tol = 2e-12 * std::max(1.0, std::fabs(want));
  if (std::fabs(got - want) <= tol) return;
  ++failures;
  std::printf("FAIL %s: got %.17g want %.17g\n", what.c_str(), got, want);
}

void expect_values(const std::string& what, const std::vector<double>& got,
                   const std::vector<double>& want) {
  if (got.size() != want.size()) {
    ++failures;
    std::printf("FAIL %s count: got %zu want %zu\n", what.c_str(), got.size(),
                want.size());
    return;
  }
  for (size_t i = 0; i < got.size(); ++i)
    expect_near(what + "[" + std::to_string(i) + "]", got[i], want[i]);
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

void expect_structured_arrays(int B) {
  const std::string fixture = "tests/fixtures/structured_arrays.tmir.sexp";
  const std::string tag = "structured B=" + std::to_string(B);
  const std::string mir = slurp(fixture.c_str());
  const std::string data = "{\"B\":" + std::to_string(B) + "}";
  char err[8192]{};
  stanli_model* model =
      stanli_model_new(mir.c_str(), data.c_str(), err, sizeof err);
  if (model == nullptr) {
    ++failures;
    std::printf("FAIL %s construction: %s\n", tag.c_str(), err);
    return;
  }

  const std::vector<double> q = oracle::q(B);
  if (stanli_n_unconstrained(model) != oracle::n_unc(B)) {
    ++failures;
    std::printf("FAIL %s unconstrained count\n", tag.c_str());
  }
  double lp = 0;
  std::vector<double> grad(q.size());
  if (stanli_grad(model, q.data(), &lp, grad.data()) != 0) {
    ++failures;
    std::printf("FAIL %s gradient call\n", tag.c_str());
  } else {
    expect_near(tag + " lp", lp, oracle::lp(B));
    expect_values(tag + " gradient", grad, oracle::grad(B));
  }

  const std::vector<std::string> con_names = oracle::constrained_names(B);
  if (stanli_n_constrained(model) != oracle::n_con(B)) {
    ++failures;
    std::printf("FAIL %s constrained count\n", tag.c_str());
  }
  for (size_t i = 0; i < con_names.size(); ++i) {
    const char* got = stanli_constrained_name(model, (int64_t)i);
    expect_eq((tag + " constrained name " + std::to_string(i)).c_str(),
              got == nullptr ? "<null>" : got, con_names[i]);
  }
  std::vector<double> constrained(con_names.size());
  if (stanli_constrain(model, q.data(), constrained.data()) != 0) {
    ++failures;
    std::printf("FAIL %s constrain call\n", tag.c_str());
  } else {
    expect_values(tag + " constrain", constrained, oracle::constrained(B));
  }

  const std::vector<std::string> write_names = oracle::write_names(B);
  if (stanli_wa_n_columns(model) != (int64_t)write_names.size()) {
    ++failures;
    std::printf("FAIL %s write-array count\n", tag.c_str());
  }
  for (size_t i = 0; i < write_names.size(); ++i) {
    const char* got = stanli_wa_column_name(model, (int64_t)i);
    expect_eq((tag + " write-array name " + std::to_string(i)).c_str(),
              got == nullptr ? "<null>" : got, write_names[i]);
  }
  std::vector<double> row(write_names.size());
  stanli_wa_seed(model, 1234);
  if (stanli_wa_row(model, q.data(), row.data()) != 0) {
    ++failures;
    std::printf("FAIL %s write-array row\n", tag.c_str());
  } else {
    expect_values(tag + " write-array", row, oracle::write_values(B));
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

  // Generated Stan reads nested scalar arrays outer-first from q, while
  // constrained output is first-index-fast. The two orders are deliberately
  // different for this non-square declaration.
  expect_names("tests/fixtures/viewa_scalar_column.tmir.sexp", "{}",
               {"a.1.1", "a.2.1", "a.1.2", "a.2.2", "a.1.3", "a.2.3"},
               {1, 2, 3, 4, 5, 6}, {1, 4, 2, 5, 3, 6});

  for (int B = 0; B <= 2; ++B) expect_structured_arrays(B);

  if (failures == 0) std::printf("test_capi OK\n");
  return failures == 0 ? 0 : 1;
}
