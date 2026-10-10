// Common-subexpression elimination shares pure values while retaining each
// active source pullback and its floating-point accumulation order.
#include "env_helpers.hpp"
#include "graph_helpers.hpp"
#include <stanli/cse.hpp>
#include <stanli/graph.hpp>
#include <stanli/optable.hpp>
#include <stan/math.hpp>

#include <cmath>
#include <cstring>
#include <limits>
#include <cstdio>
#include <memory>
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

static double fill_at(int64_t i) { return 0.3 + 0.2 * (i % 2); }
static std::vector<double> run_grad(Graph g, const Fills& fills) {
  return testutil::run_grad(std::move(g), fills, fill_at);
}

static void expect_same_values(const char* what, const std::vector<double>& got,
                               const std::vector<double>& want) {
  bool ok = got.size() == want.size();
  for (size_t i = 0; ok && i < got.size(); ++i)
    ok = std::abs(got[i] - want[i]) <= 1e-13 * std::max(1.0, std::abs(want[i]));
  if (!ok) {
    ++failures;
    std::printf("FAIL %s\n", what);
    for (size_t i = 0; i < want.size(); ++i)
      std::printf("  [%zu] got %.17g want %.17g\n", i,
                  i < got.size() ? got[i] : 0.0, want[i]);
  }
}

static int count_opcode(const Graph& g, uint16_t oc) {
  int n = 0;
  for (const Op& op : g.ops) n += op.opcode == oc;
  return n;
}

// exp(p) computed once, but both source pullbacks remain independent.
static void test_merges_identical_ops() {
  Graph g;
  Fills fills;
  const int p = g.add_slot(1, true);
  const int e1 = g.add_slot(1, false), e2 = g.add_slot(1, false);
  g.add_op(OP_EXP, {p}, e1);
  g.add_op(OP_EXP, {p}, e2);
  const int s = g.add_slot(1, false);
  g.add_op(OP_ADD, {e1, e2}, s);
  g.result_slot = s;

  Graph ref = g;
  const std::vector<double> want = run_grad(std::move(ref), fills);

  std::vector<int> terms;
  const CseStats st = cse(g, fills, terms, {});
  expect("one primal shared", st.primals_shared == 1 && st.ops_removed == 0);
  expect("two EXP pullbacks", count_opcode(g, OP_EXP) == 2);
  expect("adjoint identities retained",
         g.ops[2].in[0] == e1 && g.ops[2].in[1] == e2);
  Executor storage(g);
  expect("shared value storage",
         storage.value_ptr(e1) == storage.value_ptr(e2));
  Executor clone(storage);
  expect("clone retains value sharing",
         clone.value_ptr(e1) == clone.value_ptr(e2));
  expect("clone has independent storage",
         clone.value_ptr(e1) != storage.value_ptr(e1));
  expect_same_values("value and gradient unchanged",
                     run_grad(std::move(g), fills), want);
}

// Target terms are the whole win on the M*_model family: the duplicated ops
// there have no op consumer at all.
static void test_rewrites_target_terms() {
  Graph g;
  Fills fills;
  const int p = g.add_slot(1, true);
  const int t1 = g.add_slot(1, false), t2 = g.add_slot(1, false);
  g.add_op(OP_EXP, {p}, t1);
  g.add_op(OP_EXP, {p}, t2);
  std::vector<int> terms{t1, t2};
  const CseStats st = cse(g, fills, terms, {});
  expect("duplicate term primal shared", st.primals_shared == 1);
  expect("terms keep separate adjoints",
         terms.size() == 2 && terms[0] == t1 && terms[1] == t2);
}

// Effects are graph semantics: two prints print twice.
static void test_keeps_effectful_ops() {
  const uint16_t effectful[] = {OP_PRINT,
                                OP_REJECT,
                                OP_RNG,
                                OP_CHECK_STRUCTURED,
                                OP_CHECK_MATCHING_DIMS,
                                OP_CHECK_LOWER,
                                OP_CHECK_UPPER};
  for (uint16_t oc : effectful) {
    Graph g;
    Fills fills;
    const int p = g.add_slot(1, true);
    const int a = g.add_slot(1, false), b = g.add_slot(1, false);
    g.add_op(oc, {p}, a);
    g.add_op(oc, {p}, b);
    std::vector<int> terms;
    const CseStats st = cse(g, fills, terms, {});
    std::string what = std::string(opcode_name(oc)) + " never merges";
    expect(what.c_str(), st.ops_removed == 0 && g.ops.size() == 2);
  }
}

