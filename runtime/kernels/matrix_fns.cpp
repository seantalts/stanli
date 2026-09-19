// Matrix-valued legacy ops: GP covariance, cholesky_decompose, diag_matrix,
// multi_normal(_cholesky). Most forwards run the prim (double)
// implementation and backwards replay the same call on a nested var tape,
// seeded with the dot trick. The profiled single-vector Cholesky density below
// is the compact native exception: it retains Stan Math's closed-form matrix
// partials from forward to backward.
//
// Matrices live in slots column-major, matching Eigen and the rest of the
// pipeline, so a flat slot maps straight onto Map<MatrixXd>.
#include <stanli/graph.hpp>
#include <stanli/density_registry.hpp>
#include <stanli/recorder.hpp>
#include <stanli/legacy.hpp>
#include <stanli/optable.hpp>
#include <stanli/packet.hpp>

#include <stan/math.hpp>

#include <type_traits>
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

// ---- gp_*_cov(x, alpha, rho) ----------------------------------------------
// in = {x (data, N*D), alpha, rho}; idata = {N, D}; out = N*N column-major.
// x as array[N] real is D == 1; array[N] vector[D] flattens array-major.
// The variant selects the covariance function.
std::vector<VecD> gp_points(const KernelCtx& ctx) {
  const int64_t N = ctx.idata[0], D = ctx.idata[1];
  std::vector<VecD> pts(N, VecD(D));
  for (int64_t n = 0; n < N; ++n)
    for (int64_t d = 0; d < D; ++d) pts[n](d) = ctx.in[0].data[n * D + d];
  return pts;
}
template <typename P, typename S>
auto gp_cov_call(uint8_t variant, const P& pts, const S& sigma,
                 const S& length_scale) {
  switch (variant) {
    case kGpMatern32:
      return stan::math::gp_matern32_cov(pts, sigma, length_scale);
    case kGpMatern52:
      return stan::math::gp_matern52_cov(pts, sigma, length_scale);
    case kGpExponential:
      return stan::math::gp_exponential_cov(pts, sigma, length_scale);
    default:
      return stan::math::gp_exp_quad_cov(pts, sigma, length_scale);
  }
}
void gp_cov_fwd(KernelCtx& ctx) {
  const int64_t N = ctx.idata[0];
  auto pts = gp_points(ctx);
  MatD c = gp_cov_call(ctx.variant, pts, ctx.in[1].data[0], ctx.in[2].data[0]);
  MapM(ctx.out.data, N, N) = c;
}
void gp_cov_replay_bwd(KernelCtx& ctx) {
  if (!ctx.in_adj[0].data) {
    // Fixed locations need no distance derivatives. Keep the hyperparameters
    // and the weighted matrix reduction on the same Stan Math tape; only the
    // inactive geometry stays double. Active locations retain the full path.
    stan::math::nested_rev_autodiff nested;
    stan::math::var sigma = ctx.in[1].data[0], rho = ctx.in[2].data[0];
    const auto covariance =
        gp_cov_call(ctx.variant, gp_points(ctx), sigma, rho);
    const int64_t n = ctx.idata[0];
    if (n == 0) return;
    const stan::math::var objective = stan::math::sum(stan::math::elt_multiply(
        covariance, CMapM(ctx.out_adj_vec.data, n, n)));
    stan::math::grad(objective.vi_);
    if (ctx.in_adj[1].data) ctx.in_adj[1].data[0] += sigma.adj();
    if (ctx.in_adj[2].data) ctx.in_adj[2].data[0] += rho.adj();
    return;
  }
  // x may be a parameter: rebuild the points from the promoted xs[0] so its
  // adjoints flow back too. nary_bwd copies back whatever input carries an
  // adjoint slot, so a data x simply contributes nothing here.
  const int64_t N = ctx.idata[0], D = ctx.idata[1];
  const uint8_t variant = ctx.variant;
  nary_bwd(ctx, [&](std::vector<VarV>& xs) {
    std::vector<VarV> pts(N, VarV(D));
    for (int64_t n = 0; n < N; ++n)
      for (int64_t d = 0; d < D; ++d) pts[n](d) = xs[0](n * D + d);
    return gp_cov_call(variant, pts, xs[1](0), xs[2](0));
  });
}

