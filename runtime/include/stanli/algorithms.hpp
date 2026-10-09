// Stan's inference algorithms other than NUTS: fixed_param, Laplace
// sampling, multi-path Pathfinder, and ADVI.
//
// Each is Stan's own service (stan/services/) run on the executor through a
// model adapter that behaves like a generated Stan model: rejections throw,
// and generated quantities draw from the service's generator in the
// service's order, so a seed names the run it names in CmdStan. Nothing here
// reimplements a method.
//
// Unlike run_nuts and run_pathfinder, these return CONSTRAINED rows: the
// services interleave generated-quantity draws with their own, so the rows
// have to be written as the service goes.
#ifndef STANLI_ALGORITHMS_HPP
#define STANLI_ALGORITHMS_HPP

#include <stanli/graph.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace stanli {

class WaRng;

// What a run needs from whoever owns the model.
struct AlgorithmHost {
  // The CSV columns, and the row that fills them at unconstrained `q`,
  // drawing generated quantities from `rng`. A domain_error is a rejected
  // row. With no row, the columns are the unconstrained parameters.
  std::vector<std::string> names;
  std::function<void(const double* q, double* out, WaRng& rng)> row;
  // What the service logs: level 0 info, 1 warning, 2 error.
  std::function<void(int level, const std::string& text)> log;
  // Asked on the calling thread about every 100 ms; true stops the run.
  std::function<bool()> poll;
};

struct AlgorithmResult {
  int64_t n_columns = 0;
  // Row-major, n_columns per draw.
  std::vector<double> values;
  // Per draw, where the algorithm has them: the model's log density at the
  // draw, the approximation's, and the Pathfinder path the draw came from.
  std::vector<double> lp;
  std::vector<double> lp_approx;
  std::vector<double> path;
  // ADVI only: the columns at the mean of the approximation.
  std::vector<double> mean;
  int return_code = 0;  // 0 = success
  bool interrupted = false;
  std::string message;

  int64_t rows() const {
    return n_columns > 0 ? (int64_t)values.size() / n_columns : 0;
  }
};

// ---- fixed_param -----------------------------------------------------------
// The parameters stay at the starting point; only generated quantities move.
struct FixedParamConfig {
  uint32_t seed = 1;
  int chain_id = 1;
  int samples = 1000;
  int thin = 1;
  double init_radius = 2.0;
  const double* init = nullptr;  // unconstrained, or null for random
  int refresh = 0;               // log progress every `refresh` draws
};

// One chain. stan::services::sample::fixed_param.
AlgorithmResult run_fixed_param(Executor& ex, const AlgorithmHost& host,
                                const FixedParamConfig& cfg);

// ---- Laplace ---------------------------------------------------------------
struct LaplaceConfig {
  uint32_t seed = 1;
  int draws = 1000;
  // Must be true, for the reason OptimizeConfig::jacobian must be.
  bool jacobian = true;
  bool calculate_lp = true;
  int refresh = 0;
};

// Draws from the normal approximation at `mode` (unconstrained, usually
// run_optimize's). stan::services::laplace_sample. lp is log_p__, lp_approx
// is log_q__.
AlgorithmResult run_laplace(Executor& ex, const AlgorithmHost& host,
                            const double* mode, const LaplaceConfig& cfg);

// ---- Pathfinder, single- or multi-path -------------------------------------
struct PathfinderRunConfig {
  uint32_t seed = 1;
  int chain_id = 1;  // Stan's stride_id; path p uses chain_id + p
  int num_paths = 4;
  int num_draws = 1000;       // per path
  int num_psis_draws = 1000;  // returned after resampling
  int num_elbo_draws = 25;
  int max_lbfgs_iters = 1000;
  int history_size = 5;
  double init_alpha = 0.001;
  double tol_obj = 1e-12;
  double tol_rel_obj = 1e4;
  double tol_grad = 1e-8;
  double tol_rel_grad = 1e7;
  double tol_param = 1e-8;
  double init_radius = 2.0;
  // num_paths * n_params unconstrained values, path-major, or null.
  const double* inits = nullptr;
  bool psis_resample = true;
  bool calculate_lp = true;
  int refresh = 0;
};

// The most rows a run under `cfg` can return.
int64_t pathfinder_max_draws(const PathfinderRunConfig& cfg);

// stan::services::pathfinder::pathfinder_lbfgs_multi, or _single when
// num_paths is 1, which is the choice CmdStan makes. Paths run one after
// another: they share one executor.
AlgorithmResult run_pathfinder_paths(Executor& ex, const AlgorithmHost& host,
                                     const PathfinderRunConfig& cfg);

// ---- ADVI ------------------------------------------------------------------
struct VariationalConfig {
  uint32_t seed = 1;
  int chain_id = 1;
  bool fullrank = false;
  int iter = 10000;
  int grad_samples = 1;
  int elbo_samples = 100;
  double eta = 1.0;
  bool adapt_engaged = true;
  int adapt_iter = 50;
  double tol_rel_obj = 0.01;
  int eval_elbo = 100;
  int output_samples = 1000;
  double init_radius = 2.0;
  const double* init = nullptr;  // unconstrained, or null for random
};

// stan::services::experimental::advi::meanfield / fullrank. lp is log_p__,
// lp_approx is log_g__.
AlgorithmResult run_variational(Executor& ex, const AlgorithmHost& host,
                                const VariationalConfig& cfg);

}  // namespace stanli

#endif
