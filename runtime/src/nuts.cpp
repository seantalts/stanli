#include <stanli/nuts.hpp>

#include <stanli/model_adapter.hpp>

#include <stan/callbacks/logger.hpp>
#include <stan/math/rev/core/chainablestack.hpp>
#include <stan/mcmc/hmc/nuts/adapt_diag_e_nuts.hpp>
#include <stan/services/util/create_rng.hpp>

#include <boost/random/uniform_real_distribution.hpp>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <thread>

namespace stanli {

std::vector<std::vector<double>> run_nuts(Executor& ex,
                                          const NutsConfig& cfg,
                                          SamplerStats* stats,
                                          const DrawObserver& observe) {
  // CmdStan's generator, seeded CmdStan's way: same engine (mixmax in this
  // Stan version, ecuyer1988 in older ones -- create_rng is what tracks
  // that), same (0, 1, seed, chain) construction, chain 1 as the default
  // `id`. Before this the seeds named unrelated streams, so "seed 1" meant
  // a different starting point in each engine and any comparison of a
  // sampling run was comparing two different draws as much as two
  // samplers. With the stream matched and the same draw order below, the
  // initial point is the same one CmdStan starts from.
  using rng_t = stan::rng_t;
  ExecutorModel model(ex);
  rng_t rng = stan::services::util::create_rng(cfg.seed, cfg.chain_id);
  stan::mcmc::adapt_diag_e_nuts<ExecutorModel, rng_t> sampler(model, rng);
  stan::callbacks::logger logger;

  const int64_t n = ex.n_params();
  Eigen::VectorXd q(n);
  boost::random::uniform_real_distribution<double> init_dist(-cfg.init_radius,
                                                            cfg.init_radius);

  // CmdStan draws uniform(-2, 2) on the unconstrained scale and REJECTS the
  // draw unless both the log density and its whole gradient are finite,
  // retrying up to 100 times (stan::services::util::initialize). We took the
  // first draw unconditionally, so a model whose typical set only covers
  // part of that hypercube -- accel_gp's GP hyperparameters overflow exp()
  // over much of it -- failed outright on seeds whose first draw landed
  // badly, with the failure surfacing later as a stepsize-search error.
  {
    // An explicit init and a zero radius both name ONE point, so retrying
    // is pointless there: the same point fails the same way, and a
    // hundred identical attempts would only hide which of the two the
    // user asked for behind a generic message.
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
        // stan::io::random_var_context draws one per unconstrained
        // parameter, in declaration order, from this distribution.
        for (int64_t i = 0; i < n; ++i) q(i) = init_dist(rng);
      for (int64_t i = 0; i < n; ++i) ex.params_data()[i] = q(i);
      // A density that rejects its argument outright (stan-math throws on a
      // NaN location, an out-of-support outcome) counts as a rejected draw,
      // exactly as CmdStan's initialize treats it -- not as a fatal error.
      // CmdStan's two-stage check, in its order: the log density on
      // doubles first (stan::services::util::initialize), then the
      // gradient. The stages can disagree on an ODE model -- the value
      // path solves the states alone and the gradient path solves the
      // coupled system -- and running only the second accepted initial
      // points CmdStan rejects, which on lotka_volterra meant starting
      // in a region warmup could not climb out of.
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
          cfg.init != nullptr
              ? std::string("initialization failed: the supplied "
                            "unconstrained init has no finite log density "
                            "and gradient")
              : cfg.init_radius == 0.0
                    ? std::string("initialization failed: the origin has no "
                                  "finite log density and gradient (init "
                                  "radius is 0)")
                    : "initialization failed: no draw in " +
                          std::to_string(kMaxInitAttempts) +
                          " attempts had finite log density and gradient");
  }
  if (std::getenv("STANLI_DEBUG_INIT")) {
    std::fprintf(stderr, "init");
    for (int64_t i = 0; i < n; ++i) std::fprintf(stderr, " %.17g", q(i));
    std::fprintf(stderr, "\n");
  }
  sampler.seed(q);

  sampler.init_stepsize(logger);
  sampler.set_stepsize_jitter(0.0);
  sampler.get_stepsize_adaptation().set_mu(
      std::log(10.0 * sampler.get_nominal_stepsize()));
  sampler.get_stepsize_adaptation().set_delta(cfg.delta);
  sampler.set_max_depth(cfg.max_depth);
  sampler.set_window_params(cfg.warmup, 75, 50, 25, logger);
  sampler.engage_adaptation();

  stan::mcmc::sample s(q, 0, 0);

