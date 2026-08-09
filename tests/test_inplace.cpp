// In-place functional updates: a chain of OP_SET_INDEX writes into the
// same vector must collapse onto one buffer (O(N) instead of O(N^2)) with
// values and gradients unchanged.
#include "graph_helpers.hpp"
#include <stanli/graph.hpp>
#include <stanli/inplace.hpp>
#include <stanli/optable.hpp>

#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

static int failures = 0;
static void expect(const char* what, bool ok) {
  if (!ok) { ++failures; std::printf("FAIL %s\n", what); }
}
static void expect_close(const char* what, double got, double want) {
  const double rel =
      std::abs(got - want) / std::max(std::abs(want), 1e-300);
  if (!(rel < 1e-12)) {
    ++failures;
    std::printf("FAIL %-26s got %.17g want %.17g rel %.2e\n", what, got,
                want, rel);
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
    expect_close(("repeat v" + std::to_string(i)).c_str(), second[i],
                 first[i]);
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
    expect("no writes left", op.opcode != OP_SET_INDEX &&
                                 op.opcode != OP_SET_INDEX_INPLACE);
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

int main() {
  test_chain_collapses();
  test_native_lse_allows_destructive();
  test_store_to_load_forwarding();
  test_keeps_live_writes();
  test_bail_value_reading_consumer();
  test_bail_later_reader();
  test_bail_root();
  test_dead_slots_freed();
  if (failures) { std::printf("%d failures\n", failures); return 1; }
  std::printf("test_inplace OK\n");
  return 0;
}
