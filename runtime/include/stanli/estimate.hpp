// Point estimates and variational draws: L-BFGS, Laplace, and Pathfinder.
//
// All three are stan's own services (stan/services/optimize/,
// stan/services/pathfinder/) driven through the same ExecutorModel the
// sampler uses. They are algorithms over a log density and its gradient,
// which the executor already provides, so nothing here reimplements a
// method -- it supplies the model concept the services expect and
// collects what they write.
//
// Pathfinder matters twice: as a deliverable, and as the modern answer to
// "where should the chains start", which is what NutsConfig::init takes.
#ifndef STANLI_ESTIMATE_HPP
#define STANLI_ESTIMATE_HPP

#include <stanli/graph.hpp>
#include <stanli/model_adapter.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace stanli {

struct OptimizeConfig {
  uint32_t seed = 1;
  int chain_id = 1;
  int iter = 2000;
  // MUST be true, and it is checked rather than assumed.
  //
  // CmdStan's `optimize` defaults to jacobian=0 -- the penalized maximum
  // likelihood -- and stanli cannot offer that. The Jacobian terms are
  // folded into the graph at lowering time and the adapter's log_prob
  // ignores the template flag entirely (see model_adapter.hpp), so a
  // `false` here would silently return the posterior MODE while claiming
  // to be the other thing. The two differ for any constrained parameter,
  // which is most models.
  //
  // Excluding them is possible in principle -- lower.cpp already collects
  // jac_slots separately -- and would be the fix; until then this refuses.
  bool jacobian = true;
  double init_alpha = 0.001;
  double tol_obj = 1e-12;
  double tol_rel_obj = 1e4;
  double tol_grad = 1e-8;
  double tol_rel_grad = 1e7;
  double tol_param = 1e-8;
  int history_size = 5;
  double init_radius = 2.0;
  const double* init = nullptr;  // unconstrained, or null for random
};

struct OptimizeResult {
  double lp = 0;
  std::vector<double> unconstrained;  // the mode, on the sampler's scale
  std::vector<double> values;         // every CSV column at that point
  std::vector<std::string> names;
  int return_code = 0;  // 0 = converged
  std::string message;
};

// L-BFGS, the same one CmdStan's `optimize` runs.
OptimizeResult run_optimize(Executor& ex, const WriteArray* wa,
                            const OptimizeConfig& cfg);

// ---- Pathfinder ------------------------------------------------------------
// Single path only. Multi-path needs real TBB (tbb::parallel_for in
// stan/services/pathfinder/multi.hpp, tbb::parallel_invoke in psis.hpp)
// and this build stubs TBB out, so it does not link.

struct PathfinderConfig {
  uint32_t seed = 1;
  // Pathfinder's `stride_id`, which is CmdStan's chain id: it selects the
  // rng stream, so a matched (seed, chain_id) puts Pathfinder on the same
  // starting point run_nuts and run_walnuts use.
  int chain_id = 1;
  int num_draws = 1000;
  int num_elbo_draws = 25;
  int num_iterations = 1000;
  int history_size = 5;
  double init_alpha = 0.001;
  double tol_obj = 1e-12;
  double tol_rel_obj = 1e4;
  double tol_grad = 1e-8;
  double tol_rel_grad = 1e7;
  double tol_param = 1e-8;
  double init_radius = 2.0;
  const double* init = nullptr;  // unconstrained, or null for random
};

// One point on the L-BFGS path. `iter` 0 is the starting point.
struct PathIterate {
  int iter = 0;
  double lp = 0;
};

// Called as each iterate is reached, for a live view of the climb.
using PathObserver = std::function<void(const PathIterate&)>;

struct PathfinderResult {
  // Unconstrained, one vector per draw, the orientation run_nuts returns.
  std::vector<std::vector<double>> draws;
  std::vector<double> lp;         // the model's log density at each draw
  std::vector<double> lp_approx;  // the normal approximation's, at each draw
  std::vector<PathIterate> path;
  // Index into `path` of the iterate whose approximation maximised the
  // ELBO, and that ELBO. -1 and NaN when the run failed before choosing.
  int selected_iter = -1;
  double selected_elbo = 0;
  // Pareto shape of the importance ratios lp - lp_approx. Above 0.7 the
  // weights have no usable variance and the draws should not be trusted.
  double khat = 0;
  double elapsed_ms = 0;
  int return_code = 0;  // 0 = success
  std::string message;
};

PathfinderResult run_pathfinder(Executor& ex, const PathfinderConfig& cfg,
                                const PathObserver& observe = {});

// Zhang & Stephens (2009) profile-likelihood fit of a generalized Pareto
// to the largest min(0.2 S, 3 sqrt(S)) of `log_ratios`, returning the
// shape. NaN when there are too few points to fit. This is the tail
// diagnostic from the PSIS paper (arXiv:1507.02646).
double pareto_khat(std::vector<double> log_ratios);

}  // namespace stanli

#endif
