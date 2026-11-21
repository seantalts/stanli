// fixed_param, Laplace, Pathfinder and ADVI through the shipped C ABI.
//
// The posterior in tests/fixtures/alg_gauss.stan is Gaussian on the
// unconstrained scale: theta ~ multi_normal(m, S) with correlation 0.8, and
// log(tau) ~ normal(0, 0.5). So Laplace, Pathfinder and full-rank ADVI
// should reproduce it up to Monte Carlo error, and mean-field ADVI should be
// wrong in the way a diagonal family is wrong about a correlated target.
//
// Tolerances. With S draws the standard error of a mean is sd / sqrt(S) and
// of a standard deviation about sd / sqrt(2 S). The exact methods are held
// to five standard errors. ADVI is stochastic optimization stopped by a
// relative-ELBO rule, so its own error dominates and its bands are wider;
// each is stated where it is used.
#include <stanli/capi.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void expect(const std::string& what, bool ok) {
  if (!ok) {
    ++failures;
    std::printf("FAIL %s\n", what.c_str());
  }
}

void expect_near(const std::string& what, double got, double want, double tol) {
  if (!(std::fabs(got - want) <= tol)) {
    ++failures;
    std::printf("FAIL %-34s got %.6g want %.6g +- %.3g\n", what.c_str(), got,
                want, tol);
  }
}