// A compiled region or solver never merges, even without a udata payload.
static void test_never_merges_solver_ops() {
  const uint16_t solver_ops[] = {OP_DAE, OP_ODE_ADJOINT};
  for (uint16_t oc : solver_ops) {
    Graph g;
    Fills fills;
    const int p = g.add_slot(1, true);
    const int a = g.add_slot(1, false), b = g.add_slot(1, false);
    g.add_op(oc, {p}, a);
    g.add_op(oc, {p}, b);
    std::vector<int> terms;
    const CseStats st = cse(g, fills, terms, {});
    std::string what = std::string(opcode_name(oc)) + " never merges";
    expect(what.c_str(), st.ops_removed == 0 && g.ops.size() == 2);
  }
}

// Same opcode, same inputs, different immediates: different elements.
static void test_idata_distinguishes() {
  Graph g;
  Fills fills;
  const int v = g.add_slot(3, true);
  const int a = g.add_slot(1, false), b = g.add_slot(1, false);
  g.add_op(OP_INDEX, {v}, a, {0});
  g.add_op(OP_INDEX, {v}, b, {1});
  const int s = g.add_slot(1, false);
  g.add_op(OP_ADD, {a, b}, s);
  g.result_slot = s;

  std::vector<int> terms;
  const CseStats st = cse(g, fills, terms, {});
  expect("differing idata does not merge", st.ops_removed == 0);
  expect("both reads survive", count_opcode(g, OP_INDEX) == 2);
}

// A destructive store rewrites the vector between the two reads, so the
// producer of the pre-store version may not stand in for the post-store one.
static void test_refuses_mutated_slot() {
  Graph g;
  Fills fills;
  const int p = g.add_slot(1, true);
  const int base = g.add_slot(2, false);
  fills.emplace_back(base, std::vector<double>{0.0, 0.0});
  const int vec = g.add_slot(2, false);
  g.add_op(OP_ADD, {base, base}, vec);  // a fresh, op-written buffer
  const int r1 = g.add_slot(1, false);
  g.add_op(OP_INDEX, {vec}, r1, {0});
  g.add_op(OP_SET_INDEX_INPLACE, {vec, p}, vec, {0});
  const int r2 = g.add_slot(1, false);
  g.add_op(OP_INDEX, {vec}, r2, {0});
  const int s = g.add_slot(1, false);
  g.add_op(OP_ADD, {r1, r2}, s);
  g.result_slot = s;

  Graph ref = g;
  const std::vector<double> want = run_grad(std::move(ref), fills);

  std::vector<int> terms;
  const CseStats st = cse(g, fills, terms, {});
  expect("read across a mutation does not merge", st.ops_removed == 0);
  expect_same_values("mutation case unchanged", run_grad(std::move(g), fills),
                     want);
}

// (a + b) twice, then (a + b) * c twice: both levels collapse in one pass.
static void test_chain_dedup() {
  Graph g;
  Fills fills;
  const int a = g.add_slot(1, true), b = g.add_slot(1, true);
  const int c = g.add_slot(1, true);
  const int s1 = g.add_slot(1, false), s2 = g.add_slot(1, false);
  g.add_op(OP_ADD, {a, b}, s1);
  g.add_op(OP_ADD, {a, b}, s2);
  const int m1 = g.add_slot(1, false), m2 = g.add_slot(1, false);
  g.add_op(OP_MUL, {s1, c}, m1);
  g.add_op(OP_MUL, {s2, c}, m2);
  const int r = g.add_slot(1, false);
  g.add_op(OP_ADD, {m1, m2}, r);
  g.result_slot = r;

  Graph ref = g;
  const std::vector<double> want = run_grad(std::move(ref), fills);

  std::vector<int> terms;
  const CseStats st = cse(g, fills, terms, {});
  expect("both levels share primals", st.primals_shared == 2);
  expect("all pullbacks remain", g.ops.size() == 5);
  expect_same_values("chain case unchanged", run_grad(std::move(g), fills),
                     want);
}

