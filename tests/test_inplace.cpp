// In-place functional updates: a chain of OP_SET_INDEX writes into the
// same vector must collapse onto one buffer (O(N) instead of O(N^2)) with
// values and gradients unchanged.
#include "env_helpers.hpp"
#include "graph_helpers.hpp"
#include <stanli/graph.hpp>
#include <stanli/inplace.hpp>
#include <stanli/optable.hpp>

#include <cmath>
#include <cstdio>
#include <limits>
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
static void expect_close(const char* what, double got, double want) {
  const double rel = std::abs(got - want) / std::max(std::abs(want), 1e-300);
  if (!(rel < 1e-12)) {
    ++failures;
    std::printf("FAIL %-26s got %.17g want %.17g rel %.2e\n", what, got, want,
                rel);
  }
}

using namespace stanli;
using stanli::testutil::Fills;
using stanli::testutil::reduce_into_result;

static double fill_at(int64_t i) { return 0.2 + 0.1 * (i % 3); }
static std::vector<double> run_grad(Graph g, const Fills& fills) {
  return testutil::run_grad(std::move(g), fills, fill_at);
}

// Two gradient evaluations must agree: an in-place chain that wrongly
// mutated a fill-backed slot would drift on the second pass.
static std::vector<double> run_grad_twice(Graph g, const Fills& fills) {
  Executor ex(std::move(g));
  for (const auto& f : fills) {
    double* p = ex.value_ptr(f.first);
    for (size_t j = 0; j < f.second.size(); ++j) p[j] = f.second[j];
  }
  for (int64_t i = 0; i < ex.n_params(); ++i) ex.params_data()[i] = fill_at(i);
  std::vector<double> first(1 + ex.n_params()), second(1 + ex.n_params());
  first[0] = ex.gradient(first.data() + 1);
  second[0] = ex.gradient(second.data() + 1);
  for (size_t i = 0; i < first.size(); ++i)
    expect_close(("repeat v" + std::to_string(i)).c_str(), second[i], first[i]);
  return second;
}

// radon_county_intercept's shape: per lane
//   mu_next = SET_INDEX(mu_prev, alpha_g + beta*x_n, n)
//   lp_n    = NORMAL(y_n, INDEX(mu_next, n), sigma)
// The read-back makes mu_next's LAST use the next lane's SET_INDEX, so a
// single-use rule would refuse; a last-use rule accepts.
static Graph build_chain(int L, Fills& fills, std::vector<int>& terms,
                         int* n_vec_slots) {
  Graph g;
  const int beta = g.add_slot(1, true);
  const int sigma = g.add_slot(1, true);
  const int mu0 = g.add_slot(L, false);  // declared vector: fill-backed
  fills.emplace_back(mu0, std::vector<double>((size_t)L, 0.0));
  auto cslot = [&](double v) {
    const int s = g.add_slot(1, false);
    fills.emplace_back(s, std::vector<double>{v});
    return s;
  };
  int prev = mu0;
  *n_vec_slots = 1;
  for (int n = 0; n < L; ++n) {
    const int prod = g.add_slot(1, false);
    g.add_op(OP_MUL, {beta, cslot(0.1 * n - 0.3)}, prod);
    const int nxt = g.add_slot(L, false);
    ++*n_vec_slots;
    g.add_op(OP_SET_INDEX, {prev, prod}, nxt, {n});
    const int rd = g.add_slot(1, false);
    g.add_op(OP_INDEX, {nxt}, rd, {n});
    const int lp = g.add_slot(1, false);
    const int id = g.add_op(OP_NORMAL_LPDF, {cslot(0.2 * n), rd, sigma}, lp);
    g.ops[id].variant = 0x06;
    terms.push_back(lp);
    prev = nxt;
  }
  return g;
}