std::string slurp(const std::string& path) {
  std::ifstream in(path);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

stanli_model* load(const std::string& stem) {
  const std::string mir = slurp("tests/fixtures/" + stem + ".tmir.sexp");
  const std::string data = slurp("tests/fixtures/" + stem + ".json");
  char err[4096]{};
  stanli_model* m = stanli_model_new_with_opts(mir.c_str(), data.c_str(),
                                               nullptr, err, sizeof err);
  if (m == nullptr) {
    ++failures;
    std::printf("FAIL loading %s: %s\n", stem.c_str(), err);
  }
  return m;
}

struct Moments {
  double mean = 0;
  double sd = 0;
};

// Column `col` of a row-major table, optionally through log().
Moments moments(const std::vector<double>& values, int64_t rows, int64_t width,
                int64_t col, bool take_log = false) {
  Moments out;
  for (int64_t i = 0; i < rows; ++i) {
    const double v = values[(size_t)(i * width + col)];
    out.mean += take_log ? std::log(v) : v;
  }
  out.mean /= (double)rows;
  for (int64_t i = 0; i < rows; ++i) {
    double v = values[(size_t)(i * width + col)];
    if (take_log) v = std::log(v);
    out.sd += (v - out.mean) * (v - out.mean);
  }
  out.sd = std::sqrt(out.sd / (double)(rows - 1));
  return out;
}

double correlation(const std::vector<double>& values, int64_t rows,
                   int64_t width, int64_t a, int64_t b) {
  const Moments ma = moments(values, rows, width, a);
  const Moments mb = moments(values, rows, width, b);
  double cov = 0;
  for (int64_t i = 0; i < rows; ++i)
    cov += (values[(size_t)(i * width + a)] - ma.mean) *
           (values[(size_t)(i * width + b)] - mb.mean);
  return cov / (double)(rows - 1) / (ma.sd * mb.sd);
}

bool all_finite(const std::vector<double>& v) {
  for (double x : v)
    if (!std::isfinite(x)) return false;
  return true;
}

// alg_gauss columns: theta[1], theta[2], tau, tau2, noise.
constexpr int64_t kWidth = 5;
constexpr double kMean1 = 1.0, kMean2 = -2.0, kRho = 0.8, kLogTauSd = 0.5;

// The Gaussian's moments, each held to `k` standard errors for `rows` draws.
void expect_gaussian(const std::string& tag, const std::vector<double>& values,
                     int64_t rows, double k) {
  const double se = k / std::sqrt((double)rows);
  const double se_sd = k / std::sqrt(2.0 * (double)rows);
  const Moments t1 = moments(values, rows, kWidth, 0);
  const Moments t2 = moments(values, rows, kWidth, 1);
  const Moments lt = moments(values, rows, kWidth, 2, true);
  const Moments noise = moments(values, rows, kWidth, 4);
  expect_near(tag + " mean theta[1]", t1.mean, kMean1, se);
  expect_near(tag + " mean theta[2]", t2.mean, kMean2, se);
  expect_near(tag + " sd theta[1]", t1.sd, 1.0, se_sd);
  expect_near(tag + " sd theta[2]", t2.sd, 1.0, se_sd);
  expect_near(tag + " mean log tau", lt.mean, 0.0, kLogTauSd * se);
  expect_near(tag + " sd log tau", lt.sd, kLogTauSd, kLogTauSd * se_sd);
  // The correlation's standard error is (1 - rho^2) / sqrt(S).
  expect_near(tag + " cor theta", correlation(values, rows, kWidth, 0, 1), kRho,
              (1 - kRho * kRho) * se);
  // noise = normal_rng(theta[1], 1): variance 1 + 1.
  expect_near(tag + " mean noise", noise.mean, kMean1, std::sqrt(2.0) * se);
  expect_near(tag + " sd noise", noise.sd, std::sqrt(2.0),
              std::sqrt(2.0) * se_sd);
  // tau2 is square(tau), exactly, on every row.
  bool squares = true;
  for (int64_t i = 0; i < rows; ++i) {
    const double tau = values[(size_t)(i * kWidth + 2)];
    squares &= values[(size_t)(i * kWidth + 3)] == tau * tau;
  }
  expect(tag + " tau2 is square(tau)", squares);
}

struct Log {
  int info = 0, warnings = 0, errors = 0;
  std::string text;
};
void on_log(int32_t level, const char* text, void* user) {
  Log* log = static_cast<Log*>(user);
  if (level == 0) ++log->info;
  if (level == 1) ++log->warnings;
  if (level == 2) ++log->errors;
  log->text += text;
  log->text += "\n";
}

int stop_now(void* user) {
  ++*static_cast<int*>(user);
  return 1;
}

void test_fixed_param(stanli_model* m) {
  char err[4096]{};
  stanli_fixed_param_opts o;
  stanli_fixed_param_opts_init(&o);
  expect("fixed_param defaults",
         o.seed == 1 && o.chains == 4 && o.chain_id == 1 && o.samples == 1000 &&
             o.thin == 1 && o.init_radius == 2.0 && o.inits == nullptr);
  o.chains = 2;
  o.samples = 2000;
  o.seed = 4711;
  // theta = (0.3, -0.7), tau = exp(log 2) = 2, in both chains.
  const double q[6] = {0.3, -0.7, std::log(2.0), 0.3, -0.7, std::log(2.0)};
  o.inits = q;
  const int64_t rows = stanli_fixed_param_n_draws(&o);
  expect("fixed_param row count", rows == 2000);
  std::vector<double> values((size_t)(2 * rows * kWidth));
  int interrupted = -1;
  double seconds[2] = {-1, -1};
  int rc = stanli_fixed_param(m, &o, values.data(), seconds, nullptr, nullptr,
                              nullptr, nullptr, &interrupted, err, sizeof err);
  expect(std::string("fixed_param runs: ") + err, rc == 0 && interrupted == 0);
  expect("fixed_param times each chain", seconds[0] >= 0 && seconds[1] >= 0);
  bool fixed = true;
  for (int64_t i = 0; i < 2 * rows; ++i) {
    const double* row = values.data() + i * kWidth;
    fixed &= row[0] == 0.3 && row[1] == -0.7 &&
             std::fabs(row[2] - 2.0) < 1e-15 && std::fabs(row[3] - 4.0) < 1e-14;
  }
  expect("fixed_param parameters equal the init on every row", fixed);
  // noise = normal_rng(0.3, 1) varies; five standard errors over 4000 draws.
  const Moments noise = moments(values, 2 * rows, kWidth, 4);
  expect_near("fixed_param mean noise", noise.mean, 0.3, 5 / std::sqrt(4000.0));
  expect_near("fixed_param sd noise", noise.sd, 1.0, 5 / std::sqrt(8000.0));
  expect("fixed_param chains draw from different streams",
         values[4] != values[(size_t)(rows * kWidth + 4)]);

  // The same seed is the same run; another seed is another.
  std::vector<double> again(values.size());
  rc = stanli_fixed_param(m, &o, again.data(), nullptr, nullptr, nullptr,
                          nullptr, nullptr, nullptr, err, sizeof err);
  expect("fixed_param is reproducible", rc == 0 && again == values);
  o.seed = 4712;
  rc = stanli_fixed_param(m, &o, again.data(), nullptr, nullptr, nullptr,
                          nullptr, nullptr, nullptr, err, sizeof err);
  expect("fixed_param follows the seed", rc == 0 && again != values);

  // Random starting points: fixed within a chain, different across chains,
  // inside the constrained image of (-2, 2).
  o.inits = nullptr;
  o.samples = 10;
  o.thin = 3;
  expect("fixed_param thinned row count", stanli_fixed_param_n_draws(&o) == 4);
  std::vector<double> thin((size_t)(2 * 4 * kWidth));
  rc = stanli_fixed_param(m, &o, thin.data(), nullptr, nullptr, nullptr,
                          nullptr, nullptr, nullptr, err, sizeof err);
  expect(std::string("fixed_param random init: ") + err, rc == 0);
  bool constant = true;
  for (int c = 0; c < 2; ++c)
    for (int i = 1; i < 4; ++i)
      for (int j = 0; j < 4; ++j)
        constant &= thin[(size_t)((c * 4 + i) * kWidth + j)] ==
                    thin[(size_t)(c * 4 * kWidth + j)];
  expect("fixed_param random init stays put", constant);
  expect("fixed_param chains start apart",
         thin[0] != thin[(size_t)(4 * kWidth)]);
  expect("fixed_param random init within radius",
         std::fabs(thin[0]) <= 2.0 && std::fabs(thin[1]) <= 2.0 &&
             thin[2] >= std::exp(-2.0) && thin[2] <= std::exp(2.0));

  // Errors.
  rc = stanli_fixed_param(m, nullptr, thin.data(), nullptr, nullptr, nullptr,
                          nullptr, nullptr, nullptr, err, sizeof err);
  expect("fixed_param refuses null options", rc != 0 && err[0] != '\0');
  o.thin = 0;
  rc = stanli_fixed_param(m, &o, thin.data(), nullptr, nullptr, nullptr,
                          nullptr, nullptr, nullptr, err, sizeof err);
  expect("fixed_param refuses thin 0",
         rc != 0 && std::strstr(err, "thin") != nullptr);
}

void test_noparam() {
  stanli_model* m = load("alg_noparam");
  if (m == nullptr) return;
  char err[4096]{};
  expect("noparam has no parameters", stanli_n_unconstrained(m) == 0);
  const int64_t width = stanli_wa_n_columns(m);
  expect("noparam has two columns", width == 2);
  stanli_fixed_param_opts o;
  stanli_fixed_param_opts_init(&o);
  o.chains = 1;
  o.samples = 4000;
  o.seed = 99;
  std::vector<double> values((size_t)(4000 * width));
  const int rc =
      stanli_fixed_param(m, &o, values.data(), nullptr, nullptr, nullptr,
                         nullptr, nullptr, nullptr, err, sizeof err);
  expect(std::string("noparam runs: ") + err, rc == 0);
  if (rc == 0 && width == 2) {
    // y = normal_rng(5, 2), k = poisson_rng(3); five standard errors.
    const Moments y = moments(values, 4000, width, 0);
    const Moments k = moments(values, 4000, width, 1);
    expect_near("noparam mean y", y.mean, 5.0, 5 * 2 / std::sqrt(4000.0));
    expect_near("noparam sd y", y.sd, 2.0, 5 * 2 / std::sqrt(8000.0));
    expect_near("noparam mean k", k.mean, 3.0,
                5 * std::sqrt(3.0) / std::sqrt(4000.0));
  }
  stanli_model_free(m);
}

// The posterior mode on the unconstrained scale: theta at m, and log(tau)
// at 0, the mode of normal(0, 0.5).
std::vector<double> find_mode(stanli_model* m) {
  char err[4096]{};
  stanli_optimize_opts o;
  stanli_optimize_opts_init(&o);
  o.seed = 5;
  o.jacobian = 1;
  std::vector<double> q(3), values((size_t)kWidth);
  double lp = 0;
  const int rc =
      stanli_optimize(m, &o, q.data(), values.data(), &lp, err, sizeof err);
  expect(std::string("mode found: ") + err, rc == 0);
  expect_near("mode theta[1]", q[0], kMean1, 1e-4);
  expect_near("mode theta[2]", q[1], kMean2, 1e-4);
  expect_near("mode log tau", q[2], 0.0, 1e-4);
  double model_lp = 0;
  std::vector<double> grad(3);
  expect("model lp at the mode",
         stanli_grad(m, q.data(), &model_lp, grad.data()) == 0);
  expect_near("reported lp is the model's lp", lp, model_lp, 1e-9);

  // One iteration cannot reach the mode from a random start, so these see
  // whether the starting point reached the optimizer.
  o.iter = 1;
  std::vector<double> again(3), zero(3, 0.0), from_zero(3);
  o.init = q.data();
  stanli_optimize(m, &o, again.data(), values.data(), &lp, err, sizeof err);
  for (int i = 0; i < 3; ++i)
    expect_near("optimize is stable at the mode", again[(size_t)i],
                q[(size_t)i], 1e-6);
  o.init = zero.data();
  stanli_optimize(m, &o, from_zero.data(), values.data(), &lp, err, sizeof err);
  o.init = nullptr;
  o.init_radius = 0.0;
  stanli_optimize(m, &o, again.data(), values.data(), &lp, err, sizeof err);
  expect("init_radius = 0 starts at the origin", again == from_zero);
  return q;
}

void test_optimize_options(stanli_model* m) {
  char err[4096]{};
  std::vector<double> q(3), values((size_t)kWidth);
  double lp = 0;
  for (int bad_iter : {0, -5}) {
    stanli_optimize_opts o;
    stanli_optimize_opts_init(&o);
    o.iter = bad_iter;
    err[0] = '\0';
    const int rc =
        stanli_optimize(m, &o, q.data(), values.data(), &lp, err, sizeof err);
    expect("optimize rejects iter < 1",
           rc != 0 && std::string(err) == "iter must be an integer >= 1");
  }
  stanli_optimize_opts o;
  stanli_optimize_opts_init(&o);
  o.init_radius = -1.0;
  err[0] = '\0';
  const int rc =
      stanli_optimize(m, &o, q.data(), values.data(), &lp, err, sizeof err);
  expect("optimize rejects a negative init_radius",
         rc != 0 &&
             std::string(err) == "init_radius must be a finite number >= 0");
}

stanli_model* from_stan(const char* code) {
  if (!stanli_has_embedded_stanc()) return nullptr;
  char err[4096]{};
  stanli_model* m = stanli_model_new_from_stan_with_opts(code, "{}", nullptr,
                                                         err, sizeof err);
  if (m == nullptr) {
    ++failures;
    std::printf("FAIL compiling a test model: %s\n", err);
  }
  return m;
}

// CmdStan's optimize draws the row at the mode from the stream that chose
// the start, create_rng(seed, chain_id), as fixed_param draws its first row.
void test_optimize_generated_quantities_stream() {
  stanli_model* m = from_stan(
      "parameters { real mu; } model { mu ~ normal(0, 1); }\n"
      "generated quantities { real y = normal_rng(0, 1); }\n");
  if (m == nullptr) return;
  const auto y_at_mode = [m](uint32_t seed, int chain_id) {
    stanli_optimize_opts o;
    stanli_optimize_opts_init(&o);
    o.seed = seed;
    o.chain_id = chain_id;
    double q = 0, lp = 0, values[2] = {0, 0};
    char err[4096]{};
    const int rc = stanli_optimize(m, &o, &q, values, &lp, err, sizeof err);
    expect(std::string("optimize runs: ") + err, rc == 0);
    return values[1];
  };
  const auto y_fixed_param = [m](uint32_t seed, int chain_id) {
    stanli_fixed_param_opts o;
    stanli_fixed_param_opts_init(&o);
    o.seed = seed;
    o.chains = 1;
    o.chain_id = chain_id;
    o.samples = 1;
    double values[2] = {0, 0};
    char err[4096]{};
    const int rc =
        stanli_fixed_param(m, &o, values, nullptr, nullptr, nullptr, nullptr,
                           nullptr, nullptr, err, sizeof err);
    expect(std::string("fixed_param runs: ") + err, rc == 0);
    return values[1];
  };
  const double y5 = y_at_mode(5, 1);
  expect("optimize generated quantities repeat for one seed",
         y_at_mode(5, 1) == y5);
  expect("optimize generated quantities follow the seed",
         y_at_mode(6, 1) != y5);
  expect("optimize generated quantities follow the chain id",
         y_at_mode(5, 2) != y5);
  for (int chain_id : {1, 2})
    expect("optimize draws from the (seed, chain_id) stream of fixed_param",
           y_at_mode(5, chain_id) == y_fixed_param(5, chain_id));
  // A supplied start still moves the stream on by one draw per parameter,
  // as CmdStan's initialize does; fixed_param runs that initialize itself.
  {
    const double start = 0.25;
    stanli_optimize_opts o;
    stanli_optimize_opts_init(&o);
    o.seed = 5;
    o.init = &start;
    double q = 0, lp = 0, values[2] = {0, 0};
    char err[4096]{};
    stanli_optimize(m, &o, &q, values, &lp, err, sizeof err);
    stanli_fixed_param_opts f;
    stanli_fixed_param_opts_init(&f);
    f.seed = 5;
    f.chains = 1;
    f.samples = 1;
    f.inits = &start;
    double row[2] = {0, 0};
    stanli_fixed_param(m, &f, row, nullptr, nullptr, nullptr, nullptr, nullptr,
                       nullptr, err, sizeof err);
    expect("a supplied start leaves optimize on fixed_param's stream",
           values[1] == row[1] && row[0] == start);
  }
  double q = 0.3, row[2];
  stanli_wa_row(m, &q, row);
  expect("optimize ignores the model's own stream", y_at_mode(5, 1) == y5);
  stanli_model_free(m);
}

// Each options struct has one check, which its entry point applies first,
// with CmdStan's argument rules and one wording.
template <class Opts>
void expect_refused(const char* what, int (*check)(const Opts*, char*, size_t),
                    void (*init)(Opts*), void (*mutate)(Opts&),
                    const std::string& message,
                    const std::function<int(const Opts&, char*, size_t)>& run) {
  Opts o;
  init(&o);
  char err[4096]{};
  expect(std::string(what) + ": defaults pass",
         check(&o, err, sizeof err) == 0);
  mutate(o);
  err[0] = '\0';
  expect(
      std::string(what) + ": check says '" + message + "', got '" + err + "'",
      check(&o, err, sizeof err) != 0 && err == message);
  err[0] = '\0';
  const int rc = run(o, err, sizeof err);
  expect(std::string(what) + ": entry point says '" + message + "', got '" +
             err + "'",
         rc != 0 && err == message);
}

void test_option_checks(stanli_model* m) {
  const int64_t n = stanli_n_unconstrained(m);
  std::vector<double> q((size_t)n), values(100000), lp(100000);
  double one_lp = 0;
  const auto optimize = [&](const stanli_optimize_opts& o, char* err,
                            size_t len) {
    return stanli_optimize(m, &o, q.data(), values.data(), &one_lp, err, len);
  };
  using O = stanli_optimize_opts;
  const auto opt = [&](const char* what, void (*mutate)(O&),
                       const std::string& message) {
    expect_refused<O>(what, stanli_optimize_opts_check,
                      stanli_optimize_opts_init, mutate, message, optimize);
  };
  opt(
      "optimize chain_id", [](O& o) { o.chain_id = -1; },
      "chain_id must be an integer >= 0");
  opt(
      "optimize iter", [](O& o) { o.iter = 0; },
      "iter must be an integer >= 1");
  opt(
      "optimize history_size", [](O& o) { o.history_size = 0; },
      "history_size must be an integer >= 1");
  opt(
      "optimize init_alpha", [](O& o) { o.init_alpha = 0; },
      "init_alpha must be a finite number > 0");
  opt(
      "optimize tol_obj", [](O& o) { o.tol_obj = -1; },
      "tol_obj must be a finite number > 0");
  opt(
      "optimize tol_rel_obj", [](O& o) { o.tol_rel_obj = 0; },
      "tol_rel_obj must be a finite number > 0");
  opt(
      "optimize tol_grad", [](O& o) { o.tol_grad = 0; },
      "tol_grad must be a finite number > 0");
  opt(
      "optimize tol_rel_grad", [](O& o) { o.tol_rel_grad = 0; },
      "tol_rel_grad must be a finite number > 0");
  opt(
      "optimize tol_param", [](O& o) { o.tol_param = 0; },
      "tol_param must be a finite number > 0");
  opt(
      "optimize init_radius", [](O& o) { o.init_radius = -1; },
      "init_radius must be a finite number >= 0");

  using F = stanli_fixed_param_opts;
  const auto fixed = [&](const F& o, char* err, size_t len) {
    return stanli_fixed_param(m, &o, values.data(), nullptr, nullptr, nullptr,
                              nullptr, nullptr, nullptr, err, len);
  };
  const auto fp = [&](const char* what, void (*mutate)(F&),
                      const std::string& message) {
    expect_refused<F>(what, stanli_fixed_param_opts_check,
                      stanli_fixed_param_opts_init, mutate, message, fixed);
  };
  fp(
      "fixed_param chains", [](F& o) { o.chains = 0; },
      "chains must be an integer >= 1");
  fp(
      "fixed_param chain_id", [](F& o) { o.chain_id = -1; },
      "chain_id must be an integer >= 0");
  fp(
      "fixed_param samples", [](F& o) { o.samples = -1; },
      "samples must be an integer >= 0");
  fp(
      "fixed_param thin", [](F& o) { o.thin = 0; },
      "thin must be an integer >= 1");
  fp(
      "fixed_param init_radius", [](F& o) { o.init_radius = -1; },
      "init_radius must be a finite number >= 0");
  fp(
      "fixed_param refresh", [](F& o) { o.refresh = -1; },
      "refresh must be an integer >= 0");

  using L = stanli_laplace_opts;
  const auto laplace = [&](const L& o, char* err, size_t len) {
    return stanli_laplace_sample(m, &o, q.data(), values.data(), nullptr,
                                 nullptr, nullptr, nullptr, nullptr, nullptr,
                                 nullptr, err, len);
  };
  expect_refused<L>(
      "laplace draws", stanli_laplace_opts_check, stanli_laplace_opts_init,
      [](L& o) { o.draws = 0; }, "draws must be an integer >= 1", laplace);
  expect_refused<L>(
      "laplace refresh", stanli_laplace_opts_check, stanli_laplace_opts_init,
      [](L& o) { o.refresh = -1; }, "refresh must be an integer >= 0", laplace);

  using P = stanli_pathfinder_opts;
  const auto path = [&](const P& o, char* err, size_t len) {
    int64_t rows = 0;
    return stanli_pathfinder(m, &o, values.data(), nullptr, nullptr, nullptr,
                             &rows, nullptr, nullptr, nullptr, nullptr, nullptr,
                             err, len);
  };
  const auto pf = [&](const char* what, void (*mutate)(P&),
                      const std::string& message) {
    expect_refused<P>(what, stanli_pathfinder_opts_check,
                      stanli_pathfinder_opts_init, mutate, message, path);
  };
  pf(
      "pathfinder chain_id", [](P& o) { o.chain_id = -1; },
      "chain_id must be an integer >= 0");
  pf(
      "pathfinder num_paths", [](P& o) { o.num_paths = 0; },
      "num_paths must be an integer >= 1");
  pf(
      "pathfinder num_draws", [](P& o) { o.num_draws = 0; },
      "num_draws must be an integer >= 1");
  pf(
      "pathfinder num_elbo_draws", [](P& o) { o.num_elbo_draws = 0; },
      "num_elbo_draws must be an integer >= 1");
  pf(
      "pathfinder max_lbfgs_iters", [](P& o) { o.max_lbfgs_iters = 0; },
      "max_lbfgs_iters must be an integer >= 1");
  pf(
      "pathfinder history_size", [](P& o) { o.history_size = 0; },
      "history_size must be an integer >= 1");
  pf(
      "pathfinder init_alpha", [](P& o) { o.init_alpha = 0; },
      "init_alpha must be a finite number > 0");
  pf(
      "pathfinder tol_obj", [](P& o) { o.tol_obj = 0; },
      "tol_obj must be a finite number > 0");
  pf(
      "pathfinder init_radius", [](P& o) { o.init_radius = -1; },
      "init_radius must be a finite number >= 0");
  pf(
      "pathfinder refresh", [](P& o) { o.refresh = -1; },
      "refresh must be an integer >= 0");

  using V = stanli_variational_opts;
  const auto advi = [&](const V& o, char* err, size_t len) {
    return stanli_variational(m, &o, nullptr, values.data(), nullptr, nullptr,
                              nullptr, nullptr, nullptr, nullptr, nullptr, err,
                              len);
  };
  const auto va = [&](const char* what, void (*mutate)(V&),
                      const std::string& message) {
    expect_refused<V>(what, stanli_variational_opts_check,
                      stanli_variational_opts_init, mutate, message, advi);
  };
  va(
      "variational chain_id", [](V& o) { o.chain_id = -1; },
      "chain_id must be an integer >= 0");
  va(
      "variational algorithm", [](V& o) { o.algorithm = 9; },
      "algorithm must be STANLI_ADVI_MEANFIELD or STANLI_ADVI_FULLRANK");
  va(
      "variational iter", [](V& o) { o.iter = 0; },
      "iter must be an integer >= 1");
  va(
      "variational grad_samples", [](V& o) { o.grad_samples = 0; },
      "grad_samples must be an integer >= 1");
  va(
      "variational elbo_samples", [](V& o) { o.elbo_samples = 0; },
      "elbo_samples must be an integer >= 1");
  va(
      "variational eta", [](V& o) { o.eta = 0; },
      "eta must be a finite number > 0");
  va(
      "variational adapt_iter", [](V& o) { o.adapt_iter = 0; },
      "adapt_iter must be an integer >= 1");
  va(
      "variational tol_rel_obj", [](V& o) { o.tol_rel_obj = 0; },
      "tol_rel_obj must be a finite number > 0");
  va(
      "variational eval_elbo", [](V& o) { o.eval_elbo = 0; },
      "eval_elbo must be an integer >= 1");
  va(
      "variational output_samples", [](V& o) { o.output_samples = 0; },
      "output_samples must be an integer >= 1");
  va(
      "variational init_radius", [](V& o) { o.init_radius = -1; },
      "init_radius must be a finite number >= 0");

  std::vector<double> inits((size_t)(4 * n));
  char err[4096]{};
  expect("pathfinder inits refuse chain_id -1",
         stanli_pathfinder_inits(m, 1, -1, 4, 1000, 25, 5, 2.0, inits.data(),
                                 nullptr, nullptr, err, sizeof err) != 0 &&
             std::string(err) == "chain_id must be an integer >= 0");
}

// CmdStan accepts id=0 and uses its stream; nothing turns 0 into 1.
// The reference values are CmdStan 2.40's `optimize random seed=S id=I` on
// this model (y in output.csv).
void test_optimize_chain_ids() {
  stanli_model* m = from_stan(
      "parameters { real mu; } model { mu ~ normal(0, 1); }\n"
      "generated quantities { real y = normal_rng(0, 1); }\n");
  if (m == nullptr) return;
  const auto y_at_mode = [m](uint32_t seed, int chain_id) {
    stanli_optimize_opts o;
    stanli_optimize_opts_init(&o);
    o.seed = seed;
    o.chain_id = chain_id;
    double q = 0, lp = 0, values[2] = {0, 0};
    char err[4096]{};
    const int rc = stanli_optimize(m, &o, &q, values, &lp, err, sizeof err);
    expect(std::string("optimize runs: ") + err, rc == 0);
    return values[1];
  };
  expect_near("CmdStan seed 5 id 1", y_at_mode(5, 1), -1.4395515, 5e-8);
  expect_near("CmdStan seed 5 id 2", y_at_mode(5, 2), -0.14566454, 5e-9);
  expect_near("CmdStan seed 6 id 1", y_at_mode(6, 1), 0.59328452, 5e-9);
  expect("chain id 0 is its own stream", y_at_mode(5, 0) != y_at_mode(5, 1));
  stanli_model_free(m);
}

// CmdStan validates a supplied start for optimize as for sampling: one try,
// and a start with no finite log density is refused.
void test_optimize_validates_init() {
  stanli_model* m = from_stan(
      "parameters { real<lower=0> s; } model { target += log(s - 1); }\n");
  if (m == nullptr) return;
  stanli_optimize_opts o;
  stanli_optimize_opts_init(&o);
  const double init = std::log(0.5);
  o.init = &init;
  double q = 0, lp = 0, value = 0;
  char err[4096]{};
  const int rc = stanli_optimize(m, &o, &q, &value, &lp, err, sizeof err);
  expect(std::string("optimize refuses a start outside the support: ") + err,
         rc != 0 && std::string(err).find("initialization failed: the "
                                          "supplied unconstrained init") == 0);
  stanli_model_free(m);
}

// A poll that asks to stop on its (k + 1)-th call. Each call takes longer
// than the 100 ms between polls, so a run that keeps going is asked again at
// its next opportunity.
struct StopAfter {
  int k = 0;
  int calls = 0;
};

int stop_after(void* user) {
  auto* s = static_cast<StopAfter*>(user);
  std::this_thread::sleep_for(std::chrono::milliseconds(110));
  return s->calls++ >= s->k ? 1 : 0;
}

int never_stop(void* user) {
  ++static_cast<StopAfter*>(user)->calls;
  return 0;
}

// Optimize, Pathfinder inits and the Hessian stop when the poll says so.
void test_interruptible_estimates() {
  // A long narrow valley: L-BFGS needs many iterations and the Hessian many
  // gradients, so neither finishes between two polls.
  stanli_model* m = from_stan(
      "parameters { vector[200] x; }\n"
      "model { for (i in 2:200) target += -100 * square(x[i] - square(x[i - "
      "1])) - square(1 - x[i - 1]); }\n");
  if (m == nullptr) return;
  const int64_t n = 200;
  std::vector<double> q((size_t)n), hess((size_t)(n * n)), values((size_t)n),
      inits((size_t)(4 * n));
  double lp = 0;
  char err[4096]{};
  stanli_optimize_opts o;
  stanli_optimize_opts_init(&o);
  o.tol_obj = o.tol_rel_obj = o.tol_grad = o.tol_rel_grad = o.tol_param =
      1e-300;
  o.iter = 100000;
  for (int k : {0, 2}) {
    StopAfter stop{k};
    int interrupted = 0;
    const int rc = stanli_optimize_with_poll(m, &o, q.data(), values.data(),
                                             &lp, stop_after, &stop,
                                             &interrupted, err, sizeof err);
    expect("optimize stops on request after " + std::to_string(k) + " polls",
           rc == 0 && interrupted == 1 && stop.calls == k + 1);
    stop = StopAfter{k};
    interrupted = 0;
    const int prc = stanli_pathfinder_inits_with_poll(
        m, 1, 1, 4, 1000, 25, 5, 2.0, inits.data(), nullptr, nullptr,
        stop_after, &stop, &interrupted, err, sizeof err);
    expect("pathfinder inits stop on request after " + std::to_string(k) +
               " polls",
           prc == 0 && interrupted == 1 && stop.calls == k + 1);
    stop = StopAfter{k};
    interrupted = 0;
    const int hrc = stanli_hessian_with_poll(m, q.data(), &lp, nullptr,
                                             hess.data(), stop_after, &stop,
                                             &interrupted, err, sizeof err);
    expect("hessian stops on request after " + std::to_string(k) + " polls",
           hrc == 0 && interrupted == 1 && stop.calls == k + 1);
  }
  // A poll that never asks to stop changes nothing.
  o.iter = 50;
  std::vector<double> plain((size_t)n), polled((size_t)n);
  double lp_plain = 0, lp_polled = 0;
  StopAfter count;
  int interrupted = 1;
  stanli_optimize(m, &o, plain.data(), values.data(), &lp_plain, err,
                  sizeof err);
  stanli_optimize_with_poll(m, &o, polled.data(), values.data(), &lp_polled,
                            never_stop, &count, &interrupted, err, sizeof err);
  expect("a poll that never stops leaves optimize alone",
         plain == polled && lp_plain == lp_polled && interrupted == 0 &&
             count.calls >= 1);
  stanli_model_free(m);
}

void test_laplace(stanli_model* m) {
  char err[4096]{};
  const std::vector<double> mode = find_mode(m);
  stanli_laplace_opts o;
  stanli_laplace_opts_init(&o);
  expect("laplace defaults", o.seed == 1 && o.draws == 1000 &&
                                 o.jacobian == 1 && o.calculate_lp == 1);
  o.draws = 4000;
  o.seed = 20261009;
  std::vector<double> values((size_t)(o.draws * kWidth));
  std::vector<double> lp((size_t)o.draws), lq((size_t)o.draws);
  int rc = stanli_laplace_sample(m, &o, mode.data(), values.data(), lp.data(),
                                 lq.data(), nullptr, nullptr, nullptr, nullptr,
                                 nullptr, err, sizeof err);
  expect(std::string("laplace runs: ") + err, rc == 0);
  if (rc == 0) {
    expect("laplace output is finite",
           all_finite(values) && all_finite(lp) && all_finite(lq));
    expect_gaussian("laplace", values, o.draws, 5);
    // The target IS its Laplace approximation, so the model's log density
    // and the approximation's differ by one constant across all draws.
    double lo = lp[0] - lq[0], hi = lo;
    for (size_t i = 1; i < lp.size(); ++i) {
      lo = std::fmin(lo, lp[i] - lq[i]);
      hi = std::fmax(hi, lp[i] - lq[i]);
    }
    // ...up to the error of Stan's finite-difference Hessian.
    expect_near("laplace lp - lp_approx is constant", hi - lo, 0.0, 1e-3);
  }

  std::vector<double> again(values.size());
  rc = stanli_laplace_sample(m, &o, mode.data(), again.data(), nullptr, nullptr,
                             nullptr, nullptr, nullptr, nullptr, nullptr, err,
                             sizeof err);
  expect("laplace is reproducible", rc == 0 && again == values);
  o.seed += 1;
  rc = stanli_laplace_sample(m, &o, mode.data(), again.data(), nullptr, nullptr,
                             nullptr, nullptr, nullptr, nullptr, nullptr, err,
                             sizeof err);
  expect("laplace follows the seed", rc == 0 && again != values);

  o.calculate_lp = 0;
  rc = stanli_laplace_sample(m, &o, mode.data(), again.data(), lp.data(),
                             nullptr, nullptr, nullptr, nullptr, nullptr,
                             nullptr, err, sizeof err);
  expect("laplace without lp leaves it NaN", rc == 0 && std::isnan(lp[0]));
  o.calculate_lp = 1;

  o.jacobian = 0;
  rc = stanli_laplace_sample(m, &o, mode.data(), again.data(), nullptr, nullptr,
                             nullptr, nullptr, nullptr, nullptr, nullptr, err,
                             sizeof err);
  expect("laplace refuses jacobian = 0",
         rc != 0 && std::strstr(err, "Jacobian") != nullptr);
  o.jacobian = 1;
  rc = stanli_laplace_sample(m, &o, nullptr, again.data(), nullptr, nullptr,
                             nullptr, nullptr, nullptr, nullptr, nullptr, err,
                             sizeof err);
  expect("laplace refuses a missing mode", rc != 0);
  o.draws = 0;
  rc = stanli_laplace_sample(m, &o, mode.data(), again.data(), nullptr, nullptr,
                             nullptr, nullptr, nullptr, nullptr, nullptr, err,
                             sizeof err);
  expect("laplace refuses zero draws", rc != 0);
}

void test_pathfinder(stanli_model* m) {
  char err[4096]{};
  stanli_pathfinder_opts o;
  stanli_pathfinder_opts_init(&o);
  expect("pathfinder defaults",
         o.seed == 1 && o.chain_id == 1 && o.num_paths == 1 &&
             o.num_draws == 1000 && o.num_elbo_draws == 25 &&
             o.max_lbfgs_iters == 1000 && o.history_size == 5 &&
             o.calculate_lp == 1 && o.inits == nullptr);
  expect("pathfinder default capacity",
         stanli_pathfinder_max_draws(&o) == 1000);

  o.seed = 8128;
  o.num_draws = 4000;
  const int64_t cap = stanli_pathfinder_max_draws(&o);
  expect("pathfinder capacity is num_draws", cap == 4000);
  std::vector<double> values((size_t)(cap * kWidth));
  std::vector<double> lp((size_t)cap), lq((size_t)cap), path((size_t)cap);
  int64_t n = -1;
  Log log;
  int rc = stanli_pathfinder(m, &o, values.data(), lp.data(), lq.data(),
                             path.data(), &n, on_log, &log, nullptr, nullptr,
                             nullptr, err, sizeof err);
  expect(std::string("pathfinder runs: ") + err, rc == 0);
  expect("pathfinder returns num_draws rows", n == 4000);
  expect("pathfinder logs no errors", log.errors == 0);
  if (rc == 0 && n == 4000) {
    expect("pathfinder output is finite",
           all_finite(values) && all_finite(lp) && all_finite(lq));
    expect_gaussian("pathfinder", values, n, 5);
    bool ids = true;
    for (int64_t i = 0; i < n; ++i) ids &= path[(size_t)i] == 1.0;
    expect("pathfinder path id is the chain id", ids);
  }

  std::vector<double> again(values.size());
  int64_t n2 = -1;
  rc = stanli_pathfinder(m, &o, again.data(), nullptr, nullptr, nullptr, &n2,
                         nullptr, nullptr, nullptr, nullptr, nullptr, err,
                         sizeof err);
  expect("pathfinder is reproducible", rc == 0 && n2 == n && again == values);
  o.seed += 1;
  rc = stanli_pathfinder(m, &o, again.data(), nullptr, nullptr, nullptr, &n2,
                         nullptr, nullptr, nullptr, nullptr, nullptr, err,
                         sizeof err);
  expect("pathfinder follows the seed", rc == 0 && again != values);
  o.seed -= 1;
  // The chain id names the stream and the path.
  o.chain_id = 3;
  rc = stanli_pathfinder(m, &o, again.data(), nullptr, nullptr, path.data(),
                         &n2, nullptr, nullptr, nullptr, nullptr, nullptr, err,
                         sizeof err);
  expect("pathfinder follows the chain id",
         rc == 0 && again != values && path[0] == 3.0);
  o.chain_id = 1;

  // Stan reuses the draws it scored the chosen approximation with, which
  // carry their log density; only the draws beyond those go without.
  o.calculate_lp = 0;
  o.num_draws = 50;
  rc = stanli_pathfinder(m, &o, again.data(), lp.data(), nullptr, nullptr, &n2,
                         nullptr, nullptr, nullptr, nullptr, nullptr, err,
                         sizeof err);
  expect(std::string("pathfinder without lp: ") + err,
         rc == 0 && n2 == 50 && std::isfinite(lp[0]) && std::isnan(lp[49]));
  o.calculate_lp = 1;

  // An explicit starting point is used: the run depends on it, and at the
  // mode itself L-BFGS has nowhere to go, which Stan reports as a failure.
  o.num_draws = 200;
  const std::vector<double> start_a = {0, 0, 1};
  const std::vector<double> start_b = {2, -1, -1};
  std::vector<double> from_a((size_t)(200 * kWidth)), from_b(from_a.size());
  o.inits = start_a.data();
  rc = stanli_pathfinder(m, &o, from_a.data(), nullptr, nullptr, nullptr, &n,
                         nullptr, nullptr, nullptr, nullptr, nullptr, err,
                         sizeof err);
  expect(std::string("pathfinder with an init: ") + err, rc == 0 && n == 200);
  o.inits = start_b.data();
  rc = stanli_pathfinder(m, &o, from_b.data(), nullptr, nullptr, nullptr, &n,
                         nullptr, nullptr, nullptr, nullptr, nullptr, err,
                         sizeof err);
  expect("pathfinder depends on its init",
         rc == 0 && n == 200 && from_a != from_b);
  const std::vector<double> at_mode = {1, -2, 0};
  o.inits = at_mode.data();
  Log stuck;
  rc = stanli_pathfinder(m, &o, from_b.data(), nullptr, nullptr, nullptr, &n,
                         on_log, &stuck, nullptr, nullptr, nullptr, err,
                         sizeof err);
  expect("pathfinder reports a path that cannot move",
         rc != 0 && stuck.errors > 0 && std::strstr(err, "LBFGS") != nullptr);
  o.inits = nullptr;

  // Multi-path is held: Stan 2.40's multi-path service reads past the end
  // of an array, so more than one path is refused before anything runs.
  for (int paths : {2, 4}) {
    o.num_paths = paths;
    expect("multi-path has no capacity", stanli_pathfinder_max_draws(&o) == 0);
    Log refused;
    n = -1;
    rc = stanli_pathfinder(m, &o, from_b.data(), nullptr, nullptr, nullptr, &n,
                           on_log, &refused, nullptr, nullptr, nullptr, err,
                           sizeof err);
    expect("multi-path pathfinder is refused",
           rc != 0 && n == 0 && refused.info == 0 &&
               std::strstr(err, "multi-path Pathfinder is not available") !=
                   nullptr &&
               std::strstr(err, "num_paths = 1") != nullptr);
  }

  // Errors and interrupts.
  o.num_paths = 0;
  rc = stanli_pathfinder(m, &o, values.data(), nullptr, nullptr, nullptr, &n,
                         nullptr, nullptr, nullptr, nullptr, nullptr, err,
                         sizeof err);
  expect("pathfinder refuses zero paths",
         rc != 0 && std::strstr(err, "num_paths") != nullptr && n == 0);
  o.num_paths = 1;
  o.num_draws = 0;
  rc = stanli_pathfinder(m, &o, values.data(), nullptr, nullptr, nullptr, &n,
                         nullptr, nullptr, nullptr, nullptr, nullptr, err,
                         sizeof err);
  expect("pathfinder refuses zero draws",
         rc != 0 && std::strstr(err, "num_draws") != nullptr);
  o.num_draws = 200;
  int asked = 0, interrupted = 0;
  rc = stanli_pathfinder(m, &o, values.data(), nullptr, nullptr, nullptr, &n,
                         nullptr, nullptr, stop_now, &asked, &interrupted, err,
                         sizeof err);
  expect("pathfinder stops when asked",
         rc == 0 && interrupted == 1 && asked == 1 && n == 0);
}

void test_variational(stanli_model* m) {
  char err[4096]{};
  stanli_variational_opts o;
  stanli_variational_opts_init(&o);
  expect("variational defaults",
         o.seed == 1 && o.algorithm == STANLI_ADVI_MEANFIELD &&
             o.iter == 10000 && o.grad_samples == 1 && o.elbo_samples == 100 &&
             o.eta == 1.0 && o.adapt_engaged == 1 && o.adapt_iter == 50 &&
             o.tol_rel_obj == 0.01 && o.eval_elbo == 100 &&
             o.output_samples == 1000);
  o.seed = 314159;
  o.output_samples = 4000;
  // ADVI's default stopping rule ends within a few hundred noisy steps. A
  // tighter tolerance with more gradient samples is what lets the result be
  // compared with the known answer; it is still a stochastic optimum, so
  // means are held to 0.15 and standard deviations to 15%.
  o.tol_rel_obj = 0.001;
  o.grad_samples = 10;
  const int64_t rows = o.output_samples;
  std::vector<double> mean((size_t)kWidth), values((size_t)(rows * kWidth));
  std::vector<double> lp((size_t)rows), lg((size_t)rows);

  // Full rank: the family contains the target.
  o.algorithm = STANLI_ADVI_FULLRANK;
  Log log;
  int rc = stanli_variational(m, &o, mean.data(), values.data(), lp.data(),
                              lg.data(), on_log, &log, nullptr, nullptr,
                              nullptr, err, sizeof err);
  expect(std::string("fullrank runs: ") + err, rc == 0);
  expect("fullrank logs its ELBO table",
         log.text.find("ELBO") != std::string::npos && log.errors == 0);
  if (rc == 0) {
    expect("fullrank output is finite",
           all_finite(values) && all_finite(lp) && all_finite(lg));
    const Moments t1 = moments(values, rows, kWidth, 0);
    const Moments t2 = moments(values, rows, kWidth, 1);
    const Moments lt = moments(values, rows, kWidth, 2, true);
    expect_near("fullrank mean theta[1]", t1.mean, kMean1, 0.15);
    expect_near("fullrank mean theta[2]", t2.mean, kMean2, 0.15);
    expect_near("fullrank sd theta[1]", t1.sd, 1.0, 0.15);
    expect_near("fullrank sd theta[2]", t2.sd, 1.0, 0.15);
    expect_near("fullrank mean log tau", lt.mean, 0.0, 0.1);
    expect_near("fullrank sd log tau", lt.sd, kLogTauSd, 0.075);
    expect_near("fullrank cor theta", correlation(values, rows, kWidth, 0, 1),
                kRho, 0.1);
    // The first row is the mean of the approximation, not a draw.
    expect_near("fullrank mean row theta[1]", mean[0], t1.mean, 0.1);
    expect_near("fullrank mean row tau2", mean[3], mean[2] * mean[2], 1e-12);
    // log_g__ is -0.5 |eta|^2 for a standard normal eta: never positive,
    // and its mean is -dimension / 2 = -1.5 (sd sqrt(1.5), 5 se).
    double g_max = lg[0], g_mean = 0;
    for (double g : lg) {
      g_max = std::fmax(g_max, g);
      g_mean += g;
    }
    expect("fullrank log_g is nonpositive", g_max <= 0);
    expect_near("fullrank mean log_g", g_mean / (double)rows, -1.5,
                5 * std::sqrt(1.5) / std::sqrt((double)rows));
  }

  // Mean field: a diagonal family fitted to a correlated Gaussian by KL(q||p)
  // matches the conditional, not the marginal, standard deviation:
  // sqrt(1 - rho^2) = 0.6 instead of 1, with no correlation.
  o.algorithm = STANLI_ADVI_MEANFIELD;
  rc = stanli_variational(m, &o, mean.data(), values.data(), lp.data(),
                          lg.data(), nullptr, nullptr, nullptr, nullptr,
                          nullptr, err, sizeof err);
  expect(std::string("meanfield runs: ") + err, rc == 0);
  if (rc == 0) {
    const Moments t1 = moments(values, rows, kWidth, 0);
    const Moments t2 = moments(values, rows, kWidth, 1);
    const Moments lt = moments(values, rows, kWidth, 2, true);
    expect_near("meanfield mean theta[1]", t1.mean, kMean1, 0.15);
    expect_near("meanfield mean theta[2]", t2.mean, kMean2, 0.15);
    expect_near("meanfield sd theta[1]", t1.sd, 0.6, 0.09);
    expect_near("meanfield sd theta[2]", t2.sd, 0.6, 0.09);
    expect_near("meanfield sd log tau", lt.sd, kLogTauSd, 0.075);
    // Independent draws: the sample correlation's standard error is
    // 1 / sqrt(S); five of them.
    expect_near("meanfield cor theta", correlation(values, rows, kWidth, 0, 1),
                0.0, 5 / std::sqrt((double)rows));
  }

  std::vector<double> again(values.size());
  rc = stanli_variational(m, &o, nullptr, again.data(), nullptr, nullptr,
                          nullptr, nullptr, nullptr, nullptr, nullptr, err,
                          sizeof err);
  expect("variational is reproducible", rc == 0 && again == values);
  o.seed += 1;
  rc = stanli_variational(m, &o, nullptr, again.data(), nullptr, nullptr,
                          nullptr, nullptr, nullptr, nullptr, nullptr, err,
                          sizeof err);
  expect("variational follows the seed", rc == 0 && again != values);

  // A fixed step size, and running out of iterations, are both ordinary
  // runs: Stan reports the latter through the log.
  o.adapt_engaged = 0;
  o.eta = 0.1;
  o.iter = 200;
  o.output_samples = 100;
  Log short_log;
  rc = stanli_variational(m, &o, nullptr, again.data(), nullptr, nullptr,
                          on_log, &short_log, nullptr, nullptr, nullptr, err,
                          sizeof err);
  expect(std::string("variational with fixed eta: ") + err, rc == 0);
  expect(
      "variational reports the iteration limit",
      short_log.text.find("maximum number of iterations") != std::string::npos);
  expect("variational skips adaptation when told to",
         short_log.text.find("eta adaptation") == std::string::npos);

  // Errors and interrupts.
  o.algorithm = 7;
  rc = stanli_variational(m, &o, nullptr, again.data(), nullptr, nullptr,
                          nullptr, nullptr, nullptr, nullptr, nullptr, err,
                          sizeof err);
  expect("variational refuses an unknown family", rc != 0);
  o.algorithm = STANLI_ADVI_MEANFIELD;
  o.elbo_samples = 0;
  rc = stanli_variational(m, &o, nullptr, again.data(), nullptr, nullptr,
                          nullptr, nullptr, nullptr, nullptr, nullptr, err,
                          sizeof err);
  expect("variational checks elbo_samples before Stan does",
         rc != 0 && std::string(err) == "elbo_samples must be an integer >= 1");
  o.elbo_samples = 100;
  o.iter = 10000;
  int asked = 0, interrupted = 0;
  rc = stanli_variational(m, &o, nullptr, again.data(), nullptr, nullptr,
                          nullptr, nullptr, stop_now, &asked, &interrupted, err,
                          sizeof err);
  expect("variational stops when asked",
         rc == 0 && interrupted == 1 && asked == 1);
}

void test_hessian(stanli_model* gauss) {
  char err[4096]{};
  const int64_t n = 3;
  // theta is multi_normal with S = [[1, .8], [.8, 1]], so its block is -S^-1
  // wherever it is evaluated; log(tau) ~ normal(0, .5) after the Jacobian
  // contributes -4. Every gradient component is linear in q.
  const double d = 1.0 / (1.0 - 0.64);
  const double want[9] = {-d, 0.8 * d, 0, 0.8 * d, -d, 0, 0, 0, -4.0};
  const double points[2][3] = {{0.0, 0.0, 0.0}, {1.7, -2.3, 0.6}};
  for (const auto& pt : points) {
    double lp = 0, ref_lp = 0;
    std::vector<double> grad(n, -1.0), ref_grad(n), hess(n * n, -1.0);
    err[0] = '\0';
    const int rc = stanli_hessian(gauss, pt, &lp, grad.data(), hess.data(), err,
                                  sizeof err);
    expect(std::string("hessian succeeds: ") + err, rc == 0);
    expect(
        "hessian log density matches stanli_grad",
        stanli_grad(gauss, pt, &ref_lp, ref_grad.data()) == 0 && lp == ref_lp);
    for (int64_t i = 0; i < n; ++i) {
      expect_near("hessian gradient matches stanli_grad", grad[(size_t)i],
                  ref_grad[(size_t)i], 0);
      for (int64_t j = 0; j < n; ++j) {
        expect_near("hessian equals -precision", hess[(size_t)(i * n + j)],
                    want[i * n + j], 1e-8);
        expect("hessian is exactly symmetric",
               hess[(size_t)(i * n + j)] == hess[(size_t)(j * n + i)]);
      }
    }
    std::vector<double> no_grad(n * n, -1.0);
    expect("hessian accepts a null gradient",
           stanli_hessian(gauss, pt, &lp, nullptr, no_grad.data(), err,
                          sizeof err) == 0 &&
               no_grad == hess);
  }
  const double q[3] = {0, 0, 0};
  double lp = 0;
  std::vector<double> hess(n * n);
  err[0] = '\0';
  expect("hessian rejects a null point",
         stanli_hessian(gauss, nullptr, &lp, nullptr, hess.data(), err,
                        sizeof err) != 0 &&
             err[0] != '\0');
  err[0] = '\0';
  expect(
      "hessian rejects a null output",
      stanli_hessian(gauss, q, &lp, nullptr, nullptr, err, sizeof err) != 0 &&
          err[0] != '\0');

  const std::string mir = slurp("tests/fixtures/trunc_param.tmir.sexp");
  stanli_model* tp =
      stanli_model_new_with_opts(mir.c_str(), "{}", nullptr, err, sizeof err);
  expect("trunc_param loads", tp != nullptr);
  if (tp == nullptr) return;
  // Nonlinear in both coordinates and coupled through the constrained scale.
  const double tq[2] = {0.4, -0.3};
  double tlp = 0;
  std::vector<double> tgrad(2), thess(4, -1.0);
  err[0] = '\0';
  expect(std::string("trunc_param hessian: ") + err,
         stanli_hessian(tp, tq, &tlp, tgrad.data(), thess.data(), err,
                        sizeof err) == 0);
  const double h = 1e-4;
  for (int j = 0; j < 2; ++j) {
    double up[2] = {tq[0], tq[1]}, dn[2] = {tq[0], tq[1]};
    up[j] += h;
    dn[j] -= h;
    double l = 0, gu[2], gd[2];
    expect(
        "central difference points evaluate",
        stanli_grad(tp, up, &l, gu) == 0 && stanli_grad(tp, dn, &l, gd) == 0);
    for (int i = 0; i < 2; ++i)
      expect_near("hessian matches central differences of stanli_grad",
                  thess[(size_t)(i * 2 + j)], (gu[i] - gd[i]) / (2 * h), 1e-6);
  }
  expect("coupling is nonzero", std::fabs(thess[1]) > 1e-3);
  expect("trunc_param hessian is exactly symmetric", thess[1] == thess[2]);

  const double bad[2] = {0.0, -800.0};
  err[0] = '\0';
  const int rc =
      stanli_hessian(tp, bad, &tlp, nullptr, thess.data(), err, sizeof err);
  expect("hessian reports a rejected evaluation", rc != 0 && err[0] != '\0');
  expect("the model still evaluates after a rejection",
         stanli_grad(tp, tq, &tlp, tgrad.data()) == 0);
  stanli_model_free(tp);
}

}  // namespace

