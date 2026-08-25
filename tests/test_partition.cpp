// Lane bucketing: lanes found by their delimiter rather than by a period, so
// they fuse whether or not they are adjacent, and refuse whenever a lane's
// values would move.
#include "env_helpers.hpp"
#include "graph_helpers.hpp"
#include <stanli/graph.hpp>
#include <stanli/optable.hpp>
#include <stanli/partition.hpp>

#ifndef _WIN32
#include <sys/resource.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
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
    std::printf("FAIL %-28s got %.17g want %.17g rel %.2e\n", what.c_str(), got,
                want, rel);
  }
}

using namespace stanli;
using stanli::testutil::Fills;
using stanli::testutil::reduce_into_result;

static double fill_at(int64_t i) { return 0.25 + 0.15 * (double)(i % 4); }
static std::vector<double> run_grad(Graph g, const Fills& fills) {
  return testutil::run_grad(std::move(g), fills, fill_at);
}

static int count_opcode(const Graph& g, uint16_t oc) {
  int n = 0;
  for (const Op& op : g.ops) n += op.opcode == oc;
  return n;
}

// The pre-pass graph is the reference: never an analytic value.
static void expect_same_grad(const std::string& what, Graph g, Fills fills,
                             std::vector<int> terms,
                             const std::vector<double>& want) {
  reduce_into_result(g, terms);
  const std::vector<double> got = run_grad(std::move(g), fills);
  if (got.size() != want.size()) {
    ++failures;
    std::printf("FAIL %s gradient size %zu != %zu\n", what.c_str(), got.size(),
                want.size());
    return;
  }
  for (size_t i = 0; i < want.size(); ++i)
    expect_close(what + " v" + std::to_string(i), got[i], want[i]);
}

static std::vector<double> reference(const Graph& g, const Fills& fills,
                                     const std::vector<int>& terms) {
  Graph ref = g;
  reduce_into_result(ref, terms);
  return run_grad(std::move(ref), fills);
}

// ---- shapes ----------------------------------------------------------------

// One lane per element of a parameter vector: {INDEX(v, l); NORMAL(y_l, idx,
// sigma)} with the lp a target term. The whole base is read in order, so the
// fused density reads it directly and no gather survives.
struct Lanes {
  Graph g;
  Fills fills;
  std::vector<int> terms;
  int base = -1, sigma = -1;
};

static Lanes build_index_lanes(int L) {
  Lanes b;
  b.base = b.g.add_slot(L, true);
  b.sigma = b.g.add_slot(1, true);
  for (int l = 0; l < L; ++l) {
    const int y = b.g.add_slot(1, false);
    b.fills.emplace_back(y, std::vector<double>{0.2 * l - 0.5});
    const int idx = b.g.add_slot(1, false);
    b.g.add_op(OP_INDEX, {b.base}, idx, {l});
    const int lp = b.g.add_slot(1, false);
    const int id = b.g.add_op(OP_NORMAL_LPDF, {y, idx, b.sigma}, lp);
    b.g.ops[(size_t)id].variant = 0x06;
    b.terms.push_back(lp);
  }
  return b;
}

static void test_contiguous_bucket() {
  Lanes b = build_index_lanes(8);
  const std::vector<double> want = reference(b.g, b.fills, b.terms);

  std::vector<int> tt = b.terms;
  Fills f2 = b.fills;
  const PartitionStats st = partition_lanes(b.g, f2, tt, {});
  expect("contiguous one group", st.groups == 1 && st.lanes == 8);
  expect("contiguous one op", b.g.ops.size() == 1);
  expect("contiguous reads the base", b.g.ops[0].in[1] == b.base);
  expect("contiguous one term", tt.size() == 1);
  expect_same_grad("contiguous", std::move(b.g), f2, tt, want);
}

