// Constraint transform ops: constrained value, log-jacobian, and gradient
// (through both the constrained value and the jacobian) vs stan-math's
// *_constrain var path.
#include <stanli/graph.hpp>
#include <stanli/optable.hpp>

#include <stan/math.hpp>
#include <cstdio>
#include <string>

static int failures = 0;
static void expect_eq(const std::string& what, double got, double want) {
  if (got != want) {
    ++failures;
    std::printf("FAIL %-20s got %.17g want %.17g\n", what.c_str(), got, want);
  }
}

// Graph: lp = sum(constrained) + jac  (sum via dot with ones through
// NORMAL-free ops: use OP_ADD_N over a 1-vector? constrained is a vector;
// use OP_DOT against a ones data vector once Task 4 lands. Here: scalar
// case uses ADD_N directly; vector case multiplies into a normal_lpdf-free
// path via BCAST_FMA trick is overkill, so vector case feeds
// OP_CONSTRAIN_* out into OP_SUM_VEC. OP_SUM_VEC arrives with Task 4; to
// keep Task 3 self-contained it is declared there but implemented here.)
static void run_case(const std::string& tag, uint16_t opcode, int n,
                     const double* x0, double lb, double ub) {
  using namespace stanli;
  using stan::math::var;

  Graph g;
  const int x = g.add_slot(n, true);
  const int b1 = g.add_slot(1, false);
  const int b2 = g.add_slot(1, false);
  const int con = g.add_slot(n, false);
  const int jac = g.add_slot(1, false);
  const int s = g.add_slot(1, false);
  const int lp = g.add_slot(1, false);
  {
    Op op;
    op.opcode = opcode;
    op.out = con;
    op.out2 = jac;
    op.n_in = 0;
    op.in[op.n_in++] = x;
    op.in[op.n_in++] = b1;
    if (opcode == OP_CONSTRAIN_LU) op.in[op.n_in++] = b2;
    g.ops.push_back(op);
  }
  g.add_op(OP_SUM_VEC, {con}, s);
  g.add_op(OP_ADD_N, {s, jac}, lp);
  g.result_slot = lp;

  Executor ex(std::move(g));
  for (int i = 0; i < n; ++i) ex.param_ptr(x)[i] = x0[i];
  ex.value_ptr(b1)[0] = lb;
  ex.value_ptr(b2)[0] = ub;
  double grad[8];
  const double got = ex.gradient(grad);

  // Var reference: lp_ref = sum(constrain(x)) + jac, via *_constrain<true>.
  Eigen::Matrix<var, -1, 1> vx(n);
  for (int i = 0; i < n; ++i) vx(i) = x0[i];
  var vjac = 0.0;
  Eigen::Matrix<var, -1, 1> vcon;
  if (opcode == OP_CONSTRAIN_LOWER) {
    vcon = stan::math::lb_constrain<true>(vx, lb, vjac);
  } else if (opcode == OP_CONSTRAIN_UPPER) {
    vcon = stan::math::ub_constrain<true>(vx, lb, vjac);
  } else {
    vcon = stan::math::lub_constrain<true>(vx, lb, ub, vjac);
  }
  var vlp = stan::math::sum(vcon) + vjac;
  vlp.grad();

  expect_eq(tag + " lp", got, vlp.val());
  for (int i = 0; i < n; ++i)
    expect_eq(tag + " g" + std::to_string(i), grad[i], vx(i).adj());

  // out2 carries the Jacobian adjoint. A second reverse sweep must start
  // from a clean compact arena just like an ordinary single-output op.
  double grad2[8];
  const double got2 = ex.gradient(grad2);
  expect_eq(tag + " lp repeat", got2, got);
  for (int i = 0; i < n; ++i)
    expect_eq(tag + " g repeat" + std::to_string(i), grad2[i], grad[i]);
  stan::math::recover_memory();
}