void test_optimize_rejected_mode() {
  stanli_model* m = load("alg_reject_mode");
  if (m == nullptr) return;
  char err[4096]{};
  stanli_optimize_opts o;
  stanli_optimize_opts_init(&o);
  o.seed = 5;
  o.jacobian = 1;
  double q = 0, value[2] = {0, 0}, lp = 0;
  const int rc = stanli_optimize(m, &o, &q, value, &lp, err, sizeof err);
  expect("optimize reports a mode row that write_array rejects", rc != 0);
  expect("optimize names the rejection",
         std::strstr(err, "negative") != nullptr);
  stanli_model_free(m);
}

// A binding may call any entry point from a thread of its own, one that
// has never evaluated a model. Each must work there as on the main thread.
void test_algorithms_on_a_new_thread(stanli_model* m) {
  const auto on_new_thread = [](const std::function<int(char*)>& call) {
    char err[4096]{};
    int rc = -1;
    std::thread([&] { rc = call(err); }).join();
    return rc == 0 ? std::string() : std::string(err) + " ";
  };
  std::vector<double> q(3), values((size_t)(400 * kWidth));
  std::vector<double> lp(400), lq(400), path(400);
  const std::string optimize = on_new_thread([&](char* err) {
    stanli_optimize_opts o;
    stanli_optimize_opts_init(&o);
    double mode_lp = 0;
    return stanli_optimize(m, &o, q.data(), values.data(), &mode_lp, err, 4096);
  });
  expect("optimize on a new thread " + optimize, optimize.empty());
  const std::string inits = on_new_thread([&](char* err) {
    std::vector<double> starts(3 * 2);
    return stanli_pathfinder_inits(m, 1, 1, 2, 1000, 25, 5, 2.0, starts.data(),
                                   nullptr, nullptr, err, 4096);
  });
  expect("pathfinder_inits on a new thread " + inits, inits.empty());
  const std::string laplace = on_new_thread([&](char* err) {
    stanli_laplace_opts o;
    stanli_laplace_opts_init(&o);
    o.draws = 400;
    return stanli_laplace_sample(m, &o, q.data(), values.data(), lp.data(),
                                 lq.data(), nullptr, nullptr, nullptr, nullptr,
                                 nullptr, err, 4096);
  });
  expect("laplace on a new thread " + laplace, laplace.empty());
  const std::string pathfinder = on_new_thread([&](char* err) {
    stanli_pathfinder_opts o;
    stanli_pathfinder_opts_init(&o);
    o.num_draws = 400;
    int64_t rows = 0;
    return stanli_pathfinder(m, &o, values.data(), lp.data(), lq.data(),
                             path.data(), &rows, nullptr, nullptr, nullptr,
                             nullptr, nullptr, err, 4096);
  });
  expect("pathfinder on a new thread " + pathfinder, pathfinder.empty());
  const std::string advi = on_new_thread([&](char* err) {
    stanli_variational_opts o;
    stanli_variational_opts_init(&o);
    o.output_samples = 400;
    std::vector<double> mean((size_t)kWidth);
    return stanli_variational(m, &o, mean.data(), values.data(), lp.data(),
                              lq.data(), nullptr, nullptr, nullptr, nullptr,
                              nullptr, err, 4096);
  });
  expect("variational on a new thread " + advi, advi.empty());
  const std::string fixed = on_new_thread([&](char* err) {
    stanli_fixed_param_opts o;
    stanli_fixed_param_opts_init(&o);
    o.chains = 1;
    o.samples = 400;
    return stanli_fixed_param(m, &o, values.data(), nullptr, nullptr, nullptr,
                              nullptr, nullptr, nullptr, err, 4096);
  });
  expect("fixed_param on a new thread " + fixed, fixed.empty());
}