// A slot the executor reads straight out of the arena keeps its writer.
static void test_keeps_roots() {
  Graph g;
  Fills fills;
  const int p = g.add_slot(1, true);
  const int e1 = g.add_slot(1, false), e2 = g.add_slot(1, false);
  g.add_op(OP_EXP, {p}, e1);
  g.add_op(OP_EXP, {p}, e2);
  g.result_slot = e1;
  std::vector<int> terms;
  const CseStats st = cse(g, fills, terms, {e2});
  expect("root output is not merged away", st.ops_removed == 0);
}

static void test_env_disable() {
  test_setenv("STANLI_NO_CSE", "1", 1);
  Graph g;
  Fills fills;
  const int p = g.add_slot(1, true);
  const int e1 = g.add_slot(1, false), e2 = g.add_slot(1, false);
  g.add_op(OP_EXP, {p}, e1);
  g.add_op(OP_EXP, {p}, e2);
  const int s = g.add_slot(1, false);
  g.add_op(OP_ADD, {e1, e2}, s);
  g.result_slot = s;
  std::vector<int> terms;
  const CseStats st = cse(g, fills, terms, {});
  test_unsetenv("STANLI_NO_CSE");
  expect("disabled by env", st.ops_removed == 0 && g.ops.size() == 3);
}

static void test_gradient_accumulation_order() {
  // Merging the three identical multiplications changes a cancellation from
  // repeated scaled contributions into one scaled subtotal. Their values
  // may share storage; their adjoints must not share an accumulator.
  for (double input : {0.3, -0.71, 1.13}) {
    Graph g;
    Fills fills;
    const int p = g.add_slot(1, true), c = g.add_slot(1, false);
    fills.emplace_back(c, std::vector<double>{1.1});
    const double weights[] = {1e16, -1e16, 1.0};
    std::vector<int> terms;
    for (double weight : weights) {
      const int w = g.add_slot(1, false);
      fills.emplace_back(w, std::vector<double>{weight});
      const int product = g.add_slot(1, false), term = g.add_slot(1, false);
      g.add_op(OP_MUL, {p, c}, product);
      g.add_op(OP_MUL, {product, w}, term);
      terms.push_back(term);
    }
    const int result = g.add_slot(1, false);
    g.add_op(OP_ADD_N, {terms[0], terms[1], terms[2]}, result);
    g.result_slot = result;
    expect("cancellation primals shared",
           cse(g, fills, terms, {}).primals_shared == 2);
    Executor ex(std::move(g));
    for (const auto& fill : fills)
      ex.set_values(fill.first, fill.second.data(), fill.second.size());
    ex.params_data()[0] = input;
    stan::math::nested_rev_autodiff scope;
    stan::math::var x = input, sum = 0;
    for (double weight : weights) sum += (x * 1.1) * weight;
    stan::math::grad(sum.vi_);
    double gradient;
    expect("cancellation value exact", ex.gradient(&gradient) == sum.val());
    expect("cancellation gradient exact", gradient == x.adj());
    ex.gradient(&gradient);
    expect("repeated gradient exact", gradient == x.adj());
    ex.set_profile(true);
    expect("profiled cached value exact", ex.gradient(&gradient) == sum.val());
    expect("profiled cached gradient exact", gradient == x.adj());
    // The report is empty until some op has measured a nonzero time, and
    // one pass over a graph this small can fall inside a single tick of a
    // coarse clock (seen on Windows). Accumulate until the clock has moved.
    for (int pass = 0; pass < 100000 && ex.profile_report().empty(); ++pass)
      ex.gradient(&gradient);
    expect("cached profile is populated", !ex.profile_report().empty());
    ex.set_profile(false);
    Executor clone(ex);
    expect("cloned cached value exact", clone.gradient(&gradient) == sum.val());
    expect("cloned cached gradient exact", gradient == x.adj());
  }
}

struct SharedBackwardGraph {
  Graph g;
  Fills fills;
  int n_dups = 3;
};

