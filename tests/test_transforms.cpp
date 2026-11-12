// Constraint transform ops: constrained value, log-jacobian, and gradient
// (through both the constrained value and the jacobian) vs stan-math's
// *_constrain var path.
#include "expect.hpp"

#include <stanli/graph.hpp>
#include <stanli/optable.hpp>

#include <stan/math.hpp>
#include <cstdio>
#include <limits>
#include <string>

// Graph: lp = sum(constrained) + jac, via OP_SUM_VEC then OP_ADD_N.
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

// lub with one shared and one per-element bound. Besides covering the two
// mixed-shape overloads, `reuse_shared` gives the scalar bound an adjoint
// before the constrain callback runs. Stan Math reduces the constrain terms
// first and adds that reduction once; adding them directly to a nonzero lane
// rounds differently at adversarial points.
static void run_mixed_lu_case(const std::string& tag, int n, const double* x0,
                              int nlb, const double* lbv, int nub,
                              const double* ubv, bool jacobian,
                              bool reuse_shared) {
  using namespace stanli;
  using stan::math::var;

  Graph g;
  const int x = g.add_slot(n, true);
  const int lb = g.add_slot(nlb, true);
  const int ub = g.add_slot(nub, true);
  const int con = g.add_slot(n, false);
  const int jac = g.add_slot(1, false);
  const int s = g.add_slot(1, false);
  const int lp = g.add_slot(1, false);
  {
    Op op;
    op.opcode = OP_CONSTRAIN_LU;
    op.out = con;
    op.out2 = jac;
    op.n_in = 3;
    op.in[0] = x;
    op.in[1] = lb;
    op.in[2] = ub;
    g.ops.push_back(op);
  }
  g.add_op(OP_SUM_VEC, {con}, s);
  const int shared = nlb == 1 ? lb : ub;
  if (jacobian && reuse_shared)
    g.add_op(OP_ADD_N, {s, jac, shared}, lp);
  else if (jacobian)
    g.add_op(OP_ADD_N, {s, jac}, lp);
  else if (reuse_shared)
    g.add_op(OP_ADD_N, {s, shared}, lp);
  else
    g.add_op(OP_ADD_N, {s}, lp);
  g.result_slot = lp;

  Executor ex(std::move(g));
  for (int i = 0; i < n; ++i) ex.param_ptr(x)[i] = x0[i];
  for (int i = 0; i < nlb; ++i) ex.param_ptr(lb)[i] = lbv[i];
  for (int i = 0; i < nub; ++i) ex.param_ptr(ub)[i] = ubv[i];
  std::vector<double> grad(n + nlb + nub);
  const double got = ex.gradient(grad.data());

  Eigen::Matrix<var, -1, 1> vx(n), vlb(nlb), vub(nub);
  for (int i = 0; i < n; ++i) vx(i) = x0[i];
  for (int i = 0; i < nlb; ++i) vlb(i) = lbv[i];
  for (int i = 0; i < nub; ++i) vub(i) = ubv[i];
  var vjac = 0.0;
  Eigen::Matrix<var, -1, 1> vcon;
  if (nlb == 1)
    vcon = jacobian ? stan::math::lub_constrain<true>(vx, vlb(0), vub, vjac)
                    : stan::math::lub_constrain(vx, vlb(0), vub);
  else
    vcon = jacobian ? stan::math::lub_constrain<true>(vx, vlb, vub(0), vjac)
                    : stan::math::lub_constrain(vx, vlb, vub(0));
  var vlp = stan::math::sum(vcon);
  if (jacobian) vlp += vjac;
  if (reuse_shared) vlp += nlb == 1 ? vlb(0) : vub(0);
  vlp.grad();

  expect_eq(tag + " lp", got, vlp.val());
  for (int i = 0; i < n; ++i)
    expect_eq(tag + " gx" + std::to_string(i), grad[i], vx(i).adj());
  for (int i = 0; i < nlb; ++i)
    expect_eq(tag + " glb" + std::to_string(i), grad[n + i], vlb(i).adj());
  for (int i = 0; i < nub; ++i)
    expect_eq(tag + " gub" + std::to_string(i), grad[n + nlb + i],
              vub(i).adj());
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

  // Infinite bounds. Every *_constrain overload reads one as no bound at
  // all and hands the element back untouched, adding no jacobian term; the
  // kernels used to exponentiate through it and return inf, which reached
  // lp as -inf. A constant bound is the `real<lower=negative_infinity()>`
  // and `vector<lower=lb>[N]` declaration shape.
  const double ninf = -std::numeric_limits<double>::infinity();
  const double pinf = std::numeric_limits<double>::infinity();
  run_case("lower ninf vec", stanli::OP_CONSTRAIN_LOWER, 3, xs, ninf, 0.0);
  run_case("lower ninf scalar", stanli::OP_CONSTRAIN_LOWER, 1, x1, ninf, 0.0);
  run_case("upper pinf vec", stanli::OP_CONSTRAIN_UPPER, 3, xs, pinf, 0.0);
  run_case("upper pinf scalar", stanli::OP_CONSTRAIN_UPPER, 1, x1, pinf, 0.0);
  // lub resolves per element: lower infinite alone is the upper transform,
  // upper infinite alone the lower one, and both together the identity.
  run_case("lu ninf lower", stanli::OP_CONSTRAIN_LU, 3, xs, ninf, 2.0);
  run_case("lu pinf upper", stanli::OP_CONSTRAIN_LU, 3, xs, -1.0, pinf);
  run_case("lu both inf", stanli::OP_CONSTRAIN_LU, 3, xs, ninf, pinf);
  run_case("lu both inf scalar", stanli::OP_CONSTRAIN_LU, 1, x1, ninf, pinf);

  // The same through parameter-valued bounds, where an infinite bound also
  // has an adjoint lane: having taken no part in the value, it must collect
  // nothing. The per-element vectors mix infinite and finite entries, which
  // is what a `vector<lower=lb>[N]` with one negative_infinity() declares --
  // the finite entries still have to transform.
  const double x4[4] = {0.3, -1.2, 2.0, 0.45};
  const double ninf_1[1] = {ninf};
  const double pinf_1[1] = {pinf};
  const double lo_inf3[3] = {ninf, 0.75, -2.0};
  const double hi_inf3[3] = {3.25, pinf, 1.5};
  // Lower-only infinite, upper-only infinite, both, neither -- all four
  // element branches of lub in one call.
  const double lo_mix4[4] = {ninf, -0.5, ninf, 0.75};
  const double hi_mix4[4] = {3.25, pinf, pinf, 4.5};
  for (bool jacobian : {true, false}) {
    const std::string j = jacobian ? " jac" : " nojac";
    run_bound_case("lower ninf shared" + j, stanli::OP_CONSTRAIN_LOWER, 3, xs,
                   1, ninf_1, b2_1, jacobian);
    run_bound_case("lower ninf per-element" + j, stanli::OP_CONSTRAIN_LOWER, 3,
                   xs, 3, lo_inf3, b2_3, jacobian);
    run_bound_case("upper pinf shared" + j, stanli::OP_CONSTRAIN_UPPER, 3, xs,
                   1, pinf_1, b1_1, jacobian);
    run_bound_case("upper pinf per-element" + j, stanli::OP_CONSTRAIN_UPPER, 3,
                   xs, 3, hi_inf3, b1_3, jacobian);
    run_bound_case("lu ninf shared" + j, stanli::OP_CONSTRAIN_LU, 3, xs, 1,
                   ninf_1, b2_1, jacobian);
    run_bound_case("lu pinf shared" + j, stanli::OP_CONSTRAIN_LU, 3, xs, 1,
                   b1_1, pinf_1, jacobian);
    run_bound_case("lu both inf shared" + j, stanli::OP_CONSTRAIN_LU, 3, xs, 1,
                   ninf_1, pinf_1, jacobian);
    run_bound_case("lu mixed inf" + j, stanli::OP_CONSTRAIN_LU, 4, x4, 4,
                   lo_mix4, hi_mix4, jacobian);
  }

  // One scalar bound and one vector bound dispatch different Stan Math
  // overloads from the both-scalar and both-vector cases above. Reusing the
  // scalar in the objective makes its preexisting adjoint nonzero and pins
  // the overload's reduce-then-add rounding order.
  const double xr[3] = {-21.218105551082793, 7.4625322261737495,
                        -24.25541517136952};
  const double lo_shared[1] = {-0.5};
  const double hi_mixed[3] = {pinf, 4.5, 1.5};
  const double lo_mixed[3] = {ninf, 0.75, -2.0};
  const double hi_shared[1] = {3.25};
  for (bool jacobian : {true, false}) {
    const std::string j = jacobian ? " jac" : " nojac";
    run_mixed_lu_case("lu scalar lower mixed inf" + j, 3, xr, 1, lo_shared, 3,
                      hi_mixed, jacobian, true);
    run_mixed_lu_case("lu scalar upper mixed inf" + j, 3, xr, 3, lo_mixed, 1,
                      hi_shared, jacobian, true);
  }

  return finish("test_transforms");
}
