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

  // One generator for init and sampling, a distinct stream per
  // (seed, chain) pair. WALNUTS is a different sampler with a different
  // consumption pattern, so there is no CmdStan stream to match the way
  // nuts.cpp must; reproducibility per seed is the whole contract.
  std::seed_seq seq{cfg.seed, static_cast<uint32_t>(cfg.chain_id)};
  std::mt19937_64 rng(seq);

  // Initialization: uniform(-radius, radius) on the unconstrained scale,
  // finite log density and gradient required, up to 100 attempts as in
  // run_nuts -- but among the first 16 finite candidates the BEST log
  // density wins, where NUTS takes the first. WALNUTS earns the extra 15
  // evaluations: walnutpie's mass adaptation starts learning the metric
  // immediately, and one draw from the far tail of a stiff posterior
  // (lotka_volterra had inits at lp -16000 against a posterior living
  // near -12) collapses the inverse mass to the tail's huge gradients,
  // leaving a chain that crawls there for the whole run. Any finite init
  // is a valid init, so preferring a better one costs nothing in
  // correctness. A fixed point (an explicit init or radius 0) is taken
  // as given, once.
  Eigen::VectorXd q(n);
  {
    std::uniform_real_distribution<double> init_dist(-cfg.init_radius,
                                                     cfg.init_radius);
    const bool fixed_point = cfg.init != nullptr || cfg.init_radius == 0.0;
    const int kMaxInitAttempts = fixed_point ? 1 : 100;
    const int kWantCandidates = fixed_point ? 1 : 16;
    std::vector<double> grad((size_t)n);
    Eigen::VectorXd best_q(n);
    double best_lp = -std::numeric_limits<double>::infinity();
    int found = 0;
    for (int attempt = 0; attempt < kMaxInitAttempts && found < kWantCandidates;
         ++attempt) {
      if (cfg.init != nullptr)
        for (int64_t i = 0; i < n; ++i) q(i) = cfg.init[i];
      else if (cfg.init_radius == 0.0)
        q.setZero();
      else
        for (int64_t i = 0; i < n; ++i) q(i) = init_dist(rng);
      for (int64_t i = 0; i < n; ++i) ex.params_data()[i] = q(i);
      bool ok = false;
      double lp = 0;
      try {
        lp = ex.forward_value_only();
        ok = std::isfinite(lp);
        if (ok) {
          const double glp = ex.gradient(grad.data());
          ok = std::isfinite(glp);
          for (int64_t i = 0; ok && i < n; ++i)
            ok = std::isfinite(grad[(size_t)i]);
        }
      } catch (const std::exception&) {
        ok = false;
      }
      if (ok) {
        ++found;
        if (lp > best_lp) {
          best_lp = lp;
          best_q = q;
        }
      }
    }
    if (found == 0)
      throw std::runtime_error(
          "initialization failed: no draw in (-init_radius, init_radius) "
          "had a finite log density and gradient after 100 attempts");
    q = best_q;
  }

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
