// Tape islands: a compiled region must match the op-by-op graph it
// replaces (values and gradients), and everything the carver cannot prove
// safe must stay untouched.
#include "env_helpers.hpp"
#include "graph_helpers.hpp"
#include <stanli/compile.hpp>
#include <stanli/graph.hpp>
#include <stanli/island.hpp>
#include <stanli/optable.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

static int failures = 0;
static void expect(const char* what, bool ok) {
  if (!ok) {
    ++failures;
    std::printf("FAIL %s\n", what);
  }
}
static void expect_close(const std::string& what, double got, double want) {
  const double rel = std::abs(got - want) / std::max(std::abs(want), 1e-300);
  if (!(rel < 1e-12)) {
    ++failures;
    std::printf("FAIL %-24s got %.17g want %.17g rel %.2e\n", what.c_str(), got,
                want, rel);
  }
}

using namespace stanli;
using stanli::testutil::Fills;

static double fill_at(int64_t i) { return 0.3 + 0.15 * (i % 4); }
static std::vector<double> run_grad(Graph g, const Fills& fills) {
  return testutil::run_grad(std::move(g), fills, fill_at);
}

// The island kernel reuses its thread-local compact adjoint file. Running the
// same executor twice catches a stale cell beyond the shortened zeroed range.
static std::vector<double> run_grad_twice(Graph g, const Fills& fills) {
  Executor ex(std::move(g));
  for (const auto& f : fills) {
    double* p = ex.value_ptr(f.first);
    for (size_t j = 0; j < f.second.size(); ++j) p[j] = f.second[j];
  }
  for (int64_t i = 0; i < ex.n_params(); ++i) ex.params_data()[i] = fill_at(i);
  std::vector<double> first(1 + (size_t)ex.n_params());
  std::vector<double> second(1 + (size_t)ex.n_params());
  first[0] = ex.gradient(first.data() + 1);
  second[0] = ex.gradient(second.data() + 1);
  expect("repeat sizes", first.size() == second.size());
  for (size_t i = 0; i < first.size(); ++i)
    expect_close("repeat v" + std::to_string(i), second[i], first[i]);
  return second;
}

