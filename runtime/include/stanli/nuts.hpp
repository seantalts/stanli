#ifndef STANLI_NUTS_HPP
#define STANLI_NUTS_HPP

#include <stanli/graph.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace stanli {

struct NutsConfig {
  uint32_t seed = 0;
  // CmdStan's chain id, which is what makes chain c of a run a different
  // stream of the same seed rather than a different seed:
  // create_rng(seed, id). CmdStan numbers chains from 1.
  int chain_id = 1;
  int warmup = 1000;
  int samples = 1000;
  // Keep every `thin`-th post-warmup draw. `samples` counts transitions,
  // not kept draws, exactly as CmdStan's num_samples does, so a run with
  // thin=2 keeps samples/2 rows.
  int thin = 1;
  double delta = 0.8;  // target acceptance statistic
  // stan::mcmc::base_nuts defaults this to 5; CmdStan sets 10 and so must
  // we, or trajectories cap at 31 leapfrogs instead of 1023 and any model
  // needing deep trees is silently under-explored.
  int max_depth = 10;
  // Emit warmup draws ahead of the sampling draws, as CmdStan's
  // save_warmup does. They are not draws from the posterior; they exist
  // to diagnose adaptation.
  bool save_warmup = false;
  // Initialization, on the UNCONSTRAINED scale. CmdStan draws
  // uniform(-init_radius, init_radius) and rejects any draw whose log
  // density or gradient is not finite, retrying up to 100 times; radius 0
  // means start at the origin (CmdStan's `init=0`), which gets one
  // attempt because retrying an identical point cannot help.
  double init_radius = 2.0;
  // An explicit starting point, or null. Length must be n_params(). This
  // is the unconstrained scale because that is the scale stanli can read:
  // constrained-scale inits would need the inverse parameter transforms,
  // which do not exist here yet.
  const double* init = nullptr;
};

// One row per stored draw, in CmdStan's column order:
//   lp__, accept_stat__, stepsize__, treedepth__, n_leapfrog__,
//   divergent__, energy__
// This is the sampler-level oracle the gradient rig cannot be: comparing
// these against a CmdStan run catches configuration divergence (a wrong
// max tree depth, a wrong adaptation target) that pointwise gradient
// verification is structurally blind to.
struct SamplerStats {
  std::vector<std::array<double, 7>> rows;
};

// Optional per-transition observer for streaming consumers (the browser
// worker's live plots): called after every transition with the phase and
// the current unconstrained point.
using DrawObserver =
    std::function<void(int64_t i, bool warmup, const double* q)>;

// Adaptive diagonal-metric NUTS (stan::mcmc::adapt_diag_e_nuts) over the
// executor's log_prob_grad. Returns one unconstrained parameter vector per
// stored draw. `stats`, when non-null, receives one row per stored draw.
std::vector<std::vector<double>> run_nuts(Executor& ex, const NutsConfig& cfg,
                                          SamplerStats* stats = nullptr,
                                          const DrawObserver& observe = {});

// ---- multi-chain -----------------------------------------------------------

struct ChainResult {
  std::vector<std::vector<double>> draws;
  SamplerStats stats;
  // Empty on success. A chain that fails does not take the run down with
  // it -- CmdStan reports a failed chain and keeps the others -- so the
  // caller decides what a partial run is worth.
  std::string error;
};

// True when this build can run chains in real threads. stan-math's
// autodiff stack is a plain static unless STAN_THREADS is defined, in
// which case it is thread_local; the legacy kernels and tape islands
// build NESTED var tapes on that stack, so without STAN_THREADS two
// chains in two threads would quietly corrupt each other's tape. When
// this is false, run_nuts_chains ignores n_threads and runs sequentially.
bool thread_safe_build();

// Run one chain per executor. Chain c uses create_rng(seed, chain_id + c),
// which is CmdStan's convention, so a matched seed means a matched stream
// per chain. Each executor must be distinct: the arenas are per-evaluation
// mutable state.
//
// n_threads <= 1 runs sequentially. Larger values are honoured only on a
// thread_safe_build(); elsewhere they are clamped to 1, because the
// alternative is a wrong answer rather than a slow one.
std::vector<ChainResult> run_nuts_chains(
    const std::vector<Executor*>& execs, const NutsConfig& cfg,
    int n_threads = 1, const DrawObserver& observe = {});

// Build `n` executors over the same compiled graph, copying it out of an
// already-bound one. The caller keeps ownership; `src` is not modified.
// Data and constant fills are already in the graph's slot values, so the
// copies come out bound.
std::vector<std::unique_ptr<Executor>> clone_executors(const Executor& src,
                                                       int n);

}  // namespace stanli

#endif
