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

#include <variant>
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
  MatD c =
      stan::math::gp_exp_quad_cov(pts, ctx.in[1].data[0], ctx.in[2].data[0]);
  MapM(ctx.out.data, N, N) = c;
}
void gp_cov_bwd(KernelCtx& ctx) {
  auto pts = gp_points(ctx);
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
  matvar_bwd(ctx, n,
             [](const auto& a) { return stan::math::cholesky_decompose(a); });
}

// Bind a slot as a var matrix or vector, and scatter the adjoints back
// afterwards. Shared by the multivariate densities below and by the tail
// densities further down.
VarM tail_m(const KernelCtx& ctx, int k, int64_t rows, int64_t cols) {
  VarM M(rows, cols);
  for (int64_t j = 0; j < cols; ++j)
    for (int64_t i = 0; i < rows; ++i) M(i, j) = ctx.in[k].data[j * rows + i];
  return M;
}
VarV tail_v(const KernelCtx& ctx, int k, int64_t n) {
  VarV v(n);
  for (int64_t i = 0; i < n; ++i) v(i) = ctx.in[k].data[i];
  return v;
}
void tail_scatter(KernelCtx& ctx, int k, const VarM& M) {
  if (!ctx.in_adj[k].data) return;
  const int64_t rows = M.rows(), cols = M.cols();
  for (int64_t j = 0; j < cols; ++j)
    for (int64_t i = 0; i < rows; ++i)
      ctx.in_adj[k].data[j * rows + i] += M(i, j).adj();
}
void tail_scatter(KernelCtx& ctx, int k, const VarV& v) {
  if (!ctx.in_adj[k].data) return;
  for (int64_t i = 0; i < v.size(); ++i) ctx.in_adj[k].data[i] += v(i).adj();
}
void tail_scatter(KernelCtx& ctx, int k, const stan::math::var& x) {
  if (ctx.in_adj[k].data) ctx.in_adj[k].data[0] += x.adj();
}
void tail_scatter(KernelCtx& ctx, int k, const std::vector<VarV>& xs) {
  if (!ctx.in_adj[k].data) return;
  int64_t at = 0;
  for (const auto& x : xs)
    for (int64_t i = 0; i < x.size(); ++i)
      ctx.in_adj[k].data[at++] += x(i).adj();
}

// Complete a compact density and return all argument adjoints in slot order.
template <bool Grad, typename... Args>
[[gnu::always_inline]] inline double finish_tail_density(
    KernelCtx& ctx, const stan::math::var& density, const Args&... args) {
  const double value = density.val();
  if constexpr (Grad) {
    stan::math::var seeded = density * ctx.out_adj;
    stan::math::grad(seeded.vi_);
    int slot = 0;
    (tail_scatter(ctx, slot++, args), ...);
  }
  return value;
}

