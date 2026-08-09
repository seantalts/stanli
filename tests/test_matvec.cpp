// OP_MATVEC: out = X * beta with X data (row-major), hand-written vjp.
// lp = normal_lpdf(y | X*beta, 1.0); gradient vs var path, bitwise.
#include <stanli/graph.hpp>
#include <stanli/optable.hpp>

#include <stan/math.hpp>
#include <cstdio>
#include <string>

static int failures = 0;
static void expect_eq(const std::string& what, double got, double want) {
  if (got != want) {
    ++failures;
    std::printf("FAIL %-16s got %.17g want %.17g\n", what.c_str(), got, want);
  }
}

int main() {
  using namespace stanli;
  using stan::math::var;
  const int R = 5, C = 3;
  // Column-major X (Stan/Eigen convention), same logical matrix as before.
  double X[R * C] = {0.5, 2.0,  -0.4, 0.2, 1.3,  -1.2, -0.7, 0.9,
                     0.8, -0.1, 0.3,  1.1, -1.5, -0.6, 0.7};
  double yv[R] = {0.4, -1.0, 2.1, 0.3, -0.8};
  double betav[C] = {0.25, -0.5, 1.0};

  Graph g;
  const int beta = g.add_slot(C, true);
  const int Xs = g.add_slot(R * C, false);
  const int ys = g.add_slot(R, false);
  const int one = g.add_slot(1, false);
  const int eta = g.add_slot(R, false);
  const int lp = g.add_slot(1, false);
  g.add_op(OP_MATVEC, {Xs, beta}, eta, {R, C});
  g.add_op(OP_NORMAL_LPDF, {ys, eta, one}, lp);
  g.result_slot = lp;

  Executor ex(std::move(g));
  for (int i = 0; i < R * C; ++i) ex.value_ptr(Xs)[i] = X[i];
  for (int i = 0; i < R; ++i) ex.value_ptr(ys)[i] = yv[i];
  ex.value_ptr(one)[0] = 1.0;
  for (int i = 0; i < C; ++i) ex.param_ptr(beta)[i] = betav[i];

  double grad[C] = {0, 0, 0};
  const double v = ex.gradient(grad);

  // Var reference: same shapes, same op order. Column-major map for X.
  Eigen::Map<Eigen::MatrixXd> Xm(X, R, C);
  Eigen::Matrix<var, -1, 1> vb(C);
  for (int i = 0; i < C; ++i) vb(i) = betav[i];
  Eigen::Matrix<var, -1, 1> veta = Xm * vb;
  Eigen::Map<Eigen::VectorXd> ym(yv, R);
  var vlp = stan::math::normal_lpdf<false>(ym, veta, 1.0);
  vlp.grad();

  expect_eq("value", v, vlp.val());
  for (int i = 0; i < C; ++i)
    expect_eq("dbeta" + std::to_string(i), grad[i], vb(i).adj());

  if (failures == 0) std::printf("test_matvec OK\n");
  return failures == 0 ? 0 : 1;
}
