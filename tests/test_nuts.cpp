// NUTS end to end through the vendored stan sampler driving the executor's
// gradient. Statistical checks with fixed seeds.
#include "models.hpp"

#include <stanli/nuts.hpp>
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

    NutsConfig cfg;
    cfg.seed = 20260804;
    cfg.warmup = 1000;
    cfg.samples = 2000;
    auto draws = run_nuts(ex, cfg);
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

    NutsConfig cfg;
    cfg.seed = 8675309;
    cfg.warmup = 1000;
    cfg.samples = 2000;
    cfg.delta = 0.9;  // funnel-adjacent geometry, adapt a little tighter
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
    expect_in("es nan_count", nan_count, 0, 0);
    expect_in("es mu mean", mu_mean, 2.5, 6.5);
    expect_in("es tau mean", tau_mean, 2.0, 6.0);
  }

  // ---- initialization retry (the accel_gp class of failure) --------------
  // The target is finite only for x < 0 (it evaluates log(-x)), and
  // CmdStan's init draws uniform(-2, 2). boost::ecuyer1988's first output
  // for every small seed is the top of the range, +2.000, so EVERY seed's
  // first draw lands in the dead half and the retry path is provably
  // exercised. Before the retry landed, all of these failed outright, the
  // way accel_gp did on 3 of 5 seeds.
  {
    for (uint32_t seed = 1; seed <= 8; ++seed) {
      Graph g;
      const int x = g.add_slot(1, true);
      const int nx = g.add_slot(1, false);
      g.add_op(OP_NEG, {x}, nx);
      const int lx = g.add_slot(1, false);
      g.add_op(OP_LOGV, {nx}, lx);
      const int zero = g.add_slot(1, false);
      const int one = g.add_slot(1, false);
      const int lp = g.add_slot(1, false);
      const int id = g.add_op(OP_NORMAL_LPDF, {lx, zero, one}, lp);
      g.ops[(size_t)id].variant = 0x01;  // outcome active
      g.result_slot = lp;
      Executor ex(std::move(g));
      ex.value_ptr(zero)[0] = 0.0;
      ex.value_ptr(one)[0] = 1.0;

      NutsConfig cfg;
      cfg.seed = seed;
      cfg.warmup = 100;
      cfg.samples = 50;
      try {
        auto draws = run_nuts(ex, cfg);
        expect_in("init-retry draws seed " + std::to_string(seed),
                  (double)draws.size(), cfg.samples, cfg.samples);
        for (const auto& q : draws)
          if (!(q[0] < 0.0)) {
            ++failures;
            std::printf("FAIL init-retry seed %u drew x >= 0\n", seed);
            break;
          }
      } catch (const std::exception& e) {
        ++failures;
        std::printf("FAIL init-retry seed %u threw: %s\n", seed, e.what());
      }
    }
  }
  // A target that is non-finite EVERYWHERE must fail with the clear
  // initialization error, not a stepsize mystery.
  {
    Graph g;
    const int x = g.add_slot(1, true);
    const int nanc = g.add_slot(1, false);
    const int one = g.add_slot(1, false);
    const int lp = g.add_slot(1, false);
    const int id = g.add_op(OP_NORMAL_LPDF, {x, nanc, one}, lp);
    g.ops[(size_t)id].variant = 0x01;
    g.result_slot = lp;
    Executor ex(std::move(g));
    ex.value_ptr(nanc)[0] = std::nan("");
    ex.value_ptr(one)[0] = 1.0;
    NutsConfig cfg;
    cfg.seed = 7;
    cfg.warmup = 10;
    cfg.samples = 5;
    bool threw_init = false;
    try {
      run_nuts(ex, cfg);
    } catch (const std::exception& e) {
      threw_init =
          std::string(e.what()).find("initialization") != std::string::npos;
    }
    if (!threw_init) {
      ++failures;
      std::printf("FAIL all-NaN target: wanted the initialization error\n");
    }
  }

  // ---- sampler stats: one CmdStan-shaped row per post-warmup draw --------
  {
    const int D = 4;
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

    NutsConfig cfg;
    cfg.seed = 7;
    cfg.warmup = 200;
    cfg.samples = 100;
    SamplerStats stats;
    auto draws = run_nuts(ex, cfg, &stats);
    if (stats.rows.size() != draws.size()) {
      ++failures;
      std::printf("FAIL stats rows %zu draws %zu\n", stats.rows.size(),
                  draws.size());
    }
    int64_t leapfrogs = 0;
    for (const auto& r : stats.rows) {
      const double lp_ = r[0], accept = r[1], step = r[2], depth = r[3],
                   nleap = r[4], div = r[5], energy = r[6];
      const bool ok = std::isfinite(lp_) && accept >= 0.0 && accept <= 1.0 &&
                      step > 0.0 && depth >= 0 && depth <= cfg.max_depth &&
                      nleap >= 1 && (div == 0.0 || div == 1.0) &&
                      std::isfinite(energy) && energy >= -lp_;
      if (!ok) {
        ++failures;
        std::printf(
            "FAIL stats row: lp %g accept %g step %g depth %g "
            "nleap %g div %g energy %g\n",
            lp_, accept, step, depth, nleap, div, energy);
        break;
      }
      leapfrogs += (int64_t)nleap;
    }
    if (leapfrogs < cfg.samples) {
      ++failures;
      std::printf("FAIL total leapfrogs %lld\n", (long long)leapfrogs);
    }
  }

  if (failures == 0) std::printf("test_nuts OK\n");
  return failures == 0 ? 0 : 1;
}
