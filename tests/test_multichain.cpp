// Multi-chain sampling: executor cloning, per-chain streams, thinning,
// saved warmup, explicit inits, and the diagnostics computed over the
// result.
//
// The load-bearing claim here is that a cloned executor is the SAME
// model. Binding zeroes the arena and the data fills live in the arena,
// so a clone that copied only the graph would sample a model with all-zero
// data -- which does not crash, does not fail to converge, and produces a
// perfectly plausible wrong posterior. The gradient equality check below
// is what stands between that and a silent wrong answer.
#include "models.hpp"

#include <stanli/diagnose.hpp>
#include <stanli/nuts.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

static int failures = 0;

static void expect(const std::string& what, bool ok) {
  if (!ok) {
    ++failures;
    std::printf("FAIL %s\n", what.c_str());
  }
}

static void expect_in(const std::string& what, double got, double lo,
                      double hi) {
  if (!(got >= lo && got <= hi)) {
    ++failures;
    std::printf("FAIL %-34s got %.6g want in [%g, %g]\n", what.c_str(), got, lo,
                hi);
  }
}

// A 4-dimensional standard normal: cheap, and every marginal has known
// moments, so the summary numbers can be checked rather than eyeballed.
static stanli::Graph normal_graph(int D) {
  using namespace stanli;
  Graph g;
  const int x = g.add_slot(D, true);
  const int zero = g.add_slot(1, false);
  const int one = g.add_slot(1, false);
  const int lp = g.add_slot(1, false);
  g.add_op(OP_NORMAL_LPDF, {x, zero, one}, lp);
  g.result_slot = lp;
  return g;
}

