// Public constrained-parameter names through the shipped C ABI.
//
// The compiler already owns CmdStan's naming rules through ParamView:
// scalars are bare, containers are indexed even at length one, and matrices
// carry row/column indices in column-major order. This test is intentionally
// at the ABI boundary so a second flattening policy in capi.cpp cannot drift.
#include "categorical_check_mir.hpp"
#include "structured_array_oracles.hpp"

#include <stanli/capi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>
#include <utility>
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

void expect_invalid_data_rejected() {
  const std::string mir = slurp("tests/fixtures/newtrans.tmir.sexp");
  char err[8192]{};
  stanli_model* model =
      stanli_model_new(mir.c_str(), R"({"m":0.3,"s":-1})", err, sizeof err);
  if (model != nullptr) {
    ++failures;
    std::printf("FAIL C API accepted data below its declared bound\n");
    stanli_model_free(model);
  } else if (std::string(err).find("s") == std::string::npos) {
    ++failures;
    std::printf("FAIL C API data-bound error did not name s: %s\n", err);
  }
}

void expect_runtime_bound_rejected() {
  const std::string mir = slurp("tests/fixtures/data_and_tp_checks.tmir.sexp");
  char err[8192]{};
  stanli_model* model = stanli_model_new(
      mir.c_str(),
      R"({"d":0,"raw":0,"N":1,"M":1,"lo":[-10],"R":1,"C":1,"BR":1,"BC":1,"matrix_lo":[[-10]]})",
      err, sizeof err);
  if (model == nullptr) {
    ++failures;
    std::printf("FAIL C API bound-check model construction: %s\n", err);
    return;
  }

  double q = -1.0;
  double lp = 0.0;
  double grad = 0.0;
  if (stanli_grad(model, &q, &lp, &grad) != 1 ||
      lp != -std::numeric_limits<double>::infinity()) {
    ++failures;
    std::printf("FAIL C API gradient accepted transformed value below bound\n");
  }

  const int64_t ncol = stanli_wa_n_columns(model);
  std::vector<double> row((size_t)ncol);
  if (ncol <= 0 || stanli_wa_row(model, &q, row.data()) == 0) {
    ++failures;
    std::printf(
        "FAIL C API write_array accepted transformed value below bound\n");
  }
  stanli_model_free(model);
}

void expect_structured_checks() {
  const std::string mir = slurp("tests/fixtures/structured_checks.tmir.sexp");
  const std::string good = slurp("tests/fixtures/structured_checks.json");
  std::string bad = good;
  const std::string valid_leaf = "[0.2, 0.3, 0.5]";
  const size_t at = bad.find(valid_leaf);
  if (at == std::string::npos) {
    ++failures;
    std::printf("FAIL C API structured fixture replacement\n");
    return;
  }
  bad.replace(at, valid_leaf.size(), "[0.2, 0.3, 0.6]");

  char err[8192]{};
  stanli_model* model =
      stanli_model_new(mir.c_str(), bad.c_str(), err, sizeof err);
  if (model != nullptr) {
    ++failures;
    std::printf("FAIL C API accepted invalid simplex data\n");
    stanli_model_free(model);
  } else if (std::string(err).find("d_simplex") == std::string::npos) {
    ++failures;
    std::printf("FAIL C API invalid simplex error: %s\n", err);
  }

  model = stanli_model_new(mir.c_str(), good.c_str(), err, sizeof err);
  if (model == nullptr) {
    ++failures;
    std::printf("FAIL C API structured-check construction: %s\n", err);
    return;
  }

  double q[2] = {0.25, 0.0};
  double lp = 0.0;
  double grad[2]{};
  if (stanli_grad(model, q, &lp, grad) != 1 ||
      lp != -std::numeric_limits<double>::infinity()) {
    ++failures;
    std::printf("FAIL C API accepted invalid transformed simplex\n");
  }

  q[0] = 0.0;
  q[1] = 1.0;
  std::vector<double> row((size_t)stanli_wa_n_columns(model));
  if (row.empty() || stanli_wa_row(model, q, row.data()) == 0) {
    ++failures;
    std::printf("FAIL C API accepted invalid generated sum-to-zero vector\n");
  }
  stanli_model_free(model);
}

void expect_categorical_check() {
  const std::string mir = categorical_check_mir("categorical_lpmf", true, 1, 3);
  char err[8192]{};
  stanli_model* model = stanli_model_new(
      mir.c_str(), R"({"outcome":4,"arg":[0.2,0.3,0.5]})", err, sizeof err);
  if (model == nullptr) {
    ++failures;
    std::printf("FAIL C API categorical-check construction: %s\n", err);
    return;
  }
  double q = 0.0;
  double lp = 0.0;
  double grad = 0.0;
  if (stanli_grad(model, &q, &lp, &grad) != 1 ||
      lp != -std::numeric_limits<double>::infinity()) {
    ++failures;
    std::printf("FAIL C API accepted invalid categorical outcome\n");
  }
  stanli_model_free(model);
}