static std::string slurp(const std::string& path) {
  std::ifstream f(path);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// A necessity island whose live-out is written only inside the branch:
// `sum(a) > 0 ? ident(b + c) : c` inlines to an assignment of the --O1
// inliner's zero-length return symbol under `if (sum(a) > 0)`, and the
// program compiler sizes that symbol where it is assigned. At the zero
// point the condition is false, so those registers are the arm that did
// not run -- and the live-out harvest reads them regardless. Before the
// prologue fill (mir_prog.hpp) the backward's var replay read a register
// file no one had written and dereferenced a null vari: SIGSEGV, not a
// wrong number. Runs first so the replay's thread_local register file is
// still empty, which is the state that makes that a null rather than a
// vari from an already-recovered nested tape.
static void test_branch_bound_live_out() {
  CompiledModel cm =
      compile_model(slurp("tests/fixtures/branchudf.tmir.sexp"), DataMap());
  Executor ex(std::move(cm.graph));
  cm.bind(ex);
  const int64_t n = ex.n_params();
  expect("branchudf params", n == 150);
  for (int64_t i = 0; i < n; ++i) ex.params_data()[i] = 0.0;
  std::vector<double> grad((size_t)n, 0.0);
  const double lp = ex.gradient(grad.data());
  // The untaken arm contributes nothing: mix is c, so the target is
  // sum(c) = 0 and only c's 50 entries carry an adjoint.
  expect("branchudf lp", lp == 0.0);
  int64_t wrong = 0;
  for (int64_t i = 0; i < n; ++i)
    if (grad[(size_t)i] != (i < 100 ? 0.0 : 1.0)) ++wrong;
  expect("branchudf grad", wrong == 0);
}

// A mini HMM forward pass: per step, index the previous state pair, take
// their log-sum-exp, add per-state emission lps (scalar NORMAL, propto
// off), SET_INDEX the new pair into a zero-backed template vector. The
// final step's pair feeds one LSE2 whose out is the target term, which
// ends the region: the pair slots become the island's live-outs.
struct HmmGraph {
  Graph g;
  Fills fills;
  std::vector<int> terms;
  size_t body_ops = 0;
};

// W widens the state vector without changing what is computed: only two
// of its elements are ever read. What it does change is what the ops
// COPY -- each SET_INDEX rewrites the whole vector -- which is the shape
// the cost estimate is looking for (test_vector_copies_carved).
static HmmGraph build_hmm(int T, int W = 2) {
  HmmGraph h;
  Graph& g = h.g;
  const int gp0 = g.add_slot(W, true);  // initial log-state
  const int mu = g.add_slot(2, true);
  const int sigma = g.add_slot(1, true);
  const int z2 = g.add_slot(W, false);  // fill-backed template (absorbed)
  h.fills.emplace_back(z2, std::vector<double>((size_t)W, 0.0));
  auto cslot = [&](double v) {
    const int s = g.add_slot(1, false);
    h.fills.emplace_back(s, std::vector<double>{v});
    return s;
  };
  int gp = gp0;
  const size_t start = g.ops.size();
  for (int t = 0; t < T; ++t) {
    const int y = cslot(0.35 * t - 0.8);
    const int g0 = g.add_slot(1, false);
    g.add_op(OP_INDEX, {gp}, g0, {0});
    const int g1 = g.add_slot(1, false);
    g.add_op(OP_INDEX, {gp}, g1, {1});
    const int s0 = g.add_slot(1, false);
    g.add_op(OP_LSE2, {g0, g1}, s0);
    int next = -1;
    for (int k = 0; k < 2; ++k) {
      const int mk = g.add_slot(1, false);
      g.add_op(OP_INDEX, {mu}, mk, {k});
      const int em = g.add_slot(1, false);
      const int id = g.add_op(OP_NORMAL_LPDF, {y, mk, sigma}, em);
      g.ops[id].variant = 0x06;  // y data, mu/sigma active; propto OFF
      const int nk = g.add_slot(1, false);
      g.add_op(OP_ADD, {s0, em}, nk);
      const int dst = g.add_slot(W, false);
      // The first step fills the zero template (absorbed as a constant);
      // later steps overwrite the previous step's state, which is what a
      // forward algorithm does and what lets the registers alias.
      g.add_op(OP_SET_INDEX, {k == 0 ? (t == 0 ? z2 : gp) : next, nk}, dst,
               {k});
      next = dst;
    }
    gp = next;
  }
  h.body_ops = g.ops.size() - start;
  const int f0 = g.add_slot(1, false);
  g.add_op(OP_INDEX, {gp}, f0, {0});
  const int f1 = g.add_slot(1, false);
  g.add_op(OP_INDEX, {gp}, f1, {1});
  const int lp = g.add_slot(1, false);
  g.add_op(OP_LSE2, {f0, f1}, lp);
  h.terms.push_back(lp);
  g.result_slot = lp;
  return h;
}

static void test_hmm_parity() {
  HmmGraph ref = build_hmm(8);  // 8*11 = 88 body ops, above threshold
  const std::vector<double> want = run_grad(std::move(ref.g), ref.fills);

  HmmGraph isl = build_hmm(8);
  const size_t before = isl.g.ops.size();
  const int carved = carve_islands(isl.g, isl.fills, isl.terms, {});
  expect("hmm carved==1", carved == 1);
  // The run swallows the two trailing INDEX ops too (in vocab, non-term);
  // their outs feed the term LSE2 outside, so they are the live-outs:
  // island + 2 extractions + the final LSE2.
  expect("hmm ops==4", isl.g.ops.size() == 4);
  expect("hmm shrank", isl.g.ops.size() < before);
  expect("hmm island first", isl.g.ops[0].opcode == OP_ISLAND);
  expect("hmm 3 live-ins", isl.g.ops[0].n_in == 3);
  const std::vector<double> got = run_grad(std::move(isl.g), isl.fills);
  expect("hmm sizes", got.size() == want.size());
  for (size_t i = 0; i < want.size() && i < got.size(); ++i)
    expect_close("hmm v" + std::to_string(i), got[i], want[i]);
}

// STANLI_NO_ISLAND switches the carver off entirely. harnesses/ab_corpus.py
// builds its whole A side out of this and the three sibling switches, so a
// rename here would leave that oracle comparing the optimized graph against
// itself, green, with no ctest to catch it.
static void test_env_disable() {
  HmmGraph h = build_hmm(8);
  const size_t before = h.g.ops.size();

  test_setenv("STANLI_NO_ISLAND", "1", 1);
  expect("disabled: nothing carved",
         carve_islands(h.g, h.fills, h.terms, {}) == 0);
  expect("disabled: graph untouched", h.g.ops.size() == before);
  test_unsetenv("STANLI_NO_ISLAND");

  // The same graph with the switch off: the run carves.
  expect("enabled: one island", carve_islands(h.g, h.fills, h.terms, {}) == 1);
}

static void test_short_run_untouched() {
  HmmGraph h = build_hmm(2);  // 22 body ops, under threshold
  const size_t before = h.g.ops.size();
  const int carved = carve_islands(h.g, h.fills, h.terms, {});
  expect("short none carved", carved == 0);
  expect("short ops unchanged", h.g.ops.size() == before);
}

static void test_propto_density_refused() {
  HmmGraph h = build_hmm(8);
  for (auto& op : h.g.ops)
    if (op.opcode == OP_NORMAL_LPDF) op.variant = 0x86;  // propto ON
  const size_t before = h.g.ops.size();
  const int carved = carve_islands(h.g, h.fills, h.terms, {});
  expect("propto none carved", carved == 0);
  expect("propto ops unchanged", h.g.ops.size() == before);
}

static void test_unsupported_op_splits() {
  // A vector-out op mid-region splits the run into halves below the
  // threshold. It used to be POW, but POW -- and every scalar-out op with
  // a kernel -- now compiles as a CALL; what still refuses is an output
  // wider than one register (phase 2 of the kernel-call plan).
  HmmGraph h = build_hmm(5);  // 55 body ops
  Graph& g = h.g;
  const size_t mid = g.ops.size() / 2;
  Op pw;
  pw.opcode = OP_REP_VEC;
  pw.n_in = 1;
  pw.in[0] = g.ops[mid].in[0];
  pw.out = g.add_slot(3, false);
  g.ops.insert(g.ops.begin() + (long)mid, pw);
  h.terms.back() = h.g.result_slot;  // unchanged, re-anchor after insert
  const size_t before = g.ops.size();
  const int carved = carve_islands(g, h.fills, h.terms, {});
  expect("split none carved", carved == 0);
  expect("split ops unchanged", g.ops.size() == before);
}

// A region that carries far more state than it computes: each step drops
// one scalar into its own wide template, so the register file grows by a
// whole vector per three instructions, and the file is written by the
// forward and read back by the backward every call. This is the one shape
// the estimate still has to refuse once the backward stops building vars:
// `bones_model` is it (36 ops behind 4,024 registers) and islands cost it
// 19x replayed, 4x with a generated adjoint.
static void test_wide_state_refused() {
  Graph g;
  Fills fills;
  const int W = 64;
  int prev = g.add_slot(1, true);
  for (int t = 0; t < 12; ++t) {
    const int sq = g.add_slot(1, false);
    g.add_op(OP_MUL, {prev, prev}, sq);
    const int tmpl = g.add_slot(W, false);
    fills.emplace_back(tmpl, std::vector<double>((size_t)W, 0.0));
    const int wide = g.add_slot(W, false);
    g.add_op(OP_SET_INDEX, {tmpl, sq}, wide, {0});
    const int back = g.add_slot(1, false);
    g.add_op(OP_INDEX, {wide}, back, {0});
    prev = back;
  }
  const int lp = g.add_slot(1, false);
  g.add_op(OP_ADD, {prev, prev}, lp);
  g.result_slot = lp;
  std::vector<int> terms{lp};
  const size_t before = g.ops.size();  // 36 in vocab, above kMinIslandOps
  const int carved = carve_islands(g, fills, terms, {});
  expect("wide none carved", carved == 0);
  expect("wide ops unchanged", g.ops.size() == before);
}

// The cost estimate, on the two shapes it has to tell apart. A wide
// state vector copied per step is `iohmm_reg`: the ops move far more
// than the register file does, and the island is the cheaper form.
static void test_vector_copies_carved() {
  HmmGraph ref = build_hmm(8, 128);
  const std::vector<double> want = run_grad(std::move(ref.g), ref.fills);

  HmmGraph isl = build_hmm(8, 128);
  const int carved = carve_islands(isl.g, isl.fills, isl.terms, {});
  expect("copies carved==1", carved == 1);
  expect("copies ops==4", isl.g.ops.size() == 4);
  const std::vector<double> got = run_grad_twice(std::move(isl.g), isl.fills);
  expect("copies sizes", got.size() == want.size());
  for (size_t i = 0; i < want.size() && i < got.size(); ++i)
    expect_close("copies v" + std::to_string(i), got[i], want[i]);
}

// A measured-cost boundary made from the same two facts as iohmm_reg:
// INDEX copies share their source's adjoint cell, while ADD_N expands to a
// short instruction chain. Charging one adjoint cell per forward register
// refuses this region; charging the compact file carves it. Constants keep
// the boundary honest by adding real value/adjoint work without a graph op.
static HmmGraph build_compact_cost_boundary() {
  HmmGraph h;
  Graph& g = h.g;
  constexpr int W = 4;
  const int x = g.add_slot(W, true);
  int last = -1;
  for (int t = 0; t < 8; ++t) {
    int e[W];
    for (int k = 0; k < W; ++k) {
      e[k] = g.add_slot(1, false);
      g.add_op(OP_INDEX, {x}, e[k], {k});
    }
    const int c = g.add_slot(1, false);
    h.fills.emplace_back(c, std::vector<double>{0.1 * (t + 1)});
    last = g.add_slot(1, false);
    g.add_op(OP_ADD_N, {e[0], e[1], e[2], e[3], c}, last);
  }
  h.body_ops = g.ops.size();  // 40 scalar ops, all one maximal run
  const int lp = g.add_slot(1, false);
  g.add_op(OP_ADD, {last, last}, lp);  // target term ends the run
  h.terms.push_back(lp);
  g.result_slot = lp;
  return h;
}

static void test_compact_adjoint_cost_boundary() {
  HmmGraph forced = build_compact_cost_boundary();
  const int64_t graph_cost = 6 * (int64_t)forced.body_ops;
  test_setenv("STANLI_ISLAND_ALWAYS", "1", 1);
  expect("compact boundary forced carve",
         carve_islands(forced.g, forced.fills, forced.terms, {}) == 1);
  test_unsetenv("STANLI_ISLAND_ALWAYS");
  const IslandProg* p = nullptr;
  for (const Op& op : forced.g.ops)
    if (op.opcode == OP_ISLAND) p = static_cast<const IslandProg*>(op.udata);
  expect("compact boundary has island", p != nullptr);
  if (p) {
    const int64_t streams =
        (int64_t)p->code.size() + (int64_t)p->adj.code.size();
    const int64_t old_sparse_cost = 3 * (int64_t)p->n_regs + streams;
    const int64_t compact_cost =
        2 * (int64_t)p->n_regs + p->adj.n_regs + streams;
    expect("compact boundary new wins", compact_cost < graph_cost);
    expect("compact boundary old loses", graph_cost < old_sparse_cost);
    expect("compact boundary actually smaller",
           p->adj.n_regs < (int)p->adj.adj_reg.size());
  }

  HmmGraph normal = build_compact_cost_boundary();
  expect("compact boundary default carve",
         carve_islands(normal.g, normal.fills, normal.terms, {}) == 1);
}

// The same recurrence on a two-element state: nothing is copied, so the
// island buys no data movement at all -- only the per-op tax. The estimate
// refused this shape while the backward replayed under var, because a var
// replay of the same arithmetic cost more than the ops did; with a
// generated adjoint the corpus regions it stands for measure 1.5-1.7x
// (`hmm_example`, `hmm_gaussian`, both `hmm_drive`s). So it carves now --
// and the gradient still has to be the one the ops produced.
static void test_scalar_chain_carved() {
  HmmGraph ref = build_hmm(8);
  const std::vector<double> want = run_grad(std::move(ref.g), ref.fills);

  HmmGraph isl = build_hmm(8);
  const int carved = carve_islands(isl.g, isl.fills, isl.terms, {});
  expect("scalar chain carved==1", carved == 1);
  const std::vector<double> got = run_grad(std::move(isl.g), isl.fills);
  expect("scalar chain sizes", got.size() == want.size());
  for (size_t i = 0; i < want.size() && i < got.size(); ++i)
    expect_close("scalar chain v" + std::to_string(i), got[i], want[i]);
}

// A recurrence threaded through ops the register machine has no
// instruction for -- POW, a unary from the generated list, a cdf, an
// integer-outcome lpmf. Each compiles as a CALL to the graph's own
// kernel, so one op the machine cannot say stops ending the run, and
// the derivative is the kernel's own backward. The reference is the
// same graph uncarved: a CALL runs the identical kernel, so the graph
// is the arbiter, not the var replay (which cannot execute a CALL and
// never meets one).
static void test_kernel_call_ops_carved() {
  Graph g;
  Fills fills;
  const int p0 = g.add_slot(1, true);
  const int p1 = g.add_slot(1, true);
  const int seed_vec = g.add_slot(2, true);
  const int seed = g.add_slot(1, false);
  g.add_op(OP_INDEX, {seed_vec}, seed, {0});
  auto cslot = [&](double v) {
    const int s = g.add_slot(1, false);
    fills.emplace_back(s, std::vector<double>{v});
    return s;
  };
  // INDEX compiles to an aliasable MOV before the first CALL. Compacting
  // that shared cell shifts the CALL's later ranges, so the assertions
  // below exercise mapped addressing rather than identity by accident.
  int acc = seed;
  for (int t = 0; t < 12; ++t) {
    // inv_logit keeps the recurrence in (0, 1): pow of a negative base at
    // a non-integer exponent is NaN, and tanh below goes negative.
    const int il = g.add_slot(1, false);
    g.add_op(OP_INV_LOGIT, {acc}, il);
    const int pw = g.add_slot(1, false);
    g.add_op(OP_POW, {il, p1}, pw);  // out of vocabulary until CALL
    const int sq = g.add_slot(1, false);
    g.add_op(OP_SQUARE, {pw}, sq);
    const int lg = g.add_slot(1, false);
    g.add_op(OP_LGAMMA, {sq}, lg);  // generated-unary list
    const int sm = g.add_slot(1, false);
    g.add_op(OP_ADD, {lg, cslot(0.15 * t + 0.4)}, sm);
    const int cd = g.add_slot(1, false);
    g.add_op(OP_NORMAL_LCDF, {cslot(0.3), sm, cslot(2.0)}, cd);  // cdf
    const int pm = g.add_slot(1, false);
    {
      Op lp;  // poisson_lpmf(2 | exp-ish rate): int outcome rides in idata
      lp.opcode = OP_POISSON_LPMF;
      lp.n_in = 1;
      lp.in[0] = sq;
      lp.out = pm;
      lp.variant = 0x01;  // rate active; propto OFF
      g.idata_pool.push_back({2});
      lp.idata = g.idata_pool.back().data();
      lp.n_idata = 1;
      g.ops.push_back(lp);
    }
    const int nx = g.add_slot(1, false);
    g.add_op(OP_ADD, {cd, pm}, nx);
    const int th = g.add_slot(1, false);
    g.add_op(OP_TANHV, {nx}, th);
    acc = th;
  }
  const int lp = g.add_slot(1, false);
  g.add_op(OP_ADD, {acc, p0}, lp);
  g.result_slot = lp;
  std::vector<int> terms{lp};

  Graph ref = g;
  const std::vector<double> want = run_grad(std::move(ref), fills);
  const int carved = carve_islands(g, fills, terms, {});
  expect("callops carved==1", carved == 1);
  bool shifted_call_range = false;
  for (const Op& op : g.ops) {
    if (op.opcode != OP_ISLAND) continue;
    const auto& p = *static_cast<const IslandProg*>(op.udata);
    for (const Program::Call& call : p.calls) {
      auto check_range = [&](int reg, int len) {
        if (len == 0) return;
        const int base = p.adj.adj_reg[(size_t)reg];
        expect("call compact base", base >= 0 && base + len <= p.adj.n_regs);
        for (int k = 1; k < len; ++k)
          expect("call compact contiguous",
                 p.adj.adj_reg[(size_t)(reg + k)] == base + k);
        if (base != reg) shifted_call_range = true;
      };
      for (int k = 0; k < call.n_in; ++k)
        check_range(call.in[k], call.in_len[k]);
      check_range(call.out, call.out_len);
    }
  }
  expect("call range actually compacted", shifted_call_range);
  const std::vector<double> got = run_grad_twice(std::move(g), fills);
  expect("callops sizes", got.size() == want.size());
  for (size_t i = 0; i < want.size() && i < got.size(); ++i)
    expect_close("callops v" + std::to_string(i), got[i], want[i]);
}

static void test_too_many_live_ins() {
  Graph g;
  Fills fills;
  std::vector<int> ps;
  for (int k = 0; k < 7; ++k) ps.push_back(g.add_slot(1, true));
  int acc = ps[0];
  for (int k = 1; k < 7; ++k) {
    const int s = g.add_slot(1, false);
    g.add_op(OP_ADD, {acc, ps[k]}, s);
    acc = s;
  }
  for (int k = 0; k < 40; ++k) {
    const int s = g.add_slot(1, false);
    g.add_op(OP_TANHV, {acc}, s);
    acc = s;
  }
  const int lp = g.add_slot(1, false);
  g.add_op(OP_SQUARE, {acc}, lp);
  g.result_slot = lp;
  std::vector<int> terms{lp};
  const size_t before = g.ops.size();
  const int carved = carve_islands(g, fills, terms, {});
  expect("livein7 none carved", carved == 0);
  expect("livein7 ops unchanged", g.ops.size() == before);
}

static void test_six_live_ins_ok() {
  Graph g;
  Fills fills;
  std::vector<int> ps;
  for (int k = 0; k < 6; ++k) ps.push_back(g.add_slot(1, true));
  int acc = ps[0];
  for (int k = 1; k < 6; ++k) {
    const int s = g.add_slot(1, false);
    g.add_op(OP_ADD, {acc, ps[k]}, s);
    acc = s;
  }
  for (int k = 0; k < 40; ++k) {
    const int s = g.add_slot(1, false);
    g.add_op(OP_TANHV, {acc}, s);
    acc = s;
  }
  const int lp = g.add_slot(1, false);
  g.add_op(OP_SQUARE, {acc}, lp);
  g.result_slot = lp;
  std::vector<int> terms{lp};

  Graph ref = g;
  const std::vector<double> want = run_grad(std::move(ref), fills);
  const int carved = carve_islands(g, fills, terms, {});
  expect("livein6 carved==1", carved == 1);
  const std::vector<double> got = run_grad(std::move(g), fills);
  for (size_t i = 0; i < want.size() && i < got.size(); ++i)
    expect_close("livein6 v" + std::to_string(i), got[i], want[i]);
}

// A slot PRODUCED BEFORE the region, then read and updated in place inside
// it, and read again after: it is a live-in and a live-out at once. If the
// island's extraction wrote that same slot, its adjoint buffer would hold
// two different quantities at once -- the extraction's backward leaves
// d(lp)/d(slot-after-region) there, and the island's backward then adds
// d(lp)/d(slot-before-region) on top. The producer's backward, which runs
// later in the reverse sweep, would read the sum and double-count. The
// extraction gets a fresh slot instead, so this checks the gradient with
// respect to the producer's own parameter.
static void test_live_in_and_out_slot() {
  Graph g;
  Fills fills;
  const int seedp = g.add_slot(1, true);  // feeds the producer
  const int a = g.add_slot(1, true);
  const int vec = g.add_slot(10, false);
  g.add_op(OP_REP_VEC, {seedp}, vec);  // the producer, before the region
  auto cslot = [&](double v) {
    const int s = g.add_slot(1, false);
    fills.emplace_back(s, std::vector<double>{v});
    return s;
  };
  // Each lane reads vec[k] (the producer's value), and writes vec[k]
  // destructively, exactly as an unrolled `x[k] = f(x[k])` loop lowers.
  for (int k = 0; k < 10; ++k) {
    const int e = g.add_slot(1, false);
    g.add_op(OP_INDEX, {vec}, e, {k});
    const int m = g.add_slot(1, false);
    g.add_op(OP_MUL, {e, a}, m);
    const int s1 = g.add_slot(1, false);
    g.add_op(OP_ADD, {m, cslot(0.2 * k)}, s1);
    const int t = g.add_slot(1, false);
    g.add_op(OP_TANHV, {s1}, t);
    Op si;  // destructive, as make_inplace_updates emits
    si.opcode = OP_SET_INDEX_INPLACE;
    si.n_in = 2;
    si.in[0] = vec;
    si.in[1] = t;
    si.out = vec;
    g.idata_pool.push_back({k});
    si.idata = g.idata_pool.back().data();
    si.n_idata = 1;
    g.ops.push_back(si);
  }
  const int lp = g.add_slot(1, false);
  g.add_op(OP_LOG_SUM_EXP, {vec}, lp);
  g.result_slot = lp;
  std::vector<int> terms{lp};

  Graph ref = g;
  const std::vector<double> want = run_grad(std::move(ref), fills);
  const int carved = carve_islands(g, fills, terms, {});
  expect("liveinout carved==1", carved == 1);
  expect("liveinout vec is live-in",
         g.ops[1].opcode == OP_ISLAND && g.ops[1].n_in >= 1);
  const std::vector<double> got = run_grad(std::move(g), fills);
  for (size_t i = 0; i < want.size() && i < got.size(); ++i)
    expect_close("liveinout v" + std::to_string(i), got[i], want[i]);
}

static Graph build_slice_island(int n_updates, int width,
                                std::vector<int>& terms) {
  Graph g;
  const int seed = g.add_slot(1, true);
  const int rhs = g.add_slot(2, true);
  const int vec = g.add_slot(width, false);
  g.add_op(OP_REP_VEC, {seed}, vec);  // vector-out barrier before the region
  for (int k = 0; k < n_updates; ++k)
    // Repeated overlapping windows exercise last-write-wins in the generated
    // adjoint: each later MOVR must consume the cells before an earlier one.
    g.add_op(OP_SET_SLICE_INPLACE, {vec, rhs}, vec, {k % 5});
  const int lp = g.add_slot(1, false);
  g.add_op(OP_LOG_SUM_EXP, {vec}, lp);
  g.result_slot = lp;
  terms.push_back(lp);
  return g;
}

static void test_inplace_slices_carved() {
  std::vector<int> ref_terms;
  Graph ref = build_slice_island(36, 40, ref_terms);
  const std::vector<double> want = run_grad(std::move(ref), {});

  std::vector<int> terms;
  Graph g = build_slice_island(36, 40, terms);
  Fills fills;
  expect("slice inplace island carved",
         carve_islands(g, fills, terms, {}) == 1);
  const std::vector<double> got = run_grad(std::move(g), {});
  for (size_t i = 0; i < want.size() && i < got.size(); ++i)
    expect_close("slice island v" + std::to_string(i), got[i], want[i]);
}

// In-place slice cost is the RHS window, not the wide aliased output. If it
// were charged as a full-vector copy, this register-heavy region would look
// profitable and the carver would build a 4K-register island for tiny writes.
static void test_inplace_slice_cost_refuses_wide_state() {
  std::vector<int> terms;
  Graph g = build_slice_island(36, 4096, terms);
  Fills fills;
  const size_t before = g.ops.size();
  expect("wide inplace slices not carved",
         carve_islands(g, fills, terms, {}) == 0);
  expect("wide inplace slice graph unchanged", g.ops.size() == before);
}

int main() {
  // What the compiler does with a region, on graphs small enough to
  // reason about. The cost estimate would refuse most of them -- it is
  // policy, tested separately below, and these are about correctness.
  test_branch_bound_live_out();
  test_setenv("STANLI_ISLAND_ALWAYS", "1", 1);
  test_hmm_parity();
  test_env_disable();
  test_live_in_and_out_slot();
  test_inplace_slices_carved();
  test_short_run_untouched();
  test_propto_density_refused();
  test_unsupported_op_splits();
  test_too_many_live_ins();
  test_six_live_ins_ok();
  test_kernel_call_ops_carved();

  test_unsetenv("STANLI_ISLAND_ALWAYS");
  test_wide_state_refused();
  test_vector_copies_carved();
  test_compact_adjoint_cost_boundary();
  test_scalar_chain_carved();
  test_inplace_slice_cost_refuses_wide_state();
  if (failures) {
    std::printf("%d failures\n", failures);
    return 1;
  }
  std::printf("test_island: all passed\n");
  return 0;
}