static SharedBackwardGraph build_shared_backward_graph() {
  SharedBackwardGraph sb;
  Graph& g = sb.g;
  const int p0 = g.add_slot(1, true), p1 = g.add_slot(1, true);
  const int p2 = g.add_slot(1, true), pv = g.add_slot(3, true);
  const int k3 = g.add_slot(1, false);
  sb.fills.emplace_back(k3, std::vector<double>{3.0});
  const int hi = g.add_slot(1, false), theta = g.add_slot(1, false);
  g.add_op(OP_ADD, {p0, k3}, hi);
  g.add_op(OP_INV_LOGIT, {p2}, theta);
  const double weights[] = {1.0, -3.7e5, 0.31, 7.9e-3};
  std::vector<int> w;
  for (double x : weights) {
    w.push_back(g.add_slot(1, false));
    sb.fills.emplace_back(w.back(), std::vector<double>{x});
  }
  std::vector<int> terms;
  const auto dup = [&](auto&& emit) {
    std::vector<int> outs;
    for (int d = 0; d < sb.n_dups; ++d) {
      outs.push_back(g.add_slot(1, false));
      emit(outs.back());
    }
    for (int d = 0; d < sb.n_dups; ++d) {
      const int term = g.add_slot(1, false);
      g.add_op(OP_MUL, {outs[d], w[d]}, term);
      terms.push_back(term);
    }
  };
  dup([&](int o) { g.add_op(OP_INDEX, {pv}, o, {1}); });
  dup([&](int o) { g.add_op(OP_ADD, {p0, p1}, o); });
  dup([&](int o) { g.add_op(OP_SUB, {p0, p1}, o); });
  dup([&](int o) { g.add_op(OP_NEG, {p1}, o); });
  dup([&](int o) { g.add_op(OP_MUL, {p0, p1}, o); });
  dup([&](int o) { g.add_op(OP_EXP, {p1}, o); });
  dup([&](int o) { g.add_op(OP_INV_LOGIT, {p1}, o); });
  dup([&](int o) { g.add_op(OP_LOG1M, {theta}, o); });
  dup([&](int o) { g.add_op(OP_LOGV, {theta}, o); });
  dup([&](int o) { g.add_op(OP_SQUARE, {p1}, o); });
  dup([&](int o) { g.add_op(OP_ABS, {p1}, o); });
  dup([&](int o) { g.add_op(OP_LOG_INV_LOGIT, {p1}, o); });
  dup([&](int o) { g.add_op(OP_LSE2, {p0, p1}, o); });
  dup([&](int o) { g.add_op(OP_LOG_DIFF_EXP, {hi, p0}, o); });
  dup([&](int o) { g.add_op(OP_LOG_MIX, {theta, p0, p1}, o); });
  for (uint8_t variant : {uint8_t{0}, uint8_t{0x81}}) {
    dup([&](int o) {
      g.ops[g.add_op(OP_BERNOULLI_LPMF, {theta}, o, {1})].variant = variant;
    });
    dup([&](int o) {
      g.ops[g.add_op(OP_BINOMIAL_LPMF, {theta}, o, {1, 3, 1, 10})].variant =
          variant;
    });
    dup([&](int o) {
      g.ops[g.add_op(OP_POISSON_LPMF, {hi}, o, {4})].variant = variant;
    });
  }
  testutil::reduce_into_result(g, terms);
  return sb;
}

struct Evaluation {
  bool threw = false;
  std::vector<double> out;
};

static Evaluation evaluate_at(const Graph& g, const Fills& fills,
                              const std::vector<double>& point) {
  Evaluation e;
  try {
    Executor ex(g);
    for (const auto& f : fills)
      ex.set_values(f.first, f.second.data(), f.second.size());
    for (size_t i = 0; i < point.size(); ++i) ex.params_data()[i] = point[i];
    e.out.assign(1 + (size_t)ex.n_params(), 0.0);
    e.out[0] = ex.gradient(e.out.data() + 1);
  } catch (const std::exception&) {
    e.threw = true;
  }
  return e;
}

static bool same_bits(const Evaluation& a, const Evaluation& b) {
  if (a.threw != b.threw || a.out.size() != b.out.size()) return false;
  for (size_t i = 0; i < a.out.size(); ++i)
    if (std::memcmp(&a.out[i], &b.out[i], sizeof(double)) != 0) return false;
  return true;
}

