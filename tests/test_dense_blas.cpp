// The accelerated dense kernels (STANLI_ACCELERATE): which ops the pass at
// the end of lowering gives them, and how close they come to the plain
// kernels they replace.
//
//  * Below an operation's threshold the op keeps its opcode, so it runs the
//    plain kernel and its results are bitwise those of a build without the
//    option. At the threshold it gets the accelerated opcode, whose kernel
//    calls BLAS or LAPACK.
//  * Above the threshold the accelerated kernels agree with the plain ones
//    to rounding. The Cholesky adjoint is also checked against central
//    finite differences of the factor, which neither kernel shares.
//
// In a build without the option the pass must leave every op alone, and a
// lowered program never contains an accelerated opcode.
//
// Argument: the MIR of tests/accelerate/dense_model.stan.
#include "env_helpers.hpp"
#include "expect.hpp"

#include <stanli/compile.hpp>
#include <stanli/data.hpp>
#include <stanli/dense_blas.hpp>
#include <stanli/graph.hpp>
#include <stanli/optable.hpp>

#include <Eigen/Dense>

#ifdef __APPLE__
#include <dlfcn.h>
#endif

#include <algorithm>
#include <cmath>
#include <limits>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using Mat = Eigen::MatrixXd;
using Vec = Eigen::VectorXd;

uint16_t opcode_after_pass(uint16_t opcode, std::vector<int> idata,
                           std::vector<int64_t> in_lens, int64_t out_len,
                           uint8_t variant = 0) {
  using namespace stanli;
  Graph g;
  std::vector<int> ins;
  for (int64_t len : in_lens) ins.push_back(g.add_slot(len, false));
  const int out = g.add_slot(out_len, false);
  const int at = (int)g.ops.size();
  if (ins.size() == 1)
    g.add_op(opcode, {ins[0]}, out, idata);
  else if (ins.size() == 2)
    g.add_op(opcode, {ins[0], ins[1]}, out, idata);
  else
    g.add_op(opcode, {ins[0], ins[1], ins[2]}, out, idata);
  for (size_t k = 3; k < ins.size(); ++k) g.ops[at].in[k] = ins[k];
  g.ops[at].n_in = (int)ins.size();
  g.ops[at].variant = variant;
  accelerate_dense_ops(g);
  return g.ops[at].opcode;
}

#ifdef STANLI_ACCELERATE
// The largest difference relative to the largest magnitude of `want`.
double scaled_error(const double* got, const double* want, int64_t len) {
  double worst = 0, scale = 0;
  for (int64_t i = 0; i < len; ++i) scale = std::max(scale, std::abs(want[i]));
  for (int64_t i = 0; i < len; ++i)
    worst = std::max(worst, std::abs(got[i] - want[i]));
  return scale > 0 ? worst / scale : worst;
}

Mat spd(int64_t n) {
  Mat m(n, n);
  for (int64_t i = 0; i < n; ++i)
    for (int64_t j = 0; j < n; ++j)
      m(i, j) = std::sin(0.13 * double(i + 1) * double(j + 2));
  Mat a = (m * m.transpose()).eval() / double(n);
  a.diagonal().array() += 1.0;
  return a;
}

Mat seed(int64_t r, int64_t c, double phase) {
  Mat s(r, c);
  for (int64_t i = 0; i < r; ++i)
    for (int64_t j = 0; j < c; ++j)
      s(i, j) = std::cos(phase + 0.21 * double(i + 2) * double(j + 1));
  return s;
}

bool registered(const stanli::Kernel* k, uint16_t opcode) {
  expect_true(
      std::string("kernel registered for ") + stanli::opcode_name(opcode),
      k != nullptr && k->forward != nullptr);
  return k != nullptr && k->forward != nullptr;
}

struct Run {
  Mat out, adj0, adj1;
  uint64_t calls = 0;
};

Run run_gemm(uint16_t opcode, const Mat& a, const Mat& b) {
  using namespace stanli;
  const int dims[] = {(int)a.rows(), (int)a.cols(), (int)b.cols()};
  Run r;
  r.out = Mat::Zero(a.rows(), b.cols());
  r.adj0 = Mat::Constant(a.rows(), a.cols(), 0.25);
  r.adj1 = Mat::Constant(b.rows(), b.cols(), -0.5);
  Mat dO = seed(a.rows(), b.cols(), 0.3);
  KernelCtx ctx;
  ctx.n_in = 2;
  ctx.idata = dims;
  ctx.n_idata = 3;
  ctx.in[0] = {const_cast<double*>(a.data()), a.size()};
  ctx.in[1] = {const_cast<double*>(b.data()), b.size()};
  ctx.out = {r.out.data(), r.out.size()};
  ctx.in_adj[0] = {r.adj0.data(), r.adj0.size()};
  ctx.in_adj[1] = {r.adj1.data(), r.adj1.size()};
  ctx.out_adj_vec = {dO.data(), dO.size()};
  const Kernel* k = find_kernel(opcode);
  if (!registered(k, opcode)) return r;
  const uint64_t before = blas::call_count();
  k->forward(ctx);
  k->backward(ctx);
  r.calls = blas::call_count() - before;
  return r;
}

