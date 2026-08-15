// WALNUTS over the executor's gradient, via the vendored walnutpie
// headers. This is the only C++20 translation unit in the runtime
// (walnutpie is written against concepts); it deliberately includes no
// stan-math, only the executor interface and Eigen, so the newer
// standard never touches the rest of the build.
#include <stanli/walnuts.hpp>

#include <Eigen/Dense>

#include <walnutpie/adaptive_walnuts.hpp>
#include <walnutpie/config.hpp>
#include <walnutpie/walnuts.hpp>

#include <cmath>
#include <limits>
#include <random>
#include <stdexcept>
#include <vector>

namespace stanli {

namespace {

// walnutpie's model interface: a const callable setting logp and grad.
// Mutating the executor through a pointer keeps the callable const the
// way the LogpGrad concept wants; the arenas are per-evaluation state.
struct ExecLogpGrad {
  Executor* ex;
  void operator()(const Eigen::VectorXd& x, double& logp,
                  Eigen::VectorXd& grad) const {
    const int64_t n = ex->n_params();
    for (int64_t i = 0; i < n; ++i) ex->params_data()[i] = x(i);
    grad.resize(n);
    // The catch matters when this callable is used directly (the step
    // search below); through walnutpie it is redundant with
    // NoExceptLogpGrad, harmlessly.
    try {
      logp = ex->gradient(grad.data());
    } catch (const std::exception&) {
      logp = -std::numeric_limits<double>::infinity();
    }
    // walnutpie assumes the density throws or is finite: its acceptance
    // statistic is exp(-|logp - logp_next|), and a NaN there -- which
    // stanli's kernels produce freely, log of a negative is a NaN and
    // not an exception -- poisons the Adam step-size estimate for good,
    // surfacing as "macro_time must be in (0, inf)" at the end of
    // warmup. Out-of-support points therefore report exactly what
    // walnutpie's own exception path reports: -inf, zero gradient. A
    // non-finite gradient at a finite logp gets the same treatment,
    // because one more leapfrog step through it turns the position and
    // then the momentum into NaN anyway.
    bool ok = std::isfinite(logp);
    for (int64_t i = 0; ok && i < n; ++i) ok = std::isfinite(grad(i));
    if (!ok) {
      logp = -std::numeric_limits<double>::infinity();
      grad.setZero();
    }
  }
};

// The chain handler walnutpie calls back on: collects stored draws,
// per-draw stats, and forwards to the streaming observer. Stats rows
// keep SamplerStats' 7-column CmdStan shape so consumers can share
// code with NUTS; columns WALNUTS has no analogue for are NaN.
//   lp__, accept_stat__(NaN), stepsize__, treedepth__(NaN),
//   n_leapfrog__(NaN), divergent__(NaN), energy__(NaN)
struct CollectHandler {
  std::vector<std::vector<double>>* out;
  SamplerStats* stats;
  const DrawObserver* observe;
  int64_t warm_i = 0;
  int64_t samp_i = 0;
  double cur_step = std::numeric_limits<double>::quiet_NaN();