void expect_categorical_interpreted_write_array() {
  const std::string mir = categorical_write_array_mir(
      slurp("tests/fixtures/cat.tmir.sexp"), "categorical_lpmf", false, false,
      false, true);
  char err[8192]{};
  stanli_model* model = stanli_model_new(
      mir.c_str(), R"({"K":3,"y":2,"ys":[3,1,3]})", err, sizeof err);
  if (model == nullptr) {
    ++failures;
    std::printf("FAIL C API interpreted categorical construction: %s\n", err);
    return;
  }
  const int64_t n = stanli_wa_n_columns(model);
  std::vector<double> row((size_t)n);
  const double q[2] = {0.0, 0.0};
  stanli_wa_seed(model, 1234);
  if (stanli_wa_row(model, q, row.data()) != 0) {
    ++failures;
    std::printf("FAIL C API interpreted categorical row\n");
  } else {
    bool found = false;
    for (int64_t i = 0; i < n; ++i) {
      const char* name = stanli_wa_column_name(model, i);
      if (name != nullptr && std::string(name) == "categorical_value") {
        found = true;
        expect_near("C API interpreted categorical value", row[(size_t)i],
                    -std::log(3.0));
      }
    }
    if (!found) {
      ++failures;
      std::printf("FAIL C API interpreted categorical column\n");
    }
  }
  stanli_model_free(model);
}

void expect_necessity_effects_refused() {
  const std::string mir = slurp("tests/fixtures/necessity_effects.tmir.sexp");
  for (int mode = 1; mode <= 2; ++mode) {
    const std::string effect = mode == 1 ? "FnPrint" : "FnReject";
    const std::string data = "{\"mode\":" + std::to_string(mode) + "}";
    char err[8192]{};
    stanli_model* model =
        stanli_model_new(mir.c_str(), data.c_str(), err, sizeof err);
    if (model != nullptr) {
      ++failures;
      std::printf("FAIL C API accepted necessity island containing %s\n",
                  effect.c_str());
      stanli_model_free(model);
      continue;
    }
    const std::string msg = err;
    if (msg.find("parameter-dependent region") == std::string::npos ||
        msg.find(effect) == std::string::npos) {
      ++failures;
      std::printf("FAIL C API necessity %s error: %s\n", effect.c_str(), err);
    }
  }
}

// Pathfinder across the ABI: draws, the per-draw log densities, the
// summary block, and the per-iterate callback that lets a caller animate
// the climb. The statistics are test_pathfinder's job; what this pins is
// that the shipped boundary hands all of it back.
void expect_pathfinder() {
  const std::string mir = slurp("tests/fixtures/es.tmir.sexp");
  const std::string data = slurp("tests/fixtures/eight_schools.json");
  char err[8192]{};
  stanli_model* model =
      stanli_model_new(mir.c_str(), data.c_str(), err, sizeof err);
  if (model == nullptr) {
    ++failures;
    std::printf("FAIL C API pathfinder construction: %s\n", err);
    return;
  }
  const int64_t n = stanli_n_unconstrained(model);
  const int num_draws = 200;
  std::vector<double> draws((size_t)(num_draws * n));
  std::vector<double> lp(num_draws), lp_approx(num_draws);
  double summary[STANLI_N_PATHFINDER_SUMMARY]{};
  std::vector<std::pair<int32_t, double>> path;
  const auto on_iter = [](int32_t iter, double value, void* user) {
    static_cast<std::vector<std::pair<int32_t, double>>*>(user)->emplace_back(
        iter, value);
  };
  const int rc = stanli_run_pathfinder(model, 4242, 1, num_draws, draws.data(),
                                       lp.data(), lp_approx.data(), summary,
                                       on_iter, &path, err, sizeof err);
  if (rc != 0) {
    ++failures;
    std::printf("FAIL C API pathfinder: %s\n", err);
    stanli_model_free(model);
    return;
  }
  bool all_finite = true;
  for (double v : draws) all_finite &= std::isfinite(v);
  for (double v : lp) all_finite &= std::isfinite(v);
  for (double v : lp_approx) all_finite &= std::isfinite(v);
  if (!all_finite) {
    ++failures;
    std::printf("FAIL C API pathfinder produced nonfinite output\n");
  }
  if (path.size() < 2) {
    ++failures;
    std::printf("FAIL C API pathfinder path: %zu iterates\n", path.size());
  } else if (path[0].first != 0) {
    ++failures;
    std::printf("FAIL C API pathfinder path starts at iterate %d\n",
                path[0].first);
  }
  const double khat = summary[STANLI_PATHFINDER_KHAT];
  const double selected = summary[STANLI_PATHFINDER_SELECTED_ITER];
  if (!std::isfinite(khat) || selected < 0 || selected >= (double)path.size() ||
      !std::isfinite(summary[STANLI_PATHFINDER_SELECTED_ELBO]) ||
      !(summary[STANLI_PATHFINDER_ELAPSED_MS] >= 0)) {
    ++failures;
    std::printf("FAIL C API pathfinder summary: khat %g selected %g\n", khat,
                selected);
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
  expect_invalid_data_rejected();
  expect_runtime_bound_rejected();
  expect_structured_checks();
  expect_categorical_check();
  expect_categorical_interpreted_write_array();
  expect_necessity_effects_refused();
  expect_pathfinder();

  if (failures == 0) std::printf("test_capi OK\n");
  return failures == 0 ? 0 : 1;
}