Run run_matvec(uint16_t opcode, const Mat& x, const Vec& b) {
  using namespace stanli;
  const int dims[] = {(int)x.rows(), (int)x.cols()};
  Run r;
  r.out = Mat::Zero(x.rows(), 1);
  r.adj1 = Mat::Constant(b.size(), 1, 0.5);
  Mat dO = seed(x.rows(), 1, 0.7);
  KernelCtx ctx;
  ctx.n_in = 2;
  ctx.idata = dims;
  ctx.n_idata = 2;
  ctx.in[0] = {const_cast<double*>(x.data()), x.size()};
  ctx.in[1] = {const_cast<double*>(b.data()), b.size()};
  ctx.out = {r.out.data(), r.out.size()};
  ctx.in_adj[0] = {nullptr, x.size()};
  ctx.in_adj[1] = {r.adj1.data(), r.adj1.size()};
  ctx.out_adj_vec = {dO.data(), dO.size()};
  const Kernel* k = find_kernel(opcode);
  if (!registered(k, opcode)) return r;
  const uint64_t before = blas::call_count();
  k->forward(ctx);
  k->backward(ctx);
  r.calls = blas::call_count() - before;
  return r;
}

Run run_cholesky(uint16_t opcode, const Mat& a, const Mat& seed_adj) {
  using namespace stanli;
  const int64_t n = a.rows();
  const int dims[] = {(int)n};
  Run r;
  r.out = Mat::Zero(n, n);
  r.adj0 = Mat::Constant(n, n, 0.125);
  std::vector<double> scratch((size_t)(n * n));
  KernelCtx ctx;
  ctx.n_in = 1;
  ctx.idata = dims;
  ctx.n_idata = 1;
  ctx.in[0] = {const_cast<double*>(a.data()), n * n};
  ctx.in_adj[0] = {r.adj0.data(), n * n};
  ctx.out = {r.out.data(), n * n};
  ctx.out_adj_vec = {const_cast<double*>(seed_adj.data()), n * n};
  ctx.scratch = scratch.data();
  const Kernel* k = find_kernel(opcode);
  if (!registered(k, opcode)) return r;
  const uint64_t before = blas::call_count();
  k->forward(ctx);
  k->backward(ctx);
  r.calls = blas::call_count() - before;
  return r;
}

// multi_normal_cholesky with data y and mu and an active factor, propto: the
// shape of a Gaussian process likelihood.
Run run_mnc(uint16_t opcode, const Vec& y, const Vec& mu, const Mat& L,
            double* lp) {
  using namespace stanli;
  const int64_t n = y.size();
  // The vector layout: width, one y, one mu, no arrays.
  const int dims[] = {(int)n, 1, 1, 0};
  Run r;
  r.adj0 = Mat::Constant(n, n, 0.25);
  std::vector<double> scratch((size_t)(n * n));
  double out = 0;
  KernelCtx ctx;
  ctx.n_in = 3;
  ctx.idata = dims;
  ctx.n_idata = 4;
  ctx.variant = 0x84;
  ctx.in[0] = {const_cast<double*>(y.data()), n};
  ctx.in[1] = {const_cast<double*>(mu.data()), n};
  ctx.in[2] = {const_cast<double*>(L.data()), n * n};
  ctx.in_adj[0] = {nullptr, n};
  ctx.in_adj[1] = {nullptr, n};
  ctx.in_adj[2] = {r.adj0.data(), n * n};
  ctx.out = {&out, 1};
  ctx.out_adj = 1.0;
  ctx.scratch = scratch.data();
  const Kernel* k = find_kernel(opcode);
  if (!registered(k, opcode)) return r;
  const uint64_t before = blas::call_count();
  k->forward(ctx);
  k->backward(ctx);
  r.calls = blas::call_count() - before;
  *lp = out;
  return r;
}