// Every duplicate keeps its own seed and accumulation order, so the shared
// backward must reproduce the unmerged graph bit for bit, including at
// points where a kernel throws or a gradient is non-finite.
static void test_shared_backward_bitwise() {
  SharedBackwardGraph sb = build_shared_backward_graph();
  Graph merged = sb.g;
  std::vector<int> terms;
  const CseStats st = cse(merged, sb.fills, terms, {});
  expect("shared primals found",
         st.primals_shared >= 40 && st.ops_removed == 0);
  expect("graph keeps every pullback", merged.ops.size() == sb.g.ops.size());
  {
    Executor ex(merged);
    expect("duplicate backwards are fused", ex.fused_backward_ops() >= 40);
  }
  const double inf = std::numeric_limits<double>::infinity();
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const std::vector<std::vector<double>> points = {
      {0.3, 0.5, 0.7, 0.1, -0.4, 0.9}, {-1.3, 2.1, -0.2, 3.0, 0.5, -2.0},
      {0.0, 0.0, 0.0, 0.0, 0.0, 0.0},  {40.0, -40.0, 30.0, 1e-300, 1e300, 0.5},
      {1e3, 1e3, -1e3, 0.0, 0.0, 0.0}, {nan, 0.5, 0.2, 0.1, 0.2, 0.3},
      {0.3, nan, 0.2, 0.1, 0.2, 0.3},  {0.3, 0.5, nan, 0.1, 0.2, 0.3},
      {inf, 0.5, 0.2, 0.1, 0.2, 0.3},  {0.3, -inf, 0.2, 0.1, 0.2, 0.3},
      {0.3, 0.5, inf, 0.1, 0.2, 0.3},
  };
  int n_threw = 0, n_finite = 0;
  for (size_t k = 0; k < points.size(); ++k) {
    const Evaluation want = evaluate_at(sb.g, sb.fills, points[k]);
    const Evaluation got = evaluate_at(merged, sb.fills, points[k]);
    n_threw += got.threw;
    n_finite += !got.threw && std::isfinite(got.out[0]);
    const std::string what =
        "shared backward bitwise, point " + std::to_string(k);
    expect(what.c_str(), same_bits(got, want));
  }
  expect("some points evaluate", n_finite >= 3);
}

static bool close_values(const std::vector<double>& got,
                         const std::vector<double>& want, double tol) {
  if (got.size() != want.size()) return false;
  for (size_t i = 0; i < got.size(); ++i) {
    if (std::isfinite(want[i]) != std::isfinite(got[i])) return false;
    if (std::isfinite(want[i]) &&
        std::abs(got[i] - want[i]) > tol * std::max(1.0, std::abs(want[i])))
      return false;
  }
  return true;
}

static void test_fast_merges_active_duplicates() {
  Graph g;
  Fills fills;
  const int p = g.add_slot(1, true);
  const int e1 = g.add_slot(1, false), e2 = g.add_slot(1, false);
  g.add_op(OP_EXP, {p}, e1);
  g.add_op(OP_EXP, {p}, e2);
  const int s = g.add_slot(1, false);
  g.add_op(OP_ADD, {e1, e2}, s);
  g.result_slot = s;

  Graph ref = g;
  const std::vector<double> want = run_grad(std::move(ref), fills);

  std::vector<int> terms;
  Graph off = g;
  const CseStats st_off = cse(off, fills, terms, {}, false);
  expect("explicit off shares primal only", st_off.primals_shared == 1 &&
                                                st_off.ops_removed == 0 &&
                                                count_opcode(off, OP_EXP) == 2);

  const CseStats st = cse(g, fills, terms, {}, true);
  expect("fast removes the duplicate", st.ops_removed == 1);
  expect("fast shares no primal", st.primals_shared == 0);
  expect("one EXP remains", count_opcode(g, OP_EXP) == 1);
  expect("both ADD inputs are the survivor",
         g.ops.back().in[0] == e1 && g.ops.back().in[1] == e1);
  expect_same_values("fast value and gradient", run_grad(std::move(g), fills),
                     want);
}

static void test_fast_merge_target_terms() {
  Graph g;
  Fills fills;
  const int p = g.add_slot(1, true);
  const int t1 = g.add_slot(1, false), t2 = g.add_slot(1, false);
  g.add_op(OP_EXP, {p}, t1);
  g.add_op(OP_EXP, {p}, t2);
  std::vector<int> terms{t1, t2};
  Graph ref = g;
  testutil::reduce_into_result(ref, terms);
  const std::vector<double> want = run_grad(std::move(ref), fills);

  const CseStats st = cse(g, fills, terms, {}, true);
  expect("duplicate term removed", st.ops_removed == 1 && g.ops.size() == 1);
  expect("both terms name the survivor",
         terms.size() == 2 && terms[0] == t1 && terms[1] == t1);
  testutil::reduce_into_result(g, terms);
  expect_same_values("fast term gradient", run_grad(std::move(g), fills), want);
}

