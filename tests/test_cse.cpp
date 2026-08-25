// Common-subexpression elimination: identical pure ops collapse to one, and
// the values the graph computes do not move.
#include "env_helpers.hpp"
#include "graph_helpers.hpp"
#include <stanli/cse.hpp>
#include <stanli/graph.hpp>
#include <stanli/optable.hpp>

#include <cmath>
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

// exp(p) computed twice, both results consumed.
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
  expect("one op removed", st.ops_removed == 1);
  expect("one EXP left", count_opcode(g, OP_EXP) == 1);
  expect("the use is rewritten", g.ops[1].in[0] == e1 && g.ops[1].in[1] == e1);
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
  expect("duplicate term op removed", st.ops_removed == 1);
  expect("both terms name the survivor",
         terms.size() == 2 && terms[0] == t1 && terms[1] == t1);
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
  expect("both levels collapse", st.ops_removed == 2);
  expect("three ops left", g.ops.size() == 3);
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
  test_idata_distinguishes();
  test_refuses_mutated_slot();
  test_chain_dedup();
  test_keeps_roots();
  test_env_disable();
  if (failures == 0) std::printf("test_cse OK\n");
  return failures == 0 ? 0 : 1;
}