// Stan's algorithms reject a point only on a domain error; fatal_error()
// ends the run. stanli's run-time error for a zero-size local that a branch
// left empty is that kind too. Every algorithm starts at mu = 0, where v is
// assigned, and moves towards the mode at mu = -3, where it is not.
void test_fatal_errors_end_algorithms() {
  const char* sources[] = {
      "parameters { real mu; }\n"
      "model {\n  vector[0] v;\n  if (mu > -0.5) v = rep_vector(0, 3);\n"
      "  mu ~ normal(-3, 1);\n  target += sum(v);\n}\n",
      "parameters { real mu; }\n"
      "model {\n  if (mu < -0.5) fatal_error(\"mu is below -0.5\");\n"
      "  mu ~ normal(-3, 1);\n}\n"};
  const char* names[] = {"v was not assigned", "mu is below -0.5"};
  for (int k = 0; k < 2; ++k) {
    stanli_model* m = from_stan(sources[k]);
    if (m == nullptr) return;
    const std::string what = k == 0 ? "unassigned local" : "fatal_error";
    const auto names_it = [&](const char* err) {
      return std::strstr(err, names[k]) != nullptr;
    };
    const double start = 0.0;
    char err[4096]{};

    stanli_sample_opts so;
    stanli_sample_opts_init(&so);
    so.chains = 1;
    so.warmup = 200;
    so.samples = 200;
    so.inits = &start;
    std::vector<double> draws(200);
    const int failed = stanli_sample_multi_metric(
        m, &so, nullptr, 0, draws.data(), nullptr, nullptr, nullptr, nullptr,
        nullptr, nullptr, nullptr, nullptr, err, sizeof err);
    expect(what + ": NUTS stops with the message: " + err,
           failed != 0 && names_it(err));

    err[0] = '\0';
    const int walnuts = stanli_sample_walnuts_stats_init(
        m, 1, 1, 200, 200, 0, 2, &start, draws.data(), nullptr, nullptr,
        nullptr, err, sizeof err);
    expect(what + ": WALNUTS stops with the message: " + err,
           walnuts != 0 && names_it(err));

    err[0] = '\0';
    stanli_optimize_opts oo;
    stanli_optimize_opts_init(&oo);
    oo.init = &start;
    double q = 0, lp = 0;
    std::vector<double> row((size_t)stanli_n_output_columns(m));
    const int optimized =
        stanli_optimize(m, &oo, &q, row.data(), &lp, err, sizeof err);
    expect(what + ": optimize stops with the message: " + err,
           optimized != 0 && names_it(err));

    err[0] = '\0';
    stanli_pathfinder_opts po;
    stanli_pathfinder_opts_init(&po);
    po.inits = &start;
    std::vector<double> rows((size_t)stanli_pathfinder_max_draws(&po) *
                             (size_t)stanli_n_output_columns(m));
    int64_t n_draws = 0;
    const int pathfinder = stanli_pathfinder(
        m, &po, rows.data(), nullptr, nullptr, nullptr, &n_draws, nullptr,
        nullptr, nullptr, nullptr, nullptr, err, sizeof err);
    expect(what + ": Pathfinder stops with the message: " + err,
           pathfinder != 0 && names_it(err));

    err[0] = '\0';
    stanli_variational_opts vo;
    stanli_variational_opts_init(&vo);
    vo.init = &start;
    vo.output_samples = 10;
    std::vector<double> mean((size_t)stanli_n_output_columns(m));
    std::vector<double> vrows(10 * mean.size());
    const int advi = stanli_variational(
        m, &vo, mean.data(), vrows.data(), nullptr, nullptr, nullptr, nullptr,
        nullptr, nullptr, nullptr, err, sizeof err);
    expect(what + ": ADVI stops with the message: " + err,
           advi != 0 && names_it(err));

    err[0] = '\0';
    stanli_laplace_opts lo;
    stanli_laplace_opts_init(&lo);
    lo.draws = 10;
    const double mode = -3.0;
    std::vector<double> lrows(10 * mean.size());
    const int laplace = stanli_laplace_sample(
        m, &lo, &mode, lrows.data(), nullptr, nullptr, nullptr, nullptr,
        nullptr, nullptr, nullptr, err, sizeof err);
    expect(what + ": Laplace stops with the message: " + err,
           laplace != 0 && names_it(err));
    stanli_model_free(m);
  }

  // A model that never reaches the unassigned path samples as the same
  // model without the local does, draw for draw.
  const char* same[] = {
      "parameters { real mu; }\n"
      "model {\n  vector[0] v;\n  if (mu > -1e300) v = rep_vector(0, 3);\n"
      "  mu ~ normal(0, 1);\n  target += sum(v);\n}\n",
      "parameters { real mu; }\nmodel { mu ~ normal(0, 1); }\n"};
  std::vector<double> runs[2];
  for (int k = 0; k < 2; ++k) {
    stanli_model* m = from_stan(same[k]);
    if (m == nullptr) return;
    stanli_sample_opts so;
    stanli_sample_opts_init(&so);
    so.chains = 1;
    so.warmup = 200;
    so.samples = 200;
    runs[k].assign(200, 0.0);
    char err[4096]{};
    expect(
        std::string("a model that keeps v assigned samples: ") + err,
        stanli_sample_multi_metric(m, &so, nullptr, 0, runs[k].data(), nullptr,
                                   nullptr, nullptr, nullptr, nullptr, nullptr,
                                   nullptr, nullptr, err, sizeof err) == 0);
    stanli_model_free(m);
  }
  expect("a model that keeps v assigned has the same draws",
         runs[0] == runs[1]);
}

