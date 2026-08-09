// End to end through the compiler: es.stan MIR + posteriordb JSON data ->
// NUTS -> posterior checks. Mirrors the hand-built models' statistical test
// but with the graph produced by the compiler instead of by hand.
#include <stanli/compile.hpp>
#include <stanli/nuts.hpp>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

static int failures = 0;
static void expect_in(const std::string& what, double got, double lo,
                      double hi) {
  if (!(got >= lo && got <= hi)) {
    ++failures;
    std::printf("FAIL %-14s got %.6g want in [%g, %g]\n", what.c_str(), got, lo,
                hi);
  }
}
static std::string slurp(const std::string& path) {
  std::ifstream f(path);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

int main() {
  using namespace stanli;

  DataMap data = DataMap::from_json_file("tests/fixtures/eight_schools.json");
  CompiledModel cm = compile_model(slurp("tests/fixtures/es.tmir.sexp"), data);

  Executor ex(std::move(cm.graph));
  cm.bind(ex);

  NutsConfig cfg;
  cfg.seed = 8675309;
  cfg.warmup = 1000;
  cfg.samples = 2000;
  cfg.delta = 0.9;
  auto draws = run_nuts(ex, cfg);

  double mu_mean = 0, tau_mean = 0;
  int nan_count = 0;
  for (const auto& q : draws) {
    for (double v : q)
      if (std::isnan(v)) ++nan_count;
    mu_mean += q[0];
    tau_mean += std::exp(q[1]);
  }
  mu_mean /= draws.size();
  tau_mean /= draws.size();
  expect_in("nan_count", nan_count, 0, 0);
  expect_in("mu mean", mu_mean, 2.5, 6.5);
  expect_in("tau mean", tau_mean, 2.0, 6.0);

  if (failures == 0) std::printf("test_e2e OK\n");
  return failures == 0 ? 0 : 1;
}