// multi_normal(y | mu, Sigma) for one vector, with the activity in
// `variant`. Returns the log density and fills the three partials (the
// backward with out_adj 1), or reports the exception's message.
struct MnRun {
  double lp = 0;
  Vec dy, dmu;
  Mat dsigma;
  std::string error;
  uint64_t calls = 0;
};
MnRun run_mn(uint16_t opcode, const Vec& y, const Vec& mu, const Mat& sigma,
             uint8_t variant) {
  using namespace stanli;
  const int64_t n = y.size();
  const int dims[] = {(int)n, 1, 1, 0};
  MnRun r;
  r.dy = Vec::Zero(n);
  r.dmu = Vec::Zero(n);
  r.dsigma = Mat::Zero(n, n);
  std::vector<double> scratch((size_t)(2 * n + n * n));
  double out = 0;
  KernelCtx ctx;
  ctx.n_in = 3;
  ctx.idata = dims;
  ctx.n_idata = 4;
  ctx.variant = variant;
  ctx.in[0] = {const_cast<double*>(y.data()), n};
  ctx.in[1] = {const_cast<double*>(mu.data()), n};
  ctx.in[2] = {const_cast<double*>(sigma.data()), n * n};
  ctx.in_adj[0] = {(variant & 1) ? r.dy.data() : nullptr, n};
  ctx.in_adj[1] = {(variant & 2) ? r.dmu.data() : nullptr, n};
  ctx.in_adj[2] = {(variant & 4) ? r.dsigma.data() : nullptr, n * n};
  ctx.out = {&out, 1};
  ctx.out_adj = 1.0;
  ctx.scratch = scratch.data();
  const Kernel* k = find_kernel(opcode);
  if (!registered(k, opcode)) return r;
  const uint64_t before = blas::call_count();
  try {
    k->forward(ctx);
    k->backward(ctx);
  } catch (const std::exception& e) {
    r.error = e.what();
  }
  r.calls = blas::call_count() - before;
  r.lp = out;
  return r;
}

// normal_id_glm(y | X, alpha, beta, sigma) with data y and X: the log
// density and the partials of alpha, beta and sigma, or the error.
struct GlmRun {
  double lp = 0;
  Vec dalpha, dbeta, dsigma;
  std::string error;
  uint64_t calls = 0;
};
GlmRun run_glm(uint16_t opcode, const Vec& y, const Mat& x, const Vec& alpha,
               const Vec& beta, const Vec& sigma, uint8_t variant) {
  using namespace stanli;
  const int64_t rows = x.rows(), cols = x.cols();
  const int dims[] = {(int)rows, (int)cols, 0};
  GlmRun r;
  r.dalpha = Vec::Zero(alpha.size());
  r.dbeta = Vec::Zero(cols);
  r.dsigma = Vec::Zero(sigma.size());
  std::vector<double> scratch(
      (size_t)(rows + rows * cols + alpha.size() + cols + sigma.size()));
  double out = 0;
  KernelCtx ctx;
  ctx.n_in = 5;
  ctx.idata = dims;
  ctx.n_idata = 3;
  ctx.variant = variant;
  ctx.in[0] = {const_cast<double*>(y.data()), rows};
  ctx.in[1] = {const_cast<double*>(x.data()), rows * cols};
  ctx.in[2] = {const_cast<double*>(alpha.data()), alpha.size()};
  ctx.in[3] = {const_cast<double*>(beta.data()), cols};
  ctx.in[4] = {const_cast<double*>(sigma.data()), sigma.size()};
  ctx.in_adj[0] = {nullptr, rows};
  ctx.in_adj[1] = {nullptr, rows * cols};
  ctx.in_adj[2] = {(variant & 4) ? r.dalpha.data() : nullptr, alpha.size()};
  ctx.in_adj[3] = {(variant & 8) ? r.dbeta.data() : nullptr, cols};
  ctx.in_adj[4] = {(variant & 16) ? r.dsigma.data() : nullptr, sigma.size()};
  ctx.out = {&out, 1};
  ctx.out_adj = 1.0;
  ctx.scratch = scratch.data();
  const Kernel* k = find_kernel(opcode);
  if (!registered(k, opcode)) return r;
  const uint64_t before = blas::call_count();
  try {
    k->forward(ctx);
    k->backward(ctx);
  } catch (const std::exception& e) {
    r.error = e.what();
  }
  r.calls = blas::call_count() - before;
  r.lp = out;
  return r;
}

void check_close(const std::string& what, const Mat& got, const Mat& want,
                 double tolerance) {
  const double e = scaled_error(got.data(), want.data(), got.size());
  std::printf("%-44s scaled error %.2e\n", what.c_str(), e);
  expect_true(what + " within " + std::to_string(tolerance), e <= tolerance);
}
#endif

