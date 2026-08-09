// Constant folding: ops no parameter reaches run once at lowering and become
// data, without changing any value the surviving graph computes.
//
// The pass had no dedicated unit test when it landed (the corpus A/B and the
// CmdStan verifications covered it); these pin its three load-bearing
// properties directly:
//   * a pure-data subgraph folds, and the folded value is what the ops
//     would have computed,
//   * a slot that a surviving op reads MID-CHAIN -- before a later constant
//     op overwrites it -- is refused, since folding keeps only the final
//     contents (the destructive update chains make this reachable),
//   * STANLI_NO_CONSTFOLD=1 disables the pass.
#include "env_helpers.hpp"
#include "graph_helpers.hpp"
#include <stanli/constfold.hpp>
#include <stanli/graph.hpp>
#include <stanli/optable.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
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
  if (!(rel < 1e-14)) {
    ++failures;
    std::printf("FAIL %-30s got %.17g want %.17g rel %.2e\n", what, got, want,
                rel);
  }
}

using namespace stanli;
using stanli::testutil::Fills;

static double fill_at(int64_t i) { return 0.3 + 0.2 * (i % 2); }
static std::vector<double> run_grad(Graph g, const Fills& fills) {
  return testutil::run_grad(std::move(g), fills, fill_at);
}

// p * (a + b) with a, b data: the ADD folds, the MUL survives.
static void test_folds_data_arithmetic() {
  Graph g;
  Fills fills;
  const int p = g.add_slot(1, true);
  const int a = g.add_slot(1, false), b = g.add_slot(1, false);
  fills.emplace_back(a, std::vector<double>{2.0});
  fills.emplace_back(b, std::vector<double>{3.0});
  const int ab = g.add_slot(1, false);
  g.add_op(OP_ADD, {a, b}, ab);
  const int m = g.add_slot(1, false);
  g.add_op(OP_MUL, {p, ab}, m);
  g.result_slot = m;

  Graph ref = g;
  const std::vector<double> want = run_grad(std::move(ref), fills);

  Fills f2 = fills;
  ConstFoldStats st = const_fold(g, f2, {m});
  expect("one op folded", st.ops_removed == 1);
  expect("one slot filled", st.slots_folded == 1);
  bool have_fill = false;
  for (const auto& f : f2)
    if (f.first == ab && f.second.size() == 1 && f.second[0] == 5.0)
      have_fill = true;
  expect("folded slot carries 2+3", have_fill);
  const std::vector<double> got = run_grad(std::move(g), f2);
  expect_close("value", got[0], want[0]);
  expect_close("gradient", got[1], want[1]);
}

// A surviving op reads the chain's buffer between two constant writes.
// Folding would hand it the FINAL contents; the pass must refuse.
static void test_refuses_midchain_reader() {
  Graph g;
  Fills fills;
  const int p = g.add_slot(1, true);
  const int v = g.add_slot(2, false);
  fills.emplace_back(v, std::vector<double>{0.0, 0.0});
  const int c1 = g.add_slot(1, false), c2 = g.add_slot(1, false);
  fills.emplace_back(c1, std::vector<double>{4.0});
  fills.emplace_back(c2, std::vector<double>{9.0});
  // write v[0] = 4 (constant, in place)
  g.add_op(OP_SET_INDEX_INPLACE, {v, c1}, v, {0});
  // a PARAMETER op reads v now: v = [4, 0]
  const int d = g.add_slot(2, false);
  g.add_op(OP_MUL, {p, v}, d);
  // then a constant write lands v[1] = 9, so v's final contents are [4, 9]
  g.add_op(OP_SET_INDEX_INPLACE, {v, c2}, v, {1});
  // and the final v feeds the result too, through the parameter
  const int e = g.add_slot(2, false);
  g.add_op(OP_MUL, {p, v}, e);
  const int s1 = g.add_slot(1, false), s2 = g.add_slot(1, false);
  g.add_op(OP_SUM_VEC, {d}, s1);
  g.add_op(OP_SUM_VEC, {e}, s2);
  const int r = g.add_slot(1, false);
  g.add_op(OP_ADD_N, {s1, s2}, r);
  g.result_slot = r;

  Graph ref = g;
  const std::vector<double> want = run_grad(std::move(ref), fills);
  // want = p*4 + p*(4+9): the mid-chain reader sees [4, 0], not [4, 9].

  Fills f2 = fills;
  const_fold(g, f2, {r});
  const std::vector<double> got = run_grad(std::move(g), f2);
  expect_close("mid-chain value", got[0], want[0]);
  expect_close("mid-chain gradient", got[1], want[1]);
}

static void test_env_disable() {
  test_setenv("STANLI_NO_CONSTFOLD", "1", 1);
  Graph g;
  Fills fills;
  const int a = g.add_slot(1, false), b = g.add_slot(1, false);
  fills.emplace_back(a, std::vector<double>{1.0});
  fills.emplace_back(b, std::vector<double>{2.0});
  const int ab = g.add_slot(1, false);
  g.add_op(OP_ADD, {a, b}, ab);
  g.result_slot = ab;
  ConstFoldStats st = const_fold(g, fills, {ab});
  expect("disabled: nothing folded", st.ops_removed == 0);
  test_unsetenv("STANLI_NO_CONSTFOLD");
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
  test_folds_data_arithmetic();
  test_refuses_midchain_reader();
  test_env_disable();
  if (failures == 0) std::printf("test_constfold OK\n");
  return failures == 0 ? 0 : 1;
}
