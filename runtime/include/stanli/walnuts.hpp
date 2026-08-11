#ifndef STANLI_WALNUTS_HPP
#define STANLI_WALNUTS_HPP

#include <stanli/graph.hpp>
#include <stanli/nuts.hpp>  // DrawObserver, shared with NUTS on purpose

#include <cstdint>
#include <vector>

namespace stanli {

// WALNUTS: within-orbit adaptive step-length NUTS (arXiv:2506.18746),
// via the vendored walnutpie implementation. Where NUTS picks one step
// size per chain and lives with it, WALNUTS halves the step within a
// trajectory wherever the local error demands it, which is what makes
// funnel-like geometry tractable without cranking delta.
struct WalnutsConfig {
  uint32_t seed = 0;
  int chain_id = 1;
  int warmup = 1000;
  int samples = 1000;
  // Trajectory doublings cap, the analogue of NUTS max_depth.
  int max_depth = 10;
  // Within-trajectory step control: a macro step may be cut in half up
  // to this many times, and the joint density may drift at most
  // max_error nats across a macro step before halving kicks in.
  int max_step_halvings = 5;
  double max_error = 0.5;
  // Initial macro step size; warmup's Adam optimizer adapts it.
  double init_step_size = 1.0;
  // Initialization on the unconstrained scale, same contract as
  // NutsConfig: uniform(-init_radius, init_radius) with finite lp and
  // gradient required, or an explicit point.
  double init_radius = 2.0;
  const double* init = nullptr;
};

// Runs adaptive-walnuts warmup then fixed-parameter WALNUTS sampling.
// Returns one unconstrained parameter vector per stored draw. `stats`,
// when non-null, receives per-draw rows shaped like SamplerStats but
// with WALNUTS's own diagnostics (see walnuts.cpp for the columns).
std::vector<std::vector<double>> run_walnuts(Executor& ex,
                                             const WalnutsConfig& cfg,
                                             SamplerStats* stats = nullptr,
                                             const DrawObserver& observe = {});

}  // namespace stanli

#endif
