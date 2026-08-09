// Safety properties the graph passes depend on, tested directly rather
// than through the models that happen to exercise them.
//
// 1. The whitelist is real. `backward_ignores_input_values(op)` is what
//    lets a destructive write happen before an op's backward runs. Every
//    opcode it claims is checked by poisoning the input buffers with NaN
//    between the forward and backward sweeps: a kernel that reads values
//    there produces NaN adjoints and fails. This is the check that would
//    have caught the log_sum_exp bug immediately -- it was found instead
//    by the corpus A/B, after 8 models had been silently wrong.
//
// 2. The passes are gradient-preserving on randomly generated graphs
//    built from the shapes real models produce (write chains, read-backs,
//    whole-vector reads, elementwise arithmetic, densities), including
//    the interleavings no hand-written test would think to write.
#include "env_helpers.hpp"
#include <stanli/graph.hpp>
#include <stanli/inplace.hpp>
#include <stanli/island.hpp>
#include <stanli/optable.hpp>
#include <stanli/reroll.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <utility>
#include <vector>

static int failures = 0;
static void expect(const char* what, bool ok) {
  if (!ok) { ++failures; std::printf("FAIL %s\n", what); }
}

using namespace stanli;
using Fills = std::vector<std::pair<int, std::vector<double>>>;

// ---- 1. the whitelist ------------------------------------------------------

// Runs one opcode's forward, optionally poisons every input value, then
// runs its backward and returns the resulting input adjoints.
static std::vector<double> route_adjoints(uint16_t oc, bool poison) {
  const int64_t N = 6;
  std::vector<double> vec(N), out(N, 0.0), scalar{0.75};
  std::vector<double> vec_adj(N, 0.0), out_adj(N, 0.0), scalar_adj{0.0};
  // Extra operands for the multi-argument mixture ops, and the scratch the
  // native ones stash partials in (a separate arena a destructive write
  // never touches -- which is exactly why their backward is value-free).
  std::vector<double> scratch((size_t)N + 4, 0.0);
  std::vector<double> b_in{0.4}, c_in{-0.2}, b_adj{0.0}, c_adj{0.0};
  for (int64_t i = 0; i < N; ++i) vec[i] = 0.5 + 0.25 * i;
  std::vector<int> idata;
  KernelCtx ctx;
  ctx.n_in = 1;
  ctx.in[0] = Desc{vec.data(), N};
  ctx.in_adj[0] = Desc{vec_adj.data(), N};
  ctx.scratch = scratch.data();
  int64_t out_len = N;
  switch (oc) {
    case OP_INDEX:
      idata = {3};
      out_len = 1;
      break;
    case OP_SLICE:
      idata = {2};
      out_len = 3;
      break;
    case OP_SLICE_STRIDED:
      idata = {1, 2};
      out_len = 2;
      break;
    case OP_GATHER:
      idata = {4, 1, 1, 0};  // duplicates: the scatter-add path
      out_len = 4;
      break;
    case OP_SET_INDEX:
    case OP_SET_INDEX_INPLACE:
      idata = {2};
      out_len = N;
      ctx.n_in = 2;
      ctx.in[1] = Desc{scalar.data(), 1};
      ctx.in_adj[1] = Desc{scalar_adj.data(), 1};
      break;
    case OP_LOG_SUM_EXP:
      out_len = 1;
      break;
    case OP_LSE2:
      // Two scalar inputs; reuse the vector buffer's first element.
      ctx.n_in = 2;
      ctx.in[0] = Desc{vec.data(), 1};
      ctx.in[1] = Desc{b_in.data(), 1};
      ctx.in_adj[0] = Desc{vec_adj.data(), 1};
      ctx.in_adj[1] = Desc{b_adj.data(), 1};
      out_len = 1;
      break;
    case OP_LOG_MIX:
      // theta must be a probability.
      vec[0] = 0.35;
      ctx.n_in = 3;
      ctx.in[0] = Desc{vec.data(), 1};
      ctx.in[1] = Desc{b_in.data(), 1};
      ctx.in[2] = Desc{c_in.data(), 1};
      ctx.in_adj[0] = Desc{vec_adj.data(), 1};
      ctx.in_adj[1] = Desc{b_adj.data(), 1};
      ctx.in_adj[2] = Desc{c_adj.data(), 1};
      out_len = 1;
      break;
    default:
      return {};
  }
  // The in-place form writes through its first input: one buffer.
  const bool aliased = oc == OP_SET_INDEX_INPLACE;
  ctx.out = Desc{aliased ? vec.data() : out.data(), out_len};
  ctx.idata = idata.data();
  ctx.n_idata = (int64_t)idata.size();

  const Kernel& k = kernel(oc);
  k.forward(ctx);

  // Seed adjoints, then destroy the values the way a later destructive
  // write would. Adjoint buffers are deliberately left alone.
  for (int64_t i = 0; i < out_len; ++i) out_adj[i] = 1.0 + 0.5 * i;
  ctx.out_adj = out_adj[0];
  ctx.out_adj_vec = Desc{out_adj.data(), out_len};
  if (poison) {
    const double nan = std::nan("");
    for (int64_t i = 0; i < N; ++i) vec[i] = nan;
    scalar[0] = nan;
    b_in[0] = nan;
    c_in[0] = nan;
    if (!aliased)
      for (int64_t i = 0; i < out_len; ++i) out[i] = nan;
  }
  k.backward(ctx);

  std::vector<double> got = vec_adj;
  got.push_back(scalar_adj[0]);
  got.push_back(b_adj[0]);
  got.push_back(c_adj[0]);
  return got;
}