void thresholds_select_opcodes() {
  using namespace stanli;
#ifdef STANLI_ACCELERATE
  const blas::Thresholds& t = blas::thresholds();
  const int g = (int)t.gemm_min_dim, c = (int)t.cholesky_min_n,
            m = (int)t.tri_inverse_min_n, vr = (int)t.gemv_min_rows,
            vc = (int)t.gemv_min_cols;
#else
  const int g = 24, c = 32, m = 32, vr = 256, vc = 64;
#endif
  const bool on = std::string(blas_provider()) != "none";
  const auto expect_op = [&](const std::string& what, uint16_t got,
                             uint16_t accelerated, uint16_t plain) {
    expect_true(what + ": " + opcode_name(got),
                got == (on ? accelerated : plain));
  };
  expect_op("gemm below threshold",
            opcode_after_pass(OP_GEMM, {g, g, g - 1},
                              {int64_t(g) * g, int64_t(g) * (g - 1)},
                              int64_t(g) * (g - 1)),
            OP_GEMM, OP_GEMM);
  expect_op("gemm at threshold",
            opcode_after_pass(OP_GEMM, {g, g, g},
                              {int64_t(g) * g, int64_t(g) * g}, int64_t(g) * g),
            OP_GEMM_BLAS, OP_GEMM);
  expect_op("matvec below threshold",
            opcode_after_pass(OP_MATVEC, {vr, vc - 1},
                              {int64_t(vr) * (vc - 1), vc - 1}, vr),
            OP_MATVEC, OP_MATVEC);
  expect_op("matvec at threshold",
            opcode_after_pass(OP_MATVEC, {vr, vc}, {int64_t(vr) * vc, vc}, vr),
            OP_MATVEC_BLAS, OP_MATVEC);
  expect_op("cholesky below threshold",
            opcode_after_pass(OP_CHOLESKY, {c - 1}, {int64_t(c - 1) * (c - 1)},
                              int64_t(c - 1) * (c - 1)),
            OP_CHOLESKY, OP_CHOLESKY);
  expect_op(
      "cholesky at threshold",
      opcode_after_pass(OP_CHOLESKY, {c}, {int64_t(c) * c}, int64_t(c) * c),
      OP_CHOLESKY_BLAS, OP_CHOLESKY);
  expect_op(
      "multi_normal_cholesky below threshold",
      opcode_after_pass(OP_MULTI_NORMAL_CHOL_LPDF, {m - 1, 1, 1, 0},
                        {m - 1, m - 1, int64_t(m - 1) * (m - 1)}, 1, 0x84),
      OP_MULTI_NORMAL_CHOL_LPDF, OP_MULTI_NORMAL_CHOL_LPDF);
  expect_op("multi_normal_cholesky at threshold",
            opcode_after_pass(OP_MULTI_NORMAL_CHOL_LPDF, {m, 1, 1, 0},
                              {m, m, int64_t(m) * m}, 1, 0x84),
            OP_MULTI_NORMAL_CHOL_LPDF_BLAS, OP_MULTI_NORMAL_CHOL_LPDF);
  // multi_normal with a covariance: only one vector with an active
  // covariance is accelerated.
#ifdef STANLI_ACCELERATE
  const int v = (int)t.mn_cov_min_n;
#else
  const int v = 64;
#endif
  expect_op(
      "multi_normal below threshold",
      opcode_after_pass(OP_MULTI_NORMAL_LPDF, {v - 1, 1, 1, 0},
                        {v - 1, v - 1, int64_t(v - 1) * (v - 1)}, 1, 0x86),
      OP_MULTI_NORMAL_LPDF, OP_MULTI_NORMAL_LPDF);
  expect_op("multi_normal at threshold",
            opcode_after_pass(OP_MULTI_NORMAL_LPDF, {v, 1, 1, 0},
                              {v, v, int64_t(v) * v}, 1, 0x86),
            OP_MULTI_NORMAL_LPDF_BLAS, OP_MULTI_NORMAL_LPDF);
  expect_op("multi_normal with data covariance",
            opcode_after_pass(OP_MULTI_NORMAL_LPDF, {v, 1, 1, 0},
                              {v, v, int64_t(v) * v}, 1, 0x82),
            OP_MULTI_NORMAL_LPDF, OP_MULTI_NORMAL_LPDF);
  expect_op("multi_normal with an array of vectors",
            opcode_after_pass(OP_MULTI_NORMAL_LPDF, {v, 3, 3, 3},
                              {3 * v, 3 * v, int64_t(v) * v}, 1, 0x86),
            OP_MULTI_NORMAL_LPDF, OP_MULTI_NORMAL_LPDF);
  // normal_id_glm with data y and X and an active beta, on a design that
  // reaches the threshold; a thin design, an active X and a row-vector
  // design keep the plain kernel.
#ifdef STANLI_ACCELERATE
  const int gr = (int)t.glm_min_rows, gc = (int)t.glm_min_cols;
#else
  const int gr = 4096, gc = 24;
#endif
  const auto glm = [&](int rows, int cols, uint8_t variant, int flags) {
    return opcode_after_pass(OP_NORMAL_ID_GLM_LPDF, {rows, cols, flags},
                             {rows, int64_t(rows) * cols, 1, cols, 1}, 1,
                             variant);
  };
  expect_op("normal_id_glm at threshold", glm(gr, gc, 0x1c, 0),
            OP_NORMAL_ID_GLM_LPDF_BLAS, OP_NORMAL_ID_GLM_LPDF);
  expect_op("normal_id_glm, propto, beta only", glm(gr, gc, 0x88, 0),
            OP_NORMAL_ID_GLM_LPDF_BLAS, OP_NORMAL_ID_GLM_LPDF);
  expect_op("normal_id_glm with fewer rows", glm(gr - 1, gc, 0x1c, 0),
            OP_NORMAL_ID_GLM_LPDF, OP_NORMAL_ID_GLM_LPDF);
  expect_op("normal_id_glm with fewer columns", glm(gr, gc - 1, 0x1c, 0),
            OP_NORMAL_ID_GLM_LPDF, OP_NORMAL_ID_GLM_LPDF);
  expect_op("normal_id_glm with data beta", glm(gr, gc, 0x14, 0),
            OP_NORMAL_ID_GLM_LPDF, OP_NORMAL_ID_GLM_LPDF);
  expect_op("normal_id_glm with an active X", glm(gr, gc, 0x1e, 0),
            OP_NORMAL_ID_GLM_LPDF, OP_NORMAL_ID_GLM_LPDF);
  expect_op("normal_id_glm with an active y", glm(gr, gc, 0x1d, 0),
            OP_NORMAL_ID_GLM_LPDF, OP_NORMAL_ID_GLM_LPDF);
  // Every other activity keeps Stan Math's nested tape.
  expect_op("multi_normal_cholesky with active y",
            opcode_after_pass(OP_MULTI_NORMAL_CHOL_LPDF, {m, 1, 1, 0},
                              {m, m, int64_t(m) * m}, 1, 0x85),
            OP_MULTI_NORMAL_CHOL_LPDF, OP_MULTI_NORMAL_CHOL_LPDF);
}

