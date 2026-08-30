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
#include <memory>
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

static Lanes build_index_lanes(int L, uint16_t density = OP_NORMAL_LPDF) {
  Lanes b;
  b.base = b.g.add_slot(L, true);
  b.sigma = b.g.add_slot(1, true);
  for (int l = 0; l < L; ++l) {
    const int y = b.g.add_slot(1, false);
    b.fills.emplace_back(y, std::vector<double>{0.2 * l - 0.5});
    const int idx = b.g.add_slot(1, false);
    b.g.add_op(OP_INDEX, {b.base}, idx, {l});
    const int lp = b.g.add_slot(1, false);
    const int id = b.g.add_op(density, {y, idx, b.sigma}, lp);
    b.g.ops[(size_t)id].variant = 0x06;
    b.terms.push_back(lp);
  }
  return b;
}

// A scalar-list density that the old hand-written trait omitted. Direct
// target terms use the summed vector kernel: bit 6 must stay clear while the
// lane lps and their gradients collapse into one logistic operation.
static void test_logistic_direct_terms() {
  const int L = 8;
  Lanes b = build_index_lanes(L, OP_LOGISTIC_LPDF);
  const std::vector<double> want = reference(b.g, b.fills, b.terms);

  std::vector<int> tt = b.terms;
  Fills f2 = b.fills;
  const PartitionStats st = partition_lanes(b.g, f2, tt, {});
  expect("logistic terms one group", st.groups == 1 && st.lanes == L);
  expect("logistic terms summed", b.g.ops.size() == 1 && tt.size() == 1 &&
                                      b.g.ops[0].opcode == OP_LOGISTIC_LPDF &&
                                      (b.g.ops[0].variant & 0x40u) == 0);
  expect("logistic terms vector input",
         b.g.slots[(size_t)b.g.ops[0].in[0]].len == L &&
             b.g.slots[(size_t)b.g.ops[0].in[1]].len == L);
  expect_same_grad("logistic terms", std::move(b.g), f2, tt, want);
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
  expect("interleaved two terms", tt.size() == 2 && tt[1] == foreign_term);
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
  expect("scattered lane order", g.ops[0].n_idata == L &&
                                     g.ops[0].idata[0] == pick[0] &&
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
// L lps and the fusion is the right answer.
static void test_idata_terms() {
  const int L = 16;  // one op per lane: below this the margin declines it
  Graph g;
  Fills fills;
  const int theta = g.add_slot(1, true);
  std::vector<int> terms;
  for (int l = 0; l < L; ++l) {
    const int lp = g.add_slot(1, false);
    const int id = g.add_op(OP_POISSON_LPMF, {theta}, lp, {l});
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

// The lanes above with the outcome held constant: byte-identical lanes are
// one op once CSE runs, so fusing them evaluates the density L times where
// the scalar graph evaluates it once.
static void test_duplicate_lanes_decline() {
  const int L = 16;
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
  const size_t before = g.ops.size();

  std::vector<int> tt = terms;
  Fills f2 = fills;
  const PartitionStats st = partition_lanes(g, f2, tt, {});
  expect("duplicate lanes decline", st.groups == 0 && st.declined == 1);
  expect("duplicate graph unchanged", g.ops.size() == before && tt == terms);
}

// The capture-recapture shape: a density carrying two integer groups, one
// varying per lane and one not. Fusing concatenates group by group, in the
// [len, vals...] layout the kernels unpack.
static void test_binomial_idata_groups() {
  const int L = 8;
  Graph g;
  Fills fills;
  const int theta = g.add_slot(1, true);
  const int prior = g.add_slot(1, true);
  std::vector<int> terms;
  for (int l = 0; l < L; ++l) {
    const int lp = g.add_slot(1, false);
    const int id = g.add_op(OP_BINOMIAL_LPMF, {theta}, lp, {-1, l, -1, L});
    g.ops[(size_t)id].variant = 0x01;
    const int t = g.add_slot(1, false);
    g.add_op(OP_ADD, {prior, lp}, t);
    terms.push_back(t);
  }
  const std::vector<double> want = reference(g, fills, terms);

  std::vector<int> tt = terms;
  Fills f2 = fills;
  const PartitionStats st = partition_lanes(g, f2, tt, {});
  expect("groups one group", st.groups == 1 && st.lanes == L);
  expect("groups op count", g.ops.size() == 3 && tt.size() == 1);
  bool layout = g.ops[0].n_idata == 2 * (L + 1);
  if (layout) {
    layout = g.ops[0].idata[0] == L && g.ops[0].idata[L + 1] == L;
    for (int l = 0; l < L; ++l)
      layout = layout && g.ops[0].idata[1 + l] == l &&
               g.ops[0].idata[L + 2 + l] == L;
  }
  expect("groups concatenate per group", layout);
  expect_same_grad("groups", std::move(g), f2, tt, want);
}

// A width-5 outcome group read through a strided window: the lanes' real
// arguments become one gather and the lps come back per lane through
// OP_SUM_ROWS, because the lane consumes its density rather than ending at it.
static void test_width_w_outcome_group() {
  const int L = 24, W = 5;
  Graph g;
  Fills fills;
  const int base = g.add_slot(L * W, true);
  const int prior = g.add_slot(1, true);
  std::vector<int> terms;
  for (int l = 0; l < L; ++l) {
    const int win = g.add_slot(W, false);
    g.add_op(OP_SLICE_STRIDED, {base}, win, {l, L});
    const int lp = g.add_slot(1, false);
    const int id = g.add_op(OP_BERNOULLI_LOGIT_LPMF, {win}, lp,
                            {l % 2, 1, 0, (l + 1) % 2, 1});
    g.ops[(size_t)id].variant = 0x01;
    const int t = g.add_slot(1, false);
    g.add_op(OP_ADD, {prior, lp}, t);
    terms.push_back(t);
  }
  const std::vector<double> want = reference(g, fills, terms);

  std::vector<int> tt = terms;
  Fills f2 = fills;
  const PartitionStats st = partition_lanes(g, f2, tt, {});
  expect("width-w one group", st.groups == 1 && st.lanes == L);
  expect("width-w op count", g.ops.size() == 5 && tt.size() == 1);
  expect("width-w one gather",
         count_opcode(g, OP_GATHER) == 1 && g.ops[0].n_idata == L * W);
  expect("width-w row sum", count_opcode(g, OP_SUM_ROWS) == 1);
  expect("width-w idata concatenates", g.ops[1].n_idata == L * W);
  expect_same_grad("width-w", std::move(g), f2, tt, want);
}

// Survey_model's lane: a width-5 binomial group over one shared probability,
// added to a per-lane constant and stored. The binomial's elementwise form
// costs what its summed one does per element, so widening the group is what
// the cost model has to say yes to.
static void test_binomial_elt_fusion() {
  const int L = 16, W = 5, N = 40;
  Graph g;
  Fills fills;
  const int theta = g.add_slot(1, true);
  const int vec = g.add_slot(N, false);
  std::vector<double> vals((size_t)N);
  for (int i = 0; i < N; ++i) vals[(size_t)i] = -0.5 + 0.05 * i;
  fills.emplace_back(vec, vals);
  for (int l = 0; l < L; ++l) {
    const int lp = g.add_slot(1, false);
    const int id = g.add_op(OP_BINOMIAL_LPMF, {theta}, lp,
                            {W, l % 3, (l + 1) % 3, l % 2, 1, 0, -1, 4});
    g.ops[(size_t)id].variant = 0x01;
    const int c = g.add_slot(1, false);
    fills.emplace_back(c, std::vector<double>{0.3 + 0.1 * l});
    const int s = g.add_slot(1, false);
    g.add_op(OP_ADD, {c, lp}, s);
    g.add_op(OP_SET_INDEX_INPLACE, {vec, s}, vec, {l});
  }
  const int lse = g.add_slot(1, false);
  g.add_op(OP_LOG_SUM_EXP, {vec}, lse);
  const std::vector<int> terms{lse};
  const std::vector<double> want = reference(g, fills, terms);

  std::vector<int> tt = terms;
  Fills f2 = fills;
  const PartitionStats st = partition_lanes(g, f2, tt, {});
  expect("binomial elt one group", st.groups == 1 && st.lanes == L);
  // BINOMIAL, SUM_ROWS, ADD, SET_SLICE_INPLACE, LOG_SUM_EXP.
  expect("binomial elt op count", g.ops.size() == 5 && tt == terms);
  expect("binomial elt widens", count_opcode(g, OP_SUM_ROWS) == 1 &&
                                    count_opcode(g, OP_BINOMIAL_LPMF) == 1);
  expect("binomial elt groups concatenate",
         g.ops[0].n_idata == 2 * (L * W + 1) && g.ops[0].idata[0] == L * W &&
             g.ops[0].idata[L * W + 1] == L * W);
  expect_same_grad("binomial elt", std::move(g), f2, tt, want);
}

// Survey_model's accumulator: lanes delimited by an element store rather
// than a term, at indices that march by one. The run becomes a single slice
// store, and the reduction that reads the whole vector afterwards is what
// the values have to be right for.
static void test_store_delimited_lanes() {
  const int L = 16, N = 40, START = 4;
  Graph g;
  Fills fills;
  const int p = g.add_slot(1, true);
  const int vec = g.add_slot(N, false);
  std::vector<double> vals((size_t)N);
  for (int i = 0; i < N; ++i) vals[(size_t)i] = -0.5 + 0.05 * i;
  fills.emplace_back(vec, vals);
  for (int l = 0; l < L; ++l) {
    const int c = g.add_slot(1, false);
    fills.emplace_back(c, std::vector<double>{0.3 + 0.1 * l});
    const int m = g.add_slot(1, false);
    g.add_op(OP_MUL, {p, c}, m);
    g.add_op(OP_SET_INDEX_INPLACE, {vec, m}, vec, {START + l});
  }
  const int lse = g.add_slot(1, false);
  g.add_op(OP_LOG_SUM_EXP, {vec}, lse);
  const std::vector<int> terms{lse};
  const std::vector<double> want = reference(g, fills, terms);

  std::vector<int> tt = terms;
  Fills f2 = fills;
  const PartitionStats st = partition_lanes(g, f2, tt, {});
  expect("store one group", st.groups == 1 && st.lanes == L);
  expect("store op count", g.ops.size() == 3 && tt == terms);
  expect("store one slice store",
         count_opcode(g, OP_SET_SLICE_INPLACE) == 1 &&
             count_opcode(g, OP_SET_INDEX_INPLACE) == 0);
  expect("store start", g.ops[1].n_idata == 1 && g.ops[1].idata[0] == START);
  expect_same_grad("store", std::move(g), f2, tt, want);
}

// multi_occupancy's branch structure: two templates chosen by a data
// condition, alternating, the second a prefix-extension of the first. The
// fingerprint separates them and each bucket is rewritten at its own first
// lane.
static void test_two_templates_interleaved() {
  const int L = 12;
  Graph g;
  Fills fills;
  const int base = g.add_slot(2 * L, true);
  const int sigma = g.add_slot(1, true);
  std::vector<int> terms;
  for (int l = 0; l < 2 * L; ++l) {
    const int idx = g.add_slot(1, false);
    g.add_op(OP_INDEX, {base}, idx, {l});
    const int y = g.add_slot(1, false);
    fills.emplace_back(y, std::vector<double>{0.2 * l - 0.5});
    const int lp = g.add_slot(1, false);
    const int id = g.add_op(OP_NORMAL_LPDF, {y, idx, sigma}, lp);
    g.ops[(size_t)id].variant = 0x06;
    if (l % 2 == 0) {
      terms.push_back(lp);
      continue;
    }
    const int mixed = g.add_slot(1, false);
    g.add_op(OP_LSE2, {lp, sigma}, mixed);
    terms.push_back(mixed);
  }
  const std::vector<double> want = reference(g, fills, terms);

  std::vector<int> tt = terms;
  Fills f2 = fills;
  const PartitionStats st = partition_lanes(g, f2, tt, {});
  expect("two templates two groups", st.groups == 2 && st.lanes == 2 * L);
  expect("two templates op count", g.ops.size() == 6 && tt.size() == 2);
  expect("two templates two gathers", count_opcode(g, OP_GATHER) == 2);
  expect_same_grad("two templates", std::move(g), f2, tt, want);
}

// A write to the gathered base between lanes 7 and 8. The lanes on either
// side read different contents, so the bucket splits at the writer instead of
// declining -- and the split is what the gradient check is really testing.
static void test_mid_bucket_writer_split() {
  const int L = 16, N = 20;
  Graph g;
  Fills fills;
  const int p = g.add_slot(N, true);
  const int sigma = g.add_slot(1, true);
  const int base = g.add_slot(N, false);
  g.add_op(OP_EXPV, {p}, base);
  std::vector<int> terms;
  for (int l = 0; l < L; ++l) {
    if (l == 8) g.add_op(OP_SET_INDEX_INPLACE, {base, sigma}, base, {3});
    const int idx = g.add_slot(1, false);
    g.add_op(OP_INDEX, {base}, idx, {(3 * l) % N});
    const int y = g.add_slot(1, false);
    fills.emplace_back(y, std::vector<double>{0.2 * l - 0.5});
    const int lp = g.add_slot(1, false);
    const int id = g.add_op(OP_NORMAL_LPDF, {y, idx, sigma}, lp);
    g.ops[(size_t)id].variant = 0x06;
    terms.push_back(lp);
  }
  const std::vector<double> want = reference(g, fills, terms);

  std::vector<int> tt = terms;
  Fills f2 = fills;
  const PartitionStats st = partition_lanes(g, f2, tt, {});
  expect("split two groups", st.groups == 2 && st.lanes == L);
  expect("split keeps the writer", count_opcode(g, OP_SET_INDEX_INPLACE) == 1);
  expect("split two gathers", count_opcode(g, OP_GATHER) == 2);
  expect_same_grad("split", std::move(g), f2, tt, want);
}

// The reader half of the same hazard: a second bucket's lanes read the vector
// the first bucket's lanes store into. Nothing may read a vector while its
// run is in flight, so the store bucket refuses and the readers still see
// every element written.
static void test_cross_bucket_read_after_write() {
  const int L = 8, N = 24;
  Graph g;
  Fills fills;
  const int p = g.add_slot(1, true);
  const int vec = g.add_slot(N, false);
  std::vector<double> vals((size_t)N);
  for (int i = 0; i < N; ++i) vals[(size_t)i] = 0.1 + 0.05 * i;
  fills.emplace_back(vec, vals);
  std::vector<int> terms;
  for (int l = 0; l < L; ++l) {
    const int c = g.add_slot(1, false);
    fills.emplace_back(c, std::vector<double>{0.3 + 0.1 * l});
    const int m = g.add_slot(1, false);
    g.add_op(OP_MUL, {p, c}, m);
    g.add_op(OP_SET_INDEX_INPLACE, {vec, m}, vec, {l});
    const int r = g.add_slot(1, false);
    g.add_op(OP_INDEX, {vec}, r, {l});
    const int lp = g.add_slot(1, false);
    const int id = g.add_op(OP_NORMAL_LPDF, {r, p, p}, lp);
    g.ops[(size_t)id].variant = 0x07;
    terms.push_back(lp);
  }
  const std::vector<double> want = reference(g, fills, terms);

  std::vector<int> tt = terms;
  Fills f2 = fills;
  const PartitionStats st = partition_lanes(g, f2, tt, {});
  expect("cross-bucket store refuses",
         st.groups == 0 && count_opcode(g, OP_SET_INDEX_INPLACE) == L &&
             count_opcode(g, OP_SET_SLICE_INPLACE) == 0);
  expect_same_grad("cross-bucket", std::move(g), f2, tt, want);
}

static void test_kill_switch() {
  Lanes b = build_index_lanes(8);
  const size_t before = b.g.ops.size();
  std::vector<int> tt = b.terms;
  Fills f2 = b.fills;
  test_setenv("STANLI_NO_PARTITION", "1", 1);
  const PartitionStats st = partition_lanes(b.g, f2, tt, {});
  test_unsetenv("STANLI_NO_PARTITION");
  expect("kill switch",
         st.groups == 0 && b.g.ops.size() == before && tt == b.terms);
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

// gpcm_latent_reg_irt's per-response chain: theta[person] * alpha[item] minus
// that item's threshold segment, run through a leading zero, a cumsum, a
// softmax and a categorical. Items interleave, so the lanes are laid down
// person-major and the item is a refinement of the bucket.
struct Irt {
  Graph g;
  Fills fills;
  std::vector<int> terms;
  int theta = -1, alpha = -1, beta = -1;
};

// item_m[i] thresholds for item i, laid end to end in one beta vector.
static Irt build_irt(const std::vector<int>& item_m,
                     const std::vector<int>& person, bool split_sub) {
  Irt b;
  const int n_items = (int)item_m.size();
  int total = 0;
  for (int m : item_m) total += m;
  b.theta = b.g.add_slot((int64_t)person.size(), true);
  b.alpha = b.g.add_slot(n_items, true);
  b.beta = b.g.add_slot(split_sub ? n_items : total, true);
  const int shared = split_sub ? b.g.add_slot(item_m[0], true) : -1;
  int pos = 0;
  std::vector<int> start((size_t)n_items);
  for (int i = 0; i < n_items; ++i) {
    start[(size_t)i] = pos;
    pos += item_m[(size_t)i];
  }
  for (size_t l = 0; l < person.size(); ++l) {
    for (int i = 0; i < n_items; ++i) {
      const int m = item_m[(size_t)i];
      const int th = b.g.add_slot(1, false);
      b.g.add_op(OP_INDEX, {b.theta}, th, {person[l]});
      const int al = b.g.add_slot(1, false);
      b.g.add_op(OP_INDEX, {b.alpha}, al, {i});
      const int mul = b.g.add_slot(1, false);
      b.g.add_op(OP_MUL, {th, al}, mul);
      int diff = -1;
      if (split_sub) {
        const int bi = b.g.add_slot(1, false);
        b.g.add_op(OP_INDEX, {b.beta}, bi, {i});
        const int d1 = b.g.add_slot(1, false);
        b.g.add_op(OP_SUB, {mul, bi}, d1);
        diff = b.g.add_slot(m, false);
        b.g.add_op(OP_SUB, {d1, shared}, diff);
      } else {
        const int seg = b.g.add_slot(m, false);
        b.g.add_op(OP_SLICE, {b.beta}, seg, {start[(size_t)i]});
        diff = b.g.add_slot(m, false);
        b.g.add_op(OP_SUB, {mul, seg}, diff);
      }
      const int zero = b.g.add_slot(1, false);
      b.fills.emplace_back(zero, std::vector<double>{0.0});
      const int cat_in = b.g.add_slot(m + 1, false);
      b.g.add_op(OP_CONCAT2, {zero, diff}, cat_in);
      const int cs = b.g.add_slot(m + 1, false);
      b.g.add_op(OP_CUMSUM, {cat_in}, cs);
      const int probs = b.g.add_slot(m + 1, false);
      b.g.add_op(OP_SOFTMAX, {cs}, probs);
      const int y = b.g.add_slot(1, false);
      b.fills.emplace_back(
          y, std::vector<double>{(double)(1 + ((int)l + i) % (m + 1))});
      const int lp = b.g.add_slot(1, false);
      b.g.add_op(OP_CATEGORICAL, {y, probs}, lp);
      auto spec = std::make_shared<CategoricalSpec>();
      spec->scalar_outcome = true;
      spec->arg_autodiff = true;
      b.g.ops.back().udata = spec.get();
      b.g.udata_pool.push_back(std::move(spec));
      b.terms.push_back(lp);
    }
  }
  return b;
}

static void test_irt_glm_synthesis() {
  const std::vector<int> item_m{1, 2};
  const std::vector<int> person{0, 3, 1, 1, 4, 2, 5, 0};
  Irt b = build_irt(item_m, person, false);
  const std::vector<double> want = reference(b.g, b.fills, b.terms);

  std::vector<int> tt = b.terms;
  Fills f2 = b.fills;
  const PartitionStats st = partition_lanes(b.g, f2, tt, {});
  expect("irt two items", st.groups == 2 &&
                              st.lanes == (int)(2 * person.size()) &&
                              tt.size() == 2);
  expect("irt one glm per item",
         count_opcode(b.g, OP_CATEGORICAL_LOGIT_GLM_LPMF) == 2 &&
             count_opcode(b.g, OP_CATEGORICAL) == 0);
  expect("irt intercepts negate", count_opcode(b.g, OP_NEG) == 2 &&
                                      count_opcode(b.g, OP_CUMSUM) == 2 &&
                                      count_opcode(b.g, OP_SOFTMAX) == 0);
  // Person indices in lane order, repeats and all; y then rows then cols.
  bool layout = false;
  for (const Op& op : b.g.ops) {
    if (op.opcode != OP_GATHER) continue;
    layout = op.n_idata == (int64_t)person.size() && op.idata[0] == person[0] &&
             op.idata[3] == person[3];
  }
  expect("irt gathers in lane order", layout);
  for (const Op& op : b.g.ops) {
    if (op.opcode != OP_CATEGORICAL_LOGIT_GLM_LPMF) continue;
    const int64_t rows = op.idata[op.n_idata - 2];
    expect("irt glm idata", rows == (int64_t)person.size() &&
                                op.idata[op.n_idata - 1] == 1 &&
                                op.n_idata == rows + 2);
  }
  expect_same_grad("irt", std::move(b.g), f2, tt, want);
}

// grsm_latent_reg_irt's chain: the item's own difficulty comes off first as a
// scalar, then a threshold vector every item shares. The segment the
// intercepts cumsum is their sum.
static void test_irt_split_subtrahend() {
  const std::vector<int> item_m{3, 3};
  const std::vector<int> person{2, 0, 1, 4, 4, 3};
  Irt b = build_irt(item_m, person, true);
  const std::vector<double> want = reference(b.g, b.fills, b.terms);

  std::vector<int> tt = b.terms;
  Fills f2 = b.fills;
  const PartitionStats st = partition_lanes(b.g, f2, tt, {});
  expect("irt split two items",
         st.groups == 2 && st.lanes == (int)(2 * person.size()));
  expect("irt split one glm per item",
         count_opcode(b.g, OP_CATEGORICAL_LOGIT_GLM_LPMF) == 2 &&
             count_opcode(b.g, OP_ADD) == 2);
  expect_same_grad("irt split", std::move(b.g), f2, tt, want);
}

// The same chain without the leading zero, and with a leading constant that
// is not zero. Either way the intercepts stop being -cumsum(segment) behind a
// reference category, so the substitution does not hold.
static void test_irt_refuse_near_miss(double lead, bool concat) {
  const std::vector<int> person{0, 3, 1, 1, 4, 2, 5, 0};
  const int M = 2;
  Graph g;
  Fills fills;
  const int theta = g.add_slot(6, true);
  const int alpha = g.add_slot(1, true);
  const int beta = g.add_slot(M, true);
  std::vector<int> terms;
  for (size_t l = 0; l < person.size(); ++l) {
    const int th = g.add_slot(1, false);
    g.add_op(OP_INDEX, {theta}, th, {person[l]});
    const int al = g.add_slot(1, false);
    g.add_op(OP_INDEX, {alpha}, al, {0});
    const int mul = g.add_slot(1, false);
    g.add_op(OP_MUL, {th, al}, mul);
    const int seg = g.add_slot(M, false);
    g.add_op(OP_SLICE, {beta}, seg, {0});
    const int diff = g.add_slot(M, false);
    g.add_op(OP_SUB, {mul, seg}, diff);
    int head = diff;
    const int wide = concat ? M + 1 : M;
    if (concat) {
      const int c = g.add_slot(1, false);
      fills.emplace_back(c, std::vector<double>{lead});
      head = g.add_slot(wide, false);
      g.add_op(OP_CONCAT2, {c, diff}, head);
    }
    const int cs = g.add_slot(wide, false);
    g.add_op(OP_CUMSUM, {head}, cs);
    const int probs = g.add_slot(wide, false);
    g.add_op(OP_SOFTMAX, {cs}, probs);
    const int y = g.add_slot(1, false);
    fills.emplace_back(y, std::vector<double>{(double)(1 + (int)l % wide)});
    const int lp = g.add_slot(1, false);
    g.add_op(OP_CATEGORICAL, {y, probs}, lp);
    auto spec = std::make_shared<CategoricalSpec>();
    spec->scalar_outcome = true;
    spec->arg_autodiff = true;
    g.ops.back().udata = spec.get();
    g.udata_pool.push_back(std::move(spec));
    terms.push_back(lp);
  }
  const size_t before = g.ops.size();

  std::vector<int> tt = terms;
  Fills f2 = fills;
  const PartitionStats st = partition_lanes(g, f2, tt, {});
  expect("irt near miss refuses",
         st.groups == 0 && g.ops.size() == before && tt == terms);
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
  expect("segmentation is exactly linear", big.segment == 2 * small.segment);
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
  test_logistic_direct_terms();
  test_interleaved_lanes();
  test_scattered_indices();
  test_refuse_effectful();
  test_refuse_escaping_intermediate();
  test_refuse_identical_terms();
  test_idata_terms();
  test_duplicate_lanes_decline();
  test_binomial_idata_groups();
  test_width_w_outcome_group();
  test_binomial_elt_fusion();
  test_store_delimited_lanes();
  test_two_templates_interleaved();
  test_mid_bucket_writer_split();
  test_cross_bucket_read_after_write();
  test_kill_switch();
  test_state_space_shape();
  test_irt_glm_synthesis();
  test_irt_split_subtrahend();
  test_irt_refuse_near_miss(0.0, false);
  test_irt_refuse_near_miss(1.5, true);
  test_cost_declines();
  test_scaling();
  if (failures == 0) std::printf("test_partition OK\n");
  return failures == 0 ? 0 : 1;
}