// ---- multi_normal_lpdf(y | mu, Sigma) -------------------------------------
// in = {y, mu, Sigma}; idata = {n}. Propto and per-argument activity follow
// the density convention: stan-math drops terms by argument type, so inactive
// arguments must stay double.
//
// multi_normal_prec and multi_normal_cholesky take the same three arguments in
// the same shapes -- a precision matrix or a Cholesky factor instead of a
// covariance -- so they are the same kernel with one call swapped.
enum MnKind { kMnCov, kMnPrec, kMnChol };
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
  // 8 activity combinations x propto; bind each argument var-or-double.
  auto call = [&](auto&& a, auto&& b, auto&& c) {
    if constexpr (Kind == kMnPrec) {
      return propto ? stan::math::multi_normal_prec_lpdf<true>(a, b, c)
                    : stan::math::multi_normal_prec_lpdf<false>(a, b, c);
    } else if constexpr (Kind == kMnChol) {
      return propto ? stan::math::multi_normal_cholesky_lpdf<true>(a, b, c)
                    : stan::math::multi_normal_cholesky_lpdf<false>(a, b, c);
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
      // y stays hand-written here: it is a std::vector<VarV>, not a VarV.
      if (ay && ctx.in_adj[0].data)
        for (int64_t k = 0; k < m; ++k)
          for (int64_t i = 0; i < n; ++i)
            ctx.in_adj[0].data[k * n + i] += ys[k](i).adj();
      if (am) tail_scatter(ctx, 1, mu);
      if (aS) tail_scatter(ctx, 2, S);
    }
    return vv;
  }
  if (ay && am && aS)
    out = call(y, mu, S);
  else if (ay && am)
    out = call(y, mu, Sd);
  else if (ay && aS)
    out = call(y, mud, S);
  else if (am && aS)
    out = call(yd, mu, S);
  else if (ay)
    out = call(y, mud, Sd);
  else if (am)
    out = call(yd, mu, Sd);
  else if (aS)
    out = call(yd, mud, S);
  else
    return call(yd, mud, Sd);

  const double v = out.val();
  if constexpr (Grad) {
    var j = out * ctx.out_adj;
    stan::math::grad(j.vi_);
    if (ay) tail_scatter(ctx, 0, y);
    if (am) tail_scatter(ctx, 1, mu);
    if (aS) tail_scatter(ctx, 2, S);
  }
  return v;
}
void mn_fwd(KernelCtx& ctx) { ctx.out.data[0] = mn_eval<false>(ctx); }
void mn_bwd(KernelCtx& ctx) { mn_eval<true>(ctx); }
void mnprec_fwd(KernelCtx& ctx) {
  ctx.out.data[0] = mn_eval<false, kMnPrec>(ctx);
}
void mnprec_bwd(KernelCtx& ctx) { mn_eval<true, kMnPrec>(ctx); }
void mnc_fwd(KernelCtx& ctx) { ctx.out.data[0] = mn_eval<false, kMnChol>(ctx); }
void mnc_bwd(KernelCtx& ctx) { mn_eval<true, kMnChol>(ctx); }

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
  return finish_tail_density<Grad>(ctx, out, L);
}
void lkj_fwd(KernelCtx& ctx) { ctx.out.data[0] = lkj_eval<false>(ctx); }
void lkj_bwd(KernelCtx& ctx) { lkj_eval<true>(ctx); }
// lkj_corr takes the correlation matrix itself where the cholesky form
// takes its factor: identical argument shapes, so identical kernel.
void lkjc_fwd(KernelCtx& ctx) { ctx.out.data[0] = lkj_eval<false, false>(ctx); }
void lkjc_bwd(KernelCtx& ctx) { lkj_eval<true, false>(ctx); }

// ---- normal_id_glm_lpdf(y | X, alpha, beta, sigma) ------------------------
// in = {y, X, alpha, beta, sigma}; idata = {rows, cols}. X is a data matrix.
double nid_glm_eval(KernelCtx& ctx) {
  const int64_t rows = ctx.idata[0], cols = ctx.idata[1];
  const bool propto = (ctx.variant & 0x80u) != 0;
  // y is a parameter when stanc3's --O1 partial evaluator built the GLM
  // out of `theta ~ normal(X * b, s)`: the outcome needs its gradient
  // back. y-as-data (every hand-written GLM) keeps the double fast path.
  const bool y_var = (ctx.variant & 0x1u) != 0;
  stan::math::nested_rev_autodiff nested;
  using stan::math::var;
  CMapV yd(ctx.in[0].data, rows);
  CMapM X(ctx.in[1].data, rows, cols);
  VarV yv(y_var ? rows : 0);
  for (int64_t i = 0; i < yv.size(); ++i) yv(i) = ctx.in[0].data[i];
  VarV alpha(ctx.in[2].len), beta(ctx.in[3].len), sigma(ctx.in[4].len);
  for (int64_t i = 0; i < ctx.in[2].len; ++i) alpha(i) = ctx.in[2].data[i];
  for (int64_t i = 0; i < ctx.in[3].len; ++i) beta(i) = ctx.in[3].data[i];
  for (int64_t i = 0; i < ctx.in[4].len; ++i) sigma(i) = ctx.in[4].data[i];
  var out;
  const bool one_a = ctx.in[2].len == 1, one_s = ctx.in[4].len == 1;
  auto call = [&](auto&& y, auto&& a, auto&& s) {
    return propto ? stan::math::normal_id_glm_lpdf<true>(y, X, a, beta, s)
                  : stan::math::normal_id_glm_lpdf<false>(y, X, a, beta, s);
  };
  auto dispatch = [&](auto&& y) {
    if (one_a && one_s)
      out = call(y, alpha(0), sigma(0));
    else if (one_a)
      out = call(y, alpha(0), sigma);
    else if (one_s)
      out = call(y, alpha, sigma(0));
    else
      out = call(y, alpha, sigma);
  };
  if (y_var)
    dispatch(yv);
  else
    dispatch(yd);
  const double v = out.val();
  // One tape per gradient, not two. The forward differentiates it once
  // with a seed of 1 and keeps the partials; the backward is then the
  // contraction every other native kernel does. This used to build the
  // whole var tape in the forward, throw it away, and build it again in
  // the backward to call grad() -- 90.9% of diamonds' gradient, and more
  // work than CmdStan does for the same statement.
  stan::math::grad(out.vi_);
  double* s = ctx.scratch;
  for (int64_t i = 0; i < yv.size(); ++i) *s++ = yv(i).adj();
  for (int64_t i = 0; i < ctx.in[2].len; ++i) *s++ = alpha(i).adj();
  for (int64_t i = 0; i < ctx.in[3].len; ++i) *s++ = beta(i).adj();
  for (int64_t i = 0; i < ctx.in[4].len; ++i) *s++ = sigma(i).adj();
  return v;
}