// Two copying slices over a fill-backed vector. The first must keep its copy
// so every evaluation starts from the fill; the second can reuse that fresh
// output. The windows overlap deliberately: reverse must clear the later
// write's cells before the earlier write routes them to its RHS.
static Graph build_slice_chain(bool strided, Fills& fills,
                               std::vector<int>& first_pos,
                               std::vector<int>& second_pos) {
  const int N = 10, K = 4;
  Graph g;
  const int a = g.add_slot(K, true);
  const int b = g.add_slot(K, true);
  const int base = g.add_slot(N, false);
  std::vector<double> base_values(N);
  for (int i = 0; i < N; ++i) base_values[(size_t)i] = -0.4 + 0.1 * i;
  fills.emplace_back(base, base_values);

  const int first = g.add_slot(N, false);
  const int second = g.add_slot(N, false);
  if (strided) {
    g.add_op(OP_SET_SLICE_STRIDED, {base, a}, first, {0, 2});
    g.add_op(OP_SET_SLICE_STRIDED, {first, b}, second, {2, 2});
    first_pos = {0, 2, 4, 6};
    second_pos = {2, 4, 6, 8};
  } else {
    g.add_op(OP_SET_SLICE, {base, a}, first, {1});
    g.add_op(OP_SET_SLICE, {first, b}, second, {3});
    first_pos = {1, 2, 3, 4};
    second_pos = {3, 4, 5, 6};
  }
  const int weights = g.add_slot(N, false);
  std::vector<double> w(N);
  for (int i = 0; i < N; ++i) w[(size_t)i] = 0.5 + 0.25 * i;
  fills.emplace_back(weights, w);
  const int result = g.add_slot(1, false);
  g.add_op(OP_DOT, {second, weights}, result);
  g.result_slot = result;
  return g;
}

static void test_slice_chain(bool strided) {
  const std::string tag = strided ? "strided slice" : "contiguous slice";
  Fills fills;
  std::vector<int> first_pos, second_pos;
  Graph g = build_slice_chain(strided, fills, first_pos, second_pos);
  Graph ref = g;
  const std::vector<double> want = run_grad(std::move(ref), fills);

  expect((tag + " one later rewrite").c_str(),
         make_inplace_updates(g, {}) == 1);
  int copying = 0, inplace = 0;
  for (const Op& op : g.ops) {
    if (strided) {
      copying += op.opcode == OP_SET_SLICE_STRIDED;
      inplace += op.opcode == OP_SET_SLICE_STRIDED_INPLACE;
    } else {
      copying += op.opcode == OP_SET_SLICE;
      inplace += op.opcode == OP_SET_SLICE_INPLACE;
    }
  }
  expect((tag + " keeps fill copy").c_str(), copying == 1);
  expect((tag + " uses one shared buffer").c_str(), inplace == 1);

  const std::vector<double> got = run_grad_twice(std::move(g), fills);
  expect((tag + " gradient sizes").c_str(), got.size() == want.size());
  for (size_t i = 0; i < want.size() && i < got.size(); ++i)
    expect_close((tag + " v" + std::to_string(i)).c_str(), got[i], want[i]);

  // Parameter slots a then b occupy the eight returned gradient cells.
  for (size_t k = 0; k < first_pos.size(); ++k) {
    bool overwritten = false;
    for (int p : second_pos) overwritten = overwritten || p == first_pos[k];
    const double expected = overwritten ? 0.0 : 0.5 + 0.25 * first_pos[k];
    expect_close((tag + " first rhs " + std::to_string(k)).c_str(), got[1 + k],
                 expected);
    expect_close((tag + " second rhs " + std::to_string(k)).c_str(),
                 got[1 + first_pos.size() + k], 0.5 + 0.25 * second_pos[k]);
  }
}

static void test_slice_rewrite_bails() {
  {  // A base read outside the graph is never overwritten.
    Fills fills;
    std::vector<int> first_pos, second_pos;
    Graph g = build_slice_chain(true, fills, first_pos, second_pos);
    const int second_base = g.ops[1].in[0];
    expect("slice root blocks rewrite",
           make_inplace_updates(g, {second_base}) == 0);
  }
  {  // Direct base/RHS aliasing is unsafe in both sweeps.
    Graph g;
    const int rhs = g.add_slot(4, true);
    const int fill = g.add_slot(4, false);
    const int fresh = g.add_slot(4, false);
    g.add_op(OP_SET_SLICE, {fill, rhs}, fresh, {0});
    const int out = g.add_slot(4, false);
    g.add_op(OP_SET_SLICE, {fresh, fresh}, out, {0});
    expect("slice base-rhs alias refused", make_inplace_updates(g, {}) == 0);
  }
  {  // A malformed comb fails closed without overflowing its bound check.
    Graph g;
    const int rhs = g.add_slot(4, true);
    const int fill = g.add_slot(8, false);
    const int fresh = g.add_slot(8, false);
    g.add_op(OP_SET_SLICE, {fill, rhs}, fresh, {0});
    const int out = g.add_slot(8, false);
    g.add_op(OP_SET_SLICE_STRIDED, {fresh, rhs}, out,
             {7, std::numeric_limits<int>::max()});
    expect("slice malformed range refused", make_inplace_updates(g, {}) == 0);
  }
  {  // Pre-existing destructive writers do not make a fill a fresh value.
    Graph g;
    const int rhs = g.add_slot(2, true);
    const int fill = g.add_slot(6, false);
    const int scalar = g.add_slot(1, true);
    g.add_op(OP_SET_INDEX_INPLACE, {fill, scalar}, fill, {0});
    const int out = g.add_slot(6, false);
    g.add_op(OP_SET_SLICE, {fill, rhs}, out, {1});
    expect("prior inplace writer is not a producer",
           make_inplace_updates(g, {}) == 0);
  }
  {  // An output-reading aliased op invalidates an earlier safe producer.
    Graph g;
    const int rhs = g.add_slot(2, true);
    const int fill = g.add_slot(4, false);
    const int fresh = g.add_slot(4, false);
    g.add_op(OP_SET_SLICE, {fill, rhs}, fresh, {0});
    g.add_op(OP_EXPV, {fresh}, fresh);
    const int out = g.add_slot(4, false);
    g.add_op(OP_SET_SLICE, {fresh, rhs}, out, {2});
    expect("aliased exp clears producer safety",
           make_inplace_updates(g, {}) == 0);
  }
  {  // A malformed destructive opcode must not certify a fresh output.
    Graph g;
    const int rhs = g.add_slot(2, true);
    const int fill = g.add_slot(4, false);
    const int fresh = g.add_slot(4, false);
    g.add_op(OP_SET_SLICE, {fill, rhs}, fresh, {0});
    const int malformed = g.add_slot(4, false);
    g.add_op(OP_SET_SLICE_INPLACE, {fresh, rhs}, malformed, {0});
    const int out = g.add_slot(4, false);
    g.add_op(OP_SET_SLICE, {malformed, rhs}, out, {2});
    expect("malformed inplace is not a safe producer",
           make_inplace_updates(g, {}) == 0);
  }
}

