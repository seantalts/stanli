// Single-path Pathfinder end to end through stan's own service driving the
// executor's gradient, plus the Pareto k-hat diagnostic that says whether
// its draws are worth trusting.
//
// Pathfinder is an approximation, so the checks are looser than the
// sampler tests: what they pin down is that draws arrive at all (the
// service writes them through an Eigen row-vector overload that a
// std::vector-only collector drops on the floor), that they land on the
// right posterior, and that the init is the one NUTS would have used.
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

static stanli::Graph std_normal_graph(int D) {
  using namespace stanli;
  Graph g;
  const int x = g.add_slot(D, true);
  const int zero = g.add_slot(1, false);
  const int one = g.add_slot(1, false);
  const int lp = g.add_slot(1, false);
  g.add_op(OP_NORMAL_LPDF, {x, zero, one}, lp);
  g.result_slot = lp;
  return g;
}

int main() {
  using namespace stanli;

  // ---- 10-dim standard normal: Pathfinder's target case -----------------
  // A Gaussian posterior is exactly what the Taylor approximation along
  // the L-BFGS path represents, so the draws should be near-exact and
  // k-hat should say so.
  {
    const int D = 10;
    Executor ex(std_normal_graph(D));
    ex.value_ptr(1)[0] = 0.0;
    ex.value_ptr(2)[0] = 1.0;

    PathfinderConfig cfg;
    cfg.seed = 20260815;
    cfg.num_draws = 2000;
    PathfinderResult r = run_pathfinder(ex, cfg);
    expect("normal return code " + std::to_string(r.return_code),
           r.return_code == 0);
    expect("normal draw count " + std::to_string(r.draws.size()),
           (int)r.draws.size() == cfg.num_draws);
    expect("normal lp per draw", r.lp.size() == r.draws.size());
    expect("normal lp_approx per draw", r.lp_approx.size() == r.draws.size());
    if (r.draws.empty()) {
      std::printf("%d failures\n", ++failures);
      return 1;
    }
    for (int d = 0; d < D; ++d) {
      double m = 0, m2 = 0;
      for (const auto& q : r.draws) m += q[d];
      m /= r.draws.size();
      for (const auto& q : r.draws) m2 += (q[d] - m) * (q[d] - m);
      const double sd = std::sqrt(m2 / (r.draws.size() - 1));
      expect_in("norm mean[" + std::to_string(d) + "]", m, -0.15, 0.15);
      expect_in("norm sd[" + std::to_string(d) + "]", sd, 0.85, 1.15);
    }
    expect_in("norm khat", r.khat, -1.0, 0.5);
    expect("norm path non-empty", !r.path.empty());
    expect("norm selected iterate in path",
           r.selected_iter >= 0 && r.selected_iter < (int)r.path.size());
  }

  // ---- eight schools: a funnel, which Pathfinder approximates badly ----
  // The band is far wider and far lower than the sampler tests' [2.5, 6.5]
  // because a single normal approximation cannot cover this posterior: it
  // lands near mu = 1.5 with a quarter of NUTS's spread. That is the
  // method, not the wiring, and it is exactly what the comparison view is
  // for. k-hat stays small because a narrow proposal has light importance
  // ratios -- it says the weights are stable, never that they cover.
  {
    auto m = testmodels::eight_schools();
    Executor ex(std::move(m.graph));
    testmodels::fill_eight_schools_data(m, ex);

    PathfinderConfig cfg;
    cfg.seed = 8675309;
    cfg.num_draws = 2000;
    PathfinderResult r = run_pathfinder(ex, cfg);
    expect("es return code " + std::to_string(r.return_code),
           r.return_code == 0);
    if (r.draws.empty()) {
      std::printf("%d failures\n", ++failures);
      return 1;
    }
    double mu_mean = 0;
    int nan_count = 0;
    for (const auto& q : r.draws) {
      for (double v : q)
        if (std::isnan(v)) ++nan_count;
      mu_mean += q[0];
    }
    mu_mean /= r.draws.size();
    expect_in("es nan_count", nan_count, 0, 0);
    expect_in("es mu mean", mu_mean, -1.0, 4.0);
    expect("es khat finite", std::isfinite(r.khat));

    // The path climbs: the ELBO picks an iterate on the way up, never the
    // starting point the optimizer was handed.
    expect("es path non-empty", r.path.size() > 1);
    expect("es selected iterate in path",
           r.selected_iter > 0 && r.selected_iter < (int)r.path.size());
    if (r.selected_iter > 0 && r.selected_iter < (int)r.path.size())
      expect("es selected lp above the start",
             r.path[(size_t)r.selected_iter].lp > r.path[0].lp);
    expect("es selected elbo finite", std::isfinite(r.selected_elbo));
  }

  // ---- determinism -----------------------------------------------------
  {
    Executor ex(std_normal_graph(3));
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
    Executor ex(std_normal_graph(4));
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
    Executor ex(std_normal_graph(2));
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
