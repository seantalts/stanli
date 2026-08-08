// Matrix-valued legacy ops: GP covariance, cholesky_decompose, diag_matrix,
// multi_normal(_cholesky). Forward runs the prim (double) implementation;
// backward replays the same call on a nested var tape and seeds the output
// adjoints with the dot trick. Correct by construction against the code
// CmdStan runs, at the cost of a nested tape per gradient.
//
// Matrices live in slots column-major, matching Eigen and the rest of the
// pipeline, so a flat slot maps straight onto Map<MatrixXd>.
#include <stanli/graph.hpp>
#include <stanli/legacy.hpp>
#include <stanli/optable.hpp>

#include <stan/math.hpp>

#include <vector>

namespace stanli {
namespace {

using MatD = Eigen::MatrixXd;
using VecD = Eigen::VectorXd;
using MapM = Eigen::Map<MatD>;
using CMapM = Eigen::Map<const MatD>;
using CMapV = Eigen::Map<const VecD>;
using VarV = Eigen::Matrix<stan::math::var, -1, 1>;
using VarM = Eigen::Matrix<stan::math::var, -1, -1>;

// Promote every input to var on a nested tape, call f, seed the output
// adjoints, and copy back the adjoints of inputs that carry one. Inputs
// arrive as flat vectors; f reshapes what it needs.
template <typename F>
void nary_bwd(KernelCtx& ctx, F&& f) {
  stan::math::nested_rev_autodiff nested;
  using stan::math::var;
  std::vector<VarV> xs(ctx.n_in);
  for (int k = 0; k < ctx.n_in; ++k) {
    xs[k].resize(ctx.in[k].len);
    for (int64_t i = 0; i < ctx.in[k].len; ++i) xs[k](i) = ctx.in[k].data[i];
  }
  auto out = f(xs);
  // Seed without copying the result: several rev overloads return a
  // var_value<Matrix> (SoA), and assigning that into a Matrix<var> copies
  // values into fresh vars, silently dropping the tape connection. Seeding
  // through stan-math ops on whatever type came back keeps it.
  var j;
  if constexpr (std::is_same_v<std::decay_t<decltype(out)>, var>) {
    j = out * ctx.out_adj;
  } else {
    MatD seed(out.rows(), out.cols());
    for (Eigen::Index c = 0, k = 0; c < out.cols(); ++c)
      for (Eigen::Index r = 0; r < out.rows(); ++r, ++k)
        seed(r, c) = ctx.out_adj_vec.data[k];
    j = stan::math::sum(stan::math::elt_multiply(out, seed));
  }
  stan::math::grad(j.vi_);
  for (int k = 0; k < ctx.n_in; ++k) {
    if (ctx.in_adj[k].data == nullptr) continue;
    for (int64_t i = 0; i < ctx.in[k].len; ++i)
      ctx.in_adj[k].data[i] += xs[k](i).adj();
  }
}

// ---- gp_exp_quad_cov(x, alpha, rho) ---------------------------------------
// in = {x (data, N*D), alpha, rho}; idata = {N, D}; out = N*N column-major.
// x as array[N] real is D == 1; array[N] vector[D] flattens array-major.
std::vector<VecD> gp_points(const KernelCtx& ctx) {
  const int64_t N = ctx.idata[0], D = ctx.idata[1];
  std::vector<VecD> pts(N, VecD(D));
  for (int64_t n = 0; n < N; ++n)
    for (int64_t d = 0; d < D; ++d) pts[n](d) = ctx.in[0].data[n * D + d];
  return pts;
}
void gp_cov_fwd(KernelCtx& ctx) {
  const int64_t N = ctx.idata[0];
  auto pts = gp_points(ctx);
  MatD c = stan::math::gp_exp_quad_cov(pts, ctx.in[1].data[0],
                                       ctx.in[2].data[0]);
  MapM(ctx.out.data, N, N) = c;
}
void gp_cov_bwd(KernelCtx& ctx) {
  const int64_t N = ctx.idata[0];
  auto pts = gp_points(ctx);
  (void)N;
  nary_bwd(ctx, [&](std::vector<VarV>& xs) {
    return stan::math::gp_exp_quad_cov(pts, xs[1](0), xs[2](0));
  });
}

// ---- diag_matrix(v) -------------------------------------------------------
void diag_fwd(KernelCtx& ctx) {
  const int64_t n = ctx.in[0].len;
  for (int64_t i = 0; i < n * n; ++i) ctx.out.data[i] = 0.0;
  for (int64_t i = 0; i < n; ++i) ctx.out.data[i * n + i] = ctx.in[0].data[i];
}
void diag_bwd(KernelCtx& ctx) {
  if (ctx.in_adj[0].data == nullptr) return;
  const int64_t n = ctx.in[0].len;
  for (int64_t i = 0; i < n; ++i)
    ctx.in_adj[0].data[i] += ctx.out_adj_vec.data[i * n + i];
}

// Single matrix input, matrix output, replayed on a varmat operand:
// `var_value<MatrixXd>` is one vari over contiguous value and adjoint
// blocks, so the flat arena slot maps straight in with no per-element
// promotion, and stan-math's varmat rev overload (what `stanc --O1`
// reaches for) runs instead of the AoS one. Column-major throughout,
// matching the slot layout.
template <typename F>
void matvar_bwd(KernelCtx& ctx, int64_t n, F&& f) {
  if (ctx.in_adj[0].data == nullptr) return;
  stan::math::nested_rev_autodiff nested;
  stan::math::var_value<Eigen::MatrixXd> a(CMapM(ctx.in[0].data, n, n));
  auto out = f(a);
  stan::math::var j = stan::math::sum(stan::math::elt_multiply(
      out, stan::math::to_matrix(CMapM(ctx.out_adj_vec.data, n, n))));
  stan::math::grad(j.vi_);
  MapM(ctx.in_adj[0].data, n, n) += a.adj();
}

// ---- cholesky_decompose(A) ------------------------------------------------
void chol_fwd(KernelCtx& ctx) {
  const int64_t n = ctx.idata[0];
  MatD a = CMapM(ctx.in[0].data, n, n);
  MapM(ctx.out.data, n, n) = stan::math::cholesky_decompose(a);
}
void chol_bwd(KernelCtx& ctx) {
  const int64_t n = ctx.idata[0];
  matvar_bwd(ctx, n, [](const auto& a) {
    return stan::math::cholesky_decompose(a);
  });
}

// ---- multi_normal_cholesky_lpdf(y | mu, L) --------------------------------
// in = {y, mu, L}; idata = {n}. Propto and per-argument activity follow the
// density convention: stan-math drops terms by argument type, so inactive
// arguments must stay double.
template <bool Grad>
double mnc_eval(KernelCtx& ctx) {
  const int64_t n = ctx.idata[0];
  const int64_t m = ctx.n_idata > 1 ? ctx.idata[1] : 1;
  const bool propto = (ctx.variant & 0x80u) != 0;
  const unsigned mask = ctx.variant == 0 ? 0x7u : (ctx.variant & 0x3fu);
  stan::math::nested_rev_autodiff nested;
  using stan::math::var;
  // m > 1: y is an array of m K-vectors (stan-math's vectorized form).
  std::vector<VarV> ys(m, VarV(n));
  std::vector<VecD> ysd(m, VecD(n));
  for (int64_t k = 0; k < m; ++k)
    for (int64_t i = 0; i < n; ++i) {
      ys[k](i) = ctx.in[0].data[k * n + i];
      ysd[k](i) = ctx.in[0].data[k * n + i];
    }
  VarV y = ys[0], mu(n);
  VarM L(n, n);
  for (int64_t i = 0; i < n; ++i) mu(i) = ctx.in[1].data[i];
  for (int64_t j = 0; j < n; ++j)
    for (int64_t i = 0; i < n; ++i) L(i, j) = ctx.in[2].data[j * n + i];
  CMapV yd(ctx.in[0].data, n), mud(ctx.in[1].data, n);
  CMapM Ld(ctx.in[2].data, n, n);

  // 8 activity combinations x propto; bind each argument var-or-double.
  auto call = [&](auto&& a, auto&& b, auto&& c) {
    return propto ? stan::math::multi_normal_cholesky_lpdf<true>(a, b, c)
                  : stan::math::multi_normal_cholesky_lpdf<false>(a, b, c);
  };
  var out;
  const bool ay = mask & 1u, am = mask & 2u, aL = mask & 4u;
  if (m > 1) {
    // Vectorized form: y is an array of m K-vectors and may itself be a
    // parameter expression (array[S] vector[K] built from parameters), so
    // it binds var-or-double per the activity mask like the others.
    var v_out;
    if (ay)
      v_out = aL ? (am ? call(ys, mu, L) : call(ys, mud, L))
                  : (am ? call(ys, mu, Ld) : call(ys, mud, Ld));
    else
      v_out = aL ? (am ? call(ysd, mu, L) : call(ysd, mud, L))
                  : (am ? call(ysd, mu, Ld) : call(ysd, mud, Ld));
    const double vv = v_out.val();
    if constexpr (Grad) {
      var jj = v_out * ctx.out_adj;
      stan::math::grad(jj.vi_);
      if (ay && ctx.in_adj[0].data)
        for (int64_t k = 0; k < m; ++k)
          for (int64_t i = 0; i < n; ++i)
            ctx.in_adj[0].data[k * n + i] += ys[k](i).adj();
      if (am && ctx.in_adj[1].data)
        for (int64_t i = 0; i < n; ++i) ctx.in_adj[1].data[i] += mu(i).adj();
      if (aL && ctx.in_adj[2].data)
        for (int64_t j2 = 0; j2 < n; ++j2)
          for (int64_t i = 0; i < n; ++i)
            ctx.in_adj[2].data[j2 * n + i] += L(i, j2).adj();
    }
    return vv;
  }
  if (ay && am && aL) out = call(y, mu, L);
  else if (ay && am) out = call(y, mu, Ld);
  else if (ay && aL) out = call(y, mud, L);
  else if (am && aL) out = call(yd, mu, L);
  else if (ay) out = call(y, mud, Ld);
  else if (am) out = call(yd, mu, Ld);
  else if (aL) out = call(yd, mud, L);
  else return call(yd, mud, Ld);

  const double v = out.val();
  if constexpr (Grad) {
    var j = out * ctx.out_adj;
    stan::math::grad(j.vi_);
    if (ay && ctx.in_adj[0].data)
      for (int64_t i = 0; i < n; ++i) ctx.in_adj[0].data[i] += y(i).adj();
    if (am && ctx.in_adj[1].data)
      for (int64_t i = 0; i < n; ++i) ctx.in_adj[1].data[i] += mu(i).adj();
    if (aL && ctx.in_adj[2].data)
      for (int64_t j2 = 0; j2 < n; ++j2)
        for (int64_t i = 0; i < n; ++i)
          ctx.in_adj[2].data[j2 * n + i] += L(i, j2).adj();
  }
  return v;
}
void mnc_fwd(KernelCtx& ctx) { ctx.out.data[0] = mnc_eval<false>(ctx); }
void mnc_bwd(KernelCtx& ctx) { mnc_eval<true>(ctx); }

// ---- multi_normal_lpdf(y | mu, Sigma) -------------------------------------
// multi_normal_prec takes the same three arguments in the same shapes -- a
// precision matrix instead of a covariance -- so it is the same kernel with
// one call swapped.
enum MnKind { kMnCov, kMnPrec };
template <bool Grad, MnKind Kind = kMnCov>
double mn_eval(KernelCtx& ctx) {
  const int64_t n = ctx.idata[0];
  const int64_t m = ctx.n_idata > 1 ? ctx.idata[1] : 1;
  const bool propto = (ctx.variant & 0x80u) != 0;
  const unsigned mask = ctx.variant == 0 ? 0x7u : (ctx.variant & 0x3fu);
  stan::math::nested_rev_autodiff nested;
  using stan::math::var;
  // m > 1: y is an array of m K-vectors (stan-math's vectorized form).
  std::vector<VarV> ys(m, VarV(n));
  std::vector<VecD> ysd(m, VecD(n));
  for (int64_t k = 0; k < m; ++k)
    for (int64_t i = 0; i < n; ++i) {
      ys[k](i) = ctx.in[0].data[k * n + i];
      ysd[k](i) = ctx.in[0].data[k * n + i];
    }
  VarV y = ys[0], mu(n);
  VarM S(n, n);
  for (int64_t i = 0; i < n; ++i) mu(i) = ctx.in[1].data[i];
  for (int64_t j = 0; j < n; ++j)
    for (int64_t i = 0; i < n; ++i) S(i, j) = ctx.in[2].data[j * n + i];
  CMapV yd(ctx.in[0].data, n), mud(ctx.in[1].data, n);
  CMapM Sd(ctx.in[2].data, n, n);
  auto call = [&](auto&& a, auto&& b, auto&& c) {
    if constexpr (Kind == kMnPrec) {
      return propto ? stan::math::multi_normal_prec_lpdf<true>(a, b, c)
                    : stan::math::multi_normal_prec_lpdf<false>(a, b, c);
    } else {
      return propto ? stan::math::multi_normal_lpdf<true>(a, b, c)
                    : stan::math::multi_normal_lpdf<false>(a, b, c);
    }
  };
  var out;
  const bool ay = mask & 1u, am = mask & 2u, aS = mask & 4u;
  if (m > 1) {
    // Vectorized form: y is an array of m K-vectors and may itself be a
    // parameter expression (array[S] vector[K] built from parameters), so
    // it binds var-or-double per the activity mask like the others.
    var v_out;
    if (ay)
      v_out = aS ? (am ? call(ys, mu, S) : call(ys, mud, S))
                  : (am ? call(ys, mu, Sd) : call(ys, mud, Sd));
    else
      v_out = aS ? (am ? call(ysd, mu, S) : call(ysd, mud, S))
                  : (am ? call(ysd, mu, Sd) : call(ysd, mud, Sd));
    const double vv = v_out.val();
    if constexpr (Grad) {
      var jj = v_out * ctx.out_adj;
      stan::math::grad(jj.vi_);
      if (ay && ctx.in_adj[0].data)
        for (int64_t k = 0; k < m; ++k)
          for (int64_t i = 0; i < n; ++i)
            ctx.in_adj[0].data[k * n + i] += ys[k](i).adj();
      if (am && ctx.in_adj[1].data)
        for (int64_t i = 0; i < n; ++i) ctx.in_adj[1].data[i] += mu(i).adj();
      if (aS && ctx.in_adj[2].data)
        for (int64_t j2 = 0; j2 < n; ++j2)
          for (int64_t i = 0; i < n; ++i)
            ctx.in_adj[2].data[j2 * n + i] += S(i, j2).adj();
    }
    return vv;
  }
  if (ay && am && aS) out = call(y, mu, S);
  else if (ay && am) out = call(y, mu, Sd);
  else if (ay && aS) out = call(y, mud, S);
  else if (am && aS) out = call(yd, mu, S);
  else if (ay) out = call(y, mud, Sd);
  else if (am) out = call(yd, mu, Sd);
  else if (aS) out = call(yd, mud, S);
  else return call(yd, mud, Sd);

  const double v = out.val();
  if constexpr (Grad) {
    var j = out * ctx.out_adj;
    stan::math::grad(j.vi_);
    if (ay && ctx.in_adj[0].data)
      for (int64_t i = 0; i < n; ++i) ctx.in_adj[0].data[i] += y(i).adj();
    if (am && ctx.in_adj[1].data)
      for (int64_t i = 0; i < n; ++i) ctx.in_adj[1].data[i] += mu(i).adj();
    if (aS && ctx.in_adj[2].data)
      for (int64_t j2 = 0; j2 < n; ++j2)
        for (int64_t i = 0; i < n; ++i)
          ctx.in_adj[2].data[j2 * n + i] += S(i, j2).adj();
  }
  return v;
}
void mn_fwd(KernelCtx& ctx) { ctx.out.data[0] = mn_eval<false>(ctx); }
void mn_bwd(KernelCtx& ctx) { mn_eval<true>(ctx); }
void mnprec_fwd(KernelCtx& ctx) {
  ctx.out.data[0] = mn_eval<false, kMnPrec>(ctx);
}
void mnprec_bwd(KernelCtx& ctx) { mn_eval<true, kMnPrec>(ctx); }

// ---- general matrix product: out = A * B ----------------------------------
// idata = {rows_a, cols_a, cols_b}; either side may carry adjoints.
void gemm_fwd(KernelCtx& ctx) {
  const int64_t ra = ctx.idata[0], ca = ctx.idata[1], cb = ctx.idata[2];
  CMapM A(ctx.in[0].data, ra, ca);
  CMapM B(ctx.in[1].data, ca, cb);
  MapM(ctx.out.data, ra, cb) = A * B;
}
void gemm_bwd(KernelCtx& ctx) {
  const int64_t ra = ctx.idata[0], ca = ctx.idata[1], cb = ctx.idata[2];
  CMapM A(ctx.in[0].data, ra, ca);
  CMapM B(ctx.in[1].data, ca, cb);
  CMapM dO(ctx.out_adj_vec.data, ra, cb);
  if (ctx.in_adj[0].data)
    MapM(ctx.in_adj[0].data, ra, ca) += dO * B.transpose();
  if (ctx.in_adj[1].data)
    MapM(ctx.in_adj[1].data, ca, cb) += A.transpose() * dO;
}

// ---- lkj_corr_cholesky_lpdf(L | eta) --------------------------------------
// in = {L, eta}; idata = {K}. eta is a data scalar in practice.
template <bool Grad, bool Chol = true>
double lkj_eval(KernelCtx& ctx) {
  const int64_t K = ctx.idata[0];
  const bool propto = (ctx.variant & 0x80u) != 0;
  stan::math::nested_rev_autodiff nested;
  using stan::math::var;
  VarM L(K, K);
  for (int64_t j = 0; j < K; ++j)
    for (int64_t i = 0; i < K; ++i) L(i, j) = ctx.in[0].data[j * K + i];
  const double eta = ctx.in[1].data[0];
  var out;
  if constexpr (Chol) {
    out = propto ? stan::math::lkj_corr_cholesky_lpdf<true>(L, eta)
                 : stan::math::lkj_corr_cholesky_lpdf<false>(L, eta);
  } else {
    out = propto ? stan::math::lkj_corr_lpdf<true>(L, eta)
                 : stan::math::lkj_corr_lpdf<false>(L, eta);
  }
  const double v = out.val();
  if constexpr (Grad) {
    var j = out * ctx.out_adj;
    stan::math::grad(j.vi_);
    if (ctx.in_adj[0].data)
      for (int64_t j2 = 0; j2 < K; ++j2)
        for (int64_t i = 0; i < K; ++i)
          ctx.in_adj[0].data[j2 * K + i] += L(i, j2).adj();
  }
  return v;
}
void lkj_fwd(KernelCtx& ctx) { ctx.out.data[0] = lkj_eval<false>(ctx); }
void lkj_bwd(KernelCtx& ctx) { lkj_eval<true>(ctx); }
// lkj_corr takes the correlation matrix itself where the cholesky form
// takes its factor: identical argument shapes, so identical kernel.
void lkjc_fwd(KernelCtx& ctx) {
  ctx.out.data[0] = lkj_eval<false, false>(ctx);
}
void lkjc_bwd(KernelCtx& ctx) { lkj_eval<true, false>(ctx); }

// ---- normal_id_glm_lpdf(y | X, alpha, beta, sigma) ------------------------
// in = {y, X, alpha, beta, sigma}; idata = {rows, cols}. X is a data matrix.
template <bool Grad>
double nid_glm_eval(KernelCtx& ctx) {
  const int64_t rows = ctx.idata[0], cols = ctx.idata[1];
  const bool propto = (ctx.variant & 0x80u) != 0;
  const unsigned mask = ctx.variant == 0 ? 0x1fu : (ctx.variant & 0x3fu);
  stan::math::nested_rev_autodiff nested;
  using stan::math::var;
  CMapV yd(ctx.in[0].data, rows);
  CMapM X(ctx.in[1].data, rows, cols);
  VarV alpha(ctx.in[2].len), beta(ctx.in[3].len), sigma(ctx.in[4].len);
  for (int64_t i = 0; i < ctx.in[2].len; ++i) alpha(i) = ctx.in[2].data[i];
  for (int64_t i = 0; i < ctx.in[3].len; ++i) beta(i) = ctx.in[3].data[i];
  for (int64_t i = 0; i < ctx.in[4].len; ++i) sigma(i) = ctx.in[4].data[i];
  auto scalar_or_vec = [](auto& v, int64_t len) {
    return len == 1 ? v(0) : var(0);  // placeholder, unused when len > 1
  };
  (void)scalar_or_vec;
  var out;
  const bool one_a = ctx.in[2].len == 1, one_s = ctx.in[4].len == 1;
  auto call = [&](auto&& a, auto&& s) {
    return propto ? stan::math::normal_id_glm_lpdf<true>(yd, X, a, beta, s)
                  : stan::math::normal_id_glm_lpdf<false>(yd, X, a, beta, s);
  };
  if (one_a && one_s) out = call(alpha(0), sigma(0));
  else if (one_a) out = call(alpha(0), sigma);
  else if (one_s) out = call(alpha, sigma(0));
  else out = call(alpha, sigma);
  const double v = out.val();
  // One tape per gradient, not two. The forward differentiates it once
  // with a seed of 1 and keeps the partials; the backward is then the
  // contraction every other native kernel does. This used to build the
  // whole var tape in the forward, throw it away, and build it again in
  // the backward to call grad() -- 90.9% of diamonds' gradient, and more
  // work than CmdStan does for the same statement.
  stan::math::grad(out.vi_);
  double* s = ctx.scratch;
  for (int64_t i = 0; i < ctx.in[2].len; ++i) *s++ = alpha(i).adj();
  for (int64_t i = 0; i < ctx.in[3].len; ++i) *s++ = beta(i).adj();
  for (int64_t i = 0; i < ctx.in[4].len; ++i) *s++ = sigma(i).adj();
  return v;
}

int64_t nid_glm_scratch(const Op& op, const Slot* slots) {
  return slots[op.in[2]].len + slots[op.in[3]].len + slots[op.in[4]].len;
}

void nid_glm_fwd(KernelCtx& ctx) { ctx.out.data[0] = nid_glm_eval<false>(ctx); }

void nid_glm_bwd(KernelCtx& ctx) {
  const unsigned mask = ctx.variant == 0 ? 0x1fu : (ctx.variant & 0x3fu);
  const double* s = ctx.scratch;
  const double w = ctx.out_adj;
  for (int k = 2; k <= 4; ++k) {
    const bool active = (mask & (0x4u << (k - 2))) && ctx.in_adj[k].data;
    for (int64_t i = 0; i < ctx.in[k].len; ++i, ++s)
      if (active) ctx.in_adj[k].data[i] += w * *s;
  }
}

// ---- transpose ------------------------------------------------------------
// idata = {rows, cols} of the input; output is cols x rows, col-major.
void transpose_fwd(KernelCtx& ctx) {
  const int64_t r = ctx.idata[0], c = ctx.idata[1];
  for (int64_t j = 0; j < c; ++j)
    for (int64_t i = 0; i < r; ++i)
      ctx.out.data[i * c + j] = ctx.in[0].data[j * r + i];
}
void transpose_bwd(KernelCtx& ctx) {
  if (!ctx.in_adj[0].data) return;
  const int64_t r = ctx.idata[0], c = ctx.idata[1];
  for (int64_t j = 0; j < c; ++j)
    for (int64_t i = 0; i < r; ++i)
      ctx.in_adj[0].data[j * r + i] += ctx.out_adj_vec.data[i * c + j];
}

// ---- symmetric eigendecomposition ----------------------------------------
// idata = {n}. Values ascending, vectors as columns, matching Eigen's
// SelfAdjointEigenSolver, which is what stan-math uses.
void eigvals_fwd(KernelCtx& ctx) {
  const int64_t n = ctx.idata[0];
  MatD a = CMapM(ctx.in[0].data, n, n);
  Eigen::Map<VecD>(ctx.out.data, n) = stan::math::eigenvalues_sym(a);
}
void eigvals_bwd(KernelCtx& ctx) {
  const int64_t n = ctx.idata[0];
  nary_bwd(ctx, [&](std::vector<VarV>& xs) {
    VarM a(n, n);
    for (int64_t j = 0; j < n; ++j)
      for (int64_t i = 0; i < n; ++i) a(i, j) = xs[0](j * n + i);
    return stan::math::eigenvalues_sym(a);
  });
}
void eigvecs_fwd(KernelCtx& ctx) {
  const int64_t n = ctx.idata[0];
  MatD a = CMapM(ctx.in[0].data, n, n);
  MapM(ctx.out.data, n, n) = stan::math::eigenvectors_sym(a);
}
void eigvecs_bwd(KernelCtx& ctx) {
  const int64_t n = ctx.idata[0];
  nary_bwd(ctx, [&](std::vector<VarV>& xs) {
    VarM a(n, n);
    for (int64_t j = 0; j < n; ++j)
      for (int64_t i = 0; i < n; ++i) a(i, j) = xs[0](j * n + i);
    return stan::math::eigenvectors_sym(a);
  });
}

int64_t no_scratch(const Op&, const Slot*) { return 0; }

}  // namespace

void register_matrix_kernels() {
  register_kernel(OP_GP_EXP_QUAD_COV,
                  Kernel{gp_cov_fwd, gp_cov_bwd, no_scratch});
  register_kernel(OP_DIAG_MATRIX, Kernel{diag_fwd, diag_bwd, no_scratch});
  register_kernel(OP_CHOLESKY, Kernel{chol_fwd, chol_bwd, no_scratch});
  register_kernel(OP_MULTI_NORMAL_CHOL_LPDF,
                  Kernel{mnc_fwd, mnc_bwd, no_scratch});
  register_kernel(OP_MULTI_NORMAL_LPDF, Kernel{mn_fwd, mn_bwd, no_scratch});
  register_kernel(OP_MULTI_NORMAL_PREC_LPDF,
                  Kernel{mnprec_fwd, mnprec_bwd, no_scratch});
  register_kernel(OP_GEMM, Kernel{gemm_fwd, gemm_bwd, no_scratch});
  register_kernel(OP_EIGENVALUES_SYM,
                  Kernel{eigvals_fwd, eigvals_bwd, no_scratch});
  register_kernel(OP_EIGENVECTORS_SYM,
                  Kernel{eigvecs_fwd, eigvecs_bwd, no_scratch});
  register_kernel(OP_TRANSPOSE,
                  Kernel{transpose_fwd, transpose_bwd, no_scratch});
  register_kernel(OP_LKJ_CORR_CHOL_LPDF, Kernel{lkj_fwd, lkj_bwd, no_scratch});
  register_kernel(OP_LKJ_CORR_LPDF,
                  Kernel{lkjc_fwd, lkjc_bwd, no_scratch});
  register_kernel(OP_NORMAL_ID_GLM_LPDF,
                  Kernel{nid_glm_fwd, nid_glm_bwd, nid_glm_scratch});
}

}  // namespace stanli
