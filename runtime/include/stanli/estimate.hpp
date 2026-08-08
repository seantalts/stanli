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
  int return_code = 0;                // 0 = converged
  std::string message;
};

// L-BFGS, the same one CmdStan's `optimize` runs.
OptimizeResult run_optimize(Executor& ex, const WriteArray* wa,
                            const OptimizeConfig& cfg);

// Pathfinder is NOT here yet, deliberately. ExecutorModel now satisfies
// enough of the model concept for stan's service to compile and run
// against it, but the draws come back empty -- the parameter writer is
// never called -- and shipping an entry point that silently returns
// nothing is worse than not shipping one. Multi-path additionally needs
// real TBB, which this build stubs out (tests/tbb_stub.cpp), so its
// tbb::parallel_for does not link at all.

}  // namespace stanli

#endif