static void test_whitelist_backwards_ignore_values() {
  // Kernels register on first Executor construction; this test calls them
  // directly, so build a trivial one to populate the table.
  {
    Graph g;
    const int a = g.add_slot(1, true);
    const int o = g.add_slot(1, false);
    g.add_op(OP_EXP, {a}, o);
    g.result_slot = o;
    Executor warm(std::move(g));
    (void)warm.n_params();
  }
  int checked = 0;
  for (uint16_t oc = 1; oc < OP_COUNT_; ++oc) {
    if (!backward_ignores_input_values(oc)) continue;
    const std::string name = opcode_name(oc);
    const std::vector<double> clean = route_adjoints(oc, /*poison=*/false);
    if (clean.empty()) {
      // The whitelist grew an opcode this test does not know how to set
      // up; the pass would be trusting an unverified claim.
      ++failures;
      std::printf("FAIL %s is whitelisted but untested here\n", name.c_str());
      continue;
    }
    ++checked;
    const std::vector<double> poisoned = route_adjoints(oc, /*poison=*/true);
    for (size_t i = 0; i < clean.size(); ++i) {
      if (clean[i] != poisoned[i] || !std::isfinite(poisoned[i])) {
        ++failures;
        std::printf("FAIL %s backward reads input values (adj[%zu] %g -> %g)\n",
                    name.c_str(), i, clean[i], poisoned[i]);
        break;
      }
    }
  }
  expect("whitelist is non-empty", checked >= 6);
}

// ---- 2. randomized graphs --------------------------------------------------

static std::vector<double> run_grad(Graph g, const Fills& fills) {
  Executor ex(std::move(g));
  for (const auto& f : fills) {
    double* p = ex.value_ptr(f.first);
    for (size_t j = 0; j < f.second.size(); ++j) p[j] = f.second[j];
  }
  for (int64_t i = 0; i < ex.n_params(); ++i)
    ex.params_data()[i] = 0.3 + 0.2 * std::sin(0.7 * (double)i);
  std::vector<double> out(1 + ex.n_params());
  out[0] = ex.gradient(out.data() + 1);
  return out;
}

