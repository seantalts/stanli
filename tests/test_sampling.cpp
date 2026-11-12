// Max tree depth defaulted to 5 instead of CmdStan's 10 for the whole life of
// the project, which left every gradient bit-identical and every posterior
// needing a long trajectory under-explored. test_sampler_parity sets max_depth
// explicitly, so this is the check on the default: ar1 is a badly conditioned
// Gaussian that needs trajectories longer than a depth-5 sampler can produce.
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
  test_trajectory_depth();
  if (failures == 0) std::printf("test_sampling OK\n");
  return failures == 0 ? 0 : 1;
}