// Last-use and earlier-reader proofs are not enough: the op that produced the
// base can need its own output value during reverse. EXPV does. Its first
// update must therefore copy, while that copying store safely produces the
// version a second update may reuse.
static void test_output_reading_producer_keeps_copy() {
  Graph g;
  const int p = g.add_slot(4, true);
  const int first_rhs = g.add_slot(2, true);
  const int second_rhs = g.add_slot(2, true);
  const int exp_p = g.add_slot(4, false);
  g.add_op(OP_EXPV, {p}, exp_p);
  const int old = g.add_slot(1, false);
  g.add_op(OP_INDEX, {exp_p}, old, {0});
  const int first = g.add_slot(4, false);
  g.add_op(OP_SET_SLICE, {exp_p, first_rhs}, first, {0});
  const int second = g.add_slot(4, false);
  g.add_op(OP_SET_SLICE, {first, second_rhs}, second, {2});
  const int sum = g.add_slot(1, false);
  g.add_op(OP_SUM_VEC, {second}, sum);
  const int result = g.add_slot(1, false);
  g.add_op(OP_ADD, {old, sum}, result);
  g.result_slot = result;

  Graph ref = g;
  const std::vector<double> want = run_grad(std::move(ref), {});
  expect("exp producer keeps first slice copy",
         make_inplace_updates(g, {}) == 1);
  int copying = 0, inplace = 0;
  for (const Op& op : g.ops) {
    copying += op.opcode == OP_SET_SLICE;
    inplace += op.opcode == OP_SET_SLICE_INPLACE;
  }
  expect("exp producer one copying slice", copying == 1);
  expect("exp producer later slice inplace", inplace == 1);
  const std::vector<double> got = run_grad_twice(std::move(g), {});
  for (size_t i = 0; i < want.size() && i < got.size(); ++i)
    expect_close(("exp producer v" + std::to_string(i)).c_str(), got[i],
                 want[i]);
}

// Reroll can leave scalar destructive stores after the slice stores it
// creates. If an earlier slice is then aliased back to its base, both sides
// of every already-in-place op must follow that rename. Resolving only in[0]
// leaves out pointing at the tombstoned slot (the five-model corpus failure
// this regression was distilled from).
static void test_slice_rename_rebinds_existing_inplace_output() {
  Graph g;
  const int a = g.add_slot(2, true);
  const int b = g.add_slot(2, true);
  const int scalar = g.add_slot(1, true);
  const int base = g.add_slot(6, false);
  const int first = g.add_slot(6, false);
  g.add_op(OP_SET_SLICE, {base, a}, first, {0});
  const int second = g.add_slot(6, false);
  g.add_op(OP_SET_SLICE, {first, b}, second, {2});
  g.add_op(OP_SET_INDEX_INPLACE, {second, scalar}, second, {5});
  const int weights = g.add_slot(6, false);
  const int result = g.add_slot(1, false);
  g.add_op(OP_DOT, {second, weights}, result);
  g.result_slot = result;
  Fills fills{{base, {0.1, 0.2, 0.3, 0.4, 0.5, 0.6}},
              {weights, {0.5, 0.75, 1.0, 1.25, 1.5, 1.75}}};

  Graph ref = g;
  const std::vector<double> want = run_grad(std::move(ref), fills);
  expect("slice rename rewrites later copy", make_inplace_updates(g, {}) == 1);
  expect("slice rename tombstones old output", g.slots[second].len == 0);
  const Op& terminal = g.ops[2];
  expect("existing inplace output follows input rename",
         terminal.opcode == OP_SET_INDEX_INPLACE && terminal.in[0] == first &&
             terminal.out == first);
  const std::vector<double> got = run_grad_twice(std::move(g), fills);
  for (size_t i = 0; i < want.size() && i < got.size(); ++i)
    expect_close(("slice rebind v" + std::to_string(i)).c_str(), got[i],
                 want[i]);
}