int64_t nid_glm_scratch(const Op& op, const Slot* slots) {
  // The y section exists only when y is active (variant bit 0), but the
  // sizing hook cannot see the variant, so reserve it unconditionally.
  return slots[op.in[0]].len + slots[op.in[2]].len + slots[op.in[3]].len +
         slots[op.in[4]].len;
}

void nid_glm_fwd(KernelCtx& ctx) { ctx.out.data[0] = nid_glm_eval(ctx); }

void nid_glm_bwd(KernelCtx& ctx) {
  const unsigned mask = ctx.variant == 0 ? 0x1fu : (ctx.variant & 0x3fu);
  const double* s = ctx.scratch;
  const double w = ctx.out_adj;
  // Mirror of the forward's scratch layout: y's partials are present
  // exactly when variant bit 0 is set (never under the legacy variant==0
  // encoding, whose forward bound y as data).
  if (ctx.variant & 0x1u) {
    if (ctx.in_adj[0].data)
      for (int64_t i = 0; i < ctx.in[0].len; ++i)
        ctx.in_adj[0].data[i] += w * s[i];
    s += ctx.in[0].len;
  }
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

// ---- tail densities: one nested var tape, no hand-written derivative ----
// Everything below binds EVERY argument as var, calls the unmodified
// stan-math template, grads, and scatters the adjoints back. Two
// consequences worth stating.
//
// It has no restriction on what the density does with its scalar type.
// The recorder computes in doubles and carries no tape, so a density that
// does arithmetic on the scalar (ordered_probit's
// `c_vec[i].coeff(0) - lambda_vec[i]`, wiener's `res *= 0.0`) cannot go
// through it at all. stan::math::var has every operator, so those work
// here. That is why this file, not densities.cpp, is where the tail goes.
//
// And it is the compact tier by construction: one instantiation, no
// activity-mask expansion. A data argument's partials are computed and
// dropped, which is the right trade for a density nobody has profiled.
// ---- wishart family: (matrix W, real nu, matrix S), four of them --------
enum WishKind { kWishart, kInvWishart, kWishartChol, kInvWishartChol };
template <bool Grad, WishKind Kind>
double wish_eval(KernelCtx& ctx) {
  const int64_t K = ctx.idata[0];
  const bool propto = (ctx.variant & 0x80u) != 0;
  stan::math::nested_rev_autodiff nested;
  using stan::math::var;
  VarM W = tail_m(ctx, 0, K, K);
  var nu = ctx.in[1].data[0];
  VarM S = tail_m(ctx, 2, K, K);
  var out;
  if constexpr (Kind == kWishart) {
    out = propto ? stan::math::wishart_lpdf<true>(W, nu, S)
                 : stan::math::wishart_lpdf<false>(W, nu, S);
  } else if constexpr (Kind == kInvWishart) {
    out = propto ? stan::math::inv_wishart_lpdf<true>(W, nu, S)
                 : stan::math::inv_wishart_lpdf<false>(W, nu, S);
  } else if constexpr (Kind == kWishartChol) {
    out = propto ? stan::math::wishart_cholesky_lpdf<true>(W, nu, S)
                 : stan::math::wishart_cholesky_lpdf<false>(W, nu, S);
  } else {
    out = propto ? stan::math::inv_wishart_cholesky_lpdf<true>(W, nu, S)
                 : stan::math::inv_wishart_cholesky_lpdf<false>(W, nu, S);
  }
  return finish_tail_density<Grad>(ctx, out, W, nu, S);
}
#define STANLI_WISH_KERNEL(name, kind)             \
  void name##_fwd(KernelCtx& ctx) {                \
    ctx.out.data[0] = wish_eval<false, kind>(ctx); \
  }                                                \
  void name##_bwd(KernelCtx& ctx) { wish_eval<true, kind>(ctx); }
