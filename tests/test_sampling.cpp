// Sampling-level correctness: does the whole engine -- lowering, the log_prob
// graph, the write_array graph, the adaptive sampler -- land on the right
// posterior?
//
// Every other test in the suite, and the corpus verification rig too, pins a
// gradient at a fixed point and compares it to CmdStan. That is a strong
// oracle for the graph and a blind one for the sampler: max tree depth
// defaulted to 5 instead of CmdStan's 10 for the whole life of the project,
// which left every gradient bit-identical and every posterior needing a long
// trajectory under-explored. Nothing structural could have caught it. So:
//
//   conj  a conjugate normal whose posterior is known in closed form, with a
//         transformed data block, a transformed parameter and generated
//         quantities -- checked in distribution against the analytic moments,
//         and exactly, draw by draw, for the deterministic columns.
//   ar1   a badly conditioned Gaussian, checked for trajectories longer than
//         a depth-5 sampler can produce.
#include <stanli/compile.hpp>
#include <stanli/nuts.hpp>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect_near(const std::string& what, double got, double want, double tol) {
  if (!(std::fabs(got - want) <= tol)) {
    ++failures;
    std::printf("FAIL %-26s got %.9g want %.9g +/- %.3g\n", what.c_str(), got,
                want, tol);
  }
}

void expect_ge(const std::string& what, double got, double lo) {
  if (!(got >= lo)) {
    ++failures;
    std::printf("FAIL %-26s got %.9g want >= %.9g\n", what.c_str(), got, lo);
  }
}

void expect_eq_str(const std::string& what, const std::string& got,
                   const std::string& want) {
  if (got != want) {
    ++failures;
    std::printf("FAIL %s\n  got  %s\n  want %s\n", what.c_str(), got.c_str(),
                want.c_str());
  }
}