#ifdef STANLI_ACCELERATE
void accelerated_kernels_agree() {
  using namespace stanli;
  const blas::Thresholds& t = blas::thresholds();
  {
    const int64_t n = t.gemm_min_dim;
    const Mat a = seed(n + 3, n, 0.1), b = seed(n, n + 5, 0.9);
    const Run plain = run_gemm(OP_GEMM, a, b);
    const Run fast = run_gemm(OP_GEMM_BLAS, a, b);
    expect_true("plain gemm makes no BLAS call", plain.calls == 0);
    expect_true("accelerated gemm calls BLAS", fast.calls == 3);
    check_close("gemm value", fast.out, plain.out, 1e-14);
    check_close("gemm adjoint of A", fast.adj0, plain.adj0, 1e-14);
    check_close("gemm adjoint of B", fast.adj1, plain.adj1, 1e-14);
  }
  {
    const Mat x = seed(t.gemv_min_rows, t.gemv_min_cols, 0.4);
    const Vec b = seed(t.gemv_min_cols, 1, 1.1);
    const Run plain = run_matvec(OP_MATVEC, x, b);
    const Run fast = run_matvec(OP_MATVEC_BLAS, x, b);
    expect_true("accelerated matvec calls BLAS", fast.calls == 2);
    check_close("matvec value", fast.out, plain.out, 1e-14);
    check_close("matvec adjoint", fast.adj1, plain.adj1, 1e-14);
  }
  for (int64_t n : {t.cholesky_min_n, t.cholesky_min_n + 1, int64_t(200),
                    int64_t(257), int64_t(512)}) {
    const Mat a = spd(n);
    const Mat w = seed(n, n, 0.5);
    const Run plain = run_cholesky(OP_CHOLESKY, a, w);
    const Run fast = run_cholesky(OP_CHOLESKY_BLAS, a, w);
    expect_true("plain cholesky makes no BLAS call", plain.calls == 0);
    expect_true("accelerated cholesky calls LAPACK", fast.calls > 0);
    const std::string at = " n=" + std::to_string(n);
    check_close("cholesky factor" + at, fast.out, plain.out, 1e-14);
    check_close("cholesky adjoint vs cholesky_lambda" + at, fast.adj0,
                plain.adj0, 1e-12);
  }
  {
    // Directional derivative along a symmetric E of sum(W .* chol(A)):
    // the adjoint, which reads only the lower triangle, against a central
    // difference of the plain factor.
    const int64_t n = t.cholesky_min_n + 9;
    const Mat a = spd(n), w = seed(n, n, 0.5);
    Mat e = seed(n, n, 2.3);
    e = ((e + e.transpose()) * 0.5).eval();
    const Run fast = run_cholesky(OP_CHOLESKY_BLAS, a, w);
    if (fast.adj0.size() == 0) return;
    double analytic = 0;
    for (int64_t c = 0; c < n; ++c)
      for (int64_t r = c; r < n; ++r)
        analytic += (fast.adj0(r, c) - 0.125) * e(r, c);
    const double h = 1e-6;
    const auto objective = [&](double step) {
      const Mat ap = (a + step * e).eval();
      const Mat l = ap.llt().matrixL();
      return (w.triangularView<Eigen::Lower>().toDenseMatrix().array() *
              l.array())
          .sum();
    };
    const double numeric = (objective(h) - objective(-h)) / (2 * h);
    const double rel = std::abs(analytic - numeric) / std::abs(numeric);
    std::printf(
        "%-44s relative error %.2e (analytic %.17g, central "
        "difference %.17g)\n",
        ("cholesky adjoint vs finite differences n=" + std::to_string(n))
            .c_str(),
        rel, analytic, numeric);
    expect_true("cholesky adjoint matches finite differences", rel <= 1e-6);
  }
  // multi_normal with an active covariance: the accelerated kernel against
  // the plain one, for each activity that keeps the covariance active.
  for (int64_t n : {t.mn_cov_min_n, int64_t(151)}) {
    const Mat sigma = spd(n);
    const Vec y = seed(n, 1, 0.2), mu = seed(n, 1, 1.7) * 0.1;
    for (uint8_t variant : {uint8_t(0x84), uint8_t(0x86), uint8_t(0x87),
                            uint8_t(0x04), uint8_t(0x06), uint8_t(0x07)}) {
      const MnRun plain = run_mn(OP_MULTI_NORMAL_LPDF, y, mu, sigma, variant);
      const MnRun fast =
          run_mn(OP_MULTI_NORMAL_LPDF_BLAS, y, mu, sigma, variant);
      const std::string at = " n=" + std::to_string(n) +
                             " variant=" + std::to_string(int(variant));
      expect_true("plain multi_normal succeeds" + at, plain.error.empty());
      expect_true("accelerated multi_normal succeeds" + at, fast.error.empty());
      expect_true("accelerated multi_normal calls LAPACK" + at, fast.calls > 0);
      const double lp_rel =
          std::abs(fast.lp - plain.lp) / std::max(1.0, std::abs(plain.lp));
      std::printf("%-44s relative error %.2e\n",
                  ("multi_normal lp" + at).c_str(), lp_rel);
      expect_true("multi_normal lp" + at, lp_rel <= 1e-13);
      check_close("multi_normal adjoint of Sigma" + at, fast.dsigma,
                  plain.dsigma, 1e-12);
      check_close("multi_normal adjoint of mu" + at, fast.dmu, plain.dmu,
                  1e-12);
      check_close("multi_normal adjoint of y" + at, fast.dy, plain.dy, 1e-12);
    }
  }
  // Rejections: whenever the plain kernel throws, the accelerated one
  // throws the same message, and a covariance that the Cholesky path cannot
  // vouch for gets the plain kernel's result exactly.
  {
    const int64_t n = t.mn_cov_min_n;
    const Vec y = seed(n, 1, 0.2), mu = seed(n, 1, 1.7) * 0.1;
    Vec low = seed(n, 1, 0.9);
    struct Bad {
      const char* what;
      Mat sigma;
    };
    std::vector<Bad> bad;
    Mat asym = spd(n);
    asym(0, n - 1) += 1e-3;
    bad.push_back({"asymmetric", asym});
    Mat indefinite = spd(n);
    indefinite(n / 2, n / 2) = -1.0;
    bad.push_back({"indefinite", indefinite});
    Mat nan = spd(n);
    nan(1, 1) = std::numeric_limits<double>::quiet_NaN();
    bad.push_back({"nan", nan});
    Mat inf = spd(n);
    inf(2, 3) = inf(3, 2) = std::numeric_limits<double>::infinity();
    bad.push_back({"infinite", inf});
    bad.push_back({"singular", (low * low.transpose()).eval()});
    Mat near = (low * low.transpose()).eval();
    near.diagonal().array() += 1e-13;
    bad.push_back({"nearly singular", near});
    bad.push_back({"zero", Mat::Zero(n, n)});
    for (const Bad& b : bad) {
      const MnRun plain = run_mn(OP_MULTI_NORMAL_LPDF, y, mu, b.sigma, 0x86);
      const MnRun fast =
          run_mn(OP_MULTI_NORMAL_LPDF_BLAS, y, mu, b.sigma, 0x86);
      const std::string what = std::string("multi_normal, ") + b.what;
      std::printf("%-44s plain: %s\n", what.c_str(),
                  plain.error.empty() ? "accepted" : plain.error.c_str());
      expect_true(what + ": same error", fast.error == plain.error);
      if (plain.error.empty()) {
        expect_true(what + ": same lp bits", same_bits(fast.lp, plain.lp));
        bool same = true;
        for (int64_t i = 0; i < n * n; ++i)
          same =
              same && same_bits(fast.dsigma.data()[i], plain.dsigma.data()[i]);
        expect_true(what + ": same adjoint bits", same);
      }
    }
  }
  // normal_id_glm: the accelerated kernel against the plain one for each
  // activity with an active beta, scalar and vector alpha and sigma, and the
  // plain kernel's errors for a non-positive or non-finite scale and
  // non-finite data.
  {
    const int64_t rows = t.glm_min_rows, cols = t.glm_min_cols;
    const Mat x = seed(rows, cols, 0.3) * 0.5;
    const Vec beta = seed(cols, 1, 1.1) * 0.2;
    const Vec y = (x * beta + seed(rows, 1, 2.0) * 0.3).eval();
    const Vec a1 = Vec::Constant(1, 0.1), av = seed(rows, 1, 0.4) * 0.1;
    const Vec s1 = Vec::Constant(1, 0.7),
              sv = (seed(rows, 1, 0.8).array().abs() + 0.5).matrix();
    for (uint8_t variant : {uint8_t(0x1c), uint8_t(0x9c), uint8_t(0x88),
                            uint8_t(0x08), uint8_t(0x18), uint8_t(0x8c)})
      for (int shape = 0; shape < 4; ++shape) {
        const Vec& alpha = (shape & 1) ? av : a1;
        const Vec& sigma = (shape & 2) ? sv : s1;
        const GlmRun plain =
            run_glm(OP_NORMAL_ID_GLM_LPDF, y, x, alpha, beta, sigma, variant);
        const GlmRun fast = run_glm(OP_NORMAL_ID_GLM_LPDF_BLAS, y, x, alpha,
                                    beta, sigma, variant);
        const std::string at = " variant=" + std::to_string(int(variant)) +
                               ((shape & 1) ? " alpha vector" : "") +
                               ((shape & 2) ? " sigma vector" : "");
        expect_true("plain normal_id_glm succeeds" + at, plain.error.empty());
        expect_true("accelerated normal_id_glm succeeds" + at,
                    fast.error.empty());
        expect_true("accelerated normal_id_glm calls BLAS" + at,
                    fast.calls > 0);
        const double lp_rel =
            std::abs(fast.lp - plain.lp) / std::max(1.0, std::abs(plain.lp));
        std::printf("%-44s relative error %.2e\n",
                    ("normal_id_glm lp" + at).c_str(), lp_rel);
        expect_true("normal_id_glm lp" + at, lp_rel <= 1e-13);
        check_close("normal_id_glm adjoint of beta" + at, fast.dbeta,
                    plain.dbeta, 1e-13);
        check_close("normal_id_glm adjoint of alpha" + at, fast.dalpha,
                    plain.dalpha, 1e-13);
        check_close("normal_id_glm adjoint of sigma" + at, fast.dsigma,
                    plain.dsigma, 1e-13);
      }
    const double nan = std::numeric_limits<double>::quiet_NaN(),
                 inf = std::numeric_limits<double>::infinity();
    struct Bad {
      const char* what;
      Vec y, sigma;
      Mat x;
    };
    std::vector<Bad> bad;
    bad.push_back({"zero scale", y, Vec::Constant(1, 0.0), x});
    bad.push_back({"negative scale", y, Vec::Constant(1, -1.0), x});
    bad.push_back({"infinite scale", y, Vec::Constant(1, inf), x});
    Vec ynan = y;
    ynan(3) = nan;
    bad.push_back({"nan in y", ynan, s1, x});
    Mat xinf = x;
    xinf(5, 2) = inf;
    bad.push_back({"infinite x", y, s1, xinf});
    Mat xnan = x;
    xnan(7, 1) = nan;
    bad.push_back({"nan in x", y, s1, xnan});
    for (const Bad& b : bad) {
      const GlmRun plain =
          run_glm(OP_NORMAL_ID_GLM_LPDF, b.y, b.x, a1, beta, b.sigma, 0x1c);
      const GlmRun fast = run_glm(OP_NORMAL_ID_GLM_LPDF_BLAS, b.y, b.x, a1,
                                  beta, b.sigma, 0x1c);
      const std::string what = std::string("normal_id_glm, ") + b.what;
      std::printf("%-44s plain: %s\n", what.c_str(),
                  plain.error.empty() ? "accepted" : plain.error.c_str());
      expect_true(what + ": same error", fast.error == plain.error);
    }
  }
  for (int64_t n : {t.tri_inverse_min_n, int64_t(200)}) {
    const Mat L = spd(n).llt().matrixL();
    const Vec y = seed(n, 1, 0.2), mu = seed(n, 1, 1.7) * 0.1;
    double lp_plain = 0, lp_fast = 0;
    const Run plain = run_mnc(OP_MULTI_NORMAL_CHOL_LPDF, y, mu, L, &lp_plain);
    const Run fast =
        run_mnc(OP_MULTI_NORMAL_CHOL_LPDF_BLAS, y, mu, L, &lp_fast);
    expect_true("accelerated multi_normal_cholesky calls BLAS", fast.calls > 0);
    const std::string at = " n=" + std::to_string(n);
    const double lp_rel = std::abs(lp_fast - lp_plain) / std::abs(lp_plain);
    std::printf("%-44s relative error %.2e\n",
                ("multi_normal_cholesky lp" + at).c_str(), lp_rel);
    expect_true("multi_normal_cholesky lp" + at, lp_rel <= 1e-13);
    check_close("multi_normal_cholesky adjoint of L" + at, fast.adj0,
                plain.adj0, 1e-12);
  }
}
#endif

}  // namespace