// Builds a graph in the shapes lowering produces, with the mix chosen by
// the RNG: element writes, same-element read-backs, whole-vector reads
// (whose backward replays on values), elementwise arithmetic, densities.
static Graph random_graph(std::mt19937& rng, Fills& fills,
                          std::vector<int>& terms) {
  std::uniform_int_distribution<int> coin(0, 99);
  const int L = 4 + coin(rng) % 5;
  Graph g;
  const int mu = g.add_slot(1, true);
  const int sigma = g.add_slot(1, true);
  const int vecp = g.add_slot(L, true);
  const int base = g.add_slot(L, false);
  fills.emplace_back(base, std::vector<double>((size_t)L, 0.0));
  auto cslot = [&](double v) {
    const int s = g.add_slot(1, false);
    fills.emplace_back(s, std::vector<double>{v});
    return s;
  };

  int vec = base;
  const int steps = 6 + coin(rng) % 10;
  for (int s = 0; s < steps; ++s) {
    const int k = coin(rng) % L;
    switch (coin(rng) % 5) {
      case 0: {  // write an element
        const int v = g.add_slot(1, false);
        g.add_op(OP_MUL, {mu, cslot(0.2 + 0.3 * ((s % 4) + 1))}, v);
        const int nxt = g.add_slot(L, false);
        g.add_op(OP_SET_INDEX, {vec, v}, nxt, {k});
        vec = nxt;
        break;
      }
      case 1: {  // read one element back into a density term
        const int rd = g.add_slot(1, false);
        g.add_op(OP_INDEX, {vec}, rd, {k});
        const int lp = g.add_slot(1, false);
        const int id = g.add_op(OP_NORMAL_LPDF, {cslot(0.1 * s), rd, sigma},
                                lp);
        g.ops[id].variant = 0x06;
        terms.push_back(lp);
        break;
      }
      case 2: {  // whole-vector read: backward replays on the values
        const int lse = g.add_slot(1, false);
        g.add_op(OP_LOG_SUM_EXP, {vec}, lse);
        terms.push_back(lse);
        break;
      }
      case 3: {  // indexed read of a parameter vector (gather/slice shape)
        const int rd = g.add_slot(1, false);
        g.add_op(OP_INDEX, {vecp}, rd, {k});
        const int lp = g.add_slot(1, false);
        const int id = g.add_op(OP_NORMAL_LPDF, {cslot(0.3 - 0.05 * s), rd,
                                                 sigma}, lp);
        g.ops[id].variant = 0x06;
        terms.push_back(lp);
        break;
      }
      default: {  // plain arithmetic feeding a term
        const int a = g.add_slot(1, false);
        g.add_op(OP_ADD, {mu, cslot(0.4 * ((s % 3) + 1))}, a);
        const int lp = g.add_slot(1, false);
        const int id = g.add_op(OP_NORMAL_LPDF, {cslot(0.2 * s), a, sigma},
                                lp);
        g.ops[id].variant = 0x06;
        terms.push_back(lp);
        break;
      }
    }
  }
  if (terms.empty()) {
    const int lse = g.add_slot(1, false);
    g.add_op(OP_LOG_SUM_EXP, {vec}, lse);
    terms.push_back(lse);
  }
  return g;
}

static void reduce_into_result(Graph& g, const std::vector<int>& terms) {
  int acc = terms[0];
  for (size_t k = 1; k < terms.size(); ++k) {
    const int s = g.add_slot(1, false);
    g.add_op(OP_ADD_N, {acc, terms[k]}, s);
    acc = s;
  }
  g.result_slot = acc;
}

static void test_random_graphs_preserve_gradients() {
  std::mt19937 rng(20260806u);
  int worst_at = -1;
  double worst = 0.0;
  for (int trial = 0; trial < 400; ++trial) {
    Fills fills;
    std::vector<int> terms;
    Graph g = random_graph(rng, fills, terms);

    Graph ref = g;
    std::vector<int> ref_terms = terms;
    reduce_into_result(ref, ref_terms);
    const std::vector<double> want = run_grad(std::move(ref), fills);

    Fills f2 = fills;
    std::vector<int> tt = terms;
    make_inplace_updates(g, {});
    forward_stores_to_loads(g, {});
    reroll(g, f2, tt, {});
    // The whole pipeline, in order. Islands are forced on: these graphs
    // are small, so the pass's cost estimate would decline nearly all of
    // them, and it is the compiler that this test is for.
    carve_islands(g, f2, tt, {});
    reduce_into_result(g, tt);
    const std::vector<double> got = run_grad(std::move(g), f2);

    if (got.size() != want.size()) {
      ++failures;
      std::printf("FAIL trial %d: gradient size %zu != %zu\n", trial,
                  got.size(), want.size());
      continue;
    }
    for (size_t i = 0; i < want.size(); ++i) {
      if (!std::isfinite(got[i]) || !std::isfinite(want[i])) continue;
      const double rel =
          std::abs(got[i] - want[i]) / std::max(std::abs(want[i]), 1e-300);
      if (rel > worst) { worst = rel; worst_at = trial; }
    }
  }
  if (worst >= 1e-12) {
    ++failures;
    std::printf("FAIL random graphs: worst rel %.3e at trial %d\n", worst,
                worst_at);
  } else {
    std::printf("random graphs: 400 trials, worst rel %.2e\n", worst);
  }
}

int main() {
  test_setenv("STANLI_ISLAND_ALWAYS", "1", 1);  // see the fuzz loop
  test_whitelist_backwards_ignore_values();
  test_random_graphs_preserve_gradients();
  if (failures) { std::printf("%d failures\n", failures); return 1; }
  std::printf("test_pass_safety OK\n");
  return 0;
}
