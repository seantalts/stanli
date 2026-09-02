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
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
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

void expect_true(const std::string& what, bool ok) {
  if (ok) return;
  ++failures;
  std::printf("FAIL %s\n", what.c_str());
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
                  const std::vector<double>& expected_values = {},
                  int64_t expected_wa_gq_start = -1) {
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
  if (expected_wa_gq_start >= 0 &&
      stanli_wa_n_generated_start(model) != expected_wa_gq_start) {
    ++failures;
    std::printf("FAIL generated-quantity start for %s: got %lld want %lld\n",
                fixture,
                static_cast<long long>(stanli_wa_n_generated_start(model)),
                static_cast<long long>(expected_wa_gq_start));
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

// stanli_unconstrain_inits, checked by round trip against the model's own
// forward direction: constrain a free point through stanli_wa_row, feed the
// constrained values back as JSON, and require the original free point.
void expect_unconstrain_inits_round_trip() {
  const std::string mir = slurp("tests/fixtures/initrt.tmir.sexp");
  const std::string data = slurp("tests/fixtures/initrt.json");
  char err[8192]{};
  stanli_model* model =
      stanli_model_new(mir.c_str(), data.c_str(), err, sizeof err);
  if (model == nullptr) {
    ++failures;
    std::printf("FAIL initrt construction: %s\n", err);
    return;
  }

  const int64_t n = stanli_n_unconstrained(model);
  std::vector<double> q((size_t)n, 0.0);
  for (int64_t i = 0; i < n; ++i)
    q[(size_t)i] = 0.1 + 0.05 * (double)(i % 7) - 0.15 * (double)(i % 3);

  // The constrained parameters, in the CSV order the JSON document mirrors.
  const int64_t n_cols = stanli_wa_n_columns(model);
  std::vector<double> row((size_t)n_cols, 0.0);
  if (stanli_wa_row(model, q.data(), row.data()) != 0) {
    ++failures;
    std::printf("FAIL initrt write_array\n");
    stanli_model_free(model);
    return;
  }

  // Rebuild the document from the CSV names, which carry each parameter's
  // shape in their indices.
  std::vector<std::string> flat;
  for (int64_t i = 0; i < n_cols; ++i)
    flat.emplace_back(stanli_wa_column_name(model, i));
  std::map<std::string, std::vector<double>> by_name;
  std::vector<std::string> order;
  for (size_t i = 0; i < flat.size() && i < row.size(); ++i) {
    const std::string base = flat[i].substr(0, flat[i].find('.'));
    if (by_name.find(base) == by_name.end()) order.push_back(base);
    by_name[base].push_back(row[i]);
  }
  std::string json = "{";
  for (size_t i = 0; i < order.size(); ++i) {
    const std::vector<double>& v = by_name[order[i]];
    char buf[32];
    json += (i ? ", " : "") + ("\"" + order[i] + "\": ");
    if (v.size() == 1) {
      std::snprintf(buf, sizeof buf, "%.17g", v[0]);
      json += buf;
    } else {
      json += "[";
      for (size_t k = 0; k < v.size(); ++k) {
        std::snprintf(buf, sizeof buf, "%.17g", v[k]);
        json += (k ? ", " : "") + std::string(buf);
      }
      json += "]";
    }
  }
  json += "}";

  std::vector<double> back((size_t)n, 0.0);
  if (stanli_unconstrain_inits(model, json.c_str(), back.data(), err,
                               sizeof err) != 0) {
    ++failures;
    std::printf("FAIL stanli_unconstrain_inits: %s\n", err);
    stanli_model_free(model);
    return;
  }
  for (int64_t i = 0; i < n; ++i) {
    if (!(std::abs(back[(size_t)i] - q[(size_t)i]) < 1e-9)) {
      ++failures;
      std::printf(
          "FAIL stanli_unconstrain_inits round trip at %lld: %.17g "
          "want %.17g\n",
          (long long)i, back[(size_t)i], q[(size_t)i]);
      break;
    }
  }

  // A document missing a parameter is refused by name.
  if (stanli_unconstrain_inits(model, "{\"mu\": 0}", back.data(), err,
                               sizeof err) == 0) {
    ++failures;
    std::printf(
        "FAIL stanli_unconstrain_inits accepted an incomplete "
        "document\n");
  } else if (std::string(err).find("sigma") == std::string::npos) {
    ++failures;
    std::printf(
        "FAIL incomplete document did not name a missing parameter: "
        "%s\n",
        err);
  }

  // Unknown keys are not silently discarded: the public contract requires
  // the error to name the value the model does not declare.
  std::string unknown = json;
  unknown.insert(unknown.size() - 1, ", \"not_a_parameter\": 1");
  if (stanli_unconstrain_inits(model, unknown.c_str(), back.data(), err,
                               sizeof err) == 0) {
    ++failures;
    std::printf("FAIL stanli_unconstrain_inits accepted an unknown name\n");
  } else if (std::string(err).find("not_a_parameter") == std::string::npos) {
    ++failures;
    std::printf("FAIL unknown init error did not name its key: %s\n", err);
  }
  stanli_model_free(model);
}

void expect_parameterless_unconstrain() {
  const std::string mir = slurp("tests/fixtures/view_gq_data_matrix.tmir.sexp");
  char err[8192]{};
  stanli_model* model = stanli_model_new(
      mir.c_str(), R"({"M":[[1,2,3],[4,5,6]]})", err, sizeof err);
  if (model == nullptr) {
    ++failures;
    std::printf("FAIL parameterless model construction: %s\n", err);
    return;
  }
  double unused = 0.0;
  if (stanli_n_unconstrained(model) != 0 ||
      stanli_unconstrain_inits(model, "{}", &unused, err, sizeof err) != 0) {
    ++failures;
    std::printf("FAIL parameterless unconstrain: %s\n", err);
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

void expect_compiled_scalar_rng() {
  const std::string mir = slurp("tests/fixtures/gq_scalar_rng.tmir.sexp");
  char err[8192]{};
  stanli_model* model = stanli_model_new(mir.c_str(), "{}", err, sizeof err);
  if (model == nullptr) {
    ++failures;
    std::printf("FAIL C API scalar RNG construction: %s\n", err);
    return;
  }
  const int64_t n = stanli_wa_n_columns(model);
  if (n != 9) {
    ++failures;
    std::printf("FAIL C API scalar RNG columns: got %lld want 9\n",
                static_cast<long long>(n));
    stanli_model_free(model);
    return;
  }
  const double q[1] = {0.25};
  std::vector<double> first((size_t)n), second((size_t)n), reseeded((size_t)n),
      other_chain((size_t)n);
  stanli_wa_seed(model, 77);
  const int r1 = stanli_wa_row(model, q, first.data());
  const int r2 = stanli_wa_row(model, q, second.data());
  stanli_wa_seed(model, 77);
  const int r3 = stanli_wa_row(model, q, reseeded.data());
  stanli_wa_seed_chain(model, 77, 9);
  const int r4 = stanli_wa_row(model, q, other_chain.data());
  if (r1 != 0 || r2 != 0 || r3 != 0 || r4 != 0 || first != reseeded ||
      first == second || first == other_chain) {
    ++failures;
    std::printf("FAIL C API scalar RNG stream/reseed/chain ownership\n");
  }
  stanli_model_free(model);
}

void expect_necessity_effects() {
  const std::string mir = slurp("tests/fixtures/necessity_effects.tmir.sexp");
  char err[8192]{};
  stanli_model* model =
      stanli_model_new(mir.c_str(), "{\"mode\":1}", err, sizeof err);
  if (model == nullptr) {
    ++failures;
    std::printf("FAIL C API refused necessity print: %s\n", err);
  } else {
    const double q = 0.1;
    double lp = 0, grad = 0;
    if (stanli_grad(model, &q, &lp, &grad) != 0 || lp != 0.1 || grad != 1.0) {
      ++failures;
      std::printf(
          "FAIL C API necessity print evaluation: lp %.17g grad %.17g\n", lp,
          grad);
    }
    stanli_model_free(model);
  }

  std::fill(std::begin(err), std::end(err), '\0');
  model = stanli_model_new(mir.c_str(), "{\"mode\":2}", err, sizeof err);
  if (model == nullptr) {
    ++failures;
    std::printf("FAIL C API refused dynamic necessity reject: %s\n", err);
  } else {
    double lp = 0.0, grad = 0.0;
    const double accepted = 0.1;
    if (stanli_grad(model, &accepted, &lp, &grad) != 0 || lp != 0.1 ||
        grad != 1.0) {
      ++failures;
      std::printf("FAIL C API untaken dynamic reject\n");
    }
    const double rejected = -0.1;
    if (stanli_grad(model, &rejected, &lp, &grad) != 1 || !std::isinf(lp) ||
        lp > 0.0) {
      ++failures;
      std::printf("FAIL C API taken dynamic reject: lp %.17g\n", lp);
    }
    stanli_model_free(model);
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

  // Pathfinder-as-initialization is a distinct additive boundary: one
  // chain-major unconstrained row per requested chain, reproducible from the
  // sampling seed and not a change to stanli_sample_opts.
  std::vector<double> inits((size_t)(3 * n));
  std::vector<double> repeated(inits.size());
  std::vector<double> changed(inits.size());
  path.clear();
  const int init_rc =
      stanli_pathfinder_inits(model, 1729, 1, 3, 1000, 25, 5, 2.0, inits.data(),
                              on_iter, &path, err, sizeof err);
  const int repeated_rc = stanli_pathfinder_inits(
      model, 1729, 1, 3, 1000, 25, 5, 2.0, repeated.data(), nullptr, nullptr,
      err, sizeof err);
  const int changed_rc = stanli_pathfinder_inits(model, 1730, 1, 3, 1000, 25, 5,
                                                 2.0, changed.data(), nullptr,
                                                 nullptr, err, sizeof err);
  expect_true("C API Pathfinder inits succeed",
              init_rc == 0 && repeated_rc == 0 && changed_rc == 0);
  expect_true("C API Pathfinder inits expose progress", !path.empty());
  expect_true("C API Pathfinder inits reproduce", inits == repeated);
  expect_true("C API Pathfinder init seed changes output", inits != changed);
  expect_true("C API Pathfinder gives chains distinct starts",
              !std::equal(inits.begin(), inits.begin() + n, inits.begin() + n));

  err[0] = '\0';
  const int invalid = stanli_pathfinder_inits(
      model, 1, 1, 3, 1000, 25, 5, std::numeric_limits<double>::quiet_NaN(),
      inits.data(), nullptr, nullptr, err, sizeof err);
  expect_true("C API Pathfinder init rejects invalid settings",
              invalid != 0 &&
                  std::string(err).find("init_radius") != std::string::npos);
  stanli_model_free(model);
}

struct ProgressCapture {
  std::thread::id caller;
  bool caller_thread = true;
  std::vector<std::array<int64_t, 4>> events;
};

void capture_progress(int32_t chain_id, int64_t iteration, int64_t total,
                      int32_t warmup, void* user) {
  auto* capture = static_cast<ProgressCapture*>(user);
  capture->caller_thread =
      capture->caller_thread && std::this_thread::get_id() == capture->caller;
  capture->events.push_back({chain_id, iteration, total, warmup});
}

// The additive progress entry point must be the old sampler plus observation:
// same bytes, caller-thread callbacks, exact refresh schedule and reports.
void expect_sampling_progress() {
  const std::string mir = slurp("tests/fixtures/gqconst.tmir.sexp");
  const char* data = R"({"rectangular":[[1,4],[2,5],[3,6]]})";
  char err[8192]{};
  stanli_model* quiet = stanli_model_new(mir.c_str(), data, err, sizeof err);
  stanli_model* observed = stanli_model_new(mir.c_str(), data, err, sizeof err);
  if (quiet == nullptr || observed == nullptr) {
    ++failures;
    std::printf("FAIL C API progress model construction: %s\n", err);
    if (quiet) stanli_model_free(quiet);
    if (observed) stanli_model_free(observed);
    return;
  }

  stanli_sample_opts opts;
  stanli_sample_opts_init(&opts);
  opts.seed = 230;
  opts.chains = 2;
  opts.warmup = 4;
  opts.samples = 3;
  opts.init_radius = 0.0;
  opts.num_threads = 2;
  const int64_t n = stanli_n_unconstrained(quiet);
  const int64_t rows = stanli_n_stored_draws(&opts);
  std::vector<double> quiet_draws((size_t)(opts.chains * rows * n));
  std::vector<double> quiet_stats(
      (size_t)(opts.chains * rows * STANLI_N_SAMPLER_COLS));
  std::vector<double> observed_draws(quiet_draws.size());
  std::vector<double> observed_stats(quiet_stats.size());

  const int quiet_failed = stanli_sample_multi(
      quiet, &opts, quiet_draws.data(), quiet_stats.data(), err, sizeof err);
  ProgressCapture capture{std::this_thread::get_id()};
  std::vector<stanli_sample_report> reports((size_t)opts.chains);
  const int observed_failed = stanli_sample_multi_progress(
      observed, &opts, 2, observed_draws.data(), observed_stats.data(),
      capture_progress, &capture, reports.data(), err, sizeof err);
  expect_true("C API progress runs succeed",
              quiet_failed == 0 && observed_failed == 0);
  expect_true("C API progress leaves draws bitwise unchanged",
              quiet_draws == observed_draws);
  expect_true("C API progress leaves stats bitwise unchanged",
              quiet_stats == observed_stats);
  expect_true("C API progress callback uses the caller thread",
              capture.caller_thread);

  const std::vector<int64_t> wanted{1, 2, 4, 5, 6, 7};
  for (int chain = 1; chain <= opts.chains; ++chain) {
    std::vector<int64_t> got;
    for (const auto& event : capture.events) {
      if (event[0] != chain) continue;
      got.push_back(event[1]);
      expect_true("C API progress total is stable", event[2] == 7);
      expect_true("C API progress phase matches the boundary",
                  (event[1] <= opts.warmup) == (event[3] != 0));
    }
    expect_true(
        "C API progress refresh sequence for chain " + std::to_string(chain),
        got == wanted);
    const auto& report = reports[(size_t)(chain - 1)];
    expect_true("C API reports nonnegative timings",
                report.warmup_seconds >= 0.0 && report.sampling_seconds >= 0.0);
    expect_true("C API reports bounded problem counts",
                report.n_divergent >= 0 && report.n_divergent <= opts.samples &&
                    report.n_max_treedepth >= 0 &&
                    report.n_max_treedepth <= opts.samples);
  }

  ProgressCapture rejected{std::this_thread::get_id()};
  err[0] = '\0';
  const int negative = stanli_sample_multi_progress(
      observed, &opts, -1, observed_draws.data(), observed_stats.data(),
      capture_progress, &rejected, reports.data(), err, sizeof err);
  expect_true("C API rejects negative refresh",
              negative != 0 &&
                  std::string(err).find("refresh") != std::string::npos &&
                  rejected.events.empty());

  stanli_model_free(quiet);
  stanli_model_free(observed);
}

// Browser streaming must expose the same diagnostics as the existing
// multi-chain API without changing any draw or callback-visible row.
void expect_streaming_stats() {
  const std::string mir = slurp("tests/fixtures/es.tmir.sexp");
  const std::string data = slurp("tests/fixtures/es.json");
  char err[8192]{};
  auto* model = stanli_model_new(mir.c_str(), data.c_str(), err, sizeof err);
  expect_true("stream stats model", model != nullptr);
  if (!model) return;
  stanli_sample_opts opts;
  stanli_sample_opts_init(&opts);
  opts.seed = 292;
  opts.chains = 1;
  opts.warmup = 100;
  opts.samples = 50;
  const int64_t n = stanli_n_unconstrained(model);
  std::vector<double> reference((size_t)(opts.samples * n));
  std::vector<double> stats((size_t)(opts.samples * STANLI_N_SAMPLER_COLS));
  expect_true("reference sampling",
              stanli_sample_multi(model, &opts, reference.data(), stats.data(),
                                  err, sizeof err) == 0);
  std::vector<double> streamed(reference.size());
  std::vector<double> streamed_stats(stats.size());
  struct Capture {
    int warmup = 0, samples = 0;
    int64_t width;
    const double* draws;
    std::vector<double> observed;
  } capture{0, 0, n, streamed.data(), {}};
  const auto callback = [](int32_t i, int32_t warmup, void* user) {
    auto& c = *static_cast<Capture*>(user);
    if (warmup) {
      ++c.warmup;
    } else {
      ++c.samples;
      c.observed.insert(c.observed.end(), c.draws + i * c.width,
                        c.draws + (i + 1) * c.width);
    }
  };
  expect_true("streaming stats sampling",
              stanli_sample_stream_stats(
                  model, opts.seed, opts.warmup, opts.samples, opts.delta,
                  streamed.data(), streamed_stats.data(), callback, &capture,
                  err, sizeof err) == 0);
  expect_true("streamed draws match multi-chain bytes", streamed == reference);
  expect_true("streamed stats match multi-chain bytes",
              streamed_stats == stats);
  expect_true("callback sees completed rows", capture.observed == reference);
  expect_true("warmup is excluded from stats",
              capture.warmup == opts.warmup && capture.samples == opts.samples);
  expect_true("old sampler succeeds",
              stanli_sample(model, opts.seed, opts.warmup, opts.samples,
                            opts.delta, streamed.data(), err, sizeof err) == 0);
  expect_true("old sampler retains identical draws", streamed == reference);

  // The browser's additive explicit-init stream is the one-chain multi API,
  // byte for byte. This is the handoff Pathfinder initialization uses.
  std::vector<double> init((size_t)n, 0.0);
  opts.inits = init.data();
  expect_true("explicit-init multi sampling",
              stanli_sample_multi(model, &opts, reference.data(), stats.data(),
                                  err, sizeof err) == 0);
  expect_true("explicit-init streaming sampling",
              stanli_sample_stream_stats_init(
                  model, opts.seed, opts.warmup, opts.samples, opts.delta,
                  init.data(), streamed.data(), streamed_stats.data(), nullptr,
                  nullptr, err, sizeof err) == 0);
  expect_true("explicit-init stream matches multi draws",
              streamed == reference);
  expect_true("explicit-init stream matches multi stats",
              streamed_stats == stats);
  stanli_model_free(model);
}

}  // namespace

int main() {
  expect_unconstrain_inits_round_trip();
  expect_parameterless_unconstrain();

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
               },
               {}, {}, 8);

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
  expect_compiled_scalar_rng();
  expect_necessity_effects();
  expect_pathfinder();
  expect_sampling_progress();
  expect_streaming_stats();

  if (failures == 0) std::printf("test_capi OK\n");
  return failures == 0 ? 0 : 1;
}