// The same template, with an unrelated term computed between lanes 3 and 4.
// Lanes are found by their delimiters, so the intruder neither joins the
// bucket nor stops it, and it stays where it is.
static void test_interleaved_lanes() {
  const int L = 8;
  Lanes b;
  b.base = b.g.add_slot(L, true);
  b.sigma = b.g.add_slot(1, true);
  const int c = b.g.add_slot(1, false);
  b.fills.emplace_back(c, std::vector<double>{1.5});
  int foreign_term = -1;
  for (int l = 0; l < L; ++l) {
    if (l == 4) {
      const int e = b.g.add_slot(1, false);
      b.g.add_op(OP_EXP, {b.sigma}, e);
      const int lp = b.g.add_slot(1, false);
      const int id = b.g.add_op(OP_NORMAL_LPDF, {c, e, b.sigma}, lp);
      b.g.ops[(size_t)id].variant = 0x06;
      foreign_term = lp;
      b.terms.push_back(lp);
    }
    const int y = b.g.add_slot(1, false);
    b.fills.emplace_back(y, std::vector<double>{0.2 * l - 0.5});
    const int idx = b.g.add_slot(1, false);
    b.g.add_op(OP_INDEX, {b.base}, idx, {l});
    const int lp = b.g.add_slot(1, false);
    const int id = b.g.add_op(OP_NORMAL_LPDF, {y, idx, b.sigma}, lp);
    b.g.ops[(size_t)id].variant = 0x06;
    b.terms.push_back(lp);
  }
  const std::vector<double> want = reference(b.g, b.fills, b.terms);

  std::vector<int> tt = b.terms;
  Fills f2 = b.fills;
  const PartitionStats st = partition_lanes(b.g, f2, tt, {});
  expect("interleaved one group", st.groups == 1 && st.lanes == 8);
  expect("interleaved keeps the intruder", count_opcode(b.g, OP_EXP) == 1);
  expect("interleaved op count", b.g.ops.size() == 3);
  expect("interleaved two terms",
         tt.size() == 2 && tt[1] == foreign_term);
  expect_same_grad("interleaved", std::move(b.g), f2, tt, want);
}

// The data-condition shape: one bucket whose lanes read a shared base at
// scattered indices, out of order and with one index twice. The gather is
// emitted in lane order and its scatter-add is what makes the repeat come out
// like the scalar loop's two separate reads.
static void test_scattered_indices() {
  const int L = 8, N = 6;
  const int pick[L] = {4, 0, 3, 3, 5, 1, 2, 0};
  Graph g;
  Fills fills;
  const int base = g.add_slot(N, true);
  const int sigma = g.add_slot(1, true);
  std::vector<int> terms;
  for (int l = 0; l < L; ++l) {
    const int y = g.add_slot(1, false);
    fills.emplace_back(y, std::vector<double>{0.2 * l - 0.5});
    const int idx = g.add_slot(1, false);
    g.add_op(OP_INDEX, {base}, idx, {pick[l]});
    const int lp = g.add_slot(1, false);
    const int id = g.add_op(OP_NORMAL_LPDF, {y, idx, sigma}, lp);
    g.ops[(size_t)id].variant = 0x06;
    terms.push_back(lp);
  }
  const std::vector<double> want = reference(g, fills, terms);

  std::vector<int> tt = terms;
  Fills f2 = fills;
  const PartitionStats st = partition_lanes(g, f2, tt, {});
  expect("scattered one group", st.groups == 1 && st.lanes == L);
  expect("scattered one gather", count_opcode(g, OP_GATHER) == 1);
  expect("scattered lane order",
         g.ops[0].n_idata == L && g.ops[0].idata[0] == pick[0] &&
             g.ops[0].idata[3] == pick[3]);
  expect_same_grad("scattered", std::move(g), f2, tt, want);
}