// Store-to-load forwarding caches the last scalar element write. An
// intervening in-place slice invalidates that cache: the later read must see
// the slice RHS, not the stale scalar value.
static void test_slice_invalidates_store_forwarding(bool strided) {
  const std::string tag =
      strided ? "strided slice invalidates" : "slice invalidates";
  Graph g;
  const int scalar = g.add_slot(1, true);
  const int rhs = g.add_slot(2, true);
  const int template_vec = g.add_slot(4, false);
  const int base = g.add_slot(4, false);
  // This first functional store keeps its fill copy and is an explicitly
  // value-free producer. Store forwarding will cache its element.
  g.add_op(OP_SET_INDEX, {template_vec, scalar}, base, {1});
  const int updated = g.add_slot(4, false);
  if (strided)
    g.add_op(OP_SET_SLICE_STRIDED, {base, rhs}, updated, {1, 2});
  else
    g.add_op(OP_SET_SLICE, {base, rhs}, updated, {1});
  const int read = g.add_slot(1, false);
  g.add_op(OP_INDEX, {updated}, read, {1});
  // Keep the INDEX itself out of the root set so forwarding would really
  // drop it if the intervening slice failed to invalidate the cache.
  const int result = g.add_slot(1, false);
  g.add_op(OP_EXP, {read}, result);
  g.result_slot = result;

  Graph ref = g;
  const std::vector<double> want = run_grad(std::move(ref), {});
  expect((tag + " rewrites").c_str(), make_inplace_updates(g, {}) == 1);
  expect((tag + " keeps later read").c_str(),
         forward_stores_to_loads(g, {}) == 0);
  int reads = 0;
  for (const Op& op : g.ops) reads += op.opcode == OP_INDEX;
  expect((tag + " index survives").c_str(), reads == 1);
  const std::vector<double> got = run_grad_twice(std::move(g), {});
  for (size_t i = 0; i < want.size() && i < got.size(); ++i)
    expect_close((tag + " v" + std::to_string(i)).c_str(), got[i], want[i]);
}

static void test_chain_collapses() {
  const int L = 8;
  Fills fills;
  std::vector<int> terms;
  int n_vec = 0;
  Graph g = build_chain(L, fills, terms, &n_vec);
  expect("built L+1 vector slots", n_vec == L + 1);

  Graph ref = g;
  reduce_into_result(ref, terms);
  const std::vector<double> want = run_grad(std::move(ref), fills);

  const int n_inplace = make_inplace_updates(g, /*roots=*/{});
  // Every write but the first (whose source is the fill-backed mu0) can
  // become destructive.
  expect("L-1 writes made in place", n_inplace == L - 1);
  int copies = 0, inplaces = 0;
  for (const Op& op : g.ops) {
    copies += op.opcode == OP_SET_INDEX;
    inplaces += op.opcode == OP_SET_INDEX_INPLACE;
  }
  expect("one copying write left", copies == 1);
  expect("rest in place", inplaces == L - 1);

  reduce_into_result(g, terms);
  const std::vector<double> got = run_grad_twice(std::move(g), fills);
  expect("sizes", got.size() == want.size());
  for (size_t i = 0; i < want.size() && i < got.size(); ++i)
    expect_close(("chain v" + std::to_string(i)).c_str(), got[i], want[i]);
}

