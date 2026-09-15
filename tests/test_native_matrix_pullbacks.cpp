// Differential checks against the previously used nested Stan Math tape.
#include <stanli/graph.hpp>
#include <stanli/optable.hpp>
#include <stan/math.hpp>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using Mat = Eigen::MatrixXd;
using Vec = Eigen::VectorXd;
using Var = stan::math::var;
using VarVec = Eigen::Matrix<Var, -1, 1>;
static int failures = 0;
static void check(double got, double want, const char* tag,
                  bool exact = false) {
  if (got == want || (std::isnan(got) && std::isnan(want))) return;
  // GP summation order changes; Cholesky reuses the exact pinned pullback.
  if (!exact && std::isfinite(got) && std::isfinite(want) &&
      std::abs(got - want) <= 2e-12 * (1 + std::abs(want)))
    return;
  if (++failures < 12) std::printf("FAIL %s %.17g != %.17g\n", tag, got, want);
}
static void gp(int n, int d, int active, uint8_t variant, bool repeated,
               double rho = 0.7, double sigma = 1.3, double seed_scale = 0.7) {
  using namespace stanli;
  std::vector<double> x(n * d), xadj(n * d, 0.125), out(n * n), seed(n * n);
  double sadj = 0.125, radj = 0.125;
  for (int i = 0; i < n * d; ++i) x[i] = repeated ? 0.2 : std::sin(0.31 * i);
  for (int i = 0; i < n * n; ++i) seed[i] = std::cos(0.43 * i) * seed_scale;
  KernelCtx ctx;
  int dims[] = {n, d};
  ctx.n_in = 3;
  ctx.idata = dims;
  ctx.n_idata = 2;
  ctx.variant = variant;
  ctx.in[0] = {x.data(), n * d};
  ctx.in[1] = {&sigma, 1};
  ctx.in[2] = {&rho, 1};
  ctx.in_adj[0] = {active & 1 ? xadj.data() : nullptr, n * d};
  ctx.in_adj[1] = {active & 2 ? &sadj : nullptr, 1};
  ctx.in_adj[2] = {active & 4 ? &radj : nullptr, 1};
  ctx.out = {out.data(), n * n};
  ctx.out_adj_vec = {seed.data(), n * n};
  const Kernel& k = *find_kernel(OP_GP_COV);
  k.forward(ctx);
  k.backward(ctx);
  if (n == 0) {
    // The empty covariance has no contributions. Stan Math's Matrix<var>
    // sum oracle binds a null element reference for a 0x0 matrix under UBSan.
    check(sadj, 0.125, "empty GP sigma", true);
    check(radj, 0.125, "empty GP rho", true);
    return;
  }
  stan::math::nested_rev_autodiff scope;
  std::vector<VarVec> points(n, VarVec(d));
  for (int i = 0; i < n; ++i)
    for (int j = 0; j < d; ++j) points[i][j] = x[i * d + j];
  Var a = sigma, r = rho;
  Eigen::Matrix<Var, -1, -1> cov;
  switch (variant) {
    case kGpMatern32:
      cov = stan::math::gp_matern32_cov(points, a, r);
      break;
    case kGpMatern52:
      cov = stan::math::gp_matern52_cov(points, a, r);
      break;
    case kGpExponential:
      cov = stan::math::gp_exponential_cov(points, a, r);
      break;
    default:
      cov = stan::math::gp_exp_quad_cov(points, a, r);
  }
  Var objective = stan::math::sum(
      stan::math::elt_multiply(cov, Eigen::Map<Mat>(seed.data(), n, n)));
  stan::math::grad(objective.vi_);
  for (int i = 0; i < n * n; ++i)
    check(out[i], cov.data()[i].val(), "GP value", true);
  if (active & 1)
    for (int i = 0; i < n; ++i)
      for (int j = 0; j < d; ++j)
        check(xadj[i * d + j], 0.125 + points[i][j].adj(), "GP point");
  if (active & 2) check(sadj, 0.125 + a.adj(), "GP sigma");
  if (active & 4) check(radj, 0.125 + r.adj(), "GP rho");
}
static void chol(int n) {
  using namespace stanli;
  Mat m(n, n), seed(n, n), out(n, n), adj = Mat::Constant(n, n, 0.125);
  for (int i = 0; i < n; ++i)
    for (int j = 0; j < n; ++j) {
      m(i, j) = std::sin(0.13 * (i + 1) * (j + 2));
      seed(i, j) = std::cos(0.21 * (i + 2) * (j + 1));
    }
  Mat a = (m * m.transpose()).eval();
  a.diagonal().array() += 2;
  KernelCtx ctx;
  int dims[] = {n};
  ctx.n_in = 1;
  ctx.idata = dims;
  ctx.n_idata = 1;
  ctx.in[0] = {a.data(), n * n};
  ctx.in_adj[0] = {adj.data(), n * n};
  ctx.out = {out.data(), n * n};
  ctx.out_adj_vec = {seed.data(), n * n};
  const Kernel& k = *find_kernel(OP_CHOLESKY);
  k.forward(ctx);
  k.backward(ctx);
  if (n == 0) return;  // No factor entries and no adjoint contributions.
  stan::math::nested_rev_autodiff scope;
  stan::math::var_value<Mat> av(a);
  auto factor = stan::math::cholesky_decompose(av);
  Var objective = stan::math::sum(stan::math::elt_multiply(factor, seed));
  stan::math::grad(objective.vi_);
  for (int i = 0; i < n * n; ++i) {
    check(out.data()[i], factor.val().data()[i], "Cholesky value", true);
    check(adj.data()[i], 0.125 + av.adj().data()[i], "Cholesky adjoint", true);
  }
  ctx.in_adj[0].data = nullptr;
  k.backward(ctx);
}
int main(int argc, char** argv) {
  // Optional local UBSan diagnostic: the pinned Stan Math blocked routine
  // itself binds an empty Eigen block reference at its final panel. The
  // default (including CI's ASan-only run) always tests both algorithms.
  const bool unblocked_only =
      argc == 2 && std::strcmp(argv[1], "--unblocked-only") == 0;
  if (argc > 1 && !unblocked_only) return 2;
  for (int n : {0, 1, 5, 12})
    for (int d : {1, 3})
      for (int mask = 0; mask < 8; ++mask) {
        gp(n, d, mask, stanli::kGpExpQuad, false);
        gp(n, d, mask, stanli::kGpExpQuad, true);
      }
  for (auto variant :
       {stanli::kGpMatern32, stanli::kGpMatern52, stanli::kGpExponential})
    gp(4, 2, 7, variant, false);
  gp(4, 2, 7, stanli::kGpExpQuad, false, 1e-110);
  gp(4, 2, 7, stanli::kGpExpQuad, false, 1e110);
  for (double sigma : {1e-155, 1e-100, 1e100, 1e150})
    gp(4, 2, 6, stanli::kGpExpQuad, false, 0.7, sigma);
  gp(4, 2, 6, stanli::kGpExpQuad, false, 0.7, 1e150, 1e100);
  for (int n : {0, 1, 5, 35, 36, 80})
    if (!unblocked_only || n <= 35) chol(n);
  if (!failures) std::puts("test_native_matrix_pullbacks OK");
  return failures ? 1 : 0;
}
