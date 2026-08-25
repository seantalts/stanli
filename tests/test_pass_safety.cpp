// Safety properties the graph passes depend on, tested directly rather
// than through the models that happen to exercise them.
//
// 1. The whitelist is real. `backward_ignores_values(op)` is what
//    lets a destructive write happen before an op's backward runs. Every
//    opcode it claims is checked by poisoning every input and output value
//    buffer with NaN between the forward and backward sweeps: a kernel that
//    reads values there produces NaN adjoints and fails. This is the check
//    that would have caught the log_sum_exp bug immediately -- it was found
//    instead by the corpus A/B, after 8 models had been silently wrong.
//
// 2. The passes are gradient-preserving on randomly generated graphs
//    built from the shapes real models produce (write chains, read-backs,
//    whole-vector reads, elementwise arithmetic, densities), including
//    the interleavings no hand-written test would think to write.
#include "env_helpers.hpp"
#include "graph_helpers.hpp"
#include <stanli/constfold.hpp>
#include <stanli/graph.hpp>
#include <stanli/inplace.hpp>
#include <stanli/island.hpp>
#include <stanli/optable.hpp>
#include <stanli/partition.hpp>
#include <stanli/reroll.hpp>
#include <stanli/wa_interp.hpp>

#include <stan/math.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
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

using namespace stanli;
using stanli::testutil::Fills;
using stanli::testutil::reduce_into_result;

// ---- 1. the whitelist ------------------------------------------------------

// Runs one opcode's forward, optionally poisons all input and output values,
// then runs its backward and returns the resulting input adjoints.
static std::vector<double> route_adjoints(uint16_t oc, bool poison) {
  const int64_t N = 6;
  std::vector<double> vec(N), out(N, 0.0), rhs{0.75, -0.3, 1.1};
  std::vector<double> vec_adj(N, 0.0), out_adj(N, 0.0), rhs_adj(3, 0.0);
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
      ctx.in[1] = Desc{rhs.data(), 1};
      ctx.in_adj[1] = Desc{rhs_adj.data(), 1};
      break;
    case OP_SET_SLICE:
    case OP_SET_SLICE_INPLACE:
      idata = {2};
      out_len = N;
      ctx.n_in = 2;
      ctx.in[1] = Desc{rhs.data(), 3};
      ctx.in_adj[1] = Desc{rhs_adj.data(), 3};
      break;
    case OP_SET_SLICE_STRIDED:
    case OP_SET_SLICE_STRIDED_INPLACE:
      idata = {0, 2};
      out_len = N;
      ctx.n_in = 2;
      ctx.in[1] = Desc{rhs.data(), 3};
      ctx.in_adj[1] = Desc{rhs_adj.data(), 3};
      break;
    case OP_LOG_SUM_EXP:
      out_len = 1;
      break;
    case OP_LOG_SUM_EXP_ROWS:
    case OP_SUM_ROWS:
      idata = {3};  // two packed rows of width three
      out_len = 2;
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
  const bool aliased = oc == OP_SET_INDEX_INPLACE ||
                       oc == OP_SET_SLICE_INPLACE ||
                       oc == OP_SET_SLICE_STRIDED_INPLACE;
  ctx.out = Desc{aliased ? vec.data() : out.data(), out_len};
  ctx.idata = idata.data();
  ctx.n_idata = (int64_t)idata.size();

  const Kernel& k = kernel(oc);
  k.forward(ctx);

  // Seed adjoints, then destroy the values the way a later destructive
  // write would. Adjoint buffers are deliberately left alone.
  double* seeded_adj = aliased ? vec_adj.data() : out_adj.data();
  for (int64_t i = 0; i < out_len; ++i) seeded_adj[i] = 1.0 + 0.5 * i;
  ctx.out_adj = seeded_adj[0];
  ctx.out_adj_vec = Desc{seeded_adj, out_len};
  if (poison) {
    const double nan = std::nan("");
    for (int64_t i = 0; i < N; ++i) vec[i] = nan;
    for (double& x : rhs) x = nan;
    b_in[0] = nan;
    c_in[0] = nan;
    double* output_values = aliased ? vec.data() : out.data();
    for (int64_t i = 0; i < out_len; ++i) output_values[i] = nan;
  }
  k.backward(ctx);

  std::vector<double> got = vec_adj;
  got.insert(got.end(), rhs_adj.begin(), rhs_adj.end());
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
    if (!backward_ignores_values(oc)) continue;
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
        std::printf("FAIL %s backward reads values (adj[%zu] %g -> %g)\n",
                    name.c_str(), i, clean[i], poisoned[i]);
        break;
      }
    }
  }
  expect("whitelist is non-empty", checked >= 6);
}