// A later reader of the pre-write vector forbids the rewrite: the write is
// no longer the last use.
static void test_bail_later_reader() {
  const int L = 6;
  Fills fills;
  std::vector<int> terms;
  int n_vec = 0;
  Graph g = build_chain(L, fills, terms, &n_vec);
  // Find the second SET_INDEX and read its INPUT vector after the chain.
  int second_in = -1, seen = 0;
  for (const Op& op : g.ops)
    if (op.opcode == OP_SET_INDEX && ++seen == 2) second_in = op.in[0];
  expect("found second write", second_in >= 0);
  const int late = g.add_slot(1, false);
  g.add_op(OP_INDEX, {second_in}, late, {0});  // reads a mid-chain value
  const int lp = g.add_slot(1, false);
  // slot 1 is build_chain's sigma parameter (positive at the eval point).
  const int id = g.add_op(OP_NORMAL_LPDF, {late, late, 1}, lp);
  g.ops[id].variant = 0x06;
  terms.push_back(lp);

  Graph ref = g;
  reduce_into_result(ref, terms);
  const std::vector<double> want = run_grad(std::move(ref), fills);

  const int n_inplace = make_inplace_updates(g, {});
  // The write feeding `second_in`'s reader must stay a copy; the writes
  // after it are still free to be destructive.
  expect("late reader blocks one write", n_inplace == L - 2);
  reduce_into_result(g, terms);
  const std::vector<double> got = run_grad_twice(std::move(g), fills);
  for (size_t i = 0; i < want.size() && i < got.size(); ++i)
    expect_close(("reader v" + std::to_string(i)).c_str(), got[i], want[i]);
}

// A root (read straight out of the arena, no consuming op) blocks it too.
static void test_bail_root() {
  const int L = 6;
  Fills fills;
  std::vector<int> terms;
  int n_vec = 0;
  Graph g = build_chain(L, fills, terms, &n_vec);
  int second_in = -1, seen = 0;
  for (const Op& op : g.ops)
    if (op.opcode == OP_SET_INDEX && ++seen == 2) second_in = op.in[0];
  const int n_inplace = make_inplace_updates(g, {second_in});
  expect("root blocks one write", n_inplace == L - 2);
}

// The hmm_example shape, and the reason a last-use rule alone is not
// enough: `acc` is filled element by element, read WHOLE, then refilled
// for the next time step. A legacy nested-replay backward replays the
// function on its input values, so those values must still be there
// during the reverse sweep -- a destructive refill would hand it the next
// step's numbers. Caught by the corpus A/B: 8 models, deviations up to
// 1.7e+05 relative.
//
// The reader here is softmax, which is still legacy. log_sum_exp used to
// play this role and no longer does: mixture.cpp made it native, so its
// partials live in scratch and it stopped blocking (see the test below).
static void test_bail_value_reading_consumer() {
  const int K = 2, T = 4;
  Graph g;
  Fills fills;
  const int mu = g.add_slot(1, true);
  const int acc0 = g.add_slot(K, false);
  fills.emplace_back(acc0, std::vector<double>((size_t)K, 0.0));
  auto cslot = [&](double v) {
    const int s = g.add_slot(1, false);
    fills.emplace_back(s, std::vector<double>{v});
    return s;
  };
  std::vector<int> terms;
  int acc = acc0;
  for (int t = 0; t < T; ++t) {
    for (int k = 0; k < K; ++k) {
      const int v = g.add_slot(1, false);
      // The spread WITHIN a step must vary across steps, or every step's
      // softmax is the same and replaying on the wrong values still gives
      // the right gradient -- the bug would hide.
      g.add_op(OP_MUL, {mu, cslot(0.3 + 0.9 * t * (k + 1))}, v);
      const int nxt = g.add_slot(K, false);
      g.add_op(OP_SET_INDEX, {acc, v}, nxt, {k});
      acc = nxt;
    }
    const int sm = g.add_slot(K, false);
    g.add_op(OP_SOFTMAX, {acc}, sm);  // legacy: replays on the values
    const int s = g.add_slot(1, false);
    g.add_op(OP_SUM_VEC, {sm}, s);
    terms.push_back(s);
  }
  Graph ref = g;
  reduce_into_result(ref, terms);
  const std::vector<double> want = run_grad(std::move(ref), fills);

  const int n_inplace = make_inplace_updates(g, {});
  // The exact boundary: each step's FIRST write must copy, which is what
  // preserves the previous step's buffer for that step's log_sum_exp
  // backward; the second write may then destroy that fresh copy, because
  // this step's log_sum_exp has not read it yet. One in-place per step.
  expect("one destructive write per step", n_inplace == T);
  int copies = 0;
  for (const Op& op : g.ops) copies += op.opcode == OP_SET_INDEX;
  expect("one preserved copy per step", copies == T);
  reduce_into_result(g, terms);
  const std::vector<double> got = run_grad_twice(std::move(g), fills);
  expect("hmm sizes", got.size() == want.size());
  for (size_t i = 0; i < want.size() && i < got.size(); ++i)
    expect_close(("hmm v" + std::to_string(i)).c_str(), got[i], want[i]);
}