STANLI_WISH_KERNEL(wish, kWishart)
STANLI_WISH_KERNEL(iwish, kInvWishart)
STANLI_WISH_KERNEL(wishc, kWishartChol)
STANLI_WISH_KERNEL(iwishc, kInvWishartChol)
#undef STANLI_WISH_KERNEL

// ---- multi_gp: (matrix y, matrix Sigma, vector w) -----------------------
enum GpKind { kMultiGp, kMultiGpChol };
template <bool Grad, GpKind Kind>
double mgp_eval(KernelCtx& ctx) {
  const int64_t K = ctx.idata[0], N = ctx.idata[1];
  const bool propto = (ctx.variant & 0x80u) != 0;
  stan::math::nested_rev_autodiff nested;
  using stan::math::var;
  VarM y = tail_m(ctx, 0, K, N);
  VarM S = tail_m(ctx, 1, K, K);
  VarV w = tail_v(ctx, 2, K);
  var out;
  if constexpr (Kind == kMultiGp) {
    out = propto ? stan::math::multi_gp_lpdf<true>(y, S, w)
                 : stan::math::multi_gp_lpdf<false>(y, S, w);
  } else {
    out = propto ? stan::math::multi_gp_cholesky_lpdf<true>(y, S, w)
                 : stan::math::multi_gp_cholesky_lpdf<false>(y, S, w);
  }
  return finish_tail_density<Grad>(ctx, out, y, S, w);
}

// ---- multi_student_t: (vector y, real nu, vector mu, matrix Sigma) ------
enum MstKind { kMst, kMstChol };
template <bool Grad, MstKind Kind>
double mst_eval(KernelCtx& ctx) {
  const int64_t K = ctx.idata[0];
  const int64_t reps = ctx.n_idata > 1 ? ctx.idata[1] : 1;
  const bool propto = (ctx.variant & 0x80u) != 0;
  stan::math::nested_rev_autodiff nested;
  using stan::math::var;
  // y may be one K-vector or an array of reps of them.
  std::vector<VarV> ys;
  ys.reserve(reps);
  for (int64_t r = 0; r < reps; ++r) {
    VarV yy(K);
    for (int64_t i = 0; i < K; ++i) yy(i) = ctx.in[0].data[r * K + i];
    ys.push_back(std::move(yy));
  }
  var nu = ctx.in[1].data[0];
  VarV mu = tail_v(ctx, 2, K);
  VarM S = tail_m(ctx, 3, K, K);
  const auto call = [&](auto&& yarg) {
    if constexpr (Kind == kMst) {
      return propto ? stan::math::multi_student_t_lpdf<true>(yarg, nu, mu, S)
                    : stan::math::multi_student_t_lpdf<false>(yarg, nu, mu, S);
    } else {
      return propto ? stan::math::multi_student_t_cholesky_lpdf<true>(yarg, nu,
                                                                      mu, S)
                    : stan::math::multi_student_t_cholesky_lpdf<false>(yarg, nu,
                                                                       mu, S);
    }
  };
  var out = reps > 1 ? call(ys) : call(ys[0]);
  return finish_tail_density<Grad>(ctx, out, ys, nu, mu, S);
}

// ---- multinomial family: (array[] int ns, vector theta) -----------------
// The counts ride in idata; theta is the only propagator edge.
enum MultKind { kMultinomial, kMultinomialLogit, kDirichletMultinomial };
template <bool Grad, MultKind Kind>
double mult_eval(KernelCtx& ctx) {
  const int64_t K = ctx.in[0].len;
  const bool propto = (ctx.variant & 0x80u) != 0;
  std::vector<int> ns(ctx.idata, ctx.idata + ctx.n_idata);
  stan::math::nested_rev_autodiff nested;
  using stan::math::var;
  VarV theta = tail_v(ctx, 0, K);
  var out;
  if constexpr (Kind == kMultinomial) {
    out = propto ? stan::math::multinomial_lpmf<true>(ns, theta)
                 : stan::math::multinomial_lpmf<false>(ns, theta);
  } else if constexpr (Kind == kMultinomialLogit) {
    out = propto ? stan::math::multinomial_logit_lpmf<true>(ns, theta)
                 : stan::math::multinomial_logit_lpmf<false>(ns, theta);
  } else {
    out = propto ? stan::math::dirichlet_multinomial_lpmf<true>(ns, theta)
                 : stan::math::dirichlet_multinomial_lpmf<false>(ns, theta);
  }
  return finish_tail_density<Grad>(ctx, out, theta);
}