static void test_fast_merge_chain() {
  Graph g;
  Fills fills;
  const int a = g.add_slot(1, true), b = g.add_slot(1, true);
  const int c = g.add_slot(1, true);
  const int s1 = g.add_slot(1, false), s2 = g.add_slot(1, false);
  g.add_op(OP_ADD, {a, b}, s1);
  g.add_op(OP_ADD, {a, b}, s2);
  const int m1 = g.add_slot(1, false), m2 = g.add_slot(1, false);
  g.add_op(OP_MUL, {s1, c}, m1);
  g.add_op(OP_MUL, {s2, c}, m2);
  const int r = g.add_slot(1, false);
  g.add_op(OP_ADD, {m1, m2}, r);
  g.result_slot = r;

  Graph ref = g;
  const std::vector<double> want = run_grad(std::move(ref), fills);
  std::vector<int> terms;
  const CseStats st = cse(g, fills, terms, {}, true);
  expect("both levels merge", st.ops_removed == 2 && g.ops.size() == 3);
  expect_same_values("fast chain gradient", run_grad(std::move(g), fills),
                     want);
}

static void test_fast_keeps_active_roots_and_effects() {
  Graph g;
  Fills fills;
  const int p = g.add_slot(1, true);
  const int e1 = g.add_slot(1, false), e2 = g.add_slot(1, false);
  g.add_op(OP_EXP, {p}, e1);
  g.add_op(OP_EXP, {p}, e2);
  g.result_slot = e1;
  std::vector<int> terms;
  const CseStats st = cse(g, fills, terms, {e2}, true);
  expect("fast keeps a root", st.ops_removed == 0);

  for (uint16_t oc : {OP_PRINT, OP_REJECT, OP_CHECK_LOWER}) {
    Graph h;
    const int q = h.add_slot(1, true);
    const int a = h.add_slot(1, false), b = h.add_slot(1, false);
    h.add_op(oc, {q}, a);
    h.add_op(oc, {q}, b);
    std::vector<int> none;
    const CseStats hs = cse(h, fills, none, {}, true);
    std::string what = std::string(opcode_name(oc)) + " never merges in fast";
    expect(what.c_str(), hs.ops_removed == 0 && h.ops.size() == 2);
  }
}

static void test_fast_merge_many_kernels() {
  SharedBackwardGraph sb = build_shared_backward_graph();
  Graph merged = sb.g;
  std::vector<int> terms;
  const CseStats st = cse(merged, sb.fills, terms, {}, true);
  expect("fast removes the duplicates",
         st.ops_removed >= 40 && st.primals_shared == 0);
  expect("graph shrinks by the removed count",
         merged.ops.size() == sb.g.ops.size() - (size_t)st.ops_removed);
  const std::vector<std::vector<double>> points = {
      {0.3, 0.5, 0.7, 0.1, -0.4, 0.9},
      {-1.3, 2.1, -0.2, 3.0, 0.5, -2.0},
      {0.0, 0.0, 0.0, 0.0, 0.0, 0.0},
      {1e3, 1e3, -1e3, 0.0, 0.0, 0.0},
  };
  for (size_t k = 0; k < points.size(); ++k) {
    const Evaluation want = evaluate_at(sb.g, sb.fills, points[k]);
    const Evaluation got = evaluate_at(merged, sb.fills, points[k]);
    const std::string what =
        "fast merge matches default, point " + std::to_string(k);
    expect(what.c_str(),
           got.threw == want.threw && close_values(got.out, want.out, 1e-12));
  }
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
  test_merges_identical_ops();
  test_rewrites_target_terms();
  test_keeps_effectful_ops();
  test_never_merges_solver_ops();
  test_idata_distinguishes();
  test_refuses_mutated_slot();
  test_chain_dedup();
  test_keeps_roots();
  test_env_disable();
  test_gradient_accumulation_order();
  test_shared_backward_bitwise();
  test_fast_merges_active_duplicates();
  test_fast_merge_target_terms();
  test_fast_merge_chain();
  test_fast_keeps_active_roots_and_effects();
  test_fast_merge_many_kernels();
  if (failures == 0) std::printf("test_cse OK\n");
  return failures == 0 ? 0 : 1;
}