// A print inside a lane. Hoisting eight of them into one would print once:
// the effect blocklist is what stops that, so the count is the assertion.
static void test_refuse_effectful() {
  const int L = 8;
  Lanes b;
  b.base = b.g.add_slot(L, true);
  b.sigma = b.g.add_slot(1, true);
  for (int l = 0; l < L; ++l) {
    const int y = b.g.add_slot(1, false);
    b.fills.emplace_back(y, std::vector<double>{0.2 * l - 0.5});
    const int idx = b.g.add_slot(1, false);
    b.g.add_op(OP_INDEX, {b.base}, idx, {l});
    const int dead = b.g.add_slot(1, false);
    b.g.add_op(OP_PRINT, {idx}, dead);
    const int lp = b.g.add_slot(1, false);
    const int id = b.g.add_op(OP_NORMAL_LPDF, {y, idx, b.sigma}, lp);
    b.g.ops[(size_t)id].variant = 0x06;
    b.terms.push_back(lp);
  }
  const size_t before = b.g.ops.size();

  std::vector<int> tt = b.terms;
  Fills f2 = b.fills;
  const PartitionStats st = partition_lanes(b.g, f2, tt, {});
  expect("effectful refuses", st.groups == 0);
  expect("effectful count unchanged", count_opcode(b.g, OP_PRINT) == L);
  expect("effectful graph unchanged",
         b.g.ops.size() == before && tt == b.terms);
}

// Every lane's intermediate is read after the lane, so the lane is the
// density alone and its mean is a different slot in every lane: nothing to
// gather, nothing to invariant, refuse.
static void test_refuse_escaping_intermediate() {
  const int L = 8;
  Graph g;
  Fills fills;
  const int p = g.add_slot(1, true);
  const int sigma = g.add_slot(1, true);
  std::vector<int> mid, terms;
  for (int l = 0; l < L; ++l) {
    const int c = g.add_slot(1, false);
    fills.emplace_back(c, std::vector<double>{0.3 + 0.1 * l});
    const int m = g.add_slot(1, false);
    g.add_op(OP_MUL, {p, c}, m);
    mid.push_back(m);
    const int y = g.add_slot(1, false);
    fills.emplace_back(y, std::vector<double>{0.2 * l - 0.5});
    const int lp = g.add_slot(1, false);
    const int id = g.add_op(OP_NORMAL_LPDF, {y, m, sigma}, lp);
    g.ops[(size_t)id].variant = 0x06;
    terms.push_back(lp);
  }
  int acc = mid[0];
  for (int l = 1; l < L; ++l) {
    const int s = g.add_slot(1, false);
    g.add_op(OP_ADD, {acc, mid[(size_t)l]}, s);
    acc = s;
  }
  terms.push_back(acc);
  const size_t before = g.ops.size();

  std::vector<int> tt = terms;
  Fills f2 = fills;
  const PartitionStats st = partition_lanes(g, f2, tt, {});
  expect("escaping refuses", st.groups == 0);
  expect("escaping graph unchanged", g.ops.size() == before && tt == terms);
}

// Eight terms that are the identical scalar, down to the data argument's
// slot. Fusing them would compute one lane's lp where the target owes eight.
static void test_refuse_identical_terms() {
  const int L = 8;
  Graph g;
  Fills fills;
  const int mu = g.add_slot(1, true);
  const int sigma = g.add_slot(1, true);
  const int y = g.add_slot(1, false);
  fills.emplace_back(y, std::vector<double>{0.75});
  std::vector<int> terms;
  for (int l = 0; l < L; ++l) {
    const int lp = g.add_slot(1, false);
    const int id = g.add_op(OP_NORMAL_LPDF, {y, mu, sigma}, lp);
    g.ops[(size_t)id].variant = 0x06;
    terms.push_back(lp);
  }
  const std::vector<double> want = reference(g, fills, terms);

  std::vector<int> tt = terms;
  Fills f2 = fills;
  const PartitionStats st = partition_lanes(g, f2, tt, {});
  expect("identical terms refuse", st.groups == 0 && tt.size() == (size_t)L);
  expect_same_grad("identical terms", std::move(g), f2, tt, want);
}

