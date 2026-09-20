#include <stanli/nuts.hpp>

#include <stanli/model_adapter.hpp>

#include "initialize.hpp"

#include <stan/callbacks/logger.hpp>
#include <stan/math/rev/core/chainablestack.hpp>
#include <stan/mcmc/hmc/nuts/adapt_diag_e_nuts.hpp>
#include <stan/services/util/create_rng.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>

namespace stanli {

std::vector<std::vector<double>> run_nuts(Executor& ex, const NutsConfig& cfg,
                                          SamplerStats* stats,
                                          const DrawObserver& observe,
                                          const ProgressObserver& progress,
                                          SamplingReport* report) {
  using Clock = std::chrono::steady_clock;
  if (report) *report = SamplingReport{};
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

  // CmdStan draws uniform(-2, 2) on the unconstrained scale and REJECTS the
  // draw unless both the log density and its whole gradient are finite,
  // retrying up to 100 times (stan::services::util::initialize). We took the
  // first draw unconditionally, so a model whose typical set only covers
  // part of that hypercube -- accel_gp's GP hyperparameters overflow exp()
  // over much of it -- failed outright on seeds whose first draw landed
  // badly, with the failure surfacing later as a stepsize-search error.
  initialize_point(ex, rng, cfg.init_radius, cfg.init, q.data(),
                   FixedInitPolicy::Validate);
  if (std::getenv("STANLI_DEBUG_INIT")) {
    std::fprintf(stderr, "init");
    for (int64_t i = 0; i < n; ++i) std::fprintf(stderr, " %.17g", q(i));
    std::fprintf(stderr, "\n");
  }
  sampler.seed(q);

  // Match CmdStan's service defaults, not base_hmc's 0.1 step size.
  // Initial dual averaging is centered on the requested step size BEFORE
  // init_stepsize searches; recentering on the result changes adaptation.
  constexpr double initial_stepsize = 1.0;
  sampler.set_nominal_stepsize(initial_stepsize);
  sampler.set_stepsize_jitter(0.0);
  sampler.get_stepsize_adaptation().set_mu(
      std::log(10.0 * initial_stepsize));
  sampler.get_stepsize_adaptation().set_delta(cfg.delta);
  sampler.set_max_depth(cfg.max_depth);
  sampler.set_window_params(cfg.warmup, 75, 50, 25, logger);
  sampler.engage_adaptation();
  sampler.init_stepsize(logger);

  stan::mcmc::sample s(q, 0, 0);

  const int thin = cfg.thin > 0 ? cfg.thin : 1;
  std::vector<std::vector<double>> draws;
  draws.reserve((size_t)((cfg.save_warmup ? cfg.warmup : 0) + cfg.samples) /
                    (size_t)thin +
                1);
  if (stats) stats->rows.clear();

  Eigen::VectorXd qd(n);
  std::vector<double> sp;
  // One transition, stored or not. Storing is what `keep` decides;
  // observing happens either way, because a progress consumer wants every
  // transition and a thinned run still passes through the ones it drops.
  const auto step = [&](int64_t i, bool warmup, bool keep) {
    s = sampler.transition(s, logger);
    s.cont_params(qd);
    // A report covers every post-warmup transition even when thinning drops
    // the row. Fetch the sampler parameters once when either consumer needs
    // them; this is observational and does not touch the sampler RNG.
    const bool inspect = (keep && stats) || (!warmup && report);
    if (inspect) {
      sp.clear();
      sampler.get_sampler_params(sp);
    }
    if (keep) {
      draws.emplace_back(qd.data(), qd.data() + n);
      if (stats) {
        // get_sampler_params yields stepsize__, treedepth__, n_leapfrog__,
        // divergent__, energy__ in that order (stan::mcmc::base_nuts).
        stats->rows.push_back(
            {s.log_prob(), s.accept_stat(), sp.size() > 0 ? sp[0] : 0.0,
             sp.size() > 1 ? sp[1] : 0.0, sp.size() > 2 ? sp[2] : 0.0,
             sp.size() > 3 ? sp[3] : 0.0, sp.size() > 4 ? sp[4] : 0.0});
      }
      if (cfg.on_stored) cfg.on_stored((int64_t)draws.size() - 1, qd.data());
    }
    if (!warmup && report) {
      if (sp.size() > 3 && sp[3] != 0.0) ++report->n_divergent;
      if (sp.size() > 1 && (int)sp[1] >= cfg.max_depth)
        ++report->n_max_treedepth;
    }
    if (observe) observe(i, warmup, qd.data());
    if (progress) progress(i, warmup);
  };

  // Match Stan's timing boundary: initialization and stepsize search are
  // setup, not warmup transitions. In particular, warmup=0 should not report
  // model initialization as warmup time.
  constexpr auto poll_period = std::chrono::milliseconds(100);
  auto last_poll = Clock::now() - poll_period;
  const auto stopping = [&]() {
    if (cfg.stop && cfg.stop->load()) return true;
    if (!cfg.poll) return false;
    const auto now = Clock::now();
    if (now - last_poll < poll_period) return false;
    last_poll = now;
    if (!cfg.poll()) return false;
    if (cfg.stop) cfg.stop->store(true);
    return true;
  };
  bool interrupted = false;

  const auto warmup_started = Clock::now();
  for (int i = 0; i < cfg.warmup && !interrupted; ++i) {
    if (stopping())
      interrupted = true;
    else
      step(i, true, cfg.save_warmup && (i % thin == 0));
  }
  const auto warmup_finished = Clock::now();
  if (report)
    report->warmup_seconds =
        std::chrono::duration<double>(warmup_finished - warmup_started).count();
  sampler.disengage_adaptation();
  const auto sampling_started = Clock::now();
  for (int i = 0; i < cfg.samples && !interrupted; ++i) {
    if (stopping())
      interrupted = true;
    else
      step(i, false, i % thin == 0);
  }
  if (report) {
    report->sampling_seconds =
        std::chrono::duration<double>(Clock::now() - sampling_started).count();
    report->interrupted = interrupted;
  }
  return draws;
}

std::vector<double> cmdstan_init_point(Executor& ex, uint32_t seed,
                                       int chain_id, double init_radius,
                                       const double* init) {
  stan::rng_t rng = stan::services::util::create_rng(seed, chain_id);
  std::vector<double> q((size_t)ex.n_params());
  initialize_point(ex, rng, init_radius, init, q.data(),
                   FixedInitPolicy::Validate);
  return q;
}

bool thread_safe_build() {
#ifdef STAN_THREADS
  return true;
#else
  return false;
#endif
}

bool should_report_progress(const NutsConfig& cfg, int64_t i, bool warmup,
                            int refresh) {
  if (refresh <= 0) return false;
  const int64_t completed = i + 1;
  const int64_t phase_total = warmup ? cfg.warmup : cfg.samples;
  return completed == 1 || completed == phase_total || completed % refresh == 0;
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
                                         const DrawObserver& observe,
                                         const ChainProgressObserver& progress,
                                         int progress_refresh,
                                         const std::function<bool()>& poll,
                                         const StoredDrawWriter& write) {
  const size_t n_chains = execs.size();
  std::vector<ChainResult> out(n_chains);
  std::atomic<bool> local_stop{false};
  std::atomic<bool>* const stop = cfg.stop ? cfg.stop : &local_stop;

  // One chain's work, by index. A chain that throws records its message
  // and leaves its draws empty rather than taking the run down: CmdStan
  // reports a failed chain and keeps the others, and a three-of-four run
  // is something the caller can decide about.
  const auto run_one = [&](size_t c, const ProgressObserver& one_progress,
                           const std::function<bool()>& chain_poll) {
    NutsConfig cc = cfg;
    cc.chain_id = cfg.chain_id + (int)c;
    cc.stop = stop;
    cc.poll = chain_poll;
    if (write)
      cc.on_stored = [&write, c](int64_t row, const double* q) {
        write((int)c, row, q);
      };
    try {
      out[c].draws = run_nuts(*execs[c], cc, &out[c].stats,
                              n_chains == 1 ? observe : DrawObserver{},
                              one_progress, &out[c].report);
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
    std::exception_ptr progress_error;
    for (size_t c = 0; c < n_chains; ++c) {
      ProgressObserver one_progress;
      if (progress)
        one_progress = [&, c](int64_t i, bool warmup) {
          if (!should_report_progress(cfg, i, warmup, progress_refresh) ||
              progress_error)
            return;
          try {
            progress((int)c, i, warmup);
          } catch (...) {
            progress_error = std::current_exception();
          }
        };
      run_one(c, one_progress, poll);
    }
    if (progress_error) std::rethrow_exception(progress_error);
    return out;
  }

  // A shared cursor rather than a static split: chains of the same model
  // still finish at different times (adaptation picks different
  // trajectories), so handing each thread a fixed third leaves cores idle.
  std::atomic<size_t> next{0};
  std::vector<std::thread> pool;
  pool.reserve((size_t)threads);

  // The caller's poll runs on this thread, between progress events, so a
  // busy progress stream and a silent one both see it about every period.
  // It runs once before the pool starts as well: a stop that is already
  // pending is then seen by every chain ahead of its first transition.
  using Clock = std::chrono::steady_clock;
  constexpr auto poll_period = std::chrono::milliseconds(100);
  auto last_poll = Clock::now() - poll_period;
  const auto maybe_poll = [&] {
    if (!poll || stop->load()) return;
    const auto now = Clock::now();
    if (now - last_poll < poll_period) return;
    last_poll = now;
    if (poll()) stop->store(true);
  };
  maybe_poll();

  // R's console API is main-thread-only, and a Python callback from a new
  // native thread has avoidable interpreter overhead. Workers therefore
  // enqueue only the three small fields; this calling thread drains them and
  // invokes the public observer while the pool runs.
  struct ProgressEvent {
    int chain;
    int64_t i;
    bool warmup;
  };
  std::mutex progress_mutex;
  std::condition_variable progress_ready;
  std::deque<ProgressEvent> progress_events;
  int live_workers = threads;
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
      for (size_t c = next++; c < n_chains; c = next++) {
        ProgressObserver one_progress;
        if (progress)
          one_progress = [&, c](int64_t i, bool warmup) {
            if (!should_report_progress(cfg, i, warmup, progress_refresh))
              return;
            {
              std::lock_guard<std::mutex> lock(progress_mutex);
              progress_events.push_back({(int)c, i, warmup});
            }
            progress_ready.notify_one();
          };
        run_one(c, one_progress, {});
      }
      {
        // Completion is part of the condition-variable predicate, so mutate
        // it under the same mutex. Otherwise the final notify can race
        // between the caller's predicate check and its wait and be lost.
        std::lock_guard<std::mutex> lock(progress_mutex);
        --live_workers;
      }
      progress_ready.notify_one();
    });

  std::exception_ptr progress_error;
  if (progress || poll) {
    for (;;) {
      maybe_poll();
      ProgressEvent event{};
      {
        std::unique_lock<std::mutex> lock(progress_mutex);
        progress_ready.wait_for(lock, poll_period, [&] {
          return !progress_events.empty() || live_workers == 0;
        });
        if (progress_events.empty()) {
          if (live_workers == 0) break;
          continue;
        }
        event = progress_events.front();
        progress_events.pop_front();
      }
      // A public C++ observer is allowed to throw. Keep draining so workers
      // cannot block behind an abandoned queue, join every thread, and only
      // then let the exception escape; destroying a joinable std::thread
      // during unwinding would terminate the process.
      if (!progress_error) {
        try {
          progress(event.chain, event.i, event.warmup);
        } catch (...) {
          progress_error = std::current_exception();
        }
      }
    }
  }
  for (auto& th : pool) th.join();
  if (progress_error) std::rethrow_exception(progress_error);
  return out;
}

}  // namespace stanli
