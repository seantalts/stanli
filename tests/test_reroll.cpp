// Re-roll pass: unrolled scalar-loop regions collapse to vector ops with
// gradients preserved (up to summation order, 1e-12 rel).
#include "env_helpers.hpp"
#include "graph_helpers.hpp"
#include <stanli/compile.hpp>
#include <stanli/density_registry.hpp>
#include <stanli/graph.hpp>
#include <stanli/inplace.hpp>
#include <stanli/optable.hpp>
#include <stanli/reroll.hpp>

#include "../runtime/src/reroll_profile.hpp"
#include "../runtime/src/reroll_plan.hpp"

#ifndef _WIN32
#include <sys/resource.h>
#endif

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
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
    std::printf("FAIL %-24s got %.17g want %.17g rel %.2e\n", what, got, want,
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

// Scalar LPDF kernels all use density_fwd_v, so their re-roll eligibility is
// generated from the same list. Shapes with a distinct contract must remain
// fail-closed rather than inheriting the trait by looking density-like.
static void test_scalar_density_traits() {
#define EXPECT_REROLL_TRAIT(code, fn, arity, tier) \
  expect(#code " has scalar-density trait",        \
         has_op_trait(code, op_trait::kRerollDensity));
  STANLI_SCALAR_DENSITY_LIST(EXPECT_REROLL_TRAIT)
#undef EXPECT_REROLL_TRAIT
  expect("ordered density stays excluded",
         !has_op_trait(OP_ORDERED_LOGISTIC_LPMF, op_trait::kRerollDensity));
  expect("matrix density stays excluded",
         !has_op_trait(OP_MULTI_NORMAL_LPDF, op_trait::kRerollDensity));
  expect("unrelated opcode stays excluded",
         !has_op_trait(OP_EXP, op_trait::kRerollDensity));
}

static void test_scalar_math_widenable_traits() {
#define EXPECT_UNARY_WIDENABLE(code, fn, value, delta, topology) \
  expect(#code " has widenable trait",                           \
         has_op_trait(code, op_trait::kRerollWidenable));
  STANLI_SCALAR_UNARY_LIST(EXPECT_UNARY_WIDENABLE)
#undef EXPECT_UNARY_WIDENABLE
#define EXPECT_BINARY_WIDENABLE(code, fn, name) \
  expect(#code " has widenable trait",          \
         has_op_trait(code, op_trait::kRerollWidenable));
  STANLI_SCALAR_BINARY_LIST(EXPECT_BINARY_WIDENABLE)
#undef EXPECT_BINARY_WIDENABLE
}

// radon shape: mu = vector intermediate written by an op; per lane
// {INDEX(mu,n); NORMAL(y_const_n, idx, sigma)}, lp -> target term.
// y consts deliberately share slots (dedup pool) between lanes 1 and 5.
static void test_radon_shape() {
  const int L = 8;
  Graph g;
  Fills fills;
  const int alpha = g.add_slot(1, true);
  const int sigma = g.add_slot(1, true);
  const int base = g.add_slot(L, false);
  g.add_op(OP_REP_VEC, {alpha}, base);  // makes base a written slot
  std::vector<int> yconst(L);
  for (int n = 0; n < L; ++n) {
    if (n == 5) {
      yconst[n] = yconst[1];
      continue;
    }  // dedup'd pool
    yconst[n] = g.add_slot(1, false);
    fills.emplace_back(yconst[n], std::vector<double>{0.25 * n - 1.0});
  }
  std::vector<int> terms;
  for (int n = 0; n < L; ++n) {
    const int idx = g.add_slot(1, false);
    g.add_op(OP_INDEX, {base}, idx, {n});
    const int lp = g.add_slot(1, false);
    const int id = g.add_op(OP_NORMAL_LPDF, {yconst[n], idx, sigma}, lp);
    g.ops[id].variant = 0x06;
    terms.push_back(lp);
  }
  // Reference BEFORE the pass (reduce terms via chained ADD_N).
  Graph ref = g;
  reduce_into_result(ref, terms);
  const std::vector<double> want = run_grad(std::move(ref), fills);

  std::vector<int> tt = terms;
  Fills f2 = fills;
  const detail::ProfiledRerollStats profiled =
      detail::reroll_profiled(g, f2, tt, {});
  const RerollStats st = profiled.work;
  expect("radon regions==1", st.regions == 1);
  expect("radon term-density disposition",
         profiled.dispositions.term_density == 1);
  expect("radon no other dispositions",
         profiled.dispositions.packed_rows == 0 &&
             profiled.dispositions.element_density == 0 &&
             profiled.dispositions.term_widen == 0 &&
             profiled.dispositions.element_store == 0);
  // 1 REP_VEC survives; 8 INDEX + 8 NORMAL collapse to 1 NORMAL.
  expect("radon ops==2", g.ops.size() == 2);
  expect("radon one term", tt.size() == 1);
  expect("radon vec y filled", f2.size() == fills.size() + 1);
  g.result_slot = tt[0];
  const std::vector<double> got = run_grad(std::move(g), f2);
  expect("radon sizes", got.size() == want.size());
  for (size_t i = 0; i < want.size() && i < got.size(); ++i)
    expect_close(("radon v" + std::to_string(i)).c_str(), got[i], want[i]);
}

// arK shape: per lane {INDEX(beta,k), MUL, ADD} x K then NORMAL.
// beta INDEX ops are lane-invariant (same idata) -> hoisted once.
// MUL second args are per-lane consts (the lag values).
static void test_ark_shape() {
  const int L = 6, K = 2;
  Graph g;
  Fills fills;
  const int alpha = g.add_slot(1, true);
  const int beta = g.add_slot(K, true);
  const int sigma = g.add_slot(1, true);
  auto cslot = [&](double v) {
    const int s = g.add_slot(1, false);
    fills.emplace_back(s, std::vector<double>{v});
    return s;
  };
  std::vector<std::vector<int>> lag(K, std::vector<int>(L));
  std::vector<int> yobs(L);
  for (int l = 0; l < L; ++l) {
    for (int k = 0; k < K; ++k) lag[k][l] = cslot(0.3 * l + 0.1 * k);
    yobs[l] = cslot(0.5 * l - 0.7);
  }
  std::vector<int> terms;
  for (int l = 0; l < L; ++l) {
    int mu = alpha;
    for (int k = 0; k < K; ++k) {
      const int bk = g.add_slot(1, false);
      g.add_op(OP_INDEX, {beta}, bk, {k});
      const int prod = g.add_slot(1, false);
      g.add_op(OP_MUL, {bk, lag[k][l]}, prod);
      const int acc = g.add_slot(1, false);
      g.add_op(OP_ADD, {mu, prod}, acc);
      mu = acc;
    }
    const int lp = g.add_slot(1, false);
    const int id = g.add_op(OP_NORMAL_LPDF, {yobs[l], mu, sigma}, lp);
    g.ops[id].variant = 0x86;  // propto + mu,sigma active, like real arK
    terms.push_back(lp);
  }
  Graph ref = g;
  reduce_into_result(ref, terms);
  const std::vector<double> want = run_grad(std::move(ref), fills);

  std::vector<int> tt = terms;
  Fills f2 = fills;
  RerollStats st = reroll(g, f2, tt, {});
  expect("ark regions==1", st.regions == 1);
  // 2 hoisted INDEX + 2 vec MUL + 2 vec ADD + 1 vec NORMAL = 7 ops.
  expect("ark ops==7", g.ops.size() == 7);
  expect("ark one term", tt.size() == 1);
  g.result_slot = tt[0];
  const std::vector<double> got = run_grad(std::move(g), f2);
  expect("ark sizes", got.size() == want.size());
  for (size_t i = 0; i < want.size() && i < got.size(); ++i)
    expect_close(("ark v" + std::to_string(i)).c_str(), got[i], want[i]);
}

// (a) cross-lane dependence: lane l reads lane l-1's output (a recurrence
// on parameters). Must NOT vectorize.
static void test_bail_recurrence() {
  const int L = 6;
  Graph g;
  Fills fills;
  const int sigma = g.add_slot(1, true);
  int prev = g.add_slot(1, true);  // x0 param
  std::vector<int> terms;
  for (int l = 0; l < L; ++l) {
    const int nx = g.add_slot(1, false);
    g.add_op(OP_MUL, {prev, sigma}, nx);
    const int lp = g.add_slot(1, false);
    const int id = g.add_op(OP_NORMAL_LPDF, {nx, prev, sigma}, lp);
    g.ops[id].variant = 0x06;
    terms.push_back(lp);
    prev = nx;  // <- lane l+1 reads lane l's out
  }
  std::vector<int> tt = terms;
  const size_t before = g.ops.size();
  RerollStats st = reroll(g, fills, tt, {});
  expect("recurrence not rerolled", st.regions == 0);
  expect("recurrence ops unchanged", g.ops.size() == before);
}

// (b) density outs consumed inside their own lanes (gauss_mix shape): the
// elementwise-lp rule fuses the densities (variant bit 6), widens the
// consumer, and swaps the lane terms for one SUM_VEC.
static void test_gauss_mix_shape() {
  const int L = 6;
  Graph g;
  Fills fills;
  const int theta = g.add_slot(1, true);
  const int mu1 = g.add_slot(1, true);
  const int sig1 = g.add_slot(1, true);
  const int mu2 = g.add_slot(1, true);
  const int sig2 = g.add_slot(1, true);
  auto cslot = [&](double v) {
    const int s = g.add_slot(1, false);
    fills.emplace_back(s, std::vector<double>{v});
    return s;
  };
  std::vector<int> terms;
  for (int l = 0; l < L; ++l) {
    const int y = cslot(0.4 * l - 1.1);
    const int lp1 = g.add_slot(1, false);
    const int i1 = g.add_op(OP_NORMAL_LPDF, {y, mu1, sig1}, lp1);
    g.ops[i1].variant = 0x06;
    const int lp2 = g.add_slot(1, false);
    const int i2 = g.add_op(OP_NORMAL_LPDF, {y, mu2, sig2}, lp2);
    g.ops[i2].variant = 0x06;
    const int t = g.add_slot(1, false);
    g.add_op(OP_LOG_MIX, {theta, lp1, lp2}, t);
    terms.push_back(t);
  }
  Graph ref = g;
  reduce_into_result(ref, terms);
  const std::vector<double> want = run_grad(std::move(ref), fills);

  std::vector<int> tt = terms;
  Fills f2 = fills;
  const detail::ProfiledRerollStats profiled =
      detail::reroll_profiled(g, f2, tt, {});
  const RerollStats st = profiled.work;
  expect("gmix regions==1", st.regions == 1);
  expect("gmix element-density dispositions",
         profiled.dispositions.element_density == 2);
  expect("gmix term-widen disposition", profiled.dispositions.term_widen == 1);
  // 2 elementwise NORMAL + 1 widened LOG_MIX + 1 SUM_VEC.
  expect("gmix ops==4", g.ops.size() == 4);
  expect("gmix one term", tt.size() == 1);
  expect("gmix elt variant", (g.ops[0].variant & 0x40) != 0);
  g.result_slot = tt[0];
  const std::vector<double> got = run_grad(std::move(g), f2);
  expect("gmix sizes", got.size() == want.size());
  for (size_t i = 0; i < want.size() && i < got.size(); ++i)
    expect_close(("gmix v" + std::to_string(i)).c_str(), got[i], want[i]);
}

// Gap 3 (docs/superpowers/plans/2026-09-11-lane-layout-unification.md): the
// gauss_mix idiom above, but each lane's density result is a row rather than
// a scalar, the way a per-subject mixture over several binary items looks
// (log_mix(theta, bernoulli_logit_lpmf(y[i,:] | eta1), bernoulli_logit_lpmf
// (y[i,:] | eta2))). bernoulli_logit has an elementwise form that costs per
// element what its summed one does, so widening it costs nothing extra: an
// OP_SUM_ROWS after each row-wide elementwise call folds it back to one lp
// per lane before OP_LOG_MIX ever sees it, unchanged from the scalar case.
static void test_row_mixture_shape() {
  const int L = 12, C = 4;
  Graph g;
  Fills fills;
  const int base = g.add_slot(L * C, true);  // row-major, a parameter
  const int theta = g.add_slot(1, true);
  std::vector<int> terms;
  for (int l = 0; l < L; ++l) {
    const int row = g.add_slot(C, false);
    g.add_op(OP_SLICE, {base}, row, {l * C});
    std::vector<int> outcomes1(C), outcomes2(C);
    for (int k = 0; k < C; ++k) {
      outcomes1[(size_t)k] = (l + k) % 2;
      outcomes2[(size_t)k] = (l + k + 1) % 2;
    }
    const int lp1 = g.add_slot(1, false);
    const int i1 = g.add_op(OP_BERNOULLI_LOGIT_LPMF, {row}, lp1, outcomes1);
    g.ops[(size_t)i1].variant = 0x81;
    const int lp2 = g.add_slot(1, false);
    const int i2 = g.add_op(OP_BERNOULLI_LOGIT_LPMF, {row}, lp2, outcomes2);
    g.ops[(size_t)i2].variant = 0x81;
    const int t = g.add_slot(1, false);
    g.add_op(OP_LOG_MIX, {theta, lp1, lp2}, t);
    terms.push_back(t);
  }
  Graph ref = g;
  reduce_into_result(ref, terms);
  const std::vector<double> want = run_grad(std::move(ref), fills);

  std::vector<int> tt = terms;
  Fills f2 = fills;
  const detail::ProfiledRerollStats profiled =
      detail::reroll_profiled(g, f2, tt, {});
  const RerollStats st = profiled.work;
  expect("row mix regions==1", st.regions == 1);
  expect("row mix element-density dispositions",
         profiled.dispositions.element_density == 2);
  expect("row mix term-widen disposition",
         profiled.dispositions.term_widen == 1);
  // 2 elementwise BERNOULLI_LOGIT + 2 SUM_ROWS + 1 widened LOG_MIX + 1
  // SUM_VEC; the row itself elides.
  expect("row mix ops==6", g.ops.size() == 6);
  int densities = 0, sum_rows = 0;
  for (const Op& op : g.ops) {
    densities += op.opcode == OP_BERNOULLI_LOGIT_LPMF;
    sum_rows += op.opcode == OP_SUM_ROWS;
  }
  expect("row mix two densities, two sum_rows",
         densities == 2 && sum_rows == 2);
  expect("row mix one term", tt.size() == 1);
  reduce_into_result(g, tt);
  const std::vector<double> got = run_grad(std::move(g), f2);
  expect("row mix sizes", got.size() == want.size());
  for (size_t i = 0; i < want.size() && i < got.size(); ++i)
    expect_close(("row mix v" + std::to_string(i)).c_str(), got[i], want[i]);
}

// The same gap, column-major (a matrix row: OP_SLICE_STRIDED, the shape a
// real `y[i, :]` compiles to). The elementwise density packs in column-major
// order (element k of lane l at l + Luse*k); OP_SUM_ROWS needs lane l's
// elements contiguous, so this must repack before it, not just widen.
static void test_row_mixture_shape_column_major() {
  const int L = 12, C = 4;
  Graph g;
  Fills fills;
  const int base = g.add_slot(L * C, true);  // column-major, a parameter
  std::vector<int> terms;
  for (int l = 0; l < L; ++l) {
    const int row = g.add_slot(C, false);
    g.add_op(OP_SLICE_STRIDED, {base}, row, {l, L});
    std::vector<int> outcomes(C);
    for (int k = 0; k < C; ++k) outcomes[(size_t)k] = (l + k) % 2;
    const int lp = g.add_slot(1, false);
    const int id = g.add_op(OP_BERNOULLI_LOGIT_LPMF, {row}, lp, outcomes);
    g.ops[(size_t)id].variant = 0x81;
    const int zero = g.add_slot(1, false);
    fills.emplace_back(zero, std::vector<double>{0.0});
    const int t = g.add_slot(1, false);
    g.add_op(OP_ADD, {lp, zero}, t);
    terms.push_back(t);
  }
  Graph ref = g;
  reduce_into_result(ref, terms);
  const std::vector<double> want = run_grad(std::move(ref), fills);

  std::vector<int> tt = terms;
  Fills f2 = fills;
  const detail::ProfiledRerollStats profiled =
      detail::reroll_profiled(g, f2, tt, {});
  const RerollStats st = profiled.work;
  expect("row mix col-major regions==1", st.regions == 1);
  expect("row mix col-major element-density disposition",
         profiled.dispositions.element_density == 1);
  // BERNOULLI_LOGIT + one repacking OP_GATHER + OP_SUM_ROWS + widened ADD
  // (the zero it adds is a fresh constant per lane, so the chain does not
  // stay scalar) + OP_SUM_VEC.
  expect("row mix col-major ops==5", g.ops.size() == 5);
  int gathers = 0, sum_rows = 0;
  for (const Op& op : g.ops) {
    gathers += op.opcode == OP_GATHER;
    sum_rows += op.opcode == OP_SUM_ROWS;
  }
  expect("row mix col-major repacks before summing",
         gathers == 1 && sum_rows == 1);
  expect("row mix col-major one term", tt.size() == 1);
  reduce_into_result(g, tt);
  const std::vector<double> got = run_grad(std::move(g), f2);
  expect("row mix col-major sizes", got.size() == want.size());
  for (size_t i = 0; i < want.size() && i < got.size(); ++i)
    expect_close(("row mix col-major v" + std::to_string(i)).c_str(), got[i],
                 want[i]);
}

// Logistic was historically omitted from the hand-maintained density
// allowlist even though it has the same density_fwd_v elementwise kernel as
// normal. Its lane-local lps must fuse behind bit 6, not merely acquire a
// trait that no behavioral test exercises.
static void test_logistic_elt_shape() {
  const int L = 6;
  Graph g;
  Fills fills;
  const int mu = g.add_slot(1, true);
  const int sigma = g.add_slot(1, true);
  auto cslot = [&](double v) {
    const int s = g.add_slot(1, false);
    fills.emplace_back(s, std::vector<double>{v});
    return s;
  };
  std::vector<int> terms;
  for (int l = 0; l < L; ++l) {
    const int lp = g.add_slot(1, false);
    const int id =
        g.add_op(OP_LOGISTIC_LPDF, {cslot(0.35 * l - 0.8), mu, sigma}, lp);
    g.ops[(size_t)id].variant = 0x06;
    const int term = g.add_slot(1, false);
    g.add_op(OP_ADD, {lp, cslot(0.1 * l - 0.25)}, term);
    terms.push_back(term);
  }
  Graph ref = g;
  reduce_into_result(ref, terms);
  const std::vector<double> want = run_grad(std::move(ref), fills);

  std::vector<int> tt = terms;
  Fills f2 = fills;
  const detail::ProfiledRerollStats profiled =
      detail::reroll_profiled(g, f2, tt, {});
  expect("logistic elt regions==1", profiled.work.regions == 1);
  expect("logistic elt density disposition",
         profiled.dispositions.element_density == 1);
  expect("logistic elt term disposition",
         profiled.dispositions.term_widen == 1);
  expect("logistic elt ops==3", g.ops.size() == 3 && tt.size() == 1);
  expect("logistic elt opcode", g.ops[0].opcode == OP_LOGISTIC_LPDF &&
                                    (g.ops[0].variant & 0x40u) != 0 &&
                                    g.slots[(size_t)g.ops[0].out].len == L);
  g.result_slot = tt[0];
  const std::vector<double> got = run_grad(std::move(g), f2);
  expect("logistic elt sizes", got.size() == want.size());
  for (size_t i = 0; i < want.size() && i < got.size(); ++i)
    expect_close(("logistic elt v" + std::to_string(i)).c_str(), got[i],
                 want[i]);
}

// (b2) idata-outcome lpmf feeding an in-lane op: the elementwise lpmf
// concatenates the lane outcomes into one idata array.
static void test_elt_lpmf_shape() {
  const int L = 6;
  Graph g;
  Fills fills;
  const int alpha = g.add_slot(L, true);
  auto cslot = [&](double v) {
    const int s = g.add_slot(1, false);
    fills.emplace_back(s, std::vector<double>{v});
    return s;
  };
  std::vector<int> terms;
  for (int l = 0; l < L; ++l) {
    const int a = g.add_slot(1, false);
    g.add_op(OP_INDEX, {alpha}, a, {l});
    const int lp = g.add_slot(1, false);
    const int id = g.add_op(OP_BERNOULLI_LOGIT_LPMF, {a}, lp, {l % 2});
    g.ops[id].variant = 0x81;
    const int t = g.add_slot(1, false);
    g.add_op(OP_ADD, {lp, cslot(0.2 * l)}, t);
    terms.push_back(t);
  }
  Graph ref = g;
  reduce_into_result(ref, terms);
  const std::vector<double> want = run_grad(std::move(ref), fills);

  std::vector<int> tt = terms;
  Fills f2 = fills;
  RerollStats st = reroll(g, f2, tt, {});
  expect("eltlpmf regions==1", st.regions == 1);
  // INDEX elides; 1 elementwise BERNOULLI_LOGIT + 1 widened ADD + SUM_VEC.
  expect("eltlpmf ops==3", g.ops.size() == 3);
  expect("eltlpmf idata==L", g.ops[0].n_idata == L);
  g.result_slot = tt[0];
  const std::vector<double> got = run_grad(std::move(g), f2);
  expect("eltlpmf sizes", got.size() == want.size());
  for (size_t i = 0; i < want.size() && i < got.size(); ++i)
    expect_close(("eltlpmf v" + std::to_string(i)).c_str(), got[i], want[i]);
}

// (b3) density whose inputs are all lane-invariant, feeding an in-lane op:
// the density HOISTS to one scalar op (widening scalar inputs into a len-N
// out is the losscurve hazard); the consumer broadcasts it.
static void test_elt_scalar_density_hoists() {
  const int L = 6;
  Graph g;
  Fills fills;
  const int mu = g.add_slot(1, true);
  const int sigma = g.add_slot(1, true);
  auto cslot = [&](double v) {
    const int s = g.add_slot(1, false);
    fills.emplace_back(s, std::vector<double>{v});
    return s;
  };
  const int y = cslot(0.7);  // same slot every lane: invariant
  std::vector<int> terms;
  for (int l = 0; l < L; ++l) {
    const int lp = g.add_slot(1, false);
    const int id = g.add_op(OP_NORMAL_LPDF, {y, mu, sigma}, lp);
    g.ops[id].variant = 0x06;
    const int t = g.add_slot(1, false);
    g.add_op(OP_ADD, {lp, cslot(0.3 * l - 0.5)}, t);
    terms.push_back(t);
  }
  Graph ref = g;
  reduce_into_result(ref, terms);
  const std::vector<double> want = run_grad(std::move(ref), fills);

  std::vector<int> tt = terms;
  Fills f2 = fills;
  RerollStats st = reroll(g, f2, tt, {});
  expect("elthoist regions==1", st.regions == 1);
  // 1 hoisted scalar NORMAL + 1 widened ADD (broadcast lp) + SUM_VEC.
  expect("elthoist ops==3", g.ops.size() == 3);
  expect("elthoist density scalar",
         (g.ops[0].variant & 0x40) == 0);  // NOT elementwise
  g.result_slot = tt[0];
  const std::vector<double> got = run_grad(std::move(g), f2);
  expect("elthoist sizes", got.size() == want.size());
  for (size_t i = 0; i < want.size() && i < got.size(); ++i)
    expect_close(("elthoist v" + std::to_string(i)).c_str(), got[i], want[i]);
}

// (b4) density out consumed by ANOTHER lane's op (cross-lane mixture
// recursion): must still bail.
static void test_bail_cross_lane_density() {
  const int L = 6;
  Graph g;
  Fills fills;
  const int mu = g.add_slot(1, true);
  const int sigma = g.add_slot(1, true);
  auto cslot = [&](double v) {
    const int s = g.add_slot(1, false);
    fills.emplace_back(s, std::vector<double>{v});
    return s;
  };
  std::vector<int> terms;
  int prev_lp = -1;
  for (int l = 0; l < L; ++l) {
    const int lp = g.add_slot(1, false);
    const int id = g.add_op(OP_NORMAL_LPDF, {cslot(0.1 * l), mu, sigma}, lp);
    g.ops[id].variant = 0x06;
    const int t = g.add_slot(1, false);
    // Lane 0 consumes its own lp; every later lane consumes the PREVIOUS
    // lane's lp: the density out escapes its lane.
    g.add_op(OP_ADD, {l == 0 ? lp : prev_lp, cslot(0.2)}, t);
    prev_lp = lp;
    terms.push_back(t);
  }
  std::vector<int> tt = terms;
  const size_t before = g.ops.size();
  RerollStats st = reroll(g, fills, tt, {});
  expect("crosslane not rerolled", st.regions == 0);
  expect("crosslane ops unchanged", g.ops.size() == before);
}

// (b5) density outs consumed only outside the region (one op sums them all
// after the run): must still bail.
static void test_bail_escaping_density() {
  const int L = 6;
  Graph g;
  Fills fills;
  const int mu = g.add_slot(1, true);
  const int sigma = g.add_slot(1, true);
  auto cslot = [&](double v) {
    const int s = g.add_slot(1, false);
    fills.emplace_back(s, std::vector<double>{v});
    return s;
  };
  std::vector<int> lps;
  for (int l = 0; l < L; ++l) {
    const int lp = g.add_slot(1, false);
    const int id = g.add_op(OP_NORMAL_LPDF, {cslot(0.1 * l), mu, sigma}, lp);
    g.ops[id].variant = 0x06;
    const int sq = g.add_slot(1, false);
    g.add_op(OP_SQUARE, {lp}, sq);  // in-lane consumer, keeps shape periodic
    lps.push_back(sq);
  }
  // The escape: everything ALSO read by one op past the region.
  const int total = g.add_slot(1, false);
  g.add_op(OP_ADD_N, {lps[0], lps[1], lps[2], lps[3], lps[4], lps[5]}, total);
  std::vector<int> tt{total};
  const size_t before = g.ops.size();
  RerollStats st = reroll(g, fills, tt, {});
  expect("escape not rerolled", st.regions == 0);
  expect("escape ops unchanged", g.ops.size() == before);
}

// (c) partial-range INDEX progression (a contiguous window of a longer
// base): one OP_SLICE, which is cheaper than a gather -- no index array.
static void test_partial_range_slices() {
  const int L = 6, START = 2;
  Graph g;
  Fills fills;
  const int alpha = g.add_slot(1, true);
  const int sigma = g.add_slot(1, true);
  const int base = g.add_slot(L + 3, false);  // longer than lane count
  g.add_op(OP_REP_VEC, {alpha}, base);
  auto cslot = [&](double v) {
    const int s = g.add_slot(1, false);
    fills.emplace_back(s, std::vector<double>{v});
    return s;
  };
  std::vector<int> terms;
  for (int l = 0; l < L; ++l) {
    const int idx = g.add_slot(1, false);
    g.add_op(OP_INDEX, {base}, idx, {START + l});
    const int lp = g.add_slot(1, false);
    const int id = g.add_op(OP_NORMAL_LPDF, {cslot(0.2 * l), idx, sigma}, lp);
    g.ops[id].variant = 0x06;
    terms.push_back(lp);
  }
  Graph ref = g;
  reduce_into_result(ref, terms);
  const std::vector<double> want = run_grad(std::move(ref), fills);

  std::vector<int> tt = terms;
  Fills f2 = fills;
  RerollStats st = reroll(g, f2, tt, {});
  expect("partial range rerolled", st.regions == 1);
  int slices = 0;
  for (const Op& op : g.ops) slices += op.opcode == OP_SLICE;
  expect("window becomes a slice", slices == 1);
  expect("slice ops==3", g.ops.size() == 3);  // REP_VEC + SLICE + NORMAL
  reduce_into_result(g, tt);
  const std::vector<double> got = run_grad(std::move(g), f2);
  for (size_t i = 0; i < want.size() && i < got.size(); ++i)
    expect_close(("slice v" + std::to_string(i)).c_str(), got[i], want[i]);
}

// A data-driven index (`alpha[county_idx[n]]`, the hierarchical idiom,
// repeats and all) becomes one OP_GATHER over the lane indices.
static void test_data_index_gathers() {
  const int L = 9, J = 4;
  const int idx[L] = {0, 1, 2, 3, 0, 1, 2, 3, 0};  // repeats, wraps
  Graph g;
  Fills fills;
  const int alpha = g.add_slot(J, true);
  const int sigma = g.add_slot(1, true);
  auto cslot = [&](double v) {
    const int s = g.add_slot(1, false);
    fills.emplace_back(s, std::vector<double>{v});
    return s;
  };
  std::vector<int> terms;
  for (int l = 0; l < L; ++l) {
    const int a = g.add_slot(1, false);
    g.add_op(OP_INDEX, {alpha}, a, {idx[l]});
    const int lp = g.add_slot(1, false);
    const int id =
        g.add_op(OP_NORMAL_LPDF, {cslot(0.3 * l - 1.0), a, sigma}, lp);
    g.ops[id].variant = 0x06;
    terms.push_back(lp);
  }
  Graph ref = g;
  reduce_into_result(ref, terms);
  const std::vector<double> want = run_grad(std::move(ref), fills);

  std::vector<int> tt = terms;
  Fills f2 = fills;
  RerollStats st = reroll(g, f2, tt, {});
  expect("gather regions==1", st.regions == 1);
  int gathers = 0;
  for (const Op& op : g.ops) gathers += op.opcode == OP_GATHER;
  expect("index becomes a gather", gathers == 1);
  expect("gather ops==2", g.ops.size() == 2);  // GATHER + vector NORMAL
  reduce_into_result(g, tt);
  const std::vector<double> got = run_grad(std::move(g), f2);
  // Repeated indices mean the gather's scatter-add accumulates several
  // lanes into one alpha element: the value that matters most here.
  for (size_t i = 0; i < want.size() && i < got.size(); ++i)
    expect_close(("gather v" + std::to_string(i)).c_str(), got[i], want[i]);
}

// The cost model: the same shape as test_data_index_gathers, but at four
// lanes (the pass's own floor) instead of nine. A gather prices at
// 2*Luse*width against a scalar dispatch's kLaneOpCost=5; at four lanes
// the two positions' eliminated dispatches (4*2*5=40) do not clear their
// own cost (2*4*1 + 2*5 = 18) plus partition.cpp's 40-unit margin, so the
// region stays scalar. test_data_index_gathers proves the identical shape
// packs once there are enough lanes to amortize the fixed overhead.
static void test_cost_model_declines_small_gather() {
  const int L = 4, J = 4;
  const int idx[L] = {3, 1, 0, 2};  // a permutation: forces OP_GATHER
  Graph g;
  Fills fills;
  const int alpha = g.add_slot(J, true);
  const int sigma = g.add_slot(1, true);
  auto cslot = [&](double v) {
    const int s = g.add_slot(1, false);
    fills.emplace_back(s, std::vector<double>{v});
    return s;
  };
  std::vector<int> terms;
  for (int l = 0; l < L; ++l) {
    const int a = g.add_slot(1, false);
    g.add_op(OP_INDEX, {alpha}, a, {idx[l]});
    const int lp = g.add_slot(1, false);
    const int id =
        g.add_op(OP_NORMAL_LPDF, {cslot(0.3 * l - 1.0), a, sigma}, lp);
    g.ops[id].variant = 0x06;
    terms.push_back(lp);
  }
  const size_t before = g.ops.size();
  std::vector<int> tt = terms;
  Fills f2 = fills;
  const RerollStats st = reroll(g, f2, tt, {});
  expect("small gather declines",
         st.regions == 0 && g.ops.size() == before && tt.size() == (size_t)L);
}

// (d) STANLI_NO_REROLL disables the pass.
static void test_env_disable() {
  test_setenv("STANLI_NO_REROLL", "1", 1);
  const int L = 6;
  Graph g;
  Fills fills;
  const int mu = g.add_slot(1, true);
  const int sigma = g.add_slot(1, true);
  std::vector<int> terms;
  for (int l = 0; l < L; ++l) {
    const int c = g.add_slot(1, false);
    fills.emplace_back(c, std::vector<double>{0.1 * l});
    const int lp = g.add_slot(1, false);
    const int id = g.add_op(OP_NORMAL_LPDF, {c, mu, sigma}, lp);
    g.ops[id].variant = 0x06;
    terms.push_back(lp);
  }
  std::vector<int> tt = terms;
  const size_t before = g.ops.size();
  RerollStats st = reroll(g, fills, tt, {});
  expect("env disables", st.regions == 0);
  expect("env ops unchanged", g.ops.size() == before);
  test_unsetenv("STANLI_NO_REROLL");
}

// (e) first lane anomalous (its y is an op output, not a const): the pass
// must skip lane 0 and still re-roll lanes 1..L-1 on the second attempt.
// L is large enough that the remaining 1-position region clears reroll's
// cost model (a bare term density needs Luse > 9 to beat 40's margin at
// kLaneOpCost=5 per op); a smaller L here would report a correct decline,
// not a bug, and is exactly what test_bail_recurrence-style cases are for.
static void test_first_lane_anomalous() {
  const int L = 16;
  Graph g;
  Fills fills;
  const int mu = g.add_slot(1, true);
  const int sigma = g.add_slot(1, true);
  // lane 0's observation comes from an op, not the const pool
  const int y0 = g.add_slot(1, false);
  g.add_op(OP_MUL, {mu, sigma}, y0);
  std::vector<int> terms;
  for (int l = 0; l < L; ++l) {
    int y;
    if (l == 0) {
      y = y0;
    } else {
      y = g.add_slot(1, false);
      fills.emplace_back(y, std::vector<double>{0.1 * l});
    }
    const int lp = g.add_slot(1, false);
    const int id = g.add_op(OP_NORMAL_LPDF, {y, mu, sigma}, lp);
    g.ops[id].variant = 0x06;
    terms.push_back(lp);
  }
  Graph ref = g;
  reduce_into_result(ref, terms);
  const std::vector<double> want = run_grad(std::move(ref), fills);

  std::vector<int> tt = terms;
  Fills f2 = fills;
  RerollStats st = reroll(g, f2, tt, {});
  expect("anomalous regions==1", st.regions == 1);
  // MUL + lane-0 NORMAL survive scalar; lanes 1..7 collapse to 1 vec op.
  expect("anomalous ops==3", g.ops.size() == 3);
  expect("anomalous terms==2", tt.size() == 2);
  reduce_into_result(g, tt);
  const std::vector<double> got = run_grad(std::move(g), f2);
  expect("anomalous sizes", got.size() == want.size());
  for (size_t i = 0; i < want.size() && i < got.size(); ++i)
    expect_close(("anom v" + std::to_string(i)).c_str(), got[i], want[i]);
}

// (f) block-structured region (rats_model shape, time-major data): the
// INDEX idata is a 0..B-1 progression that restarts every block. The pass
// must split the run at block boundaries and re-roll every block.
static void test_block_structured() {
  const int B = 5;   // lanes per block (== param vector length)
  const int NB = 3;  // blocks
  Graph g;
  Fills fills;
  const int theta = g.add_slot(B, true);
  const int sigma = g.add_slot(1, true);
  std::vector<int> terms;
  for (int b = 0; b < NB; ++b) {
    for (int l = 0; l < B; ++l) {
      const int y = g.add_slot(1, false);
      fills.emplace_back(y, std::vector<double>{0.2 * (b * B + l) - 0.5});
      const int idx = g.add_slot(1, false);
      g.add_op(OP_INDEX, {theta}, idx, {l});  // restarts every block
      const int lp = g.add_slot(1, false);
      const int id = g.add_op(OP_NORMAL_LPDF, {y, idx, sigma}, lp);
      g.ops[id].variant = 0x06;
      terms.push_back(lp);
    }
  }
  Graph ref = g;
  reduce_into_result(ref, terms);
  const std::vector<double> want = run_grad(std::move(ref), fills);

  std::vector<int> tt = terms;
  Fills f2 = fills;
  RerollStats st = reroll(g, f2, tt, {});
  // The cycling index (0..B-1 restarting per block) is now a single
  // data-driven gather over all NB*B lanes, so the blocks fuse into ONE
  // region instead of one region per block.
  expect("block regions==1", st.regions == 1);
  expect("block ops==2", g.ops.size() == 2);  // GATHER + vector NORMAL
  expect("block terms==1", tt.size() == 1);
  reduce_into_result(g, tt);
  const std::vector<double> got = run_grad(std::move(g), f2);
  expect("block sizes", got.size() == want.size());
  for (size_t i = 0; i < want.size() && i < got.size(); ++i)
    expect_close(("block v" + std::to_string(i)).c_str(), got[i], want[i]);
}

// (i) Write-side fusion. `y_hat[n] = a[idx[n]]` under an unrolled loop, then
// one vector density over y_hat: the whole run of element writes becomes the
// gathered vector itself, with no store at all when it covers the vector.
//
// Builds the run at `start`, so the same code exercises the full-vector case
// (start 0, the writes ARE the vector) and the window case (start 1, which
// has to keep a real store for the element the run does not reach).
namespace writefuse {

struct Built {
  Graph g;
  Fills fills;
  std::vector<int> terms;
  int y_hat = -1, a = -1, sigma = -1;
};

Built build(int L, int start, int stride = 1) {
  Built b;
  Graph& g = b.g;
  const int J = 5;
  b.a = g.add_slot(J, true);
  b.sigma = g.add_slot(1, true);
  // stride 1: exactly the window, so the start==0 case covers the vector
  // and exercises the no-store elision. Otherwise: big enough for the comb.
  const int64_t last = start + (int64_t)(L - 1) * stride;
  const int64_t vlen =
      stride == 1 ? start + (int64_t)L : std::max((int64_t)start, last) + 1;
  b.y_hat = g.add_slot(vlen, false);
  b.fills.emplace_back(b.y_hat, std::vector<double>((size_t)vlen, 0.0));
  const int ydata = g.add_slot(vlen, false);
  std::vector<double> yv((size_t)vlen);
  for (int64_t k = 0; k < vlen; ++k) yv[(size_t)k] = 0.4 * (double)k - 1.0;
  b.fills.emplace_back(ydata, yv);
  for (int l = 0; l < L; ++l) {
    const int v = g.add_slot(1, false);
    g.add_op(OP_INDEX, {b.a}, v, {l % J});
    g.add_op(OP_SET_INDEX_INPLACE, {b.y_hat, v}, b.y_hat, {start + l * stride});
  }
  const int lp = g.add_slot(1, false);
  const int id = g.add_op(OP_NORMAL_LPDF, {ydata, b.y_hat, b.sigma}, lp);
  g.ops[(size_t)id].variant = 0x06;
  b.terms.push_back(lp);
  return b;
}

int count(const Graph& g, uint16_t oc) {
  int n = 0;
  for (const Op& op : g.ops) n += op.opcode == oc;
  return n;
}

}  // namespace writefuse

// (n) ldaK5 shape: gamma may be one shared vector refilled by every outer
// iteration, or a fresh unrolled declaration per row. Each inner loop writes
// every element and LOG_SUM_EXP reads the completed row. The former chains N
// store regions through one slot, so eager tail renaming made the pass
// quadratic in both time and memory (the real model: 32,877 iterations, 52 s
// and 49 GB to compile, which OOM-killed every CI runner).
//
// Both are linear now, but they were fixed in two separate goes: the
// lazy renaming took the quadratic term out of the allocation and left
// one in the work, which stood for a while because nothing measured it
// (n=32,000 was 350 MB and 4 s). Binary search over the sorted use and
// writer lists in reroll.cpp took out the second: 0.2 s at that size.
//
// The guards below are a memory ceiling and a scaling ratio rather than
// the wall-clock budget this test used to carry. A budget cannot work
// here. The gap between a regression and a fix is around 13x, CI's
// slowest runner is 9x slower than its fastest, and those two overlap --
// the old budget duly failed a commit that touched nothing but an
// unrelated directory. A ratio and an RSS figure mean the same thing on
// every machine.
namespace ldashape {

struct Built {
  Graph g;
  Fills fills;
  std::vector<int> terms;
};

enum class GammaMode { Shared, Fresh };

Built build(int N, int K = 5, GammaMode mode = GammaMode::Shared) {
  const int V = 7;
  Built b;
  Graph& g = b.g;
  const int t0 = g.add_slot(1, true);
  const int p0 = g.add_slot(1, true);
  const int thetav = g.add_slot(K, false);
  g.add_op(OP_REP_VEC, {t0}, thetav);
  const int phiv = g.add_slot(V, false);
  g.add_op(OP_REP_VEC, {p0}, phiv);
  int gamma = -1;
  if (mode == GammaMode::Shared) {
    gamma = g.add_slot(K, false);
    b.fills.emplace_back(gamma, std::vector<double>(K, 0.0));
  }
  for (int n = 0; n < N; ++n) {
    if (mode == GammaMode::Fresh) {
      gamma = g.add_slot(K, false);
      b.fills.emplace_back(gamma, std::vector<double>(K, 0.0));
    }
    for (int k = 0; k < K; ++k) {
      const int i1 = g.add_slot(1, false);
      g.add_op(OP_INDEX, {thetav}, i1, {k});
      const int l1 = g.add_slot(1, false);
      g.add_op(OP_LOGV, {i1}, l1);
      const int i2 = g.add_slot(1, false);
      g.add_op(OP_INDEX, {phiv}, i2, {(n + 2 * k) % V});
      const int l2 = g.add_slot(1, false);
      g.add_op(OP_LOGV, {i2}, l2);
      const int s = g.add_slot(1, false);
      g.add_op(OP_ADD, {l1, l2}, s);
      if (k == 0 && (n == 0 || mode == GammaMode::Fresh)) {
        // The in-place pass cannot mutate a fill-backed declaration on its
        // first write: repeated evaluations would inherit the last draw. A
        // shared declaration copies once; fresh unrolled declarations copy at
        // the start of every row. Later writes share the copied output.
        const int next = g.add_slot(K, false);
        g.add_op(OP_SET_INDEX, {gamma, s}, next, {k});
        gamma = next;
      } else {
        g.add_op(OP_SET_INDEX_INPLACE, {gamma, s}, gamma, {k});
      }
    }
    const int lse = g.add_slot(1, false);
    g.add_op(OP_LOG_SUM_EXP, {gamma}, lse);
    b.terms.push_back(lse);
  }
  return b;
}

static void test_lda_shape_refusals() {
  // Fewer than four row targets cannot amortize the packed form and should
  // return before even scanning the graph for a row signature.
  {
    ldashape::Built b = ldashape::build(3, 2);
    const size_t before = b.g.ops.size();
    const RerollStats st = reroll(b.g, b.fills, b.terms, {});
    expect("lda short rows do not pack",
           writefuse::count(b.g, OP_LOG_SUM_EXP_ROWS) == 0);
    expect("lda short rows skip recognizer", st.row_steps == 0);
    expect("lda short rows keep graph", b.g.ops.size() == before);
  }

  // A graph-external view of gamma is invisible to op use counts. The explicit
  // root must keep the complete mutable row chain intact.
  {
    ldashape::Built b = ldashape::build(12, 2);
    int gamma = -1;
    for (const Op& op : b.g.ops)
      if (op.opcode == OP_SET_INDEX || op.opcode == OP_SET_INDEX_INPLACE)
        gamma = op.out;
    const size_t before = b.g.ops.size();
    const RerollStats st = reroll(b.g, b.fills, b.terms, {gamma});
    expect("lda rows refuse gamma root",
           writefuse::count(b.g, OP_LOG_SUM_EXP_ROWS) == 0);
    expect("lda gamma-root refusal keeps graph", b.g.ops.size() == before);
    expect("lda gamma-root refusal stays linear",
           st.row_steps < 4 * (int64_t)before);
  }

  // The gathers are hoisted ahead of every row. A base that is also the
  // mutable gamma buffer would therefore turn a recurrence into independent
  // reads and must be rejected explicitly.
  {
    ldashape::Built b = ldashape::build(12, 2);
    int gamma = -1;
    for (const Op& op : b.g.ops)
      if (op.opcode == OP_SET_INDEX || op.opcode == OP_SET_INDEX_INPLACE)
        gamma = op.out;
    int seen_index = 0;
    for (Op& op : b.g.ops)
      if (op.opcode == OP_INDEX && (seen_index++ % 2) == 0) op.in[0] = gamma;
    reroll(b.g, b.fills, b.terms, {});
    expect("lda rows refuse mutable base",
           writefuse::count(b.g, OP_LOG_SUM_EXP_ROWS) == 0);
  }

  // Full overwrite makes the old gamma contents dead, but its final contents
  // are not dead when another op observes them after the candidate batch.
  {
    ldashape::Built b = ldashape::build(12, 2);
    int gamma = -1;
    for (const Op& op : b.g.ops)
      if (op.opcode == OP_SET_INDEX || op.opcode == OP_SET_INDEX_INPLACE)
        gamma = op.out;
    const int escaped = b.g.add_slot(1, false);
    b.g.add_op(OP_LOG_SUM_EXP, {gamma}, escaped);
    reroll(b.g, b.fills, b.terms, {});
    expect("lda rows refuse live gamma",
           writefuse::count(b.g, OP_LOG_SUM_EXP_ROWS) == 0);
  }

  // Fresh scratch does not weaken the escape rule: observing any completed
  // row buffer after the candidate run keeps all of its stores intact.
  {
    ldashape::Built b = ldashape::build(12, 2, ldashape::GammaMode::Fresh);
    int gamma = -1;
    int row = 0;
    for (const Op& op : b.g.ops)
      if (op.opcode == OP_SET_INDEX && ++row == 6) gamma = op.out;
    const int escaped = b.g.add_slot(1, false);
    b.g.add_op(OP_LOG_SUM_EXP, {gamma}, escaped);
    reroll(b.g, b.fills, b.terms, {});
    expect("lda fresh rows refuse live gamma",
           writefuse::count(b.g, OP_LOG_SUM_EXP_ROWS) == 0);
  }

  // The packed SUM replaces one contiguous subsequence. A target list with
  // the same slots in a different order must not be silently regrouped.
  {
    ldashape::Built b = ldashape::build(12, 2);
    std::swap(b.terms[1], b.terms[2]);
    const size_t before = b.g.ops.size();
    reroll(b.g, b.fills, b.terms, {});
    expect("lda rows refuse reordered targets",
           writefuse::count(b.g, OP_LOG_SUM_EXP_ROWS) == 0);
    expect("lda target-order refusal keeps graph", b.g.ops.size() == before);
  }

  // Every scalar intermediate must belong solely to its grammar consumer.
  // Give one ADD per row a second reader; K=2 also prevents the generic inner
  // reroller from obscuring the row recognizer's refusal.
  {
    ldashape::Built b = ldashape::build(12, 2);
    int seen_add = 0;
    std::vector<int> escaped;
    for (const Op& op : b.g.ops)
      if (op.opcode == OP_ADD && (seen_add++ % 2) == 0)
        escaped.push_back(op.out);
    for (int s : escaped) {
      const int dead = b.g.add_slot(1, false);
      b.g.add_op(OP_NEG, {s}, dead);
    }
    reroll(b.g, b.fills, b.terms, {});
    expect("lda rows refuse escaped intermediates",
           writefuse::count(b.g, OP_LOG_SUM_EXP_ROWS) == 0);
  }

  // One malformed first row is a boundary, not a model-wide bail: the exact
  // suffix still fuses, while the incomplete row retains its scalar LSE.
  {
    ldashape::Built b = ldashape::build(12, 2);
    for (Op& op : b.g.ops) {
      if (op.opcode != OP_SET_INDEX) continue;
      b.g.idata_pool.push_back(std::vector<int>{1});  // duplicate k=1
      op.idata = b.g.idata_pool.back().data();
      op.n_idata = 1;
      break;
    }
    reroll(b.g, b.fills, b.terms, {});
    expect("lda malformed row stays scalar",
           writefuse::count(b.g, OP_LOG_SUM_EXP) == 1);
    expect("lda valid suffix still packs",
           writefuse::count(b.g, OP_LOG_SUM_EXP_ROWS) == 1);
  }
}

}  // namespace ldashape

static void test_lda_shape_gradients() {
  const int N = 40;
  for (ldashape::GammaMode mode :
       {ldashape::GammaMode::Shared, ldashape::GammaMode::Fresh})
    for (int K : {2, 5}) {
      const std::string shape =
          mode == ldashape::GammaMode::Shared ? "shared" : "fresh";
      const std::string label = "lda " + shape + " K" + std::to_string(K);
      ldashape::Built b = ldashape::build(N, K, mode);
      expect((label + " fill shape").c_str(),
             b.fills.size() ==
                 (mode == ldashape::GammaMode::Shared ? 1u : (size_t)N));
      Graph ref = b.g;
      reduce_into_result(ref, b.terms);
      const std::vector<double> want = run_grad(std::move(ref), b.fills);

      std::vector<int> tt = b.terms;
      Fills f2 = b.fills;
      const detail::ProfiledRerollStats profiled =
          detail::reroll_profiled(b.g, f2, tt, {});
      const RerollStats st = profiled.work;
      expect((label + " becomes one region").c_str(), st.regions == 1);
      expect((label + " records packed-row disposition").c_str(),
             profiled.dispositions.packed_rows == 1);
      expect((label + " has seven fused ops plus inputs").c_str(),
             b.g.ops.size() == 9);
      expect((label + " has one packed reduction").c_str(),
             writefuse::count(b.g, OP_LOG_SUM_EXP_ROWS) == 1);
      expect((label + " has no scalar reductions").c_str(),
             writefuse::count(b.g, OP_LOG_SUM_EXP) == 0);
      expect((label + " has no stores").c_str(),
             writefuse::count(b.g, OP_SET_INDEX) == 0 &&
                 writefuse::count(b.g, OP_SET_INDEX_INPLACE) == 0);
      expect((label + " has one target").c_str(), tt.size() == 1);
      reduce_into_result(b.g, tt);
      const std::vector<double> got = run_grad(std::move(b.g), f2);
      expect((label + " gradient sizes").c_str(), got.size() == want.size());
      for (size_t i = 0; i < want.size() && i < got.size(); ++i)
        expect_close((label + " v" + std::to_string(i)).c_str(), got[i],
                     want[i]);
    }
}

// Peak resident set for the process in MB, or -1 where the platform does
// not report one. ru_maxrss is bytes on macOS and kilobytes elsewhere, a
// difference that would otherwise read as a 1000x regression.
static double peak_rss_mb() {
#ifdef _WIN32
  return -1.0;
#else
  struct rusage ru;
  if (getrusage(RUSAGE_SELF, &ru) != 0) return -1.0;
#ifdef __APPLE__
  return static_cast<double>(ru.ru_maxrss) / (1024.0 * 1024.0);
#else
  return static_cast<double>(ru.ru_maxrss) / 1024.0;
#endif
#endif
}

// What reroll costs on the lda shape at size n: deterministic row-recognizer
// work and the wall clock, which is reported but not asserted on. The fused
// graph shape is checked so a pass that got cheap by doing less is not mistaken
// for a pass that got cheap.
struct LdaCost {
  double sec;
  int64_t steps;
};

static LdaCost lda_reroll_cost(int n) {
  ldashape::Built b = ldashape::build(n);
  std::vector<int> tt = b.terms;
  const auto t0 = std::chrono::steady_clock::now();
  RerollStats st = reroll(b.g, b.fills, tt, {});
  const double sec =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
          .count();
  expect("lda big rows fused", st.regions == 1);
  expect("lda big fixed fused graph", b.g.ops.size() == 9 && tt.size() == 1);
  return {sec, st.row_steps};
}

static void test_lda_shape_cost() {
  const LdaCost small = lda_reroll_cost(8000);
  const LdaCost big = lda_reroll_cost(16000);
  const double rss = peak_rss_mb();
  std::printf(
      "  lda shape reroll: n=8000 %.2f s / %lld steps,"
      " n=16000 %.2f s / %lld steps (%.1fx steps, %.1fx time),"
      " peak RSS %.0f MB\n",
      small.sec, (long long)small.steps, big.sec, (long long)big.steps,
      small.steps > 0 ? (double)big.steps / (double)small.steps : 0.0,
      small.sec > 0.0 ? big.sec / small.sec : 0.0, rss);

  // The memory ceiling is the guard that matters, because memory is what
  // broke, and unlike a wall-clock number an RSS figure means the same
  // thing on every machine. This is the whole process's peak, so it
  // counts every case that ran before this one -- an over-estimate,
  // which is the safe direction. All of them together reach under
  // 200 MB; the same shape under the quadratic-space bug would be around
  // 12 GB at n=16000, so 1 GB separates them with room on both sides.
  if (rss > 0.0) expect("lda reroll space stays linear", rss < 1024.0);

  // Count the work, do not time it. A doubling of n doubles the scalar row
  // grammar and the ownership scan. The count is the same integer on every
  // machine and also covers late fail-closed runs, which must not retry every
  // suffix of one shared gamma chain.
  //
  // The wall clock is not. Two earlier versions of this check gated on
  // it, first as an absolute budget and then as a ratio, and between
  // them they failed three CI runs in one day on the shared macOS
  // runners, on both arches, while the same binary measured 2.1x on a
  // quiet laptop. The ratio formulation barely separated the two cases
  // even there: the quadratic scan timed 3.1x against a 3.0x bound.
  expect("lda reroll work stays near-linear", big.steps < 3 * small.steps);
}

// A graph with no density, element store, or target-term widenable op cannot
// contain a profitable region at any period. LOG_SUM_EXP is deliberately not
// in that allowlist: this is the scalar residue left by ldaK5 after its gamma
// rows are built, and making every output a target proves the fast path is not
// merely relying on an empty target list.
struct NoCandidateCost {
  double sec;
  int64_t steps;
};

static NoCandidateCost no_candidate_cost(int n) {
  Graph g;
  Fills fills;
  const int row = g.add_slot(5, true);
  std::vector<int> terms;
  terms.reserve((size_t)n);
  for (int i = 0; i < n; ++i) {
    const int out = g.add_slot(1, false);
    g.add_op(OP_LOG_SUM_EXP, {row}, out);
    terms.push_back(out);
  }
  const size_t before_ops = g.ops.size();
  const std::vector<int> before_terms = terms;
  const auto t0 = std::chrono::steady_clock::now();
  const RerollStats st = reroll(g, fills, terms, {});
  const double sec =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
          .count();
  expect("no-candidate finds no regions", st.regions == 0);
  expect("no-candidate never reads use lists", st.list_steps == 0);
  expect("no-candidate graph unchanged", g.ops.size() == before_ops);
  expect("no-candidate terms unchanged", terms == before_terms);
  return {sec, st.candidate_steps};
}

static void test_no_candidate_cost() {
  const NoCandidateCost small = no_candidate_cost(8000);
  const NoCandidateCost big = no_candidate_cost(16000);
  std::printf(
      "  no-candidate reroll: n=8000 %.6f s / %lld steps,"
      " n=16000 %.6f s / %lld steps (%.1fx steps, %.1fx time)\n",
      small.sec, (long long)small.steps, big.sec, (long long)big.steps,
      small.steps > 0 ? (double)big.steps / (double)small.steps : 0.0,
      small.sec > 0.0 ? big.sec / small.sec : 0.0);

  // Count the work rather than asserting on a machine-dependent timer. The
  // one viability pass examines every op exactly once; the old nested period
  // scan performed O(kMaxPeriod^2 * n) checks after reaching the same answer.
  expect("no-candidate small scans once", small.steps == 8000);
  expect("no-candidate big scans once", big.steps == 16000);
  expect("no-candidate work scales linearly", big.steps == 2 * small.steps);
}

// One early density makes the whole-graph guard pass, but the long tail has no
// candidates. This is nn_rbm's shape: a few prior densities followed by a
// large scalar residue. Candidate windows must be range queries over the index,
// not repeated scans through that tail.
struct SparseCandidateCost {
  double sec;
  int64_t steps;
  int64_t expected;
};

static SparseCandidateCost sparse_candidate_cost(int n) {
  Graph g;
  Fills fills;
  const int y = g.add_slot(1, false);
  const int mu = g.add_slot(1, true);
  const int sigma = g.add_slot(1, true);
  const int lp = g.add_slot(1, false);
  g.add_op(OP_NORMAL_LPDF, {y, mu, sigma}, lp);
  std::vector<int> terms{lp};
  const int row = g.add_slot(5, true);
  terms.reserve((size_t)n + 1);
  for (int i = 0; i < n; ++i) {
    const int out = g.add_slot(1, false);
    g.add_op(OP_LOG_SUM_EXP, {row}, out);
    terms.push_back(out);
  }

  const size_t n_ops = g.ops.size();
  int64_t expected = (int64_t)n_ops;  // build the next-candidate index
  for (size_t i = 0; i < n_ops; ++i)
    for (int P = 1; P <= 32 && i + 2 * (size_t)P <= n_ops; ++P) ++expected;

  const size_t before_ops = g.ops.size();
  const std::vector<int> before_terms = terms;
  const auto t0 = std::chrono::steady_clock::now();
  const RerollStats st = reroll(g, fills, terms, {});
  const double sec =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
          .count();
  expect("sparse-candidate finds no regions", st.regions == 0);
  expect("sparse-candidate never reads use lists", st.list_steps == 0);
  expect("sparse-candidate graph unchanged", g.ops.size() == before_ops);
  expect("sparse-candidate terms unchanged", terms == before_terms);
  return {sec, st.candidate_steps, expected};
}

static void test_sparse_candidate_cost() {
  const SparseCandidateCost small = sparse_candidate_cost(8000);
  const SparseCandidateCost big = sparse_candidate_cost(16000);
  std::printf(
      "  sparse-candidate reroll: n=8000 %.6f s / %lld steps,"
      " n=16000 %.6f s / %lld steps (%.1fx steps, %.1fx time)\n",
      small.sec, (long long)small.steps, big.sec, (long long)big.steps,
      small.steps > 0 ? (double)big.steps / (double)small.steps : 0.0,
      small.sec > 0.0 ? big.sec / small.sec : 0.0);

  expect("sparse-candidate small counts index queries",
         small.steps == small.expected);
  expect("sparse-candidate big counts index queries",
         big.steps == big.expected);
  expect("sparse-candidate work stays linear", big.steps < 3 * small.steps);
}

static void test_write_fusion() {
  using namespace writefuse;
  for (int start : {0, 1}) {
    const int L = 8;
    Built b = build(L, start);
    Graph ref = b.g;
    reduce_into_result(ref, b.terms);
    const std::vector<double> want = run_grad(std::move(ref), b.fills);

    std::vector<int> tt = b.terms;
    Fills f2 = b.fills;
    const detail::ProfiledRerollStats profiled =
        detail::reroll_profiled(b.g, f2, tt, {});
    const RerollStats st = profiled.work;
    const std::string tag = start ? "window" : "whole";
    expect((tag + " regions==1").c_str(), st.regions == 1);
    expect((tag + " records element-store disposition").c_str(),
           profiled.dispositions.element_store == 1);
    expect((tag + " one gather").c_str(), count(b.g, OP_GATHER) == 1);
    expect((tag + " no element writes left").c_str(),
           count(b.g, OP_SET_INDEX_INPLACE) == 0);
    // Covering the vector needs no store; a window has to keep one.
    expect((tag + " store count").c_str(),
           count(b.g, OP_SET_SLICE) == (start ? 1 : 0));
    expect((tag + " op count").c_str(),
           b.g.ops.size() == (size_t)(start ? 3 : 2));
    reduce_into_result(b.g, tt);
    const std::vector<double> got = run_grad(std::move(b.g), f2);
    for (size_t i = 0; i < want.size() && i < got.size(); ++i)
      expect_close((tag + " v" + std::to_string(i)).c_str(), got[i], want[i]);
  }
}

// (j) What write-side fusion must refuse. Each case leaves the run alone:
// redirecting later readers to the fused value is only sound when nothing
// else observes the vector mid-run, writes it afterwards, or reads it from
// outside the graph entirely.
static void test_write_fusion_bails() {
  using namespace writefuse;
  {  // a strided run fuses into OP_SET_SLICE_STRIDED, gradients preserved
    Built b = build(8, 0, 2);
    Graph ref = b.g;
    reduce_into_result(ref, b.terms);
    const std::vector<double> want = run_grad(std::move(ref), b.fills);
    Fills f2 = b.fills;
    std::vector<int> tt = b.terms;
    reroll(b.g, f2, tt, {});
    expect("strided writes fused", count(b.g, OP_SET_INDEX_INPLACE) == 0 &&
                                       count(b.g, OP_SET_SLICE_STRIDED) == 1);
    reduce_into_result(b.g, tt);
    const std::vector<double> got = run_grad(std::move(b.g), f2);
    for (size_t i = 0; i < want.size() && i < got.size(); ++i)
      expect_close(("strided v" + std::to_string(i)).c_str(), got[i], want[i]);
  }
  {  // indices marching backwards: no positive stride, no fusion
    Built b = build(8, 14, -2);
    Fills f2 = b.fills;
    std::vector<int> tt = b.terms;
    const size_t before = b.g.ops.size();
    reroll(b.g, f2, tt, {});
    expect("descending writes not fused",
           count(b.g, OP_SET_INDEX_INPLACE) == 8 && b.g.ops.size() == before);
  }
  {  // the vector is a root: something outside the graph reads it
    Built b = build(8, 0);
    Fills f2 = b.fills;
    std::vector<int> tt = b.terms;
    const size_t before = b.g.ops.size();
    reroll(b.g, f2, tt, {b.y_hat});
    expect("root vector not fused",
           count(b.g, OP_SET_INDEX_INPLACE) == 8 && b.g.ops.size() == before);
  }
  {  // a later op writes the vector again: the run still fuses, into the
     // STORE form, and the later write chains onto the store's output --
     // the interleaved-block shape (dogs) in miniature.
    Graph g;
    Fills fills;
    const int a = g.add_slot(5, true), sigma = g.add_slot(1, true);
    const int yh = g.add_slot(8, false);
    fills.emplace_back(yh, std::vector<double>(8, 0.0));
    const int yd = g.add_slot(8, false);
    fills.emplace_back(yd, std::vector<double>(8, 0.75));
    for (int l = 0; l < 8; ++l) {
      const int v = g.add_slot(1, false);
      g.add_op(OP_INDEX, {a}, v, {l % 5});
      g.add_op(OP_SET_INDEX_INPLACE, {yh, v}, yh, {l});
    }
    const int extra = g.add_slot(1, false);
    fills.emplace_back(extra, std::vector<double>{2.5});
    g.add_op(OP_SET_INDEX_INPLACE, {yh, extra}, yh, {0});
    const int lp = g.add_slot(1, false);
    const int id = g.add_op(OP_NORMAL_LPDF, {yd, yh, sigma}, lp);
    g.ops[(size_t)id].variant = 0x06;
    std::vector<int> terms{lp};

    Graph ref = g;
    reduce_into_result(ref, terms);
    const std::vector<double> want = run_grad(std::move(ref), fills);

    Fills f2 = fills;
    std::vector<int> tt = terms;
    reroll(g, f2, tt, {});
    expect("later writer forces the store form",
           count(g, OP_SET_SLICE) == 1 && count(g, OP_GATHER) == 1);
    expect("later write chains, not blocks",
           count(g, OP_SET_INDEX_INPLACE) == 1);
    reduce_into_result(g, tt);
    const std::vector<double> got = run_grad(std::move(g), f2);
    for (size_t i = 0; i < want.size() && i < got.size(); ++i)
      expect_close(("chain v" + std::to_string(i)).c_str(), got[i], want[i]);
  }
  {  // something reads the vector while it is still half-written
    // Built by hand with a read of y_hat inside every lane.
    Graph g2;
    Fills f2;
    const int a = g2.add_slot(5, true), sigma = g2.add_slot(1, true);
    const int yh = g2.add_slot(8, false);
    f2.emplace_back(yh, std::vector<double>(8, 0.0));
    const int yd = g2.add_slot(8, false);
    f2.emplace_back(yd, std::vector<double>(8, 0.75));
    std::vector<int> terms;
    for (int l = 0; l < 8; ++l) {
      const int v = g2.add_slot(1, false);
      g2.add_op(OP_INDEX, {a}, v, {l % 5});
      g2.add_op(OP_SET_INDEX_INPLACE, {yh, v}, yh, {l});
      const int peek = g2.add_slot(1, false);
      g2.add_op(OP_INDEX, {yh}, peek, {0});  // mid-run reader
    }
    const int lp = g2.add_slot(1, false);
    const int id = g2.add_op(OP_NORMAL_LPDF, {yd, yh, sigma}, lp);
    g2.ops[(size_t)id].variant = 0x06;
    terms.push_back(lp);
    std::vector<int> tt = terms;
    reroll(g2, f2, tt, {});
    expect("mid-run reader blocks fusion",
           count(g2, OP_SET_INDEX_INPLACE) == 8);
  }
}

// Reroll creates slice stores after the compiler's first in-place pass has
// already run. Two disjoint comb runs model Mtbh's column fills: the first
// store must copy its fill-backed base, while the second can reuse that fresh
// result when the pass runs again.
// Six lanes per region, not reroll's four-lane floor: two OP_INDEX/
// OP_SET_INDEX_INPLACE positions priced honestly (reroll.cpp's cost model)
// need more than the floor to beat staying scalar by more than a rounding
// error, the same bar partition.cpp's bucket pricing applies.
static void test_post_reroll_slice_inplace() {
  const int kLanes = 6;
  const int n = 2 * kLanes;
  Graph g;
  Fills fills;
  const int a = g.add_slot(n, true), sigma = g.add_slot(1, true);
  const int yh = g.add_slot(n, false);
  fills.emplace_back(yh, std::vector<double>((size_t)n, -0.25));
  const int yd = g.add_slot(n, false);
  fills.emplace_back(yd, std::vector<double>((size_t)n, 0.75));

  for (int l = 0; l < kLanes; ++l) {
    const int v = g.add_slot(1, false);
    g.add_op(OP_INDEX, {a}, v, {l});
    g.add_op(OP_SET_INDEX_INPLACE, {yh, v}, yh, {2 * l});
  }
  // A non-lane scalar op separates the two affine store regions.
  const int sep = g.add_slot(1, false);
  g.add_op(OP_SUM_VEC, {a}, sep);
  for (int l = 0; l < kLanes; ++l) {
    const int v = g.add_slot(1, false);
    g.add_op(OP_INDEX, {a}, v, {kLanes + l});
    g.add_op(OP_SET_INDEX_INPLACE, {yh, v}, yh, {1 + 2 * l});
  }
  const int lp = g.add_slot(1, false);
  const int id = g.add_op(OP_NORMAL_LPDF, {yd, yh, sigma}, lp);
  g.ops[(size_t)id].variant = 0x06;
  std::vector<int> terms{sep, lp};

  Graph ref = g;
  std::vector<int> ref_terms = terms;
  reduce_into_result(ref, ref_terms);
  const std::vector<double> want = run_grad(std::move(ref), fills);

  Fills optimized_fills = fills;
  std::vector<int> optimized_terms = terms;
  reroll(g, optimized_fills, optimized_terms, {});
  expect("reroll made two strided stores",
         writefuse::count(g, OP_SET_SLICE_STRIDED) == 2);
  std::vector<int> roots = optimized_terms;
  expect("post-reroll rewrites later slice",
         make_inplace_updates(g, roots) == 1);
  expect("post-reroll preserves first copy",
         writefuse::count(g, OP_SET_SLICE_STRIDED) == 1 &&
             writefuse::count(g, OP_SET_SLICE_STRIDED_INPLACE) == 1);
  reduce_into_result(g, optimized_terms);

  Executor ex(std::move(g));
  for (const auto& f : optimized_fills) {
    double* p = ex.value_ptr(f.first);
    for (size_t j = 0; j < f.second.size(); ++j) p[j] = f.second[j];
  }
  for (int64_t i = 0; i < ex.n_params(); ++i) ex.params_data()[i] = fill_at(i);
  std::vector<double> first(1 + ex.n_params()), second(1 + ex.n_params());
  first[0] = ex.gradient(first.data() + 1);
  second[0] = ex.gradient(second.data() + 1);
  for (size_t i = 0; i < want.size() && i < first.size(); ++i) {
    expect_close(("post-reroll v" + std::to_string(i)).c_str(), first[i],
                 want[i]);
    expect_close(("post-reroll repeat v" + std::to_string(i)).c_str(),
                 second[i], first[i]);
  }
}

// (k) The losscurve shape: a run whose value chain is scalar end to end,
// because its lane-varying inputs all come from HOISTED producers. Widening
// such an op hands the kernels scalar inputs with a vector output, and their
// scalar-x-scalar paths write element 0 only -- the rest of the window
// filled with arena zeros. Found by the corpus A/B (losscurve_sislob,
// deviation 1.14e+00); the fix keeps the chain scalar and broadcasts at the
// store.
static void test_write_fusion_scalar_chain() {
  const int L = 8;
  Graph g;
  Fills fills;
  const int a = g.add_slot(5, true), sigma = g.add_slot(1, true);
  const int yh = g.add_slot(L + 4, false);
  fills.emplace_back(yh, std::vector<double>((size_t)L + 4, 0.25));
  const int yd = g.add_slot(L + 4, false);
  fills.emplace_back(yd, std::vector<double>((size_t)L + 4, 0.75));
  const int c2 = g.add_slot(1, false);
  fills.emplace_back(c2, std::vector<double>{1.75});
  for (int l = 0; l < L; ++l) {
    // Same element of `a` every lane -> the INDEX hoists; the MUL's inputs
    // are then a hoisted scalar and an invariant scalar.
    const int v = g.add_slot(1, false);
    g.add_op(OP_INDEX, {a}, v, {2});
    const int m = g.add_slot(1, false);
    g.add_op(OP_MUL, {v, c2}, m);
    g.add_op(OP_SET_INDEX_INPLACE, {yh, m}, yh, {l});
  }
  const int lp = g.add_slot(1, false);
  const int id = g.add_op(OP_NORMAL_LPDF, {yd, yh, sigma}, lp);
  g.ops[(size_t)id].variant = 0x06;
  std::vector<int> terms{lp};

  Graph ref = g;
  reduce_into_result(ref, terms);
  const std::vector<double> want = run_grad(std::move(ref), fills);

  Fills f2 = fills;
  std::vector<int> tt = terms;
  RerollStats st = reroll(g, f2, tt, {});
  expect("scalar-chain region rewrote", st.regions == 1);
  int repv = 0;
  for (const Op& op : g.ops) repv += op.opcode == OP_REP_VEC;
  expect("scalar value broadcast once", repv == 1);
  reduce_into_result(g, tt);
  const std::vector<double> got = run_grad(std::move(g), f2);
  for (size_t i = 0; i < want.size() && i < got.size(); ++i)
    expect_close(("scalar-chain v" + std::to_string(i)).c_str(), got[i],
                 want[i]);
}

// The dogs shape: per lane, two data exponents index a data vector and raise
// a scalar parameter, and the product is the lane's target term. POW is
// widened with a broadcast base and a per-lane exponent.
static void test_pow_widens() {
  const int L = 8;
  Graph g;
  Fills fills;
  const int a = g.add_slot(1, true), b = g.add_slot(1, true);
  const int shock = g.add_slot(L, false), avoid = g.add_slot(L, false);
  std::vector<double> sv((size_t)L), av((size_t)L);
  for (int l = 0; l < L; ++l) {
    sv[(size_t)l] = (double)(l % 4);
    av[(size_t)l] = (double)(l / 2);
  }
  fills.emplace_back(shock, sv);
  fills.emplace_back(avoid, av);
  std::vector<int> terms;
  for (int l = 0; l < L; ++l) {
    const int s = g.add_slot(1, false);
    g.add_op(OP_INDEX, {shock}, s, {l});
    const int v = g.add_slot(1, false);
    g.add_op(OP_INDEX, {avoid}, v, {l});
    const int pa = g.add_slot(1, false);
    g.add_op(OP_POW, {a, s}, pa);
    const int pb = g.add_slot(1, false);
    g.add_op(OP_POW, {b, v}, pb);
    const int t = g.add_slot(1, false);
    g.add_op(OP_MUL, {pa, pb}, t);
    terms.push_back(t);
  }
  Graph ref = g;
  reduce_into_result(ref, terms);
  const std::vector<double> want = run_grad(std::move(ref), fills);

  std::vector<int> tt = terms;
  Fills f2 = fills;
  RerollStats st = reroll(g, f2, tt, {});
  expect("pow regions==1", st.regions == 1);
  // The indexed constants become the lanes' exponent vectors, so the INDEX
  // ops elide: 2 POW + MUL + SUM_VEC.
  expect("pow ops==4", g.ops.size() == 4);
  expect("pow one term", tt.size() == 1);
  int widened = 0;
  for (const Op& op : g.ops)
    if (op.opcode == OP_POW) widened += g.slots[(size_t)op.out].len == L;
  expect("both pows widened", widened == 2);
  g.result_slot = tt[0];
  const std::vector<double> got = run_grad(std::move(g), f2);
  expect("pow sizes", got.size() == want.size());
  for (size_t i = 0; i < want.size() && i < got.size(); ++i)
    expect_close(("pow v" + std::to_string(i)).c_str(), got[i], want[i]);
}

// ---- vector lanes: a lane is one row of a container ----------------------

namespace rowlanes {

// Row l of a column-major R x C matrix reads as SLICE_STRIDED {l, R}; row l
// of an array of C-vectors as SLICE {l * C}.
enum class Rows { kMatrix, kArray };

struct Built {
  Graph g;
  Fills fills;
  std::vector<int> terms;
  int base = -1;
  std::vector<std::vector<int>> outcomes;   // density lanes: y per lane
  std::vector<std::vector<double>> rowval;  // store lanes: x per lane
};

int flat(Rows rows, int L, int C, int l, int k) {
  return rows == Rows::kMatrix ? l + L * k : l * C + k;
}

// L lanes {SLICE row l of a parameter base; BERNOULLI_LOGIT(row) with C
// outcomes} -> target terms: the dogs likelihood. extra_rows > 0 leaves
// rows the lanes never read.
Built build_density(int L, int C, Rows rows, int extra_rows = 0) {
  Built b;
  Graph& g = b.g;
  const int R = L + extra_rows;
  b.base = g.add_slot((int64_t)R * C, true);
  for (int l = 0; l < L; ++l) {
    const int row = g.add_slot(C, false);
    if (rows == Rows::kMatrix)
      g.add_op(OP_SLICE_STRIDED, {b.base}, row, {l, R});
    else
      g.add_op(OP_SLICE, {b.base}, row, {l * C});
    std::vector<int> y((size_t)C);
    for (int k = 0; k < C; ++k) y[(size_t)k] = (l * 7 + k * 3) % 2;
    const int lp = g.add_slot(1, false);
    const int id = g.add_op(OP_BERNOULLI_LOGIT_LPMF, {row}, lp, y);
    g.ops[(size_t)id].variant = 0x81;
    b.outcomes.push_back(y);
    b.terms.push_back(lp);
  }
  return b;
}

Built build_density_shared_row(int L, int C, Rows rows) {
  Built b;
  Graph& g = b.g;
  b.base = g.add_slot((int64_t)L * C, true);
  const int shared = g.add_slot(1, true);
  for (int l = 0; l < L; ++l) {
    const int row = g.add_slot(C, false);
    if (rows == Rows::kMatrix)
      g.add_op(OP_SLICE_STRIDED, {b.base}, row, {l, L});
    else
      g.add_op(OP_SLICE, {b.base}, row, {l * C});
    const int fixed = g.add_slot(1, false);
    if (rows == Rows::kMatrix)
      g.add_op(OP_SLICE_STRIDED, {shared}, fixed, {0, 1});
    else
      g.add_op(OP_SLICE, {shared}, fixed, {0});
    const int combined = g.add_slot(C, false);
    g.add_op(OP_ADD, {row, fixed}, combined);
    std::vector<int> y((size_t)C);
    for (int k = 0; k < C; ++k) y[(size_t)k] = (l * 7 + k * 3) % 2;
    const int lp = g.add_slot(1, false);
    const int id = g.add_op(OP_BERNOULLI_LOGIT_LPMF, {combined}, lp, y);
    g.ops[(size_t)id].variant = 0x81;
    b.outcomes.push_back(y);
    b.terms.push_back(lp);
  }
  return b;
}

// L lanes {INDEX beta[0]; INDEX beta[1]; FMA(b1, x row, b0); store to row l}
// filling a declared R x C matrix, then one density over the whole matrix.
// Lane 0 writes the fill-backed declaration functionally, the rest in place.
Built build_store(int L, int C, Rows rows, int extra_rows = 0,
                  bool later_write = false) {
  Built b;
  Graph& g = b.g;
  const int R = L + extra_rows;
  const int beta = g.add_slot(2, true);
  const int sigma = g.add_slot(1, true);
  const int decl = g.add_slot((int64_t)R * C, false);
  b.fills.emplace_back(decl, std::vector<double>((size_t)(R * C), 0.0));
  b.base = g.add_slot((int64_t)R * C, false);
  const int ydata = g.add_slot((int64_t)R * C, false);
  std::vector<double> yv((size_t)(R * C));
  for (size_t i = 0; i < yv.size(); ++i) yv[i] = 0.3 * (double)i - 1.0;
  b.fills.emplace_back(ydata, yv);
  for (int l = 0; l < L; ++l) {
    const int b0 = g.add_slot(1, false);
    g.add_op(OP_INDEX, {beta}, b0, {0});
    const int b1 = g.add_slot(1, false);
    g.add_op(OP_INDEX, {beta}, b1, {1});
    const int x = g.add_slot(C, false);
    std::vector<double> xv((size_t)C);
    for (int k = 0; k < C; ++k) xv[(size_t)k] = 0.5 * l + 0.25 * k - 1.0;
    b.fills.emplace_back(x, xv);
    b.rowval.push_back(xv);
    const int v = g.add_slot(C, false);
    g.add_op(OP_FMA, {b1, x, b0}, v);
    const int src = l == 0 ? decl : b.base;
    if (rows == Rows::kMatrix)
      g.add_op(l == 0 ? OP_SET_SLICE_STRIDED : OP_SET_SLICE_STRIDED_INPLACE,
               {src, v}, b.base, {l, R});
    else
      g.add_op(l == 0 ? OP_SET_SLICE : OP_SET_SLICE_INPLACE, {src, v}, b.base,
               {l * C});
  }
  if (later_write) {
    const int extra = g.add_slot(C, false);
    b.fills.emplace_back(extra, std::vector<double>((size_t)C, 2.5));
    if (rows == Rows::kMatrix)
      g.add_op(OP_SET_SLICE_STRIDED_INPLACE, {b.base, extra}, b.base, {2, R});
    else
      g.add_op(OP_SET_SLICE_INPLACE, {b.base, extra}, b.base, {2 * C});
  }
  const int lp = g.add_slot(1, false);
  const int id = g.add_op(OP_NORMAL_LPDF, {ydata, b.base, sigma}, lp);
  g.ops[(size_t)id].variant = 0x06;
  b.terms.push_back(lp);
  return b;
}

int row_stores(const Graph& g) {
  return writefuse::count(g, OP_SET_SLICE) +
         writefuse::count(g, OP_SET_SLICE_INPLACE) +
         writefuse::count(g, OP_SET_SLICE_STRIDED) +
         writefuse::count(g, OP_SET_SLICE_STRIDED_INPLACE);
}

}  // namespace rowlanes

// (a, b) L row-density lanes over a matrix base and over an array of
// vectors: one density over L*C elements reading the base in place, with
// the lanes' outcomes laid out in the base's storage order.
static void test_row_density_lanes() {
  using namespace rowlanes;
  for (Rows rows : {Rows::kMatrix, Rows::kArray}) {
    const int L = 6, C = 4;
    const std::string tag =
        rows == Rows::kMatrix ? "rowdens-mat" : "rowdens-arr";
    Built b = build_density(L, C, rows);
    Graph ref = b.g;
    reduce_into_result(ref, b.terms);
    const std::vector<double> want = run_grad(std::move(ref), b.fills);

    std::vector<int> tt = b.terms;
    Fills f2 = b.fills;
    const detail::ProfiledRerollStats profiled =
        detail::reroll_profiled(b.g, f2, tt, {});
    expect((tag + " regions==1").c_str(), profiled.work.regions == 1);
    expect((tag + " term-density disposition").c_str(),
           profiled.dispositions.term_density == 1);
    expect((tag + " ops==1").c_str(), b.g.ops.size() == 1);
    expect((tag + " one term").c_str(), tt.size() == 1);
    expect((tag + " no constant materialized").c_str(),
           f2.size() == b.fills.size());
    if (b.g.ops.size() == 1) {
      const Op& d = b.g.ops[0];
      expect((tag + " density reads the base").c_str(),
             d.opcode == OP_BERNOULLI_LOGIT_LPMF && d.in[0] == b.base);
      expect((tag + " outcomes L*C").c_str(), d.n_idata == L * C);
      bool laid_out = d.n_idata == L * C;
      for (int l = 0; laid_out && l < L; ++l)
        for (int k = 0; k < C; ++k)
          if (d.idata[flat(rows, L, C, l, k)] !=
              b.outcomes[(size_t)l][(size_t)k])
            laid_out = false;
      expect((tag + " outcomes in storage order").c_str(), laid_out);
    }
    if (!tt.empty()) {
      reduce_into_result(b.g, tt);
      const std::vector<double> got = run_grad(std::move(b.g), f2);
      expect((tag + " sizes").c_str(), got.size() == want.size());
      for (size_t i = 0; i < want.size() && i < got.size(); ++i)
        expect_close((tag + " v" + std::to_string(i)).c_str(), got[i], want[i]);
    }
  }
}

// (c) L row-store lanes: the FMA chain widens to L*C, its data rows pack
// into one constant in the base's storage order, and the fused value IS
// the matrix: no per-row stores remain.
static void test_row_store_lanes() {
  using namespace rowlanes;
  for (Rows rows : {Rows::kMatrix, Rows::kArray}) {
    const int L = 6, C = 4;
    const std::string tag =
        rows == Rows::kMatrix ? "rowstore-mat" : "rowstore-arr";
    Built b = build_store(L, C, rows);
    Graph ref = b.g;
    reduce_into_result(ref, b.terms);
    const std::vector<double> want = run_grad(std::move(ref), b.fills);

    std::vector<int> tt = b.terms;
    Fills f2 = b.fills;
    const detail::ProfiledRerollStats profiled =
        detail::reroll_profiled(b.g, f2, tt, {});
    expect((tag + " regions==1").c_str(), profiled.work.regions == 1);
    expect((tag + " element-store disposition").c_str(),
           profiled.dispositions.element_store == 1);
    expect((tag + " no row stores left").c_str(), row_stores(b.g) == 0);
    expect((tag + " one FMA").c_str(), writefuse::count(b.g, OP_FMA) == 1);
    // 2 hoisted INDEX + 1 FMA + NORMAL.
    expect((tag + " ops==4").c_str(), b.g.ops.size() == 4);
    expect((tag + " one constant materialized").c_str(),
           f2.size() == b.fills.size() + 1);
    int fma_out = -1, fma_x = -1;
    for (const Op& op : b.g.ops)
      if (op.opcode == OP_FMA) {
        fma_out = op.out;
        fma_x = op.in[1];
      }
    expect((tag + " FMA widened to L*C").c_str(),
           fma_out >= 0 && b.g.slots[(size_t)fma_out].len == L * C);
    for (const Op& op : b.g.ops)
      if (op.opcode == OP_NORMAL_LPDF)
        expect((tag + " density reads the fused value").c_str(),
               op.in[1] == fma_out);
    if (f2.size() == b.fills.size() + 1) {
      const auto& packed = f2.back();
      bool laid_out =
          packed.first == fma_x && packed.second.size() == (size_t)(L * C);
      for (int l = 0; laid_out && l < L; ++l)
        for (int k = 0; k < C; ++k)
          if (packed.second[(size_t)flat(rows, L, C, l, k)] !=
              b.rowval[(size_t)l][(size_t)k])
            laid_out = false;
      expect((tag + " rows packed in storage order").c_str(), laid_out);
    }
    reduce_into_result(b.g, tt);
    const std::vector<double> got = run_grad(std::move(b.g), f2);
    expect((tag + " sizes").c_str(), got.size() == want.size());
    for (size_t i = 0; i < want.size() && i < got.size(); ++i)
      expect_close((tag + " v" + std::to_string(i)).c_str(), got[i], want[i]);
  }
}

// (d) What vector lanes must leave alone: lanes that do not cover every
// row, a reduction inside the lane, and an invariant operand as wide as
// the lane.
static void test_row_lanes_bail() {
  using namespace rowlanes;
  {  // rows 0..L-1 of an (L+1)-row base: the fused op cannot read it whole
    Built b = build_density(6, 4, Rows::kMatrix, 1);
    const size_t before = b.g.ops.size();
    Fills f2 = b.fills;
    std::vector<int> tt = b.terms;
    const RerollStats st = reroll(b.g, f2, tt, {});
    expect("uncovered rows not fused",
           st.regions == 0 && b.g.ops.size() == before && tt.size() == 6);
  }
  {  // the same for row stores
    Built b = build_store(6, 4, Rows::kMatrix, 1);
    const size_t before = b.g.ops.size();
    Fills f2 = b.fills;
    std::vector<int> tt = b.terms;
    const RerollStats st = reroll(b.g, f2, tt, {});
    expect("uncovered row stores not fused",
           st.regions == 0 && b.g.ops.size() == before && row_stores(b.g) == 6);
  }
  {  // a later writer of the matrix: the run fuses into a whole-vector
     // store and the later write chains onto it
    Built b = build_store(6, 4, Rows::kMatrix, 0, true);
    Graph ref = b.g;
    reduce_into_result(ref, b.terms);
    const std::vector<double> want = run_grad(std::move(ref), b.fills);
    Fills f2 = b.fills;
    std::vector<int> tt = b.terms;
    const RerollStats st = reroll(b.g, f2, tt, {});
    expect("later writer forces the whole-vector store form",
           st.regions == 1 && writefuse::count(b.g, OP_SET_SLICE) == 1 &&
               writefuse::count(b.g, OP_SET_SLICE_STRIDED) == 0);
    expect("later row write chains, not blocks",
           writefuse::count(b.g, OP_SET_SLICE_STRIDED_INPLACE) == 1);
    reduce_into_result(b.g, tt);
    const std::vector<double> got = run_grad(std::move(b.g), f2);
    for (size_t i = 0; i < want.size() && i < got.size(); ++i)
      expect_close(("rowstore-chain v" + std::to_string(i)).c_str(), got[i],
                   want[i]);
  }
  {  // DOT inside the lane
    const int L = 6, C = 4;
    Graph g;
    Fills fills;
    const int base = g.add_slot(L * C, true);
    const int w = g.add_slot(C, true);
    const int sigma = g.add_slot(1, true);
    std::vector<int> terms;
    for (int l = 0; l < L; ++l) {
      const int row = g.add_slot(C, false);
      g.add_op(OP_SLICE_STRIDED, {base}, row, {l, L});
      const int d = g.add_slot(1, false);
      g.add_op(OP_DOT, {row, w}, d);
      const int y = g.add_slot(1, false);
      fills.emplace_back(y, std::vector<double>{0.1 * l});
      const int lp = g.add_slot(1, false);
      const int id = g.add_op(OP_NORMAL_LPDF, {y, d, sigma}, lp);
      g.ops[(size_t)id].variant = 0x06;
      terms.push_back(lp);
    }
    const size_t before = g.ops.size();
    std::vector<int> tt = terms;
    const RerollStats st = reroll(g, fills, tt, {});
    expect("lane reduction not fused",
           st.regions == 0 && g.ops.size() == before);
  }
  {  // An invariant vector operand as wide as the lane tiles via OP_REP_MAT
     // (gap 2, lane-layout-unification.md) instead of blocking the region.
     // The row read is OP_SLICE_STRIDED (column-major), so the tile must
     // repeat each of mu's elements Luse times consecutively, not mu itself
     // Luse times end to end.
    const int L = 6, C = 4;
    Graph g;
    Fills fills;
    const int base = g.add_slot(L * C, true);
    const int mu = g.add_slot(C, true);
    std::vector<int> terms;
    for (int l = 0; l < L; ++l) {
      const int row = g.add_slot(C, false);
      g.add_op(OP_SLICE_STRIDED, {base}, row, {l, L});
      const int s = g.add_slot(C, false);
      g.add_op(OP_ADD, {row, mu}, s);
      const int lp = g.add_slot(1, false);
      const int id = g.add_op(OP_BERNOULLI_LOGIT_LPMF, {s}, lp,
                              std::vector<int>((size_t)C, l % 2));
      g.ops[(size_t)id].variant = 0x81;
      terms.push_back(lp);
    }
    Graph ref = g;
    reduce_into_result(ref, terms);
    const std::vector<double> want = run_grad(std::move(ref), fills);

    std::vector<int> tt = terms;
    Fills f2 = fills;
    const RerollStats st = reroll(g, f2, tt, {});
    expect("lane-wide invariant operand regions==1", st.regions == 1);
    expect("lane-wide invariant operand ops==3", g.ops.size() == 3);
    int rep_mats = 0, adds = 0, densities = 0;
    for (const Op& op : g.ops) {
      rep_mats += op.opcode == OP_REP_MAT;
      adds += op.opcode == OP_ADD;
      densities += op.opcode == OP_BERNOULLI_LOGIT_LPMF;
    }
    expect("lane-wide invariant operand tiles mu",
           rep_mats == 1 && adds == 1 && densities == 1);
    for (const Op& op : g.ops)
      if (op.opcode == OP_REP_MAT)
        expect("lane-wide invariant operand tiles column-major",
               op.n_idata == 3 && op.idata[0] == L && op.idata[1] == C &&
                   op.idata[2] == 2 && g.slots[(size_t)op.out].len == L * C);
    expect("lane-wide invariant operand one term", tt.size() == 1);
    reduce_into_result(g, tt);
    const std::vector<double> got = run_grad(std::move(g), f2);
    for (size_t i = 0; i < want.size() && i < got.size(); ++i)
      expect_close(("lane-wide invariant v" + std::to_string(i)).c_str(),
                   got[i], want[i]);
  }
}

static void test_row_lane_shared_read() {
  using namespace rowlanes;
  for (Rows rows : {Rows::kMatrix, Rows::kArray}) {
    const int L = 6, C = 4;
    const std::string tag =
        rows == Rows::kMatrix ? "rowshared-mat" : "rowshared-arr";
    Built b = build_density_shared_row(L, C, rows);
    Graph ref = b.g;
    reduce_into_result(ref, b.terms);
    const std::vector<double> want = run_grad(std::move(ref), b.fills);

    std::vector<int> tt = b.terms;
    Fills f2 = b.fills;
    const detail::ProfiledRerollStats profiled =
        detail::reroll_profiled(b.g, f2, tt, {});
    expect((tag + " regions==1").c_str(), profiled.work.regions == 1);
    expect((tag + " shared row hoisted").c_str(),
           writefuse::count(b.g, OP_SLICE) +
                   writefuse::count(b.g, OP_SLICE_STRIDED) <=
               1);
    if (!tt.empty()) {
      reduce_into_result(b.g, tt);
      const std::vector<double> got = run_grad(std::move(b.g), f2);
      expect((tag + " sizes").c_str(), got.size() == want.size());
      for (size_t i = 0; i < want.size() && i < got.size(); ++i)
        expect_close((tag + " v" + std::to_string(i)).c_str(), got[i], want[i]);
    }
  }
}

// Gap 1 (docs/superpowers/plans/2026-09-11-lane-layout-unification.md): a
// partial row read that lowering hands reroll as one OP_GATHER per lane
// (`m[i, 2:K]` on a column-major parameter matrix, a fixed row alongside a
// range on the other axis) rather than a strided window. Twelve lanes, each
// gathering two of the base's three columns skipping column 0: the twelve
// lanes' gathered offsets are, as a set, one contiguous span of the base
// (columns 1 and 2 for every row), so the fused form is one OP_SLICE.
static void test_gather_lane_partial_row() {
  const int N = 12, C = 3;
  Graph g;
  Fills fills;
  const int m = g.add_slot(N * C, true);  // column-major, a parameter
  const int mu = g.add_slot(1, true);
  const int sigma = g.add_slot(1, true);
  std::vector<int> terms;
  for (int i = 0; i < N; ++i) {
    const int w = g.add_slot(2, false);
    g.add_op(OP_GATHER, {m}, w, {i + N, i + 2 * N});
    const int lp = g.add_slot(1, false);
    const int id = g.add_op(OP_NORMAL_LPDF, {w, mu, sigma}, lp);
    g.ops[(size_t)id].variant = 0x07;
    terms.push_back(lp);
  }
  Graph ref = g;
  reduce_into_result(ref, terms);
  const std::vector<double> want = run_grad(std::move(ref), fills);

  std::vector<int> tt = terms;
  Fills f2 = fills;
  const RerollStats st = reroll(g, f2, tt, {});
  expect("gap1 regions==1", st.regions == 1);
  expect("gap1 ops==2", g.ops.size() == 2);  // SLICE + NORMAL_LPDF
  int slices = 0, gathers = 0;
  for (const Op& op : g.ops) {
    slices += op.opcode == OP_SLICE;
    gathers += op.opcode == OP_GATHER;
  }
  expect("gap1 is a slice, not a gather", slices == 1 && gathers == 0);
  expect("gap1 one term", tt.size() == 1);
  for (const Op& op : g.ops)
    if (op.opcode == OP_SLICE)
      expect("gap1 slice reads columns 1 and 2 of every row",
             op.idata[0] == N && g.slots[(size_t)op.out].len == 2 * N);
  reduce_into_result(g, tt);
  const std::vector<double> got = run_grad(std::move(g), f2);
  expect("gap1 sizes", got.size() == want.size());
  for (size_t i = 0; i < want.size() && i < got.size(); ++i)
    expect_close(("gap1 v" + std::to_string(i)).c_str(), got[i], want[i]);
}

// Gap 2 (docs/superpowers/plans/2026-09-11-lane-layout-unification.md): a
// vector operand shared by every lane, as wide as the row itself
// (`y[i,:] ~ normal(mu, s)` with `mu` a parameter vector). Row-major
// storage (a plain OP_SLICE row read): mu tiles tail-to-tail, mode
// {width, count, 1}, not element-repeated.
static void test_shared_vector_mean_row_major() {
  const int L = 12, C = 4;
  Graph g;
  Fills fills;
  const int base = g.add_slot(L * C, true);  // y, row-major, a parameter
  const int mu = g.add_slot(C, true);
  const int sigma = g.add_slot(1, true);
  std::vector<int> terms;
  for (int l = 0; l < L; ++l) {
    const int row = g.add_slot(C, false);
    g.add_op(OP_SLICE, {base}, row, {l * C});
    const int lp = g.add_slot(1, false);
    const int id = g.add_op(OP_NORMAL_LPDF, {row, mu, sigma}, lp);
    g.ops[(size_t)id].variant = 0x07;
    terms.push_back(lp);
  }
  Graph ref = g;
  reduce_into_result(ref, terms);
  const std::vector<double> want = run_grad(std::move(ref), fills);

  std::vector<int> tt = terms;
  Fills f2 = fills;
  const RerollStats st = reroll(g, f2, tt, {});
  expect("shared mean regions==1", st.regions == 1);
  expect("shared mean ops==2", g.ops.size() == 2);  // OP_REP_MAT + NORMAL_LPDF
  int rep_mats = 0, densities = 0;
  for (const Op& op : g.ops) {
    rep_mats += op.opcode == OP_REP_MAT;
    densities += op.opcode == OP_NORMAL_LPDF;
  }
  expect("shared mean tiles mu", rep_mats == 1 && densities == 1);
  for (const Op& op : g.ops)
    if (op.opcode == OP_REP_MAT)
      expect("shared mean tiles row-major",
             op.n_idata == 3 && op.idata[0] == C && op.idata[1] == L &&
                 op.idata[2] == 1 && op.in[0] == mu &&
                 g.slots[(size_t)op.out].len == L * C);
    else if (op.opcode == OP_NORMAL_LPDF)
      expect("shared mean density reads the base directly", op.in[0] == base);
  expect("shared mean one term", tt.size() == 1);
  reduce_into_result(g, tt);
  const std::vector<double> got = run_grad(std::move(g), f2);
  expect("shared mean sizes", got.size() == want.size());
  for (size_t i = 0; i < want.size() && i < got.size(); ++i)
    expect_close(("shared mean v" + std::to_string(i)).c_str(), got[i],
                 want[i]);
}

// ---- end to end through compile_model ------------------------------------

static std::string slurp(const char* p) {
  std::ifstream f(p);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

static std::vector<double> e2e_grad(const char* sexp, const char* json,
                                    size_t* n_ops) {
  DataMap data = DataMap::from_json(json);
  CompiledModel cm = compile_model(slurp(sexp), data);
  *n_ops = cm.graph.ops.size();
  Executor ex(std::move(cm.graph));
  cm.bind(ex);
  for (int64_t i = 0; i < ex.n_params(); ++i)
    ex.params_data()[i] = 0.1 + 0.05 * (i % 7) - 0.15 * (i % 3);
  std::vector<double> out(1 + ex.n_params());
  out[0] = ex.gradient(out.data() + 1);
  return out;
}

static void test_e2e_fixtures() {
  const char* rdata =
      "{\"N\":16,\"x\":[0.1,0.2,0.3,0.4,0.5,0.6,0.7,0.8,0.9,1.0,1.1,1.2,"
      "1.3,1.4,1.5,1.6],"
      "\"y\":[1.1,0.9,1.3,0.7,1.0,1.2,0.8,1.05,0.95,1.15,0.85,1.0,1.1,"
      "0.92,1.08,0.98]}";
  const char* adata =
      "{\"K\":2,\"T\":12,\"y\":[0.3,0.5,0.2,0.6,0.4,0.55,0.35,0.45,0.5,"
      "0.42,0.48,0.44]}";
  struct Case {
    const char* sexp;
    const char* json;
    const char* name;
  };
  const Case cases[] = {
      {"tests/fixtures/rloop.tmir.sexp", rdata, "rloop"},
      {"tests/fixtures/arloop.tmir.sexp", adata, "arloop"},
  };
  for (const Case& c : cases) {
    size_t ops_unrolled = 0, ops_rerolled = 0;
    // Both lane passes off: partition finds these same loops from their
    // delimiters, so leaving it on would compare the fused graph with itself.
    test_setenv("STANLI_NO_REROLL", "1", 1);
    test_setenv("STANLI_NO_PARTITION", "1", 1);
    const std::vector<double> want = e2e_grad(c.sexp, c.json, &ops_unrolled);
    test_unsetenv("STANLI_NO_REROLL");
    test_unsetenv("STANLI_NO_PARTITION");
    const std::vector<double> got = e2e_grad(c.sexp, c.json, &ops_rerolled);
    expect((std::string(c.name) + " sizes").c_str(), got.size() == want.size());
    for (size_t i = 0; i < want.size() && i < got.size(); ++i)
      expect_close((std::string(c.name) + " v" + std::to_string(i)).c_str(),
                   got[i], want[i]);
    std::printf("%s: %zu ops -> %zu ops\n", c.name, ops_unrolled, ops_rerolled);
    expect((std::string(c.name) + " shrinks 4x").c_str(),
           ops_rerolled * 4 <= ops_unrolled);
  }
}

// Signature prefilter: op_signature(a) != op_signature(b) must prove
// ops_match(g, a, b, *) false, checked directly against ops_match on a mix
// of handbuilt graphs covering every ops_match branch and a couple of
// compiled models.
static void test_signature_soundness() {
  const auto check = [&](const std::string& name, const Graph& g) {
    const auto result = detail::check_signature_soundness(
        g, (int64_t)detail::kMaxPeriod * detail::kMinLanes);
    expect((name + " signature sound").c_str(), result.violations == 0);
  };

  {
    ldashape::Built b = ldashape::build(24);
    check("lda", b.g);
  }
  {
    rowlanes::Built b = rowlanes::build_density(6, 4, rowlanes::Rows::kMatrix);
    check("row density matrix", b.g);
  }
  {
    rowlanes::Built b = rowlanes::build_density(6, 4, rowlanes::Rows::kArray);
    check("row density array", b.g);
  }
  {
    rowlanes::Built b = rowlanes::build_store(6, 4, rowlanes::Rows::kMatrix);
    check("row store matrix", b.g);
  }
  {
    rowlanes::Built b = rowlanes::build_store(6, 4, rowlanes::Rows::kArray);
    check("row store array", b.g);
  }
  {
    rowlanes::Built b =
        rowlanes::build_density_shared_row(6, 4, rowlanes::Rows::kMatrix);
    check("row density shared", b.g);
  }

  const char* rdata =
      "{\"N\":16,\"x\":[0.1,0.2,0.3,0.4,0.5,0.6,0.7,0.8,0.9,1.0,1.1,1.2,"
      "1.3,1.4,1.5,1.6],"
      "\"y\":[1.1,0.9,1.3,0.7,1.0,1.2,0.8,1.05,0.95,1.15,0.85,1.0,1.1,"
      "0.92,1.08,0.98]}";
  const char* adata =
      "{\"K\":2,\"T\":12,\"y\":[0.3,0.5,0.2,0.6,0.4,0.55,0.35,0.45,0.5,"
      "0.42,0.48,0.44]}";
  struct Fixture {
    const char* sexp;
    const char* json;
    const char* name;
  };
  const Fixture fixtures[] = {
      {"tests/fixtures/rloop.tmir.sexp", rdata, "rloop corpus"},
      {"tests/fixtures/arloop.tmir.sexp", adata, "arloop corpus"},
  };
  test_setenv("STANLI_NO_REROLL", "1", 1);
  for (const Fixture& f : fixtures) {
    DataMap data = DataMap::from_json(f.json);
    CompiledModel cm = compile_model(slurp(f.sexp), data);
    check(f.name, cm.graph);
    if (cm.write_array)
      check(std::string(f.name) + " write_array", cm.write_array->graph);
  }
  {
    DataMap data = DataMap::from_json_file("tests/fixtures/brmsmono.json");
    CompiledModel cm =
        compile_model(slurp("tests/fixtures/brmsmono.tmir.sexp"), data);
    check("brmsmono corpus", cm.graph);
    if (cm.write_array)
      check("brmsmono corpus write_array", cm.write_array->graph);
  }
  test_unsetenv("STANLI_NO_REROLL");
}

static bool ops_equal(const Op& a, const Op& b) {
  if (a.opcode != b.opcode || a.variant != b.variant || a.out != b.out ||
      a.out2 != b.out2 || a.n_in != b.n_in || a.n_idata != b.n_idata)
    return false;
  for (int j = 0; j < a.n_in; ++j)
    if (a.in[j] != b.in[j]) return false;
  for (int64_t k = 0; k < a.n_idata; ++k)
    if (a.idata[k] != b.idata[k]) return false;
  return true;
}

static bool graphs_equal(const Graph& a, const Graph& b) {
  if (a.ops.size() != b.ops.size()) return false;
  for (size_t u = 0; u < a.ops.size(); ++u)
    if (!ops_equal(a.ops[u], b.ops[u])) return false;
  return true;
}

// The prefilter changes how many lanes the scan reaches before ops_match is
// called, never which lanes it decides to keep: same graph, fills and
// target terms out of reroll() with STANLI_NO_REROLL_PREFILTER off and on.
static void expect_prefilter_matches(const std::string& name, const Graph& g,
                                     const Fills& fills,
                                     const std::vector<int>& terms) {
  Graph g_on = g;
  Fills fills_on = fills;
  std::vector<int> terms_on = terms;
  test_unsetenv("STANLI_NO_REROLL_PREFILTER");
  reroll(g_on, fills_on, terms_on, {});

  Graph g_off = g;
  Fills fills_off = fills;
  std::vector<int> terms_off = terms;
  test_setenv("STANLI_NO_REROLL_PREFILTER", "1", 1);
  reroll(g_off, fills_off, terms_off, {});
  test_unsetenv("STANLI_NO_REROLL_PREFILTER");

  expect((name + " prefilter ops match").c_str(), graphs_equal(g_on, g_off));
  expect((name + " prefilter terms match").c_str(), terms_on == terms_off);
  bool fills_match = fills_on.size() == fills_off.size();
  for (size_t k = 0; fills_match && k < fills_on.size(); ++k)
    fills_match = fills_on[k].first == fills_off[k].first &&
                  fills_on[k].second == fills_off[k].second;
  expect((name + " prefilter fills match").c_str(), fills_match);
}

static void test_prefilter_output_matches() {
  {
    ldashape::Built b = ldashape::build(24);
    expect_prefilter_matches("lda", b.g, b.fills, b.terms);
  }
  {
    rowlanes::Built b = rowlanes::build_density(6, 4, rowlanes::Rows::kMatrix);
    expect_prefilter_matches("row density matrix", b.g, b.fills, b.terms);
  }
  {
    rowlanes::Built b = rowlanes::build_store(6, 4, rowlanes::Rows::kArray);
    expect_prefilter_matches("row store array", b.g, b.fills, b.terms);
  }

  const char* rdata =
      "{\"N\":16,\"x\":[0.1,0.2,0.3,0.4,0.5,0.6,0.7,0.8,0.9,1.0,1.1,1.2,"
      "1.3,1.4,1.5,1.6],"
      "\"y\":[1.1,0.9,1.3,0.7,1.0,1.2,0.8,1.05,0.95,1.15,0.85,1.0,1.1,"
      "0.92,1.08,0.98]}";
  const char* adata =
      "{\"K\":2,\"T\":12,\"y\":[0.3,0.5,0.2,0.6,0.4,0.55,0.35,0.45,0.5,"
      "0.42,0.48,0.44]}";
  struct Fixture {
    const char* sexp;
    const char* json;
    const char* name;
  };
  const Fixture fixtures[] = {
      {"tests/fixtures/rloop.tmir.sexp", rdata, "rloop corpus"},
      {"tests/fixtures/arloop.tmir.sexp", adata, "arloop corpus"},
  };
  test_setenv("STANLI_NO_REROLL", "1", 1);
  for (const Fixture& f : fixtures) {
    DataMap data = DataMap::from_json(f.json);
    CompiledModel cm = compile_model(slurp(f.sexp), data);
    expect_prefilter_matches(f.name, cm.graph, cm.fills, {});
  }
  {
    DataMap data = DataMap::from_json_file("tests/fixtures/brmsmono.json");
    CompiledModel cm =
        compile_model(slurp("tests/fixtures/brmsmono.tmir.sexp"), data);
    expect_prefilter_matches("brmsmono corpus", cm.graph, cm.fills, {});
  }
  test_unsetenv("STANLI_NO_REROLL");
}

// (e) a lane output that is a graph root the pass is not told about. The
// executor reads jacobian slots and constrained-parameter views directly,
// with no consuming op, so `uses` cannot see them: a region that folds one
// away would silently stop writing a slot something still reads. Same
// graph twice, the second time declaring one lane's intermediate a root.
static void test_bail_extra_root() {
  const auto build = [](Graph& g, Fills& fills, std::vector<int>& terms) {
    const int L = 6;
    const int alpha = g.add_slot(1, true);
    const int sigma = g.add_slot(1, true);
    std::vector<int> scaled(L);
    auto cslot = [&](double v) {
      const int s = g.add_slot(1, false);
      fills.emplace_back(s, std::vector<double>{v});
      return s;
    };
    for (int l = 0; l < L; ++l) {
      scaled[l] = g.add_slot(1, false);
      g.add_op(OP_MUL, {alpha, cslot(0.3 + 0.1 * l)}, scaled[l]);
      const int lp = g.add_slot(1, false);
      const int id =
          g.add_op(OP_NORMAL_LPDF, {cslot(0.2 * l), scaled[l], sigma}, lp);
      g.ops[id].variant = 0x06;
      terms.push_back(lp);
    }
    return scaled;
  };

  {  // control: nothing else reads the intermediates, so it re-rolls
    Graph g;
    Fills fills;
    std::vector<int> tt;
    build(g, fills, tt);
    RerollStats st = reroll(g, fills, tt, {});
    expect("extra-root control rerolls", st.regions == 1);
  }
  {  // one lane's intermediate is a root: the region must not fold it away
    Graph g;
    Fills fills;
    std::vector<int> tt;
    const std::vector<int> scaled = build(g, fills, tt);
    const size_t before = g.ops.size();
    RerollStats st = reroll(g, fills, tt, {scaled[2]});
    expect("extra root not rerolled", st.regions == 0);
    expect("extra root ops unchanged", g.ops.size() == before);
  }
}

static void test_bail_categorical_ops() {
  Graph g;
  Fills fills;
  const int outcome = g.add_slot(1, false);
  const int theta = g.add_slot(3, false);
  const int mu = g.add_slot(1, true);
  const int sigma = g.add_slot(1, true);
  fills.emplace_back(outcome, std::vector<double>{2.0});
  fills.emplace_back(theta, std::vector<double>{0.2, 0.3, 0.5});
  std::vector<int> terms;
  for (int lane = 0; lane < 8; ++lane) {
    const int checked = g.add_slot(1, false);
    const int op = g.add_op(OP_CATEGORICAL, {outcome, theta}, checked);
    g.ops[(size_t)op].variant = kCategoricalScalarOutcome;
    const int y = g.add_slot(1, false);
    fills.emplace_back(y, std::vector<double>{0.1 * lane});
    const int lp = g.add_slot(1, false);
    const int density = g.add_op(OP_NORMAL_LPDF, {y, mu, sigma}, lp);
    g.ops[(size_t)density].variant = 0x06;
    terms.push_back(lp);
  }
  const size_t before = g.ops.size();
  RerollStats st = reroll(g, fills, terms, {});
  expect("categorical ops are not rerolled", st.regions == 0);
  expect("categorical ops survive", g.ops.size() == before);
  expect("categorical terms survive", terms.size() == 8);
}

static void test_bail_message_ops() {
  for (uint16_t opcode : {OP_PRINT, OP_REJECT}) {
    Graph g;
    Fills fills;
    const int mu = g.add_slot(1, true);
    const int sigma = g.add_slot(1, true);
    auto spec = std::make_shared<MessageSpec>();
    spec->chunks = {"mu = ", ""};
    std::vector<int> terms;
    for (int lane = 0; lane < 8; ++lane) {
      const int dead = g.add_slot(1, false);
      const int msg = g.add_op(opcode, {mu}, dead);
      g.ops[(size_t)msg].udata = spec.get();
      const int y = g.add_slot(1, false);
      fills.emplace_back(y, std::vector<double>{0.1 * lane});
      const int lp = g.add_slot(1, false);
      const int density = g.add_op(OP_NORMAL_LPDF, {y, mu, sigma}, lp);
      g.ops[(size_t)density].variant = 0x06;
      terms.push_back(lp);
    }
    g.udata_pool.push_back(std::move(spec));

    const size_t before = g.ops.size();
    RerollStats st = reroll(g, fills, terms, {});
    const char* name = opcode_name(opcode);
    expect(name, st.regions == 0);
    expect(name, g.ops.size() == before);
    expect(name, terms.size() == 8);
    int kept = 0;
    for (const Op& op : g.ops) kept += op.opcode == opcode;
    expect(name, kept == 8);
  }
}

// Pricing is a read-only gate, not permission inferred from the lane count.
// In particular, the four-lane cancellation fixture must retain scalar
// accumulation while a larger distinct-lane region may use the vector plan.
struct ProvenDistinctHashSource : stanli::detail::reroll_plan::GraphSource {
  static constexpr bool kDistinctLaneHashes = true;
};

static void test_proven_lane_pricing() {
  namespace rp = stanli::detail::reroll_plan;
  // This synthetic source has the same one-varying-field proof as symbolic
  // lowering. Virtual slot IDs near INT_MAX stress identity arithmetic without
  // allocating those graph slots. Pricing must not dereference them.
  Graph g;
  g.add_slot(1, true);
  g.add_slot(1, true);
  for (uint16_t anchor : {OP_FMA, OP_ADD, OP_MUL}) {
    const int arity = anchor == OP_FMA ? 3 : 2;
    for (int lanes : {1, 3, 4, 5, 6, 7, 8, 16, 33, 4096}) {
      for (int period : {1, 2, 4}) {
        for (int varying = 0; varying < arity; ++varying) {
          for (bool near_limit : {false, true}) {
            const int stride = period + 1;
            const int first = near_limit ? INT_MAX - lanes * stride : 2;
            rp::CandidatePlan plan;
            plan.lanes = lanes;
            plan.positions.resize(period);
            for (int p = 0; p < period; ++p) {
              auto& pos = plan.positions[p];
              pos.ins.resize(p == 0 ? arity : 1);
              for (int j = 0; j < (p == 0 ? arity : 1); ++j) {
                pos.ins[j].kind = p              ? rp::InKind::kLaneLocal
                                  : j == varying ? rp::InKind::kConstLanes
                                                 : rp::InKind::kInvariant;
              }
            }
            plan.positions.back().term_widen = true;
            std::vector<Op> ops;
            for (int lane = 0; lane < lanes; ++lane) {
              const int read = first + lane * stride;
              for (int p = 0; p < period; ++p) {
                Op op;
                op.opcode = p ? OP_EXPV : anchor;
                op.n_in = p ? 1 : arity;
                for (int j = 0; j < op.n_in; ++j)
                  op.in[j] = p ? read + p : j == varying ? read : j % 2;
                op.out = read + p + 1;
                ops.push_back(op);
              }
            }
            const rp::GraphSource hashed{ops.data(), period};
            ProvenDistinctHashSource proven;
            proven.ops = ops.data();
            proven.period = period;
            expect("proven unique hashes preserve exact lane selection",
                   rp::profitable(g, plan, hashed) ==
                       rp::profitable(g, plan, proven));
          }
        }
      }
    }
  }
}

static void test_lane_plan_boundary() {
  namespace rp = stanli::detail::reroll_plan;
  for (int lanes : {4, 8}) {
    for (bool repeated : {false, true}) {
      Graph g;
      Fills fills;
      const int a = g.add_slot(1, true);
      const int b = g.add_slot(1, true);
      const int prefix = g.add_slot(1, false);
      const int suffix = g.add_slot(1, false);
      fills.emplace_back(prefix, std::vector<double>{10});
      fills.emplace_back(suffix, std::vector<double>{20});
      std::vector<int> terms{prefix};
      int shared = -1;
      rp::CandidatePlan plan;
      plan.lanes = lanes;
      plan.positions.resize(2);
      auto& fma = plan.positions[0];
      fma.ins.resize(3);
      fma.ins[0].kind = rp::InKind::kInvariant;
      fma.ins[1].kind = rp::InKind::kConstLanes;
      fma.ins[2].kind = rp::InKind::kInvariant;
      auto& exp = plan.positions[1];
      exp.ins.resize(1);
      exp.ins[0].kind = rp::InKind::kLaneLocal;
      exp.ins[0].producer_pos = 0;
      exp.term_widen = true;
      for (int lane = 0; lane < lanes; ++lane) {
        const double value = repeated ? 0.2 : 0.2 + 0.1 * lane;
        int y = shared;
        if (!repeated || y < 0) {
          y = g.add_slot(1, false);
          fills.emplace_back(y, std::vector<double>{value});
          shared = y;
        }
        fma.ins[1].values.push_back(value);
        const int x = g.add_slot(1, false);
        g.add_op(OP_FMA, {b, y, a}, x);
        const int out = g.add_slot(1, false);
        g.add_op(OP_EXPV, {x}, out);
        terms.push_back(out);
      }
      terms.push_back(suffix);
      const Graph before = g;
      const Fills old_fills = fills;
      const std::vector<int> old_terms = terms;
      const rp::GraphSource source{g.ops.data(), 2};
      const bool selected = rp::profitable(g, plan, source);
      expect("lane pricing respects margin and CSE",
             selected == (lanes == 8 && !repeated));
      expect("lane pricing leaves ops unchanged", graphs_equal(g, before));
      expect("lane pricing leaves allocation unchanged",
             g.slots.size() == before.slots.size() &&
                 g.idata_pool.size() == before.idata_pool.size() &&
                 g.udata_pool.size() == before.udata_pool.size());
      expect("lane pricing leaves fills/targets unchanged",
             fills == old_fills && terms == old_terms);
      auto accepted =
          rp::SelectedPlan<rp::GraphSource>::select(g, plan, source);
      expect("selection follows unchanged price",
             accepted.has_value() == selected);
      if (!selected) {
        expect("rejected selection retains candidate for retry",
               plan.positions.size() == 2 &&
                   plan.positions[0].ins[1].values.size() == (size_t)lanes);
        continue;
      }

      std::unordered_set<int> term_set(terms.begin(), terms.end());
      std::unordered_map<int, int> renamed;
      std::vector<Op> emitted;
      std::move(*accepted).emit(g, fills, terms, term_set, emitted, renamed);
      expect("lane emitter keeps operation order",
             emitted.size() == 3 && emitted[0].opcode == OP_FMA &&
                 emitted[1].opcode == OP_EXPV &&
                 emitted[2].opcode == OP_SUM_VEC);
      expect("lane emitter preserves target boundaries",
             terms == std::vector<int>({prefix, emitted[2].out, suffix}));
      expect("lane emitter keeps source stable", graphs_equal(g, before));
      expect("lane emitter updates target membership",
             term_set == std::unordered_set<int>(terms.begin(), terms.end()));
      expect("lane emitter owns packed constants",
             fills.back().second ==
                     accepted->description().positions[0].ins[1].values &&
                 g.slots[emitted[0].out].len == lanes &&
                 g.slots[emitted[1].out].len == lanes &&
                 g.slots[emitted[2].out].len == 1);
    }
  }
}

int main() {
  test_proven_lane_pricing();
  test_lane_plan_boundary();
  test_scalar_density_traits();
  test_scalar_math_widenable_traits();
  test_radon_shape();
  test_ark_shape();
  test_bail_recurrence();
  test_gauss_mix_shape();
  test_row_mixture_shape();
  test_row_mixture_shape_column_major();
  test_logistic_elt_shape();
  test_elt_lpmf_shape();
  test_elt_scalar_density_hoists();
  test_bail_cross_lane_density();
  test_bail_escaping_density();
  test_partial_range_slices();
  test_data_index_gathers();
  test_cost_model_declines_small_gather();
  test_env_disable();
  test_first_lane_anomalous();
  test_block_structured();
  test_bail_extra_root();
  test_bail_categorical_ops();
  test_bail_message_ops();
  test_lda_shape_gradients();
  ldashape::test_lda_shape_refusals();
  test_lda_shape_cost();
  test_no_candidate_cost();
  test_sparse_candidate_cost();
  test_write_fusion();
  test_write_fusion_bails();
  test_post_reroll_slice_inplace();
  test_write_fusion_scalar_chain();
  test_pow_widens();
  test_row_density_lanes();
  test_row_store_lanes();
  test_row_lanes_bail();
  test_row_lane_shared_read();
  test_gather_lane_partial_row();
  test_shared_vector_mean_row_major();
  test_e2e_fixtures();
  test_signature_soundness();
  test_prefilter_output_matches();
  if (failures) {
    std::printf("%d failures\n", failures);
    return 1;
  }
  std::printf("test_reroll OK\n");
  return 0;
}
