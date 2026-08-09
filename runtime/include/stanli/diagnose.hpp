// Multi-chain summaries and convergence diagnostics.
//
// The estimators are stan's own (stan/analyze/mcmc/): rank-normalized
// split-Rhat and bulk/tail ESS from Vehtari et al. 2021, and stan's MCSE.
// That is deliberate reuse rather than convenience -- these are the
// numbers `stansummary` prints, and a diagnostic that disagreed with
// CmdStan's by a little would be worse than none at all.
//
// The one estimator written here is E-BFMI, which stan computes inside
// its diagnostic services rather than exposing: see ebfmi() below.
//
// Living in the runtime rather than in the Python wrapper is what lets
// the CLI, the wheel and the browser report the same numbers.
#ifndef STANLI_DIAGNOSE_HPP
#define STANLI_DIAGNOSE_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace stanli {

// Draws are chain-major and packed: draw i of chain c, column j lives at
// draws[(c * n_draws + i) * n_cols + j]. This is the layout the C ABI
// fills and the layout numpy reshapes to (chains, draws, cols) for free.
struct DrawSet {
  const double* draws = nullptr;
  int64_t n_chains = 0;
  int64_t n_draws = 0;
  int64_t n_cols = 0;
  const double* at(int64_t chain, int64_t draw) const {
    return draws + ((chain * n_draws) + draw) * n_cols;
  }
};

// One row of the summary table, in stansummary's column order plus the
// rank-normalized pair that replaced the old single Rhat.
struct ParamSummary {
  std::string name;
  double mean = 0, sd = 0;
  double mcse_mean = 0, mcse_sd = 0;
  double q5 = 0, q50 = 0, q95 = 0;
  double ess_bulk = 0, ess_tail = 0;
  // rhat is max(bulk, tail): a parameter is converged only if both are,
  // and one number is what a user scans a table for.
  double rhat = 0, rhat_bulk = 0, rhat_tail = 0;
};

// The seven sampler columns, per draw, in CmdStan's order. This mirrors
// SamplerStats (nuts.hpp) but flattened across chains the same way draws
// are, because the diagnostics below are computed across chains.
enum SamplerCol {
  COL_LP = 0,
  COL_ACCEPT_STAT = 1,
  COL_STEPSIZE = 2,
  COL_TREEDEPTH = 3,
  COL_N_LEAPFROG = 4,
  COL_DIVERGENT = 5,
  COL_ENERGY = 6,
  N_SAMPLER_COLS = 7
};

// Betancourt's checks, in the form his stan_utility asks them.
// Thresholds are named constants below rather than buried in the
// formatter, because a user who wants to argue with one should be able
// to find it.
struct FitDiagnostics {
  int64_t n_chains = 0, n_draws = 0;
  int max_depth = 10;  // the configured limit, to interpret saturation

  std::vector<int64_t> divergent_by_chain;
  std::vector<int64_t> max_treedepth_by_chain;
  std::vector<double> ebfmi_by_chain;
  std::vector<double> stepsize_by_chain;     // adapted, final
  std::vector<double> accept_stat_by_chain;  // mean over post-warmup

  int64_t n_divergent = 0;
  int64_t n_max_treedepth = 0;

  // Worst over all parameters, with the parameter named: a summary line
  // that says "1.04" without saying which parameter is not actionable.
  double max_rhat = 0;
  std::string max_rhat_param;
  double min_ess_bulk = 0;
  std::string min_ess_bulk_param;
  double min_ess_tail = 0;
  std::string min_ess_tail_param;
};

// Thresholds. Rhat 1.01 and ESS 100/chain are Vehtari et al. 2021, which
// is what the rank-normalized estimators above are from -- Betancourt's
// older 1.1 was calibrated for the older estimator and is too loose for
// this one. E-BFMI 0.3 is Stan's current threshold; Betancourt's
// original paper says 0.2 and Stan tightened it.
inline constexpr double kRhatThreshold = 1.01;
inline constexpr double kEssPerChainThreshold = 100.0;
inline constexpr double kEbfmiThreshold = 0.3;

// E-BFMI, the energy Bayesian fraction of missing information
// (Betancourt, "A Conceptual Introduction to Hamiltonian Monte Carlo",
// eq. 66). The estimator is the ratio of the mean square of successive
// energy differences to the marginal energy variance:
//
//   E-BFMI = sum_i (E_i - E_{i-1})^2 / sum_i (E_i - mean(E))^2
//
// Low values mean the momentum resampling is not moving the chain across
// the energy distribution, so the sampler explores the tails badly --
// the failure mode a heavy-tailed posterior produces and that Rhat and
// ESS are both blind to. Returns NaN for fewer than 2 draws or a
// degenerate (zero-variance) energy sequence.
double ebfmi(const double* energy, int64_t n, int64_t stride);

// Per-parameter summary over all chains. `names` may be shorter than
// n_cols; missing names come out empty.
std::vector<ParamSummary> summarize(const DrawSet& d,
                                    const std::vector<std::string>& names);

// Whole-fit diagnostics. `stats` is the sampler columns in the same
// chain-major packing as `d`, N_SAMPLER_COLS wide; pass a null stats
// pointer to compute only the Rhat/ESS half.
FitDiagnostics diagnose(const DrawSet& d,
                        const std::vector<ParamSummary>& summary,
                        const double* stats, int max_depth);

// stansummary's table. Column widths adapt to the data.
std::string format_summary(const std::vector<ParamSummary>& s);

// Betancourt's checks as prose: one line per check, each either a
// confirmation or a specific complaint with the number that failed and
// what to do about it. Empty problems produce "no problems detected".
std::string format_diagnostics(const FitDiagnostics& d);

}  // namespace stanli

#endif
