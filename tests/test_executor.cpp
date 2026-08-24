// Executor forward/backward on a tiny graph: lp = exp(a) + b.
// Gradient must be {exp(a), 1.0}, value exp(a) + b, both bitwise.
#include <stanli/graph.hpp>
#include <stanli/optable.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>

static int failures = 0;
static void expect_eq(const char* what, double got, double want) {
  if (got != want) {
    ++failures;
    std::printf("FAIL %-12s got %.17g want %.17g\n", what, got, want);
  }
}

int main() {
  using namespace stanli;

  Graph g;
  const int a = g.add_slot(1, /*is_param=*/true);
  const int b = g.add_slot(1, /*is_param=*/true);
  const int ea = g.add_slot(1, false);
  const int lp = g.add_slot(1, false);
  g.add_op(OP_EXP, {a}, ea);
  g.add_op(OP_ADD_N, {ea, b}, lp);
  g.result_slot = lp;

  Executor ex(std::move(g));
  if (ex.n_params() != 2) {
    std::printf("FAIL n_params got %lld want 2\n", (long long)ex.n_params());
    return 1;
  }
  *ex.param_ptr(a) = 0.3;
  *ex.param_ptr(b) = -1.1;

  expect_eq("forward", ex.forward(), std::exp(0.3) + -1.1);

  // This graph has no kernel whose double instantiation changes its value,
  // and a value-only pass must not disturb a gradient taken afterwards.
  // That second half is the safety argument for the mode: gradient() runs
  // its own full forward, so nothing a value-only sweep skipped can survive
  // into a reverse sweep.
  expect_eq("forward_value_only", ex.forward_value_only(),
            std::exp(0.3) + -1.1);

  double grad[2] = {0, 0};
  const double v = ex.gradient(grad);
  expect_eq("grad value", v, std::exp(0.3) + -1.1);
  expect_eq("d/da", grad[0], std::exp(0.3));
  expect_eq("d/db", grad[1], 1.0);

  // ... and again with a value-only sweep in between.
  ex.forward_value_only();
  double grad2[2] = {0, 0};
  expect_eq("grad after value-only", ex.gradient(grad2), std::exp(0.3) + -1.1);
  expect_eq("d/da after value-only", grad2[0], grad[0]);
  expect_eq("d/db after value-only", grad2[1], grad[1]);

  // A large data slot between active slots used to occupy the adjoint arena
  // and get cleared on every gradient even though kernels correctly receive
  // a null adjoint for it. The private adjoint layout must contain only the
  // parameter, the indexed scalar, and the result -- not the million data
  // values. Repeating after changing both inputs catches stale compact cells.
  Graph sparse;
  const int sx = sparse.add_slot(1, true);
  const int sd = sparse.add_slot(INT64_C(1) << 20, false);
  const int sp = sparse.add_slot(1, false);
  const int slp = sparse.add_slot(1, false);
  sparse.add_op(OP_INDEX, {sd}, sp, {17});
  sparse.add_op(OP_MUL, {sx, sp}, slp);
  sparse.result_slot = slp;
  Executor sparse_ex(std::move(sparse));
  if (sparse_ex.adjoint_storage_size() != 3) {
    ++failures;
    std::printf("FAIL compact adjoints got %lld want 3\n",
                (long long)sparse_ex.adjoint_storage_size());
  }
  *sparse_ex.param_ptr(sx) = 2.0;
  sparse_ex.value_ptr(sd)[17] = 3.5;
  double sparse_grad[1] = {0};
  expect_eq("sparse value", sparse_ex.gradient(sparse_grad), 7.0);
  expect_eq("sparse grad", sparse_grad[0], 3.5);
  *sparse_ex.param_ptr(sx) = -4.0;
  sparse_ex.value_ptr(sd)[17] = -1.25;
  expect_eq("sparse value 2", sparse_ex.gradient(sparse_grad), 5.0);
  expect_eq("sparse grad 2", sparse_grad[0], -1.25);

  // BCAST_FMA forward: out[i] = a + b * x[i].
  Graph g2;
  const int s_a = g2.add_slot(1, true);
  const int s_b = g2.add_slot(1, true);
  const int s_x = g2.add_slot(3, false);
  const int s_o = g2.add_slot(3, false);
  g2.add_op(OP_BCAST_FMA, {s_a, s_b, s_x}, s_o);
  g2.result_slot = s_o;  // vector result: forward-only check via value_ptr
  Executor ex2(std::move(g2));
  *ex2.param_ptr(s_a) = 0.5;
  *ex2.param_ptr(s_b) = 2.0;
  double* x = ex2.value_ptr(s_x);
  x[0] = 1.0;
  x[1] = -2.0;
  x[2] = 0.25;
  ex2.run_forward_only();
  const double* o = ex2.value_ptr(s_o);
  expect_eq("fma[0]", o[0], 0.5 + 2.0 * 1.0);
  expect_eq("fma[1]", o[1], 0.5 + 2.0 * -2.0);
  expect_eq("fma[2]", o[2], 0.5 + 2.0 * 0.25);

  // A forward-only graph need not designate a scalar result. Binding still
  // compacts its written adjoints without indexing result_slot == -1.
  Graph no_result;
  const int nr_in = no_result.add_slot(1, false);
  const int nr_out = no_result.add_slot(1, false);
  no_result.add_op(OP_EXP, {nr_in}, nr_out);
  Executor nr_ex(std::move(no_result));
  *nr_ex.value_ptr(nr_in) = -0.4;
  nr_ex.run_forward_only();
  expect_eq("no-result forward", *nr_ex.value_ptr(nr_out), std::exp(-0.4));

  // A zero-op constant graph still needs one seed cell even with no
  // parameters or written slots.
  Graph constant;
  const int constant_result = constant.add_slot(1, false);
  constant.result_slot = constant_result;
  Executor constant_ex(std::move(constant));
  *constant_ex.value_ptr(constant_result) = 2.75;
  double no_grad = 0.0;
  expect_eq("constant result", constant_ex.gradient(&no_grad), 2.75);
  if (constant_ex.adjoint_storage_size() != 1) {
    ++failures;
    std::printf("FAIL constant adjoints got %lld want 1\n",
                (long long)constant_ex.adjoint_storage_size());
  }

  if (failures == 0) std::printf("test_executor OK\n");
  return failures == 0 ? 0 : 1;
}