// The same three kernels with the bounds themselves as parameters, at both
// bound widths. A bound with one value per element is what
// `vector<lower=vlb>[N]` declares and what `lower_bound_constrain(v, vlb)`
// passes; the kernels used to read element 0 of it for every element, so a
// vector-bounded declaration silently used its first bound everywhere.
// `jacobian` chooses between the with-lp and no-lp overloads: the callable
// `*_constrain` leaves the jacobian output unrooted, which is the same
// thing as seeding its adjoint with zero.
static void run_bound_case(const std::string& tag, uint16_t opcode, int n,
                           const double* x0, int nb, const double* b1v,
                           const double* b2v, bool jacobian) {
  using namespace stanli;
  using stan::math::var;
  const bool lu = opcode == OP_CONSTRAIN_LU;

  Graph g;
  const int x = g.add_slot(n, true);
  const int b1 = g.add_slot(nb, true);
  const int b2 = g.add_slot(lu ? nb : 1, true);
  const int con = g.add_slot(n, false);
  const int jac = g.add_slot(1, false);
  const int s = g.add_slot(1, false);
  const int lp = g.add_slot(1, false);
  {
    Op op;
    op.opcode = opcode;
    op.out = con;
    op.out2 = jac;
    op.n_in = 0;
    op.in[op.n_in++] = x;
    op.in[op.n_in++] = b1;
    if (lu) op.in[op.n_in++] = b2;
    g.ops.push_back(op);
  }
  g.add_op(OP_SUM_VEC, {con}, s);
  // Without the jacobian the sum IS the result, which is what leaving the
  // op's second output unrooted comes to in a lowered model.
  if (jacobian)
    g.add_op(OP_ADD_N, {s, jac}, lp);
  else
    g.add_op(OP_ADD_N, {s}, lp);
  g.result_slot = lp;

  Executor ex(std::move(g));
  for (int i = 0; i < n; ++i) ex.param_ptr(x)[i] = x0[i];
  for (int i = 0; i < nb; ++i) ex.param_ptr(b1)[i] = b1v[i];
  for (int i = 0; i < (lu ? nb : 1); ++i) ex.param_ptr(b2)[i] = b2v[i];
  double grad[16];
  const double got = ex.gradient(grad);

  Eigen::Matrix<var, -1, 1> vx(n), vb1(nb), vb2(nb);
  for (int i = 0; i < n; ++i) vx(i) = x0[i];
  for (int i = 0; i < nb; ++i) vb1(i) = b1v[i];
  for (int i = 0; i < nb; ++i) vb2(i) = b2v[i];
  var vjac = 0.0;
  Eigen::Matrix<var, -1, 1> vcon;
  // stan-math spells a shared bound as a scalar overload and a per-element
  // one as a matrix overload; the kernel's 0/1 stride has to reproduce both.
  const auto apply = [&](auto&& lo, auto&& hi) {
    if (opcode == OP_CONSTRAIN_LOWER)
      vcon = jacobian ? stan::math::lb_constrain<true>(vx, lo, vjac)
                      : stan::math::lb_constrain(vx, lo);
    else if (opcode == OP_CONSTRAIN_UPPER)
      vcon = jacobian ? stan::math::ub_constrain<true>(vx, lo, vjac)
                      : stan::math::ub_constrain(vx, lo);
    else
      vcon = jacobian ? stan::math::lub_constrain<true>(vx, lo, hi, vjac)
                      : stan::math::lub_constrain(vx, lo, hi);
  };
  if (nb == 1)
    apply(vb1(0), vb2(0));
  else
    apply(vb1, vb2);
  var vlp = jacobian ? stan::math::sum(vcon) + vjac : stan::math::sum(vcon);
  vlp.grad();

  expect_eq(tag + " lp", got, vlp.val());
  for (int i = 0; i < n; ++i)
    expect_eq(tag + " gx" + std::to_string(i), grad[i], vx(i).adj());
  for (int i = 0; i < nb; ++i)
    expect_eq(tag + " gb1_" + std::to_string(i), grad[n + i], vb1(i).adj());
  if (lu)
    for (int i = 0; i < nb; ++i)
      expect_eq(tag + " gb2_" + std::to_string(i), grad[n + nb + i],
                vb2(i).adj());
  stan::math::recover_memory();
}

static void test_out2_is_result() {
  using namespace stanli;
  Graph g;
  const int x = g.add_slot(1, true);
  const int lb = g.add_slot(1, false);
  const int constrained = g.add_slot(1, false);
  const int jac = g.add_slot(1, false);
  Op op;
  op.opcode = OP_CONSTRAIN_LOWER;
  op.out = constrained;
  op.out2 = jac;
  op.n_in = 2;
  op.in[0] = x;
  op.in[1] = lb;
  g.ops.push_back(op);
  g.result_slot = jac;

  Executor ex(std::move(g));
  *ex.param_ptr(x) = 0.7;
  *ex.value_ptr(lb) = -2.0;
  double grad = 0.0;
  expect_eq("out2 result lp", ex.gradient(&grad), 0.7);
  expect_eq("out2 result grad", grad, 1.0);
}

int main() {
  const double xs[3] = {0.3, -1.2, 2.0};
  const double x1[1] = {0.7};
  run_case("lower vec", stanli::OP_CONSTRAIN_LOWER, 3, xs, 0.0, 0.0);
  run_case("lower scalar", stanli::OP_CONSTRAIN_LOWER, 1, x1, 2.5, 0.0);
  run_case("upper vec", stanli::OP_CONSTRAIN_UPPER, 3, xs, 1.5, 0.0);
  run_case("lu vec", stanli::OP_CONSTRAIN_LU, 3, xs, -1.0, 2.0);
  run_case("lu scalar", stanli::OP_CONSTRAIN_LU, 1, x1, 0.0, 1.0);
  test_out2_is_result();

  // Parameter-dependent bounds, shared and per-element, with and without
  // the jacobian.
  const double b1_1[1] = {-0.5};
  const double b2_1[1] = {3.25};
  const double b1_3[3] = {-0.5, 0.75, -2.0};
  const double b2_3[3] = {3.25, 4.5, 1.5};
  for (bool jacobian : {true, false}) {
    const std::string j = jacobian ? " jac" : " nojac";
    for (int nb : {1, 3}) {
      const std::string w = nb == 1 ? " shared" : " per-element";
      const double* lo = nb == 1 ? b1_1 : b1_3;
      const double* hi = nb == 1 ? b2_1 : b2_3;
      run_bound_case("lower" + w + j, stanli::OP_CONSTRAIN_LOWER, 3, xs, nb, lo,
                     hi, jacobian);
      run_bound_case("upper" + w + j, stanli::OP_CONSTRAIN_UPPER, 3, xs, nb, hi,
                     lo, jacobian);
      run_bound_case("lu" + w + j, stanli::OP_CONSTRAIN_LU, 3, xs, nb, lo, hi,
                     jacobian);
    }
    // A length-1 value takes stan-math's scalar overloads on both sides.
    run_bound_case("lower scalar" + j, stanli::OP_CONSTRAIN_LOWER, 1, x1, 1,
                   b1_1, b2_1, jacobian);
    run_bound_case("lu scalar" + j, stanli::OP_CONSTRAIN_LU, 1, x1, 1, b1_1,
                   b2_1, jacobian);
  }

  if (failures == 0) std::printf("test_transforms OK\n");
  return failures == 0 ? 0 : 1;
}