// The radon shape end to end: write mu[n], read it straight back, and
// never touch mu again. Both the write and the read should disappear,
// leaving arithmetic the re-roll pass can vectorize.
static void test_store_to_load_forwarding() {
  const int L = 8;
  Fills fills;
  std::vector<int> terms;
  int n_vec = 0;
  Graph g = build_chain(L, fills, terms, &n_vec);

  Graph ref = g;
  reduce_into_result(ref, terms);
  const std::vector<double> want = run_grad(std::move(ref), fills);

  make_inplace_updates(g, {});
  const int removed = forward_stores_to_loads(g, {});
  // L read-backs plus the L writes they made redundant.
  expect("forwarded and swept 2L ops", removed == 2 * L);
  for (const Op& op : g.ops) {
    expect("no writes left",
           op.opcode != OP_SET_INDEX && op.opcode != OP_SET_INDEX_INPLACE);
    expect("no reads left", op.opcode != OP_INDEX);
  }
  reduce_into_result(g, terms);
  const std::vector<double> got = run_grad_twice(std::move(g), fills);
  expect("fwd sizes", got.size() == want.size());
  for (size_t i = 0; i < want.size() && i < got.size(); ++i)
    expect_close(("fwd v" + std::to_string(i)).c_str(), got[i], want[i]);
}

// When the vector IS read as a whole afterwards, its writes are live and
// must survive, even though the per-element read-backs still forward.
static void test_keeps_live_writes() {
  const int L = 6;
  Fills fills;
  std::vector<int> terms;
  int n_vec = 0;
  Graph g = build_chain(L, fills, terms, &n_vec);
  int final_vec = -1;
  for (const Op& op : g.ops)
    if (op.opcode == OP_SET_INDEX) final_vec = op.out;
  const int lse = g.add_slot(1, false);
  g.add_op(OP_LOG_SUM_EXP, {final_vec}, lse);  // whole-vector read
  terms.push_back(lse);

  Graph ref = g;
  reduce_into_result(ref, terms);
  const std::vector<double> want = run_grad(std::move(ref), fills);

  make_inplace_updates(g, {});
  forward_stores_to_loads(g, {});
  int writes = 0;
  for (const Op& op : g.ops)
    writes += op.opcode == OP_SET_INDEX || op.opcode == OP_SET_INDEX_INPLACE;
  expect("live writes kept", writes == L);
  reduce_into_result(g, terms);
  const std::vector<double> got = run_grad_twice(std::move(g), fills);
  for (size_t i = 0; i < want.size() && i < got.size(); ++i)
    expect_close(("live v" + std::to_string(i)).c_str(), got[i], want[i]);
}

// The same shape with log_sum_exp, which mixture.cpp made native: its
// partials are stashed in scratch, so nothing needs the buffer's values
// at reverse time and every write after the first may be destructive.
static void test_native_lse_allows_destructive() {
  const int K = 2, T = 4;
  Graph g;
  Fills fills;
  const int mu = g.add_slot(1, true);
  const int acc0 = g.add_slot(K, false);
  fills.emplace_back(acc0, std::vector<double>((size_t)K, 0.0));
  auto cslot = [&](double v) {
    const int s = g.add_slot(1, false);
    fills.emplace_back(s, std::vector<double>{v});
    return s;
  };
  std::vector<int> terms;
  int acc = acc0;
  for (int t = 0; t < T; ++t) {
    for (int k = 0; k < K; ++k) {
      const int v = g.add_slot(1, false);
      g.add_op(OP_MUL, {mu, cslot(0.3 + 0.9 * t * (k + 1))}, v);
      const int nxt = g.add_slot(K, false);
      g.add_op(OP_SET_INDEX, {acc, v}, nxt, {k});
      acc = nxt;
    }
    const int lse = g.add_slot(1, false);
    g.add_op(OP_LOG_SUM_EXP, {acc}, lse);
    terms.push_back(lse);
  }
  Graph ref = g;
  reduce_into_result(ref, terms);
  const std::vector<double> want = run_grad(std::move(ref), fills);

  const int n_inplace = make_inplace_updates(g, {});
  // Only the first write, whose source is the fill-backed acc0, copies.
  expect("native lse frees every later write", n_inplace == T * K - 1);
  reduce_into_result(g, terms);
  const std::vector<double> got = run_grad_twice(std::move(g), fills);
  expect("native lse sizes", got.size() == want.size());
  for (size_t i = 0; i < want.size() && i < got.size(); ++i)
    expect_close(("nlse v" + std::to_string(i)).c_str(), got[i], want[i]);
}