// ---- the two the recorder cannot take ----------------------------------
// ordered_probit does `c_vec[i].coeff(0) - lambda_vec[i]` and wiener
// `res *= 0.0` on the scalar type. var has those operators; rvar
// deliberately does not (see recorder.hpp), so they live here.
// ordered_probit(y | lambda, c): counts in idata, lambda and c real edges.
template <bool Grad>
double oprobit_eval(KernelCtx& ctx) {
  const int64_t N = ctx.n_idata, K = ctx.in[1].len;
  const bool propto = (ctx.variant & 0x80u) != 0;
  std::vector<int> y(ctx.idata, ctx.idata + N);
  stan::math::nested_rev_autodiff nested;
  using stan::math::var;
  VarV lambda = tail_v(ctx, 0, ctx.in[0].len);
  VarV c = tail_v(ctx, 1, K);
  var out;
  if (ctx.in[0].len == 1) {
    out = propto ? stan::math::ordered_probit_lpmf<true>(y, lambda(0), c)
                 : stan::math::ordered_probit_lpmf<false>(y, lambda(0), c);
  } else {
    out = propto ? stan::math::ordered_probit_lpmf<true>(y, lambda, c)
                 : stan::math::ordered_probit_lpmf<false>(y, lambda, c);
  }
  return finish_tail_density<Grad>(ctx, out, lambda, c);
}

// wiener(y | alpha, tau, beta, delta): five real arguments, and every one
// of them vectorizes in the language. A length-1 slot must enter stan-math
// as a scalar: its sequence views broadcast scalars but require vectors to
// match sizes, and the conformance sweep caught the old scalar-only tail
// silently evaluating every observation with element 0's parameters.
template <bool Grad>
double wiener_eval(KernelCtx& ctx) {
  const bool propto = (ctx.variant & 0x80u) != 0;
  stan::math::nested_rev_autodiff nested;
  using stan::math::var;
  using Arg = std::variant<var, VarV>;
  auto load = [&](int k) -> Arg {
    if (ctx.in[k].len == 1) return var(ctx.in[k].data[0]);
    return tail_v(ctx, k, ctx.in[k].len);
  };
  Arg y = load(0), alpha = load(1), tau = load(2), beta = load(3),
      delta = load(4);
  var out = std::visit(
      [&](const auto&... a) -> var {
        return propto ? stan::math::wiener_lpdf<true>(a...)
                      : stan::math::wiener_lpdf<false>(a...);
      },
      y, alpha, tau, beta, delta);
  const double value = out.val();
  if constexpr (Grad) {
    var seeded = out * ctx.out_adj;
    stan::math::grad(seeded.vi_);
    int slot = 0;
    for (const Arg* p : {&y, &alpha, &tau, &beta, &delta}) {
      std::visit([&](const auto& a) { tail_scatter(ctx, slot, a); }, *p);
      ++slot;
    }
  }
  return value;
}

#define STANLI_TAIL_KERNEL(name, fn, kind)                                    \
  void name##_fwd(KernelCtx& ctx) { ctx.out.data[0] = fn<false, kind>(ctx); } \
  void name##_bwd(KernelCtx& ctx) { fn<true, kind>(ctx); }
STANLI_TAIL_KERNEL(mgp, mgp_eval, kMultiGp)
STANLI_TAIL_KERNEL(mgpc, mgp_eval, kMultiGpChol)
STANLI_TAIL_KERNEL(mst, mst_eval, kMst)
STANLI_TAIL_KERNEL(mstc, mst_eval, kMstChol)
STANLI_TAIL_KERNEL(multn, mult_eval, kMultinomial)
STANLI_TAIL_KERNEL(multnl, mult_eval, kMultinomialLogit)
STANLI_TAIL_KERNEL(dirmult, mult_eval, kDirichletMultinomial)
#undef STANLI_TAIL_KERNEL
void oprobit_fwd(KernelCtx& ctx) { ctx.out.data[0] = oprobit_eval<false>(ctx); }
void oprobit_bwd(KernelCtx& ctx) { oprobit_eval<true>(ctx); }
void wiener_fwd(KernelCtx& ctx) { ctx.out.data[0] = wiener_eval<false>(ctx); }
void wiener_bwd(KernelCtx& ctx) { wiener_eval<true>(ctx); }