// The same shape with an integer outcome (multi_occupancy's detection terms):
// here the lanes' immediates concatenate, so the one fused op does compute
// eight lps and the fusion is the right answer.
static void test_identical_idata_terms() {
  const int L = 16;  // one op per lane: below this the margin declines it
  Graph g;
  Fills fills;
  const int theta = g.add_slot(1, true);
  std::vector<int> terms;
  for (int l = 0; l < L; ++l) {
    const int lp = g.add_slot(1, false);
    const int id = g.add_op(OP_BERNOULLI_LPMF, {theta}, lp, {1});
    g.ops[(size_t)id].variant = 0x81;
    terms.push_back(lp);
  }
  const std::vector<double> want = reference(g, fills, terms);

  std::vector<int> tt = terms;
  Fills f2 = fills;
  const PartitionStats st = partition_lanes(g, f2, tt, {});
  expect("idata terms fuse", st.groups == 1 && st.lanes == L);
  expect("idata terms concatenate",
         g.ops.size() == 1 && g.ops[0].n_idata == L && tt.size() == 1);
  expect_same_grad("idata terms", std::move(g), f2, tt, want);
}

static void test_kill_switch() {
  Lanes b = build_index_lanes(8);
  const size_t before = b.g.ops.size();
  std::vector<int> tt = b.terms;
  Fills f2 = b.fills;
  test_setenv("STANLI_NO_PARTITION", "1", 1);
  const PartitionStats st = partition_lanes(b.g, f2, tt, {});
  test_unsetenv("STANLI_NO_PARTITION");
  expect("kill switch", st.groups == 0 && b.g.ops.size() == before &&
                            tt == b.terms);
}

// state_space_stochastic_level_stochastic_seasonal, in miniature: a window-sum
// loop followed by a random-walk loop. Re-roll fuses neither -- the first
// leaves its period scan mis-phased for the second -- and lane bounds are
// computed here rather than discovered, so both fall out.
static void test_state_space_shape() {
  const int N = 24, WIN = 11;
  Graph g;
  Fills fills;
  const int seasonal = g.add_slot(N, true);
  const int mu = g.add_slot(N, true);
  const int sig = g.add_slot(2, true);
  std::vector<int> terms;
  for (int t = WIN; t < N; ++t) {
    const int y = g.add_slot(1, false);
    g.add_op(OP_INDEX, {seasonal}, y, {t});
    const int win = g.add_slot(WIN, false);
    g.add_op(OP_SLICE, {seasonal}, win, {t - WIN});
    const int s = g.add_slot(1, false);
    g.add_op(OP_SUM_VEC, {win}, s);
    const int neg = g.add_slot(1, false);
    g.add_op(OP_NEG, {s}, neg);
    const int sg = g.add_slot(1, false);
    g.add_op(OP_INDEX, {sig}, sg, {0});
    const int lp = g.add_slot(1, false);
    const int id = g.add_op(OP_NORMAL_LPDF, {y, neg, sg}, lp);
    g.ops[(size_t)id].variant = 0x07;
    terms.push_back(lp);
  }
  for (int t = 1; t < N; ++t) {
    const int y = g.add_slot(1, false);
    g.add_op(OP_INDEX, {mu}, y, {t});
    const int prev = g.add_slot(1, false);
    g.add_op(OP_INDEX, {mu}, prev, {t - 1});
    const int sg = g.add_slot(1, false);
    g.add_op(OP_INDEX, {sig}, sg, {1});
    const int lp = g.add_slot(1, false);
    const int id = g.add_op(OP_NORMAL_LPDF, {y, prev, sg}, lp);
    g.ops[(size_t)id].variant = 0x07;
    terms.push_back(lp);
  }
  const std::vector<double> want = reference(g, fills, terms);

  std::vector<int> tt = terms;
  Fills f2 = fills;
  const PartitionStats st = partition_lanes(g, f2, tt, {});
  expect("state_space two groups", st.groups == 2);
  expect("state_space lanes", st.lanes == (N - WIN) + (N - 1));
  // seasonal: SLICE, GATHER, SUM_ROWS, NEG, INDEX, NORMAL.
  // mu:       SLICE, SLICE, INDEX, NORMAL.
  expect("state_space op count", g.ops.size() == 10);
  expect("state_space window sum", count_opcode(g, OP_SUM_ROWS) == 1);
  expect("state_space one gather", count_opcode(g, OP_GATHER) == 1);
  expect("state_space three slices", count_opcode(g, OP_SLICE) == 3);
  expect("state_space two densities",
         count_opcode(g, OP_NORMAL_LPDF) == 2 && tt.size() == 2);
  expect_same_grad("state_space", std::move(g), f2, tt, want);
}

