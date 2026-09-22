// OP_MATVEC: out = X * beta with X data (column-major).
// lp = normal_lpdf(y | X*beta, 1.0); gradient vs the var path, <= 10 ULP.
#include <stanli/graph.hpp>
#include <stanli/optable.hpp>

#include <stan/math.hpp>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int failures = 0;

static int64_t ulp_key(double d) {
  int64_t i;
  std::memcpy(&i, &d, sizeof(i));
  return i < 0 ? (-(int64_t(1) << 63)) - i : i;
}
static int64_t ulp_distance(double a, double b) {
  const int64_t k = ulp_key(a) - ulp_key(b);
  return k < 0 ? -k : k;
}
static void expect_ulp(const std::string& what, double got, double want,
                       int64_t budget) {
  const int64_t d = ulp_distance(got, want);
  if (d > budget) {
    ++failures;
    std::printf("FAIL %-16s got %.17g want %.17g (%lld ulp)\n", what.c_str(),
                got, want, (long long)d);
  }
}

static void run_case(const std::string& name, int R, int C,
                     const std::vector<double>& X, const std::vector<double>& y,
                     const std::vector<double>& beta, int64_t budget) {
  using namespace stanli;
  using stan::math::var;

  Graph g;
  const int b = g.add_slot(C, true);
  const int Xs = g.add_slot(R * C, false);
  const int ys = g.add_slot(R, false);
  const int one = g.add_slot(1, false);
  const int eta = g.add_slot(R, false);
  const int lp = g.add_slot(1, false);
  g.add_op(OP_MATVEC, {Xs, b}, eta, {R, C});
  g.add_op(OP_NORMAL_LPDF, {ys, eta, one}, lp);
  g.result_slot = lp;

  Executor ex(std::move(g));
  for (int i = 0; i < R * C; ++i) ex.value_ptr(Xs)[i] = X[i];
  for (int i = 0; i < R; ++i) ex.value_ptr(ys)[i] = y[i];
  ex.value_ptr(one)[0] = 1.0;
  for (int i = 0; i < C; ++i) ex.param_ptr(b)[i] = beta[i];

  std::vector<double> grad(C, 0.0);
  const double v = ex.gradient(grad.data());

  Eigen::Map<const Eigen::MatrixXd> Xm(X.data(), R, C);
  Eigen::Matrix<var, -1, 1> vb(C);
  for (int i = 0; i < C; ++i) vb(i) = beta[i];
  Eigen::Matrix<var, -1, 1> veta = stan::math::multiply(Xm, vb);
  Eigen::Map<const Eigen::VectorXd> ym(y.data(), R);
  var vlp = stan::math::normal_lpdf<false>(ym, veta, 1.0);
  vlp.grad();

  expect_ulp(name + " value", v, vlp.val(), budget);
  for (int i = 0; i < C; ++i)
    expect_ulp(name + " dbeta" + std::to_string(i), grad[i], vb(i).adj(),
               budget);
  stan::math::recover_memory();
}

// Nonuniform seeds and cancellation distinguish Stan's scalar adjoint view
// from a contiguous-double matrix product. Exercise both native product
// opcodes with the same upstream multiply expression.
static void cancellation_case(int rows, int cols, int outputs) {
  using namespace stanli;
  using stan::math::var;
  Eigen::MatrixXd x(rows, cols), seeds(rows, outputs);
  Eigen::Matrix<var, -1, -1> b(cols, outputs);
  for (int j = 0; j < cols; ++j)
    for (int i = 0; i < rows; ++i)
      x(i, j) = (i < rows / 2 ? 0.5 : -0.5) * (1.0 + 0.01 * j);
  for (int j = 0; j < outputs; ++j) {
    for (int i = 0; i < rows; ++i)
      seeds(i, j) = 0.7 + 0.013 * ((i * 7 + j * 3) % 11);
    for (int i = 0; i < cols; ++i) b(i, j) = 0.1 + 0.03 * i;
  }
  Eigen::Matrix<var, -1, -1> y;
  if (outputs == 1) {
    const Eigen::Matrix<var, -1, 1> vector_b = b.col(0);
    y = stan::math::multiply(x, vector_b);
  } else {
    y = stan::math::multiply(x, b);
  }
  for (int j = 0; j < outputs; ++j)
    for (int i = 0; i < rows; ++i) y(i, j).adj() = seeds(i, j);
  stan::math::grad();
  const Eigen::MatrixXd values = b.val();
  Eigen::MatrixXd out(rows, outputs),
      grad = Eigen::MatrixXd::Zero(cols, outputs);
  int shape[] = {rows, cols, outputs};
  KernelCtx c{};
  c.n_in = 2;
  c.in[0] = {const_cast<double*>(x.data()), x.size()};
  c.in[1] = {const_cast<double*>(values.data()), values.size()};
  c.out = {out.data(), out.size()};
  c.in_adj[1] = {grad.data(), grad.size()};
  c.out_adj_vec = {seeds.data(), seeds.size()};
  c.idata = shape;
  c.n_idata = outputs == 1 ? 2 : 3;
  const Kernel* k = find_kernel(outputs == 1 ? OP_MATVEC : OP_GEMM);
  k->forward(c);
  k->backward(c);
  for (int j = 0; j < outputs; ++j)
    for (int i = 0; i < cols; ++i)
      expect_ulp("cancelling matrix gradient " + std::to_string(rows) + "x" +
                     std::to_string(cols) + "x" + std::to_string(outputs),
                 grad(i, j), b(i, j).adj(), 0);
  stan::math::recover_memory();
}

int main() {
  for (int rows : {1, 7, 40, 129, 572})
    for (int cols : {1, 3})
      for (int outputs : {1, 3}) cancellation_case(rows, cols, outputs);
  {
    const int R = 5, C = 3;
    // Column-major X (Stan/Eigen convention).
    const std::vector<double> X = {0.5, 2.0,  -0.4, 0.2, 1.3,  -1.2, -0.7, 0.9,
                                   0.8, -0.1, 0.3,  1.1, -1.5, -0.6, 0.7};
    const std::vector<double> y = {0.4, -1.0, 2.1, 0.3, -0.8};
    const std::vector<double> beta = {0.25, -0.5, 1.0};
    run_case("small", R, C, X, y, beta, 10);
  }
  {
    const int R = 67, C = 64;
    std::vector<double> X(R * C), y(R), beta(C);
    for (int c = 0; c < C; ++c) {
      const double scale = (c % 2 == 0) ? 1e6 : 1e-6;
      for (int r = 0; r < R; ++r) {
        const double sign = ((r + c) % 2 == 0) ? 1.0 : -1.0;
        X[c * R + r] = sign * scale * (0.3 + 0.01 * ((r * 31 + c * 17) % 13));
      }
    }
    for (int c = 0; c < C; ++c) beta[c] = 0.05 + 0.001 * c;
    for (int r = 0; r < R; ++r) y[r] = 0.2 - 0.001 * r;
    run_case("reassoc", R, C, X, y, beta, 10);
  }

  if (failures == 0) std::printf("test_matvec OK\n");
  return failures == 0 ? 0 : 1;
}
