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

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
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
  stanli_model* m =
      stanli_model_new(mir.c_str(), data.c_str(), err, sizeof err);
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
  int rc = stanli_fixed_param(m, &o, values.data(), nullptr, nullptr, nullptr,
                              nullptr, &interrupted, err, sizeof err);
  expect(std::string("fixed_param runs: ") + err, rc == 0 && interrupted == 0);
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
                          nullptr, nullptr, err, sizeof err);
  expect("fixed_param is reproducible", rc == 0 && again == values);
  o.seed = 4712;
  rc = stanli_fixed_param(m, &o, again.data(), nullptr, nullptr, nullptr,
                          nullptr, nullptr, err, sizeof err);
  expect("fixed_param follows the seed", rc == 0 && again != values);

  // Random starting points: fixed within a chain, different across chains,
  // inside the constrained image of (-2, 2).
  o.inits = nullptr;
  o.samples = 10;
  o.thin = 3;
  expect("fixed_param thinned row count", stanli_fixed_param_n_draws(&o) == 4);
  std::vector<double> thin((size_t)(2 * 4 * kWidth));
  rc = stanli_fixed_param(m, &o, thin.data(), nullptr, nullptr, nullptr,
                          nullptr, nullptr, err, sizeof err);
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
                          nullptr, nullptr, err, sizeof err);
  expect("fixed_param refuses null options", rc != 0 && err[0] != '\0');
  o.thin = 0;
  rc = stanli_fixed_param(m, &o, thin.data(), nullptr, nullptr, nullptr,
                          nullptr, nullptr, err, sizeof err);
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
  const int rc = stanli_fixed_param(m, &o, values.data(), nullptr, nullptr,
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
  return q;
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
         o.seed == 1 && o.num_paths == 4 && o.num_draws == 1000 &&
             o.num_psis_draws == 1000 && o.num_elbo_draws == 25 &&
             o.max_lbfgs_iters == 1000 && o.history_size == 5 &&
             o.psis_resample == 1 && o.calculate_lp == 1);
  expect("pathfinder default capacity",
         stanli_pathfinder_max_draws(&o) == 1000);

  // Multi-path with resampling, the CmdStan default shape.
  o.seed = 8128;
  o.num_psis_draws = 4000;
  const int64_t cap = stanli_pathfinder_max_draws(&o);
  expect("pathfinder resampled capacity", cap == 4000);
  std::vector<double> values((size_t)(cap * kWidth));
  std::vector<double> lp((size_t)cap), lq((size_t)cap), path((size_t)cap);
  int64_t n = -1;
  Log log;
  int rc = stanli_pathfinder(m, &o, values.data(), lp.data(), lq.data(),
                             path.data(), &n, on_log, &log, nullptr, nullptr,
                             nullptr, err, sizeof err);
  expect(std::string("pathfinder runs: ") + err, rc == 0);
  expect("pathfinder returns num_psis_draws rows", n == 4000);
  expect("pathfinder logs no errors", log.errors == 0);
  if (rc == 0 && n == 4000) {
    expect("pathfinder output is finite",
           all_finite(values) && all_finite(lp) && all_finite(lq));
    // Resampled draws repeat, so the effective sample is smaller than 4000;
    // ten standard errors at the nominal count allows for that.
    expect_gaussian("pathfinder", values, n, 10);
    bool ids = true;
    bool seen[4] = {false, false, false, false};
    for (int64_t i = 0; i < n; ++i) {
      const int id = (int)path[(size_t)i];
      ids &= id >= 1 && id <= 4 && path[(size_t)i] == id;
      if (id >= 1 && id <= 4) seen[id - 1] = true;
    }
    expect("pathfinder path ids are 1..4", ids);
    expect("pathfinder draws come from every path",
           seen[0] && seen[1] && seen[2] && seen[3]);
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

  // Without resampling: every path's draws, in path order.
  o.psis_resample = 0;
  o.num_draws = 500;
  expect("pathfinder unresampled capacity",
         stanli_pathfinder_max_draws(&o) == 2000);
  rc = stanli_pathfinder(m, &o, values.data(), lp.data(), lq.data(),
                         path.data(), &n, nullptr, nullptr, nullptr, nullptr,
                         nullptr, err, sizeof err);
  expect(std::string("pathfinder without resampling: ") + err,
         rc == 0 && n == 2000);
  if (rc == 0 && n == 2000) {
    const std::vector<double> head(values.begin(),
                                   values.begin() + (size_t)(n * kWidth));
    expect_gaussian("pathfinder unresampled", head, n, 5);
  }

  // One path is the single-path service.
  o.num_paths = 1;
  o.num_draws = 3000;
  expect("pathfinder single capacity", stanli_pathfinder_max_draws(&o) == 3000);
  rc = stanli_pathfinder(m, &o, values.data(), lp.data(), lq.data(),
                         path.data(), &n, nullptr, nullptr, nullptr, nullptr,
                         nullptr, err, sizeof err);
  expect(std::string("single-path pathfinder: ") + err, rc == 0 && n == 3000);
  if (rc == 0 && n == 3000) {
    const std::vector<double> head(values.begin(),
                                   values.begin() + (size_t)(n * kWidth));
    expect_gaussian("pathfinder single", head, n, 5);
  }

  // Explicit starting points are used: the run depends on them, and at the
  // mode itself L-BFGS has nowhere to go, which Stan reports as a failure.
  o.num_paths = 2;
  o.num_draws = 200;
  o.psis_resample = 1;
  o.num_psis_draws = 200;
  const std::vector<double> start_a = {0, 0, 1, 2, -1, -1};
  const std::vector<double> start_b = {-1, 1, 0.5, 0, -3, 1};
  std::vector<double> from_a((size_t)(200 * kWidth)), from_b(from_a.size());
  o.inits = start_a.data();
  rc = stanli_pathfinder(m, &o, from_a.data(), nullptr, nullptr, nullptr, &n,
                         nullptr, nullptr, nullptr, nullptr, nullptr, err,
                         sizeof err);
  expect(std::string("pathfinder with inits: ") + err, rc == 0 && n == 200);
  o.inits = start_b.data();
  rc = stanli_pathfinder(m, &o, from_b.data(), nullptr, nullptr, nullptr, &n,
                         nullptr, nullptr, nullptr, nullptr, nullptr, err,
                         sizeof err);
  expect("pathfinder depends on its inits",
         rc == 0 && n == 200 && from_a != from_b);
  const std::vector<double> at_mode = {1, -2, 0, 1, -2, 0};
  o.inits = at_mode.data();
  Log stuck;
  rc = stanli_pathfinder(m, &o, from_b.data(), nullptr, nullptr, nullptr, &n,
                         on_log, &stuck, nullptr, nullptr, nullptr, err,
                         sizeof err);
  expect("pathfinder reports a path that cannot move",
         rc != 0 && stuck.errors > 0 && std::strstr(err, "LBFGS") != nullptr);
  o.inits = nullptr;

  // Errors and interrupts.
  o.num_paths = 0;
  rc = stanli_pathfinder(m, &o, values.data(), nullptr, nullptr, nullptr, &n,
                         nullptr, nullptr, nullptr, nullptr, nullptr, err,
                         sizeof err);
  expect("pathfinder refuses zero paths",
         rc != 0 && std::strstr(err, "num_paths") != nullptr && n == 0);
  o.num_paths = 4;
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
  expect("variational reports Stan's own argument check",
         rc != 0 && std::strstr(err, "ELBO") != nullptr);
  o.elbo_samples = 100;
  o.iter = 10000;
  int asked = 0, interrupted = 0;
  rc = stanli_variational(m, &o, nullptr, again.data(), nullptr, nullptr,
                          nullptr, nullptr, stop_now, &asked, &interrupted, err,
                          sizeof err);
  expect("variational stops when asked",
         rc == 0 && interrupted == 1 && asked == 1);
}

}  // namespace

int main() {
  stanli_model* m = load("alg_gauss");
  if (m != nullptr) {
    expect("alg_gauss width", stanli_wa_n_columns(m) == kWidth);
    expect("alg_gauss parameters", stanli_n_unconstrained(m) == 3);
    test_fixed_param(m);
    test_laplace(m);
    test_pathfinder(m);
    test_variational(m);
    // The algorithms borrow the model's executor; it must still sample.
    std::vector<double> draws(200 * 3);
    char err[4096]{};
    expect(
        "the model still samples afterwards",
        stanli_sample(m, 1, 200, 200, 0.8, draws.data(), err, sizeof err) == 0);
    stanli_model_free(m);
  }
  test_noparam();
  if (failures) {
    std::printf("%d failures\n", failures);
    return 1;
  }
  std::printf("all algorithm C API tests passed\n");
  return 0;
}
