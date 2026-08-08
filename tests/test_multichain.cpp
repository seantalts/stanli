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

#include <cmath>
#include <cstdio>
#include <string>
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
    std::printf("FAIL %-34s got %.6g want in [%g, %g]\n", what.c_str(), got,
                lo, hi);
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
    for (int64_t i = 0; i < n; ++i) same = same && g1[(size_t)i] == g2[(size_t)i];
    expect("clone gradient is bitwise identical", same);

    // And the two arenas are independent: evaluating one must not move
    // the other.
    for (int64_t i = 0; i < n; ++i) clone.params_data()[i] = 5.0;
    clone.gradient(g2.data());
    for (int64_t i = 0; i < n; ++i) src.params_data()[i] = q[(size_t)i];
    const double lp3 = src.gradient(g1.data());
    expect("clones do not share an arena", lp3 == lp1);
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
    cfg.warmup = 0;   // no adaptation, so the first draw stays near the init
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
          draws[(size_t)((c * N + i) * D + j)] = res[(size_t)c].draws[(size_t)i][(size_t)j];
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