// The other half of the O(N^2) fix: the dead intermediate slots of a
// collapsed chain must stop occupying arena. bind_() sizes the arenas from
// slot lengths, not from which slots ops still reference, so without the
// dead-slot sweep the memory stayed quadratic even after the ops collapsed
// (2.58 GB on radon_county_intercept, unchanged by the pass until the sweep
// landed).
static void test_dead_slots_freed() {
  const int N = 64;
  Fills fills;
  std::vector<int> terms;
  int n_vec_slots = 0;
  Graph g = build_chain(N, fills, terms, &n_vec_slots);
  const int64_t before = [&] {
    int64_t s = 0;
    for (const auto& sl : g.slots) s += sl.len;
    return s;
  }();
  std::vector<int> roots = terms;
  make_inplace_updates(g, roots);
  forward_stores_to_loads(g, roots);
  int64_t after = 0;
  for (const auto& sl : g.slots) after += sl.len;
  // The chain alone held N vectors of length N. Afterwards the total must
  // be linear in N -- a generous constant, but nowhere near N*N/2.
  expect("chain arena was quadratic", before > (int64_t)N * N / 2);
  expect("dead slots freed (arena linear)", after < (int64_t)(8 * N + 64));
}

// STANLI_NO_INPLACE switches both entry points off; inplace.cpp reads the
// variable once per pass. harnesses/ab_corpus.py builds its whole A side out
// of this and the three sibling switches, so a rename here would leave that
// oracle comparing the optimized graph against itself, green, with no ctest
// to catch it.
static void test_env_disable() {
  const int L = 8;
  Fills fills;
  std::vector<int> terms;
  int n_vec = 0;
  Graph g = build_chain(L, fills, terms, &n_vec);
  const size_t before = g.ops.size();

  test_setenv("STANLI_NO_INPLACE", "1", 1);
  expect("disabled: no writes made in place", make_inplace_updates(g, {}) == 0);
  expect("disabled: no stores forwarded", forward_stores_to_loads(g, {}) == 0);
  expect("disabled: graph untouched", g.ops.size() == before);
  for (const Op& op : g.ops)
    expect("disabled: no destructive write", op.opcode != OP_SET_INDEX_INPLACE);
  test_unsetenv("STANLI_NO_INPLACE");

  // The same graph with the switch off: both passes do their work.
  expect("enabled: writes made in place", make_inplace_updates(g, {}) == L - 1);
  expect("enabled: stores forwarded", forward_stores_to_loads(g, {}) == 2 * L);
}

// normal_mixture_k's shape: per data point the vectorized lanes fill a
// K-element scratch vector whole, then reduce it. `partial` leaves the
// first element to an earlier write, which is what a real window store
// looks like; `elsewhere` gives the stored value a second reader.
static Graph build_full_store_chain(int L, int K, Fills& fills,
                                    std::vector<int>& terms, int* vec_out,
                                    bool partial = false,
                                    bool elsewhere = false) {
  Graph g;
  const int p = g.add_slot(K, true);
  const int c = g.add_slot(K, false);
  fills.push_back({c, std::vector<double>((size_t)K, 0.5)});
  const int vec = g.add_slot(K, false);
  *vec_out = vec;
  g.add_op(OP_ADD, {p, c}, vec);  // the scratch starts out defined
  for (int l = 0; l < L; ++l) {
    const int val = g.add_slot(partial ? K - 1 : K, false);
    if (partial) {
      const int w = g.add_slot(K - 1, false);
      g.add_op(OP_SLICE, {p}, w, {1});
      g.add_op(OP_EXPV, {w}, val);
    } else {
      g.add_op(OP_EXPV, {p}, val);
    }
    Op st;
    st.opcode = OP_SET_SLICE_INPLACE;
    st.n_in = 2;
    st.in[0] = vec;
    st.in[1] = val;
    st.out = vec;
    g.idata_pool.push_back({partial ? 1 : 0});
    st.idata = g.idata_pool.back().data();
    st.n_idata = 1;
    g.ops.push_back(st);
    const int lse = g.add_slot(1, false);
    g.add_op(OP_LOG_SUM_EXP, {vec}, lse);
    terms.push_back(lse);
    if (elsewhere) {
      const int s = g.add_slot(1, false);
      g.add_op(OP_LOG_SUM_EXP, {val}, s);
      terms.push_back(s);
    }
  }
  return g;
}

static int count_opcode(const Graph& g, uint16_t oc) {
  int n = 0;
  for (const Op& op : g.ops) n += op.opcode == oc;
  return n;
}

