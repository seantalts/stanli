// The older single-path Pathfinder entry, run_pathfinder: determinism, the
// init it shares with NUTS, an explicit init, the per-iterate observer, and
// the Pareto k-hat diagnostic. test_capi expect_pathfinder checks its draws
// through the ABI and test_algorithms pins the service's stream.
#include "models.hpp"

#include <stanli/estimate.hpp>
#include <stanli/nuts.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

static int failures = 0;
static void expect(const std::string& what, bool ok) {
  if (!ok) {
    ++failures;
    std::printf("FAIL %s\n", what.c_str());
  }
}
static void expect_in(const std::string& what, double got, double lo,
                      double hi) {
  if (!(got >= lo && got <= hi)) {
    ++failures;
    std::printf("FAIL %-28s got %.6g want in [%g, %g]\n", what.c_str(), got, lo,
                hi);
  }
}

int main() {
  using namespace stanli;

  // ---- determinism -----------------------------------------------------
  {
    Executor ex(testmodels::std_normal(3));
    ex.value_ptr(1)[0] = 0.0;
    ex.value_ptr(2)[0] = 1.0;

    PathfinderConfig a;
    a.seed = 7;
    a.num_draws = 50;
    PathfinderResult r1 = run_pathfinder(ex, a);
    PathfinderResult r2 = run_pathfinder(ex, a);
    expect("same seed, same draws", r1.draws == r2.draws);

    PathfinderConfig b = a;
    b.seed = 8;
    PathfinderResult r3 = run_pathfinder(ex, b);
    expect("new seed, new draws", r1.draws != r3.draws);
  }

  // ---- inits are shared with NUTS --------------------------------------
  // The comparison contract, same as WALNUTS's: for a matched (seed,
  // chain_id) Pathfinder starts its L-BFGS from the exact point run_nuts
  // draws, so a NUTS-vs-Pathfinder run compares methods and not starting
  // places. Path iterate 0 is that point, so its lp is the oracle.
  {
    Executor ex(testmodels::std_normal(4));
    ex.value_ptr(1)[0] = 0.0;
    ex.value_ptr(2)[0] = 1.0;

    PathfinderConfig cfg;
    cfg.seed = 99;
    cfg.chain_id = 3;
    cfg.num_draws = 50;
    PathfinderResult r = run_pathfinder(ex, cfg);
    const auto p = cmdstan_init_point(ex, cfg.seed, cfg.chain_id,
                                      cfg.init_radius, nullptr);
    for (size_t i = 0; i < p.size(); ++i) ex.params_data()[i] = p[i];
    const double init_lp = ex.forward();
    expect("path non-empty", !r.path.empty());
    if (!r.path.empty())
      expect_in("path starts at the NUTS init", r.path[0].lp, init_lp - 1e-8,
                init_lp + 1e-8);
  }

  // ---- explicit init ---------------------------------------------------
  {
    Executor ex(testmodels::std_normal(2));
    ex.value_ptr(1)[0] = 0.0;
    ex.value_ptr(2)[0] = 1.0;

    const double start[2] = {1.5, -1.5};
    PathfinderConfig cfg;
    cfg.seed = 11;
    cfg.num_draws = 50;
    cfg.init = start;
    PathfinderResult r = run_pathfinder(ex, cfg);
    ex.params_data()[0] = start[0];
    ex.params_data()[1] = start[1];
    const double want = ex.forward();
    expect("explicit init path non-empty", !r.path.empty());
    if (!r.path.empty())
      expect_in("path starts at the explicit init", r.path[0].lp, want - 1e-8,
                want + 1e-8);
  }

  // ---- per-iterate observer fires live ---------------------------------
  {
    auto m = testmodels::eight_schools();
    Executor ex(std::move(m.graph));
    testmodels::fill_eight_schools_data(m, ex);

    std::vector<PathIterate> seen;
    PathfinderConfig cfg;
    cfg.seed = 4242;
    cfg.num_draws = 100;
    PathfinderResult r = run_pathfinder(
        ex, cfg, [&](const PathIterate& it) { seen.push_back(it); });
    expect("observer saw every iterate", seen.size() == r.path.size());
    // The ELBO picks an iterate on the way up, never the starting point.
    expect("selected iterate past the start",
           r.selected_iter > 0 && r.selected_iter < (int)r.path.size() &&
               r.path[(size_t)r.selected_iter].lp > r.path[0].lp);
    for (size_t i = 0; i < seen.size() && i < r.path.size(); ++i)
      expect("observer iterate " + std::to_string(i),
             seen[i].iter == r.path[i].iter && seen[i].lp == r.path[i].lp);
  }

  // ---- pareto_khat -----------------------------------------------------
  // An exponential sample has an exactly exponential tail, which is the
  // generalized Pareto with shape 0.
  {
    std::mt19937 rng(5);
    std::exponential_distribution<double> exp1(1.0);
    std::vector<double> x(4000);
    for (double& v : x) v = exp1(rng);
    expect_in("khat of an exponential tail", pareto_khat(x), -0.2, 0.2);

    // A Cauchy tail is Pareto with shape 1: heavy enough that importance
    // weights have no variance.
    std::cauchy_distribution<double> cauchy(0.0, 1.0);
    std::vector<double> c(4000);
    for (double& v : c) v = cauchy(rng);
    expect_in("khat of a Cauchy tail", pareto_khat(c), 0.7, 1.4);

    expect("khat of too few points is not a number",
           std::isnan(pareto_khat(std::vector<double>{1.0, 2.0, 3.0})));
  }

  if (failures) {
    std::printf("%d failures\n", failures);
    return 1;
  }
  std::printf("OK\n");
  return 0;
}