// Match the pinned Stan Math fixed-location exp-quad callback's blocked
// reduction order. Scaling each term before summing is not interchangeable
// for nearly singular covariances: it amplifies rounding in hypergradients.
// See stan/math/rev/fun/gp_exp_quad_cov.hpp for the reference callback.
void gp_cov_bwd(KernelCtx& ctx) {
  const double sigma = ctx.in[1].data[0], rho = ctx.in[2].data[0];
  if (ctx.variant != kGpExpQuad || ctx.in_adj[0].data ||
      !std::isnormal(sigma * sigma) || !std::isnormal(rho * rho * rho)) {
    gp_cov_replay_bwd(ctx);
    return;
  }
  const int64_t n = ctx.idata[0], d = ctx.idata[1];
  if (n == 0) return;
  double covariance_sum = 0, distance_sum = 0;
  constexpr int64_t block = 10;
  for (int64_t jb = 0; jb < n; jb += block) {
    const int64_t j_end = std::min(n, jb + block);
    for (int64_t ib = jb; ib < n; ib += block) {
      const int64_t i_end = std::min(n, ib + block);
      for (int64_t j = jb; j < j_end; ++j) {
        for (int64_t i = std::max(ib, j + 1); i < i_end; ++i) {
          const double distance2 = (CMapV(ctx.in[0].data + i * d, d) -
                                    CMapV(ctx.in[0].data + j * d, d))
                                       .squaredNorm();
          const double seed = 0.0 + ctx.out_adj_vec.data[i * n + j] +
                              ctx.out_adj_vec.data[j * n + i];
          const double weighted = ctx.out.data[j * n + i] * seed;
          if (!std::isfinite(distance2) || !std::isfinite(weighted)) {
            gp_cov_replay_bwd(ctx);
            return;
          }
          distance_sum += weighted * distance2;
          covariance_sum += weighted;
        }
      }
    }
  }
  // Matrix<var> exposes its diagonal values through a scalar Eigen accessor.
  // Keep that reduction order: a packetized double-vector sum rounds
  // differently.
  Eigen::VectorXd diagonal(n), diagonal_adj(n);
  for (int64_t j = 0; j < n; ++j) {
    diagonal[j] = ctx.out.data[j * n + j];
    diagonal_adj[j] = 0.0 + ctx.out_adj_vec.data[j * n + j];
  }
  covariance_sum +=
      (diagonal.unaryExpr([](double value) { return value; }).array() *
       diagonal_adj.array())
          .sum();
  const double sigma_adj = covariance_sum * 2 / sigma;
  const double rho_adj = distance_sum / (rho * rho * rho);
  if (!std::isfinite(sigma_adj) || !std::isfinite(rho_adj)) {
    gp_cov_replay_bwd(ctx);
    return;
  }
  if (ctx.in_adj[1].data) ctx.in_adj[1].data[0] += sigma_adj;
  if (ctx.in_adj[2].data) ctx.in_adj[2].data[0] += rho_adj;
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

// ---- cholesky_decompose(A) ------------------------------------------------
void chol_fwd(KernelCtx& ctx) {
  const int64_t n = ctx.idata[0];
  MatD a = CMapM(ctx.in[0].data, n, n);
  MapM(ctx.out.data, n, n) = stan::math::cholesky_decompose(a);
}
// Minimal adjoint views let the pinned Stan Math pullbacks operate directly
// on the saved factor and executor adjoints, with no vari or nested tape.
struct MatrixAdjointView {
  MapM storage;
  Eigen::Index rows() const { return storage.rows(); }
  Eigen::Index cols() const { return storage.cols(); }
  MapM& adj() { return storage; }
};
void chol_bwd(KernelCtx& ctx) {
  if (!ctx.in_adj[0].data) return;
  const int64_t n = ctx.idata[0];
  CMapM factor(ctx.out.data, n, n);
  MatrixAdjointView out{MapM(ctx.out_adj_vec.data, n, n)};
  MatrixAdjointView in{MapM(ctx.in_adj[0].data, n, n)};
  if (n <= 35)
    stan::math::internal::unblocked_cholesky_lambda(factor, out, in)();
  else
    stan::math::internal::cholesky_lambda(factor, out, in)();
}

// ---- matrix_exp(A) ---------------------------------------------------------
void matrix_exp_fwd(KernelCtx& ctx) {
  const int64_t n = ctx.idata[0];
  MapM(ctx.out.data, n, n) =
      stan::math::matrix_exp(CMapM(ctx.in[0].data, n, n));
}
void matrix_exp_bwd_n(KernelCtx& ctx, int64_t n) {
  if (!ctx.in_adj[0].data || n == 0) return;
  const CMapM a(ctx.in[0].data, n, n);
  const CMapM g(ctx.out_adj_vec.data, n, n);
  const MatD at = a.transpose();
  const double anorm = at.cwiseAbs().colwise().sum().maxCoeff();
  const double gnorm = g.cwiseAbs().colwise().sum().maxCoeff();
  const double scale = gnorm > 0 ? gnorm / std::max(anorm, 1.0) : 1.0;
  MatD block = MatD::Zero(2 * n, 2 * n);
  block.topLeftCorner(n, n) = at;
  block.bottomRightCorner(n, n) = at;
  block.topRightCorner(n, n) = g / scale;
  const MatD e = stan::math::matrix_exp(block);
  MapM(ctx.in_adj[0].data, n, n) += e.topRightCorner(n, n) * scale;
}
void matrix_exp_bwd(KernelCtx& ctx) { matrix_exp_bwd_n(ctx, ctx.idata[0]); }
int64_t dynamic_square_extent(const KernelCtx& ctx) {
  if (ctx.n_in != 2 || ctx.in[1].len != 1)
    throw std::logic_error("dynamic matrix_exp extent is not scalar");
  const double raw = ctx.in[1].data[0];
  if (!std::isfinite(raw) || std::trunc(raw) != raw || raw < 0)
    throw std::domain_error("dynamic matrix_exp extent is invalid");
  const int64_t n = static_cast<int64_t>(raw);
  if (n != 0 && n > ctx.in[0].len / n)
    throw std::out_of_range("dynamic matrix_exp extent exceeds capacity");
  return n;
}
void matrix_exp_dynamic_fwd(KernelCtx& ctx) {
  const int64_t n = dynamic_square_extent(ctx);
  std::fill(ctx.out.data, ctx.out.data + ctx.out.len, 0.0);
  if (n != 0)
    MapM(ctx.out.data, n, n) =
        stan::math::matrix_exp(CMapM(ctx.in[0].data, n, n));
}
void matrix_exp_dynamic_bwd(KernelCtx& ctx) {
  const int64_t n = dynamic_square_extent(ctx);
  matrix_exp_bwd_n(ctx, n);
}

// ---- inverse / inverse_spd / log_determinant -----------------------------
// All three use Stan Math itself in both sweeps. Inverse and log_determinant
// have specialized rev overloads whose forward values are their double
// implementations. inverse_spd is a scalar-templated LDLT, so its active
// forward must run on Matrix<var> too: Eigen can otherwise choose different
// packet arithmetic from the CmdStan expression.
void inverse_fwd(KernelCtx& ctx) {
  const int64_t n = ctx.idata[0];
  MapM(ctx.out.data, n, n) = stan::math::inverse(CMapM(ctx.in[0].data, n, n));
}
void inverse_bwd(KernelCtx& ctx) {
  const int64_t n = ctx.idata[0];
  nary_bwd(ctx, [n](std::vector<VarV>& xs) {
    Eigen::Map<VarM> a(xs[0].data(), n, n);
    return stan::math::inverse(a);
  });
}

void inverse_spd_fwd(KernelCtx& ctx) {
  const int64_t n = ctx.idata[0];
  if (ctx.variant == 0) {
    MapM(ctx.out.data, n, n) =
        stan::math::inverse_spd(CMapM(ctx.in[0].data, n, n));
    return;
  }
  stan::math::nested_rev_autodiff nested;
  VarM a(n, n);
  for (int64_t i = 0; i < n * n; ++i) a.data()[i] = ctx.in[0].data[i];
  MapM(ctx.out.data, n, n) = stan::math::value_of(stan::math::inverse_spd(a));
}
void inverse_spd_bwd(KernelCtx& ctx) {
  const int64_t n = ctx.idata[0];
  nary_bwd(ctx, [n](std::vector<VarV>& xs) {
    Eigen::Map<VarM> a(xs[0].data(), n, n);
    return stan::math::inverse_spd(a);
  });
}

void log_det_fwd(KernelCtx& ctx) {
  const int64_t n = ctx.idata[0];
  ctx.out.data[0] = stan::math::log_determinant(CMapM(ctx.in[0].data, n, n));
}
void log_det_bwd(KernelCtx& ctx) {
  const int64_t n = ctx.idata[0];
  nary_bwd(ctx, [n](std::vector<VarV>& xs) {
    Eigen::Map<VarM> a(xs[0].data(), n, n);
    return stan::math::log_determinant(a);
  });
}

// ---- quad_form(A, B) ------------------------------------------------------
// in = {A (n x n), B (n x m)}; idata = {n, m}. Variant bits match the
// quad_form_sym convention: bit 0 marks vector B and bit 1 marks an active
// expression. The active vector overload associates B' * A * B, whereas the
// primitive overload uses B.dot(A * B).
void qf_fwd(KernelCtx& ctx) {
  const int64_t n = ctx.idata[0], m = ctx.idata[1];
  const CMapM a(ctx.in[0].data, n, n);
  if (!(ctx.variant & 1u)) {
    MapM(ctx.out.data, m, m) =
        stan::math::quad_form(a, CMapM(ctx.in[1].data, n, m));
    return;
  }
  const VecD b = CMapV(ctx.in[1].data, n);
  if (!(ctx.variant & 2u)) {
    ctx.out.data[0] = stan::math::quad_form(a, b);
    return;
  }
  stan::math::check_square("quad_form", "A", a);
  stan::math::check_multiplicable("quad_form", "A", a, "B", b);
  const MatD c = b.transpose() * a * b;
  ctx.out.data[0] = c(0, 0);
}
void qf_bwd_impl(KernelCtx& ctx, int64_t n, int64_t m, bool vec) {
  if (!ctx.in_adj[0].data && !ctx.in_adj[1].data) return;
  const CMapM a(ctx.in[0].data, n, n);
  const CMapM b(ctx.in[1].data, n, m);
  MatD g(m, m);
  if (vec) {
    g(0, 0) = ctx.out_adj;
  } else {
    g = CMapM(ctx.out_adj_vec.data, m, m);
  }
  if (ctx.in_adj[0].data)
    MapM(ctx.in_adj[0].data, n, n) += b * g * b.transpose();
  if (ctx.in_adj[1].data)
    MapM(ctx.in_adj[1].data, n, m) +=
        a * b * g.transpose() + a.transpose() * b * g;
}
void qf_bwd(KernelCtx& ctx) {
  const int64_t n = ctx.idata[0];
  const bool vec = ctx.variant & 1u;
  qf_bwd_impl(ctx, n, vec ? 1 : ctx.idata[1], vec);
}

// ---- add_diag(A, d) -------------------------------------------------------
// idata = {rows, cols}; variant 0 is a vector diagonal, 1 is a scalar.
void add_diag_fwd(KernelCtx& ctx) {
  const int64_t rows = ctx.idata[0], cols = ctx.idata[1];
  MapM out(ctx.out.data, rows, cols);
  out = CMapM(ctx.in[0].data, rows, cols);
  const int64_t n = std::min(rows, cols);
  for (int64_t i = 0; i < n; ++i)
    out(i, i) += ctx.in[1].data[ctx.variant == 1 ? 0 : i];
}
void add_diag_bwd(KernelCtx& ctx) {
  const int64_t rows = ctx.idata[0], cols = ctx.idata[1];
  if (ctx.in_adj[0].data)
    MapM(ctx.in_adj[0].data, rows, cols) +=
        CMapM(ctx.out_adj_vec.data, rows, cols);
  if (!ctx.in_adj[1].data) return;
  const int64_t n = std::min(rows, cols);
  if (ctx.variant == 1) {
    // Eigen creates the diagonal scalar additions in increasing coefficient
    // order; Stan's tape replays them in reverse.
    for (int64_t i = n; i-- > 0;)
      ctx.in_adj[1].data[0] += ctx.out_adj_vec.data[i * rows + i];
  } else {
    for (int64_t i = 0; i < n; ++i)
      ctx.in_adj[1].data[i] += ctx.out_adj_vec.data[i * rows + i];
  }
}

// ---- quad_form_sym(A, B) --------------------------------------------------
// in = {A (n x n), B (n x m)}; idata = {n, m}. The output is the m x m
// matrix 0.5 * (C + C') with C = B' A B, or the single scalar extracted from
// that 1 x 1 symmetrised matrix when B is a reverse-mode vector. The latter
// still performs the add and multiply: although algebraically redundant,
// those operations affect IEEE overflow and must match CmdStan exactly.
// stan-math checks A for symmetry and throws what CmdStan would when it is
// not.
//
// Variant bit 0 says the second operand is a vector; bit 1 says CmdStan
// would have typed this expression `var`. Only the vector overload needs
// that second bit, because stan-math associates it two ways: prim computes
// B.dot(A * B), a gemv and then a dot, while the rev path builds a
// quad_form_vari over a column vector and evaluates B' * A * B, grouping
// from the other end. The matrix overload has one association in both.
void qfs_fwd(KernelCtx& ctx) {
  const int64_t n = ctx.idata[0], m = ctx.idata[1];
  const CMapM a(ctx.in[0].data, n, n);
  if (!(ctx.variant & 1u)) {
    const CMapM b(ctx.in[1].data, n, m);
    MapM(ctx.out.data, m, m) = stan::math::quad_form_sym(a, b);
    return;
  }
  const VecD b = CMapV(ctx.in[1].data, n);
  if (!(ctx.variant & 2u)) {
    ctx.out.data[0] = stan::math::quad_form_sym(a, b);
    return;
  }
  stan::math::check_multiplicable("quad_form_sym", "A", a, "B", b);
  stan::math::check_symmetric("quad_form_sym", "A", a);
  MatD c = b.transpose() * a * b;
  const MatD sym = 0.5 * (c + c.transpose());
  c = sym;
  ctx.out.data[0] = c(0, 0);
}
void qfs_bwd(KernelCtx& ctx) {
  const int64_t n = ctx.idata[0];
  const bool vec = ctx.variant & 1u;
  qf_bwd_impl(ctx, n, vec ? 1 : ctx.idata[1], vec);
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

double* tail_stash(double* s, const VarM& M) {
  for (int64_t i = 0; i < M.size(); ++i) *s++ = M.data()[i].adj();
  return s;
}
double* tail_stash(double* s, const VarV& v) {
  for (int64_t i = 0; i < v.size(); ++i) *s++ = v(i).adj();
  return s;
}
double* tail_stash(double* s, const stan::math::var& x) {
  *s++ = x.adj();
  return s;
}
double* tail_stash(double* s, const std::vector<VarV>& xs) {
  for (const auto& x : xs) s = tail_stash(s, x);
  return s;
}

// finish_tail_density split across the sweeps: one tape per gradient, gradded
// in the FORWARD with a seed of 1, contracted in the backward. That is only
// bitwise for a density whose reverse sweep multiplies the output adjoint in
// once per operand -- the partials_propagator family. Densities that reduce
// through var arithmetic (wishart, lkj, wiener, multi_normal_prec) round the
// two orders differently and stay on the two-tape form above.
template <typename... Args>
[[gnu::always_inline]] inline double tail_density_fwd(
    KernelCtx& ctx, const stan::math::var& density, const Args&... args) {
  const double value = density.val();
  if (!values_only()) {
    stan::math::grad(density.vi_);
    double* s = ctx.scratch;
    ((s = tail_stash(s, args)), ...);
  }
  return value;
}

// Mirror of tail_density_fwd's layout: one partial per element of the first
// NArgs inputs, in slot order.
template <int NArgs>
void tail_density_bwd(KernelCtx& ctx) {
  const double* s = ctx.scratch;
  const double w = ctx.out_adj;
  for (int k = 0; k < NArgs; ++k) {
    if (ctx.in_adj[k].data)
      Eigen::Map<Eigen::ArrayXd>(ctx.in_adj[k].data, ctx.in[k].len) +=
          w * Eigen::Map<const Eigen::ArrayXd>(s, ctx.in[k].len);
    s += ctx.in[k].len;
  }
}

template <int NArgs>
int64_t tail_density_scratch(const Op& op, const Slot* slots) {
  int64_t t = 0;
  for (int k = 0; k < NArgs && k < op.n_in; ++k) {
    if (op.in[k] < 0) return 0;
    t += slots[op.in[k]].len;
  }
  return t;
}

template <int NArgs>
sink recorded_tail_sink(KernelCtx& ctx) {
  sink s;
  int64_t offset = 0;
  for (int k = 0; k < NArgs; ++k) {
    s.buf[k] = ctx.scratch + offset;
    s.len[k] = ctx.in[k].len;
    offset += s.len[k];
  }
  s.connected = ctx.scratch + offset;
  return s;
}
template <int NArgs>
void recorded_tail_bwd(KernelCtx& ctx) {
  int64_t offset = 0;
  for (int k = 0; k < NArgs; ++k) offset += ctx.in[k].len;
  if (ctx.scratch[offset] != 0.0) tail_density_bwd<NArgs>(ctx);
}
template <int NArgs>
int64_t recorded_tail_scratch(const Op& op, const Slot* slots) {
  return tail_density_scratch<NArgs>(op, slots) + 1;
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

inline int64_t mvt_count(int encoded) { return encoded < 0 ? 1 : encoded; }

template <typename Scalar, typename Input>
std::vector<Eigen::Matrix<Scalar, Eigen::Dynamic, 1>> mvt_vectors(
    const Input& input, int64_t width, int encoded) {
  using Vector = Eigen::Matrix<Scalar, Eigen::Dynamic, 1>;
  const int64_t count = mvt_count(encoded);
  std::vector<Vector> result((size_t)count, Vector(width));
  for (int64_t k = 0; k < count; ++k)
    for (int64_t i = 0; i < width; ++i)
      result[(size_t)k](i) = input.data[k * width + i];
  return result;
}

template <typename Vectors, typename F>
decltype(auto) with_mvt_argument(Vectors& values, int encoded, F&& f) {
  return encoded < 0 ? std::forward<F>(f)(values[0])
                     : std::forward<F>(f)(values);
}

template <bool Grad, MnKind Kind = kMnCov>
double mn_eval(KernelCtx& ctx) {
  const int64_t n = ctx.idata[0];
  // New calls encode each vectorized argument independently. Retain the old
  // {width, repetitions} interpretation for serialized/direct test graphs.
  const int y_encoded =
      ctx.n_idata > 2
          ? ctx.idata[1]
          : (ctx.n_idata > 1 && ctx.idata[1] > 1 ? ctx.idata[1] : -1);
  const int mu_encoded = ctx.n_idata > 2 ? ctx.idata[2] : -1;
  const bool propto = (ctx.variant & 0x80u) != 0;
  const unsigned mask = ctx.variant == 0 ? 0x7u : (ctx.variant & 0x3fu);
  const bool ay = mask & 1u, am = mask & 2u, aS = mask & 4u;
  stan::math::nested_rev_autodiff nested;
  using stan::math::var;
  // Bind only the representation selected by activity. Inactive var copies
  // and active double copies were never passed to Stan Math. Precision's
  // replay retains its original inactive-var scatter contract.
  std::vector<VarV> ys, mus;
  std::vector<VecD> ysd, musd;
  if (ay || Kind == kMnPrec) ys = mvt_vectors<var>(ctx.in[0], n, y_encoded);
  if (!ay) ysd = mvt_vectors<double>(ctx.in[0], n, y_encoded);
  if (am || Kind == kMnPrec) mus = mvt_vectors<var>(ctx.in[1], n, mu_encoded);
  if (!am) musd = mvt_vectors<double>(ctx.in[1], n, mu_encoded);
  VarM S;
  if (aS || Kind == kMnPrec) {
    S.resize(n, n);
    for (int64_t j = 0; j < n; ++j)
      for (int64_t i = 0; i < n; ++i) S(i, j) = ctx.in[2].data[j * n + i];
  }
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
  const auto dispatch_mu = [&](auto&& y) -> var {
    const auto invoke = [&](auto&& mu) -> var {
      return aS ? call(y, mu, S) : call(y, mu, Sd);
    };
    return am ? with_mvt_argument(mus, mu_encoded, invoke)
              : with_mvt_argument(musd, mu_encoded, invoke);
  };
  var out = ay ? with_mvt_argument(ys, y_encoded, dispatch_mu)
               : with_mvt_argument(ysd, y_encoded, dispatch_mu);

  if constexpr (Kind == kMnPrec) {
    return finish_tail_density<Grad>(ctx, out, ys, mus, S);
  } else {
    if (!values_only()) {
      stan::math::grad(out.vi_);
      double* partial = ctx.scratch;
      const auto stash = [&](int k, bool active, const auto& operand) {
        if (active)
          tail_stash(partial, operand);
        else
          std::fill_n(partial, ctx.in[k].len, 0.0);
        partial += ctx.in[k].len;
      };
      stash(0, ay, ys);
      stash(1, am, mus);
      stash(2, aS, S);
    }
    return out.val();
  }
}
bool mn_single_shape(const KernelCtx& ctx) {
  if (ctx.n_in != 3 || ctx.n_idata < 1 || ctx.idata == nullptr ||
      ctx.idata[0] < 0 || ctx.out.len != 1)
    return false;
  // Match mn_eval's legacy repetition encoding as well as its independent
  // vector/array descriptors. Array operands keep the established path.
  if (ctx.n_idata > 2 ? (ctx.idata[1] >= 0 || ctx.idata[2] >= 0)
                      : (ctx.n_idata > 1 && ctx.idata[1] > 1))
    return false;
  const int64_t n = ctx.idata[0];
  return ctx.in[0].len == n && ctx.in[1].len == n && ctx.in[2].len == n * n;
}

// The single-vector overload in Stan Math's prim/prob/multi_normal_lpdf.hpp
// computes double partials before constructing its autodiff edges. Keep its
// validation and activity-specific arithmetic, depositing those partials in
// scratch instead of allocating three var operands and traversing their tape.
// In particular, active covariance uses an explicit inverse for the value;
// inactive covariance uses the solve. Substituting one for the other changes
// rounding. The shape gate proves the overload's three size comparisons.
double mn_single_fwd(KernelCtx& ctx) {
  static constexpr const char* function = "multi_normal_lpdf";
  const int64_t n = ctx.idata[0];
  const unsigned mask = ctx.variant == 0 ? 7u : (ctx.variant & 7u);
  const bool propto = (ctx.variant & 0x80u) != 0;
  const bool record = !values_only();
  CMapV y(ctx.in[0].data, n), mu(ctx.in[1].data, n);
  CMapM sigma(ctx.in[2].data, n, n);
  stan::math::check_positive(function, "Covariance matrix rows", sigma.rows());
  stan::math::check_finite(function, "Location parameter", mu);
  stan::math::check_not_nan(function, "Random variable", y);
  stan::math::check_symmetric(function, "Covariance matrix", sigma);
  auto factor = stan::math::make_ldlt_factor(sigma);
  stan::math::check_ldlt_factor(function, "LDLT_Factor of covariance parameter",
                                factor);
  if (record) std::fill_n(ctx.scratch, 2 * n + n * n, 0.0);

  double logp = 0.0;
  if (!propto) logp += stan::math::NEG_LOG_SQRT_TWO_PI * n;
  if (mask != 0 || !propto) {
    VecD half(n);
    VecD difference = (y - mu).eval();
    if (!(mask & 4u)) {
      half = stan::math::mdivide_left_ldlt(factor, difference);
      if (!propto) logp += -0.5 * stan::math::log_determinant_ldlt(factor);
    } else {
      MatD inverse =
          stan::math::mdivide_left_ldlt(factor, MatD::Identity(n, n));
      half.noalias() = inverse * difference;
      logp += -0.5 * stan::math::log_determinant_ldlt(factor);
      if (record)
        MapM(ctx.scratch + 2 * n, n, n) +=
            0.5 * (half * half.transpose() - inverse);
    }
    logp += -0.5 * stan::math::dot_product(difference, half);
    if (record && (mask & 1u)) Eigen::Map<VecD>(ctx.scratch, n) += -half;
    if (record && (mask & 2u)) Eigen::Map<VecD>(ctx.scratch + n, n) += half;
  }
  return logp;
}

void mn_fwd(KernelCtx& ctx) {
  ctx.out.data[0] =
      mn_single_shape(ctx) ? mn_single_fwd(ctx) : mn_eval<false>(ctx);
}
void mn_bwd(KernelCtx& ctx) { tail_density_bwd<3>(ctx); }
void mnprec_fwd(KernelCtx& ctx) {
  ctx.out.data[0] = mn_eval<false, kMnPrec>(ctx);
}
void mnprec_bwd(KernelCtx& ctx) { mn_eval<true, kMnPrec>(ctx); }

// gp_regr's single-observation Cholesky density has data y and mu, active L,
// and propto=true. In that exact instantiation Stan Math's partials
// propagator computes a closed-form L pullback in doubles, then builds a var
// edge around it. Retain that matrix in scratch during the forward instead of
// rebuilding an AoS var matrix and nested tape in both sweeps. The equality
// checks here are deliberately strict: every other activity, propto, and
// vectorized shape stays on mn_eval's generic replay.
inline bool mnc_native_variant(uint8_t variant, const int* idata,
                               int64_t n_idata) {
  return variant == 0x84u && idata != nullptr && idata[0] >= 0 &&
         ((n_idata == 2 && idata[1] == 1) ||
          (n_idata == 3 && idata[1] == -1 && idata[2] == -1));
}

bool mnc_native_shape(const KernelCtx& ctx) {
  if (!mnc_native_variant(ctx.variant, ctx.idata, ctx.n_idata) ||
      ctx.n_in != 3 || ctx.out.len != 1)
    return false;
  const int64_t n = ctx.idata[0];
  return ctx.in[0].len == n && ctx.in[1].len == n && ctx.in[2].len == n * n;
}

double mnc_native_fwd(KernelCtx& ctx) {
  static constexpr const char* function = "multi_normal_cholesky_lpdf";
  const int64_t n = ctx.idata[0];
  CMapV y(ctx.in[0].data, n), mu(ctx.in[1].data, n);
  CMapM L(ctx.in[2].data, n, n);

  // Copy the pinned single-vector Stan Math overload's checks and
  // arithmetic order. That overload intentionally does not call
  // check_cholesky_factor; changing its observable domain here would make the
  // native and fallback paths disagree.
  stan::math::check_size_match(function, "Size of random variable", y.size(),
                               "size of location parameter", mu.size());
  stan::math::check_size_match(function, "Size of random variable", y.size(),
                               "rows of covariance parameter", L.rows());
  stan::math::check_size_match(function, "Size of random variable", y.size(),
                               "columns of covariance parameter", L.cols());
  stan::math::check_finite(function, "Location parameter", mu);
  stan::math::check_not_nan(function, "Random variable", y);
  if (n == 0) return 0.0;

  VecD y_minus_mu = y - mu;
  MatD inv_L = stan::math::mdivide_left_tri<Eigen::Lower>(L);
  Eigen::RowVectorXd half;
  half = (inv_L.template triangularView<Eigen::Lower>() *
          y_minus_mu.template cast<double>())
             .transpose();

  VecD scaled_diff;
  if (!values_only()) {
    scaled_diff =
        (half * inv_L.template triangularView<Eigen::Lower>()).transpose();
  }

  double logp(0.0);
  logp += stan::math::sum(stan::math::log(inv_L.diagonal()));
  if (!values_only())
    MapM(ctx.scratch, n, n) = scaled_diff * half - inv_L.transpose();
  logp -= 0.5 * stan::math::sum(stan::math::dot_self(half));
  return logp;
}

void mnc_fwd(KernelCtx& ctx) {
  ctx.out.data[0] = mnc_native_shape(ctx) ? mnc_native_fwd(ctx)
                                          : mn_eval<false, kMnChol>(ctx);
}
void mnc_bwd(KernelCtx& ctx) {
  if (!mnc_native_shape(ctx)) {
    tail_density_bwd<3>(ctx);
    return;
  }
  if (!ctx.in_adj[2].data) return;
  const int64_t n = ctx.idata[0];
  for (int64_t i = 0; i < n * n; ++i)
    ctx.in_adj[2].data[i] += ctx.out_adj * ctx.scratch[i];
}

// Everything that misses the native gate replays through mn_eval, which
// stashes one partial per input element instead of the pullback's n*n.
int64_t mnc_scratch(const Op& op, const Slot* slots) {
  if (op.n_in != 3 || op.out < 0 || slots == nullptr || op.in[0] < 0 ||
      op.in[1] < 0 || op.in[2] < 0)
    return 0;
  if (!mnc_native_variant(op.variant, op.idata, op.n_idata))
    return tail_density_scratch<3>(op, slots);
  const int64_t n = op.idata[0];
  if (slots[op.in[0]].len != n || slots[op.in[1]].len != n ||
      slots[op.in[2]].len != n * n || slots[op.out].len != 1)
    return tail_density_scratch<3>(op, slots);
  return n * n;
}

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

// ---- crossprod(A): out = A' * A ------------------------------------------
// idata = {rows, cols}; variant bit 0 records an autodiff result. Stan Math's
// double overload uses a symmetric rank update, while its reverse-mode
// overload computes the value as a general matrix product. Both groupings are
// observable, so forward-only evaluation takes the former and a gradient
// evaluation takes the latter, matching the overload CmdStan instantiates.
void crossprod_fwd(KernelCtx& ctx) {
  const int64_t rows = ctx.idata[0], cols = ctx.idata[1];
  const CMapM a(ctx.in[0].data, rows, cols);
  if ((ctx.variant & 1u) && !values_only())
    MapM(ctx.out.data, cols, cols) = a.transpose() * a;
  else
    MapM(ctx.out.data, cols, cols) = stan::math::crossprod(a);
}
void crossprod_bwd(KernelCtx& ctx) {
  const int64_t rows = ctx.idata[0], cols = ctx.idata[1];
  nary_bwd(ctx, [rows, cols](std::vector<VarV>& xs) {
    Eigen::Map<VarM> a(xs[0].data(), rows, cols);
    return stan::math::crossprod(a);
  });
}

// ---- multiply_lower_tri_self_transpose(L): out = tril(L) * tril(L)' -------
// idata = {rows, cols}; variant bit 0 records an autodiff result. The upper
// triangle is dropped, not read: a plain L * L' agrees only when L already
// has zeros above the diagonal, which a cholesky_factor_* parameter does and
// an ordinary matrix does not. As with crossprod the two stan-math overloads
// group the arithmetic differently -- the double one accumulates each entry
// as a dot product over the shared head of two columns, the reverse-mode one
// masks first and forms a triangular-times-dense product -- so a forward-only
// evaluation takes the former and a gradient evaluation the latter.
void mlt_self_transpose_fwd(KernelCtx& ctx) {
  const int64_t rows = ctx.idata[0], cols = ctx.idata[1];
  const CMapM a(ctx.in[0].data, rows, cols);
  if ((ctx.variant & 1u) && !values_only()) {
    const MatD masked = a.triangularView<Eigen::Lower>();
    MapM(ctx.out.data, rows, rows) =
        masked.triangularView<Eigen::Lower>() * masked.transpose();
  } else {
    MapM(ctx.out.data, rows, rows) =
        stan::math::multiply_lower_tri_self_transpose(a);
  }
}
void mlt_self_transpose_bwd(KernelCtx& ctx) {
  const int64_t rows = ctx.idata[0], cols = ctx.idata[1];
  if (!ctx.in_adj[0].data || !rows || !cols) return;
  const CMapM a(ctx.in[0].data, rows, cols);
  const MatD masked = a.triangularView<Eigen::Lower>();
  // Match the existing weighted-sum tape's initially zero output adjoints,
  // then its triangular callback and final input scatter. In particular,
  // direct assignment of the seed would change negative-zero behavior.
  const MatD seed =
      (CMapM(ctx.out_adj_vec.data, rows, rows).array() + 0.0).matrix();
  MatD local = MatD::Zero(rows, cols);
  local += ((seed.transpose() + seed) * masked.triangularView<Eigen::Lower>())
               .triangularView<Eigen::Lower>();
  for (int64_t i = 0; i < ctx.in[0].len; ++i)
    ctx.in_adj[0].data[i] += local.data()[i];
}

// ---- matrix solves: `A \ B` and `B / A` -----------------------------------
// Argument order is the operator's, so in = {A, B} for the left solve and
// {B, A} for the right one; idata = {n, k} with n the divisor's order and k
// the dividend's other extent. Output has the dividend's shape.
//
// Both operand types are part of the answer here, not just of the speed,
// because stan-math solves through whatever it is handed:
//
//   * scalar type. mdivide_left at double is prim, a FullPivLU solve; at var
//     it is the hand-written rev overload, whose value is a HouseholderQR
//     solve instead. mdivide_right has no rev overload at all, so the prim
//     template runs at whatever scalar type reaches it. Variant bit 0 says
//     the result is active, bits 2 and 3 preserve the divisor and dividend
//     scalar types independently. That distinction also selects stan-math's
//     vv/vd/dv SPD and triangular pullbacks, whose floating-point association
//     is observably different.
//   * Eigen shape. A vector dividend passed as a one-column matrix takes
//     Eigen's matrix code paths, which reassociate: 1 ULP on the value and
//     on the adjoint, measured on `A \ v`. Variant bit 1 says the dividend
//     is a vector (a column under `\`, a row under `/`) rather than a
//     matrix that happens to be narrow.
//
// The MIR interpreter makes both distinctions too -- the scalar one falls
// out of overload resolution on its own T -- which is what keeps the two
// halves of the runtime answering the same thing.
//   * factorisation family. The Stan language names three, and they are
//     different answers rather than different speeds: the plain solve
//     factors a general matrix, `_spd` takes an LLT of a symmetric positive
//     definite one, and `_tri_low` reads only the lower triangle and never
//     looks at the rest. Each gets its own opcode; `Kind` selects the call.
enum class SolveKind { Plain, Spd, TriLow };

template <bool Left, SolveKind Kind, typename A, typename B>
auto solve_at(const A& a, const B& b) {
  if constexpr (Kind == SolveKind::Spd) {
    if constexpr (Left) {
      return stan::math::mdivide_left_spd(a, b);
    } else {
      return stan::math::mdivide_right_spd(b, a);
    }
  } else if constexpr (Kind == SolveKind::TriLow) {
    if constexpr (Left) {
      return stan::math::mdivide_left_tri_low(a, b);
    } else {
      return stan::math::mdivide_right_tri_low(b, a);
    }
  } else {
    if constexpr (Left) {
      return stan::math::mdivide_left(a, b);
    } else {
      return stan::math::mdivide_right(b, a);
    }
  }
}

// The Eigen type of a dividend: a matrix, or the vector its side implies.
template <bool Left, bool Vec, typename T>
using Dividend = std::conditional_t<
    !Vec, Eigen::Matrix<T, -1, -1>,
    std::conditional_t<Left, Eigen::Matrix<T, -1, 1>, Eigen::Matrix<T, 1, -1>>>;

// Replay the exact operand scalar types on a nested tape and write the
// value out. The mixed stan-math overloads are numerically distinct from
// promoting their data operand to var, so the two activity flags are
// template parameters rather than a single result-active flag.
template <bool Left, SolveKind Kind, bool DivisorVar, bool DividendVar,
          bool Vec>
void solve_var(KernelCtx& ctx) {
  using stan::math::var;
  static_assert(DivisorVar || DividendVar);
  using DivisorScalar = std::conditional_t<DivisorVar, var, double>;
  using DividendScalar = std::conditional_t<DividendVar, var, double>;
  const int64_t n = ctx.idata[0], k = ctx.idata[1];
  const int ai = Left ? 0 : 1, bi = Left ? 1 : 0;
  const int64_t br = Left ? n : k, bc = Left ? k : n;
  stan::math::nested_rev_autodiff nested;
  Eigen::Matrix<DivisorScalar, -1, -1> a(n, n);
  for (int64_t i = 0; i < n * n; ++i) a.data()[i] = ctx.in[ai].data[i];
  Dividend<Left, Vec, DividendScalar> b(br, bc);
  for (int64_t i = 0; i < br * bc; ++i) b.data()[i] = ctx.in[bi].data[i];
  auto out = solve_at<Left, Kind>(a, b);
  for (Eigen::Index i = 0; i < out.size(); ++i)
    ctx.out.data[i] = out.data()[i].val();
}

template <SolveKind Kind, Eigen::UpLoType Tri = Eigen::Lower>
void left_adjoint(const MatD& a, const MatD& x, const MatD& g, bool divisor_var,
                  bool dividend_var, MatD* adj_a, MatD* adj_b) {
  if constexpr (Kind == SolveKind::Spd) {
    const Eigen::LLT<MatD> fac(a);
    if (dividend_var) {
      const MatD y = fac.solve(g);
      if (adj_b) *adj_b += y;
      if (divisor_var) *adj_a -= y * x.transpose();
    } else if (divisor_var) {
      *adj_a -= fac.solve(MatD(g * x.transpose()));
    }
  } else if constexpr (Kind == SolveKind::TriLow) {
    const auto tri = a.template triangularView<Tri>();
    if (dividend_var) {
      const MatD y = tri.transpose().solve(g);
      if (adj_b) *adj_b += y;
      if (divisor_var) {
        MatD full = MatD::Zero(a.rows(), a.cols());
        full.template triangularView<Tri>() = -(y * x.transpose());
        *adj_a += full;
      }
    } else if (divisor_var) {
      MatD full = MatD::Zero(a.rows(), a.cols());
      full.template triangularView<Tri>() =
          -MatD(tri.transpose().solve(MatD(g * x.transpose())));
      *adj_a += full;
    }
  } else {
    const Eigen::HouseholderQR<MatD> qr(a);
    const MatD y =
        qr.householderQ() * MatD(qr.matrixQR()
                                     .template triangularView<Eigen::Upper>()
                                     .transpose()
                                     .solve(g));
    if (dividend_var && adj_b) *adj_b += y;
    if (divisor_var) *adj_a -= y * x.transpose();
  }
}

template <SolveKind Kind>
void right_adjoint(const MatD& a, const MatD& x, const MatD& g,
                   bool divisor_var, bool dividend_var, MatD* adj_a,
                   MatD* adj_b) {
  constexpr Eigen::UpLoType kFlipped =
      Kind == SolveKind::TriLow ? Eigen::Upper : Eigen::Lower;
  MatD adj_a_t = MatD::Zero(a.cols(), a.rows());
  MatD adj_b_t = MatD::Zero(x.cols(), x.rows());
  left_adjoint<Kind, kFlipped>(
      a.transpose(), x.transpose(), g.transpose(), divisor_var, dividend_var,
      divisor_var ? &adj_a_t : nullptr, dividend_var ? &adj_b_t : nullptr);
  if (divisor_var) {
    if constexpr (Kind == SolveKind::Spd) {
      *adj_a += adj_a_t;
    } else {
      *adj_a += adj_a_t.transpose();
    }
  }
  if (dividend_var) *adj_b += adj_b_t.transpose();
}

template <bool Left, SolveKind Kind, bool Vec>
void solve_double(KernelCtx& ctx) {
  const int64_t n = ctx.idata[0], k = ctx.idata[1];
  const int ai = Left ? 0 : 1, bi = Left ? 1 : 0;
  const int64_t br = Left ? n : k, bc = Left ? k : n;
  MatD a = CMapM(ctx.in[ai].data, n, n);
  Dividend<Left, Vec, double> b(br, bc);
  for (int64_t i = 0; i < br * bc; ++i) b.data()[i] = ctx.in[bi].data[i];
  auto out = solve_at<Left, Kind>(a, b);
  for (Eigen::Index i = 0; i < out.size(); ++i) ctx.out.data[i] = out.data()[i];
}

template <bool Left, SolveKind Kind = SolveKind::Plain>
void solve_active_fwd(KernelCtx& ctx, bool vec) {
  // Bits 2 and 3 are the exact divisor/dividend scalar types. Activity 0 on
  // an active result is the pre-detail encoding; retain its old vv behavior
  // for an in-memory graph built by an older caller.
  switch ((ctx.variant >> 2u) & 3u) {
    case 1u:
      vec ? solve_var<Left, Kind, true, false, true>(ctx)
          : solve_var<Left, Kind, true, false, false>(ctx);
      return;
    case 2u:
      vec ? solve_var<Left, Kind, false, true, true>(ctx)
          : solve_var<Left, Kind, false, true, false>(ctx);
      return;
    default:
      vec ? solve_var<Left, Kind, true, true, true>(ctx)
          : solve_var<Left, Kind, true, true, false>(ctx);
      return;
  }
}

template <bool Left, SolveKind Kind = SolveKind::Plain>
void solve_fwd(KernelCtx& ctx) {
  const bool vec = (ctx.variant & 2u) != 0;
  // forward_value_only() is CmdStan's log_prob<double> path. Even when the
  // same graph carries adjoints for gradient(), it must take the prim solve:
  // mdivide_left's active overload uses HouseholderQR while its double
  // overload uses FullPivLU, and those are observably different answers on
  // ill-conditioned inputs.
  if ((ctx.variant & 1u) && !values_only()) {
    solve_active_fwd<Left, Kind>(ctx, vec);
  } else {
    vec ? solve_double<Left, Kind, true>(ctx)
        : solve_double<Left, Kind, false>(ctx);
  }
}

template <bool Left, SolveKind Kind = SolveKind::Plain>
void solve_bwd(KernelCtx& ctx) {
  const int64_t n = ctx.idata[0], k = ctx.idata[1];
  const int ai = Left ? 0 : 1, bi = Left ? 1 : 0;
  const int64_t br = Left ? n : k, bc = Left ? k : n;
  bool divisor_var = true, dividend_var = true;
  switch ((ctx.variant >> 2u) & 3u) {
    case 1u:
      dividend_var = false;
      break;
    case 2u:
      divisor_var = false;
      break;
    default:
      break;
  }
  const CMapM a(ctx.in[ai].data, n, n);
  const CMapM x(ctx.out.data, br, bc);
  const CMapM g(ctx.out_adj_vec.data, br, bc);
  MatD adj_a = MatD::Zero(n, n);
  MatD adj_b = MatD::Zero(br, bc);
  if constexpr (Left) {
    left_adjoint<Kind>(a, x, g, divisor_var, dividend_var,
                       divisor_var ? &adj_a : nullptr,
                       dividend_var ? &adj_b : nullptr);
  } else {
    right_adjoint<Kind>(a, x, g, divisor_var, dividend_var,
                        divisor_var ? &adj_a : nullptr,
                        dividend_var ? &adj_b : nullptr);
  }
  if (divisor_var && ctx.in_adj[ai].data)
    MapM(ctx.in_adj[ai].data, n, n) += adj_a;
  if (dividend_var && ctx.in_adj[bi].data)
    MapM(ctx.in_adj[bi].data, br, bc) += adj_b;
}

// ---- lkj_corr_cholesky_lpdf(L | eta) --------------------------------------
// in = {L, eta}; idata = {K}. eta is an ordinary differentiable shape
// parameter (variant bit 1); binding it as data used to zero d(lp)/d(eta)
// silently and, under propto, drop normalization terms that are not
// constant when eta is a parameter -- a 0.5% CmdStan gradient divergence
// found by the signature-reference gate. The double binding remains the
// data fast path and keeps that instantiation's tape unchanged.
template <bool Grad, bool Chol = true>
double lkj_eval(KernelCtx& ctx) {
  const int64_t K = ctx.idata[0];
  const bool propto = (ctx.variant & 0x80u) != 0;
  stan::math::nested_rev_autodiff nested;
  using stan::math::var;
  VarM L(K, K);
  for (int64_t j = 0; j < K; ++j)
    for (int64_t i = 0; i < K; ++i) L(i, j) = ctx.in[0].data[j * K + i];
  var eta(ctx.in[1].data[0]);
  var out;
  const auto call = [&](const auto& shape) {
    if constexpr (Chol) {
      return propto ? stan::math::lkj_corr_cholesky_lpdf<true>(L, shape)
                    : stan::math::lkj_corr_cholesky_lpdf<false>(L, shape);
    } else {
      return propto ? stan::math::lkj_corr_lpdf<true>(L, shape)
                    : stan::math::lkj_corr_lpdf<false>(L, shape);
    }
  };
  if (ctx.variant & 0x2u)
    out = call(eta);
  else
    out = call(ctx.in[1].data[0]);
  return finish_tail_density<Grad>(ctx, out, L, eta);
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
  // X is a parameter when the model reads `y ~ normal_id_glm(X, ...)` with a
  // parameter design matrix; a data X (the common case) keeps the map.
  const bool x_var = (ctx.variant & 0x2u) != 0;
  stan::math::nested_rev_autodiff nested;
  using stan::math::var;
  using VarM = Eigen::Matrix<var, -1, -1>;
  const bool one_y = ctx.in[0].len == 1;
  CMapV yd(ctx.in[0].data, ctx.in[0].len);
  CMapM Xd(ctx.in[1].data, rows, cols);
  VarM Xv(x_var ? rows : 0, x_var ? cols : 0);
  for (int64_t j = 0; j < Xv.cols(); ++j)
    for (int64_t i = 0; i < Xv.rows(); ++i)
      Xv(i, j) = ctx.in[1].data[j * rows + i];
  var y_scalar = ctx.in[0].len ? ctx.in[0].data[0] : 0.0;
  VarV yv(y_var && !one_y ? ctx.in[0].len : 0);
  for (int64_t i = 0; i < yv.size(); ++i) yv(i) = ctx.in[0].data[i];
  VarV alpha(ctx.in[2].len), beta(ctx.in[3].len), sigma(ctx.in[4].len);
  for (int64_t i = 0; i < ctx.in[2].len; ++i) alpha(i) = ctx.in[2].data[i];
  for (int64_t i = 0; i < ctx.in[3].len; ++i) beta(i) = ctx.in[3].data[i];
  for (int64_t i = 0; i < ctx.in[4].len; ++i) sigma(i) = ctx.in[4].data[i];
  var out;
  const bool one_a = ctx.in[2].len == 1, one_s = ctx.in[4].len == 1;
  auto call = [&](auto&& y, auto&& x, auto&& a, auto&& s) {
    return propto ? stan::math::normal_id_glm_lpdf<true>(y, x, a, beta, s)
                  : stan::math::normal_id_glm_lpdf<false>(y, x, a, beta, s);
  };
  auto dispatch = [&](auto&& y, auto&& x) {
    if (one_a && one_s)
      out = call(y, x, alpha(0), sigma(0));
    else if (one_a)
      out = call(y, x, alpha(0), sigma);
    else if (one_s)
      out = call(y, x, alpha, sigma(0));
    else
      out = call(y, x, alpha, sigma);
  };
  const bool row_x = ctx.n_idata >= 5 && ctx.idata[4] == 1;
  auto pick_x = [&](auto&& y) {
    if (row_x) {
      if (x_var)
        dispatch(y, Xv.row(0));
      else
        dispatch(y, Xd.row(0));
    } else if (x_var) {
      dispatch(y, Xv);
    } else {
      dispatch(y, Xd);
    }
  };
  if (y_var) {
    if (one_y)
      pick_x(y_scalar);
    else
      pick_x(yv);
  } else if (one_y) {
    pick_x(ctx.in[0].data[0]);
  } else {
    pick_x(yd);
  }
  const double v = out.val();
  // One tape per gradient, not two. The forward differentiates it once
  // with a seed of 1 and keeps the partials; the backward is then the
  // contraction every other native kernel does. This used to build the
  // whole var tape in the forward, throw it away, and build it again in
  // the backward to call grad() -- 90.9% of diamonds' gradient, and more
  // work than CmdStan does for the same statement.
  if (!values_only()) {
    stan::math::grad(out.vi_);
    double* s = ctx.scratch;
    if (y_var) {
      if (one_y)
        *s++ = y_scalar.adj();
      else
        for (int64_t i = 0; i < yv.size(); ++i) *s++ = yv(i).adj();
    }
    // Column-major, matching the slot layout the backward scatters into.
    for (int64_t j = 0; j < Xv.cols(); ++j)
      for (int64_t i = 0; i < Xv.rows(); ++i) *s++ = Xv(i, j).adj();
    for (int64_t i = 0; i < ctx.in[2].len; ++i) *s++ = alpha(i).adj();
    for (int64_t i = 0; i < ctx.in[3].len; ++i) *s++ = beta(i).adj();
    for (int64_t i = 0; i < ctx.in[4].len; ++i) *s++ = sigma(i).adj();
  }
  return v;
}

int64_t nid_glm_scratch(const Op& op, const Slot* slots) {
  // The y and X sections exist only when those inputs are active (variant
  // bits 0 and 1), but the sizing hook cannot see the variant, so reserve
  // both unconditionally.
  return slots[op.in[0]].len + slots[op.in[1]].len + slots[op.in[2]].len +
         slots[op.in[3]].len + slots[op.in[4]].len;
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
  if (ctx.variant & 0x2u) {
    if (ctx.in_adj[1].data)
      for (int64_t i = 0; i < ctx.in[1].len; ++i)
        ctx.in_adj[1].data[i] += w * s[i];
    s += ctx.in[1].len;
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
  if (n == 0) return;
  MatD a = CMapM(ctx.in[0].data, n, n);
  if (values_only()) {
    Eigen::Map<VecD>(ctx.out.data, n) = stan::math::eigenvalues_sym(a);
    return;
  }
  // Keep the decomposition that stan-math's reverse callback would retain.
  // The old backward rebuilt a nested var matrix and decomposed it again;
  // retaining the vectors makes the pullback the same two GEMMs with no
  // second eigensolve or tape.  Use stan-math's exact check spelling before
  // dropping to the Eigen solver its prim implementation wraps.
  stan::math::check_symmetric("eigenvalues_sym", "m", a);
  Eigen::SelfAdjointEigenSolver<MatD> solver(a);
  Eigen::Map<VecD>(ctx.out.data, n) = solver.eigenvalues();
  MapM(ctx.scratch, n, n) = solver.eigenvectors();
}
void eigvals_bwd(KernelCtx& ctx) {
  if (!ctx.in_adj[0].data) return;
  const int64_t n = ctx.idata[0];
  if (n == 0) return;
  CMapM eigenvecs(ctx.scratch, n, n);
  CMapV eigenvals_adj(ctx.out_adj_vec.data, n);
  // stan/math/rev/fun/eigenvalues_sym.hpp, in the same association order.
  MapM(ctx.in_adj[0].data, n, n) +=
      eigenvecs * eigenvals_adj.asDiagonal() * eigenvecs.transpose();
}
void eigvecs_fwd(KernelCtx& ctx) {
  const int64_t n = ctx.idata[0];
  if (n == 0) return;
  MatD a = CMapM(ctx.in[0].data, n, n);
  // eigenvectors_sym's prim check deliberately names eigenvalues_sym; retain
  // that observable spelling together with its underlying full solver.
  stan::math::check_symmetric("eigenvalues_sym", "m", a);
  Eigen::SelfAdjointEigenSolver<MatD> solver(a);
  MapM(ctx.out.data, n, n) = solver.eigenvectors();
  if (!values_only()) Eigen::Map<VecD>(ctx.scratch, n) = solver.eigenvalues();
}
void eigvecs_bwd(KernelCtx& ctx) {
  if (!ctx.in_adj[0].data) return;
  const int64_t n = ctx.idata[0];
  if (n == 0) return;
  CMapM eigenvecs(ctx.out.data, n, n);
  CMapV eigenvals(ctx.scratch, n);
  CMapM eigenvecs_adj(ctx.out_adj_vec.data, n, n);
  // stan/math/rev/fun/eigenvectors_sym.hpp, expression for expression.
  Eigen::MatrixXd f = (1 / (eigenvals.rowwise().replicate(n).transpose() -
                            eigenvals.rowwise().replicate(n))
                               .array());
  f.diagonal().setZero();
  MapM(ctx.in_adj[0].data, n, n) +=
      eigenvecs * f.cwiseProduct(eigenvecs.transpose() * eigenvecs_adj) *
      eigenvecs.transpose();
}

int64_t eigvals_scratch(const Op& op, const Slot*) {
  const int64_t n = op.idata[0];
  return n * n;
}

int64_t eigvecs_scratch(const Op& op, const Slot*) { return op.idata[0]; }

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
  const int y_encoded =
      ctx.n_idata > 2
          ? ctx.idata[1]
          : (ctx.n_idata > 1 && ctx.idata[1] > 1 ? ctx.idata[1] : -1);
  const int mu_encoded = ctx.n_idata > 2 ? ctx.idata[2] : -1;
  const bool propto = (ctx.variant & 0x80u) != 0;
  stan::math::nested_rev_autodiff nested;
  using stan::math::var;
  std::vector<VarV> ys = mvt_vectors<var>(ctx.in[0], K, y_encoded);
  var nu = ctx.in[1].data[0];
  std::vector<VarV> mus = mvt_vectors<var>(ctx.in[2], K, mu_encoded);
  VarM S = tail_m(ctx, 3, K, K);
  const auto call = [&](auto&& yarg, auto&& muarg) {
    if constexpr (Kind == kMst) {
      return propto
                 ? stan::math::multi_student_t_lpdf<true>(yarg, nu, muarg, S)
                 : stan::math::multi_student_t_lpdf<false>(yarg, nu, muarg, S);
    } else {
      return propto ? stan::math::multi_student_t_cholesky_lpdf<true>(yarg, nu,
                                                                      muarg, S)
                    : stan::math::multi_student_t_cholesky_lpdf<false>(
                          yarg, nu, muarg, S);
    }
  };
  const auto dispatch_mu = [&](auto&& yarg) -> var {
    return with_mvt_argument(mus, mu_encoded, [&](auto&& muarg) -> var {
      return call(yarg, muarg);
    });
  };
  var out = with_mvt_argument(ys, y_encoded, dispatch_mu);
  return finish_tail_density<Grad>(ctx, out, ys, nu, mus, S);
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

// Multinomial has one independent probability edge per count. Preserve the
// validation and summation order of Stan Math's prim/prob/multinomial_lpmf.hpp.
// The reverse uses the weighted multiply_log rule (rev/fun/multiply_log.hpp),
// multiplying before dividing; caching unit partials changes that rounding.
void multn_fwd(KernelCtx& ctx) {
  const std::vector<int> counts(ctx.idata, ctx.idata + ctx.n_idata);
  const CMapV theta(ctx.in[0].data, ctx.in[0].len);
  constexpr const char* fn = "multinomial_lpmf";
  stan::math::check_size_match(fn, "Size of number of trials variable",
                               counts.size(), "rows of probabilities parameter",
                               theta.rows());
  stan::math::check_nonnegative(fn, "Number of trials variable", counts);
  stan::math::check_simplex(fn, "Probabilities parameter", theta);
  double value = 0;
  if (!(ctx.variant & 0x80u)) {
    double total = 1;
    for (int count : counts) {
      total += count;
      value -= stan::math::lgamma(count + 1.0);
    }
    value += stan::math::lgamma(total);
  }
  for (int64_t i = 0; i < ctx.in[0].len; ++i)
    value += counts[i] == 1 ? std::log(theta(i))
                            : stan::math::multiply_log(counts[i], theta(i));
  ctx.out.data[0] = value;
}
void multn_bwd(KernelCtx& ctx) {
  if (!ctx.in_adj[0].data) return;
  for (int64_t i = 0; i < ctx.in[0].len; ++i)
    ctx.in_adj[0].data[i] +=
        ctx.idata[i] == 1 ? ctx.out_adj / ctx.in[0].data[i]
                          : ctx.out_adj * ctx.idata[i] / ctx.in[0].data[i];
}

// ---- the two the recorder cannot take ----------------------------------
// ordered_probit does `c_vec[i].coeff(0) - lambda_vec[i]` and wiener
// `res *= 0.0` on the scalar type. var has those operators; rvar
// deliberately does not (see recorder.hpp), so they live here.
// ordered_logistic/ordered_probit(y | lambda, c): the integer outcomes prefix
// idata and the shared vector-layout payload describes whether c is one
// cutpoint vector or an array containing one vector per observation.
enum OrderedKind { kOrderedLogistic, kOrderedProbit };
template <bool Grad, OrderedKind Kind>
double ordered_eval(KernelCtx& ctx) {
  if (ctx.n_idata < 4 ||
      ctx.idata[ctx.n_idata - 3] != kVectorizedDensityLayoutMarker)
    throw std::runtime_error("ordered density has no vector layout");
  const int64_t N = ctx.n_idata - 3;
  const int64_t K = ctx.idata[ctx.n_idata - 2];
  const int cuts_encoded = ctx.idata[ctx.n_idata - 1];
  const bool propto = (ctx.variant & 0x80u) != 0;
  std::vector<int> y(ctx.idata, ctx.idata + N);
  stan::math::nested_rev_autodiff nested;
  using stan::math::var;
  // ordered_probit does not condition its summands on propto or the scalar
  // activity type. Its forward needs only values; retain the original var
  // replay in backward so weighted adjoint arithmetic stays unchanged.
  using Scalar =
      std::conditional_t<!Grad && Kind == kOrderedProbit, double, var>;
  Eigen::Matrix<Scalar, -1, 1> lambda(ctx.in[0].len);
  for (int64_t i = 0; i < ctx.in[0].len; ++i) lambda(i) = ctx.in[0].data[i];
  auto cuts = mvt_vectors<Scalar>(ctx.in[1], K, cuts_encoded);
  Scalar out;
  const auto call = [&](const auto& location, const auto& cutpoints) {
    if constexpr (Kind == kOrderedLogistic)
      return propto ? stan::math::ordered_logistic_lpmf<true>(y, location,
                                                              cutpoints)
                    : stan::math::ordered_logistic_lpmf<false>(y, location,
                                                               cutpoints);
    else
      return propto
                 ? stan::math::ordered_probit_lpmf<true>(y, location, cutpoints)
                 : stan::math::ordered_probit_lpmf<false>(y, location,
                                                          cutpoints);
  };
  with_mvt_argument(cuts, cuts_encoded, [&](const auto& cutpoints) {
    out = ctx.in[0].len == 1 ? call(lambda(0), cutpoints)
                             : call(lambda, cutpoints);
  });
  if constexpr (!Grad && Kind == kOrderedProbit)
    return out;
  else
    return finish_tail_density<Grad>(ctx, out, lambda, cuts);
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
STANLI_TAIL_KERNEL(multnl, mult_eval, kMultinomialLogit)
STANLI_TAIL_KERNEL(dirmult, mult_eval, kDirichletMultinomial)
#undef STANLI_TAIL_KERNEL
void ologistic_fwd(KernelCtx& ctx) {
  // Arrays of cutpoint vectors retain the general nested-tape path. For a
  // shared cutpoint vector, Stan Math's propagator can write its partials
  // straight to scratch; no autodiff tape or second density evaluation is
  // needed. Keep both operands active, as in ordered_eval, so propto and
  // validation semantics are unchanged.
  if (ctx.n_idata < 4 ||
      ctx.idata[ctx.n_idata - 3] != kVectorizedDensityLayoutMarker ||
      ctx.idata[ctx.n_idata - 1] >= 0) {
    ctx.out.data[0] = ordered_eval<false, kOrderedLogistic>(ctx);
    return;
  }
  sink s = recorded_tail_sink<2>(ctx);
  sink_scope active(s);
  const std::vector<int> y(ctx.idata, ctx.idata + ctx.n_idata - 3);
  const auto cuts = as_rvar(ctx.in[1]);
  const auto call = [&](const auto& location) {
    record_probability_call([&] {
      return (ctx.variant & 0x80u)
                 ? stan::math::ordered_logistic_lpmf<true>(y, location, cuts)
                 : stan::math::ordered_logistic_lpmf<false>(y, location, cuts);
    });
  };
  if (ctx.in[0].len == 1)
    call(rvar(ctx.in[0].data[0]));
  else
    call(as_rvar(ctx.in[0]));
  ctx.out.data[0] = s.value;
}
void ologistic_bwd(KernelCtx& ctx) {
  if (ctx.idata[ctx.n_idata - 1] >= 0) {
    ordered_eval<true, kOrderedLogistic>(ctx);
    return;
  }
  recorded_tail_bwd<2>(ctx);
}
// For shared cutpoints retain the primitive CDF partials and log-difference
// denominators. The weighted reverse follows rev/fun/log_diff_exp.hpp before
// multiplying the CDF partial, preserving its division/rounding order.
bool oprobit_shared(const KernelCtx& ctx) {
  return ctx.n_idata >= 4 &&
         ctx.idata[ctx.n_idata - 3] == kVectorizedDensityLayoutMarker &&
         ctx.idata[ctx.n_idata - 1] < 0 && ctx.in[0].len > 0 &&
         ctx.idata[ctx.n_idata - 2] == ctx.in[1].len;
}
void oprobit_fwd(KernelCtx& ctx) {
  if (!oprobit_shared(ctx)) {
    ctx.out.data[0] = ordered_eval<false, kOrderedProbit>(ctx);
    return;
  }
  const int64_t n = ctx.in[0].len, width = ctx.in[1].len;
  const std::vector<int> outcomes(ctx.idata, ctx.idata + ctx.n_idata - 3);
  const CMapV cuts(ctx.in[1].data, width), locations(ctx.in[0].data, n);
  // Shared-vector validation follows prim/prob/ordered_probit_lpmf.hpp.
  // Keep its checks before all CDF work and preserve scalar broadcasting.
  constexpr const char* fn = "ordered_probit";
  stan::math::check_nonzero_size(fn, "Cut-points", cuts);
  if (n == 1)
    stan::math::check_consistent_sizes(fn, "Integers", outcomes, "Locations",
                                       locations(0));
  else
    stan::math::check_consistent_sizes(fn, "Integers", outcomes, "Locations",
                                       locations);
  stan::math::check_bounded(fn, "Random variable", outcomes, 1, width + 1);
  stan::math::check_nonzero_size(fn, "First cutpoint set", cuts);
  stan::math::check_ordered(fn, "Cut-points", cuts);
  if (width > 1)
    stan::math::check_finite(fn, "Final cut point", cuts(width - 1));
  stan::math::check_finite(fn, "First cut point", cuts(0));
  if (n == 1)
    stan::math::check_finite(fn, "Location parameter", locations(0));
  else
    stan::math::check_finite(fn, "Location parameter", locations);
  double value = 0;
  const auto cdf = [](double argument, double* partial) {
    sink s;
    s.buf[0] = partial;
    s.len[0] = 1;
    sink_scope scope(s);
    record_probability_call(
        [&] { return stan::math::std_normal_lcdf(rvar(argument)); });
    return s.value;
  };
  for (int64_t i = 0; i < n; ++i) {
    double* state = ctx.scratch + 4 * i;
    const int outcome = ctx.idata[i];
    const double location = ctx.in[0].data[i];
    if (outcome == 1) {
      value += cdf(ctx.in[1].data[0] - location, state);
    } else if (outcome == width + 1) {
      value += cdf(location - ctx.in[1].data[width - 1], state);
    } else {
      const double upper = cdf(ctx.in[1].data[outcome - 1] - location, state);
      const double lower =
          cdf(ctx.in[1].data[outcome - 2] - location, state + 1);
      value += stan::math::log_diff_exp(upper, lower);
      state[2] = std::expm1(lower - upper);
      state[3] = std::expm1(upper - lower);
    }
  }
  ctx.out.data[0] = value;
}
void oprobit_bwd(KernelCtx& ctx) {
  if (!oprobit_shared(ctx)) {
    ordered_eval<true, kOrderedProbit>(ctx);
    return;
  }
  const int64_t n = ctx.in[0].len, width = ctx.in[1].len;
  double* locations = ctx.scratch + 4 * n;
  double* cuts = locations + n;
  std::fill_n(locations, n + width, 0.0);
  for (int64_t i = n; i-- > 0;) {
    const double* state = ctx.scratch + 4 * i;
    const int outcome = ctx.idata[i];
    if (outcome == 1) {
      const double g = 0.0 + ctx.out_adj * state[0];
      cuts[0] += g;
      locations[i] -= g;
    } else if (outcome == width + 1) {
      const double g = 0.0 + ctx.out_adj * state[0];
      locations[i] += g;
      cuts[width - 1] -= g;
    } else {
      const double lower = 0.0 + (0.0 - ctx.out_adj / state[3]) * state[1];
      const double upper = 0.0 + (0.0 - ctx.out_adj / state[2]) * state[0];
      cuts[outcome - 2] += lower;
      locations[i] -= lower;
      cuts[outcome - 1] += upper;
      locations[i] -= upper;
    }
  }
  // Match the replay's final scatter, including aliased caller edges.
  if (ctx.in_adj[0].data)
    for (int64_t i = 0; i < n; ++i) ctx.in_adj[0].data[i] += locations[i];
  if (ctx.in_adj[1].data)
    for (int64_t i = 0; i < width; ++i) ctx.in_adj[1].data[i] += cuts[i];
}
int64_t oprobit_scratch(const Op& op, const Slot* slots) {
  return 5 * slots[op.in[0]].len + slots[op.in[1]].len;
}
// Scalar observations with a fixed outcome need derivatives only
// for boundary, nondecision time, bias and drift. Keep those types active
// (including under propto), and retain their unit partials from a single Stan
// Math tape.
bool wiener_fixed_observation(const KernelCtx& ctx) {
  if (ctx.in_adj[0].data) return false;
  for (int k = 0; k < 5; ++k)
    if (ctx.in[k].len != 1) return false;
  return true;
}
void wiener_fwd(KernelCtx& ctx) {
  if (!wiener_fixed_observation(ctx)) {
    ctx.out.data[0] = wiener_eval<false>(ctx);
    return;
  }
  stan::math::nested_rev_autodiff nested;
  using stan::math::var;
  var alpha = ctx.in[1].data[0], tau = ctx.in[2].data[0],
      beta = ctx.in[3].data[0], delta = ctx.in[4].data[0];
  const double y = ctx.in[0].data[0];
  var out = (ctx.variant & 0x80u)
                ? stan::math::wiener_lpdf<true>(y, alpha, tau, beta, delta)
                : stan::math::wiener_lpdf<false>(y, alpha, tau, beta, delta);
  ctx.out.data[0] = out.val();
  if (!values_only()) {
    stan::math::grad(out.vi_);
    ctx.scratch[0] = alpha.adj();
    ctx.scratch[1] = tau.adj();
    ctx.scratch[2] = beta.adj();
    ctx.scratch[3] = delta.adj();
  }
}
void wiener_bwd(KernelCtx& ctx) {
  // A unit seed reuses the same reverse arithmetic. Weighted replay must
  // propagate its seed through that arithmetic: post-scaling unit partials
  // can hide intermediate overflow or change underflow/cancellation.
  if (!wiener_fixed_observation(ctx) || ctx.out_adj != 1.0) {
    wiener_eval<true>(ctx);
    return;
  }
  for (int j = 0; j < 4; ++j)
    if (!std::isfinite(ctx.scratch[j])) {
      wiener_eval<true>(ctx);
      return;
    }
  const int inputs[] = {1, 2, 3, 4};
  for (int j = 0; j < 4; ++j)
    if (ctx.in_adj[inputs[j]].data)
      ctx.in_adj[inputs[j]].data[0] += ctx.out_adj * ctx.scratch[j];
}
int64_t wiener_scratch(const Op& op, const Slot* slots) {
  for (int k = 0; k < 5; ++k)
    if (slots[op.in[k]].len != 1) return 0;
  return 4;
}

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

// Binomial GLMs retain their established nested-tape path.
// Categorical and ordered-logistic GLMs below record partials directly.
// idata = [outcome..., rows, cols] and, for binomial, the trial counts.
enum GlmKind { kBinomLogitGlm, kCatLogitGlm };
template <typename F>
decltype(auto) with_glm_integer(std::vector<int>& values, bool scalar, F&& f) {
  return scalar ? std::forward<F>(f)(values[0]) : std::forward<F>(f)(values);
}
template <GlmKind Kind>
double tglm_eval(KernelCtx& ctx) {
  const bool scalar_layout =
      ctx.n_idata >= 4 && ctx.idata[ctx.n_idata - 2] == kGlmScalarLayoutMarker;
  const int tail = scalar_layout ? 4 : 2;
  const int64_t rows = ctx.idata[ctx.n_idata - tail];
  const int64_t cols = ctx.idata[ctx.n_idata - tail + 1];
  const unsigned scalar_mask =
      scalar_layout ? (unsigned)ctx.idata[ctx.n_idata - 1] : 0u;
  const bool propto = (ctx.variant & 0x80u) != 0;
  stan::math::nested_rev_autodiff nested;
  using stan::math::var;
  VarM X = tail_m(ctx, 0, rows, cols);
  var out;
  if constexpr (Kind == kBinomLogitGlm) {
    // idata = [n..., N..., rows, cols]
    std::vector<int> nn(ctx.idata, ctx.idata + rows);
    std::vector<int> NN(ctx.idata + rows, ctx.idata + 2 * rows);
    VarV beta = tail_v(ctx, 2, cols);
    if (ctx.in[1].len == 1) {
      var alpha = ctx.in[1].data[0];
      with_glm_integer(nn, scalar_mask & 1u, [&](const auto& n_arg) {
        with_glm_integer(NN, scalar_mask & 2u, [&](const auto& N_arg) {
          out = propto ? stan::math::binomial_logit_glm_lpmf<true>(
                             n_arg, N_arg, X, alpha, beta)
                       : stan::math::binomial_logit_glm_lpmf<false>(
                             n_arg, N_arg, X, alpha, beta);
        });
      });
      return tail_density_fwd(ctx, out, X, alpha, beta);
    }
    VarV alpha = tail_v(ctx, 1, rows);
    with_glm_integer(nn, scalar_mask & 1u, [&](const auto& n_arg) {
      with_glm_integer(NN, scalar_mask & 2u, [&](const auto& N_arg) {
        out = propto ? stan::math::binomial_logit_glm_lpmf<true>(n_arg, N_arg,
                                                                 X, alpha, beta)
                     : stan::math::binomial_logit_glm_lpmf<false>(
                           n_arg, N_arg, X, alpha, beta);
      });
    });
    return tail_density_fwd(ctx, out, X, alpha, beta);
  } else {
    std::vector<int> y(ctx.idata, ctx.idata + rows);
    VarV alpha = tail_v(ctx, 1, ctx.in[1].len);
    VarM beta = tail_m(ctx, 2, cols, ctx.in[2].len / cols);
    with_glm_integer(y, scalar_mask & 1u, [&](const auto& y_arg) {
      out = propto ? stan::math::categorical_logit_glm_lpmf<true>(y_arg, X,
                                                                  alpha, beta)
                   : stan::math::categorical_logit_glm_lpmf<false>(y_arg, X,
                                                                   alpha, beta);
    });
    return tail_density_fwd(ctx, out, X, alpha, beta);
  }
}

void lkjcov_fwd(KernelCtx& ctx) { ctx.out.data[0] = lkjcov_eval<false>(ctx); }
void lkjcov_bwd(KernelCtx& ctx) { lkjcov_eval<true>(ctx); }
void blglm_fwd(KernelCtx& ctx) {
  ctx.out.data[0] = tglm_eval<kBinomLogitGlm>(ctx);
}
void clglm_fwd(KernelCtx& ctx) {
  const bool scalar_layout =
      ctx.n_idata >= 4 && ctx.idata[ctx.n_idata - 2] == kGlmScalarLayoutMarker;
  const int tail = scalar_layout ? 4 : 2;
  const int64_t rows = ctx.idata[ctx.n_idata - tail];
  const int64_t cols = ctx.idata[ctx.n_idata - tail + 1];
  const bool scalar_y = scalar_layout && (ctx.idata[ctx.n_idata - 1] & 1u);
  std::vector<int> y(ctx.idata, ctx.idata + rows);
  sink s = recorded_tail_sink<3>(ctx);
  sink_scope active(s);
  const auto X = as_rvar_matrix(ctx.in[0], rows, cols);
  const auto alpha = as_rvar(ctx.in[1]);
  const auto beta = as_rvar_matrix(ctx.in[2], cols, ctx.in[2].len / cols);
  // Preserve the established all-active instantiation, including design
  // matrices supplied as data. Stan Math computes these partials directly;
  // the recorder deposits them without allocating or walking a var tape.
  with_glm_integer(y, scalar_y, [&](const auto& outcome) {
    record_probability_call([&] {
      return (ctx.variant & 0x80u)
                 ? stan::math::categorical_logit_glm_lpmf<true>(outcome, X,
                                                                alpha, beta)
                 : stan::math::categorical_logit_glm_lpmf<false>(outcome, X,
                                                                 alpha, beta);
    });
  });
  ctx.out.data[0] = s.value;
}
void olglm_fwd(KernelCtx& ctx) {
  const bool scalar_layout =
      ctx.n_idata >= 4 && ctx.idata[ctx.n_idata - 2] == kGlmScalarLayoutMarker;
  const int tail = scalar_layout ? 4 : 2;
  const int64_t rows = ctx.idata[ctx.n_idata - tail];
  const int64_t cols = ctx.idata[ctx.n_idata - tail + 1];
  const bool scalar_y = scalar_layout && (ctx.idata[ctx.n_idata - 1] & 1u);
  std::vector<int> y(ctx.idata, ctx.idata + rows);
  sink s = recorded_tail_sink<3>(ctx);
  sink_scope active(s);
  const auto X = as_rvar_matrix(ctx.in[0], rows, cols);
  const auto beta = as_rvar(ctx.in[1]), cuts = as_rvar(ctx.in[2]);
  with_glm_integer(y, scalar_y, [&](const auto& outcome) {
    record_probability_call([&] {
      return (ctx.variant & 0x80u)
                 ? stan::math::ordered_logistic_glm_lpmf<true>(outcome, X, beta,
                                                               cuts)
                 : stan::math::ordered_logistic_glm_lpmf<false>(outcome, X,
                                                                beta, cuts);
    });
  });
  ctx.out.data[0] = s.value;
}

// ---- the cdfs the recorder cannot take ---------------------------------
// Same reason ordered_probit and wiener are here, one step further out:
// von_mises_cdf writes `res *= 0.0` and compares `x_n == -pi` on the
// scalar type, and neg_binomial_2_lcdf forms `phi / (phi + mu)`. Neither
// builds its result through stan-math's partials propagator, so neither
// can be handed an rvar. See STANLI_TAIL_CDF_LIST in optable.hpp.

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
  register_kernel(OP_GP_COV, Kernel{gp_cov_fwd, gp_cov_bwd, nullptr});
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
  register_kernel(OP_ORDERED_LOGISTIC_LPMF, Kernel{ologistic_fwd, ologistic_bwd,
                                                   recorded_tail_scratch<2>});
  register_kernel(OP_ORDERED_PROBIT_LPMF,
                  Kernel{oprobit_fwd, oprobit_bwd, oprobit_scratch});
  register_kernel(OP_WIENER_LPDF,
                  Kernel{wiener_fwd, wiener_bwd, wiener_scratch});
#define STANLI_REGISTER_TAIL_CDF(code, fn, nreal, tier) \
  register_kernel(code, Kernel{fn##_fwd, fn##_bwd, nullptr});
  STANLI_TAIL_CDF_LIST(STANLI_REGISTER_TAIL_CDF)
  STANLI_TAIL_INT_CDF_LIST(STANLI_REGISTER_TAIL_CDF)
#undef STANLI_REGISTER_TAIL_CDF
  register_kernel(OP_LKJ_COV_LPDF, Kernel{lkjcov_fwd, lkjcov_bwd, nullptr});
  register_kernel(
      OP_BINOMIAL_LOGIT_GLM_LPMF,
      Kernel{blglm_fwd, tail_density_bwd<3>, tail_density_scratch<3>});
  register_kernel(
      OP_CATEGORICAL_LOGIT_GLM_LPMF,
      Kernel{clglm_fwd, recorded_tail_bwd<3>, recorded_tail_scratch<3>});
  register_kernel(
      OP_ORDERED_LOGISTIC_GLM_LPMF,
      Kernel{olglm_fwd, recorded_tail_bwd<3>, recorded_tail_scratch<3>});
  register_kernel(OP_DIAG_MATRIX, Kernel{diag_fwd, diag_bwd, nullptr});
  register_kernel(OP_CHOLESKY, Kernel{chol_fwd, chol_bwd, nullptr});
  register_kernel(OP_MATRIX_EXP,
                  Kernel{matrix_exp_fwd, matrix_exp_bwd, nullptr});
  register_kernel(
      OP_MATRIX_EXP_DYNAMIC,
      Kernel{matrix_exp_dynamic_fwd, matrix_exp_dynamic_bwd, nullptr});
  register_kernel(OP_INVERSE, Kernel{inverse_fwd, inverse_bwd, nullptr});
  register_kernel(OP_INVERSE_SPD,
                  Kernel{inverse_spd_fwd, inverse_spd_bwd, nullptr});
  register_kernel(OP_LOG_DETERMINANT,
                  Kernel{log_det_fwd, log_det_bwd, nullptr});
  register_kernel(OP_QUAD_FORM, Kernel{qf_fwd, qf_bwd, nullptr});
  register_kernel(OP_ADD_DIAG, Kernel{add_diag_fwd, add_diag_bwd, nullptr});
  register_kernel(OP_QUAD_FORM_SYM, Kernel{qfs_fwd, qfs_bwd, nullptr});
  register_kernel(OP_MULTI_NORMAL_CHOL_LPDF,
                  Kernel{mnc_fwd, mnc_bwd, mnc_scratch});
  register_kernel(OP_MULTI_NORMAL_LPDF,
                  Kernel{mn_fwd, mn_bwd, tail_density_scratch<3>});
  register_kernel(OP_MULTI_NORMAL_PREC_LPDF,
                  Kernel{mnprec_fwd, mnprec_bwd, nullptr});
  register_kernel(OP_GEMM, Kernel{gemm_fwd, gemm_bwd, nullptr});
  register_kernel(OP_CROSSPROD, Kernel{crossprod_fwd, crossprod_bwd, nullptr});
  register_kernel(
      OP_MULT_LOWER_TRI_SELF_TRANSPOSE,
      Kernel{mlt_self_transpose_fwd, mlt_self_transpose_bwd, nullptr});
  register_kernel(OP_MDIVIDE_LEFT,
                  Kernel{solve_fwd<true>, solve_bwd<true>, nullptr});
  register_kernel(OP_MDIVIDE_RIGHT,
                  Kernel{solve_fwd<false>, solve_bwd<false>, nullptr});
  register_kernel(OP_MDIVIDE_LEFT_SPD,
                  Kernel{solve_fwd<true, SolveKind::Spd>,
                         solve_bwd<true, SolveKind::Spd>, nullptr});
  register_kernel(OP_MDIVIDE_RIGHT_SPD,
                  Kernel{solve_fwd<false, SolveKind::Spd>,
                         solve_bwd<false, SolveKind::Spd>, nullptr});
  register_kernel(OP_MDIVIDE_LEFT_TRI_LOW,
                  Kernel{solve_fwd<true, SolveKind::TriLow>,
                         solve_bwd<true, SolveKind::TriLow>, nullptr});
  register_kernel(OP_MDIVIDE_RIGHT_TRI_LOW,
                  Kernel{solve_fwd<false, SolveKind::TriLow>,
                         solve_bwd<false, SolveKind::TriLow>, nullptr});
  register_kernel(OP_EIGENVALUES_SYM,
                  Kernel{eigvals_fwd, eigvals_bwd, eigvals_scratch});
  register_kernel(OP_EIGENVECTORS_SYM,
                  Kernel{eigvecs_fwd, eigvecs_bwd, eigvecs_scratch});
  register_kernel(OP_TRANSPOSE, Kernel{transpose_fwd, transpose_bwd, nullptr});
  register_kernel(OP_LKJ_CORR_CHOL_LPDF, Kernel{lkj_fwd, lkj_bwd, nullptr});
  register_kernel(OP_LKJ_CORR_LPDF, Kernel{lkjc_fwd, lkjc_bwd, nullptr});
  register_kernel(OP_NORMAL_ID_GLM_LPDF,
                  Kernel{nid_glm_fwd, nid_glm_bwd, nid_glm_scratch});
}

}  // namespace stanli