// The whole destination is overwritten every iteration, so every store is
// a rename: the reduction reads the fused value itself.
static void test_full_extent_stores_elided() {
  const int L = 6, K = 5;
  Fills fills;
  std::vector<int> terms;
  int vec = 0;
  Graph g = build_full_store_chain(L, K, fills, terms, &vec);

  Graph ref = g;
  reduce_into_result(ref, terms);
  const std::vector<double> want = run_grad(std::move(ref), fills);

  expect("elided every full store", elide_full_extent_stores(g, {}) == L);
  expect("no slice stores left", count_opcode(g, OP_SET_SLICE_INPLACE) == 0);
  reduce_into_result(g, terms);
  const std::vector<double> got = run_grad_twice(std::move(g), fills);
  expect("elide sizes", got.size() == want.size());
  for (size_t i = 0; i < want.size() && i < got.size(); ++i)
    expect(("elide bitwise v" + std::to_string(i)).c_str(), got[i] == want[i]);
}

// A window store leaves elements the value does not supply: it stays.
static void test_window_store_kept() {
  const int L = 4, K = 5;
  Fills fills;
  std::vector<int> terms;
  int vec = 0;
  Graph g = build_full_store_chain(L, K, fills, terms, &vec, true);
  expect("window store not elided", elide_full_extent_stores(g, {}) == 0);
  expect("window stores intact", count_opcode(g, OP_SET_SLICE_INPLACE) == L);
}

// A second reader of the stored value would receive the destination's
// adjoints in a different order, so the store stays.
static void test_shared_value_kept() {
  const int L = 4, K = 5;
  Fills fills;
  std::vector<int> terms;
  int vec = 0;
  Graph g = build_full_store_chain(L, K, fills, terms, &vec, false, true);
  expect("shared value not elided", elide_full_extent_stores(g, {}) == 0);
}

// The destination is read from outside the graph: it must still be written.
static void test_root_destination_kept() {
  const int L = 3, K = 5;
  Fills fills;
  std::vector<int> terms;
  int vec = 0;
  Graph g = build_full_store_chain(L, K, fills, terms, &vec);
  expect("root destination not elided",
         elide_full_extent_stores(g, {vec}) == 0);
}

static void test_elide_env_disable() {
  const int L = 3, K = 5;
  Fills fills;
  std::vector<int> terms;
  int vec = 0;
  Graph g = build_full_store_chain(L, K, fills, terms, &vec);
  test_setenv("STANLI_NO_INPLACE", "1", 1);
  expect("disabled: no stores elided", elide_full_extent_stores(g, {}) == 0);
  test_unsetenv("STANLI_NO_INPLACE");
}

// The copying form covers its whole fresh output too: its readers want the
// value it was handed, so the copy goes.
static void test_copying_full_store_elided() {
  const int K = 4;
  Fills fills;
  Graph g;
  const int p = g.add_slot(K, true);
  const int base = g.add_slot(K, false);
  fills.push_back({base, std::vector<double>((size_t)K, 0.25)});
  const int val = g.add_slot(K, false);
  g.add_op(OP_EXPV, {p}, val);
  const int copy = g.add_slot(K, false);
  g.add_op(OP_SET_SLICE, {base, val}, copy, {0});
  const int out = g.add_slot(1, false);
  g.add_op(OP_LOG_SUM_EXP, {copy}, out);
  g.result_slot = out;

  Graph ref = g;
  const std::vector<double> want = run_grad(std::move(ref), fills);
  expect("copying full store elided", elide_full_extent_stores(g, {}) == 1);
  expect("copy gone", count_opcode(g, OP_SET_SLICE) == 0);
  const std::vector<double> got = run_grad_twice(std::move(g), fills);
  for (size_t i = 0; i < want.size() && i < got.size(); ++i)
    expect(("copying elide bitwise v" + std::to_string(i)).c_str(),
           got[i] == want[i]);
}

int main() {
  test_chain_collapses();
  test_slice_chain(false);
  test_slice_chain(true);
  test_slice_rewrite_bails();
  test_output_reading_producer_keeps_copy();
  test_slice_rename_rebinds_existing_inplace_output();
  test_slice_invalidates_store_forwarding(false);
  test_slice_invalidates_store_forwarding(true);
  test_env_disable();
  test_native_lse_allows_destructive();
  test_store_to_load_forwarding();
  test_keeps_live_writes();
  test_bail_value_reading_consumer();
  test_bail_later_reader();
  test_bail_root();
  test_dead_slots_freed();
  test_full_extent_stores_elided();
  test_copying_full_store_elided();
  test_window_store_kept();
  test_shared_value_kept();
  test_root_destination_kept();
  test_elide_env_disable();
  if (failures) {
    std::printf("%d failures\n", failures);
    return 1;
  }
  std::printf("test_inplace OK\n");
  return 0;
}