int main() {
  using namespace stanli;

  // ---- a clone is the same model ----------------------------------------
  // Eight schools has real data in its arena, which is the case a
  // graph-only clone gets wrong.
  {
    auto m = testmodels::eight_schools();
    Executor src(std::move(m.graph));
    testmodels::fill_eight_schools_data(m, src);

    Executor clone(src);
    expect("clone has the same parameter count",
           clone.n_params() == src.n_params());

    const int64_t n = src.n_params();
    std::vector<double> q((size_t)n), g1((size_t)n), g2((size_t)n);
    for (int64_t i = 0; i < n; ++i) q[(size_t)i] = 0.1 + 0.03 * (double)i;
    for (int64_t i = 0; i < n; ++i) {
      src.params_data()[i] = q[(size_t)i];
      clone.params_data()[i] = q[(size_t)i];
    }
    const double lp1 = src.gradient(g1.data());
    const double lp2 = clone.gradient(g2.data());
    // Bitwise: the clone runs the same ops over the same doubles in the
    // same order, so anything short of equality is a real difference.
    bool same = lp1 == lp2;
    for (int64_t i = 0; i < n; ++i)
      same = same && g1[(size_t)i] == g2[(size_t)i];
    expect("clone gradient is bitwise identical", same);

    // Copying after a reverse sweep must rebuild compact adjoint offsets and
    // contexts, not inherit the source arena's interior pointers or dirt.
    Executor post_grad_clone(src);
    std::vector<double> g_after((size_t)n);
    const double lp_after = post_grad_clone.gradient(g_after.data());
    bool same_after = lp_after == lp1;
    for (int64_t i = 0; i < n; ++i)
      same_after = same_after && g_after[(size_t)i] == g1[(size_t)i];
    expect("post-gradient clone is bitwise identical", same_after);

    // And the two arenas are independent: evaluating one must not move
    // the other.
    for (int64_t i = 0; i < n; ++i) clone.params_data()[i] = 5.0;
    clone.gradient(g2.data());
    for (int64_t i = 0; i < n; ++i) src.params_data()[i] = q[(size_t)i];
    const double lp3 = src.gradient(g1.data());
    expect("clones do not share an arena", lp3 == lp1);
  }

  // Exercise the complete bound-executor lifetime as a black box. This
  // model combines matrix metadata, integer-valued observations, density
  // scratch, multiple reverse kernels, and immutable data in the arena. The
  // clone must remain a usable model after both the source executor and the
  // graph builder that supplied its metadata have died. Repetition also
  // checks that forward/reverse scratch and adjoints are safely reusable.
  {
    std::unique_ptr<Executor> survivor;
    std::vector<double> q;
    std::vector<double> want_grad;
    double want_lp = 0.0;
    {
      auto m = testmodels::logistic_glm();
      Executor src(std::move(m.graph));
      testmodels::fill_logistic_glm_data(m, src);
      const int64_t n = src.n_params();
      q.resize((size_t)n);
      want_grad.resize((size_t)n);
      for (int64_t i = 0; i < n; ++i) {
        q[(size_t)i] = -0.2 + 0.07 * (double)i;
        src.params_data()[i] = q[(size_t)i];
      }
      want_lp = src.gradient(want_grad.data());
      survivor = std::make_unique<Executor>(src);
    }

    std::vector<double> got_grad(q.size());
    bool same = true;
    for (int rep = 0; rep < 8; ++rep) {
      for (size_t i = 0; i < q.size(); ++i) survivor->params_data()[i] = q[i];
      const double got_lp = survivor->gradient(got_grad.data());
      same = same && got_lp == want_lp && got_grad == want_grad;
    }
    expect("source-destroyed clone remains bitwise identical", same);
  }

  // ---- chain ids give different streams, seeds reproduce ----------------
  {
    Executor a(normal_graph(4)), b(normal_graph(4)), c(normal_graph(4));
    a.value_ptr(2)[0] = 1.0;  // sigma slot; mu stays 0
    b.value_ptr(2)[0] = 1.0;
    c.value_ptr(2)[0] = 1.0;

    NutsConfig cfg;
    cfg.seed = 4242;
    cfg.warmup = 200;
    cfg.samples = 200;

    cfg.chain_id = 1;
    auto d1 = run_nuts(a, cfg);
    cfg.chain_id = 2;
    auto d2 = run_nuts(b, cfg);
    cfg.chain_id = 1;
    auto d3 = run_nuts(c, cfg);

    expect("same seed and chain id reproduces", d1 == d3);
    expect("a different chain id is a different stream", d1 != d2);
  }

  // ---- run_nuts_chains agrees with running the chains by hand -----------
  // This is what makes the multi-chain path trustworthy: it must be the
  // single-chain path, N times, with the chain id walked.
  {
    Executor base(normal_graph(3));
    base.value_ptr(2)[0] = 1.0;
    auto clones = clone_executors(base, 2);
    std::vector<Executor*> execs{&base, clones[0].get(), clones[1].get()};

    NutsConfig cfg;
    cfg.seed = 99;
    cfg.warmup = 150;
    cfg.samples = 150;
    cfg.chain_id = 1;
    auto res = run_nuts_chains(execs, cfg, 1);
    expect("three chains returned", res.size() == 3);
    for (const auto& r : res) expect("chain ok", r.error.empty());
    expect("each chain stored its draws",
           res[0].draws.size() == 150 && res[2].draws.size() == 150);
    expect("each chain has stats", res[0].stats.rows.size() == 150);

    Executor solo(normal_graph(3));
    solo.value_ptr(2)[0] = 1.0;
    NutsConfig c2 = cfg;
    c2.chain_id = 3;  // what chain index 2 should have used
    auto solo_draws = run_nuts(solo, c2);
    expect("chain 2 matches the same chain run alone",
           res[2].draws == solo_draws);
  }

  // ---- threaded chains equal sequential chains, bitwise -----------------
  // On a build without STANLI_THREADS this is a tautology (n_threads is
  // clamped to 1), and that is the point: the assertion is the same
  // either way, so turning threads on cannot change an answer without
  // this failing.
  //
  // It also guards a specific crash. stan-math's autodiff stack is
  // thread_local under STAN_THREADS and starts NULL in every new thread;
  // stan-math requires each child thread to instantiate a ChainableStack
  // before touching the AD system. CmdStan gets that from a TBB
  // scheduler-entry hook, which this build stubs out, so raw threads
  // segfaulted in start_nested() until run_nuts_chains did it itself.
  {
    const int C = 4;
    Executor a(normal_graph(3));
    a.value_ptr(2)[0] = 1.0;
    auto ca = clone_executors(a, C - 1);
    std::vector<Executor*> ea{&a};
    for (auto& c : ca) ea.push_back(c.get());

    Executor b(normal_graph(3));
    b.value_ptr(2)[0] = 1.0;
    auto cb = clone_executors(b, C - 1);
    std::vector<Executor*> eb{&b};
    for (auto& c : cb) eb.push_back(c.get());

    NutsConfig cfg;
    cfg.seed = 1234;
    cfg.warmup = 200;
    cfg.samples = 200;
    auto seq = run_nuts_chains(ea, cfg, 1);
    auto par = run_nuts_chains(eb, cfg, C);
    bool same = seq.size() == par.size();
    for (size_t c = 0; same && c < seq.size(); ++c) {
      same = same && par[c].error.empty();
      same = same && seq[c].draws == par[c].draws;
    }
    expect("threaded chains are bitwise the sequential chains", same);
  }

  // ---- per-chain inits are threaded like any other chain ----------------
  {
    const int C = 4;
    const int D = 3;
    std::vector<double> inits((size_t)(C * D));
    for (size_t i = 0; i < inits.size(); ++i) inits[i] = 0.2 * (double)i - 1.0;

    Executor a(normal_graph(D));
    a.value_ptr(2)[0] = 1.0;
    auto ca = clone_executors(a, C - 1);
    std::vector<Executor*> ea{&a};
    for (auto& c : ca) ea.push_back(c.get());

    NutsConfig cfg;
    cfg.seed = 77;
    cfg.warmup = 100;
    cfg.samples = 100;
    cfg.chain_id = 1;
    auto par = run_nuts_chains(ea, cfg, C, {}, {}, 1, {}, {}, inits.data());

    bool same = par.size() == (size_t)C;
    for (int c = 0; same && c < C; ++c) {
      Executor solo(normal_graph(D));
      solo.value_ptr(2)[0] = 1.0;
      NutsConfig one = cfg;
      one.chain_id = cfg.chain_id + c;
      one.init = inits.data() + (size_t)(c * D);
      same = par[(size_t)c].error.empty() &&
             par[(size_t)c].draws == run_nuts(solo, one);
    }
    expect("threaded chains with inits equal each chain run alone", same);
  }

  // ---- progress is rate-limited, caller-threaded, and observational ----
  {
    NutsConfig schedule;
    schedule.warmup = 4;
    schedule.samples = 3;
    std::vector<int64_t> selected;
    for (int i = 0; i < schedule.warmup; ++i)
      if (should_report_progress(schedule, i, true, 2))
        selected.push_back(i + 1);
    for (int i = 0; i < schedule.samples; ++i)
      if (should_report_progress(schedule, i, false, 2))
        selected.push_back(schedule.warmup + i + 1);
    expect("refresh selects first, boundary, multiples, and final",
           selected == std::vector<int64_t>({1, 2, 4, 5, 6, 7}));
    NutsConfig nonmultiple;
    nonmultiple.warmup = 3;
    nonmultiple.samples = 4;
    selected.clear();
    for (int i = 0; i < nonmultiple.warmup; ++i)
      if (should_report_progress(nonmultiple, i, true, 2))
        selected.push_back(i + 1);
    for (int i = 0; i < nonmultiple.samples; ++i)
      if (should_report_progress(nonmultiple, i, false, 2))
        selected.push_back(nonmultiple.warmup + i + 1);
    expect("refresh intervals restart at the sampling phase",
           selected == std::vector<int64_t>({1, 2, 3, 4, 5, 7}));
    expect("refresh zero selects no transitions",
           !should_report_progress(schedule, 0, true, 0) &&
               !should_report_progress(schedule, 2, false, 0));

    const int C = 2;
    Executor quiet_base(normal_graph(3));
    quiet_base.value_ptr(2)[0] = 1.0;
    auto quiet_clones = clone_executors(quiet_base, C - 1);
    std::vector<Executor*> quiet_execs{&quiet_base};
    for (auto& c : quiet_clones) quiet_execs.push_back(c.get());

    Executor observed_base(normal_graph(3));
    observed_base.value_ptr(2)[0] = 1.0;
    auto observed_clones = clone_executors(observed_base, C - 1);
    std::vector<Executor*> observed_execs{&observed_base};
    for (auto& c : observed_clones) observed_execs.push_back(c.get());

    NutsConfig cfg;
    cfg.seed = 8181;
    cfg.warmup = 50;
    cfg.samples = 30;
    const auto quiet = run_nuts_chains(quiet_execs, cfg, C);

    const std::thread::id caller = std::this_thread::get_id();
    bool caller_thread = true;
    std::vector<std::vector<int64_t>> events(C);
    const auto observed = run_nuts_chains(
        observed_execs, cfg, C, {},
        [&](int chain, int64_t i, bool warmup) {
          caller_thread = caller_thread && std::this_thread::get_id() == caller;
          events[(size_t)chain].push_back(warmup ? i + 1 : cfg.warmup + i + 1);
        },
        7);
    bool same = quiet.size() == observed.size();
    for (int c = 0; same && c < C; ++c) {
      same = quiet[(size_t)c].draws == observed[(size_t)c].draws &&
             quiet[(size_t)c].stats.rows == observed[(size_t)c].stats.rows;
      expect(
          "progress events are monotone within a chain",
          std::is_sorted(events[(size_t)c].begin(), events[(size_t)c].end()));
      expect("progress includes each chain's final transition",
             !events[(size_t)c].empty() &&
                 events[(size_t)c].back() == cfg.warmup + cfg.samples);
      expect("sampling timings are nonnegative",
             observed[(size_t)c].report.warmup_seconds >= 0.0 &&
                 observed[(size_t)c].report.sampling_seconds >= 0.0);
    }
    expect("progress callbacks run on the caller thread", caller_thread);
    expect("progress does not change draws or sampler stats", same);

    Executor throwing_base(normal_graph(3));
    throwing_base.value_ptr(2)[0] = 1.0;
    auto throwing_clones = clone_executors(throwing_base, C - 1);
    std::vector<Executor*> throwing_execs{&throwing_base};
    for (auto& c : throwing_clones) throwing_execs.push_back(c.get());
    bool callback_error_rethrown = false;
    try {
      (void)run_nuts_chains(
          throwing_execs, cfg, C, {},
          [](int, int64_t, bool) {
            throw std::runtime_error("progress callback failed");
          },
          7);
    } catch (const std::runtime_error& e) {
      callback_error_rethrown =
          std::string(e.what()) == "progress callback failed";
    }
    expect("throwing progress callback finishes safely and rethrows",
           callback_error_rethrown);

    NutsConfig empty_cfg = cfg;
    empty_cfg.warmup = 0;
    empty_cfg.samples = 0;
    Executor empty_base(normal_graph(3));
    empty_base.value_ptr(2)[0] = 1.0;
    auto empty_clones = clone_executors(empty_base, C - 1);
    std::vector<Executor*> empty_execs{&empty_base};
    for (auto& c : empty_clones) empty_execs.push_back(c.get());
    int empty_events = 0;
    const auto empty = run_nuts_chains(
        empty_execs, empty_cfg, C, {},
        [&](int, int64_t, bool) { ++empty_events; }, 1);
    expect("zero-transition chains complete without callbacks",
           empty.size() == C && empty_events == 0);
  }

  // ---- reports cover transitions that thinning does not store -----------
  {
    Executor full_ex(normal_graph(2));
    full_ex.value_ptr(2)[0] = 1.0;
    Executor thin_ex(normal_graph(2));
    thin_ex.value_ptr(2)[0] = 1.0;
    NutsConfig full_cfg;
    full_cfg.seed = 9191;
    full_cfg.warmup = 30;
    full_cfg.samples = 40;
    full_cfg.max_depth = 1;
    SamplerStats full_stats;
    SamplingReport full_report;
    (void)run_nuts(full_ex, full_cfg, &full_stats, {}, {}, &full_report);

    int64_t full_depth_hits = 0;
    for (const auto& row : full_stats.rows)
      if ((int)row[3] >= full_cfg.max_depth) ++full_depth_hits;
    expect("report agrees with unthinned sampler stats",
           full_report.n_max_treedepth == full_depth_hits);
    expect("depth-one run exercises saturation reporting", full_depth_hits > 0);

    NutsConfig thin_cfg = full_cfg;
    thin_cfg.thin = 7;
    thin_cfg.save_warmup = true;
    SamplingReport thin_report;
    SamplerStats thin_stats;
    (void)run_nuts(thin_ex, thin_cfg, &thin_stats, {}, {}, &thin_report);
    expect("thinned report keeps every post-warmup diagnostic",
           thin_report.n_divergent == full_report.n_divergent &&
               thin_report.n_max_treedepth == full_report.n_max_treedepth);
    expect(
        "thinned stats really omit transitions",
        thin_stats.rows.size() < (size_t)(thin_cfg.warmup + thin_cfg.samples));
  }

  // ---- thinning and saved warmup change the row count -------------------
  {
    Executor ex(normal_graph(2));
    ex.value_ptr(2)[0] = 1.0;
    NutsConfig cfg;
    cfg.seed = 7;
    cfg.warmup = 100;
    cfg.samples = 100;

    cfg.thin = 4;
    SamplerStats st;
    auto d = run_nuts(ex, cfg, &st);
    expect("thin keeps every 4th draw", d.size() == 25);
    expect("stats are thinned too", st.rows.size() == d.size());

    Executor ex2(normal_graph(2));
    ex2.value_ptr(2)[0] = 1.0;
    cfg.thin = 1;
    cfg.save_warmup = true;
    auto d2 = run_nuts(ex2, cfg);
    expect("saved warmup prepends its rows", d2.size() == 200);
  }

  // ---- explicit inits ---------------------------------------------------
  {
    Executor ex(normal_graph(3));
    ex.value_ptr(2)[0] = 1.0;
    const double init[3] = {0.5, -0.25, 1.5};
    NutsConfig cfg;
    cfg.seed = 5;
    cfg.warmup = 0;  // no adaptation, so the first draw stays near the init
    cfg.samples = 1;
    cfg.init = init;
    auto d = run_nuts(ex, cfg);
    expect("init honoured", d.size() == 1);

    // An init outside the support must fail with a message that says so
    // rather than silently falling back to a random draw.
    Executor bad(normal_graph(1));
    bad.value_ptr(2)[0] = 1.0;
    const double nan_init[1] = {std::nan("")};
    NutsConfig bcfg;
    bcfg.seed = 5;
    bcfg.warmup = 1;
    bcfg.samples = 1;
    bcfg.init = nan_init;
    bool threw = false;
    std::string msg;
    try {
      run_nuts(bad, bcfg);
    } catch (const std::exception& e) {
      threw = true;
      msg = e.what();
    }
    expect("a bad explicit init throws", threw);
    expect("and says the init was the problem",
           msg.find("supplied") != std::string::npos);
  }

  // ---- end to end: four chains of a standard normal look converged ------
  {
    const int D = 2, C = 4, N = 500;
    Executor base(normal_graph(D));
    base.value_ptr(2)[0] = 1.0;
    auto clones = clone_executors(base, C - 1);
    std::vector<Executor*> execs{&base};
    for (auto& c : clones) execs.push_back(c.get());

    NutsConfig cfg;
    cfg.seed = 20260808;
    cfg.warmup = 500;
    cfg.samples = N;
    auto res = run_nuts_chains(execs, cfg, 1);

    // Pack chain-major, exactly as the C ABI does, and summarize.
    std::vector<double> draws((size_t)(C * N * D));
    std::vector<double> stats((size_t)(C * N * N_SAMPLER_COLS));
    for (int c = 0; c < C; ++c)
      for (int i = 0; i < N; ++i) {
        for (int j = 0; j < D; ++j)
          draws[(size_t)((c * N + i) * D + j)] =
              res[(size_t)c].draws[(size_t)i][(size_t)j];
        for (int k = 0; k < N_SAMPLER_COLS; ++k)
          stats[(size_t)((c * N + i) * N_SAMPLER_COLS + k)] =
              res[(size_t)c].stats.rows[(size_t)i][(size_t)k];
      }
    DrawSet ds{draws.data(), C, N, D};
    auto s = summarize(ds, {"x.1", "x.2"});
    expect_in("mean ~ 0", s[0].mean, -0.15, 0.15);
    expect_in("sd ~ 1", s[0].sd, 0.85, 1.15);
    expect_in("rhat ~ 1", s[0].rhat, 0.99, 1.05);
    expect_in("ess is a decent fraction of 2000", s[0].ess_bulk, 400, 4000);

    auto fd = diagnose(ds, s, stats.data(), cfg.max_depth);
    expect("no divergences on a standard normal", fd.n_divergent == 0);
    expect("stepsize reported per chain",
           fd.stepsize_by_chain.size() == (size_t)C &&
               fd.stepsize_by_chain[0] > 0);
    expect_in("ebfmi healthy", fd.ebfmi_by_chain[0], 0.3, 3.0);
    expect("clean run reports no problems",
           format_diagnostics(fd).find("No problems detected") !=
               std::string::npos);
  }

  if (failures == 0) std::printf("test_multichain: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
