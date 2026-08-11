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
    logp = ex->gradient(grad.data());
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

  // Same initialization contract as run_nuts: uniform(-radius, radius)
  // on the unconstrained scale, kept only when the log density and the
  // whole gradient are finite, up to 100 attempts. A fixed point (an
  // explicit init or radius 0) gets one attempt; retrying an identical
  // point cannot help.
  Eigen::VectorXd q(n);
  {
    std::uniform_real_distribution<double> init_dist(-cfg.init_radius,
                                                     cfg.init_radius);
    const bool fixed_point = cfg.init != nullptr || cfg.init_radius == 0.0;
    const int kMaxInitAttempts = fixed_point ? 1 : 100;
    std::vector<double> grad((size_t)n);
    bool ok = false;
    for (int attempt = 0; attempt < kMaxInitAttempts && !ok; ++attempt) {
      if (cfg.init != nullptr)
        for (int64_t i = 0; i < n; ++i) q(i) = cfg.init[i];
      else if (cfg.init_radius == 0.0)
        q.setZero();
      else
        for (int64_t i = 0; i < n; ++i) q(i) = init_dist(rng);
      for (int64_t i = 0; i < n; ++i) ex.params_data()[i] = q(i);
      try {
        const double lp = ex.forward_value_only();
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
    }
    if (!ok)
      throw std::runtime_error(
          "initialization failed: no draw in (-init_radius, init_radius) "
          "had a finite log density and gradient after 100 attempts");
  }

  std::vector<std::vector<double>> draws;
  draws.reserve((size_t)cfg.samples);
  CollectHandler handler{&draws, stats, &observe};
  ExecLogpGrad logp_grad{&ex};

  const walnutpie::InitChainConfig init_cfg(cfg.init_step_size, q,
                                            Eigen::VectorXd::Ones(n));
  const walnutpie::WarmupConfig warmup_cfg =
      walnutpie::WarmupConfigBuilder{}
          .min_max_iter((size_t)cfg.warmup, (size_t)cfg.warmup)
          .build();
  const walnutpie::SamplingConfig sampling_cfg =
      walnutpie::SamplingConfigBuilder{}
          .min_max_iter((size_t)cfg.samples, (size_t)cfg.samples)
          .max_trajectory_doublings((size_t)cfg.max_depth)
          .max_step_halvings((size_t)cfg.max_step_halvings)
          .max_hamiltonian_error(cfg.max_error)
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