// ---- the last five ------------------------------------------------------
// lkj_cov(Sigma | mu, sigma, eta): a covariance matrix, two vectors of
// lognormal hyperparameters for the scales, and the LKJ shape.
template <bool Grad>
double lkjcov_eval(KernelCtx& ctx) {
  const int64_t K = ctx.idata[0];
  const bool propto = (ctx.variant & 0x80u) != 0;
  stan::math::nested_rev_autodiff nested;
  using stan::math::var;
  VarM S = tail_m(ctx, 0, K, K);
  VarV mu = tail_v(ctx, 1, ctx.in[1].len);
  VarV sig = tail_v(ctx, 2, ctx.in[2].len);
  var eta = ctx.in[3].data[0];
  var out = propto ? stan::math::lkj_cov_lpdf<true>(S, mu, sig, eta)
                   : stan::math::lkj_cov_lpdf<false>(S, mu, sig, eta);
  return finish_tail_density<Grad>(ctx, out, S, mu, sig, eta);
}

// The three remaining GLMs. Unlike the ones in densities.cpp these carry
// argument shapes the recorder cannot express (a cutpoint vector, a
// coefficient matrix, a second int group), so they take the var tape.
// idata = [outcome..., rows, cols] and, for binomial, the trial counts.
enum GlmKind { kBinomLogitGlm, kCatLogitGlm, kOrdLogisticGlm };
template <bool Grad, GlmKind Kind>
double tglm_eval(KernelCtx& ctx) {
  const int64_t rows = ctx.idata[ctx.n_idata - 2];
  const int64_t cols = ctx.idata[ctx.n_idata - 1];
  const bool propto = (ctx.variant & 0x80u) != 0;
  stan::math::nested_rev_autodiff nested;
  using stan::math::var;
  VarM X = tail_m(ctx, 0, rows, cols);
  var out;
  if constexpr (Kind == kBinomLogitGlm) {
    // idata = [n..., N..., rows, cols]
    std::vector<int> nn(ctx.idata, ctx.idata + rows);
    std::vector<int> NN(ctx.idata + rows, ctx.idata + 2 * rows);
    var alpha = ctx.in[1].data[0];
    VarV beta = tail_v(ctx, 2, cols);
    out = propto ? stan::math::binomial_logit_glm_lpmf<true>(nn, NN, X, alpha,
                                                             beta)
                 : stan::math::binomial_logit_glm_lpmf<false>(nn, NN, X, alpha,
                                                              beta);
    return finish_tail_density<Grad>(ctx, out, X, alpha, beta);
  } else {
    std::vector<int> y(ctx.idata, ctx.idata + rows);
    if constexpr (Kind == kCatLogitGlm) {
      VarV alpha = tail_v(ctx, 1, ctx.in[1].len);
      VarM beta = tail_m(ctx, 2, cols, ctx.in[2].len / cols);
      out = propto ? stan::math::categorical_logit_glm_lpmf<true>(y, X, alpha,
                                                                  beta)
                   : stan::math::categorical_logit_glm_lpmf<false>(y, X, alpha,
                                                                   beta);
      return finish_tail_density<Grad>(ctx, out, X, alpha, beta);
    } else {
      // in = {X, beta, cutpoints}; alpha above is beta for this one.
      VarV beta = tail_v(ctx, 1, cols);
      VarV cuts = tail_v(ctx, 2, ctx.in[2].len);
      out =
          propto
              ? stan::math::ordered_logistic_glm_lpmf<true>(y, X, beta, cuts)
              : stan::math::ordered_logistic_glm_lpmf<false>(y, X, beta, cuts);
      return finish_tail_density<Grad>(ctx, out, X, beta, cuts);
    }
  }
}

void lkjcov_fwd(KernelCtx& ctx) { ctx.out.data[0] = lkjcov_eval<false>(ctx); }
void lkjcov_bwd(KernelCtx& ctx) { lkjcov_eval<true>(ctx); }
void blglm_fwd(KernelCtx& ctx) {
  ctx.out.data[0] = tglm_eval<false, kBinomLogitGlm>(ctx);
}
void blglm_bwd(KernelCtx& ctx) { tglm_eval<true, kBinomLogitGlm>(ctx); }
void clglm_fwd(KernelCtx& ctx) {
  ctx.out.data[0] = tglm_eval<false, kCatLogitGlm>(ctx);
}
void clglm_bwd(KernelCtx& ctx) { tglm_eval<true, kCatLogitGlm>(ctx); }
void olglm_fwd(KernelCtx& ctx) {
  ctx.out.data[0] = tglm_eval<false, kOrdLogisticGlm>(ctx);
}
void olglm_bwd(KernelCtx& ctx) { tglm_eval<true, kOrdLogisticGlm>(ctx); }

