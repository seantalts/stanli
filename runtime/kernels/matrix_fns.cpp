// Matrix-valued ops and the densities with matrix arguments: GP covariance,
// cholesky_decompose, diag_matrix, multi_normal(_cholesky), wishart, wiener,
// the ordered and GLM densities, multinomial and the tail CDFs. Most forwards
// run the prim (double) implementation and backwards replay the same call on a
// nested var tape, seeded with the dot trick. The profiled single-vector
// Cholesky density below is the compact native exception: it retains Stan
// Math's closed-form matrix partials from forward to backward.
//
// Matrices live in slots column-major, matching Eigen and the rest of the
// pipeline, so a flat slot maps straight onto Map<MatrixXd>.
#include "density_fused.hpp"

#include <stanli/graph.hpp>
#include <stanli/dense_blas.hpp>
#include <stanli/density_registry.hpp>
#include <stanli/recorder.hpp>
#include <stanli/legacy.hpp>
#include <stanli/optable.hpp>
#include <stanli/packet.hpp>
#include <stanli/pairwise_sum.hpp>

#include <stan/math.hpp>

#include <algorithm>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace stanli {
void register_solve_kernels();

namespace {

using MatD = Eigen::MatrixXd;
using VecD = Eigen::VectorXd;
using MapM = Eigen::Map<MatD>;
using CMapM = Eigen::Map<const MatD>;
template <int R, int C>
using FixM = Eigen::Matrix<double, R, C>;
template <int R, int C>
using FixMap = Eigen::Map<FixM<R, C>>;
template <int R, int C>
using FixCMap = Eigen::Map<const FixM<R, C>>;
template <int V>
using IntC = std::integral_constant<int, V>;

template <typename F>
void with_small_nm(int64_t n, int64_t m, F&& f) {
  if (fixed_small_matrices()) {
    if (n == 2 && m == 1) return f(IntC<2>{}, IntC<1>{});
    if (n == 2 && m == 2) return f(IntC<2>{}, IntC<2>{});
  }
  f(IntC<-1>{}, IntC<-1>{});
}
using CMapV = Eigen::Map<const VecD>;
using VarV = Eigen::Matrix<stan::math::var, -1, 1>;
using VarM = Eigen::Matrix<stan::math::var, -1, -1>;

// ---- gp_*_cov(x, alpha, rho[, period]) ------------------------------------
// in = {x (data, N*D), alpha, rho, then the period for kGpPeriodic};
// idata = {N, D}; out = N*N column-major. x as array[N] real is D == 1;
// array[N] vector[D] flattens array-major. The variant is a GpCovMode: the
// covariance function, whether rho holds one length scale per dimension
// (D values) instead of one, and whether the call is the cross form. An
// optional scalar input after the hyperparameters adds to each diagonal in
// increasing source order, on this same tape.
//
// The cross form gp_*_cov(x1, x2, alpha, rho[, period]) has
// in = {x1 (N1*D), x2 (N2*D), alpha, rho[, period]}, idata = {N1, D, N2} and
// out = N1*N2 column-major.
template <typename T = double>
std::vector<Eigen::Matrix<T, -1, 1>> gp_points(const KernelCtx& ctx,
                                               int input = 0) {
  const int64_t D = ctx.idata[1];
  const int64_t N = input == 0 ? ctx.idata[0] : ctx.idata[2];
  std::vector<Eigen::Matrix<T, -1, 1>> pts(N, Eigen::Matrix<T, -1, 1>(D));
  for (int64_t n = 0; n < N; ++n)
    for (int64_t d = 0; d < D; ++d) pts[n](d) = ctx.in[input].data[n * D + d];
  return pts;
}
// L is a scalar length scale, or a std::vector of one per dimension. P is
// one location array (the square form) or two (the cross form).
template <typename S, typename L, typename... P>
auto gp_cov_call(GpCov family, const S& sigma, const L& length_scale,
                 const P&... pts) {
  using Result = Eigen::Matrix<stan::return_type_t<P..., S, L>, -1, -1>;
  switch (family) {
    case kGpMatern32:
      return Result(stan::math::gp_matern32_cov(pts..., sigma, length_scale));
    case kGpMatern52:
      return Result(stan::math::gp_matern52_cov(pts..., sigma, length_scale));
    case kGpExponential:
      return Result(
          stan::math::gp_exponential_cov(pts..., sigma, length_scale));
    default:
      return Result(stan::math::gp_exp_quad_cov(pts..., sigma, length_scale));
  }
}
template <typename T>
std::vector<T> gp_length_scales(const Desc& rho) {
  return std::vector<T>(rho.data, rho.data + rho.len);
}
// Stan Math uses its periodic matrix callback only when both the length scale
// and the period are var. CmdStan passes a data argument as double, so pass
// an inactive one as double here too and run the overload CmdStan runs.
VarM gp_periodic_fixed(const KernelCtx& ctx, const std::vector<VecD>& pts,
                       const stan::math::var& sigma,
                       const stan::math::var& length_scale,
                       const stan::math::var& period) {
  const bool length_active = ctx.in_adj[2].data,
             period_active = ctx.in_adj[3].data;
  if (length_active && period_active)
    return stan::math::gp_periodic_cov(pts, sigma, length_scale, period);
  if (length_active)
    return stan::math::gp_periodic_cov(pts, sigma, length_scale,
                                       ctx.in[3].data[0]);
  if (period_active)
    return stan::math::gp_periodic_cov(pts, sigma, ctx.in[2].data[0], period);
  return stan::math::gp_periodic_cov(pts, sigma, ctx.in[2].data[0],
                                     ctx.in[3].data[0]);
}
// The cross form has no upstream matrix callback, so its value and every
// adjoint come from Stan Math's prim functions. Inactive locations stay
// double, as CmdStan passes them.
template <typename T, typename... P>
Eigen::Matrix<T, -1, -1> gp_cross_call(GpCovMode mode, const T& sigma,
                                       const std::vector<T>& length_scales,
                                       const T& period, const P&... pts) {
  if (mode.family == kGpPeriodic)
    return stan::math::gp_periodic_cov(pts..., sigma, length_scales[0], period);
  if (mode.per_dimension)
    return gp_cov_call(mode.family, sigma, length_scales, pts...);
  return gp_cov_call(mode.family, sigma, length_scales[0], pts...);
}
void gp_cross_fwd(KernelCtx& ctx) {
  const GpCovMode mode = GpCovMode::of(ctx.variant);
  const double period = mode.family == kGpPeriodic ? ctx.in[4].data[0] : 0.0;
  const MatD c = gp_cross_call(mode, ctx.in[2].data[0],
                               gp_length_scales<double>(ctx.in[3]), period,
                               gp_points(ctx, 0), gp_points(ctx, 1));
  MapM(ctx.out.data, c.rows(), c.cols()) = c;
}
void gp_cross_bwd(KernelCtx& ctx) {
  const GpCovMode mode = GpCovMode::of(ctx.variant);
  const int64_t N1 = ctx.idata[0], N2 = ctx.idata[2];
  if (N1 == 0 || N2 == 0) return;
  using stan::math::var;
  stan::math::nested_rev_autodiff nested;
  var sigma = ctx.in[2].data[0];
  std::vector<var> length_scales = gp_length_scales<var>(ctx.in[3]);
  var period = mode.family == kGpPeriodic ? ctx.in[4].data[0] : 0.0;
  std::vector<VarV> x1, x2;
  const auto seed = [&](const VarM& covariance) {
    stan::math::grad(
        stan::math::sum(stan::math::elt_multiply(
                            covariance, CMapM(ctx.out_adj_vec.data, N1, N2)))
            .vi_);
  };
  const auto with_x2 = [&](const auto& first) {
    if (ctx.in_adj[1].data) {
      x2 = gp_points<var>(ctx, 1);
      seed(gp_cross_call(mode, sigma, length_scales, period, first, x2));
    } else {
      seed(gp_cross_call(mode, sigma, length_scales, period, first,
                         gp_points(ctx, 1)));
    }
  };
  if (ctx.in_adj[0].data) {
    x1 = gp_points<var>(ctx, 0);
    with_x2(x1);
  } else {
    with_x2(gp_points(ctx, 0));
  }
  const auto harvest = [&](int input, const std::vector<VarV>& pts) {
    if (!ctx.in_adj[input].data) return;
    const int64_t D = ctx.idata[1];
    for (size_t n = 0; n < pts.size(); ++n)
      for (int64_t d = 0; d < D; ++d)
        ctx.in_adj[input].data[n * D + d] += pts[n](d).adj();
  };
  harvest(0, x1);
  harvest(1, x2);
  if (ctx.in_adj[2].data) ctx.in_adj[2].data[0] += sigma.adj();
  if (ctx.in_adj[3].data)
    for (size_t d = 0; d < length_scales.size(); ++d)
      ctx.in_adj[3].data[d] += length_scales[d].adj();
  if (mode.family == kGpPeriodic && ctx.in_adj[4].data)
    ctx.in_adj[4].data[0] += period.adj();
}
void gp_cov_fwd(KernelCtx& ctx) {
  const int64_t N = ctx.idata[0];
  const GpCovMode mode = GpCovMode::of(ctx.variant);
  if (mode.cross) {
    gp_cross_fwd(ctx);
    return;
  }
  auto pts = gp_points(ctx);
  const double sigma = ctx.in[1].data[0];
  MatD c = mode.family == kGpPeriodic
               ? MatD(stan::math::gp_periodic_cov(pts, sigma, ctx.in[2].data[0],
                                                  ctx.in[3].data[0]))
           : mode.per_dimension
               ? gp_cov_call(mode.family, sigma,
                             gp_length_scales<double>(ctx.in[2]), pts)
               : gp_cov_call(mode.family, sigma, ctx.in[2].data[0], pts);
  if (gp_cov_has_jitter(mode, ctx.n_in)) {
    const double jitter = ctx.in[gp_cov_jitter_input(mode.family)].data[0];
    for (int64_t i = 0; i < N; ++i) c(i, i) += jitter;
  }
  MapM(ctx.out.data, N, N) = c;
}
void gp_cov_replay_bwd(KernelCtx& ctx) {
  const GpCovMode mode = GpCovMode::of(ctx.variant);
  const bool periodic = mode.family == kGpPeriodic;
  const bool has_jitter = gp_cov_has_jitter(mode, ctx.n_in);
  const int jitter_input = gp_cov_jitter_input(mode.family);
  if (!ctx.in_adj[0].data) {
    // Fixed locations need no distance derivatives. Keep the hyperparameters
    // and the weighted matrix reduction on the same Stan Math tape; only the
    // inactive geometry stays double. Active locations retain the full path.
    stan::math::nested_rev_autodiff nested;
    stan::math::var sigma = ctx.in[1].data[0], rho = ctx.in[2].data[0];
    std::vector<stan::math::var> rhos;
    if (mode.per_dimension) rhos = gp_length_scales<stan::math::var>(ctx.in[2]);
    stan::math::var period;
    if (periodic) period = ctx.in[3].data[0];
    VarM covariance =
        periodic ? gp_periodic_fixed(ctx, gp_points(ctx), sigma, rho, period)
        : mode.per_dimension
            ? gp_cov_call(mode.family, sigma, rhos, gp_points(ctx))
            : gp_cov_call(mode.family, sigma, rho, gp_points(ctx));
    const int64_t n = ctx.idata[0];
    if (n == 0) return;
    stan::math::var jitter;
    if (has_jitter) {
      jitter = ctx.in[jitter_input].data[0];
      for (int64_t i = 0; i < n; ++i) covariance(i, i) += jitter;
    }
    const stan::math::var objective = stan::math::sum(stan::math::elt_multiply(
        covariance, CMapM(ctx.out_adj_vec.data, n, n)));
    stan::math::grad(objective.vi_);
    if (ctx.in_adj[1].data) ctx.in_adj[1].data[0] += sigma.adj();
    if (ctx.in_adj[2].data) {
      if (mode.per_dimension)
        for (size_t d = 0; d < rhos.size(); ++d)
          ctx.in_adj[2].data[d] += rhos[d].adj();
      else
        ctx.in_adj[2].data[0] += rho.adj();
    }
    if (periodic && ctx.in_adj[3].data) ctx.in_adj[3].data[0] += period.adj();
    if (has_jitter && ctx.in_adj[jitter_input].data)
      ctx.in_adj[jitter_input].data[0] += jitter.adj();
    return;
  }
  // x may be a parameter: rebuild the points from the promoted xs[0] so its
  // adjoints flow back too. nary_bwd copies back whatever input carries an
  // adjoint slot, so a data x simply contributes nothing here.
  const int64_t N = ctx.idata[0], D = ctx.idata[1];
  nary_bwd(ctx, [&](std::vector<VarV>& xs) {
    std::vector<VarV> pts(N, VarV(D));
    for (int64_t n = 0; n < N; ++n)
      for (int64_t d = 0; d < D; ++d) pts[n](d) = xs[0](n * D + d);
    VarM covariance =
        periodic ? VarM(stan::math::gp_periodic_cov(pts, xs[1](0), xs[2](0),
                                                    xs[3](0)))
        : mode.per_dimension
            ? gp_cov_call(mode.family, xs[1](0),
                          std::vector<stan::math::var>(
                              xs[2].data(), xs[2].data() + xs[2].size()),
                          pts)
            : gp_cov_call(mode.family, xs[1](0), xs[2](0), pts);
    if (has_jitter)
      for (int64_t i = 0; i < N; ++i) covariance(i, i) += xs[jitter_input](0);
    return covariance;
  });
}

// Match the pinned Stan Math fixed-location exp-quad callback's blocked
// reduction order. Scaling each term before summing is not interchangeable
// for nearly singular covariances: it amplifies rounding in hypergradients.
// See stan/math/rev/fun/gp_exp_quad_cov.hpp for the reference callback.
void gp_cov_bwd(KernelCtx& ctx) {
  const GpCovMode mode = GpCovMode::of(ctx.variant);
  if (mode.cross) {
    gp_cross_bwd(ctx);
    return;
  }
  const double sigma = ctx.in[1].data[0], rho = ctx.in[2].data[0];
  if (mode.family != kGpExpQuad || mode.per_dimension ||
      gp_cov_has_jitter(mode, ctx.n_in) || ctx.in_adj[0].data ||
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
template <int N>
void chol_fwd_t(KernelCtx& ctx) {
  const int64_t n = ctx.idata[0];
  FixM<N, N> a = FixCMap<N, N>(ctx.in[0].data, n, n);
  FixMap<N, N>(ctx.out.data, n, n) = stan::math::cholesky_decompose(a);
}
void chol_fwd(KernelCtx& ctx) {
  const int64_t n = ctx.idata[0];
  if (n == 2 && fixed_small_matrices()) return chol_fwd_t<2>(ctx);
  chol_fwd_t<Eigen::Dynamic>(ctx);
}
// Minimal adjoint views let the pinned Stan Math pullbacks operate directly
// on the saved factor and executor adjoints, with no vari or nested tape.
struct MatrixAdjointView {
  MapM storage;
  Eigen::Index rows() const { return storage.rows(); }
  Eigen::Index cols() const { return storage.cols(); }
  MapM& adj() { return storage; }
};
void chol_bwd_small(KernelCtx& ctx, int n) {
  const double* L_A = ctx.out.data;
  double adjL[4], adjA[4] = {};
  for (int i = 0; i < n; ++i)
    for (int j = 0; j <= i; ++j)
      adjL[i * n + j] = ctx.out_adj_vec.data[j * n + i];
  for (int i = n - 1; i >= 0; --i) {
    for (int j = i; j >= 0; --j) {
      if (i == j) {
        adjA[j * n + i] = 0.5 * adjL[i * n + j] / L_A[j * n + i];
      } else {
        adjA[j * n + i] = adjL[i * n + j] / L_A[j * n + j];
        adjL[j * n + j] -= adjL[i * n + j] * L_A[j * n + i] / L_A[j * n + j];
      }
      for (int k = j - 1; k >= 0; --k) {
        adjL[i * n + k] -= adjA[j * n + i] * L_A[k * n + j];
        adjL[j * n + k] -= adjA[j * n + i] * L_A[k * n + i];
      }
    }
  }
  for (int i = 0; i < n * n; ++i) ctx.in_adj[0].data[i] += adjA[i];
}
void chol_bwd(KernelCtx& ctx) {
  if (!ctx.in_adj[0].data) return;
  const int64_t n = ctx.idata[0];
  if (n <= 2 && fixed_small_matrices())
    return chol_bwd_small(ctx, static_cast<int>(n));
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
template <int N, typename A, typename B>
FixM<N == Eigen::Dynamic ? Eigen::Dynamic : 1,
     N == Eigen::Dynamic ? Eigen::Dynamic : 1>
quad_vec_chain(const A& a, const B& b) {
  if constexpr (N == Eigen::Dynamic) {
    return b.transpose() * a * b;
  } else {
    const FixM<1, N> t = b.transpose() * a + FixM<1, N>::Zero();
    return t * b;
  }
}
template <int N, int M, bool Vec>
void qf_fwd_t(KernelCtx& ctx) {
  constexpr int K = (N == Eigen::Dynamic) ? Eigen::Dynamic : (Vec ? 1 : M);
  const int64_t n = ctx.idata[0], m = ctx.idata[1];
  const FixCMap<N, N> a(ctx.in[0].data, n, n);
  if constexpr (!Vec) {
    const FixCMap<N, M> b(ctx.in[1].data, n, m);
    if constexpr (N == 2) {
      stan::math::check_square("quad_form", "A", a);
      stan::math::check_multiplicable("quad_form", "A", a, "B", b);
      FixMap<M, M>(ctx.out.data, m, m) =
          b.transpose() * a * b + FixM<M, M>::Zero();
    } else {
      FixMap<M, M>(ctx.out.data, m, m) = stan::math::quad_form(a, b);
    }
  } else {
    const FixM<N, 1> b = Eigen::Map<const FixM<N, 1>>(ctx.in[1].data, n);
    if (!(ctx.variant & 2u)) {
      if constexpr (N == Eigen::Dynamic) {
        ctx.out.data[0] = stan::math::quad_form(a, b);
      } else {
        stan::math::check_square("quad_form", "A", a);
        stan::math::check_multiplicable("quad_form", "A", a, "B", b);
        const FixM<N, 1> y = a * b + FixM<N, 1>::Zero();
        ctx.out.data[0] = b.dot(y);
      }
      return;
    }
    stan::math::check_square("quad_form", "A", a);
    stan::math::check_multiplicable("quad_form", "A", a, "B", b);
    const FixM<K, K> c = quad_vec_chain<N>(a, b);
    ctx.out.data[0] = c(0, 0);
  }
}
template <typename F>
void qf_shape_dispatch(const KernelCtx& ctx, F&& f) {
  const int64_t n = ctx.idata[0];
  const bool vec = ctx.variant & 1u;
  if (n == 2 && fixed_small_matrices()) {
    if (vec) return f(IntC<2>{}, IntC<1>{}, std::true_type{});
    if (ctx.idata[1] == 2) return f(IntC<2>{}, IntC<2>{}, std::false_type{});
  }
  if (vec) return f(IntC<-1>{}, IntC<-1>{}, std::true_type{});
  f(IntC<-1>{}, IntC<-1>{}, std::false_type{});
}
void qf_fwd(KernelCtx& ctx) {
  qf_shape_dispatch(
      ctx, [&](auto N, auto M, auto Vec) { qf_fwd_t<N, M, Vec>(ctx); });
}
template <int N, int M>
void qf_bwd_t(KernelCtx& ctx, bool vec) {
  if (!ctx.in_adj[0].data && !ctx.in_adj[1].data) return;
  const int64_t n = ctx.idata[0], m = vec ? 1 : ctx.idata[1];
  const FixCMap<N, N> a(ctx.in[0].data, n, n);
  const FixCMap<N, M> b(ctx.in[1].data, n, m);
  FixM<M, M> g(m, m);
  if (vec) {
    g(0, 0) = ctx.out_adj;
  } else {
    g = FixCMap<M, M>(ctx.out_adj_vec.data, m, m);
  }
  if (ctx.in_adj[0].data)
    FixMap<N, N>(ctx.in_adj[0].data, n, n) += b * g * b.transpose();
  if (ctx.in_adj[1].data)
    FixMap<N, M>(ctx.in_adj[1].data, n, m) +=
        a * b * g.transpose() + a.transpose() * b * g;
}
void qf_bwd_impl(KernelCtx& ctx, bool vec) {
  with_small_nm(ctx.idata[0], vec ? 1 : ctx.idata[1],
                [&](auto N, auto M) { qf_bwd_t<N, M>(ctx, vec); });
}
void qf_bwd(KernelCtx& ctx) { qf_bwd_impl(ctx, ctx.variant & 1u); }

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
template <int N, int M, bool Vec>
void qfs_fwd_t(KernelCtx& ctx) {
  constexpr int K = (N == Eigen::Dynamic) ? Eigen::Dynamic : (Vec ? 1 : M);
  const int64_t n = ctx.idata[0], m = ctx.idata[1];
  const FixCMap<N, N> a(ctx.in[0].data, n, n);
  if constexpr (!Vec) {
    const FixCMap<N, M> b(ctx.in[1].data, n, m);
    if constexpr (N == 2) {
      stan::math::check_multiplicable("quad_form_sym", "A", a, "B", b);
      stan::math::check_symmetric("quad_form_sym", "A", a);
      const FixM<M, M> ret = b.transpose() * a * b + FixM<M, M>::Zero();
      FixMap<M, M>(ctx.out.data, m, m) = 0.5 * (ret + ret.transpose());
    } else {
      FixMap<M, M>(ctx.out.data, m, m) = stan::math::quad_form_sym(a, b);
    }
  } else {
    const FixM<N, 1> b = Eigen::Map<const FixM<N, 1>>(ctx.in[1].data, n);
    if (!(ctx.variant & 2u)) {
      if constexpr (N == Eigen::Dynamic) {
        ctx.out.data[0] = stan::math::quad_form_sym(a, b);
      } else {
        stan::math::check_multiplicable("quad_form_sym", "A", a, "B", b);
        stan::math::check_symmetric("quad_form_sym", "A", a);
        const FixM<N, 1> y = a * b + FixM<N, 1>::Zero();
        ctx.out.data[0] = b.dot(y);
      }
      return;
    }
    stan::math::check_multiplicable("quad_form_sym", "A", a, "B", b);
    stan::math::check_symmetric("quad_form_sym", "A", a);
    FixM<K, K> c = quad_vec_chain<N>(a, b);
    const FixM<K, K> sym = 0.5 * (c + c.transpose());
    c = sym;
    ctx.out.data[0] = c(0, 0);
  }
}
void qfs_fwd(KernelCtx& ctx) {
  qf_shape_dispatch(
      ctx, [&](auto N, auto M, auto Vec) { qfs_fwd_t<N, M, Vec>(ctx); });
}
void qfs_bwd(KernelCtx& ctx) { qf_bwd_impl(ctx, ctx.variant & 1u); }

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

// Under propto, Stan Math drops the terms whose arguments are all data, so a
// data argument has to reach it as double. Calls f with x when active, and
// with its value otherwise. Bind=false keeps x as var, for an argument whose
// binding cannot change which terms Stan Math keeps.
template <bool Bind = true, typename T, typename F>
stan::math::var bind_active(bool active, const T& x, F&& f) {
  if constexpr (Bind)
    if (!active) return f(stan::math::value_of(x));
  return f(x);
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
  if (!values_only(ctx)) {
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

template <typename Scalar, typename Input>
std::vector<Eigen::Matrix<Scalar, Eigen::Dynamic, 1>> mvt_vectors(
    const Input& input, int64_t width, int64_t count) {
  using Vector = Eigen::Matrix<Scalar, Eigen::Dynamic, 1>;
  std::vector<Vector> result((size_t)count, Vector(width));
  for (int64_t k = 0; k < count; ++k)
    for (int64_t i = 0; i < width; ++i)
      result[(size_t)k](i) = input.data[k * width + i];
  return result;
}

template <typename Vectors, typename F>
decltype(auto) with_mvt_argument(Vectors& values, bool array, F&& f) {
  return array ? std::forward<F>(f)(values) : std::forward<F>(f)(values[0]);
}

template <bool Grad, MnKind Kind = kMnCov>
double mn_eval(KernelCtx& ctx) {
  const VectorizedLayout layout = vectorized_layout(ctx.idata, ctx.n_idata, 2);
  const int64_t n = layout.width;
  const bool y_array = layout.is_array(0), mu_array = layout.is_array(1);
  const bool propto = density_mode(ctx).propto;
  const unsigned mask = density_mode(ctx).active;
  const bool ay = mask & 1u, am = mask & 2u, aS = mask & 4u;
  stan::math::nested_rev_autodiff nested;
  using stan::math::var;
  // Bind only the representation selected by activity. Inactive var copies
  // and active double copies were never passed to Stan Math. Precision's
  // replay retains its original inactive-var scatter contract.
  std::vector<VarV> ys, mus;
  std::vector<VecD> ysd, musd;
  if (ay || Kind == kMnPrec)
    ys = mvt_vectors<var>(ctx.in[0], n, layout.count[0]);
  if (!ay) ysd = mvt_vectors<double>(ctx.in[0], n, layout.count[0]);
  if (am || Kind == kMnPrec)
    mus = mvt_vectors<var>(ctx.in[1], n, layout.count[1]);
  if (!am) musd = mvt_vectors<double>(ctx.in[1], n, layout.count[1]);
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
    return am ? with_mvt_argument(mus, mu_array, invoke)
              : with_mvt_argument(musd, mu_array, invoke);
  };
  var out = ay ? with_mvt_argument(ys, y_array, dispatch_mu)
               : with_mvt_argument(ysd, y_array, dispatch_mu);

  if constexpr (Kind == kMnPrec) {
    return finish_tail_density<Grad>(ctx, out, ys, mus, S);
  } else {
    if (!values_only(ctx)) {
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
  if (ctx.n_in != 3 || ctx.out.len != 1) return false;
  // Array operands keep the established path.
  const VectorizedLayout layout = vectorized_layout(ctx.idata, ctx.n_idata, 2);
  if (layout.is_array(0) || layout.is_array(1)) return false;
  const int64_t n = layout.width;
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
  const int64_t n = vectorized_layout(ctx.idata, ctx.n_idata, 2).width;
  const unsigned mask = density_mode(ctx).active & 7u;
  const bool propto = density_mode(ctx).propto;
  const bool record = !values_only(ctx);
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
enum MncPath { kMncGeneric, kMncLegacy, kMncSmall };

inline MncPath mnc_path(uint8_t variant, const int* idata, int64_t n_idata,
                        int64_t n_in, int64_t len_y, int64_t len_mu,
                        int64_t len_l, int64_t len_out) {
  if (n_in != 3 || len_out != 1) return kMncGeneric;
  const VectorizedLayout layout = vectorized_layout(idata, n_idata, 2);
  if (layout.is_array(0) || layout.is_array(1)) return kMncGeneric;
  const int64_t n = layout.width;
  if (len_y != n || len_mu != n || len_l != n * n) return kMncGeneric;
  const DensityMode mode = DensityMode::of(variant);
  const unsigned mask = mode.active & 7u;
  if (n >= 1 && n <= 2 && mask != 0 && fixed_small_matrices()) return kMncSmall;
  // Propto with only the Cholesky factor active.
  return mode.propto && !mode.elementwise && mode.active == 0x4u ? kMncLegacy
                                                                 : kMncGeneric;
}

MncPath mnc_path(const KernelCtx& ctx) {
  return mnc_path(ctx.variant, ctx.idata, ctx.n_idata, ctx.n_in,
                  ctx.n_in == 3 ? ctx.in[0].len : -1,
                  ctx.n_in == 3 ? ctx.in[1].len : -1,
                  ctx.n_in == 3 ? ctx.in[2].len : -1, ctx.out.len);
}

// The pinned single-vector Stan Math overload's checks. That overload
// intentionally does not call check_cholesky_factor; changing its observable
// domain here would make the native and fallback paths disagree.
void mnc_legacy_checks(const CMapV& y, const CMapV& mu, const CMapM& L) {
  static constexpr const char* function = "multi_normal_cholesky_lpdf";
  stan::math::check_size_match(function, "Size of random variable", y.size(),
                               "size of location parameter", mu.size());
  stan::math::check_size_match(function, "Size of random variable", y.size(),
                               "rows of covariance parameter", L.rows());
  stan::math::check_size_match(function, "Size of random variable", y.size(),
                               "columns of covariance parameter", L.cols());
  stan::math::check_finite(function, "Location parameter", mu);
  stan::math::check_not_nan(function, "Random variable", y);
}

double mnc_legacy_fwd(KernelCtx& ctx) {
  const int64_t n = vectorized_layout(ctx.idata, ctx.n_idata, 2).width;
  CMapV y(ctx.in[0].data, n), mu(ctx.in[1].data, n);
  CMapM L(ctx.in[2].data, n, n);

  // The overload's checks and arithmetic order.
  mnc_legacy_checks(y, mu, L);
  if (n == 0) return 0.0;

  VecD y_minus_mu = y - mu;
  MatD inv_L = stan::math::mdivide_left_tri<Eigen::Lower>(L);
  Eigen::RowVectorXd half;
  half = (inv_L.template triangularView<Eigen::Lower>() *
          y_minus_mu.template cast<double>())
             .transpose();

  VecD scaled_diff;
  if (!values_only(ctx)) {
    scaled_diff =
        (half * inv_L.template triangularView<Eigen::Lower>()).transpose();
  }

  double logp(0.0);
  logp += stan::math::sum(stan::math::log(inv_L.diagonal()));
  if (!values_only(ctx))
    MapM(ctx.scratch, n, n) = scaled_diff * half - inv_L.transpose();
  logp -= 0.5 * stan::math::sum(stan::math::dot_self(half));
  return logp;
}

template <int N>
double mnc_small_fwd(KernelCtx& ctx) {
  static constexpr const char* function = "multi_normal_cholesky_lpdf";
  const int64_t n = vectorized_layout(ctx.idata, ctx.n_idata, 2).width;
  const unsigned mask = density_mode(ctx).active & 7u;
  const bool propto = density_mode(ctx).propto;
  const bool record = !values_only(ctx);
  const FixM<N, 1> y = FixCMap<N, 1>(ctx.in[0].data, n, 1);
  const FixM<N, 1> mu = FixCMap<N, 1>(ctx.in[1].data, n, 1);
  const FixM<N, N> L = FixCMap<N, N>(ctx.in[2].data, n, n);
  stan::math::check_size_match(function, "Size of random variable", y.size(),
                               "size of location parameter", mu.size());
  stan::math::check_size_match(function, "Size of random variable", y.size(),
                               "rows of covariance parameter", L.rows());
  stan::math::check_size_match(function, "Size of random variable", y.size(),
                               "columns of covariance parameter", L.cols());
  stan::math::check_finite(function, "Location parameter", mu);
  stan::math::check_not_nan(function, "Random variable", y);

  double logp = 0.0;
  if (!propto) logp += stan::math::NEG_LOG_SQRT_TWO_PI * n;
  const FixM<N, 1> y_minus_mu = y - mu;
  Eigen::Matrix<double, 1, N> half;
  FixM<N, 1> scaled_diff;
  FixM<N, N> l_partial;
  if (!(mask & 4u)) {
    // Stan Math's mdivide_left_tri and mdivide_right_tri, as Eigen's
    // dynamic triangular solver computes them: it multiplies by the
    // reciprocal of each diagonal entry.
    const double r0 = 1.0 / L(0, 0);
    half(0) = y_minus_mu(0) * r0;
    if constexpr (N == 2) {
      const double r1 = 1.0 / L(1, 1);
      half(1) = (y_minus_mu(1) - half(0) * L(1, 0)) * r1;
      scaled_diff(1) = half(1) * r1;
      scaled_diff(0) = (half(0) - L(1, 0) * scaled_diff(1)) * r0;
    } else {
      scaled_diff(0) = half(0) * r0;
    }
    if (!propto) logp -= stan::math::sum(stan::math::log(L.diagonal()));
  } else {
    const FixM<N, N> inv_L = stan::math::mdivide_left_tri<Eigen::Lower>(L);
    half = (inv_L.template triangularView<Eigen::Lower>() * y_minus_mu)
               .transpose();
    if (record) {
      scaled_diff =
          (half * inv_L.template triangularView<Eigen::Lower>()).transpose();
      l_partial = scaled_diff * half - inv_L.transpose();
    }
    logp += stan::math::sum(stan::math::log(inv_L.diagonal()));
  }
  logp -= 0.5 * stan::math::sum(stan::math::dot_self(half));
  if (record) {
    FixMap<N, 1> py(ctx.scratch, n, 1), pmu(ctx.scratch + n, n, 1);
    FixMap<N, N> pl(ctx.scratch + 2 * n, n, n);
    py.setZero();
    pmu.setZero();
    pl.setZero();
    if (mask & 1u) py -= scaled_diff;
    if (mask & 2u) pmu += scaled_diff;
    if (mask & 4u) pl += l_partial;
  }
  return logp;
}

void mnc_fwd(KernelCtx& ctx) {
  switch (mnc_path(ctx)) {
    case kMncSmall:
      ctx.out.data[0] = vectorized_layout(ctx.idata, ctx.n_idata, 2).width == 1
                            ? mnc_small_fwd<1>(ctx)
                            : mnc_small_fwd<2>(ctx);
      return;
    case kMncLegacy:
      ctx.out.data[0] = mnc_legacy_fwd(ctx);
      return;
    default:
      ctx.out.data[0] = mn_eval<false, kMnChol>(ctx);
  }
}
void mnc_bwd(KernelCtx& ctx) {
  if (mnc_path(ctx) != kMncLegacy) {
    tail_density_bwd<3>(ctx);
    return;
  }
  if (!ctx.in_adj[2].data) return;
  const int64_t n = vectorized_layout(ctx.idata, ctx.n_idata, 2).width;
  for (int64_t i = 0; i < n * n; ++i)
    ctx.in_adj[2].data[i] += ctx.out_adj * ctx.scratch[i];
}

int64_t mnc_scratch(const Op& op, const Slot* slots) {
  if (op.n_in != 3 || op.out < 0 || slots == nullptr || op.in[0] < 0 ||
      op.in[1] < 0 || op.in[2] < 0)
    return 0;
  if (mnc_path(op.variant, op.idata, op.n_idata, op.n_in, slots[op.in[0]].len,
               slots[op.in[1]].len, slots[op.in[2]].len,
               slots[op.out].len) == kMncLegacy) {
    const int64_t n = vectorized_layout(op.idata, op.n_idata, 2).width;
    return n * n;
  }
  return tail_density_scratch<3>(op, slots);
}

// ---- general matrix product: out = A * B ----------------------------------
// idata = {rows_a, cols_a, cols_b}; either side may carry adjoints.
// A retained loop can give a matrix-vector product a live row count below
// idata[0]; the left operand then holds its live entries packed at the front.
int64_t gemm_rows(const KernelCtx& ctx, int64_t live) {
  return ctx.dyn_lengths ? live : ctx.idata[0];
}
void gemm_fwd_dynamic(KernelCtx& ctx) {
  const int64_t ra = gemm_rows(ctx, ctx.out.len),
                ca = live_product_cols(ctx, 3), cb = ctx.idata[2];
  CMapM A(ctx.in[0].data, ra, ca);
  CMapM B(ctx.in[1].data, ca, cb);
  MapM(ctx.out.data, ra, cb) = A * B;
}
void gemm_bwd_dynamic(KernelCtx& ctx) {
  const int64_t ra = gemm_rows(ctx, ctx.out_adj_vec.len),
                ca = live_product_cols(ctx, 3), cb = ctx.idata[2];
  CMapM A(ctx.in[0].data, ra, ca);
  CMapM B(ctx.in[1].data, ca, cb);
  CMapM dO(ctx.out_adj_vec.data, ra, cb);
  const auto scalar_adjoint = dO.unaryExpr([](double value) { return value; });
  // The mixed Stan Math overloads consume Matrix<var>::adj_op() directly;
  // only the two-active-input overload evaluates that view into doubles.
  if (ctx.in_adj[0].data) {
    if (ctx.in_adj[1].data)
      MapM(ctx.in_adj[0].data, ra, ca) += dO * B.transpose();
    else
      MapM(ctx.in_adj[0].data, ra, ca) += scalar_adjoint * B.transpose();
  }
  if (ctx.in_adj[1].data) {
    if (ctx.in_adj[0].data)
      MapM(ctx.in_adj[1].data, ca, cb) += A.transpose() * dO;
    else
      MapM(ctx.in_adj[1].data, ca, cb) += A.transpose() * scalar_adjoint;
  }
}
template <int CB>
void gemm_fwd_2x2(KernelCtx& ctx) {
  const int64_t cb = ctx.idata[2];
  FixCMap<2, 2> A(ctx.in[0].data, 2, 2);
  FixCMap<2, CB> B(ctx.in[1].data, 2, cb);
  FixMap<2, CB>(ctx.out.data, 2, cb) = (A * B).eval() + FixM<2, CB>::Zero();
}
template <int CB>
void gemm_bwd_2x2(KernelCtx& ctx) {
  const int64_t cb = ctx.idata[2];
  FixCMap<2, 2> A(ctx.in[0].data, 2, 2);
  FixCMap<2, CB> B(ctx.in[1].data, 2, cb);
  FixCMap<2, CB> dO(ctx.out_adj_vec.data, 2, cb);
  const auto scalar_adjoint = dO.unaryExpr([](double value) { return value; });
  if (ctx.in_adj[0].data) {
    FixMap<2, 2> dA(ctx.in_adj[0].data, 2, 2);
    if (ctx.in_adj[1].data)
      dA += dO * B.transpose();
    else
      dA += scalar_adjoint * B.transpose();
  }
  if (ctx.in_adj[1].data) {
    FixMap<2, CB> dB(ctx.in_adj[1].data, 2, cb);
    if (ctx.in_adj[0].data)
      dB += A.transpose() * dO;
    else
      dB += A.transpose() * scalar_adjoint;
  }
}
bool gemm_is_2x2(const KernelCtx& ctx) {
  return fixed_small_matrices() && !ctx.dyn_lengths && ctx.idata[0] == 2 &&
         ctx.idata[1] == 2 && (ctx.idata[2] == 1 || ctx.idata[2] == 2);
}
void gemm_fwd(KernelCtx& ctx) {
  if (!gemm_is_2x2(ctx)) return gemm_fwd_dynamic(ctx);
  ctx.idata[2] == 1 ? gemm_fwd_2x2<1>(ctx) : gemm_fwd_2x2<2>(ctx);
}
void gemm_bwd(KernelCtx& ctx) {
  if (!gemm_is_2x2(ctx)) return gemm_bwd_dynamic(ctx);
  ctx.idata[2] == 1 ? gemm_bwd_2x2<1>(ctx) : gemm_bwd_2x2<2>(ctx);
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
  if ((ctx.variant & 1u) && !values_only(ctx))
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
  if ((ctx.variant & 1u) && !values_only(ctx)) {
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

// ---- lkj_corr_cholesky_lpdf(L | eta) --------------------------------------
// in = {L, eta}; idata = {K}. eta is an ordinary differentiable shape
// parameter (activity bit 1); binding it as data used to zero d(lp)/d(eta)
// silently and, under propto, drop normalization terms that are not
// constant when eta is a parameter -- a 0.5% CmdStan gradient divergence
// found by the signature-reference gate. The double binding remains the
// data fast path and keeps that instantiation's tape unchanged.
template <bool Grad, bool Chol = true>
double lkj_eval(KernelCtx& ctx) {
  const int64_t K = ctx.idata[0];
  const bool propto = density_mode(ctx).propto;
  stan::math::nested_rev_autodiff nested;
  using stan::math::var;
  VarM L(K, K);
  for (int64_t j = 0; j < K; ++j)
    for (int64_t i = 0; i < K; ++i) L(i, j) = ctx.in[0].data[j * K + i];
  var eta(ctx.in[1].data[0]);
  var out;
  const auto call = [&](const auto& l, const auto& shape) -> var {
    if constexpr (Chol) {
      return propto ? stan::math::lkj_corr_cholesky_lpdf<true>(l, shape)
                    : stan::math::lkj_corr_cholesky_lpdf<false>(l, shape);
    } else {
      return propto ? stan::math::lkj_corr_lpdf<true>(l, shape)
                    : stan::math::lkj_corr_lpdf<false>(l, shape);
    }
  };
  if (density_mode(ctx).is_active(1))
    out = call(L, eta);
  else
    out =
        bind_active(!propto || density_mode(ctx).is_active(0), L,
                    [&](const auto& l) { return call(l, ctx.in[1].data[0]); });
  return finish_tail_density<Grad>(ctx, out, L, eta);
}
void lkj_fwd(KernelCtx& ctx) { ctx.out.data[0] = lkj_eval<false>(ctx); }
void lkj_bwd(KernelCtx& ctx) { lkj_eval<true>(ctx); }
// lkj_corr takes the correlation matrix itself where the cholesky form
// takes its factor: identical argument shapes, so identical kernel.
void lkjc_fwd(KernelCtx& ctx) { ctx.out.data[0] = lkj_eval<false, false>(ctx); }
void lkjc_bwd(KernelCtx& ctx) { lkj_eval<true, false>(ctx); }

// ---- normal_id_glm_lpdf(y | X, alpha, beta, sigma) ------------------------
// Preserve the established activity and broadcast overloads while recording
// Stan Math's analytical partials directly, without a temporary var tape.
double nid_glm_eval(KernelCtx& ctx) {
  const GlmLayout layout = glm_layout(ctx.idata, ctx.n_idata);
  const int64_t rows = layout.rows, cols = layout.cols;
  const DensityMode mode = density_mode(ctx);
  const bool propto = mode.propto;
  const bool y_var = mode.is_active(0);
  const bool x_var = mode.is_active(1);
  sink s;
  double* partials = ctx.scratch;
  for (int k = 0; k < 5; ++k) {
    if ((k == 0 && !y_var) || (k == 1 && !x_var)) continue;
    s.len[k] = ctx.in[k].len;
    if (!values_only(ctx)) {
      s.buf[k] = partials;
      partials += ctx.in[k].len;
    }
  }
  sink_scope active(s);
  const bool one_y = ctx.in[0].len == 1;
  CMapV yd(ctx.in[0].data, ctx.in[0].len);
  CMapM Xd(ctx.in[1].data, rows, cols);
  const auto Xv = as_rvar_matrix(ctx.in[1], rows, cols);
  const auto yv = as_rvar(ctx.in[0]);
  const auto alpha = as_rvar(ctx.in[2]);
  const auto beta = as_rvar(ctx.in[3]);
  const auto sigma = as_rvar(ctx.in[4]);
  const bool one_a = ctx.in[2].len == 1, one_s = ctx.in[4].len == 1;
  const bool sigma_data = propto && !mode.is_active(4);
  const bool others_data = propto && (mode.active & 0xfu) == 0;
  if (!values_only(ctx) && sigma_data)
    for (int k = others_data ? 2 : 4; k <= 4; ++k)
      if (s.buf[k]) std::fill_n(s.buf[k], ctx.in[k].len, 0.0);
  auto call = [&](const auto& y, const auto& x, const auto& a,
                  const auto& scale) {
    record_probability_call([&] {
      return propto
                 ? stan::math::normal_id_glm_lpdf<true>(y, x, a, beta, scale)
                 : stan::math::normal_id_glm_lpdf<false>(y, x, a, beta, scale);
    });
  };
  auto call_data_scale = [&](const auto& y, const auto& x, const auto& a,
                             const auto& b, const auto& scale) {
    record_probability_call([&] {
      return stan::math::normal_id_glm_lpdf<true>(y, x, a, b, scale);
    });
  };
  auto with_scale_data = [&](auto&& f) {
    if (one_s)
      f(ctx.in[4].data[0]);
    else
      f(CMapV(ctx.in[4].data, ctx.in[4].len));
  };
  auto dispatch = [&](const auto& y, const auto& x) {
    if (sigma_data) {
      with_scale_data([&](const auto& scale) {
        if (one_a)
          call_data_scale(y, x, alpha(0), beta, scale);
        else
          call_data_scale(y, x, alpha, beta, scale);
      });
    } else if (one_a && one_s) {
      call(y, x, alpha(0), sigma(0));
    } else if (one_a) {
      call(y, x, alpha(0), sigma);
    } else if (one_s) {
      call(y, x, alpha, sigma(0));
    } else {
      call(y, x, alpha, sigma);
    }
  };
  const bool row_x = layout.row_vector_design();
  auto pick_x = [&](const auto& y) {
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
  if (sigma_data && others_data) {
    const CMapV beta_data(ctx.in[3].data, ctx.in[3].len);
    auto all_data = [&](const auto& y, const auto& x) {
      with_scale_data([&](const auto& scale) {
        if (one_a)
          call_data_scale(y, x, ctx.in[2].data[0], beta_data, scale);
        else
          call_data_scale(y, x, CMapV(ctx.in[2].data, ctx.in[2].len), beta_data,
                          scale);
      });
    };
    auto pick_data_x = [&](const auto& y) {
      if (row_x)
        all_data(y, Xd.row(0));
      else
        all_data(y, Xd);
    };
    if (one_y)
      pick_data_x(ctx.in[0].data[0]);
    else
      pick_data_x(yd);
  } else if (y_var) {
    if (one_y)
      pick_x(yv(0));
    else
      pick_x(yv);
  } else if (one_y) {
    pick_x(ctx.in[0].data[0]);
  } else {
    pick_x(yd);
  }
  // The previous unit reverse accumulated each partial into an initially
  // positive-zero adjoint. Preserve that addition, including signed zero.
  if (!values_only(ctx))
    for (int k = 0; k < 5; ++k)
      if (s.buf[k])
        for (int64_t i = 0; i < s.len[k]; ++i) s.buf[k][i] = 0.0 + s.buf[k][i];
  return s.value;
}

int64_t nid_glm_scratch(const Op& op, const Slot* slots) {
  // The y and X sections exist only when those inputs are active (activity
  // bits 0 and 1), but this hook reserves both unconditionally.
  return slots[op.in[0]].len + slots[op.in[1]].len + slots[op.in[2]].len +
         slots[op.in[3]].len + slots[op.in[4]].len;
}

void nid_glm_fwd(KernelCtx& ctx) { ctx.out.data[0] = nid_glm_eval(ctx); }

void nid_glm_bwd(KernelCtx& ctx) {
  const DensityMode mode = density_mode(ctx);
  const unsigned mask = mode.active;
  const double* s = ctx.scratch;
  const double w = ctx.out_adj;
  // Mirror of the forward's scratch layout: y's partials are present
  // exactly when y is active.
  if (mode.is_active(0)) {
    if (ctx.in_adj[0].data)
      for (int64_t i = 0; i < ctx.in[0].len; ++i)
        ctx.in_adj[0].data[i] += w * s[i];
    s += ctx.in[0].len;
  }
  if (mode.is_active(1)) {
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
  if (values_only(ctx)) {
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
  if (!values_only(ctx))
    Eigen::Map<VecD>(ctx.scratch, n) = solver.eigenvalues();
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
// Everything below binds its arguments as var, calls the unmodified
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
// Without propto it is one instantiation. Under propto a data argument goes
// in as double where that changes which terms Stan Math keeps (bind_active);
// otherwise its partials are computed and dropped.
// ---- wishart family: (matrix W, real nu, matrix S), four of them --------
enum WishKind { kWishart, kInvWishart, kWishartChol, kInvWishartChol };
template <bool Grad, WishKind Kind>
double wish_eval(KernelCtx& ctx) {
  const int64_t K = ctx.idata[0];
  const bool propto = density_mode(ctx).propto;
  const unsigned mask = density_mode(ctx).active & 0x7u;
  stan::math::nested_rev_autodiff nested;
  using stan::math::var;
  VarM W = tail_m(ctx, 0, K, K);
  var nu = ctx.in[1].data[0];
  VarM S = tail_m(ctx, 2, K, K);
  const auto call = [](auto propto_tag, const auto& w, const auto& n,
                       const auto& s) -> var {
    constexpr bool P = decltype(propto_tag)::value;
    if constexpr (Kind == kWishart)
      return stan::math::wishart_lpdf<P>(w, n, s);
    else if constexpr (Kind == kInvWishart)
      return stan::math::inv_wishart_lpdf<P>(w, n, s);
    else if constexpr (Kind == kWishartChol)
      return stan::math::wishart_cholesky_lpdf<P>(w, n, s);
    else
      return stan::math::inv_wishart_cholesky_lpdf<P>(w, n, s);
  };
  // The Cholesky forms keep one term for nu alone and one for all three
  // arguments together, so only nu's binding matters there.
  constexpr bool chol = Kind == kWishartChol || Kind == kInvWishartChol;
  var out;
  if (!propto) {
    out = call(std::false_type{}, W, nu, S);
  } else {
    out = bind_active<!chol>(mask & 1u, W, [&](const auto& w) {
      return bind_active(mask & 2u, nu, [&](const auto& n) {
        return bind_active<!chol>(mask & 4u, S, [&](const auto& s) {
          return call(std::true_type{}, w, n, s);
        });
      });
    });
  }
  const double value = finish_tail_density<Grad>(ctx, out, W, nu, S);
  return propto && mask == 0 ? 0.0 : value;
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

// ---- multi_gp: (matrix y, matrix Sigma, vector w), y K x N, Sigma N x N --
enum GpKind { kMultiGp, kMultiGpChol };
template <bool Grad, GpKind Kind>
double mgp_eval(KernelCtx& ctx) {
  const int64_t K = ctx.idata[0], N = ctx.idata[1];
  const bool propto = density_mode(ctx).propto;
  stan::math::nested_rev_autodiff nested;
  using stan::math::var;
  const unsigned mask = density_mode(ctx).active & 0x7u;
  VarM y = tail_m(ctx, 0, K, N);
  VarM S = tail_m(ctx, 1, N, N);
  VarV w = tail_v(ctx, 2, K);
  const auto call = [](auto propto_tag, const auto& a, const auto& b,
                       const auto& c) -> var {
    constexpr bool P = decltype(propto_tag)::value;
    if constexpr (Kind == kMultiGp)
      return stan::math::multi_gp_lpdf<P>(a, b, c);
    else
      return stan::math::multi_gp_cholesky_lpdf<P>(a, b, c);
  };
  // y enters only the term that also takes Sigma and w, so y's binding
  // matters only when all three are data.
  var out;
  if (!propto) {
    out = call(std::false_type{}, y, S, w);
  } else {
    out = bind_active(mask & 2u, S, [&](const auto& b) {
      return bind_active(mask & 4u, w, [&](const auto& c) {
        return call(std::true_type{}, y, b, c);
      });
    });
  }
  const double value = finish_tail_density<Grad>(ctx, out, y, S, w);
  return propto && mask == 0 ? 0.0 : value;
}

// ---- multi_student_t: (vector y, real nu, vector mu, matrix Sigma) ------
enum MstKind { kMst, kMstChol };
template <bool Grad, MstKind Kind>
double mst_eval(KernelCtx& ctx) {
  const VectorizedLayout layout = vectorized_layout(ctx.idata, ctx.n_idata, 2);
  const int64_t K = layout.width;
  const bool y_array = layout.is_array(0), mu_array = layout.is_array(1);
  const bool propto = density_mode(ctx).propto;
  stan::math::nested_rev_autodiff nested;
  using stan::math::var;
  std::vector<VarV> ys = mvt_vectors<var>(ctx.in[0], K, layout.count[0]);
  var nu = ctx.in[1].data[0];
  std::vector<VarV> mus = mvt_vectors<var>(ctx.in[2], K, layout.count[1]);
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
    return with_mvt_argument(
        mus, mu_array, [&](auto&& muarg) -> var { return call(yarg, muarg); });
  };
  var out = with_mvt_argument(ys, y_array, dispatch_mu);
  const double value = finish_tail_density<Grad>(ctx, out, ys, nu, mus, S);
  // Every argument is bound as var, so Stan Math kept the terms it drops
  // under propto when nu or Sigma is data (activity bits 1 and 3). Take
  // them back out; with every argument data it keeps nothing.
  const unsigned active = density_mode(ctx).active & 0xfu;
  if (!propto) return value;
  if (active == 0) return 0.0;
  const double n = static_cast<double>(std::max(ys.size(), mus.size()));
  const double nu_val = nu.val();
  double dropped = 0.0;
  if (!(active & 0x2u))
    dropped +=
        n * (stan::math::lgamma(0.5 * (nu_val + K)) -
             stan::math::lgamma(0.5 * nu_val) - 0.5 * K * std::log(nu_val));
  if (!(active & 0x8u)) {
    const Eigen::MatrixXd s = stan::math::value_of(S);
    if constexpr (Kind == kMst) {
      auto ldlt = stan::math::make_ldlt_factor(s);
      dropped -= n * 0.5 * stan::math::log_determinant_ldlt(ldlt);
    } else {
      dropped -= n * s.diagonal().array().log().sum();
    }
  }
  return value - dropped;
}

// ---- multinomial family: (array[] int ns, vector theta) -----------------
// The counts ride in idata; theta is the only propagator edge.
enum MultKind { kMultinomialSoftmax, kDirichletMultinomial };
template <bool Grad, MultKind Kind>
double mult_eval(KernelCtx& ctx) {
  const int64_t K = ctx.in[0].len;
  const bool propto = density_mode(ctx).propto;
  std::vector<int> ns(ctx.idata, ctx.idata + ctx.n_idata);
  stan::math::nested_rev_autodiff nested;
  using stan::math::var;
  VarV theta = tail_v(ctx, 0, K);
  const auto call = [&](const auto& t) -> var {
    if constexpr (Kind == kMultinomialSoftmax) {
      return propto ? stan::math::multinomial_lpmf<true>(ns,
                                                         stan::math::softmax(t))
                    : stan::math::multinomial_lpmf<false>(
                          ns, stan::math::softmax(t));
    } else {
      return propto ? stan::math::dirichlet_multinomial_lpmf<true>(ns, t)
                    : stan::math::dirichlet_multinomial_lpmf<false>(ns, t);
    }
  };
  var out = bind_active(!propto || density_mode(ctx).is_active(0), theta, call);
  return finish_tail_density<Grad>(ctx, out, theta);
}

void check_multinomial_counts(const KernelCtx& ctx, const char* fn,
                              const char* rows) {
  stan::math::check_size_match(fn, "Size of number of trials variable",
                               static_cast<size_t>(ctx.n_idata), rows,
                               static_cast<Eigen::Index>(ctx.in[0].len));
  if (std::any_of(ctx.idata, ctx.idata + ctx.n_idata,
                  [](int count) { return count < 0; }))
    stan::math::check_nonnegative(
        fn, "Number of trials variable",
        std::vector<int>(ctx.idata, ctx.idata + ctx.n_idata));
}

// A second input is the coefficient, which the plan computed once.
double multinomial_coefficient_of(const KernelCtx& ctx, uint16_t opcode) {
  return ctx.n_in == 2
             ? ctx.in[1].data[0]
             : multinomial_coefficient(opcode, ctx.idata, ctx.n_idata);
}

// Multinomial has one independent probability edge per count. Preserve the
// validation and summation order of Stan Math's prim/prob/multinomial_lpmf.hpp.
// The reverse uses the weighted multiply_log rule (rev/fun/multiply_log.hpp),
// multiplying before dividing; caching unit partials changes that rounding.
void multn_fwd(KernelCtx& ctx) {
  const int* counts = ctx.idata;
  const CMapV theta(ctx.in[0].data, ctx.in[0].len);
  constexpr const char* fn = "multinomial_lpmf";
  check_multinomial_counts(ctx, fn, "rows of probabilities parameter");
  stan::math::check_simplex(fn, "Probabilities parameter", theta);
  const DensityMode mode = density_mode(ctx);
  if (mode.propto && !mode.is_active(0)) {
    ctx.out.data[0] = 0;
    return;
  }
  double value =
      mode.propto ? 0.0 : multinomial_coefficient_of(ctx, OP_MULTINOMIAL_LPMF);
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

// multinomial_logit_lpmf follows Stan Math's
// prim/prob/multinomial_logit_lpmf.hpp and the reverse rules of its var
// operations.
//
// multinomial_lpmf(y | softmax(x)) is computed from the logits x. With
// m = max(x) and S = sum(exp(x - m)), log softmax(x)_i is (x_i - m) - log(S),
// and the derivative by x_i is (n_i S - N exp(x_i - m)) / S. Subtracting m
// first keeps the large part of each logit exact. When a logit is not
// finite, or there are none, the kernel calls Stan's multinomial_lpmf of
// softmax(x). That gives Stan's error messages, and Stan's value when a
// logit is -inf.
template <bool Softmax>
bool multinomial_softmax_fallback(const KernelCtx& ctx) {
  if constexpr (!Softmax) return false;
  const CMapV x(ctx.in[0].data, ctx.in[0].len);
  return x.size() == 0 || !x.allFinite();
}

template <bool Softmax>
void multinomial_logits_fwd(KernelCtx& ctx) {
  if (multinomial_softmax_fallback<Softmax>(ctx)) {
    ctx.out.data[0] = mult_eval<false, kMultinomialSoftmax>(ctx);
    return;
  }
  const CMapV x(ctx.in[0].data, ctx.in[0].len);
  if constexpr (Softmax) {
    check_multinomial_counts(ctx, "multinomial_lpmf",
                             "rows of probabilities parameter");
  } else {
    check_multinomial_counts(ctx, "multinomial_logit_lpmf",
                             "rows of log-probabilities parameter");
    stan::math::check_finite("multinomial_logit_lpmf",
                             "log-probabilities parameter", x);
  }
  const DensityMode mode = density_mode(ctx);
  if (mode.propto && !mode.is_active(0)) {
    ctx.out.data[0] = 0;
    return;
  }
  double value = 0.0;
  if (!mode.propto)
    value += multinomial_coefficient_of(
        ctx, Softmax ? OP_MULTINOMIAL_SOFTMAX_LPMF : OP_MULTINOMIAL_LOGIT_LPMF);
  if constexpr (Softmax) {
    const double m = x.maxCoeff();
    double sum = 0.0;
    for (int64_t i = 0; i < x.size(); ++i) sum += std::exp(x(i) - m);
    const double log_sum = std::log(sum);
    ctx.scratch[0] = m;
    ctx.scratch[1] = sum;
    for (int64_t i = 0; i < x.size(); ++i)
      if (ctx.idata[i] != 0) value += ctx.idata[i] * ((x(i) - m) - log_sum);
  } else {
    const double alpha = stan::math::log_sum_exp(x);
    ctx.scratch[0] = alpha;
    for (int64_t i = 0; i < x.size(); ++i)
      if (ctx.idata[i] != 0) value += ctx.idata[i] * (x(i) - alpha);
  }
  ctx.out.data[0] = value;
}

// The logit form adds the adjoints of the terms first, then those of
// log_sum_exp's callback, which calls std::exp for each element. Eigen's
// packet exp would change the low bits.
template <bool Softmax>
void multinomial_logits_bwd(KernelCtx& ctx) {
  if (multinomial_softmax_fallback<Softmax>(ctx)) {
    mult_eval<true, kMultinomialSoftmax>(ctx);
    return;
  }
  double* adj = ctx.in_adj[0].data;
  if (!adj) return;
  const double* x = ctx.in[0].data;
  const int64_t K = ctx.in[0].len;
  if constexpr (Softmax) {
    const double m = ctx.scratch[0], sum = ctx.scratch[1];
    double total = 0.0;
    for (int64_t i = 0; i < K; ++i) total += ctx.idata[i];
    for (int64_t i = 0; i < K; ++i)
      adj[i] +=
          ctx.out_adj * (ctx.idata[i] * sum - total * std::exp(x[i] - m)) / sum;
    return;
  }
  const double alpha = ctx.scratch[0];
  double alpha_adj = 0.0;
  for (int64_t i = K - 1; i >= 0; --i)
    if (ctx.idata[i] != 0) alpha_adj -= ctx.out_adj * ctx.idata[i];
  for (int64_t i = 0; i < K; ++i)
    if (ctx.idata[i] != 0) adj[i] += ctx.out_adj * ctx.idata[i];
  for (int64_t i = 0; i < K; ++i) adj[i] += alpha_adj * std::exp(x[i] - alpha);
}

int64_t multinomial_logits_scratch(const Op&, const Slot*) { return 2; }

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
  const VectorizedLayout layout = vectorized_layout(ctx.idata, ctx.n_idata, 1);
  const int64_t N = layout.n_outcomes;
  const int64_t K = layout.width;
  const int64_t cuts_count = layout.count[0];
  const bool cuts_array = layout.is_array(0);
  const bool propto = density_mode(ctx).propto;
  std::vector<int> y(ctx.idata, ctx.idata + N);
  if constexpr (Kind == kOrderedLogistic) {
    if (propto && (density_mode(ctx).active & 0x3u) == 0) {
      if constexpr (Grad) return 0.0;
      Eigen::VectorXd location =
          Eigen::Map<const Eigen::VectorXd>(ctx.in[0].data, ctx.in[0].len);
      auto data_cuts = mvt_vectors<double>(ctx.in[1], K, cuts_count);
      double value = 0;
      with_mvt_argument(data_cuts, cuts_array, [&](const auto& cutpoints) {
        value = ctx.in[0].len == 1 ? stan::math::ordered_logistic_lpmf<true>(
                                         y, location(0), cutpoints)
                                   : stan::math::ordered_logistic_lpmf<true>(
                                         y, location, cutpoints);
      });
      return value;
    }
  }
  stan::math::nested_rev_autodiff nested;
  using stan::math::var;
  // ordered_probit does not condition its summands on propto or the scalar
  // activity type. Its forward needs only values; retain the original var
  // replay in backward so weighted adjoint arithmetic stays unchanged.
  using Scalar =
      std::conditional_t<!Grad && Kind == kOrderedProbit, double, var>;
  Eigen::Matrix<Scalar, -1, 1> lambda(ctx.in[0].len);
  for (int64_t i = 0; i < ctx.in[0].len; ++i) lambda(i) = ctx.in[0].data[i];
  auto cuts = mvt_vectors<Scalar>(ctx.in[1], K, cuts_count);
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
  with_mvt_argument(cuts, cuts_array, [&](const auto& cutpoints) {
    out = ctx.in[0].len == 1 ? call(lambda(0), cutpoints)
                             : call(lambda, cutpoints);
  });
  if constexpr (!Grad && Kind == kOrderedProbit)
    return out;
  else
    return finish_tail_density<Grad>(ctx, out, lambda, cuts);
}

// With every argument of length 1 the density is called on scalars. Otherwise a
// length-1 argument is spread to the common length as one shared variable:
// stan-math broadcasts scalars, not one-element vectors.
using WienerData = Eigen::Map<const Eigen::VectorXd>;
using WienerVar = stan::arena_t<VarV>;
template <bool Scalar, bool Active>
using WienerArg =
    std::conditional_t<Active,
                       std::conditional_t<Scalar, stan::math::var, WienerVar>,
                       std::conditional_t<Scalar, double, WienerData>>;
unsigned wiener_activity(const KernelCtx& ctx) {
  return density_mode(ctx).active & 0x1fu;
}
int64_t wiener_extent(const KernelCtx& ctx) {
  int64_t n = 1;
  for (int k = 0; k < 5; ++k)
    if (ctx.in[k].len != 1) n = ctx.in[k].len;
  return n;
}
template <bool Scalar, bool Active>
WienerArg<Scalar, Active> wiener_load(const KernelCtx& ctx, int k, int64_t n) {
  const double* d = ctx.in[k].data;
  if constexpr (Scalar) {
    if constexpr (Active)
      return stan::math::var(d[0]);
    else
      return d[0];
  } else {
    const bool spread = ctx.in[k].len == 1;
    const int64_t len = spread ? n : ctx.in[k].len;
    if constexpr (!Active) {
      if (!spread) return WienerData(d, len);
      double* wide =
          stan::math::ChainableStack::instance_->memalloc_.alloc_array<double>(
              len);
      std::fill_n(wide, len, d[0]);
      return WienerData(wide, len);
    } else {
      WienerVar v(len);
      if (spread)
        v.setConstant(stan::math::var(d[0]));
      else
        for (int64_t i = 0; i < len; ++i) v(i) = d[i];
      return v;
    }
  }
}
double wiener_adjoint(const stan::math::var& x, int64_t) { return x.adj(); }
double wiener_adjoint(const WienerVar& v, int64_t i) {
  return v.size() ? v(i).adj() : 0.0;
}
enum class WienerPass { Value, Partials, Seeded };
template <typename Arg>
void wiener_collect(KernelCtx& ctx, int k, const Arg& a, WienerPass pass,
                    double*& stash) {
  constexpr bool active =
      std::is_same_v<Arg, stan::math::var> || std::is_same_v<Arg, WienerVar>;
  const int64_t len = ctx.in[k].len;
  if (pass == WienerPass::Partials) {
    for (int64_t i = 0; i < len; ++i) {
      if constexpr (active)
        stash[i] = wiener_adjoint(a, len == 1 ? 0 : i);
      else
        stash[i] = 0.0;
    }
    stash += len;
  } else if constexpr (active) {
    if (ctx.in_adj[k].data)
      for (int64_t i = 0; i < len; ++i)
        ctx.in_adj[k].data[i] += wiener_adjoint(a, len == 1 ? 0 : i);
  }
}
template <bool Scalar, unsigned Mask>
double wiener_eval(KernelCtx& ctx, WienerPass pass) {
  stan::math::nested_rev_autodiff nested;
  using stan::math::var;
  const int64_t n = Scalar ? 1 : wiener_extent(ctx);
  const WienerArg<Scalar, (Mask & 1u) != 0> y =
      wiener_load<Scalar, (Mask & 1u) != 0>(ctx, 0, n);
  const WienerArg<Scalar, (Mask & 2u) != 0> alpha =
      wiener_load<Scalar, (Mask & 2u) != 0>(ctx, 1, n);
  const WienerArg<Scalar, (Mask & 4u) != 0> tau =
      wiener_load<Scalar, (Mask & 4u) != 0>(ctx, 2, n);
  const WienerArg<Scalar, (Mask & 8u) != 0> beta =
      wiener_load<Scalar, (Mask & 8u) != 0>(ctx, 3, n);
  const WienerArg<Scalar, (Mask & 16u) != 0> delta =
      wiener_load<Scalar, (Mask & 16u) != 0>(ctx, 4, n);
  var out = stan::math::wiener_lpdf<false>(y, alpha, tau, beta, delta);
  const double value = out.val();
  if (pass == WienerPass::Value) return value;
  if (pass == WienerPass::Seeded) {
    var seeded = out * ctx.out_adj;
    stan::math::grad(seeded.vi_);
  } else {
    stan::math::grad(out.vi_);
  }
  double* stash = ctx.scratch;
  wiener_collect(ctx, 0, y, pass, stash);
  wiener_collect(ctx, 1, alpha, pass, stash);
  wiener_collect(ctx, 2, tau, pass, stash);
  wiener_collect(ctx, 3, beta, pass, stash);
  wiener_collect(ctx, 4, delta, pass, stash);
  return value;
}
template <bool Scalar, unsigned... Mask>
double wiener_select(KernelCtx& ctx, unsigned active, WienerPass pass,
                     std::integer_sequence<unsigned, Mask...>) {
  using Eval = double (*)(KernelCtx&, WienerPass);
  static constexpr Eval table[] = {&wiener_eval<Scalar, Mask>...};
  return table[active](ctx, pass);
}
// Stan Math checks every argument before it returns 0 for an empty one. When
// the others have length 1, wiener_load spreads them to length 0 and Stan
// Math sees no value to check, so check them here in its order.
void wiener_check_beside_empty(const KernelCtx& ctx) {
  bool empty = false;
  for (int k = 0; k < 5; ++k) {
    if (ctx.in[k].len > 1) return;
    empty = empty || ctx.in[k].len == 0;
  }
  if (!empty) return;
  constexpr const char* fn = "wiener_lpdf";
  const auto value = [&](int k, auto check) {
    if (ctx.in[k].len == 1) check(ctx.in[k].data[0]);
  };
  namespace sm = stan::math;
  value(0, [](double y) { sm::check_positive(fn, "Random variable", y); });
  value(1, [](double a) {
    sm::check_positive_finite(fn, "Boundary separation", a);
  });
  value(2,
        [](double t) { sm::check_positive_finite(fn, "Nondecision time", t); });
  value(3, [](double w) { sm::check_bounded(fn, "A-priori bias", w, 0, 1); });
  value(4, [](double v) { sm::check_finite(fn, "Drift rate", v); });
}
double wiener_run(KernelCtx& ctx, unsigned active, WienerPass pass) {
  wiener_check_beside_empty(ctx);
  const auto masks = std::make_integer_sequence<unsigned, 32>{};
  return wiener_extent(ctx) == 1
             ? wiener_select<true>(ctx, active, pass, masks)
             : wiener_select<false>(ctx, active, pass, masks);
}

#define STANLI_TAIL_KERNEL(name, fn, kind)                                    \
  void name##_fwd(KernelCtx& ctx) { ctx.out.data[0] = fn<false, kind>(ctx); } \
  void name##_bwd(KernelCtx& ctx) { fn<true, kind>(ctx); }
STANLI_TAIL_KERNEL(mgp, mgp_eval, kMultiGp)
STANLI_TAIL_KERNEL(mgpc, mgp_eval, kMultiGpChol)
STANLI_TAIL_KERNEL(mst, mst_eval, kMst)
STANLI_TAIL_KERNEL(mstc, mst_eval, kMstChol)
STANLI_TAIL_KERNEL(dirmult, mult_eval, kDirichletMultinomial)
#undef STANLI_TAIL_KERNEL
void ologistic_fwd(KernelCtx& ctx) {
  // Arrays of cutpoint vectors retain the general nested-tape path. For a
  // shared cutpoint vector, Stan Math's propagator can write its partials
  // straight to scratch; no autodiff tape or second density evaluation is
  // needed. Keep both operands active, as in ordered_eval, so propto and
  // validation semantics are unchanged.
  const VectorizedLayout layout = vectorized_layout(ctx.idata, ctx.n_idata, 1);
  if (layout.is_array(0) ||
      (density_mode(ctx).propto && (density_mode(ctx).active & 0x3u) == 0)) {
    ctx.out.data[0] = ordered_eval<false, kOrderedLogistic>(ctx);
    return;
  }
  if (dens::fused_density_active()) {
    dens::ordered_logistic_lpmf_fused(ctx, layout.n_outcomes);
    return;
  }
  if constexpr (!dens::kFusedOnly) {
    sink s = recorded_tail_sink<2>(ctx);
    sink_scope active(s);
    const std::vector<int> y(layout.outcomes,
                             layout.outcomes + layout.n_outcomes);
    const auto cuts = as_rvar(ctx.in[1]);
    const auto call = [&](const auto& location) {
      record_probability_call([&] {
        return density_mode(ctx).propto
                   ? stan::math::ordered_logistic_lpmf<true>(y, location, cuts)
                   : stan::math::ordered_logistic_lpmf<false>(y, location,
                                                              cuts);
      });
    };
    if (ctx.in[0].len == 1)
      call(rvar(ctx.in[0].data[0]));
    else
      call(as_rvar(ctx.in[0]));
    ctx.out.data[0] = s.value;
  }
}
void ologistic_bwd(KernelCtx& ctx) {
  if (vectorized_layout(ctx.idata, ctx.n_idata, 1).is_array(0)) {
    ordered_eval<true, kOrderedLogistic>(ctx);
    return;
  }
  recorded_tail_bwd<2>(ctx);
}
// For shared cutpoints retain the primitive CDF partials and log-difference
// denominators. The weighted reverse follows rev/fun/log_diff_exp.hpp before
// multiplying the CDF partial, preserving its division/rounding order.
bool oprobit_shared(const KernelCtx& ctx) {
  const VectorizedLayout layout = vectorized_layout(ctx.idata, ctx.n_idata, 1);
  return !layout.is_array(0) && ctx.in[0].len > 0 &&
         layout.width == ctx.in[1].len;
}
void oprobit_fwd(KernelCtx& ctx) {
  if (!oprobit_shared(ctx)) {
    ctx.out.data[0] = ordered_eval<false, kOrderedProbit>(ctx);
    return;
  }
  const int64_t n = ctx.in[0].len, width = ctx.in[1].len;
  const VectorizedLayout layout = vectorized_layout(ctx.idata, ctx.n_idata, 1);
  const std::vector<int> outcomes(layout.outcomes,
                                  layout.outcomes + layout.n_outcomes);
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
void wiener_fwd(KernelCtx& ctx) {
  // Under propto with every argument data, Stan Math keeps no term.
  const DensityMode mode = density_mode(ctx);
  if (mode.propto && mode.active == 0) {
    wiener_run(ctx, 0, WienerPass::Value);
    ctx.out.data[0] = 0;
    return;
  }
  ctx.out.data[0] = values_only(ctx) ? wiener_run(ctx, 0, WienerPass::Value)
                                     : wiener_run(ctx, wiener_activity(ctx),
                                                  WienerPass::Partials);
  // Stan Math's propto density of constants is zero. Evaluate it anyway so
  // invalid arguments still reject.
  if (mode.propto && (mode.active & 0x1fu) == 0) ctx.out.data[0] = 0.0;
}
void wiener_bwd(KernelCtx& ctx) {
  unsigned active = wiener_activity(ctx);
  for (int k = 0; k < 5; ++k)
    if (ctx.in_adj[k].data) active |= 1u << k;
  bool reusable = std::isfinite(ctx.out_adj) && active == wiener_activity(ctx);
  int64_t total = 0;
  for (int k = 0; k < 5; ++k) total += ctx.in[k].len;
  for (int64_t i = 0; reusable && i < total; ++i)
    reusable = std::isfinite(ctx.scratch[i]);
  if (!reusable) {
    wiener_run(ctx, active, WienerPass::Seeded);
    return;
  }
  const double* stash = ctx.scratch;
  for (int k = 0; k < 5; ++k) {
    if (ctx.in_adj[k].data)
      for (int64_t i = 0; i < ctx.in[k].len; ++i)
        ctx.in_adj[k].data[i] += ctx.out_adj * stash[i];
    stash += ctx.in[k].len;
  }
}
int64_t wiener_scratch(const Op& op, const Slot* slots) {
  int64_t total = 0;
  for (int k = 0; k < 5; ++k) total += slots[op.in[k]].len;
  return total;
}

// wiener(y | a, t0, w, v, sv[, sw, st0], precision): the five- and
// seven-parameter densities, and the four- and seven-parameter unnormalized
// log cdf and ccdf. Input 0 holds the real arguments argument-major,
// one entry per lane; input 1 is the derivative precision. idata: argument
// count, lane count, active-argument mask, propto, WienerKind. Scratch keeps
// the partials.
template <typename Y, typename P>
stan::math::var wiener_packed_call(WienerKind kind, bool propto, int nargs,
                                   const Y& y, const std::vector<P>& p,
                                   double precision) {
  namespace sm = stan::math;
  if (kind == WienerKind::Lcdf)
    return nargs == 5
               ? sm::wiener_lcdf_unnorm(y, p[0], p[1], p[2], p[3], precision)
               : sm::wiener_lcdf_unnorm(y, p[0], p[1], p[2], p[3], p[4], p[5],
                                        p[6], precision);
  if (kind == WienerKind::Lccdf)
    return nargs == 5
               ? sm::wiener_lccdf_unnorm(y, p[0], p[1], p[2], p[3], precision)
               : sm::wiener_lccdf_unnorm(y, p[0], p[1], p[2], p[3], p[4], p[5],
                                         p[6], precision);
  if (nargs == 6)
    return propto ? sm::wiener_lpdf<true>(y, p[0], p[1], p[2], p[3], p[4],
                                          precision)
                  : sm::wiener_lpdf<false>(y, p[0], p[1], p[2], p[3], p[4],
                                           precision);
  return propto ? sm::wiener_lpdf<true>(y, p[0], p[1], p[2], p[3], p[4], p[5],
                                        p[6], precision)
                : sm::wiener_lpdf<false>(y, p[0], p[1], p[2], p[3], p[4], p[5],
                                         p[6], precision);
}
void wiener_packed_fwd(KernelCtx& ctx) {
  const int nargs = ctx.idata[0];
  const int64_t lanes = ctx.idata[1];
  const unsigned active = (unsigned)ctx.idata[2];
  const bool propto = ctx.idata[3] != 0;
  const WienerKind kind = static_cast<WienerKind>(ctx.idata[4]);
  const int64_t width = nargs * lanes;
  double* partials = values_only(ctx) ? nullptr : ctx.scratch;
  if (partials) std::fill_n(partials, width, 0.0);
  if (propto && active == 0) {
    ctx.out.data[0] = 0.0;
    return;
  }
  const double precision = ctx.in[1].data[0];
  const double* in = ctx.in[0].data;
  const bool y_active = (active & 1u) != 0;
  const bool y_var = y_active || kind != WienerKind::Lpdf;
  stan::math::nested_rev_autodiff nested;
  using stan::math::var;
  var out;
  if (lanes == 1) {
    std::vector<var> p;
    for (int k = 1; k < nargs; ++k) p.emplace_back(in[k]);
    var y = in[0];
    out = y_var ? wiener_packed_call(kind, propto, nargs, y, p, precision)
                : wiener_packed_call(kind, propto, nargs, in[0], p, precision);
    ctx.out.data[0] = out.val();
    if (!partials) return;
    stan::math::grad(out.vi_);
    if (y_active) partials[0] = y.adj();
    for (int k = 1; k < nargs; ++k) partials[k] = p[k - 1].adj();
    return;
  }
  std::vector<VarV> p(nargs - 1, VarV(lanes));
  for (int k = 1; k < nargs; ++k)
    for (int64_t i = 0; i < lanes; ++i) p[k - 1](i) = in[k * lanes + i];
  const Eigen::Map<const Eigen::VectorXd> y_data(in, lanes);
  VarV y = y_data.cast<var>();
  out = y_var ? wiener_packed_call(kind, propto, nargs, y, p, precision)
              : wiener_packed_call(kind, propto, nargs, Eigen::VectorXd(y_data),
                                   p, precision);
  ctx.out.data[0] = out.val();
  if (!partials) return;
  stan::math::grad(out.vi_);
  for (int64_t i = 0; i < lanes; ++i) {
    if (y_active) partials[i] = y(i).adj();
    for (int k = 1; k < nargs; ++k) partials[k * lanes + i] = p[k - 1](i).adj();
  }
}
void wiener_packed_bwd(KernelCtx& ctx) {
  if (!ctx.in_adj[0].data) return;
  for (int64_t i = 0; i < ctx.in[0].len; ++i)
    ctx.in_adj[0].data[i] += ctx.out_adj * ctx.scratch[i];
}
int64_t wiener_packed_scratch(const Op& op, const Slot* slots) {
  return slots[op.in[0]].len;
}

// ---- the last five ------------------------------------------------------
// lkj_cov(Sigma | mu, sigma, eta): a covariance matrix, two vectors of
// lognormal hyperparameters for the scales, and the LKJ shape.
template <bool Grad>
double lkjcov_eval(KernelCtx& ctx) {
  const int64_t K = ctx.idata[0];
  const bool propto = density_mode(ctx).propto;
  stan::math::nested_rev_autodiff nested;
  using stan::math::var;
  VarM S = tail_m(ctx, 0, K, K);
  VarV mu = tail_v(ctx, 1, ctx.in[1].len);
  VarV sig = tail_v(ctx, 2, ctx.in[2].len);
  var eta = ctx.in[3].data[0];
  const unsigned mask = density_mode(ctx).active & 0xfu;
  var out;
  if (!propto) {
    out = stan::math::lkj_cov_lpdf<false>(S, mu, sig, eta);
  } else {
    out = bind_active(mask & 1u, S, [&](const auto& a) {
      return bind_active(mask & 2u, mu, [&](const auto& b) {
        return bind_active(mask & 4u, sig, [&](const auto& c) {
          return bind_active(mask & 8u, eta, [&](const auto& d) {
            return stan::math::lkj_cov_lpdf<true>(a, b, c, d);
          });
        });
      });
    });
  }
  return finish_tail_density<Grad>(ctx, out, S, mu, sig, eta);
}

// Binomial logit GLM on the nested tape.
// idata is a GlmLayout over n then N.
template <typename F>
decltype(auto) with_glm_integer(std::vector<int>& values, bool scalar, F&& f) {
  return scalar ? std::forward<F>(f)(values[0]) : std::forward<F>(f)(values);
}
double blglm_eval(KernelCtx& ctx) {
  const GlmLayout layout = glm_layout(ctx.idata, ctx.n_idata);
  const int64_t rows = layout.rows, cols = layout.cols;
  const bool row_design = layout.row_vector_design();
  const int64_t outcomes = row_design ? layout.n_outcomes / 2 : rows;
  const auto with_design = [&](const auto& X, auto&& f) {
    if (row_design)
      f(X.row(0));
    else
      f(X);
  };
  const bool propto = density_mode(ctx).propto;
  std::vector<int> nn(ctx.idata, ctx.idata + outcomes);
  std::vector<int> NN(ctx.idata + outcomes, ctx.idata + 2 * outcomes);
  if (propto && (density_mode(ctx).active & 0x7u) == 0) {
    const CMapM Xd(ctx.in[0].data, rows, cols);
    const CMapV beta_d(ctx.in[2].data, cols);
    if (!values_only(ctx))
      std::fill_n(ctx.scratch, ctx.in[0].len + ctx.in[1].len + ctx.in[2].len,
                  0.0);
    double lp = 0;
    with_design(Xd, [&](const auto& x) {
      lp = with_glm_integer(
          nn, layout.scalar_outcome(0), [&](const auto& n_arg) {
            return with_glm_integer(
                NN, layout.scalar_outcome(1), [&](const auto& N_arg) {
                  if (ctx.in[1].len == 1)
                    return stan::math::binomial_logit_glm_lpmf<true>(
                        n_arg, N_arg, x, ctx.in[1].data[0], beta_d);
                  return stan::math::binomial_logit_glm_lpmf<true>(
                      n_arg, N_arg, x, CMapV(ctx.in[1].data, ctx.in[1].len),
                      beta_d);
                });
          });
    });
    return lp;
  }
  stan::math::nested_rev_autodiff nested;
  using stan::math::var;
  VarM X = tail_m(ctx, 0, rows, cols);
  var out;
  VarV beta = tail_v(ctx, 2, cols);
  const auto eval = [&](const auto& alpha) {
    with_design(X, [&](const auto& x) {
      with_glm_integer(nn, layout.scalar_outcome(0), [&](const auto& n_arg) {
        with_glm_integer(NN, layout.scalar_outcome(1), [&](const auto& N_arg) {
          out = propto ? stan::math::binomial_logit_glm_lpmf<true>(
                             n_arg, N_arg, x, alpha, beta)
                       : stan::math::binomial_logit_glm_lpmf<false>(
                             n_arg, N_arg, x, alpha, beta);
        });
      });
    });
    return tail_density_fwd(ctx, out, X, alpha, beta);
  };
  if (ctx.in[1].len == 1) return eval(var(ctx.in[1].data[0]));
  return eval(tail_v(ctx, 1, ctx.in[1].len));
}

void lkjcov_fwd(KernelCtx& ctx) { ctx.out.data[0] = lkjcov_eval<false>(ctx); }
void lkjcov_bwd(KernelCtx& ctx) { lkjcov_eval<true>(ctx); }
void blglm_fwd(KernelCtx& ctx) { ctx.out.data[0] = blglm_eval(ctx); }
// Stan Math's categorical_logit_glm_lpmf with every operand active, except
// for how the log density is summed. Stan Math adds sum(log normalizer),
// -sum(row max) and sum(selected logit) one after another. When the logits
// sit far from zero the last two cancel, and their rounding errors are left
// at the scale of the result. Here each row's log probability is formed first
// and the rows are added pairwise. The partials are Stan Math's
// expressions.
void clglm_rows_compensated(KernelCtx& ctx, sink& s, const std::vector<int>& y,
                            bool scalar_y, int64_t rows, int64_t cols) {
  using Eigen::Array;
  using Eigen::Dynamic;
  static constexpr const char* function = "categorical_logit_glm_lpmf";
  const int64_t classes = ctx.in[1].len;
  const CMapM x_val(ctx.in[0].data, rows, cols);
  const CMapV alpha_val(ctx.in[1].data, classes);
  const CMapM beta_val(ctx.in[2].data, cols, classes);
  Eigen::ArrayXi outcome(rows);
  if (scalar_y)
    outcome.setConstant(y[0]);
  else
    outcome = Eigen::Map<const Eigen::ArrayXi>(y.data(), rows);
  stan::math::check_bounded(function, "categorical outcome out of support",
                            outcome, 1, static_cast<int>(classes));
  Array<double, Dynamic, Dynamic> lin =
      (x_val * beta_val).rowwise() + alpha_val.transpose();
  Array<double, Dynamic, 1> lin_max = lin.rowwise().maxCoeff();
  Array<double, Dynamic, Dynamic> exp_lin = exp(lin.colwise() - lin_max);
  Array<double, Dynamic, 1> inv_sum_exp_lin = 1 / exp_lin.rowwise().sum();
  Array<double, Dynamic, 1> row_lp = log(inv_sum_exp_lin);
  for (int64_t i = 0; i < rows; ++i)
    row_lp(i) += lin(i, outcome[i] - 1) - lin_max(i);
  const double logp = pairwise_sum(row_lp.data(), rows);
  if (!std::isfinite(logp)) {
    stan::math::check_finite(function, "Weight vector", beta_val);
    stan::math::check_finite(function, "Intercept", alpha_val);
    stan::math::check_finite(function, "Matrix of independent variables",
                             x_val);
  }
  Eigen::Map<Eigen::MatrixXd> x_partials(s.buf[0], rows, cols);
  Array<double, Dynamic, Dynamic> beta_y(rows, cols);
  for (int64_t i = 0; i < rows; ++i)
    beta_y.row(i) = beta_val.col(outcome[i] - 1);
  x_partials =
      (beta_y - (exp_lin.matrix() * beta_val.transpose()).array().colwise() *
                    inv_sum_exp_lin)
          .matrix();
  Array<double, Dynamic, Dynamic> neg_softmax_lin =
      exp_lin.colwise() * -inv_sum_exp_lin;
  Eigen::Map<Eigen::VectorXd> alpha_partials(s.buf[1], classes);
  alpha_partials = neg_softmax_lin.colwise().sum().transpose().matrix();
  for (int64_t i = 0; i < rows; ++i) alpha_partials[outcome[i] - 1] += 1;
  Eigen::MatrixXd beta_derivative =
      x_val.transpose() * neg_softmax_lin.matrix();
  for (int64_t i = 0; i < rows; ++i)
    beta_derivative.col(outcome[i] - 1) += x_val.row(i).transpose();
  Eigen::Map<Eigen::MatrixXd>(s.buf[2], cols, classes) = beta_derivative;
  s.value = logp;
  s.deposited = true;
  *s.connected = 1.0;
}
void clglm_fwd(KernelCtx& ctx) {
  const GlmLayout layout = glm_layout(ctx.idata, ctx.n_idata);
  const int64_t rows = layout.rows, cols = layout.cols;
  const bool scalar_y = layout.scalar_outcome(0);
  std::vector<int> y(ctx.idata, ctx.idata + rows);
  sink s = recorded_tail_sink<3>(ctx);
  sink_scope active(s);
  if (density_mode(ctx).propto && (density_mode(ctx).active & 0x7u) == 0) {
    const CMapM Xd(ctx.in[0].data, rows, cols);
    const CMapV alpha_d(ctx.in[1].data, ctx.in[1].len);
    const CMapM beta_d(ctx.in[2].data, cols, ctx.in[2].len / cols);
    with_glm_integer(y, scalar_y, [&](const auto& outcome) {
      record_probability_call([&] {
        return stan::math::categorical_logit_glm_lpmf<true>(outcome, Xd,
                                                            alpha_d, beta_d);
      });
    });
    ctx.out.data[0] = s.value;
    return;
  }
  if (rows > kPlainSumMax && ctx.in[1].len > 1) {
    clglm_rows_compensated(ctx, s, y, scalar_y, rows, cols);
    ctx.out.data[0] = s.value;
    return;
  }
  const auto X = as_rvar_matrix(ctx.in[0], rows, cols);
  const auto alpha = as_rvar(ctx.in[1]);
  const auto beta = as_rvar_matrix(ctx.in[2], cols, ctx.in[2].len / cols);
  // Preserve the established all-active instantiation, including design
  // matrices supplied as data. Stan Math computes these partials directly;
  // the recorder deposits them without allocating or walking a var tape.
  with_glm_integer(y, scalar_y, [&](const auto& outcome) {
    record_probability_call([&] {
      return density_mode(ctx).propto
                 ? stan::math::categorical_logit_glm_lpmf<true>(outcome, X,
                                                                alpha, beta)
                 : stan::math::categorical_logit_glm_lpmf<false>(outcome, X,
                                                                 alpha, beta);
    });
  });
  ctx.out.data[0] = s.value;
}
void olglm_fwd(KernelCtx& ctx) {
  const GlmLayout layout = glm_layout(ctx.idata, ctx.n_idata);
  const int64_t rows = layout.rows, cols = layout.cols;
  const bool scalar_y = layout.scalar_outcome(0);
  std::vector<int> y(ctx.idata, ctx.idata + rows);
  sink s = recorded_tail_sink<3>(ctx);
  sink_scope active(s);
  if (density_mode(ctx).propto && (density_mode(ctx).active & 0x7u) == 0) {
    const CMapM Xd(ctx.in[0].data, rows, cols);
    const CMapV beta_d(ctx.in[1].data, ctx.in[1].len);
    const CMapV cuts_d(ctx.in[2].data, ctx.in[2].len);
    with_glm_integer(y, scalar_y, [&](const auto& outcome) {
      record_probability_call([&] {
        return stan::math::ordered_logistic_glm_lpmf<true>(outcome, Xd, beta_d,
                                                           cuts_d);
      });
    });
    ctx.out.data[0] = s.value;
    return;
  }
  const auto X = as_rvar_matrix(ctx.in[0], rows, cols);
  const auto beta = as_rvar(ctx.in[1]), cuts = as_rvar(ctx.in[2]);
  with_glm_integer(y, scalar_y, [&](const auto& outcome) {
    record_probability_call([&] {
      return density_mode(ctx).propto
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

#define STANLI_TAIL_CDF_KERNEL(code, fn, nreal)                              \
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
#define STANLI_TAIL_INT_CDF_KERNEL(code, fn, nreal)                          \
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

#ifdef STANLI_ACCELERATE
// ---- accelerated forms of the large dense kernels -------------------------
// accelerate_dense_ops (runtime/src/dense_blas.cpp) gives an op one of these
// opcodes only when its static sizes reach the threshold in
// blas::thresholds(), and never inside a retained loop (dyn_lengths), so
// the plain kernels above still run every smaller op. The results agree with
// the plain kernels to rounding, not bitwise.

// out = A * B; idata = {rows_a, cols_a, cols_b}.
void gemm_blas_fwd(KernelCtx& ctx) {
  const int64_t ra = ctx.idata[0], ca = ctx.idata[1], cb = ctx.idata[2];
  blas::gemm(false, false, ra, cb, ca, 1.0, ctx.in[0].data, ra, ctx.in[1].data,
             ca, 0.0, ctx.out.data, ra);
}
void gemm_blas_bwd(KernelCtx& ctx) {
  const int64_t ra = ctx.idata[0], ca = ctx.idata[1], cb = ctx.idata[2];
  const double* dO = ctx.out_adj_vec.data;
  if (ctx.in_adj[0].data)
    blas::gemm(false, true, ra, ca, cb, 1.0, dO, ra, ctx.in[1].data, ca, 1.0,
               ctx.in_adj[0].data, ra);
  if (ctx.in_adj[1].data)
    blas::gemm(true, false, ca, cb, ra, 1.0, ctx.in[0].data, ra, dO, ra, 1.0,
               ctx.in_adj[1].data, ca);
}

// out = X * beta with data X; idata = {rows, cols}.
void matvec_blas_fwd(KernelCtx& ctx) {
  const int64_t rows = ctx.idata[0], cols = ctx.idata[1];
  blas::gemv(false, rows, cols, 1.0, ctx.in[0].data, rows, ctx.in[1].data, 0.0,
             ctx.out.data);
}
void matvec_blas_bwd(KernelCtx& ctx) {
  if (!ctx.in_adj[1].data) return;
  const int64_t rows = ctx.idata[0], cols = ctx.idata[1];
  blas::gemv(true, rows, cols, 1.0, ctx.in[0].data, rows, ctx.out_adj_vec.data,
             1.0, ctx.in_adj[1].data);
}

// cholesky_decompose with LAPACK's dpotrf and the checks and messages of
// Stan Math's double overload.
void chol_blas_fwd(KernelCtx& ctx) {
  static constexpr const char* function = "cholesky_decompose";
  const int64_t n = ctx.idata[0];
  CMapM a(ctx.in[0].data, n, n);
  stan::math::check_symmetric(function, "m", a);
  stan::math::check_not_nan(function, "m", a);
  double* l = ctx.out.data;
  std::copy(a.data(), a.data() + n * n, l);
  const int info = blas::potrf_lower(n, l, n);
  bool positive = info == 0;
  for (int64_t j = 0; j < n; ++j) {
    positive = positive && l[j + j * n] > 0.0;
    for (int64_t i = 0; i < j; ++i) l[i + j * n] = 0.0;
  }
  if (!positive)
    stan::math::throw_domain_error(function, "Matrix",
                                   " is not positive definite", "m");
}

// The blocked reverse pass of Stan Math's cholesky_lambda (Iain Murray,
// "Differentiation of the Cholesky decomposition", 2016), step for step,
// with each block operation a BLAS call. Blocks are views into the n x n
// factor L and the working adjoint W, both column-major with leading
// dimension n; W starts as the lower triangle of the output adjoint.
void chol_blas_bwd(KernelCtx& ctx) {
  if (!ctx.in_adj[0].data) return;
  const int64_t n = ctx.idata[0];
  const double* L = ctx.out.data;
  double* W = ctx.scratch;
  for (int64_t c = 0; c < n; ++c)
    for (int64_t r = 0; r < n; ++r)
      W[r + c * n] = r >= c ? ctx.out_adj_vec.data[r + c * n] : 0.0;
  const auto at = [n](auto* base, int64_t r, int64_t c) {
    return base + r + c * n;
  };
  int64_t block = std::max<int64_t>(n / 8, 8);
  block = std::min<int64_t>(block, 128);
  for (int64_t k = n; k > 0; k -= block) {
    const int64_t j = std::max<int64_t>(0, k - block);
    const int64_t nb = k - j, mb = n - k;
    const double* R = at(L, j, 0);  // nb x j
    const double* D = at(L, j, j);  // nb x nb, lower
    const double* B = at(L, k, 0);  // mb x j
    const double* C = at(L, k, j);  // mb x nb
    double* R_adj = at(W, j, 0);
    double* D_adj = at(W, j, j);
    double* B_adj = at(W, k, 0);
    double* C_adj = at(W, k, j);
    if (mb > 0) {
      blas::trsm_lower(false, false, mb, nb, 1.0, D, n, C_adj, n);
      blas::gemm(false, false, mb, j, nb, -1.0, C_adj, n, R, n, 1.0, B_adj, n);
      blas::gemm(true, false, nb, nb, mb, -1.0, C_adj, n, C, n, 1.0, D_adj, n);
    }
    // D_adj = D' * tril(D_adj), then copy its lower triangle to the upper.
    for (int64_t c = 1; c < nb; ++c)
      for (int64_t r = 0; r < c; ++r) D_adj[r + c * n] = 0.0;
    blas::trmm_lower_left(true, nb, nb, 1.0, D, n, D_adj, n);
    for (int64_t c = 1; c < nb; ++c)
      for (int64_t r = 0; r < c; ++r) D_adj[r + c * n] = D_adj[c + r * n];
    blas::trsm_lower(true, true, nb, nb, 1.0, D, n, D_adj, n);
    blas::trsm_lower(false, false, nb, nb, 1.0, D, n, D_adj, n);
    blas::gemm(true, false, nb, j, mb, -1.0, C_adj, n, B, n, 1.0, R_adj, n);
    blas::symm_lower_left(nb, j, -1.0, D_adj, n, R, n, 1.0, R_adj, n);
    for (int64_t d = 0; d < nb; ++d) D_adj[d + d * n] *= 0.5;
  }
  double* adj = ctx.in_adj[0].data;
  for (int64_t c = 0; c < n; ++c)
    for (int64_t r = c; r < n; ++r) adj[r + c * n] += W[r + c * n];
}
int64_t chol_blas_scratch(const Op& op, const Slot*) {
  return op.n_idata > 0 ? int64_t(op.idata[0]) * op.idata[0] : 0;
}

// multi_normal_cholesky on mnc_legacy_fwd's path, with the inverse of L from
// LAPACK's dtrtri and the two triangular products from dtrmv.
double mnc_blas_fwd_value(KernelCtx& ctx) {
  const int64_t n = vectorized_layout(ctx.idata, ctx.n_idata, 2).width;
  CMapV y(ctx.in[0].data, n), mu(ctx.in[1].data, n);
  CMapM L(ctx.in[2].data, n, n);
  mnc_legacy_checks(y, mu, L);
  if (n == 0) return 0.0;

  VecD y_minus_mu = y - mu;
  MatD inv_L = MatD::Zero(n, n);
  inv_L.triangularView<Eigen::Lower>() = L.triangularView<Eigen::Lower>();
  // A zero on the diagonal: keep Stan Math's infinities and NaNs.
  if (blas::trtri_lower(n, inv_L.data(), n) != 0)
    inv_L = stan::math::mdivide_left_tri<Eigen::Lower>(L);
  VecD half = y_minus_mu;
  blas::trmv_lower(false, n, inv_L.data(), n, half.data());

  double logp(0.0);
  logp += stan::math::sum(stan::math::log(inv_L.diagonal()));
  if (!values_only(ctx)) {
    VecD scaled_diff = half;
    blas::trmv_lower(true, n, inv_L.data(), n, scaled_diff.data());
    MapM(ctx.scratch, n, n) =
        scaled_diff * half.transpose() - inv_L.transpose();
  }
  logp -= 0.5 * stan::math::sum(stan::math::dot_self(half));
  return logp;
}
void mnc_blas_fwd(KernelCtx& ctx) { ctx.out.data[0] = mnc_blas_fwd_value(ctx); }
void mnc_blas_bwd(KernelCtx& ctx) {
  if (!ctx.in_adj[2].data) return;
  const int64_t n = vectorized_layout(ctx.idata, ctx.n_idata, 2).width;
  for (int64_t i = 0; i < n * n; ++i)
    ctx.in_adj[2].data[i] += ctx.out_adj * ctx.scratch[i];
}
#endif

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
  register_kernel(
      OP_MULTINOMIAL_LOGIT_LPMF,
      Kernel{multinomial_logits_fwd<false>, multinomial_logits_bwd<false>,
             multinomial_logits_scratch});
  register_kernel(
      OP_MULTINOMIAL_SOFTMAX_LPMF,
      Kernel{multinomial_logits_fwd<true>, multinomial_logits_bwd<true>,
             multinomial_logits_scratch});
  register_kernel(OP_DIRICHLET_MULTINOMIAL_LPMF,
                  Kernel{dirmult_fwd, dirmult_bwd, nullptr});
  register_kernel(OP_ORDERED_LOGISTIC_LPMF, Kernel{ologistic_fwd, ologistic_bwd,
                                                   recorded_tail_scratch<2>});
  register_kernel(OP_ORDERED_PROBIT_LPMF,
                  Kernel{oprobit_fwd, oprobit_bwd, oprobit_scratch});
  register_kernel(OP_WIENER_LPDF,
                  Kernel{wiener_fwd, wiener_bwd, wiener_scratch});
  register_kernel(
      OP_WIENER_PACKED_LPDF,
      Kernel{wiener_packed_fwd, wiener_packed_bwd, wiener_packed_scratch});
#define STANLI_REGISTER_TAIL_CDF(code, fn, nreal) \
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
  register_solve_kernels();
  register_kernel(OP_EIGENVALUES_SYM,
                  Kernel{eigvals_fwd, eigvals_bwd, eigvals_scratch});
  register_kernel(OP_EIGENVECTORS_SYM,
                  Kernel{eigvecs_fwd, eigvecs_bwd, eigvecs_scratch});
  register_kernel(OP_TRANSPOSE, Kernel{transpose_fwd, transpose_bwd, nullptr});
  register_kernel(OP_LKJ_CORR_CHOL_LPDF, Kernel{lkj_fwd, lkj_bwd, nullptr});
  register_kernel(OP_LKJ_CORR_LPDF, Kernel{lkjc_fwd, lkjc_bwd, nullptr});
  Kernel normal_glm{nid_glm_fwd, nid_glm_bwd, nid_glm_scratch};
  normal_glm.derivative_mechanism = "recorded_partials";
  register_kernel(OP_NORMAL_ID_GLM_LPDF, normal_glm);
#ifdef STANLI_ACCELERATE
  register_kernel(OP_GEMM_BLAS, Kernel{gemm_blas_fwd, gemm_blas_bwd, nullptr});
  register_kernel(OP_MATVEC_BLAS,
                  Kernel{matvec_blas_fwd, matvec_blas_bwd, nullptr});
  register_kernel(OP_CHOLESKY_BLAS,
                  Kernel{chol_blas_fwd, chol_blas_bwd, chol_blas_scratch});
  register_kernel(OP_MULTI_NORMAL_CHOL_LPDF_BLAS,
                  Kernel{mnc_blas_fwd, mnc_blas_bwd, mnc_scratch});
#endif
}

#ifdef STANLI_ACCELERATE
namespace blas {
uint16_t accelerated_opcode(const Op& op, const Slot* slots) {
  if (op.dyn_lengths) return op.opcode;
  const Thresholds& t = thresholds();
  switch (op.opcode) {
    case OP_GEMM:
      if (op.n_idata >= 3 &&
          std::min({op.idata[0], op.idata[1], op.idata[2]}) >= t.gemm_min_dim)
        return OP_GEMM_BLAS;
      break;
    case OP_MATVEC:
      if (op.n_idata >= 2 && op.idata[0] >= t.gemv_min_rows &&
          op.idata[1] >= t.gemv_min_cols)
        return OP_MATVEC_BLAS;
      break;
    case OP_CHOLESKY:
      if (op.n_idata >= 1 && op.idata[0] >= t.cholesky_min_n)
        return OP_CHOLESKY_BLAS;
      break;
    case OP_MULTI_NORMAL_CHOL_LPDF:
      if (op.n_in == 3 && op.out >= 0 && op.in[0] >= 0 && op.in[1] >= 0 &&
          op.in[2] >= 0 &&
          mnc_path(op.variant, op.idata, op.n_idata, op.n_in,
                   slots[op.in[0]].len, slots[op.in[1]].len,
                   slots[op.in[2]].len, slots[op.out].len) == kMncLegacy &&
          vectorized_layout(op.idata, op.n_idata, 2).width >=
              t.tri_inverse_min_n)
        return OP_MULTI_NORMAL_CHOL_LPDF_BLAS;
      break;
    default:
      break;
  }
  return op.opcode;
}
}  // namespace blas
#endif

}  // namespace stanli