// ---- 2. randomized graphs --------------------------------------------------

static double fill_at(int64_t i) {
  return 0.3 + 0.2 * std::sin(0.7 * (double)i);
}
static std::vector<double> run_grad(Graph g, const Fills& fills) {
  return testutil::run_grad(std::move(g), fills, fill_at);
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
        const int id =
            g.add_op(OP_NORMAL_LPDF, {cslot(0.1 * s), rd, sigma}, lp);
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
        const int id =
            g.add_op(OP_NORMAL_LPDF, {cslot(0.3 - 0.05 * s), rd, sigma}, lp);
        g.ops[id].variant = 0x06;
        terms.push_back(lp);
        break;
      }
      default: {  // plain arithmetic feeding a term
        const int a = g.add_slot(1, false);
        g.add_op(OP_ADD, {mu, cslot(0.4 * ((s % 3) + 1))}, a);
        const int lp = g.add_slot(1, false);
        const int id = g.add_op(OP_NORMAL_LPDF, {cslot(0.2 * s), a, sigma}, lp);
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
    make_inplace_updates(g, tt);  // slice stores reroll just created
    partition_lanes(g, f2, tt, {});
    make_inplace_updates(g, tt);
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
      if (rel > worst) {
        worst = rel;
        worst_at = trial;
      }
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

static void test_rng_is_an_effect_barrier() {
  Graph g;
  Fills fills;
  const int probabilities = g.add_slot(4, false);
  fills.emplace_back(probabilities,
                     std::vector<double>{0.125, 0.25, 0.375, 0.25});
  const int location = g.add_slot(2, false);
  const int covariance = g.add_slot(4, false);
  fills.emplace_back(location, std::vector<double>{0.5, -1.25});
  fills.emplace_back(covariance, std::vector<double>{1.69, 0.364, 0.364, 0.64});
  std::vector<int> roots;
  for (int i = 0; i < 40; ++i) {
    const bool vector_draw = i == 39;
    const int out = g.add_slot(vector_draw ? 2 : 1, false);
    if (vector_draw) {
      g.add_op(OP_RNG, {location, covariance}, out, {2});
      g.ops.back().variant = kMultiNormalRngVariant;
    } else {
      g.add_op(OP_RNG, {probabilities}, out);
      g.ops.back().variant = kCategoricalRngVariant;
    }
    if (i == 39) roots.push_back(out);
  }
  g.result_slot = roots.back();
  const ConstFoldStats folded = const_fold(g, fills, roots);
  std::vector<int> terms;
  const RerollStats rerolled = reroll(g, fills, terms, roots);
  const int islands = carve_islands(g, fills, terms, roots);
  int rng_ops = 0;
  for (const Op& op : g.ops)
    if (op.opcode == OP_RNG) ++rng_ops;
  expect("RNG effects survive constfold", folded.ops_removed == 0);
  expect("RNG effects survive reroll/hoist", rerolled.regions == 0);
  expect("RNG effects split islands", islands == 0);
  expect("RNG effect count/order preserved", rng_ops == 40);

  // The structural checks above are opcode-wide; executing the vector-input
  // variant additionally proves that preserving 40 ops preserved their
  // stream order, not merely their count.
  Executor ex(std::move(g));
  for (const auto& fill : fills) {
    double* values = ex.value_ptr(fill.first);
    for (size_t i = 0; i < fill.second.size(); ++i) values[i] = fill.second[i];
  }
  Eigen::VectorXd theta(4);
  theta << 0.125, 0.25, 0.375, 0.25;
  WaRng got_rng(20260824), want_rng(20260824);
  ex.run_forward_only(EvalState{&got_rng});
  for (int i = 0; i < 39; ++i)
    (void)stan::math::categorical_rng(theta, want_rng.gen());
  Eigen::VectorXd mu(2);
  mu << 0.5, -1.25;
  Eigen::MatrixXd sigma(2, 2);
  sigma << 1.69, 0.364, 0.364, 0.64;
  const Eigen::VectorXd want =
      stan::math::multi_normal_rng(mu, sigma, want_rng.gen());
  expect("vector RNG effect order preserved",
         ex.value_ptr(roots.back())[0] == want[0] &&
             ex.value_ptr(roots.back())[1] == want[1]);
  expect("vector RNG next state preserved",
         got_rng.gen()() == want_rng.gen()());
}

static void test_product_is_forward_only_pass_barrier() {
  Graph g;
  Fills fills;
  const int input = g.add_slot(5, true);
  constexpr int kLanes = 20;
  const int result = g.add_slot(kLanes, false);
  fills.emplace_back(result, std::vector<double>(kLanes, 0.0));
  // Each period contains a candidate element store, so reroll reaches
  // ops_match and must reject PROD explicitly.  Without that vocabulary
  // barrier the invariant product is eligible to hoist across all lanes.
  for (int lane = 0; lane < kLanes; ++lane) {
    const int output = g.add_slot(1, false);
    g.add_op(OP_PROD_VEC, {input}, output);
    g.add_op(OP_SET_INDEX_INPLACE, {result, output}, result, {lane});
  }
  const int final = g.add_slot(1, false);
  g.add_op(OP_SUM_VEC, {result}, final);
  g.result_slot = final;
  std::vector<int> roots{final};
  std::vector<int> terms;

  const ConstFoldStats folded = const_fold(g, fills, roots);
  const RerollStats rerolled = reroll(g, fills, terms, roots);
  const int islands = carve_islands(g, fills, terms, roots);
  int products = 0, stores = 0, island_ops = 0;
  for (const Op& op : g.ops) {
    products += op.opcode == OP_PROD_VEC;
    stores += op.opcode == OP_SET_INDEX_INPLACE;
    island_ops += op.opcode == OP_ISLAND;
  }
  expect("product has no backward kernel",
         find_kernel(OP_PROD_VEC) != nullptr &&
             kernel(OP_PROD_VEC).backward == nullptr);
  expect("product is absent from value-free backward traits",
         !backward_ignores_values(OP_PROD_VEC));
  expect("product survives constfold with parameter input",
         folded.ops_removed == 0);
  expect("product is absent from reroll vocabulary", rerolled.regions == 0);
  expect("product is absent from island CALL vocabulary", islands == 0);
  expect("product remains a standalone forward op",
         products == kLanes && stores == kLanes && island_ops == 0);

  Executor ex(std::move(g));
  const double values[] = {1e200, 1e200, 1e-200, 1e-200, 3.0};
  for (int i = 0; i < 5; ++i) ex.params_data()[i] = values[i];
  ex.run_forward_only();
  Eigen::VectorXd pinned(5);
  for (int i = 0; i < 5; ++i) pinned[i] = values[i];
  const double want = stan::math::prod(pinned);
  bool exact = true;
  for (int lane = 0; lane < kLanes; ++lane)
    exact &= ex.value_ptr(result)[lane] == want;
  exact &= ex.value_ptr(final)[0] == static_cast<double>(kLanes) * want;
  expect("standalone products keep pinned Stan Math grouping", exact);
}

static void test_extrema_is_forward_only_pass_barrier() {
  Graph g;
  Fills fills;
  const int input = g.add_slot(7, true);
  constexpr int kLanes = 40;
  const int result = g.add_slot(kLanes, false);
  fills.emplace_back(result, std::vector<double>(kLanes, 0.0));
  // This is intentionally a real reroll candidate: with EXTREMA removed from
  // ops_match's explicit refusal, the two-lane min/max period plus advancing
  // stores forms a region.  Root only the downstream sum; rooting the vector
  // being filled would itself make the element-store candidate ineligible.
  for (int lane = 0; lane < kLanes; ++lane) {
    const int output = g.add_slot(1, false);
    g.add_op(OP_EXTREMA_VEC, {input}, output);
    g.ops.back().variant = static_cast<uint8_t>(lane & 1);
    g.add_op(OP_SET_INDEX_INPLACE, {result, output}, result, {lane});
  }
  const int final = g.add_slot(1, false);
  g.add_op(OP_SUM_VEC, {result}, final);
  g.result_slot = final;
  std::vector<int> roots{final};
  std::vector<int> terms;

  const ConstFoldStats folded = const_fold(g, fills, roots);
  const RerollStats rerolled = reroll(g, fills, terms, roots);
  const int store_islands = carve_islands(g, fills, terms, roots);
  int extrema_ops = 0, stores = 0, island_ops = 0;
  for (const Op& op : g.ops) {
    extrema_ops += op.opcode == OP_EXTREMA_VEC;
    stores += op.opcode == OP_SET_INDEX_INPLACE;
    island_ops += op.opcode == OP_ISLAND;
  }
  const Kernel* extrema = find_kernel(OP_EXTREMA_VEC);
  expect("extrema has a forward-only no-scratch kernel",
         extrema != nullptr && extrema->backward == nullptr &&
             extrema->scratch_size == nullptr);
  expect("extrema is absent from value-free backward traits",
         !backward_ignores_values(OP_EXTREMA_VEC));
  expect("extrema survives constfold with parameter input",
         folded.ops_removed == 0);
  expect("extrema is absent from reroll vocabulary", rerolled.regions == 0);
  expect("vector-store graph does not form an island", store_islands == 0);
  expect("extrema remains a standalone forward op",
         extrema_ops == kLanes && stores == kLanes && island_ops == 0);

  Executor ex(std::move(g));
  const double values[] = {0.0, -0.0, 7.0, -11.0, 7.0, -3.0, -11.0};
  for (int i = 0; i < 7; ++i) ex.params_data()[i] = values[i];
  ex.run_forward_only();
  Eigen::VectorXd pinned(7);
  for (int i = 0; i < 7; ++i) pinned[i] = values[i];
  const double wants[] = {stan::math::min(pinned), stan::math::max(pinned)};
  bool exact = true;
  for (int lane = 0; lane < kLanes; ++lane)
    exact &= ex.value_ptr(result)[lane] == wants[lane & 1];
  exact &= ex.value_ptr(final)[0] == (kLanes / 2) * (wants[0] + wants[1]);
  expect("standalone extrema keep pinned Stan Math values", exact);

  // A distinct all-scalar graph makes the island guard diagnostic.  With
  // EXTREMA explicitly refused, each ADD run has length one and is below the
  // carving threshold.  Removing only that refusal admits the whole 80-op
  // chain and produces an island.
  Graph island_graph;
  const int island_input = island_graph.add_slot(7, true);
  int accumulator = island_graph.add_slot(1, true);
  for (int lane = 0; lane < kLanes; ++lane) {
    const int extreme = island_graph.add_slot(1, false);
    island_graph.add_op(OP_EXTREMA_VEC, {island_input}, extreme);
    island_graph.ops.back().variant = static_cast<uint8_t>(lane & 1);
    const int next = island_graph.add_slot(1, false);
    island_graph.add_op(OP_ADD, {accumulator, extreme}, next);
    accumulator = next;
  }
  island_graph.result_slot = accumulator;
  Fills island_fills;
  std::vector<int> island_terms;
  std::vector<int> island_roots{accumulator};
  const int scalar_islands =
      carve_islands(island_graph, island_fills, island_terms, island_roots);
  int scalar_extrema = 0, scalar_adds = 0;
  for (const Op& op : island_graph.ops) {
    scalar_extrema += op.opcode == OP_EXTREMA_VEC;
    scalar_adds += op.opcode == OP_ADD;
  }
  expect(
      "extrema splits otherwise island-eligible scalar chain",
      scalar_islands == 0 && scalar_extrema == kLanes && scalar_adds == kLanes);

  Executor island_ex(std::move(island_graph));
  for (int i = 0; i < 7; ++i) island_ex.params_data()[i] = values[i];
  island_ex.params_data()[7] = 0.25;
  island_ex.run_forward_only();
  expect("split scalar chain preserves extrema values",
         island_ex.value_ptr(accumulator)[0] ==
             0.25 + (kLanes / 2) * (wants[0] + wants[1]));
}

int main() {
  test_setenv("STANLI_ISLAND_ALWAYS", "1", 1);  // see the fuzz loop
  test_whitelist_backwards_ignore_values();
  test_rng_is_an_effect_barrier();
  test_product_is_forward_only_pass_barrier();
  test_extrema_is_forward_only_pass_barrier();
  test_random_graphs_preserve_gradients();
  if (failures) {
    std::printf("%d failures\n", failures);
    return 1;
  }
  std::printf("test_pass_safety OK\n");
  return 0;
}