std::string slurp(const std::string& path) {
  std::ifstream f(path);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

double mean(const std::vector<double>& v) {
  double s = 0;
  for (double x : v) s += x;
  return s / (double)v.size();
}

double sd(const std::vector<double>& v) {
  const double m = mean(v);
  double s = 0;
  for (double x : v) s += (x - m) * (x - m);
  return std::sqrt(s / (double)(v.size() - 1));
}

// Flattened CSV header, exactly as stanli_run writes it.
std::string header(const std::vector<stanli::CompiledModel::ParamView>& cols) {
  std::string h;
  for (const auto& n : stanli::CompiledModel::csv_names(cols)) {
    if (!h.empty()) h += ',';
    h += n;
  }
  return h;
}

// ---- conjugate normal, against the closed-form posterior -------------------
//
// Both parameters are declared without a prior, so the prior is uniform on
// the CONSTRAINED scale -- that is what the `<lower=0>` jacobian is for, and
// it means sigma is flat on (0, inf), not log sigma. Writing
// ss = sum((y - ybar)^2), the posterior is then
//
//   1/sigma^2 | y ~ Gamma((N-2)/2, rate = ss/2)
//   mu_c      | y ~ t_{N-2} scaled to variance ss / (N (N-4))
//
// so E[prec] = (N-2)/ss, sd[prec] = sqrt(2(N-2))/ss, E[mu_c] = 0 and
// sd[mu_c] = sqrt(ss / (N (N-4))). No special functions needed, which is why
// this model was picked. A 200,000-draw CmdStan run on this exact data agrees
// with all four to within its Monte Carlo error (0.389499 / 0.079736 /
// 0.000471 / 0.230671).
void test_conjugate() {
  using namespace stanli;

  DataMap data = DataMap::from_json_file("tests/fixtures/conj.json");
  const std::vector<double>& y = data.at("y").r;
  const int N = (int)y.size();
  double ybar = 0;
  for (double v : y) ybar += v;
  ybar /= N;
  double ss = 0;
  for (double v : y) ss += (v - ybar) * (v - ybar);

  CompiledModel cm =
      compile_model(slurp("tests/fixtures/conj.tmir.sexp"), data);
  if (!cm.write_array) {
    std::printf("FAIL conj: no write_array graph was compiled\n");
    ++failures;
    return;
  }
  expect_eq_str("conj write_array truncated", cm.write_array->truncated, "");

  // CmdStan's column order: constrained parameters, then transformed
  // parameters, then generated quantities, each in declaration order.
  std::string want_hdr = "mu_c,sigma,prec,mu,sd_from_prec";
  for (int i = 1; i <= N; ++i) want_hdr += ",resid." + std::to_string(i);
  expect_eq_str("conj csv header", header(cm.write_array->columns), want_hdr);

  Executor ex(std::move(cm.graph));
  cm.bind(ex);

  NutsConfig cfg;
  cfg.seed = 20260806;
  cfg.warmup = 1000;
  cfg.samples = 2000;
  auto draws = run_nuts(ex, cfg);

  Executor wex(std::move(cm.write_array->graph));
  cm.write_array->bind(wex);
  if (wex.n_params() != ex.n_params()) {
    std::printf("FAIL conj: write_array takes %lld params, log_prob %lld\n",
                (long long)wex.n_params(), (long long)ex.n_params());
    ++failures;
    return;
  }

  // Column offsets into a flattened draw row.
  std::vector<int64_t> off;
  int64_t at = 0;
  for (const auto& c : cm.write_array->columns) {
    off.push_back(at);
    at += c.len;
  }
  const int64_t c_mu_c = off[0], c_sigma = off[1], c_prec = off[2],
                c_mu = off[3], c_sdp = off[4], c_resid = off[5];

  std::vector<std::vector<double>> rows;
  rows.reserve(draws.size());
  for (const auto& q : draws) {
    for (size_t i = 0; i < q.size(); ++i) wex.params_data()[i] = q[i];
    wex.run_forward_only();
    std::vector<double> row;
    row.reserve((size_t)at);
    for (const auto& c : cm.write_array->columns) {
      const double* p = wex.value_ptr(c.slot);
      row.insert(row.end(), p, p + c.len);
    }
    rows.push_back(std::move(row));
  }

  // Exact, per draw: the transformed parameter and the generated quantities
  // are deterministic functions of the parameters, so they can be checked
  // without any appeal to Monte Carlo error.
  double worst_det = 0;
  int nan_count = 0;
  for (const auto& r : rows) {
    for (double v : r)
      if (std::isnan(v)) ++nan_count;
    const double mu_c = r[c_mu_c], sigma = r[c_sigma];
    auto rel = [&](double got, double want) {
      const double d = std::fabs(got - want) / std::max(1.0, std::fabs(want));
      if (d > worst_det) worst_det = d;
    };
    rel(r[c_prec], 1.0 / (sigma * sigma));  // transformed parameter
    rel(r[c_mu], mu_c + ybar);              // gq reading transformed data
    rel(r[c_sdp], sigma);                   // gq reading a transformed param
    for (int i = 0; i < N; ++i)             // vectorized gq
      rel(r[c_resid + i], (y[(size_t)i] - ybar) - mu_c);
  }
  expect_near("conj nan count", nan_count, 0, 0);
  expect_near("conj deterministic cols", worst_det, 0.0, 1e-12);

  // In distribution, against the analytic moments. Each band is about three
  // times the worst deviation seen across eight seeds, so it is loose enough
  // not to track Monte Carlo noise and tight enough that a real posterior
  // error of a few percent trips it.
  auto col = [&](int64_t c) {
    std::vector<double> v;
    v.reserve(rows.size());
    for (const auto& r : rows) v.push_back(r[(size_t)c]);
    return v;
  };
  const std::vector<double> mu_c = col(c_mu_c), prec = col(c_prec),
                            mu = col(c_mu);
  const double sd_mu_c = std::sqrt(ss / ((double)N * (N - 4)));
  const double e_prec = (N - 2) / ss;
  const double sd_prec = std::sqrt(2.0 * (N - 2)) / ss;

  expect_near("conj E[mu_c]", mean(mu_c), 0.0, 0.030);
  expect_near("conj sd[mu_c]", sd(mu_c), sd_mu_c, 0.028);
  expect_near("conj E[prec]", mean(prec), e_prec, 0.015);
  expect_near("conj sd[prec]", sd(prec), sd_prec, 0.007);
  expect_near("conj E[mu]", mean(mu), ybar, 0.030);
}

// ---- trajectory length, on a target that needs long ones -------------------
//
// AR(1) with rho = 0.99: every marginal is standard normal, but the target is
// conditioned badly enough that a diagonal metric cannot rescue it and NUTS
// needs trees several levels past depth 5. A sampler capped at depth 5 can
// spend at most 31 leapfrog steps per iteration; this one spends ~57.
void test_trajectory_depth() {
  using namespace stanli;

  DataMap data = DataMap::from_json_file("tests/fixtures/ar1.json");
  const int K = data.at("K").i[0];
  CompiledModel cm = compile_model(slurp("tests/fixtures/ar1.tmir.sexp"), data);
  Executor ex(std::move(cm.graph));
  cm.bind(ex);

  NutsConfig cfg;
  cfg.seed = 424242;
  cfg.warmup = 1000;
  cfg.samples = 2000;
  auto draws = run_nuts(ex, cfg);

  const double per_iter =
      (double)ex.n_grad_evals() / (cfg.warmup + cfg.samples);
  // 2^5 - 1 = 31 is the hard ceiling at max tree depth 5. Anything above it
  // proves the sampler is allowed past depth 5; the margin keeps the check
  // from tracking adaptation noise.
  expect_ge("ar1 leapfrogs per iter", per_iter, 40.0);

  // And the draws are still the right distribution, which is what makes the
  // deeper trees worth having.
  double worst_mean = 0, worst_sd = 0;
  for (int k = 0; k < K; ++k) {
    std::vector<double> v;
    v.reserve(draws.size());
    for (const auto& q : draws) v.push_back(q[(size_t)k]);
    worst_mean = std::max(worst_mean, std::fabs(mean(v)));
    worst_sd = std::max(worst_sd, std::fabs(sd(v) - 1.0));
  }
  expect_near("ar1 worst |mean|", worst_mean, 0.0, 0.30);
  expect_near("ar1 worst |sd - 1|", worst_sd, 0.0, 0.12);
}

}  // namespace

int main() {
  test_conjugate();
  test_trajectory_depth();
  if (failures == 0) std::printf("test_sampling OK\n");
  return failures == 0 ? 0 : 1;
}