// ---- the cdfs the recorder cannot take ---------------------------------
// Same reason ordered_probit and wiener are here, one step further out:
// von_mises_cdf writes `res *= 0.0` and compares `x_n == -pi` on the
// scalar type, and neg_binomial_2_lcdf forms `phi / (phi + mu)`. Neither
// builds its result through stan-math's partials propagator, so neither
// can be handed an rvar. See STANLI_TAIL_CDF_LIST in optable.hpp.
//
// A length-1 slot enters stan-math as a scalar rather than a
// one-element vector: the sequence views broadcast a scalar but require
// vectors to match sizes. That is the same rule bind_args_m follows in
// densities_impl.hpp, and getting it wrong is not a size error -- it is
// every observation evaluated with element 0's parameters, which is what
// the sweep caught in the old scalar-only wiener tail.
using TailArg = std::variant<stan::math::var, VarV>;
TailArg tail_arg(const KernelCtx& ctx, int k) {
  if (ctx.in[k].len == 1) return stan::math::var(ctx.in[k].data[0]);
  return tail_v(ctx, k, ctx.in[k].len);
}

// finish_tail_density's shape, over arguments that are variants: one
// visit picks the scalar-or-vector instantiation, a second scatters each
// argument's adjoints back through the overload its own alternative
// selects.
template <bool Grad, typename F, typename... A>
double tail_visit(KernelCtx& ctx, F&& f, const A&... a) {
  stan::math::var out = std::visit(
      [&](const auto&... x) -> stan::math::var { return f(x...); }, a...);
  const double value = out.val();
  if constexpr (Grad) {
    stan::math::var seeded = out * ctx.out_adj;
    stan::math::grad(seeded.vi_);
    int slot = 0;
    ((std::visit([&](const auto& x) { tail_scatter(ctx, slot, x); }, a),
      ++slot),
     ...);
  }
  return value;
}

#define STANLI_TAIL_CDF_KERNEL(code, fn, nreal, tier)                        \
  template <bool Grad>                                                       \
  double fn##_eval(KernelCtx& ctx) {                                         \
    stan::math::nested_rev_autodiff nested;                                  \
    const TailArg a0 = tail_arg(ctx, 0), a1 = tail_arg(ctx, 1),              \
                  a2 = tail_arg(ctx, 2);                                     \
    return tail_visit<Grad>(                                                 \
        ctx, [](const auto&... x) { return stan::math::fn(x...); }, a0, a1,  \
        a2);                                                                 \
  }                                                                          \
  void fn##_fwd(KernelCtx& ctx) { ctx.out.data[0] = fn##_eval<false>(ctx); } \
  void fn##_bwd(KernelCtx& ctx) { fn##_eval<true>(ctx); }
STANLI_TAIL_CDF_LIST(STANLI_TAIL_CDF_KERNEL)
#undef STANLI_TAIL_CDF_KERNEL

// The integer outcome rides in idata as one whole group, the way the
// other integer-outcome cdfs read theirs; the lowering has already
// replicated a language-level scalar to the lane count.
#define STANLI_TAIL_INT_CDF_KERNEL(code, fn, nreal, tier)                    \
  template <bool Grad>                                                       \
  double fn##_eval(KernelCtx& ctx) {                                         \
    stan::math::nested_rev_autodiff nested;                                  \
    Eigen::Map<const Eigen::VectorXi> y(                                     \
        ctx.idata, static_cast<Eigen::Index>(ctx.n_idata));                  \
    const TailArg a0 = tail_arg(ctx, 0), a1 = tail_arg(ctx, 1);              \
    return tail_visit<Grad>(                                                 \
        ctx, [&](const auto&... x) { return stan::math::fn(y, x...); }, a0,  \
        a1);                                                                 \
  }                                                                          \
  void fn##_fwd(KernelCtx& ctx) { ctx.out.data[0] = fn##_eval<false>(ctx); } \
  void fn##_bwd(KernelCtx& ctx) { fn##_eval<true>(ctx); }
STANLI_TAIL_INT_CDF_LIST(STANLI_TAIL_INT_CDF_KERNEL)
#undef STANLI_TAIL_INT_CDF_KERNEL

}  // namespace

