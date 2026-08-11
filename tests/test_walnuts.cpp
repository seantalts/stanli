// WALNUTS end to end through the vendored walnutpie sampler driving the
// executor's gradient. Statistical checks with fixed seeds, same shape as
// test_nuts.cpp so the two samplers stay comparable.
#include "models.hpp"

#include <stanli/walnuts.hpp>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

static int failures = 0;
static void expect_in(const std::string& what, double got, double lo,
                      double hi) {
  if (!(got >= lo && got <= hi)) {
    ++failures;
    std::printf("FAIL %-18s got %.6g want in [%g, %g]\n", what.c_str(), got, lo,
                hi);
  }
}

int main() {
  using namespace stanli;

  // ---- 10-dim standard normal: mean 0, sd 1 ------------------------------
  {
    const int D = 10;
    Graph g;
    const int x = g.add_slot(D, true);
    const int zero = g.add_slot(1, false);
    const int one = g.add_slot(1, false);
    const int lp = g.add_slot(1, false);
    g.add_op(OP_NORMAL_LPDF, {x, zero, one}, lp);
    g.result_slot = lp;
    Executor ex(std::move(g));
    ex.value_ptr(zero)[0] = 0.0;
    ex.value_ptr(one)[0] = 1.0;

    WalnutsConfig cfg;
    cfg.seed = 20260811;
    cfg.warmup = 1000;
    cfg.samples = 2000;
    auto draws = run_walnuts(ex, cfg);
    if ((int)draws.size() != cfg.samples) {
      std::printf("FAIL draw count %zu\n", draws.size());
      return 1;
    }
    for (int d = 0; d < D; ++d) {
      double m = 0, m2 = 0;
      for (const auto& q : draws) m += q[d];
      m /= draws.size();
      for (const auto& q : draws) m2 += (q[d] - m) * (q[d] - m);
      const double sd = std::sqrt(m2 / (draws.size() - 1));
      expect_in("norm mean[" + std::to_string(d) + "]", m, -0.15, 0.15);
      expect_in("norm sd[" + std::to_string(d) + "]", sd, 0.85, 1.15);
    }
  }

  // ---- eight schools: posterior locations, no NaNs -----------------------
  {
    auto m = testmodels::eight_schools();
    Executor ex(std::move(m.graph));
    testmodels::fill_eight_schools_data(m, ex);

    WalnutsConfig cfg;
    cfg.seed = 8675309;
    cfg.warmup = 1000;
    cfg.samples = 2000;
    auto draws = run_walnuts(ex, cfg);

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
    expect_in("es nan_count", nan_count, 0, 0);
    expect_in("es mu mean", mu_mean, 2.5, 6.5);
    expect_in("es tau mean", tau_mean, 2.0, 6.0);
  }

  // ---- determinism: same seed, same chain; new seed, new chain -----------
  {
    Graph g;
    const int x = g.add_slot(3, true);
    const int zero = g.add_slot(1, false);
    const int one = g.add_slot(1, false);
    const int lp = g.add_slot(1, false);
    g.add_op(OP_NORMAL_LPDF, {x, zero, one}, lp);
    g.result_slot = lp;
    Executor ex(std::move(g));
    ex.value_ptr(zero)[0] = 0.0;
    ex.value_ptr(one)[0] = 1.0;

    WalnutsConfig cfg;
    cfg.seed = 42;
    cfg.warmup = 200;
    cfg.samples = 100;
    auto a = run_walnuts(ex, cfg);
    auto b = run_walnuts(ex, cfg);
    cfg.seed = 43;
    auto c = run_walnuts(ex, cfg);
    bool same = a == b;
    expect_in("same seed identical", same ? 1 : 0, 1, 1);
    bool differ = a != c;
    expect_in("new seed differs", differ ? 1 : 0, 1, 1);
  }

  // ---- observer: sees every stored draw, phases in order -----------------
  {
    Graph g;
    const int x = g.add_slot(2, true);
    const int zero = g.add_slot(1, false);
    const int one = g.add_slot(1, false);
    const int lp = g.add_slot(1, false);
    g.add_op(OP_NORMAL_LPDF, {x, zero, one}, lp);
    g.result_slot = lp;
    Executor ex(std::move(g));
    ex.value_ptr(zero)[0] = 0.0;
    ex.value_ptr(one)[0] = 1.0;

    WalnutsConfig cfg;
    cfg.seed = 7;
    cfg.warmup = 50;
    cfg.samples = 60;
    int n_warm = 0, n_samp = 0;
    auto draws = run_walnuts(
        ex, cfg, nullptr,
        [&](int64_t, bool warm, const double*) { (warm ? n_warm : n_samp)++; });
    expect_in("observer warmup", n_warm, 50, 50);
    expect_in("observer samples", n_samp, 60, 60);
    expect_in("draws", (double)draws.size(), 60, 60);
  }

  if (failures) {
    std::printf("%d failures\n", failures);
    return 1;
  }
  std::printf("OK\n");
  return 0;
}
