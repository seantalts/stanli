// Public constrained-parameter names through the shipped C ABI.
//
// The compiler already owns CmdStan's naming rules through ParamView:
// scalars are bare, containers are indexed even at length one, and matrices
// carry row/column indices in column-major order. This test is intentionally
// at the ABI boundary so a second flattening policy in capi.cpp cannot drift.
#include "../runtime/third_party/bridgestan.h"
#include "categorical_check_mir.hpp"
#include "env_helpers.hpp"

#include <stanli/capi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
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

void expect_names(const char* fixture, const std::string& data,
                  const std::vector<std::string>& expected,
                  const std::vector<double>& q = {},
                  const std::vector<double>& expected_values = {},
                  int64_t expected_wa_gq_start = -1) {
  const std::string mir = slurp(fixture);
  char err[8192]{};
  stanli_model* model = stanli_model_new_with_opts(mir.c_str(), data.c_str(),
                                                   nullptr, err, sizeof err);
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

// stanli_unconstrain_inits, checked by round trip against the model's own
// forward direction: constrain a free point through stanli_wa_row, feed the
// constrained values back as JSON, and require the original free point.
void expect_unconstrain_inits_round_trip() {
  const std::string mir = slurp("tests/fixtures/initrt.tmir.sexp");
  const std::string data = slurp("tests/fixtures/initrt.json");
  char err[8192]{};
  stanli_model* model = stanli_model_new_with_opts(mir.c_str(), data.c_str(),
                                                   nullptr, err, sizeof err);
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
  stanli_model* model = stanli_model_new_with_opts(
      mir.c_str(), R"({"M":[[1,2,3],[4,5,6]]})", nullptr, err, sizeof err);
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
  stanli_model* model = stanli_model_new_with_opts(
      mir.c_str(), R"({"m":0.3,"s":-1})", nullptr, err, sizeof err);
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
  stanli_model* model = stanli_model_new_with_opts(
      mir.c_str(),
      R"({"d":0,"raw":0,"N":1,"M":1,"lo":[-10],"R":1,"C":1,"BR":1,"BC":1,"matrix_lo":[[-10]]})",
      nullptr, err, sizeof err);
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

void expect_compiled_scalar_rng() {
  const std::string mir = slurp("tests/fixtures/gq_scalar_rng.tmir.sexp");
  char err[8192]{};
  stanli_model* model =
      stanli_model_new_with_opts(mir.c_str(), "{}", nullptr, err, sizeof err);
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

// Pathfinder across the ABI: draws, the per-draw log densities, the
// summary block, and the per-iterate callback that lets a caller animate
// the climb. The statistics are test_pathfinder's job; what this pins is
// that the shipped boundary hands all of it back.
void expect_pathfinder() {
  const std::string mir = slurp("tests/fixtures/es.tmir.sexp");
  const std::string data = slurp("tests/fixtures/eight_schools.json");
  char err[8192]{};
  stanli_model* model = stanli_model_new_with_opts(mir.c_str(), data.c_str(),
                                                   nullptr, err, sizeof err);
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
  std::vector<std::array<int64_t, 6>> events;
};

void capture_progress(int32_t chain_id, int64_t iteration, int64_t total,
                      int32_t warmup, int64_t n_divergent,
                      int64_t n_max_treedepth, void* user) {
  auto* capture = static_cast<ProgressCapture*>(user);
  capture->caller_thread =
      capture->caller_thread && std::this_thread::get_id() == capture->caller;
  capture->events.push_back(
      {chain_id, iteration, total, warmup, n_divergent, n_max_treedepth});
}

// Answers stop only after the chains have had time to finish: a sampler
// that starts them before asking has nothing left to interrupt.
int poll_stop_after_pause(void* user) {
  ++*static_cast<int*>(user);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  return 1;
}

int poll_keep_going(void* user) {
  ++*static_cast<int*>(user);
  return 0;
}

// The interruptible entry point is the progress sampler plus a poll: a stop
// answer ends the run cleanly with the flag set, and a run whose poll never
// stops it produces the same bytes as the plain sampler.
void expect_interruptible_sampling() {
  const std::string mir = slurp("tests/fixtures/gqconst.tmir.sexp");
  const char* data = R"({"rectangular":[[1,4],[2,5],[3,6]]})";
  char err[8192]{};
  stanli_model* model =
      stanli_model_new_with_opts(mir.c_str(), data, nullptr, err, sizeof err);
  expect_true("interruptible sampling model builds", model != nullptr);
  if (model == nullptr) return;
  stanli_sample_opts opts;
  stanli_sample_opts_init(&opts);
  opts.seed = 7;
  opts.chains = 2;
  opts.warmup = 40;
  opts.samples = 40;
  const int64_t n = stanli_n_unconstrained(model);
  const int64_t rows = stanli_n_stored_draws(&opts);
  const size_t n_draws = (size_t)(opts.chains * rows * n);
  const size_t n_stats = (size_t)(opts.chains * rows * STANLI_N_SAMPLER_COLS);
  for (int threads : {1, 2}) {
    opts.num_threads = threads;
    const std::string mode = threads == 1 ? " (sequential)" : " (threaded)";
    std::vector<double> draws(n_draws, -1.0);
    std::vector<double> stats(n_stats, -1.0);
    stanli_sample_report reports[2];
    int polls = 0;
    int interrupted = -1;
    err[0] = '\0';
    int rc = stanli_sample_multi_metric(model, &opts, nullptr, 0, draws.data(),
                                        stats.data(), nullptr, nullptr, nullptr,
                                        poll_stop_after_pause, &polls,
                                        &interrupted, reports, err, sizeof err);
    expect_true("a stop answer ends sampling cleanly" + mode,
                rc == 0 && interrupted == 1 && polls >= 1);
    expect_true("a stop before the first transition stores nothing" + mode,
                std::all_of(draws.begin(), draws.end(),
                            [](double x) { return x == -1.0; }) &&
                    std::all_of(stats.begin(), stats.end(),
                                [](double x) { return x == -1.0; }));

    std::vector<double> plain_draws(n_draws), plain_stats(n_stats);
    stanli_sample_report plain_reports[2];
    rc = stanli_sample_multi_metric(model, &opts, nullptr, 0,
                                    plain_draws.data(), plain_stats.data(),
                                    nullptr, nullptr, nullptr, nullptr, nullptr,
                                    nullptr, plain_reports, err, sizeof err);
    expect_true("plain sampling succeeds" + mode, rc == 0);
    polls = 0;
    interrupted = -1;
    rc = stanli_sample_multi_metric(model, &opts, nullptr, 0, draws.data(),
                                    stats.data(), nullptr, nullptr, nullptr,
                                    poll_keep_going, &polls, &interrupted,
                                    reports, err, sizeof err);
    expect_true(
        "a poll that never stops is observational" + mode,
        rc == 0 && interrupted == 0 && polls >= 1 && draws == plain_draws &&
            stats == plain_stats &&
            reports[0].n_divergent == plain_reports[0].n_divergent &&
            reports[1].n_max_treedepth == plain_reports[1].n_max_treedepth);
  }
  stanli_model_free(model);
}

// Full-output sampling consumes the live chain RNG. Only RNG-free outputs
// agree with a parameter-only run followed by standalone generated quantities.
void expect_in_worker_write_array_on(const char* label, stanli_model* model,
                                     bool random = false) {
  stanli_sample_opts opts;
  stanli_sample_opts_init(&opts);
  opts.seed = 7;
  opts.chains = 2;
  opts.warmup = 20;
  opts.samples = 20;
  const int64_t n = stanli_n_unconstrained(model);
  const int64_t rows = stanli_n_stored_draws(&opts);
  const int64_t wa = stanli_wa_n_columns(model);
  const int64_t width = wa > 0 ? wa : stanli_n_constrained(model);
  const size_t n_draws = (size_t)(opts.chains * rows * n);
  const size_t n_stats = (size_t)(opts.chains * rows * STANLI_N_SAMPLER_COLS);
  const size_t n_values = (size_t)(opts.chains * rows * width);
  char err[8192]{};
  std::vector<double> serial_draws, serial_stats, serial_values;
  for (int threads : {1, 2}) {
    opts.num_threads = threads;
    const std::string mode = std::string(" [") + label +
                             (threads == 1 ? " sequential]" : " threaded]");

    std::vector<double> want_draws(n_draws), want_stats(n_stats);
    stanli_sample_report want_reports[2];
    err[0] = '\0';
    int rc = stanli_sample_multi_metric(
        model, &opts, nullptr, 0, want_draws.data(), want_stats.data(), nullptr,
        nullptr, nullptr, nullptr, nullptr, nullptr, want_reports, err,
        sizeof err);
    expect_true("post-hoc reference run succeeds" + mode, rc == 0);

    std::vector<double> want_values(n_values);
    std::vector<double> row((size_t)width);
    bool reference_ok = true;
    for (int64_t c = 0; c < opts.chains; ++c) {
      if (wa > 0) stanli_wa_seed_chain(model, opts.seed, (uint32_t)(1 + c));
      for (int64_t i = 0; i < rows; ++i) {
        const double* q = want_draws.data() + (c * rows + i) * n;
        const int wrc = wa > 0 ? stanli_wa_row(model, q, row.data())
                               : stanli_constrain(model, q, row.data());
        if (wrc != 0) reference_ok = false;
        std::copy(row.begin(), row.end(),
                  want_values.begin() + (size_t)((c * rows + i) * width));
      }
    }
    expect_true("post-hoc write_array succeeds" + mode, reference_ok);

    std::vector<double> draws(n_draws, -1.0), stats(n_stats, -1.0);
    std::vector<double> values(n_values, -1.0);
    stanli_sample_report reports[2];
    int interrupted = -1;
    err[0] = '\0';
    rc = stanli_sample_multi_metric(model, &opts, nullptr, 0, draws.data(),
                                    stats.data(), values.data(), nullptr,
                                    nullptr, nullptr, nullptr, &interrupted,
                                    reports, err, sizeof err);
    expect_true("in-worker write_array sampling succeeds" + mode,
                rc == 0 && interrupted == 0);
    expect_true("live RNG changes only random-output trajectories" + mode,
                random ? draws != want_draws && stats != want_stats
                       : draws == want_draws && stats == want_stats);
    expect_true("only RNG-free output agrees with the post-hoc loop" + mode,
                (values != want_values) == random);
    if (threads == 1) {
      serial_draws = draws;
      serial_stats = stats;
      serial_values = values;
    } else {
      expect_true("serial and parallel full-output runs agree" + mode,
                  draws == serial_draws && stats == serial_stats &&
                      values == serial_values);
    }

    std::vector<double> null_draws(n_draws, -1.0), null_stats(n_stats, -1.0);
    err[0] = '\0';
    rc = stanli_sample_multi_metric(
        model, &opts, nullptr, 0, null_draws.data(), null_stats.data(), nullptr,
        nullptr, nullptr, nullptr, nullptr, nullptr, reports, err, sizeof err);
    expect_true(
        "a null values buffer skips generated quantities" + mode,
        rc == 0 && null_draws == want_draws && null_stats == want_stats);
  }
}

void expect_in_worker_write_array() {
  char err[8192]{};
  stanli_model* graph = stanli_model_new_with_opts(
      slurp("tests/fixtures/gqconst.tmir.sexp").c_str(),
      R"({"rectangular":[[1,4],[2,5],[3,6]]})", nullptr, err, sizeof err);
  expect_true("in-worker write_array graph model builds", graph != nullptr);
  if (graph != nullptr) {
    expect_in_worker_write_array_on("graph", graph);
    stanli_model_free(graph);
  }

  const std::string interp_mir = categorical_write_array_mir(
      slurp("tests/fixtures/cat.tmir.sexp"), "categorical_lpmf", false, false,
      false, true);
  err[0] = '\0';
  stanli_model* interp = stanli_model_new_with_opts(
      interp_mir.c_str(), R"({"K":3,"y":2,"ys":[3,1,3]})", nullptr, err,
      sizeof err);
  expect_true("in-worker write_array interpreted model builds",
              interp != nullptr);
  if (interp != nullptr) {
    expect_in_worker_write_array_on("interpreted", interp, true);
    stanli_model_free(interp);
  }

  stanli_model* random = stanli_model_new_with_opts(
      slurp("tests/fixtures/gq_scalar_rng.tmir.sexp").c_str(), "{}", nullptr,
      err, sizeof err);
  expect_true("random compiled write_array model builds", random != nullptr);
  if (random) {
    expect_in_worker_write_array_on("random graph", random, true);
    stanli_model_free(random);
  }

  err[0] = '\0';
  stanli_model* reject = stanli_model_new_with_opts(
      slurp("tests/fixtures/gq_row_reject.tmir.sexp").c_str(), "{}", nullptr,
      err, sizeof err);
  expect_true("in-worker write_array reject model builds", reject != nullptr);
  if (reject == nullptr) return;
  expect_true("in-worker write_array reject model has columns",
              stanli_wa_n_columns(reject) > 0);
  stanli_sample_opts opts;
  stanli_sample_opts_init(&opts);
  opts.seed = 7;
  opts.chains = 1;
  opts.warmup = 20;
  opts.samples = 20;
  const int64_t n = stanli_n_unconstrained(reject);
  const int64_t rows = stanli_n_stored_draws(&opts);
  const int64_t width = stanli_wa_n_columns(reject);
  std::vector<double> draws((size_t)(rows * n));
  std::vector<double> stats((size_t)(rows * STANLI_N_SAMPLER_COLS));
  std::vector<double> values((size_t)(rows * width));
  stanli_sample_report reports[1];
  int interrupted = -1;
  err[0] = '\0';
  const int rc = stanli_sample_multi_metric(
      reject, &opts, nullptr, 0, draws.data(), stats.data(), values.data(),
      nullptr, nullptr, nullptr, nullptr, &interrupted, reports, err,
      sizeof err);
  expect_true("a rejected output row does not stop the chain",
              rc == 0 && interrupted == 0 &&
                  std::all_of(draws.begin(), draws.end(),
                              [](double x) { return std::isfinite(x); }) &&
                  std::all_of(stats.begin(), stats.end(),
                              [](double x) { return std::isfinite(x); }) &&
                  std::any_of(values.begin(), values.end(),
                              [](double x) { return std::isnan(x); }));
  stanli_model_free(reject);
}

// The additive progress entry point must be the old sampler plus observation:
// same bytes, caller-thread callbacks, exact refresh schedule and reports.
void expect_sampling_progress() {
  const std::string mir = slurp("tests/fixtures/gqconst.tmir.sexp");
  const char* data = R"({"rectangular":[[1,4],[2,5],[3,6]]})";
  char err[8192]{};
  stanli_model* quiet =
      stanli_model_new_with_opts(mir.c_str(), data, nullptr, err, sizeof err);
  stanli_model* observed =
      stanli_model_new_with_opts(mir.c_str(), data, nullptr, err, sizeof err);
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

  const int quiet_failed = stanli_sample_multi_metric(
      quiet, &opts, nullptr, 0, quiet_draws.data(), quiet_stats.data(), nullptr,
      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, err, sizeof err);
  ProgressCapture capture{std::this_thread::get_id()};
  std::vector<stanli_sample_report> reports((size_t)opts.chains);
  const int observed_failed = stanli_sample_multi_metric(
      observed, &opts, nullptr, 2, observed_draws.data(), observed_stats.data(),
      nullptr, capture_progress, &capture, nullptr, nullptr, nullptr,
      reports.data(), err, sizeof err);
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
  const int negative = stanli_sample_multi_metric(
      observed, &opts, nullptr, -1, observed_draws.data(),
      observed_stats.data(), nullptr, capture_progress, &rejected, nullptr,
      nullptr, nullptr, reports.data(), err, sizeof err);
  expect_true("C API rejects negative refresh",
              negative != 0 &&
                  std::string(err).find("refresh") != std::string::npos &&
                  rejected.events.empty());

  stanli_model_free(quiet);
  stanli_model_free(observed);
}

// Progress carries each chain's running post-warmup divergence and
// max-treedepth counts, ending at the report's totals. A fixed step size of 5
// diverges on this model; depth 1 saturates every tree.
void expect_sampling_progress_counts() {
  const std::string mir = slurp("tests/fixtures/gqconst.tmir.sexp");
  const char* data = R"({"rectangular":[[1,4],[2,5],[3,6]]})";
  for (const bool diverging : {true, false}) {
    const std::string label = diverging ? "divergent" : "saturating";
    char err[8192]{};
    stanli_model* quiet =
        stanli_model_new_with_opts(mir.c_str(), data, nullptr, err, sizeof err);
    stanli_model* observed =
        stanli_model_new_with_opts(mir.c_str(), data, nullptr, err, sizeof err);
    if (quiet == nullptr || observed == nullptr) {
      ++failures;
      std::printf("FAIL C API progress counts model construction: %s\n", err);
      if (quiet) stanli_model_free(quiet);
      if (observed) stanli_model_free(observed);
      return;
    }
    stanli_sample_opts opts;
    stanli_sample_opts_init(&opts);
    opts.seed = 552;
    opts.chains = 2;
    opts.warmup = 20;
    opts.samples = 40;
    opts.num_threads = 2;
    stanli_metric_opts mo;
    stanli_metric_opts_init(&mo);
    if (diverging) {
      mo.adapt_engaged = 0;
      mo.step_size = 5.0;
    } else {
      opts.max_depth = 1;
    }
    const int64_t n = stanli_n_unconstrained(quiet);
    const int64_t rows = stanli_n_stored_draws(&opts);
    std::vector<double> quiet_draws((size_t)(opts.chains * rows * n));
    std::vector<double> quiet_stats(
        (size_t)(opts.chains * rows * STANLI_N_SAMPLER_COLS));
    std::vector<double> draws(quiet_draws.size());
    std::vector<double> stats(quiet_stats.size());
    const int quiet_failed = stanli_sample_multi_metric(
        quiet, &opts, &mo, 0, quiet_draws.data(), quiet_stats.data(), nullptr,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, err, sizeof err);
    ProgressCapture capture{std::this_thread::get_id()};
    std::vector<stanli_sample_report> reports((size_t)opts.chains);
    const int failed = stanli_sample_multi_metric(
        observed, &opts, &mo, 1, draws.data(), stats.data(), nullptr,
        capture_progress, &capture, nullptr, nullptr, nullptr, reports.data(),
        err, sizeof err);
    expect_true("C API " + label + " progress runs succeed",
                quiet_failed == 0 && failed == 0);
    expect_true("C API " + label + " progress leaves draws and stats unchanged",
                quiet_draws == draws && quiet_stats == stats);
    for (int chain = 1; chain <= opts.chains; ++chain) {
      bool warmup_zero = true;
      const std::array<int64_t, 6>* last = nullptr;
      for (const auto& event : capture.events) {
        if (event[0] != chain) continue;
        if (event[3] != 0)
          warmup_zero = warmup_zero && event[4] == 0 && event[5] == 0;
        last = &event;
      }
      const auto& report = reports[(size_t)(chain - 1)];
      const std::string which = " for chain " + std::to_string(chain);
      expect_true("C API " + label + " warmup progress counts are zero" + which,
                  warmup_zero);
      expect_true(
          "C API " + label + " final progress counts equal the report" + which,
          last != nullptr && (*last)[3] == 0 &&
              (*last)[4] == report.n_divergent &&
              (*last)[5] == report.n_max_treedepth);
      expect_true(
          "C API " + label + " run has post-warmup problems" + which,
          diverging ? report.n_divergent > 0 : report.n_max_treedepth > 0);
    }
    stanli_model_free(quiet);
    stanli_model_free(observed);
  }
}

struct PrintThreadCapture {
  std::thread::id caller;
  size_t caller_prints = 0;
  size_t worker_prints = 0;
};

// BridgeStan's process-wide print callback has no user-data argument. The
// runtime serializes calls, and sampling joins all workers before we inspect
// or reset this capture.
PrintThreadCapture print_thread_capture;

void capture_print_thread(const char*, size_t) {
  if (std::this_thread::get_id() == print_thread_capture.caller)
    ++print_thread_capture.caller_prints;
  else
    ++print_thread_capture.worker_prints;
}

// Explicit inits must still dispatch sampling to the worker pool and return
// the bytes of the sequential run. Observe model evaluation through the
// public print callback: progress is marshalled to the caller, and its event
// order cannot prove concurrency (a fast chain may finish before another
// worker starts).
void expect_inits_run_in_parallel() {
  if (!stanli_thread_safe()) return;
  const std::string mir = slurp("tests/fixtures/rejectprint.tmir.sexp");
  const char* data = R"({"N":3,"lim":1.0})";
  char err[8192]{};
  stanli_model* model =
      stanli_model_new_with_opts(mir.c_str(), data, nullptr, err, sizeof err);
  expect_true("per-chain inits model builds", model != nullptr);
  if (model == nullptr) return;
  stanli_sample_opts opts;
  stanli_sample_opts_init(&opts);
  opts.seed = 31;
  opts.chains = 4;
  opts.warmup = 100;
  opts.samples = 100;
  const int64_t n = stanli_n_unconstrained(model);
  const int64_t rows = stanli_n_stored_draws(&opts);
  std::vector<double> rows_init((size_t)(opts.chains * n));
  for (size_t i = 0; i < rows_init.size(); ++i) rows_init[i] = 0.25 * (double)i;
  std::vector<double> shared_init((size_t)(opts.chains * n), 0.5);
  const std::vector<double>* cases[] = {&rows_init, &shared_init};
  const char* labels[] = {"one row per chain", "one vector for every chain"};
  char* print_error = nullptr;
  const int print_rc =
      bs_set_print_callback(capture_print_thread, &print_error);
  expect_true("install sampling print callback", print_rc == 0);
  bs_free_error_msg(print_error);
  if (print_rc != 0) {
    stanli_model_free(model);
    return;
  }
  for (int k = 0; k < 2; ++k) {
    opts.inits = cases[k]->data();
    std::vector<double> serial_draws((size_t)(opts.chains * rows * n));
    std::vector<double> serial_stats(
        (size_t)(opts.chains * rows * STANLI_N_SAMPLER_COLS));
    opts.num_threads = 1;
    print_thread_capture = {std::this_thread::get_id()};
    int rc = stanli_sample_multi_metric(
        model, &opts, nullptr, 0, serial_draws.data(), serial_stats.data(),
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, err,
        sizeof err);
    expect_true(std::string("sequential run with inits succeeds: ") + labels[k],
                rc == 0);
    expect_true(
        std::string("sequential inits evaluate on the caller: ") + labels[k],
        print_thread_capture.caller_prints > 0 &&
            print_thread_capture.worker_prints == 0);

    std::vector<double> draws(serial_draws.size());
    std::vector<double> stats(serial_stats.size());
    ProgressCapture capture{std::this_thread::get_id()};
    opts.num_threads = opts.chains;
    print_thread_capture = {std::this_thread::get_id()};
    rc = stanli_sample_multi_metric(model, &opts, nullptr, 1, draws.data(),
                                    stats.data(), nullptr, capture_progress,
                                    &capture, nullptr, nullptr, nullptr,
                                    nullptr, err, sizeof err);
    expect_true(std::string("threaded run with inits succeeds: ") + labels[k],
                rc == 0);
    expect_true(
        std::string("threaded inits draws match sequential: ") + labels[k],
        draws == serial_draws && stats == serial_stats);
    expect_true(
        std::string(
            "threaded inits progress covers every transition on caller: ") +
            labels[k],
        capture.caller_thread &&
            capture.events.size() ==
                (size_t)(opts.chains * (opts.warmup + opts.samples)));

    expect_true(std::string("threaded inits evaluate on workers: ") + labels[k],
                print_thread_capture.caller_prints == 0 &&
                    print_thread_capture.worker_prints > 0);
  }
  print_error = nullptr;
  expect_true("restore sampling print callback",
              bs_set_print_callback(nullptr, &print_error) == 0);
  bs_free_error_msg(print_error);
  stanli_model_free(model);
}

struct MessageCapture {
  std::vector<std::string> messages;
};

MessageCapture* active_message_capture = nullptr;

void collect_print(const char* text, size_t len) {
  if (active_message_capture != nullptr)
    active_message_capture->messages.emplace_back(text, len);
}

void collect_message(MessageCapture& capture,
                     const std::function<void()>& run) {
  char* error = nullptr;
  active_message_capture = &capture;
  const int rc = bs_set_print_callback(collect_print, &error);
  bs_free_error_msg(error);
  expect_true("install message collector", rc == 0);
  try {
    run();
  } catch (...) {
    bs_set_print_callback(nullptr, &error);
    bs_free_error_msg(error);
    active_message_capture = nullptr;
    throw;
  }
  bs_set_print_callback(nullptr, &error);
  bs_free_error_msg(error);
  active_message_capture = nullptr;
}

size_t count_containing(const MessageCapture& capture,
                        const std::string& needle) {
  size_t n = 0;
  for (const auto& m : capture.messages)
    if (m.find(needle) != std::string::npos) ++n;
  return n;
}

void expect_sampling_messages() {
  const std::string mir = slurp("tests/fixtures/sample_reject.tmir.sexp");
  char err[8192]{};
  stanli_model_opts model_opts;
  stanli_model_opts_init(&model_opts);
  stanli_model* model = stanli_model_new_with_opts(
      mir.c_str(), "{\"y\":0.5,\"cutoff\":0}", &model_opts, err, sizeof err);
  expect_true("sampling messages model builds", model != nullptr);
  if (model == nullptr) return;
  stanli_sample_opts opts;
  stanli_sample_opts_init(&opts);
  opts.seed = 7;
  opts.chains = 2;
  opts.warmup = 300;
  opts.samples = 300;
  opts.num_threads = 1;
  const int64_t n = stanli_n_unconstrained(model);
  const int64_t rows = stanli_n_stored_draws(&opts);
  std::vector<double> draws((size_t)(opts.chains * rows * n));
  std::vector<double> stats(
      (size_t)(opts.chains * rows * STANLI_N_SAMPLER_COLS));
  const auto run = [&](int refresh) {
    ProgressCapture progress{std::this_thread::get_id()};
    return stanli_sample_multi_metric(
        model, &opts, nullptr, refresh, draws.data(), stats.data(), nullptr,
        refresh > 0 ? capture_progress : nullptr, &progress, nullptr, nullptr,
        nullptr, nullptr, err, sizeof err);
  };

  MessageCapture quiet;
  int rc = 1;
  collect_message(quiet, [&] { rc = run(0); });
  expect_true("quiet run succeeds", rc == 0);
  expect_true("refresh 0 prints no sampler messages", quiet.messages.empty());
  const std::vector<double> quiet_draws = draws;

  MessageCapture loud;
  collect_message(loud, [&] { rc = run(100); });
  expect_true("refresh run succeeds", rc == 0);
  expect_true("messages leave the draws bitwise unchanged",
              draws == quiet_draws);
  const std::string header =
      "Informational Message: The current Metropolis proposal is about to be "
      "rejected because of the following issue:";
  expect_true("rejected proposals are reported", !loud.messages.empty());
  expect_true("every report is one block per transition",
              count_containing(loud, header) == loud.messages.size());
  for (const auto& m : loud.messages) {
    if (m.rfind("Chain [1] ", 0) != 0 && m.rfind("Chain [2] ", 0) != 0) {
      expect_true("report starts with its chain: " + m, false);
      break;
    }
  }
  expect_true("report names the reject text",
              count_containing(loud, "mu must be >= 0, found mu = ") ==
                  loud.messages.size());
  expect_true("report carries CmdStan's closing note",
              count_containing(loud,
                               "If this warning occurs sporadically, such as "
                               "for highly constrained variable types like "
                               "covariance matrices, then the sampler is "
                               "fine,\nChain [") +
                      count_containing(
                          loud,
                          "but if this warning occurs often then your model "
                          "may be either severely ill-conditioned or "
                          "misspecified.") >
                  0);
  expect_true("both chains report",
              count_containing(loud, "Chain [1] Informational") > 0 &&
                  count_containing(loud, "Chain [2] Informational") > 0);
  for (const auto& m : loud.messages)
    for (size_t at = m.find("Chain ["); at != std::string::npos;
         at = m.find("Chain [", at + 1))
      if (at != 0 && m[at - 1] != '\n') {
        expect_true("prefix only at line starts", false);
        break;
      }

  if (stanli_thread_safe()) {
    opts.num_threads = 2;
    MessageCapture threaded;
    collect_message(threaded, [&] { rc = run(100); });
    expect_true("threaded refresh run succeeds", rc == 0);
    expect_true("threaded blocks stay whole",
                count_containing(threaded, header) == threaded.messages.size());
    expect_true("threaded run matches sequential draws", draws == quiet_draws);
    opts.num_threads = 1;
  }

  stanli_model_free(model);

  if (!stanli_thread_safe()) return;
  model_opts.threads_per_chain = 2;
  model = stanli_model_new_with_opts(mir.c_str(), "{\"y\":0.5,\"cutoff\":0}",
                                     &model_opts, err, sizeof err);
  expect_true("threaded model builds", model != nullptr);
  if (model == nullptr) return;
  MessageCapture note;
  collect_message(note, [&] { rc = run(100); });
  expect_true("threads note run succeeds", rc == 0);
  expect_true(
      "unused threads_per_chain is noted once per call",
      count_containing(note,
                       "Note: threads_per_chain = 2 has no effect on this "
                       "model, which has no reduce_sum call to run in "
                       "parallel.") == 1);
  MessageCapture note_quiet;
  collect_message(note_quiet, [&] { rc = run(0); });
  expect_true("refresh 0 prints no threads note",
              count_containing(note_quiet, "threads_per_chain") == 0);
  stanli_model_free(model);
}

std::string take_messages() {
  char* raw = stanli_take_messages();
  if (raw == nullptr) return {};
  std::string text(raw);
  stanli_string_free(raw);
  return text;
}

size_t count_substring(const std::string& text, const std::string& needle) {
  size_t n = 0;
  for (size_t at = text.find(needle); at != std::string::npos;
       at = text.find(needle, at + needle.size()))
    ++n;
  return n;
}

void expect_captured_messages() {
  expect_true("nothing is waiting before capture starts",
              stanli_take_messages() == nullptr);
  stanli_capture_messages(1);
  char err[8192]{};

  const std::string print_mir = slurp("tests/fixtures/rejectprint.tmir.sexp");
  stanli_model* printer = stanli_model_new_with_opts(
      print_mir.c_str(), R"({"N":3,"lim":1.0})", nullptr, err, sizeof err);
  expect_true("capture: print model builds", printer != nullptr);
  if (printer == nullptr) {
    stanli_capture_messages(0);
    return;
  }
  expect_eq("transformed data print is held, not written", take_messages(),
            "compiled with N = 3\n");
  expect_true("taking empties the queue", stanli_take_messages() == nullptr);

  std::vector<double> q((size_t)stanli_n_unconstrained(printer), 0.25);
  std::vector<double> grad(q.size());
  double lp = 0;
  expect_true("capture: gradient succeeds",
              stanli_grad(printer, q.data(), &lp, grad.data()) == 0);
  expect_eq("model print is held", take_messages(),
            "drawing at x = 0.25 v = [0.25,0.25]\n");

  stanli_sample_opts opts;
  stanli_sample_opts_init(&opts);
  opts.seed = 5;
  opts.chains = 4;
  opts.warmup = 20;
  opts.samples = 20;
  opts.num_threads = stanli_thread_safe() ? 4 : 1;
  const int64_t n = stanli_n_unconstrained(printer);
  const int64_t rows = stanli_n_stored_draws(&opts);
  std::vector<double> draws((size_t)(opts.chains * rows * n));
  int polls = 0;
  std::string seen_during_poll;
  struct PollState {
    int* polls;
    std::string* seen;
  } state{&polls, &seen_during_poll};
  auto poll = [](void* user) -> int {
    auto* st = static_cast<PollState*>(user);
    ++*st->polls;
    *st->seen += take_messages();
    return 0;
  };
  int rc = stanli_sample_multi_metric(
      printer, &opts, nullptr, 0, draws.data(), nullptr, nullptr, nullptr,
      nullptr, poll, &state, nullptr, nullptr, err, sizeof err);
  expect_true("capture: threaded sample succeeds", rc == 0);
  const std::string sampled = seen_during_poll + take_messages();
  const size_t lines = count_substring(sampled, "\n");
  expect_true(
      "every chain's prints arrive whole",
      lines > 0 && count_substring(sampled, "drawing at x = ") == lines);
  expect_true("held text ends on a line boundary",
              !sampled.empty() && sampled.back() == '\n');
  stanli_model_free(printer);

  const std::string reject_mir =
      slurp("tests/fixtures/sample_reject.tmir.sexp");
  stanli_model* rejecting = stanli_model_new_with_opts(
      reject_mir.c_str(), "{\"y\":0.5,\"cutoff\":0}", nullptr, err, sizeof err);
  expect_true("capture: rejecting model builds", rejecting != nullptr);
  if (rejecting != nullptr) {
    opts.warmup = 300;
    opts.samples = 300;
    std::vector<double> d((size_t)(opts.chains * stanli_n_stored_draws(&opts) *
                                   stanli_n_unconstrained(rejecting)));
    rc = stanli_sample_multi_metric(rejecting, &opts, nullptr, 100, d.data(),
                                    nullptr, nullptr, nullptr, nullptr, nullptr,
                                    nullptr, nullptr, nullptr, err, sizeof err);
    expect_true("capture: refresh run succeeds", rc == 0);
    const std::string notices = take_messages();
    expect_true(
        "sampler notices are held with their chain",
        notices.find("Chain [1] Informational Message") != std::string::npos &&
            notices.find("Chain [4] Informational Message") !=
                std::string::npos);
    stanli_model_free(rejecting);
  }

  stanli_capture_messages(0);
  expect_true("nothing is left over", stanli_take_messages() == nullptr);
}

void expect_sampling_warnings_api() {
  const std::string mir = slurp("tests/fixtures/sample_reject.tmir.sexp");
  char err[8192]{};
  stanli_model* model = stanli_model_new_with_opts(
      mir.c_str(), "{\"y\":0.5,\"cutoff\":-1e9}", nullptr, err, sizeof err);
  expect_true("warnings model builds", model != nullptr);
  if (model == nullptr) return;
  stanli_sample_opts opts;
  stanli_sample_opts_init(&opts);
  opts.seed = 7;
  opts.chains = 2;
  opts.warmup = 50;
  opts.samples = 50;
  opts.num_threads = 1;
  const int64_t n = stanli_n_unconstrained(model);
  const int64_t rows = stanli_n_stored_draws(&opts);
  std::vector<double> draws((size_t)(opts.chains * rows * n));
  std::vector<double> stats(
      (size_t)(opts.chains * rows * STANLI_N_SAMPLER_COLS));
  std::vector<stanli_sample_report> reports((size_t)opts.chains);
  const int rc = stanli_sample_multi_metric(
      model, &opts, nullptr, 0, draws.data(), stats.data(), nullptr, nullptr,
      nullptr, nullptr, nullptr, nullptr, reports.data(), err, sizeof err);
  expect_true("warnings run succeeds", rc == 0);
  char text[4096];
  expect_true("a healthy run has nothing to warn about",
              stanli_sampling_warnings(&opts, reports.data(), stats.data(),
                                       text, sizeof text) == 1 &&
                  text[0] == '\0');
  reports[0].n_divergent = 4;
  const int64_t need =
      stanli_sampling_warnings(&opts, reports.data(), stats.data(), nullptr, 0);
  expect_true("the size query reports the text length", need > 1);
  std::vector<char> big((size_t)need);
  stanli_sampling_warnings(&opts, reports.data(), stats.data(), big.data(),
                           big.size());
  expect_true(
      "the text names the divergences",
      std::string(big.data())
              .rfind("Warning: 4 of 100 (4.0%) transitions ended with a "
                     "divergence.\n",
                     0) == 0);
  expect_true("null arguments are refused",
              stanli_sampling_warnings(&opts, nullptr, stats.data(), text,
                                       sizeof text) == 0);
  stanli_model_free(model);
}

// Browser streaming must expose the same diagnostics as the existing
// multi-chain API without changing any draw or callback-visible row.
void expect_streaming_stats() {
  const std::string mir = slurp("tests/fixtures/es.tmir.sexp");
  const std::string data = slurp("tests/fixtures/es.json");
  char err[8192]{};
  auto* model = stanli_model_new_with_opts(mir.c_str(), data.c_str(), nullptr,
                                           err, sizeof err);
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
              stanli_sample_multi_metric(
                  model, &opts, nullptr, 0, reference.data(), stats.data(),
                  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
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
              stanli_sample_stream_stats_init(
                  model, opts.seed, opts.warmup, opts.samples, opts.delta,
                  nullptr, streamed.data(), streamed_stats.data(), callback,
                  &capture, err, sizeof err) == 0);
  expect_true("streamed draws match multi-chain bytes", streamed == reference);
  expect_true("streamed stats match multi-chain bytes",
              streamed_stats == stats);
  expect_true("callback sees completed rows", capture.observed == reference);
  expect_true("warmup is excluded from stats",
              capture.warmup == opts.warmup && capture.samples == opts.samples);

  // The browser's additive explicit-init stream is the one-chain multi API,
  // byte for byte. This is the handoff Pathfinder initialization uses.
  std::vector<double> init((size_t)n, 0.0);
  opts.inits = init.data();
  expect_true("explicit-init multi sampling",
              stanli_sample_multi_metric(
                  model, &opts, nullptr, 0, reference.data(), stats.data(),
                  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
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

void expect_sample_options_validated() {
  const std::string mir = slurp("tests/fixtures/es.tmir.sexp");
  const std::string data = slurp("tests/fixtures/es.json");
  char err[8192]{};
  auto* model = stanli_model_new_with_opts(mir.c_str(), data.c_str(), nullptr,
                                           err, sizeof err);
  expect_true("option validation model", model != nullptr);
  if (!model) return;

  struct Case {
    const char* what;
    void (*mutate)(stanli_sample_opts&);
    const char* message;
  };
  const Case cases[] = {
      {"chains = 0", [](stanli_sample_opts& o) { o.chains = 0; },
       "chains must be an integer >= 1"},
      {"chains < 0", [](stanli_sample_opts& o) { o.chains = -3; },
       "chains must be an integer >= 1"},
      {"warmup < 0", [](stanli_sample_opts& o) { o.warmup = -1; },
       "warmup must be an integer >= 0"},
      {"samples < 0", [](stanli_sample_opts& o) { o.samples = -5; },
       "samples must be an integer >= 0"},
      {"thin = 0", [](stanli_sample_opts& o) { o.thin = 0; },
       "thin must be an integer >= 1"},
      {"max_depth = 0", [](stanli_sample_opts& o) { o.max_depth = 0; },
       "max_depth must be an integer >= 1"},
      {"delta = 0", [](stanli_sample_opts& o) { o.delta = 0.0; },
       "delta must be between 0 and 1"},
      {"delta = 1", [](stanli_sample_opts& o) { o.delta = 1.0; },
       "delta must be between 0 and 1"},
      {"delta = 1.5", [](stanli_sample_opts& o) { o.delta = 1.5; },
       "delta must be between 0 and 1"},
      {"delta = NaN",
       [](stanli_sample_opts& o) {
         o.delta = std::numeric_limits<double>::quiet_NaN();
       },
       "delta must be between 0 and 1"},
      {"init_radius < 0", [](stanli_sample_opts& o) { o.init_radius = -1.0; },
       "init_radius must be a finite number >= 0"},
      {"init_radius = Inf",
       [](stanli_sample_opts& o) {
         o.init_radius = std::numeric_limits<double>::infinity();
       },
       "init_radius must be a finite number >= 0"},
      {"chain_id = 0", [](stanli_sample_opts& o) { o.chain_id = 0; },
       "chain_id must be an integer >= 1"},
  };
  for (const Case& c : cases) {
    stanli_sample_opts opts;
    stanli_sample_opts_init(&opts);
    opts.chains = 1;
    opts.warmup = 5;
    opts.samples = 5;
    c.mutate(opts);
    err[0] = '\0';
    expect_true(std::string("check rejects ") + c.what,
                stanli_sample_opts_check(&opts, err, sizeof err) != 0 &&
                    std::string(err) == c.message);

    const int64_t n = stanli_n_unconstrained(model);
    const int64_t chains = opts.chains > 0 ? opts.chains : 1;
    const int64_t rows = std::max<int64_t>(
        1, std::max<int64_t>(opts.samples, 0) + std::max(opts.warmup, 0));
    std::vector<double> draws((size_t)(chains * rows * n));
    err[0] = '\0';
    const int rc = stanli_sample_multi_metric(
        model, &opts, nullptr, 0, draws.data(), nullptr, nullptr, nullptr,
        nullptr, nullptr, nullptr, nullptr, nullptr, err, sizeof err);
    expect_true(std::string("sampling rejects ") + c.what,
                rc != 0 && std::string(err) == c.message);
  }

  stanli_sample_opts ok;
  stanli_sample_opts_init(&ok);
  err[0] = '\0';
  expect_true("default options pass the check",
              stanli_sample_opts_check(&ok, err, sizeof err) == 0);
  ok.warmup = 0;
  ok.samples = 0;
  expect_true("zero warmup and zero samples pass the check",
              stanli_sample_opts_check(&ok, err, sizeof err) == 0);
  stanli_model_free(model);
}

double type7_quantile(std::vector<double> x, double p) {
  std::sort(x.begin(), x.end());
  const double h = (double)(x.size() - 1) * p;
  const size_t lo = (size_t)std::floor(h);
  if (lo + 1 >= x.size()) return x.back();
  return x[lo] + (h - (double)lo) * (x[lo + 1] - x[lo]);
}

void expect_summary_stats() {
  const int64_t C = 4, N = 1000, P = 2;
  std::vector<double> d((size_t)(C * N * P));
  std::mt19937 rng(17);
  std::exponential_distribution<double> ex(1.0);
  std::vector<double> x;
  for (int64_t c = 0; c < C; ++c)
    for (int64_t i = 0; i < N; ++i) {
      const double v = 3.0 + ex(rng);
      d[(size_t)((c * N + i) * P + 0)] = v;
      d[(size_t)((c * N + i) * P + 1)] = 5.0;
      x.push_back(v);
    }
  double mean = 0;
  for (double v : x) mean += v;
  mean /= (double)x.size();
  double ss = 0;
  for (double v : x) ss += (v - mean) * (v - mean);
  const double sd = std::sqrt(ss / (double)(x.size() - 1));

  std::vector<double> out((size_t)(P * STANLI_N_SUMMARY_STATS), -1.0);
  expect_true("summary_stats succeeds",
              stanli_summary_stats(d.data(), C, N, P, out.data()) == 0);
  const double* r = out.data();
  const auto near = [](double got, double want) {
    return std::fabs(got - want) <= 1e-12 * std::max(1.0, std::fabs(want));
  };
  expect_true("summary mean", near(r[STANLI_STAT_MEAN], mean));
  expect_true("summary sd", near(r[STANLI_STAT_SD], sd));
  expect_true("summary q5", near(r[STANLI_STAT_Q5], type7_quantile(x, 0.05)));
  expect_true("summary q50", near(r[STANLI_STAT_Q50], type7_quantile(x, 0.5)));
  expect_true("summary q95", near(r[STANLI_STAT_Q95], type7_quantile(x, 0.95)));
  expect_true("summary rhat near 1",
              r[STANLI_STAT_RHAT] > 0.99 && r[STANLI_STAT_RHAT] < 1.02);
  expect_true("summary bulk ess",
              r[STANLI_STAT_ESS_BULK] > 2000 && r[STANLI_STAT_ESS_BULK] < 6000);
  expect_true("summary tail ess",
              r[STANLI_STAT_ESS_TAIL] > 1500 && r[STANLI_STAT_ESS_TAIL] < 6000);
  expect_true("summary mcse mean",
              r[STANLI_STAT_MCSE_MEAN] > 0.5 * sd / std::sqrt(4000.0) &&
                  r[STANLI_STAT_MCSE_MEAN] < 2.0 * sd / std::sqrt(4000.0));
  expect_true("summary mcse sd",
              r[STANLI_STAT_MCSE_SD] > 0 && r[STANLI_STAT_MCSE_SD] < 0.1);
  const double* k = out.data() + STANLI_N_SUMMARY_STATS;
  expect_true("constant column mean",
              k[STANLI_STAT_MEAN] == 5.0 && k[STANLI_STAT_Q50] == 5.0);
  expect_true("constant column has no rhat or ess",
              std::isnan(k[STANLI_STAT_RHAT]) &&
                  std::isnan(k[STANLI_STAT_ESS_BULK]) &&
                  std::isnan(k[STANLI_STAT_ESS_TAIL]));

  std::vector<double> split((size_t)(2 * 500));
  for (int64_t c = 0; c < 2; ++c)
    for (int64_t i = 0; i < 500; ++i)
      split[(size_t)(c * 500 + i)] = ex(rng) + (c == 0 ? 0.0 : 10.0);
  double s[STANLI_N_SUMMARY_STATS];
  expect_true("summary reads chains as chains",
              stanli_summary_stats(split.data(), 2, 500, 1, s) == 0 &&
                  s[STANLI_STAT_RHAT] > 1.5);
}

void expect_diagnose_text() {
  enum {
    kAcceptStat = 1,
    kStepsize,
    kTreedepth,
    kLeapfrog,
    kDivergent,
    kEnergy
  };
  const int64_t C = 4, N = 1000;
  std::vector<double> d((size_t)(C * N));
  std::vector<double> st((size_t)(C * N * STANLI_N_SAMPLER_COLS), 0.0);
  std::mt19937 rng(23);
  std::normal_distribution<double> nd(0, 1);
  for (int64_t c = 0; c < C; ++c)
    for (int64_t i = 0; i < N; ++i) {
      d[(size_t)(c * N + i)] = nd(rng);
      double* row = &st[(size_t)((c * N + i) * STANLI_N_SAMPLER_COLS)];
      row[kAcceptStat] = 0.9;
      row[kStepsize] = 0.5;
      row[kTreedepth] = (c == 1 && i < 2) ? 4 : 3;
      row[kEnergy] = nd(rng);
    }
  const char* names[] = {"mu"};
  const auto text = [&](const double* draws, const char* const* nm,
                        const double* stats, int max_depth) {
    const int64_t need =
        stanli_diagnose_text(draws, C, N, 1, nm, stats, max_depth, nullptr, 0);
    std::vector<char> buf((size_t)std::max<int64_t>(need, 1));
    stanli_diagnose_text(draws, C, N, 1, nm, stats, max_depth, buf.data(),
                         buf.size());
    const std::string t(buf.data());
    expect_true("diagnose_text reports its full length",
                need > 1 && t.size() + 1 == (size_t)need && t.back() == '\n');
    return t;
  };
  const auto has = [](const std::string& t, const char* s) {
    return t.find(s) != std::string::npos;
  };

  const auto counts = [&](const double* draws, const double* stats,
                          int max_depth) {
    int failed = -1, unavailable = -1;
    const int rc = stanli_diagnose_counts(draws, C, N, 1, stats, max_depth,
                                          &failed, &unavailable);
    expect_true("diagnose_counts succeeds", rc == 0);
    return std::make_pair(failed, unavailable);
  };

  const std::string ok = text(d.data(), names, st.data(), 10);
  expect_true("converged counts",
              counts(d.data(), st.data(), 10) == std::make_pair(0, 0));
  expect_true("converged text: " + ok,
              has(ok, "No divergent transitions.") &&
                  has(ok,
                      "No transitions saturated the maximum treedepth "
                      "of 10.") &&
                  has(ok, "E-BFMI is above 0.3 in every chain.") &&
                  has(ok, "R-hat is below") && has(ok, ", mu).") &&
                  has(ok, "Bulk ESS is at least") &&
                  has(ok, "Tail ESS is at least") &&
                  has(ok, "No problems detected."));
  const std::string depth = text(d.data(), names, st.data(), 4);
  expect_true("max_depth reaches the check: " + depth,
              has(depth,
                  "2 of 4000 transitions saturated the maximum "
                  "treedepth of 4."));
  st[(size_t)(5 * STANLI_N_SAMPLER_COLS + kDivergent)] = 1;
  const std::string div = text(d.data(), names, st.data(), 10);
  expect_true("divergences reach the text: " + div,
              has(div, "1 of 4000 transitions (0.0%) diverged.") &&
                  has(div, "1 diagnostic check failed."));
  expect_true("divergences reach the counts",
              counts(d.data(), st.data(), 10) == std::make_pair(1, 0));
  expect_true("max_depth reaches the counts",
              counts(d.data(), st.data(), 4) == std::make_pair(2, 0));
  const std::string draws_only = text(d.data(), nullptr, nullptr, 10);
  expect_true("null stats run only the R-hat and ESS half: " + draws_only,
              !has(draws_only, "divergent") && !has(draws_only, "E-BFMI") &&
                  has(draws_only, "R-hat is below"));

  for (int64_t i = 0; i < N; ++i) d[(size_t)(3 * N + i)] += 10.0;
  const std::string split = text(d.data(), names, nullptr, 10);
  expect_true("split chains name the parameter: " + split,
              has(split, "R-hat reaches") && has(split, "(mu)"));
  const auto split_counts = counts(d.data(), nullptr, 10);
  expect_true("split chains fail a check in the counts",
              split_counts.first >= 1 && split_counts.second == 0);
  {
    std::vector<double> one(d.begin(), d.begin() + C);
    std::vector<double> one_stats(st.begin(),
                                  st.begin() + C * STANLI_N_SAMPLER_COLS);
    int failed = -1, unavailable = -1;
    expect_true("one draw per chain",
                stanli_diagnose_counts(one.data(), C, 1, 1, one_stats.data(),
                                       10, &failed, &unavailable) == 0);
    expect_true("one draw per chain leaves checks unavailable",
                unavailable > 0);
  }

  char small[8];
  const int64_t need = stanli_diagnose_text(d.data(), C, N, 1, names, nullptr,
                                            10, small, sizeof small);
  expect_true("a short buffer is truncated and terminated",
              need == (int64_t)split.size() + 1 &&
                  std::string(small) == split.substr(0, sizeof small - 1));
}

}  // namespace

void expect_interpreter_policy(const char* fixture,
                               const char* expected_reason) {
  const std::string mir = slurp(fixture);
  const std::string tag(fixture);
  char err[8192]{};
  stanli_model_opts strict;
  stanli_model_opts_init(&strict);
  strict.allow_interpreter = 0;
  stanli_model* model =
      stanli_model_new_with_opts(mir.c_str(), "{}", &strict, err, sizeof err);
  if (expected_reason != nullptr) {
    const std::string refused(err);
    expect_true(
        tag + " refused with allow_interpreter 0: " + refused,
        model == nullptr && refused.find(expected_reason) != std::string::npos);
    if (model != nullptr) stanli_model_free(model);
  } else if (model != nullptr) {
    stanli_model_free(model);
  } else {
    ++failures;
    std::printf("FAIL strict construction for %s: %s\n", fixture, err);
  }
  test_setenv("STANLI_NO_INTERPRETER", "1", 1);
  model =
      stanli_model_new_with_opts(mir.c_str(), "{}", nullptr, err, sizeof err);
  test_unsetenv("STANLI_NO_INTERPRETER");
  if (expected_reason == nullptr) {
    if (model == nullptr) {
      ++failures;
      std::printf("FAIL model construction for %s: %s\n", fixture, err);
      return;
    }
    expect_eq((tag + " no warning").c_str(), stanli_warnings(model), "");
    expect_true(tag + " no interpreter fallbacks",
                stanli_n_interpreter_fallbacks(model) == 0);
    stanli_model_free(model);
    return;
  }
  const std::string error(err);
  expect_true(tag + " refused under STANLI_NO_INTERPRETER: " + error,
              model == nullptr);
  if (model != nullptr) stanli_model_free(model);
  expect_true(tag + " error carries the reason: " + error,
              error.find(expected_reason) != std::string::npos);
  expect_true(tag + " error names the variable: " + error,
              error.find("STANLI_NO_INTERPRETER") != std::string::npos);
  expect_true(
      tag + " error points at the issue tracker: " + error,
      error.find("github.com/seantalts/stanli/issues") != std::string::npos);

  model =
      stanli_model_new_with_opts(mir.c_str(), "{}", nullptr, err, sizeof err);
  if (model == nullptr) {
    ++failures;
    std::printf("FAIL default model construction for %s: %s\n", fixture, err);
    return;
  }
  const std::string text = stanli_warnings(model);
  expect_true(tag + " warning carries the reason: " + text,
              text.find(expected_reason) != std::string::npos);
  bool listed = false;
  for (int64_t i = 0; i < stanli_n_interpreter_fallbacks(model); ++i)
    listed = listed || std::string(stanli_interpreter_fallback(model, i))
                               .find(expected_reason) != std::string::npos;
  expect_true(tag + " the reason is a listed fallback", listed);
  expect_eq((tag + " out of range fallback").c_str(),
            stanli_interpreter_fallback(model, -1), "");
  expect_true(tag + " warning names the build: " + text,
              text.find(stanli_build_id()) != std::string::npos);
  stanli_model_free(model);
}

// When the interpreter throws at every probe point there are no columns for
// transformed parameters or generated quantities. Construction fails rather
// than returning a model whose draws silently lack them (#523).
void test_interpreter_probe_failure() {
  const std::string mir =
      slurp("tests/fixtures/execution_rng_reject.tmir.sexp");
  char err[8192]{};
  stanli_model* model =
      stanli_model_new_with_opts(mir.c_str(), "{}", nullptr, err, sizeof err);
  const std::string error(err);
  expect_true("probe failure refuses the model", model == nullptr);
  if (model != nullptr) stanli_model_free(model);
  expect_true("probe failure error carries the interpreter's error: " + error,
              error.find("Shape parameter") != std::string::npos);
  expect_true(
      "probe failure error points at the issue tracker: " + error,
      error.find("github.com/seantalts/stanli/issues") != std::string::npos);
}

// Null options build with seed 1; only the same construction seed reproduces
// a model's transformed data.
void test_transformed_data_seed() {
  const std::string mir = slurp("tests/fixtures/tdrng.tmir.sexp");
  char err[8192]{};
  stanli_model* one =
      stanli_model_new_with_opts(mir.c_str(), "{}", nullptr, err, sizeof err);
  stanli_model_opts opts;
  stanli_model_opts_init(&opts);
  stanli_model* seeded =
      stanli_model_new_with_opts(mir.c_str(), "{}", &opts, err, sizeof err);
  opts.seed = 2;
  stanli_model* other =
      stanli_model_new_with_opts(mir.c_str(), "{}", &opts, err, sizeof err);
  expect_true(std::string("transformed-data seed construction: ") + err,
              one != nullptr && seeded != nullptr && other != nullptr);
  if (one != nullptr && seeded != nullptr && other != nullptr) {
    const double q[1] = {0.0};
    const auto row = [&](stanli_model* m) {
      std::vector<double> out((size_t)stanli_wa_n_columns(m));
      expect_true("transformed-data seed wa_row",
                  stanli_wa_row(m, q, out.data()) == 0);
      return out;
    };
    const std::vector<double> r1 = row(one), r2 = row(seeded), r3 = row(other);
    expect_true("transformed-data seed column count", r1.size() == 11);
    expect_true("transformed data reports its draw",
                stanli_transformed_data_rng(one) == 1);
    expect_true("unseeded construction draws transformed data at seed 1",
                r1 == r2);
    expect_true("a different construction seed changes transformed data",
                r1.size() == r3.size() && r1 != r3);
  }
  stanli_model_free(one);
  stanli_model_free(seeded);
  stanli_model_free(other);
  // Generated-quantities draws do not make a model seed-dependent.
  const std::string gq = slurp("tests/fixtures/gq_scalar_rng.tmir.sexp");
  stanli_model* gq_only =
      stanli_model_new_with_opts(gq.c_str(), "{}", nullptr, err, sizeof err);
  expect_true("generated-quantities-only RNG model construction",
              gq_only != nullptr);
  if (gq_only != nullptr)
    expect_true("generated-quantities draws are not construction draws",
                stanli_transformed_data_rng(gq_only) == 0);
  stanli_model_free(gq_only);
}

void test_reduce_sum_threads() {
  // Validate counts before parsing, even without an embedded compiler.
  char err[8192]{};
  stanli_model_opts model_opts;
  stanli_model_opts_init(&model_opts);
  model_opts.threads_per_chain = 0;
  auto* invalid =
      stanli_model_new_with_opts("", "{}", &model_opts, err, sizeof err);
  expect_true(
      "zero within-chain thread count rejected",
      invalid == nullptr &&
          std::string(err).find("threads_per_chain") != std::string::npos);
  stanli_model_free(invalid);
  const std::string mir = slurp("tests/fixtures/reduce_sum_bindings.tmir.sexp");
  std::string data = "{\"N\":100000,\"y\":[0.5";
  for (int i = 1; i < 100000; ++i) data += ",0.5";
  data += "]}";
  auto* serial = stanli_model_new_with_opts(mir.c_str(), data.c_str(), nullptr,
                                            err, sizeof err);
  model_opts.threads_per_chain = 4;
  auto* parallel = stanli_model_new_with_opts(mir.c_str(), data.c_str(),
                                              &model_opts, err, sizeof err);
  expect_true("serial ABI reduction construction", serial != nullptr);
  if (!stanli_thread_safe()) {
    expect_true("nonthreaded ABI rejects workers",
                parallel == nullptr &&
                    std::string(err).find("thread-safe") != std::string::npos);
    stanli_model_free(serial);
    stanli_model_free(parallel);
    return;
  }
  expect_true("threaded ABI reduction construction", parallel != nullptr);
  if (serial && parallel) {
    expect_true("ABI serial reduction count",
                stanli_reduce_sum_count(serial) == 0);
    expect_true("ABI retained native reduction",
                stanli_reduce_sum_count(parallel) == 1);
    expect_true("ABI accepted reduction has no refusal",
                stanli_n_reduce_sum_fallbacks(parallel) == 0 &&
                    stanli_reduce_sum_fallback(parallel, 0) == nullptr);
    double q = 0.2, slp = 0, plp = 0, sg = 0, pg = 0;
    expect_true("ABI serial gradient succeeds",
                stanli_grad(serial, &q, &slp, &sg) == 0);
    expect_true("ABI threaded gradient succeeds",
                stanli_grad(parallel, &q, &plp, &pg) == 0);
    expect_near("ABI reduction density", plp, slp);
    expect_near("ABI reduction gradient", pg, sg);
    expect_near("ABI reduction analytic gradient", pg, 50000 - 100001 * q);
    stanli_sample_opts opts;
    stanli_sample_opts_init(&opts);
    opts.chains = 2;
    opts.warmup = 20;
    opts.samples = 5;
    opts.max_depth = 5;
    opts.num_threads = 2;
    std::vector<double> a(10), b(10);
    expect_true("ABI threaded sampling",
                stanli_sample_multi_metric(parallel, &opts, nullptr, 0,
                                           a.data(), nullptr, nullptr, nullptr,
                                           nullptr, nullptr, nullptr, nullptr,
                                           nullptr, err, sizeof err) == 0);
    opts.num_threads = 1;
    expect_true("ABI repeated sampling",
                stanli_sample_multi_metric(parallel, &opts, nullptr, 0,
                                           b.data(), nullptr, nullptr, nullptr,
                                           nullptr, nullptr, nullptr, nullptr,
                                           nullptr, err, sizeof err) == 0);
    expect_true("ABI chain scheduling preserves draws", a == b);
    // The model-owned team must still be live after sampling destroys clones.
    expect_true("ABI team survives sample return",
                stanli_grad(parallel, &q, &plp, &pg) == 0);
    expect_near("ABI post-sampling gradient", pg, sg);
  }
  stanli_model_free(serial);
  stanli_model_free(parallel);
}

// A continued run is the same run: 1000 + 1000 transitions reproduce 2000 in
// one go, draws, sampler columns and generated quantities, because the
// record carries the position, the RNG after the last generated quantities,
// the step size and the post-warmup transition count.
struct ResumeRun {
  int rc = -1;
  std::string err;
  int64_t rows = 0;
  std::vector<double> draws, stats, values, step, inv;
  std::vector<uint8_t> resume;
  std::vector<int64_t> progress_iterations, progress_totals;
};

void record_progress(int32_t, int64_t iteration, int64_t total, int32_t,
                     int64_t, int64_t, void* user) {
  auto* run = static_cast<ResumeRun*>(user);
  run->progress_iterations.push_back(iteration);
  run->progress_totals.push_back(total);
}

ResumeRun resume_run(stanli_model* m, const stanli_sample_opts& opts,
                     int metric, const std::vector<double>* inv_metric,
                     const std::vector<uint8_t>* resume_in, int refresh = 0) {
  ResumeRun r;
  const int64_t n = stanli_n_unconstrained(m);
  const int64_t width = stanli_wa_n_columns(m);
  const size_t record = stanli_resume_size(m);
  const int64_t metric_len = metric == STANLI_METRIC_DENSE ? n * n : n;
  stanli_sample_opts o = opts;
  std::vector<const uint8_t*> in_ptrs;
  std::vector<size_t> in_lens;
  if (resume_in != nullptr) {
    for (int c = 0; c < o.chains; ++c) {
      in_ptrs.push_back(resume_in->data() + (size_t)c * record);
      in_lens.push_back(record);
    }
    o.resume_in = in_ptrs.data();
    o.resume_in_len = in_lens.data();
  }
  r.resume.assign((size_t)o.chains * record, 0);
  o.resume_out = r.resume.data();
  r.rows = stanli_n_stored_draws(&o);
  r.draws.assign((size_t)(o.chains * r.rows * n), -1.0);
  r.stats.assign((size_t)(o.chains * r.rows * STANLI_N_SAMPLER_COLS), -1.0);
  r.values.assign((size_t)(o.chains * r.rows * width), -1.0);
  r.step.assign((size_t)o.chains, -1.0);
  r.inv.assign((size_t)(o.chains * metric_len), -1.0);
  stanli_metric_opts mo;
  stanli_metric_opts_init(&mo);
  mo.metric = metric;
  if (inv_metric != nullptr) {
    mo.inv_metric = inv_metric->data();
    mo.adapt_engaged = 0;
  }
  mo.adapted_step_size = r.step.data();
  mo.adapted_inv_metric = r.inv.data();
  char err[4096]{};
  r.rc = stanli_sample_multi_metric(
      m, &o, &mo, refresh, r.draws.data(), r.stats.data(), r.values.data(),
      refresh > 0 ? record_progress : nullptr, &r, nullptr, nullptr, nullptr,
      nullptr, err, sizeof err);
  r.err = err;
  return r;
}

// Chain c's rows of `first` followed by its rows of `second`.
std::vector<double> concat_chains(const std::vector<double>& first,
                                  int64_t first_rows,
                                  const std::vector<double>& second,
                                  int64_t second_rows, int chains,
                                  int64_t width) {
  std::vector<double> out;
  for (int c = 0; c < chains; ++c) {
    out.insert(out.end(), first.begin() + c * first_rows * width,
               first.begin() + (c + 1) * first_rows * width);
    out.insert(out.end(), second.begin() + c * second_rows * width,
               second.begin() + (c + 1) * second_rows * width);
  }
  return out;
}

bool same_bits(const std::vector<double>& a, const std::vector<double>& b) {
  return a.size() == b.size() &&
         std::memcmp(a.data(), b.data(), sizeof(double) * a.size()) == 0;
}

void expect_resume_matches_one_run(stanli_model* m, int metric, int threads,
                                   int thin, bool save_warmup) {
  const std::string label =
      std::string(metric == STANLI_METRIC_DENSE ? "dense" : "diag") +
      " threads " + std::to_string(threads) + " thin " + std::to_string(thin) +
      (save_warmup ? " save_warmup" : "");
  stanli_sample_opts opts;
  stanli_sample_opts_init(&opts);
  opts.seed = 11;
  opts.chains = 2;
  opts.warmup = 200;
  opts.thin = thin;
  opts.save_warmup = save_warmup ? 1 : 0;
  opts.num_threads = threads;
  const int64_t n = stanli_n_unconstrained(m);
  const int64_t width = stanli_wa_n_columns(m);

  stanli_sample_opts whole = opts;
  whole.samples = 41;
  const ResumeRun one = resume_run(m, whole, metric, nullptr, nullptr);
  stanli_sample_opts head = opts;
  head.samples = 20;
  const ResumeRun first = resume_run(m, head, metric, nullptr, nullptr);
  stanli_sample_opts tail = opts;
  tail.warmup = 0;
  tail.save_warmup = 0;
  tail.samples = 21;
  const ResumeRun second =
      resume_run(m, tail, metric, &first.inv, &first.resume, 10);
  expect_true(
      "resume " + label + ": runs succeed: " + one.err + first.err + second.err,
      one.rc == 0 && first.rc == 0 && second.rc == 0);
  if (one.rc != 0 || first.rc != 0 || second.rc != 0) return;
  expect_true("resume " + label + ": stored rows add up",
              first.rows + second.rows == one.rows);
  expect_true("resume " + label + ": draws match one run bitwise",
              same_bits(concat_chains(first.draws, first.rows, second.draws,
                                      second.rows, opts.chains, n),
                        one.draws));
  expect_true(
      "resume " + label + ": sampler columns match one run bitwise",
      same_bits(concat_chains(first.stats, first.rows, second.stats,
                              second.rows, opts.chains, STANLI_N_SAMPLER_COLS),
                one.stats));
  expect_true(
      "resume " + label + ": generated quantities match one run bitwise",
      same_bits(concat_chains(first.values, first.rows, second.values,
                              second.rows, opts.chains, width),
                one.values));
  expect_true("resume " + label + ": the final record matches one run's",
              second.resume == one.resume);
  expect_true("resume " + label + ": the step size is carried",
              same_bits(second.step, one.step));
  // Progress continues the numbering: sampling resumes at transition 21 of
  // 41, and the refresh schedule is the one-run schedule.
  expect_true("resume " + label + ": progress continues the numbering",
              !second.progress_iterations.empty() &&
                  second.progress_iterations.front() == 21 &&
                  second.progress_iterations.back() == 41 &&
                  second.progress_totals.front() == 41);
  bool on_schedule = true;
  for (size_t i = 0; i < second.progress_iterations.size(); ++i)
    on_schedule = on_schedule && (second.progress_iterations[i] == 21 ||
                                  second.progress_iterations[i] == 41 ||
                                  second.progress_iterations[i] % 10 == 0);
  expect_true("resume " + label + ": refresh counts across the seam",
              on_schedule);
}

void expect_resume() {
  const std::string mir = slurp("tests/fixtures/resume_gq_rng.tmir.sexp");
  const char* data = R"({"N":5,"y":[0.3,-1.2,2.1,0.8,1.5]})";
  char err[8192]{};
  stanli_model* m =
      stanli_model_new_with_opts(mir.c_str(), data, nullptr, err, sizeof err);
  expect_true(std::string("resume model builds: ") + err, m != nullptr);
  if (m == nullptr) return;
  const int64_t n = stanli_n_unconstrained(m);
  expect_true("resume record has a size", stanli_resume_size(m) > 8 * n);

  stanli_sample_opts defaults;
  stanli_sample_opts_init(&defaults);
  expect_true("resume fields default to null",
              defaults.resume_in == nullptr &&
                  defaults.resume_in_len == nullptr &&
                  defaults.resume_out == nullptr);

  const int threads = stanli_thread_safe() ? 2 : 1;
  for (int metric : {STANLI_METRIC_DIAG, STANLI_METRIC_DENSE}) {
    expect_resume_matches_one_run(m, metric, 1, 1, false);
    expect_resume_matches_one_run(m, metric, threads, 1, false);
    expect_resume_matches_one_run(m, metric, 1, 3, false);
  }
  expect_resume_matches_one_run(m, STANLI_METRIC_DIAG, threads, 3, true);

  stanli_sample_opts opts;
  stanli_sample_opts_init(&opts);
  opts.seed = 5;
  opts.chains = 2;
  opts.warmup = 100;
  opts.samples = 10;
  const ResumeRun base =
      resume_run(m, opts, STANLI_METRIC_DIAG, nullptr, nullptr);
  expect_true("resume base run succeeds: " + base.err, base.rc == 0);
  if (base.rc != 0) {
    stanli_model_free(m);
    return;
  }

  stanli_sample_opts more = opts;
  more.warmup = 0;
  more.thin = 3;
  more.samples = 10;
  {
    const size_t record = stanli_resume_size(m);
    const uint8_t* ptrs[2] = {base.resume.data(), base.resume.data() + record};
    const size_t lens[2] = {record, record};
    stanli_sample_opts o = more;
    o.resume_in = ptrs;
    o.resume_in_len = lens;
    // Transitions 10..19 keep 12, 15 and 18.
    expect_true("stored rows count from the record's offset",
                stanli_n_stored_draws(&o) == 3);
  }

  stanli_sample_opts warm = more;
  warm.warmup = 10;
  const ResumeRun warmed =
      resume_run(m, warm, STANLI_METRIC_DIAG, &base.inv, &base.resume);
  expect_true("resume with warmup is refused: " + warmed.err,
              warmed.rc != 0 && warmed.err.find("warmup") != std::string::npos);

  stanli_model* other = stanli_model_new_with_opts(
      mir.c_str(), R"({"N":5,"y":[0.3,-1.2,2.1,0.8,9.5]})", nullptr, err,
      sizeof err);
  expect_true("resume other-data model builds", other != nullptr);
  if (other != nullptr) {
    const ResumeRun mismatched =
        resume_run(other, more, STANLI_METRIC_DIAG, &base.inv, &base.resume);
    expect_true(
        "resume on different data is refused: " + mismatched.err,
        mismatched.rc != 0 && mismatched.err.find("different model or data") !=
                                  std::string::npos);
    stanli_model_free(other);
  }

  stanli_model* wider = stanli_model_new_with_opts(
      slurp("tests/fixtures/gq_scalar_rng_container_guard.tmir.sexp").c_str(),
      "{}", nullptr, err, sizeof err);
  stanli_model* narrower = stanli_model_new_with_opts(
      slurp("tests/fixtures/gq_scalar_rng.tmir.sexp").c_str(), "{}", nullptr,
      err, sizeof err);
  expect_true("resume other models build",
              wider != nullptr && narrower != nullptr);
  if (wider != nullptr && narrower != nullptr) {
    expect_true("the other model has as many parameters",
                stanli_n_unconstrained(wider) == n);
    const ResumeRun other_model =
        resume_run(wider, more, STANLI_METRIC_DIAG, &base.inv, &base.resume);
    expect_true("resume on a different model is refused: " + other_model.err,
                other_model.rc != 0 &&
                    other_model.err.find("different model or data") !=
                        std::string::npos);
    const std::vector<double> one_inv(2, 1.0);
    const ResumeRun other_size =
        resume_run(narrower, more, STANLI_METRIC_DIAG, &one_inv, &base.resume);
    expect_true(
        "resume on a model of another size is refused: " + other_size.err,
        other_size.rc != 0 && other_size.err.find("unconstrained parameters") !=
                                  std::string::npos);
  }
  if (wider != nullptr) stanli_model_free(wider);
  if (narrower != nullptr) stanli_model_free(narrower);

  std::vector<uint8_t> bad_version = base.resume;
  bad_version[4] ^= 0x7f;
  // A later format may put anything where this one keeps the offset.
  for (size_t c = 0; c < 2; ++c)
    std::memset(bad_version.data() + c * stanli_resume_size(m) + 24, 0xff, 8);
  const ResumeRun versioned =
      resume_run(m, more, STANLI_METRIC_DIAG, &base.inv, &bad_version);
  expect_true(
      "resume with an unknown version is refused: " + versioned.err,
      versioned.rc != 0 && versioned.err.find("version") != std::string::npos);

  {
    const size_t record = stanli_resume_size(m);
    const uint8_t* ptrs[2] = {base.resume.data(), base.resume.data() + record};
    size_t lens[2] = {record - 1, record};
    stanli_sample_opts o = more;
    o.resume_in = ptrs;
    o.resume_in_len = lens;
    const int64_t rows = stanli_n_stored_draws(&o);
    std::vector<double> d((size_t)(2 * (rows > 0 ? rows : 1) * n));
    char e[1024]{};
    const int rc = stanli_sample_multi_metric(
        m, &o, nullptr, 0, d.data(), nullptr, nullptr, nullptr, nullptr,
        nullptr, nullptr, nullptr, nullptr, e, sizeof e);
    expect_true(std::string("resume with a short record is refused: ") + e,
                rc != 0 && std::strstr(e, "bytes") != nullptr);

    std::vector<double> starts((size_t)(2 * n), 0.0);
    lens[0] = record;
    o.inits = starts.data();
    e[0] = '\0';
    const int rc_inits = stanli_sample_multi_metric(
        m, &o, nullptr, 0, d.data(), nullptr, nullptr, nullptr, nullptr,
        nullptr, nullptr, nullptr, nullptr, e, sizeof e);
    expect_true(std::string("resume with inits is refused: ") + e,
                rc_inits != 0 && std::strstr(e, "inits") != nullptr);
  }
  stanli_model_free(m);
}

void expect_metric_options() {
  char err[8192]{};
  stanli_model* model = stanli_model_new_with_opts(
      slurp("tests/fixtures/gq_scalar_rng_container_guard.tmir.sexp").c_str(),
      "{}", nullptr, err, sizeof err);
  expect_true("metric model builds", model != nullptr);
  if (model == nullptr) return;
  const int64_t n = stanli_n_unconstrained(model);
  expect_true("metric model has two parameters", n == 2);

  stanli_sample_opts opts;
  stanli_sample_opts_init(&opts);
  opts.seed = 3;
  opts.chains = 2;
  opts.warmup = 300;
  opts.samples = 20;
  const int64_t rows = stanli_n_stored_draws(&opts);
  const size_t n_draws = (size_t)(opts.chains * rows * n);
  const size_t n_stats = (size_t)(opts.chains * rows * STANLI_N_SAMPLER_COLS);

  stanli_metric_opts dm;
  stanli_metric_opts_init(&dm);
  expect_true("metric defaults",
              dm.metric == STANLI_METRIC_DIAG && dm.inv_metric == nullptr &&
                  dm.step_size == 1.0 && dm.adapt_engaged == 1 &&
                  dm.adapt_metric == 1 && dm.adapted_step_size == nullptr &&
                  dm.adapted_inv_metric == nullptr);

  std::vector<double> want_draws(n_draws), want_stats(n_stats);
  err[0] = '\0';
  int rc = stanli_sample_multi_metric(
      model, &opts, nullptr, 0, want_draws.data(), want_stats.data(), nullptr,
      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, err, sizeof err);
  expect_true("metric reference run succeeds", rc == 0);

  std::vector<double> draws(n_draws, -1.0), stats(n_stats, -1.0);
  err[0] = '\0';
  rc = stanli_sample_multi_metric(
      model, &opts, &dm, 0, draws.data(), stats.data(), nullptr, nullptr,
      nullptr, nullptr, nullptr, nullptr, nullptr, err, sizeof err);
  expect_true("default metric options succeed", rc == 0);
  expect_true("default metric options reproduce write_array bitwise",
              draws == want_draws && stats == want_stats);

  std::vector<double> inv(opts.chains * n * n);
  for (int64_t c = 0; c < opts.chains; ++c) {
    inv[(size_t)(c * 4 + 0)] = 2.0;
    inv[(size_t)(c * 4 + 1)] = 0.5;
    inv[(size_t)(c * 4 + 2)] = 0.5;
    inv[(size_t)(c * 4 + 3)] = 1.5;
  }
  std::vector<double> step(opts.chains, -1.0), adapted(inv.size(), -1.0);
  stanli_metric_opts dense = dm;
  dense.metric = STANLI_METRIC_DENSE;
  dense.inv_metric = inv.data();
  dense.adapted_step_size = step.data();
  dense.adapted_inv_metric = adapted.data();
  err[0] = '\0';
  rc = stanli_sample_multi_metric(
      model, &opts, &dense, 0, draws.data(), stats.data(), nullptr, nullptr,
      nullptr, nullptr, nullptr, nullptr, nullptr, err, sizeof err);
  expect_true(
      std::string("dense metric with a starting inverse metric: ") + err,
      rc == 0);
  bool sized = true;
  for (int64_t c = 0; c < opts.chains; ++c) {
    sized = sized && step[(size_t)c] > 0 && std::isfinite(step[(size_t)c]);
    const double* m = adapted.data() + c * 4;
    sized = sized && m[0] > 0 && m[3] > 0 && std::fabs(m[1] - m[2]) < 1e-12 &&
            m[0] * m[3] - m[1] * m[2] > 0;
  }
  expect_true("dense metric reports positive step sizes and SPD matrices",
              sized);
  expect_true("dense metric adapts away from the starting matrix",
              adapted[0] != inv[0] || adapted[1] != inv[1]);

  std::vector<double> diag_adapted((size_t)(opts.chains * n), -1.0);
  stanli_metric_opts diag = dm;
  diag.adapted_inv_metric = diag_adapted.data();
  rc = stanli_sample_multi_metric(
      model, &opts, &diag, 0, draws.data(), stats.data(), nullptr, nullptr,
      nullptr, nullptr, nullptr, nullptr, nullptr, err, sizeof err);
  expect_true("diag metric reports n values per chain",
              rc == 0 && std::all_of(diag_adapted.begin(), diag_adapted.end(),
                                     [](double x) { return x > 0; }));

  std::vector<double> unit_adapted(4, -1.0);
  stanli_metric_opts unit = dm;
  unit.metric = STANLI_METRIC_UNIT;
  unit.adapted_inv_metric = unit_adapted.data();
  rc = stanli_sample_multi_metric(
      model, &opts, &unit, 0, draws.data(), stats.data(), nullptr, nullptr,
      nullptr, nullptr, nullptr, nullptr, nullptr, err, sizeof err);
  expect_true("unit metric writes no inverse metric",
              rc == 0 && std::all_of(unit_adapted.begin(), unit_adapted.end(),
                                     [](double x) { return x == -1.0; }));

  std::vector<double> fixed_step(opts.chains, -1.0);
  stanli_metric_opts fixed = dm;
  fixed.step_size = 0.25;
  fixed.adapt_engaged = 0;
  fixed.adapted_step_size = fixed_step.data();
  rc = stanli_sample_multi_metric(
      model, &opts, &fixed, 0, draws.data(), stats.data(), nullptr, nullptr,
      nullptr, nullptr, nullptr, nullptr, nullptr, err, sizeof err);
  bool constant = rc == 0;
  for (int64_t c = 0; c < opts.chains; ++c) {
    constant = constant && fixed_step[(size_t)c] == 0.25;
    for (int64_t i = 0; i < rows; ++i)
      constant =
          constant &&
          stats[(size_t)((c * rows + i) * STANLI_N_SAMPLER_COLS + 2)] == 0.25;
  }
  expect_true("adapt off keeps the supplied step size throughout", constant);

  {
    std::vector<double> kept_step(opts.chains, -1.0),
        kept((size_t)(opts.chains * n * n), -1.0);
    stanli_metric_opts step_only = dense;
    step_only.adapt_metric = 0;
    step_only.adapted_step_size = kept_step.data();
    step_only.adapted_inv_metric = kept.data();
    err[0] = '\0';
    rc = stanli_sample_multi_metric(
        model, &opts, &step_only, 0, draws.data(), stats.data(), nullptr,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, err, sizeof err);
    expect_true(std::string("dense step-size-only adaptation: ") + err,
                rc == 0);
    expect_true("step-size-only keeps a dense inverse metric bitwise",
                kept == inv);
    bool moved = true;
    for (int64_t c = 0; c < opts.chains; ++c)
      moved = moved && kept_step[(size_t)c] != dense.step_size &&
              kept_step[(size_t)c] > 0 && std::isfinite(kept_step[(size_t)c]);
    expect_true("step-size-only still adapts the step size", moved);
    bool constant_after = true;
    for (int64_t c = 0; c < opts.chains; ++c)
      for (int64_t i = 0; i < rows; ++i)
        constant_after =
            constant_after &&
            stats[(size_t)((c * rows + i) * STANLI_N_SAMPLER_COLS + 2)] ==
                kept_step[(size_t)c];
    expect_true("step-size-only samples at the adapted step size",
                constant_after);

    std::vector<double> diag_inv((size_t)(opts.chains * n));
    for (size_t i = 0; i < diag_inv.size(); ++i) diag_inv[i] = 0.5 + 0.25 * i;
    std::vector<double> diag_kept(diag_inv.size(), -1.0);
    stanli_metric_opts diag_step_only = dm;
    diag_step_only.adapt_metric = 0;
    diag_step_only.inv_metric = diag_inv.data();
    diag_step_only.adapted_step_size = kept_step.data();
    diag_step_only.adapted_inv_metric = diag_kept.data();
    rc = stanli_sample_multi_metric(
        model, &opts, &diag_step_only, 0, draws.data(), stats.data(), nullptr,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, err, sizeof err);
    expect_true("diag step-size-only keeps a supplied diagonal bitwise",
                rc == 0 && diag_kept == diag_inv);

    std::fill(diag_kept.begin(), diag_kept.end(), -1.0);
    diag_step_only.inv_metric = nullptr;
    rc = stanli_sample_multi_metric(
        model, &opts, &diag_step_only, 0, draws.data(), stats.data(), nullptr,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, err, sizeof err);
    expect_true("diag step-size-only with no metric keeps the identity",
                rc == 0 && std::all_of(diag_kept.begin(), diag_kept.end(),
                                       [](double x) { return x == 1.0; }));

    std::vector<double> a_draws(n_draws), b_draws(n_draws), a_stats(n_stats),
        b_stats(n_stats);
    const auto same_draws = [&](const stanli_metric_opts& a,
                                const stanli_metric_opts& b) {
      const int ra = stanli_sample_multi_metric(
          model, &opts, &a, 0, a_draws.data(), a_stats.data(), nullptr, nullptr,
          nullptr, nullptr, nullptr, nullptr, nullptr, err, sizeof err);
      const int rb = stanli_sample_multi_metric(
          model, &opts, &b, 0, b_draws.data(), b_stats.data(), nullptr, nullptr,
          nullptr, nullptr, nullptr, nullptr, nullptr, err, sizeof err);
      return ra == 0 && rb == 0 && a_draws == b_draws && a_stats == b_stats;
    };
    stanli_metric_opts unit_off = unit;
    unit_off.adapted_inv_metric = nullptr;
    stanli_metric_opts unit_on = unit_off;
    unit_off.adapt_metric = 0;
    expect_true("adapt_metric is a no-op with the unit metric",
                same_draws(unit_off, unit_on));
    stanli_metric_opts fixed_off = fixed;
    fixed_off.adapted_step_size = nullptr;
    stanli_metric_opts fixed_on = fixed_off;
    fixed_off.adapt_metric = 0;
    expect_true("adapt_metric is a no-op without adaptation",
                same_draws(fixed_off, fixed_on));
  }

  std::vector<double> bad = inv;
  bad[0] = -1.0;
  bad[3] = -1.0;
  stanli_metric_opts nonspd = dm;
  nonspd.metric = STANLI_METRIC_DENSE;
  nonspd.inv_metric = bad.data();
  err[0] = '\0';
  rc = stanli_sample_multi_metric(
      model, &opts, &nonspd, 0, draws.data(), stats.data(), nullptr, nullptr,
      nullptr, nullptr, nullptr, nullptr, nullptr, err, sizeof err);
  expect_true(
      "a non positive definite inverse metric fails with Stan's message",
      rc != 0 && std::strstr(err, "positive definite") != nullptr);
  expect_true("an invalid metric fails once, before any chain runs",
              std::strstr(err, "chain ") == nullptr);

  stanli_metric_opts bad_step = dm;
  bad_step.step_size = 0.0;
  err[0] = '\0';
  rc = stanli_sample_multi_metric(
      model, &opts, &bad_step, 0, draws.data(), stats.data(), nullptr, nullptr,
      nullptr, nullptr, nullptr, nullptr, nullptr, err, sizeof err);
  expect_true("a zero step size is rejected",
              rc != 0 && std::strstr(err, "step_size") != nullptr);

  stanli_metric_opts bad_kind = dm;
  bad_kind.metric = 7;
  err[0] = '\0';
  rc = stanli_sample_multi_metric(
      model, &opts, &bad_kind, 0, draws.data(), stats.data(), nullptr, nullptr,
      nullptr, nullptr, nullptr, nullptr, nullptr, err, sizeof err);
  expect_true("an unknown metric kind is rejected",
              rc != 0 && std::strstr(err, "metric") != nullptr);
  stanli_model_free(model);
}

void test_source_include_paths() {
  char err[8192]{};
  expect_true("null include array is rejected",
              stanli_stan_to_mir_with_opts("model {}", nullptr, 1, 0, err,
                                           sizeof err) == nullptr);
  expect_true("null include array names the error",
              std::strstr(err, "include_paths") != nullptr);
  const char* null_path[] = {nullptr};
  expect_true("null include element is rejected",
              stanli_stan_to_mir_with_opts("model {}", null_path, 1, 0, err,
                                           sizeof err) == nullptr);
  if (!stanli_has_embedded_stanc()) return;

  // Enough copied path strings to collect while marshalling under a small
  // OCaml minor heap. All arguments must remain live until the callback ends.
  const std::vector<const char*> paths(1024, "tests/compiler");
  const char* code = "#include portable_include.stan\n";
  char* mir = stanli_stan_to_mir_with_opts(code, paths.data(), paths.size(), 0,
                                           err, sizeof err);
  expect_true(std::string("embedded nested includes: ") + err, mir != nullptr);
  if (mir) {
    stanli_model* model =
        stanli_model_new_with_opts(mir, "{}", nullptr, err, sizeof err);
    stanli_string_free(mir);
    expect_true(std::string("included MIR loads: ") + err, model != nullptr);
    if (model) {
      double q = 0.5, lp = 0, grad = 0;
      expect_true("included gradient succeeds",
                  stanli_grad(model, &q, &lp, &grad) == 0);
      expect_true("included gradient is exact", grad == -0.5);
      stanli_model_free(model);
    }
  }
  mir = stanli_stan_to_mir_with_opts(code, nullptr, 0, 0, err, sizeof err);
  expect_true("include paths do not leak between calls", mir == nullptr);
  stanli_string_free(mir);
  expect_true("missing include keeps its filename",
              std::strstr(err, "portable_include.stan") != nullptr);
  mir =
      stanli_stan_to_mir_with_opts("model {}", nullptr, 0, 0, err, sizeof err);
  expect_true("compiler recovers after include error", mir != nullptr);
  stanli_string_free(mir);

  // multinma's model as shipped, with its eleven /include/ files, against the
  // flattened copy in the corpus.
  const std::string data = slurp("tests/multinma/multinma_smoking_re.json");
  const char* multinma_paths[] = {"tests/multinma/unflattened"};
  const std::string sources[] = {
      slurp("tests/multinma/unflattened/binomial_1par.stan"),
      slurp("tests/multinma/multinma_smoking_re.stan")};
  std::vector<double> lp(2), grads[2];
  for (int i = 0; i < 2; ++i) {
    char* m = stanli_stan_to_mir_with_opts(sources[i].c_str(), multinma_paths,
                                           i == 0 ? 1 : 0, 0, err, sizeof err);
    expect_true(std::string("multinma compiles: ") + err, m != nullptr);
    if (!m) return;
    stanli_model* model =
        stanli_model_new_with_opts(m, data.c_str(), nullptr, err, sizeof err);
    stanli_string_free(m);
    expect_true(std::string("multinma loads: ") + err, model != nullptr);
    if (!model) return;
    std::vector<double> q(stanli_n_unconstrained(model));
    for (size_t j = 0; j < q.size(); ++j) q[j] = 0.1 * (j % 5) - 0.2;
    grads[i].resize(q.size());
    expect_true("multinma gradient succeeds",
                stanli_grad(model, q.data(), &lp[i], grads[i].data()) == 0);
    stanli_model_free(model);
  }
  expect_true("unflattened multinma matches the flattened corpus model",
              lp[0] == lp[1] && grads[0] == grads[1] && !grads[0].empty());
}

void test_fast_math_options() {
  stanli_model_opts defaults;
  stanli_model_opts_init(&defaults);
  expect_true("opts default seed", defaults.seed == 1);
  expect_true("opts default threads", defaults.threads_per_chain == 1);
  expect_true("opts default fast_math off", defaults.fast_math == 0);
  expect_true("opts default allows the interpreter",
              defaults.allow_interpreter == 1);
  if (!stanli_has_embedded_stanc()) return;

  char err[8192]{};
  const std::string code = slurp("tests/compiler/portable_fast_math.stan");
  const char* data = "{\"N\":4,\"x\":[1,2,3,4],\"y\":[1.1,1.9,3.2,3.9]}";
  char* plain = stanli_stan_to_mir_with_opts(code.c_str(), nullptr, 0, 0, err,
                                             sizeof err);
  expect_true(std::string("mir without fast math: ") + err, plain != nullptr);
  char* fast = stanli_stan_to_mir_with_opts(code.c_str(), nullptr, 0, 1, err,
                                            sizeof err);
  expect_true(std::string("mir with fast math: ") + err, fast != nullptr);
  if (plain && fast) {
    expect_true("fast_math 1 changes the MIR", std::strcmp(plain, fast) != 0);
  }
  const char* include_paths[] = {"tests/compiler"};
  char* fast_inc = stanli_stan_to_mir_with_opts(
      "#include portable_include.stan\n", include_paths, 1, 1, err, sizeof err);
  expect_true(std::string("fast math with includes: ") + err,
              fast_inc != nullptr);
  stanli_string_free(fast_inc);

  const double q[3] = {0.3, 0.7, -0.2};
  double lp_plain = 0, lp_fast = 0, g_plain[3]{}, g_fast[3]{};
  stanli_model* a = stanli_model_new_from_stan_with_opts(
      code.c_str(), data, nullptr, err, sizeof err);
  expect_true(std::string("model without opts: ") + err, a != nullptr);
  stanli_model_opts fast_opts = defaults;
  fast_opts.fast_math = 1;
  stanli_model* b = stanli_model_new_from_stan_with_opts(
      code.c_str(), data, &fast_opts, err, sizeof err);
  expect_true(std::string("model with fast math from source: ") + err,
              b != nullptr);
  stanli_model* c = nullptr;
  if (fast)
    c = stanli_model_new_with_opts(fast, data, &fast_opts, err, sizeof err);
  expect_true(std::string("model with fast math from MIR: ") + err,
              c != nullptr);
  stanli_model* d = stanli_model_new_from_stan_with_opts(
      code.c_str(), data, nullptr, err, sizeof err);
  expect_true(std::string("null opts mean defaults: ") + err, d != nullptr);
  if (a && b && c && d) {
    expect_true("grad plain", stanli_grad(a, q, &lp_plain, g_plain) == 0);
    expect_true("grad fast", stanli_grad(b, q, &lp_fast, g_fast) == 0);
    expect_near("fast lp", lp_fast, lp_plain);
    for (int i = 0; i < 3; ++i) expect_near("fast grad", g_fast[i], g_plain[i]);
    double lp_c = 0, lp_d = 0, g_c[3]{}, g_d[3]{};
    expect_true("grad from fast MIR", stanli_grad(c, q, &lp_c, g_c) == 0);
    expect_true("grad null opts", stanli_grad(d, q, &lp_d, g_d) == 0);
    expect_near("fast MIR agrees with fast source", lp_c, lp_fast);
    expect_true("null opts agree with defaults", lp_d == lp_plain);
  }
  stanli_model_free(a);
  stanli_model_free(b);
  stanli_model_free(c);
  stanli_model_free(d);
  stanli_string_free(plain);
  stanli_string_free(fast);
}

// stanli_bridgestan_data builds the payload bs_model_construct reads, from
// data as a structure: non-finite tokens survive, and a manifest already in
// the data is replaced rather than duplicated.
void test_build_id_names_the_abi() {
  const std::string want = "abi" + std::to_string(STANLI_ABI_VERSION) + "-";
  const std::string id = stanli_build_id();
  expect_true("build id " + id + " starts with " + want,
              id.rfind(want, 0) == 0);
}

void test_bridgestan_data() {
  const std::string mir = slurp("tests/fixtures/conj.tmir.sexp");
  const std::string conj = slurp("tests/fixtures/conj.json");
  const std::string data =
      "{\"__stanli\": {\"build_id\": \"old\"}, "
      "\"spare\": [NaN, Infinity, -Infinity], " +
      conj.substr(conj.find('{') + 1);
  char err[8192]{};
  char* payload = stanli_bridgestan_data(data.c_str(), mir.c_str(), "conj", err,
                                         sizeof err);
  expect_true(std::string("payload builds: ") + err, payload != nullptr);
  if (payload == nullptr) return;
  const std::string text = payload;
  stanli_string_free(payload);
  expect_true("the manifest is the first member",
              text.rfind("{\"__stanli\":", 0) == 0);
  expect_true("one manifest", text.find("__stanli", 3) == std::string::npos);
  expect_true("non-finite values survive",
              text.find("[NaN,Infinity,-Infinity]") != std::string::npos);
  char* bs_err = nullptr;
  bs_model* m = bs_model_construct(text.c_str(), 1, &bs_err);
  expect_true(std::string("bs_model_construct accepts it: ") +
                  (bs_err != nullptr ? bs_err : ""),
              m != nullptr);
  if (m != nullptr) {
    expect_eq("payload name", bs_name(m), "conj");
    bs_model_destruct(m);
  }
  bs_free_error_msg(bs_err);

  expect_true("an array is not data",
              stanli_bridgestan_data("[1]", mir.c_str(), "x", err,
                                     sizeof err) == nullptr &&
                  std::strstr(err, "not a JSON object") != nullptr);
  payload =
      stanli_bridgestan_data(nullptr, mir.c_str(), nullptr, err, sizeof err);
  expect_true("null data is no data",
              payload != nullptr &&
                  std::strstr(payload, "\"name\":\"stanli_model\"") != nullptr);
  stanli_string_free(payload);
}

struct Compilation {
  std::string mir, error;
  std::vector<std::pair<stanli_compiler_message_kind, std::string>> messages;
  bool ok = false;
};

Compilation compile(const std::string& code, const char* filename) {
  Compilation out;
  stanli_compilation* c =
      stanli_compile_stan(code.c_str(), filename, nullptr, 0, 0);
  if (c == nullptr) return out;
  out.ok = stanli_compilation_mir(c) != nullptr;
  if (out.ok) out.mir = stanli_compilation_mir(c);
  if (stanli_compilation_error(c) != nullptr)
    out.error = stanli_compilation_error(c);
  for (int64_t i = 0; i < stanli_compilation_n_messages(c); ++i)
    out.messages.push_back({stanli_compilation_message_kind(c, i),
                            stanli_compilation_message(c, i)});
  stanli_compilation_free(c);
  return out;
}

// Tuple names as CmdStan's generated constrained_param_names and
// unconstrained_param_names spell them: ':' before a slot, '.' before an
// index, a simplex's free values one fewer.
void test_tuple_names() {
  if (!stanli_has_embedded_stanc()) return;
  const Compilation c = compile(
      "parameters { tuple(real, array[2] real<lower=0>) tp;"
      "  tuple(simplex[3], real) ts; }\n"
      "transformed parameters { tuple(real<lower=0>, real) tt ="
      "  (tp.2[1], ts.2); }\n"
      "model { tp.1 ~ normal(0, 1); tp.2 ~ exponential(1);"
      "  ts.2 ~ normal(ts.1[1], 1); }\n"
      "generated quantities { array[2] tuple(real, int) g;"
      "  for (i in 1:2) g[i] = (tp.1 * i, i); }\n",
      "tuples.stan");
  expect_true("tuple model compiles: " + c.error, c.ok);
  if (!c.ok) return;
  char message[256] = "";
  char* payload = stanli_bridgestan_data(nullptr, c.mir.c_str(), "tuples",
                                         message, sizeof message);
  expect_true(std::string("tuple payload: ") + message, payload != nullptr);
  if (payload == nullptr) return;
  char* err = nullptr;
  bs_model* m = bs_model_construct(payload, 1, &err);
  stanli_string_free(payload);
  expect_true(std::string("tuple model constructs: ") + (err ? err : ""),
              m != nullptr);
  bs_free_error_msg(err);
  err = nullptr;
  if (m == nullptr) return;
  expect_eq("tuple bs_param_unc_names", bs_param_unc_names(m),
            "tp:1,tp:2.1,tp:2.2,ts:1.1,ts:1.2,ts:2");
  expect_eq("tuple bs_param_names(F,F)", bs_param_names(m, false, false),
            "tp:1,tp:2.1,tp:2.2,ts:1.1,ts:1.2,ts:1.3,ts:2");
  expect_eq("tuple bs_param_names(T,F)", bs_param_names(m, true, false),
            "tp:1,tp:2.1,tp:2.2,ts:1.1,ts:1.2,ts:1.3,ts:2,tt:1,tt:2");
  expect_eq("tuple bs_param_names(F,T)", bs_param_names(m, false, true),
            "tp:1,tp:2.1,tp:2.2,ts:1.1,ts:1.2,ts:1.3,ts:2,g.1:1,g.1:2,"
            "g.2:1,g.2:2");
  expect_eq("tuple bs_param_names(T,T)", bs_param_names(m, true, true),
            "tp:1,tp:2.1,tp:2.2,ts:1.1,ts:1.2,ts:1.3,ts:2,tt:1,tt:2,g.1:1,"
            "g.1:2,g.2:1,g.2:2");
  // A constrained draw goes back through unconstrain, which reads each
  // tuple leaf by its name.
  const double q[6] = {0.3, -0.2, 0.4, 0.1, -0.5, 0.7};
  double theta[7], back[6];
  const bool ok =
      bs_param_constrain(m, false, false, q, theta, nullptr, &err) == 0 &&
      bs_param_unconstrain(m, theta, back, &err) == 0;
  expect_true(std::string("tuple constrain round trip: ") + (err ? err : ""),
              ok);
  bs_free_error_msg(err);
  for (int i = 0; ok && i < 6; ++i)
    expect_near("tuple unconstrain q[" + std::to_string(i) + "]", back[i],
                q[i]);
  bs_model_destruct(m);
}

void test_named_source_and_warnings() {
  Compilation empty = compile("", "dir/empty.stan");
  if (!stanli_has_embedded_stanc()) {
    expect_true("no embedded stanc is reported",
                !empty.ok && !empty.error.empty());
    return;
  }
  expect_true("empty model compiles: " + empty.error, empty.ok);
  expect_true("an empty model has one warning", empty.messages.size() == 1);
  if (empty.messages.size() == 1) {
    expect_true("the warning is a compiler warning",
                empty.messages[0].first == STANLI_COMPILER_WARNING);
    expect_true("the warning says so",
                empty.messages[0].second.find("Empty model detected") !=
                    std::string::npos);
    expect_true(
        "the warning names the file",
        empty.messages[0].second.find("dir/empty.stan") != std::string::npos);
  }

  Compilation clean = compile("model {}", nullptr);
  expect_true("a clean model has no messages",
              clean.ok && clean.messages.empty());

  const char* broken = "parameters { real x }\nmodel { x ~ normal(0, 1); }\n";
  Compilation bad = compile(broken, "dir/broken.stan");
  expect_true("a syntax error is reported", !bad.ok && bad.mir.empty());
  expect_true("the error names the file: " + bad.error,
              bad.error.find("dir/broken.stan") != std::string::npos &&
                  bad.error.find("'string'") == std::string::npos);

  char err[8192]{};
  stanli_model_opts defaults;
  stanli_model_opts_init(&defaults);
  stanli_model* model = stanli_model_new_from_stan_named(
      "", "dir/empty.stan", "{}", &defaults, err, sizeof err);
  expect_true(std::string("named empty model builds: ") + err,
              model != nullptr);
  if (model) {
    expect_true("model keeps the compiler warning",
                stanli_model_n_compiler_messages(model) == 1 &&
                    stanli_model_compiler_message_kind(model, 0) ==
                        STANLI_COMPILER_WARNING &&
                    std::strstr(stanli_model_compiler_message(model, 0),
                                "dir/empty.stan") != nullptr);
    expect_eq("compiler warnings stay out of stanli_warnings",
              stanli_warnings(model), "");
    stanli_model_free(model);
  }
  model = stanli_model_new_from_stan_named(broken, "dir/broken.stan", "{}",
                                           &defaults, err, sizeof err);
  expect_true(
      "named build reports syntax errors",
      model == nullptr && std::strstr(err, "dir/broken.stan") != nullptr);
}

const char* kAnnotatedHierarchy =
    "data { int<lower=1> J; vector[J] y; }\n"
    "parameters {\n"
    "  @latent vector[J] a;\n"
    "  real mu;\n"
    "  real<lower=0> sigma;\n"
    "}\n"
    "model { a ~ normal(mu, sigma); y ~ normal(a, 1); }\n";

std::string without_annotations(std::string code) {
  const std::string tag = "@latent ";
  for (size_t at = code.find(tag); at != std::string::npos; at = code.find(tag))
    code.erase(at, tag.size());
  return code;
}

void test_declaration_annotations() {
  if (!stanli_has_embedded_stanc()) return;
  const std::string annotated = kAnnotatedHierarchy;
  const std::string plain = without_annotations(annotated);
  expect_true("the plain copy has no annotation",
              plain.find('@') == std::string::npos);

  Compilation known = compile(annotated, "latent.stan");
  expect_true("@latent compiles: " + known.error, known.ok);
  expect_true("@latent does not warn", known.messages.empty());

  std::string with_foo = annotated;
  with_foo.replace(with_foo.find("  real mu;"), 10, "  @foo real mu;");
  Compilation foo = compile(with_foo, "foo.stan");
  expect_true("@foo compiles: " + foo.error, foo.ok);
  expect_true("@foo warns once", foo.messages.size() == 1);
  if (foo.messages.size() == 1) {
    expect_true("the warning is a compiler warning",
                foo.messages[0].first == STANLI_COMPILER_WARNING);
    expect_true("the warning names the annotation: " + foo.messages[0].second,
                foo.messages[0].second.find("Unknown annotation '@foo'") !=
                    std::string::npos);
  }

  const char* data = "{\"J\":3,\"y\":[0.4,-1.2,2.5]}";
  char err[8192]{};
  stanli_model* a = stanli_model_new_from_stan_with_opts(
      annotated.c_str(), data, nullptr, err, sizeof err);
  expect_true(std::string("annotated model builds: ") + err, a != nullptr);
  stanli_model* b = stanli_model_new_from_stan_with_opts(
      plain.c_str(), data, nullptr, err, sizeof err);
  expect_true(std::string("plain model builds: ") + err, b != nullptr);
  if (a && b) {
    expect_true(
        "both models have five unconstrained parameters",
        stanli_n_unconstrained(a) == 5 && stanli_n_unconstrained(b) == 5);
    const double q[5] = {0.3, -0.7, 1.1, 0.2, -0.4};
    double lp_a = 0, lp_b = 0, g_a[5]{}, g_b[5]{};
    expect_true("annotated gradient", stanli_grad(a, q, &lp_a, g_a) == 0);
    expect_true("plain gradient", stanli_grad(b, q, &lp_b, g_b) == 0);
    expect_true("annotations leave the log density bitwise equal",
                lp_a == lp_b);
    for (int i = 0; i < 5; ++i)
      expect_true("annotations leave the gradient bitwise equal",
                  g_a[i] == g_b[i]);
    const int set = STANLI_CONSTRAINED_COLUMNS;
    expect_true("variable a is first",
                std::string(stanli_variable_name(a, set, 0)) == "a");
    expect_true("a has one annotation",
                stanli_variable_n_annotations(a, set, 0) == 1);
    const char* note = stanli_variable_annotation(a, set, 0, 0);
    expect_true("a is annotated latent",
                note != nullptr && std::string(note) == "latent");
    expect_true("no annotation past the end",
                stanli_variable_annotation(a, set, 0, 1) == nullptr);
    expect_true("mu is unannotated",
                stanli_variable_n_annotations(a, set, 1) == 0);
    expect_true("out-of-range variable has no annotations",
                stanli_variable_n_annotations(a, set, 9) == 0);
    for (int64_t i = 0; i < stanli_n_variables(b, set); ++i)
      expect_true("plain model has no annotations",
                  stanli_variable_n_annotations(b, set, i) == 0);
  }
  stanli_model_free(a);
  stanli_model_free(b);
}

// A model past the structural budget compiles from O0 MIR, and the notice
// comes back as its own kind of message.
void test_o1_fallback_message() {
  if (!stanli_has_embedded_stanc()) return;
  std::string body;
  for (int i = 0; i < 2100; ++i) body += "s += 1;\n";
  std::string code = "parameters { real x; }\nmodel {\nreal s = 0;\n";
  for (int d = 0; d < 9; ++d)
    code += "for (i" + std::to_string(d) + " in 1:1) {\n";
  code += body;
  for (int d = 0; d < 9; ++d) code += "}\n";
  code += "x ~ normal(s, 1);\n}\n";
  const Compilation c = compile(code, nullptr);
  expect_true("budget model compiles: " + c.error, c.ok);
  bool found = false;
  for (const auto& m : c.messages)
    if (m.first == STANLI_COMPILER_O1_FALLBACK)
      found = m.second.find("skipped O1") != std::string::npos;
  expect_true("the O1 fallback is a typed message", found);
}

void test_variables_and_layouts() {
  const std::string mir = slurp("tests/fixtures/amatwa.tmir.sexp");
  char err[8192]{};
  stanli_model* m = stanli_model_new_with_opts(
      mir.c_str(), slurp("tests/fixtures/amatwa.json").c_str(), nullptr, err,
      sizeof err);
  expect_true(std::string("amatwa builds: ") + err, m != nullptr);
  if (m != nullptr) {
    expect_true("one constrained variable",
                stanli_n_variables(m, STANLI_CONSTRAINED_COLUMNS) == 1);
    expect_eq("its name",
              stanli_variable_name(m, STANLI_CONSTRAINED_COLUMNS, 0), "m");
    std::vector<int64_t> dims;
    for (int64_t k = 0;
         k < stanli_variable_n_dims(m, STANLI_CONSTRAINED_COLUMNS, 0); ++k)
      dims.push_back(stanli_variable_dim(m, STANLI_CONSTRAINED_COLUMNS, 0, k));
    expect_true("its dims are array then matrix",
                dims == std::vector<int64_t>({2, 2, 3}));
    expect_true("an unknown set has no variables",
                stanli_n_variables(m, 7) == 0 &&
                    stanli_variable_name(m, 7, 0) == nullptr);
    stanli_model_free(m);
  }

  bool seen_resume_out = false, sizes_agree = true;
  const stanli_field_layout* fields = stanli_field_layouts();
  for (int64_t i = 0; i < stanli_n_field_layouts(); ++i) {
    const std::string type = fields[i].type, field = fields[i].field;
    if (type == "stanli_sample_opts" && field == "resume_out")
      seen_resume_out =
          fields[i].offset == offsetof(stanli_sample_opts, resume_out) &&
          fields[i].size == sizeof(void*);
    if (type == "stanli_sample_opts")
      sizes_agree =
          sizes_agree && fields[i].type_size == sizeof(stanli_sample_opts);
  }
  expect_true("the layout table describes stanli_sample_opts",
              seen_resume_out && sizes_agree);

  expect_true(
      "summary stat names follow the enum",
      stanli_n_summary_stats() == STANLI_N_SUMMARY_STATS &&
          std::string(stanli_summary_stat_name(STANLI_STAT_RHAT)) == "r_hat" &&
          std::string(stanli_summary_stat_name(STANLI_STAT_Q50)) == "q50" &&
          stanli_summary_stat_name(STANLI_N_SUMMARY_STATS) == nullptr);
  expect_true("sampler column count",
              stanli_n_sampler_columns() == STANLI_N_SAMPLER_COLS);
}

// A run that cannot start says so with -1, distinct from a count of failed
// chains.
void test_sample_setup_error() {
  const std::string mir = slurp("tests/fixtures/conj.tmir.sexp");
  char err[8192]{};
  stanli_model* m = stanli_model_new_with_opts(
      mir.c_str(), slurp("tests/fixtures/conj.json").c_str(), nullptr, err,
      sizeof err);
  if (m == nullptr) {
    expect_true(std::string("conj builds: ") + err, false);
    return;
  }
  stanli_sample_opts opts;
  stanli_sample_opts_init(&opts);
  opts.chains = 1;
  std::vector<double> draws(1);
  expect_true(
      "negative refresh is a setup error",
      stanli_sample_multi_metric(m, &opts, nullptr, -1, draws.data(), nullptr,
                                 nullptr, nullptr, nullptr, nullptr, nullptr,
                                 nullptr, nullptr, err, sizeof err) == -1);
  expect_true(
      "null options are a setup error",
      stanli_sample_multi_metric(m, nullptr, nullptr, 0, draws.data(), nullptr,
                                 nullptr, nullptr, nullptr, nullptr, nullptr,
                                 nullptr, nullptr, err, sizeof err) == -1);
  stanli_model_free(m);
}

int main() {
  test_build_id_names_the_abi();
  test_o1_fallback_message();
  test_variables_and_layouts();
  test_sample_setup_error();
  test_bridgestan_data();
  test_named_source_and_warnings();
  test_tuple_names();
  test_declaration_annotations();
  test_fast_math_options();
  test_source_include_paths();
  test_reduce_sum_threads();
  test_transformed_data_seed();
  expect_interpreter_policy("tests/fixtures/wanames.tmir.sexp", nullptr);
  expect_interpreter_policy("tests/fixtures/wa_literal_write.tmir.sexp",
                            nullptr);
  expect_interpreter_policy("tests/fixtures/interp_fallback_region.tmir.sexp",
                            "oversized shape");
  expect_interpreter_policy("tests/fixtures/interp_fallback_ode.tmir.sexp",
                            "the ODE right-hand side rhs");
  test_interpreter_probe_failure();
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

  expect_invalid_data_rejected();
  expect_runtime_bound_rejected();
  expect_compiled_scalar_rng();
  expect_pathfinder();
  expect_sampling_progress();
  expect_sampling_progress_counts();
  expect_inits_run_in_parallel();
  expect_interruptible_sampling();
  expect_in_worker_write_array();
  expect_metric_options();
  expect_resume();
  expect_sampling_messages();
  expect_captured_messages();
  expect_sampling_warnings_api();
  expect_streaming_stats();
  expect_sample_options_validated();
  expect_summary_stats();
  expect_diagnose_text();

  if (failures == 0) std::printf("test_capi OK\n");
  return failures == 0 ? 0 : 1;
}