namespace {

bool accelerated(uint16_t opcode) {
  using namespace stanli;
  return opcode == OP_GEMM_BLAS || opcode == OP_MATVEC_BLAS ||
         opcode == OP_CHOLESKY_BLAS || opcode == OP_MULTI_NORMAL_CHOL_LPDF_BLAS;
}

// The accelerated opcodes in the log density and write_array graphs of the
// dense test model, lowered with N x N factors, an N x N by N x P product
// and an M x K by K matrix-vector product.
std::vector<uint16_t> accelerated_ops(const std::string& mir, int n, int p,
                                      int m, int k) {
  stanli::DataMap d;
  d.set_int("N", n);
  d.set_int("K", k);
  d.set_int("M", m);
  d.set_int("P", p);
  std::vector<double> x((size_t)n), y((size_t)n), big((size_t)(n * n)),
      z((size_t)(m * k));
  for (int i = 0; i < n; ++i) x[(size_t)i] = double(i) / n;
  for (int i = 0; i < n; ++i) y[(size_t)i] = std::sin(double(i));
  for (size_t i = 0; i < big.size(); ++i) big[i] = std::sin(0.3 * double(i));
  for (size_t i = 0; i < z.size(); ++i) z[i] = std::cos(0.1 * double(i));
  d.set_real_array("x", x);
  d.set_real_array("y", y);
  d.set_real_array("X", big, {n, n});
  d.set_real_array("Z", z, {m, k});
  const stanli::CompiledModel cm = stanli::compile_model(mir, d);
  std::vector<uint16_t> found;
  for (const stanli::Op& op : cm.graph.ops)
    if (accelerated(op.opcode)) found.push_back(op.opcode);
  if (cm.write_array)
    for (const stanli::Op& op : cm.write_array->graph.ops)
      if (accelerated(op.opcode)) found.push_back(op.opcode);
  return found;
}

bool contains(const std::vector<uint16_t>& ops, uint16_t opcode) {
  return std::find(ops.begin(), ops.end(), opcode) != ops.end();
}

void lowered_programs(const char* mir_path) {
  using namespace stanli;
  if (mir_path == nullptr) {
    expect_true("test_dense_blas needs the dense model's MIR", false);
    return;
  }
  const std::string mir = slurp(mir_path);
  const bool on = std::string(blas_provider()) != "none";
  const std::vector<uint16_t> large = accelerated_ops(mir, 64, 32, 256, 64);
  const std::vector<uint16_t> small = accelerated_ops(mir, 16, 8, 64, 16);
  std::printf(
      "dense model: %zu accelerated ops above the thresholds, %zu "
      "below\n",
      large.size(), small.size());
  expect_true("no accelerated op below the thresholds", small.empty());
  if (!on) {
    expect_true("a build without STANLI_ACCELERATE lowers no accelerated op",
                large.empty());
    return;
  }
  for (uint16_t opcode : {OP_GEMM_BLAS, OP_MATVEC_BLAS, OP_CHOLESKY_BLAS,
                          OP_MULTI_NORMAL_CHOL_LPDF_BLAS})
    expect_true(std::string("dense model lowers ") + opcode_name(opcode),
                contains(large, opcode));
  if (skip_without_debug_switches("STANLI_NO_ACCELERATE")) return;
  test_setenv("STANLI_NO_ACCELERATE", "1");
  const std::vector<uint16_t> off = accelerated_ops(mir, 64, 32, 256, 64);
  test_unsetenv("STANLI_NO_ACCELERATE");
  expect_true("STANLI_NO_ACCELERATE lowers no accelerated op", off.empty());
}

// Where BLASSetThreading exists every accelerated call runs on its own
// thread. Without it the accelerated kernels stay on by policy
// (kAccelerateWithoutThreadControl); the choice of kernels reads nothing
// else about the system.
void thread_policy() {
  using namespace stanli;
  expect_true("accelerated kernels with thread control",
              accelerated_kernels_enabled(true));
  expect_true(
      "accelerated kernels without thread control follow the policy",
      accelerated_kernels_enabled(false) == kAccelerateWithoutThreadControl);
#if defined(STANLI_ACCELERATE) && defined(__APPLE__)
  // A OneBlasThread runs the thread single-threaded and then gives it back
  // the setting it had, read through the same entry points by name.
  using Get = unsigned (*)();
  using Set = int (*)(unsigned);
  const auto get =
      reinterpret_cast<Get>(dlsym(RTLD_DEFAULT, "BLASGetThreading"));
  const auto set =
      reinterpret_cast<Set>(dlsym(RTLD_DEFAULT, "BLASSetThreading"));
  expect_true(
      "BLASSetThreading found exactly when thread control is available",
      (get != nullptr && set != nullptr) == blas_thread_control_available());
  if (get && set) {
    set(0);  // BLAS_THREADING_MULTI_THREADED
    unsigned inside = 99;
    {
      const OneBlasThread one;
      inside = get();
    }
    expect_true("OneBlasThread runs single-threaded", inside == 1);
    expect_true("OneBlasThread restores the multi-threaded setting",
                get() == 0);
    set(1);
    {
      const OneBlasThread one;
    }
    expect_true("OneBlasThread leaves a single-threaded setting alone",
                get() == 1);
    set(0);
  }
#endif
}

}  // namespace

int main(int argc, char** argv) {
  thresholds_select_opcodes();
  thread_policy();
  lowered_programs(argc > 1 ? argv[1] : nullptr);
#ifdef STANLI_ACCELERATE
  accelerated_kernels_agree();
#endif
  return finish("test_dense_blas");
}