void register_matrix_kernels() {
  register_kernel(OP_GP_EXP_QUAD_COV, Kernel{gp_cov_fwd, gp_cov_bwd, nullptr});
  register_kernel(OP_WISHART_LPDF, Kernel{wish_fwd, wish_bwd, nullptr});
  register_kernel(OP_INV_WISHART_LPDF, Kernel{iwish_fwd, iwish_bwd, nullptr});
  register_kernel(OP_WISHART_CHOL_LPDF, Kernel{wishc_fwd, wishc_bwd, nullptr});
  register_kernel(OP_INV_WISHART_CHOL_LPDF,
                  Kernel{iwishc_fwd, iwishc_bwd, nullptr});
  register_kernel(OP_MULTI_GP_LPDF, Kernel{mgp_fwd, mgp_bwd, nullptr});
  register_kernel(OP_MULTI_GP_CHOL_LPDF, Kernel{mgpc_fwd, mgpc_bwd, nullptr});
  register_kernel(OP_MULTI_STUDENT_T_LPDF, Kernel{mst_fwd, mst_bwd, nullptr});
  register_kernel(OP_MULTI_STUDENT_T_CHOL_LPDF,
                  Kernel{mstc_fwd, mstc_bwd, nullptr});
  register_kernel(OP_MULTINOMIAL_LPMF, Kernel{multn_fwd, multn_bwd, nullptr});
  register_kernel(OP_MULTINOMIAL_LOGIT_LPMF,
                  Kernel{multnl_fwd, multnl_bwd, nullptr});
  register_kernel(OP_DIRICHLET_MULTINOMIAL_LPMF,
                  Kernel{dirmult_fwd, dirmult_bwd, nullptr});
  register_kernel(OP_ORDERED_PROBIT_LPMF,
                  Kernel{oprobit_fwd, oprobit_bwd, nullptr});
  register_kernel(OP_WIENER_LPDF, Kernel{wiener_fwd, wiener_bwd, nullptr});
#define STANLI_REGISTER_TAIL_CDF(code, fn, nreal, tier) \
  register_kernel(code, Kernel{fn##_fwd, fn##_bwd, nullptr});
  STANLI_TAIL_CDF_LIST(STANLI_REGISTER_TAIL_CDF)
  STANLI_TAIL_INT_CDF_LIST(STANLI_REGISTER_TAIL_CDF)
#undef STANLI_REGISTER_TAIL_CDF
  register_kernel(OP_LKJ_COV_LPDF, Kernel{lkjcov_fwd, lkjcov_bwd, nullptr});
  register_kernel(OP_BINOMIAL_LOGIT_GLM_LPMF,
                  Kernel{blglm_fwd, blglm_bwd, nullptr});
  register_kernel(OP_CATEGORICAL_LOGIT_GLM_LPMF,
                  Kernel{clglm_fwd, clglm_bwd, nullptr});
  register_kernel(OP_ORDERED_LOGISTIC_GLM_LPMF,
                  Kernel{olglm_fwd, olglm_bwd, nullptr});
  register_kernel(OP_DIAG_MATRIX, Kernel{diag_fwd, diag_bwd, nullptr});
  register_kernel(OP_CHOLESKY, Kernel{chol_fwd, chol_bwd, nullptr});
  register_kernel(OP_MULTI_NORMAL_CHOL_LPDF, Kernel{mnc_fwd, mnc_bwd, nullptr});
  register_kernel(OP_MULTI_NORMAL_LPDF, Kernel{mn_fwd, mn_bwd, nullptr});
  register_kernel(OP_MULTI_NORMAL_PREC_LPDF,
                  Kernel{mnprec_fwd, mnprec_bwd, nullptr});
  register_kernel(OP_GEMM, Kernel{gemm_fwd, gemm_bwd, nullptr});
  register_kernel(OP_EIGENVALUES_SYM,
                  Kernel{eigvals_fwd, eigvals_bwd, nullptr});
  register_kernel(OP_EIGENVECTORS_SYM,
                  Kernel{eigvecs_fwd, eigvecs_bwd, nullptr});
  register_kernel(OP_TRANSPOSE, Kernel{transpose_fwd, transpose_bwd, nullptr});
  register_kernel(OP_LKJ_CORR_CHOL_LPDF, Kernel{lkj_fwd, lkj_bwd, nullptr});
  register_kernel(OP_LKJ_CORR_LPDF, Kernel{lkjc_fwd, lkjc_bwd, nullptr});
  register_kernel(OP_NORMAL_ID_GLM_LPDF,
                  Kernel{nid_glm_fwd, nid_glm_bwd, nid_glm_scratch});
}

}  // namespace stanli