  void on_warmup(const Eigen::VectorXd& q, double /*lp*/, double step,
                 const Eigen::VectorXd& /*inv_mass*/) {
    cur_step = step;
    if (*observe) (*observe)(warm_i, true, q.data());
    ++warm_i;
  }
  void on_warmup_complete(double step, const Eigen::VectorXd& /*inv_mass*/) {
    cur_step = step;
  }
  void on_sample(const Eigen::VectorXd& q, double lp) {
    out->emplace_back(q.data(), q.data() + q.size());
    if (stats) {
      const double nan = std::numeric_limits<double>::quiet_NaN();
      stats->rows.push_back({lp, nan, cur_step, nan, nan, nan, nan});
    }
    if (*observe) (*observe)(samp_i, false, q.data());
    ++samp_i;
  }
  void on_logp_exception(const Eigen::VectorXd& /*q*/,
                         const std::exception& /*e*/) noexcept {
    // walnutpie already treats a throwing density as logp = -inf, which
    // is the same "reject this point" semantics NUTS gets from stan's
    // samplers; nothing further to do.
  }
};

}  // namespace

std::vector<std::vector<double>> run_walnuts(Executor& ex,
                                             const WalnutsConfig& cfg,
                                             SamplerStats* stats,
                                             const DrawObserver& observe) {
  const int64_t n = ex.n_params();

  // The sampler's generator, a distinct stream per (seed, chain) pair.
  // The INIT does not come from here -- it comes from the CmdStan stream
  // below, shared with run_nuts -- but WALNUTS's own consumption pattern
  // has no CmdStan analogue to match, so for the step search and the
  // transitions reproducibility per seed is the whole contract.
  std::seed_seq seq{cfg.seed, static_cast<uint32_t>(cfg.chain_id)};
  std::mt19937_64 rng(seq);

  // The SAME starting point run_nuts draws for this (seed, chain_id) --
  // CmdStan's stream, first acceptable draw. Init policy is the service
  // layer's, not any one sampler's, and sharing it is what makes a
  // NUTS-vs-WALNUTS run with a matched seed a controlled comparison: both
  // samplers start from the identical point, and how each handles a bad
  // one is visible rather than papered over. (An earlier version chose
  // the best of 16 candidates here, which hid walnutpie's tail-init
  // sensitivity from the comparison; that sensitivity is reported
  // upstream instead.)
  const std::vector<double> q0 =
      cmdstan_init_point(ex, cfg.seed, cfg.chain_id, cfg.init_radius, cfg.init);
  Eigen::VectorXd q =
      Eigen::Map<const Eigen::VectorXd>(q0.data(), (Eigen::Index)n);

  std::vector<std::vector<double>> draws;
  draws.reserve((size_t)cfg.samples);
  CollectHandler handler{&draws, stats, &observe};
  ExecLogpGrad logp_grad{&ex};

  // Stan's find_reasonable_epsilon, on the unit metric: one leapfrog step
  // from the init, double or halve until the Hamiltonian error crosses
  // log(2). WALNUTS needs this MORE than NUTS does: NUTS's progressive
  // sampling still moves off a divergent trajectory's partial tree, but a
  // WALNUTS extension whose energy error survives max_step_halvings
  // simply fails, so a chain whose step is far too large for its
  // neighborhood does not move at all and Adam only learns from the ~1%
  // per iteration it drifts. From a tail init that deadlock lasted the
  // whole warmup.
  double step0 = cfg.init_step_size;
  {
    std::normal_distribution<double> stdnorm(0.0, 1.0);
    Eigen::VectorXd rho(n);
    for (int64_t i = 0; i < n; ++i) rho(i) = stdnorm(rng);
    double lp_q;
    Eigen::VectorXd g(n);
    logp_grad(q, lp_q, g);
    const double h0 = lp_q - 0.5 * rho.squaredNorm();
    const auto h_err = [&](double step) {
      Eigen::VectorXd r = rho + 0.5 * step * g;
      Eigen::VectorXd q2 = q + step * r;
      double lp2;
      Eigen::VectorXd g2(n);
      logp_grad(q2, lp2, g2);
      r += 0.5 * step * g2;
      return (lp2 - 0.5 * r.squaredNorm()) - h0;
    };
    const double thresh = std::log(2.0);
    const bool shrink = !(std::fabs(h_err(step0)) < thresh);
    for (int it = 0; it < 60; ++it) {
      const double err = std::fabs(h_err(step0));
      if (shrink ? (err < thresh) : (err > thresh)) break;
      step0 *= shrink ? 0.5 : 2.0;
    }
    // Growing overshoots by construction: the loop stops at the first
    // step past the threshold, so hand back the last one under it.
    if (!shrink) step0 *= 0.5;
  }

  const walnutpie::SamplingConfig sampling_cfg =
      walnutpie::SamplingConfigBuilder{}
          .min_max_iter((size_t)cfg.samples, (size_t)cfg.samples)
          .max_trajectory_doublings((size_t)cfg.max_depth)
          .max_step_halvings((size_t)cfg.max_step_halvings)
          .max_hamiltonian_error(cfg.max_error)
          .build();

  // Warmup itself stays walnutpie's algorithm, untouched: only the two
  // inputs its API asks the caller for -- the starting position and the
  // starting step -- are chosen more carefully above.
  const walnutpie::InitChainConfig init_cfg(step0, q, Eigen::VectorXd::Ones(n));
  const walnutpie::WarmupConfig warmup_cfg =
      walnutpie::WarmupConfigBuilder{}
          .min_max_iter((size_t)cfg.warmup, (size_t)cfg.warmup)
          .build();
  walnutpie::AdaptiveWalnuts<ExecLogpGrad, std::mt19937_64, CollectHandler>
      adaptive(rng, handler, logp_grad, init_cfg, warmup_cfg, sampling_cfg);
  for (int i = 0; i < cfg.warmup; ++i) adaptive();

  // sampler() freezes the adapted step size, mass matrix, and micro-step
  // floor, and fires on_warmup_complete; the draws it produces are the
  // Markov chain.
  auto sampler = adaptive.sampler();
  for (int i = 0; i < cfg.samples; ++i) sampler();

  return draws;
}

}  // namespace stanli