  const int thin = cfg.thin > 0 ? cfg.thin : 1;
  std::vector<std::vector<double>> draws;
  draws.reserve((size_t)((cfg.save_warmup ? cfg.warmup : 0) + cfg.samples) /
                (size_t)thin + 1);
  if (stats) stats->rows.clear();

  Eigen::VectorXd qd(n);
  std::vector<double> sp;
  // One transition, stored or not. Storing is what `keep` decides;
  // observing happens either way, because a progress consumer wants every
  // transition and a thinned run still passes through the ones it drops.
  const auto step = [&](int64_t i, bool warmup, bool keep) {
    s = sampler.transition(s, logger);
    s.cont_params(qd);
    if (keep) {
      draws.emplace_back(qd.data(), qd.data() + n);
      if (stats) {
        // get_sampler_params yields stepsize__, treedepth__, n_leapfrog__,
        // divergent__, energy__ in that order (stan::mcmc::base_nuts).
        sp.clear();
        sampler.get_sampler_params(sp);
        stats->rows.push_back({s.log_prob(), s.accept_stat(),
                               sp.size() > 0 ? sp[0] : 0.0,
                               sp.size() > 1 ? sp[1] : 0.0,
                               sp.size() > 2 ? sp[2] : 0.0,
                               sp.size() > 3 ? sp[3] : 0.0,
                               sp.size() > 4 ? sp[4] : 0.0});
      }
    }
    if (observe) observe(i, warmup, qd.data());
  };

  for (int i = 0; i < cfg.warmup; ++i)
    step(i, true, cfg.save_warmup && (i % thin == 0));
  sampler.disengage_adaptation();
  for (int i = 0; i < cfg.samples; ++i) step(i, false, i % thin == 0);
  return draws;
}

bool thread_safe_build() {
#ifdef STAN_THREADS
  return true;
#else
  return false;
#endif
}

std::vector<std::unique_ptr<Executor>> clone_executors(const Executor& src,
                                                       int n) {
  std::vector<std::unique_ptr<Executor>> out;
  out.reserve((size_t)(n > 0 ? n : 0));
  for (int i = 0; i < n; ++i) out.push_back(std::make_unique<Executor>(src));
  return out;
}

std::vector<ChainResult> run_nuts_chains(const std::vector<Executor*>& execs,
                                         const NutsConfig& cfg, int n_threads,
                                         const DrawObserver& observe) {
  const size_t n_chains = execs.size();
  std::vector<ChainResult> out(n_chains);

  // One chain's work, by index. A chain that throws records its message
  // and leaves its draws empty rather than taking the run down: CmdStan
  // reports a failed chain and keeps the others, and a three-of-four run
  // is something the caller can decide about.
  const auto run_one = [&](size_t c) {
    NutsConfig cc = cfg;
    cc.chain_id = cfg.chain_id + (int)c;
    try {
      out[c].draws = run_nuts(*execs[c], cc, &out[c].stats,
                              n_chains == 1 ? observe : DrawObserver{});
    } catch (const std::exception& e) {
      out[c].error = e.what();
    }
  };

  // Threads are honoured only where stan-math's autodiff stack is
  // thread_local. Everywhere else this clamps to sequential, because the
  // alternative is two chains silently sharing one nested var tape --
  // a wrong answer rather than a slow one.
  int threads = n_threads;
  if (threads > (int)n_chains) threads = (int)n_chains;
  if (!thread_safe_build()) threads = 1;

  if (threads <= 1) {
    for (size_t c = 0; c < n_chains; ++c) run_one(c);
    return out;
  }

  // A shared cursor rather than a static split: chains of the same model
  // still finish at different times (adaptation picks different
  // trajectories), so handing each thread a fixed third leaves cores idle.
  std::atomic<size_t> next{0};
  std::vector<std::thread> pool;
  pool.reserve((size_t)threads);
  for (int t = 0; t < threads; ++t)
    pool.emplace_back([&] {
      // stan-math REQUIRES this. Under STAN_THREADS its autodiff stack
      // pointer is thread_local and starts null in every new thread;
      // constructing a ChainableStack is what allocates that thread's
      // tape, and the destructor frees it when the thread exits.
      // CmdStan never writes this line because TBB's scheduler-entry
      // hook (ad_tape_observer, init_chainablestack.hpp) does it for
      // every worker -- and this build stubs TBB out. Without it the
      // first nested_rev_autodiff on a worker dereferences null inside
      // start_nested(), which is a segfault, not a wrong number.
      stan::math::ChainableStack ad_tape_for_this_thread;
      for (size_t c = next++; c < n_chains; c = next++) run_one(c);
    });
  for (auto& th : pool) th.join();
  return out;
}

}  // namespace stanli