int main() {
  test_fatal_errors_end_algorithms();
  stanli_model* m = load("alg_gauss");
  if (m != nullptr) {
    expect("alg_gauss width", stanli_wa_n_columns(m) == kWidth);
    expect("alg_gauss parameters", stanli_n_unconstrained(m) == 3);
    test_fixed_param(m);
    test_optimize_options(m);
    test_option_checks(m);
    test_laplace(m);
    test_pathfinder(m);
    test_variational(m);
    test_hessian(m);
    test_algorithms_on_a_new_thread(m);
    // The algorithms borrow the model's executor; it must still sample.
    std::vector<double> draws(200 * 3);
    char err[4096]{};
    expect("the model still samples afterwards",
           stanli_sample_stream_stats_init(m, 1, 200, 200, 0.8, nullptr,
                                           draws.data(), nullptr, nullptr,
                                           nullptr, err, sizeof err) == 0);
    stanli_model_free(m);
  }
  test_noparam();
  test_optimize_rejected_mode();
  test_optimize_generated_quantities_stream();
  test_interruptible_estimates();
  test_optimize_chain_ids();
  test_optimize_validates_init();
  if (failures) {
    std::printf("%d failures\n", failures);
    return 1;
  }
  std::printf("all algorithm C API tests passed\n");
  return 0;
}