// Four lanes reading a 64-wide window each: 12 op dispatches saved against
// two 256-element gathers. The cost model exists to say no to this.
static void test_cost_declines() {
  const int L = 4, W = 64;
  Graph g;
  Fills fills;
  const int base = g.add_slot(L + W, true);
  const int sigma = g.add_slot(1, true);
  std::vector<int> terms;
  for (int l = 0; l < L; ++l) {
    const int y = g.add_slot(1, false);
    fills.emplace_back(y, std::vector<double>{0.2 * l - 0.5});
    const int win = g.add_slot(W, false);
    g.add_op(OP_SLICE, {base}, win, {l});
    const int s = g.add_slot(1, false);
    g.add_op(OP_SUM_VEC, {win}, s);
    const int lp = g.add_slot(1, false);
    const int id = g.add_op(OP_NORMAL_LPDF, {y, s, sigma}, lp);
    g.ops[(size_t)id].variant = 0x06;
    terms.push_back(lp);
  }
  const size_t before = g.ops.size();

  std::vector<int> tt = terms;
  Fills f2 = fills;
  const PartitionStats st = partition_lanes(g, f2, tt, {});
  expect("wide lanes decline", st.groups == 0 && st.declined == 1);
  expect("declined graph unchanged", g.ops.size() == before && tt == terms);
}

// ---- cost -----------------------------------------------------------------

static double peak_rss_mb() {
#ifdef _WIN32
  return -1.0;
#else
  struct rusage ru;
  if (getrusage(RUSAGE_SELF, &ru) != 0) return -1.0;
#ifdef __APPLE__
  return (double)ru.ru_maxrss / (1024.0 * 1024.0);
#else
  return (double)ru.ru_maxrss / 1024.0;
#endif
#endif
}

struct Cost {
  double sec;
  int64_t segment;
  int64_t list;
};

static Cost lane_cost(int L) {
  Lanes b = build_index_lanes(L);
  std::vector<int> tt = b.terms;
  const auto t0 = std::chrono::steady_clock::now();
  const PartitionStats st = partition_lanes(b.g, b.fills, tt, {});
  const double sec =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
          .count();
  expect("scaling fused", st.groups == 1 && st.lanes == L);
  expect("scaling fixed graph", b.g.ops.size() == 1 && tt.size() == 1);
  return {sec, st.segment_steps, st.list_steps};
}

// A deterministic integer counter asserted as a ratio between two sizes. Two
// wall-clock formulations of the same property failed CI on shared runners
// while the pass was fine.
static void test_scaling() {
  const Cost small = lane_cost(8000);
  const Cost big = lane_cost(16000);
  const double rss = peak_rss_mb();
  std::printf(
      "  partition: n=8000 %.3f s / %lld segment / %lld list,"
      " n=16000 %.3f s / %lld segment / %lld list, peak RSS %.0f MB\n",
      small.sec, (long long)small.segment, (long long)small.list, big.sec,
      (long long)big.segment, (long long)big.list, rss);
  expect("segmentation is exactly linear",
         big.segment == 2 * small.segment);
  expect("list reads stay near-linear", big.list < 3 * small.list);
  if (rss > 0.0) expect("partition space stays linear", rss < 1024.0);
}

int main() {
  {  // kernels register through the first Executor
    Graph g;
    const int a = g.add_slot(1, true), o = g.add_slot(1, false);
    g.add_op(OP_EXP, {a}, o);
    g.result_slot = o;
    Executor warm(std::move(g));
    (void)warm.n_params();
  }
  test_contiguous_bucket();
  test_interleaved_lanes();
  test_scattered_indices();
  test_refuse_effectful();
  test_refuse_escaping_intermediate();
  test_refuse_identical_terms();
  test_identical_idata_terms();
  test_kill_switch();
  test_state_space_shape();
  test_cost_declines();
  test_scaling();
  if (failures == 0) std::printf("test_partition OK\n");
  return failures == 0 ? 0 : 1;
}
