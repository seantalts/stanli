// Constraint transform kernels. These mirror the REV *_constrain overloads'
// arithmetic exactly. CmdStan's parameters are Eigen matrices of vars, and
// the rev matrix overloads compute their transcendentals over strided
// .val() expressions that Eigen cannot packet-vectorize, so the reference
// arithmetic is SCALAR libm per element (numext::exp == std::exp; logistic
// is Eigen's scalar functor e/(1+e) with an inf guard, no sign branch) and
// reductions are sequential. The scalar rev overloads differ: lub uses the
// sign-branching stan::math::inv_logit, so length-1 slots take that path.
// Measured: the previous packet-vectorized kernels deviated from CmdStan by
// up to ~5 ULP in gradients on vector-bounded models (dfold fixture).
// The stored intermediate (exp_x / inv_logit_x) lives in scratch and is
// reused by backward, exactly as the rev overloads reuse their arena copies.
//
// out = constrained values, out2 = summed log-jacobian term.
#include <stanli/graph.hpp>
#include <stanli/optable.hpp>
#include <stanli/packet.hpp>

#include <stan/math.hpp>

namespace stanli {
namespace {

using Arr = Eigen::Array<double, -1, 1>;
using MapA = Eigen::Map<Arr>;
using CMapA = Eigen::Map<const Arr>;

// Sequential sum, matching Eigen's redux over a non-vectorizable strided
// expression (which is how the rev matrix overloads reduce). Under packet
// math this becomes Eigen's own (tree) reduction, as the varmat overloads
// use: 4.3x faster per element and a different, equally valid summation
// order.
inline double seq_sum(const double* p, int64_t n) {
  if (packet_math()) return CMapA(p, n).sum();
  double s = 0.0;
  for (int64_t i = 0; i < n; ++i) s += p[i];
  return s;
}

// A bound argument is either one value shared by the whole container or one
// value per element -- the only two shapes stan-math's own overloads take,
// and the only two the callable `*_constrain(x, bounds)` functions can be
// given. Reading it through a 0/1 stride keeps one code path for both, and
// on the shared path every operation is the one the scalar-bound code did,
// in the same order, so the declaration transforms stay bitwise.
struct Bound {
  const double* p;
  int64_t stride;
  explicit Bound(const Desc& a) : p(a.data), stride(a.len > 1 ? 1 : 0) {}
  double operator[](int64_t i) const { return p[i * stride]; }
  bool varies() const { return stride != 0; }
};

// A bound's reverse lane. A per-element bound takes its own term; a shared
// one collects all N, which is what the rev overloads' `.sum()` over the
// same expression comes to.
inline void add_bound_adj(KernelCtx& ctx, int k, const double* terms,
                          int64_t n) {
  if (ctx.in_adj[k].data == nullptr) return;
  if (Bound(ctx.in[k]).varies())
    for (int64_t i = 0; i < n; ++i) ctx.in_adj[k].data[i] += terms[i];
  else
    ctx.in_adj[k].data[0] += seq_sum(terms, n);
}

// An infinite bound is not a bound: every *_constrain overload short-circuits
// it to the identity, so the element passes through untouched and contributes
// no jacobian term. stan-math spells that as a `select` over an `is_inf` mask;
// here it earns a branch of its own instead, because an infinite bound is rare
// and the finite path has to stay the exact arithmetic the ULP calibration in
// the header comment was measured against.
constexpr double kNegInf = -std::numeric_limits<double>::infinity();
constexpr double kPosInf = std::numeric_limits<double>::infinity();

inline bool has_inf(const Bound& b, int64_t n, double inf) {
  if (!b.varies()) return b[0] == inf;
  for (int64_t i = 0; i < n; ++i)
    if (b.p[i] == inf) return true;
  return false;
}

// Jacobian for a per-element bound with identity elements masked to a literal
// zero -- rev's `select(x, 0).sum()` -- reduced the way seq_sum reduces, so
// the elements that do transform still land where they did.
inline double masked_sum(const double* x, const double* bound, int64_t n,
                         double inf) {
  if (packet_math())
    return (CMapA(bound, n) != inf).select(CMapA(x, n), 0.0).sum();
  double s = 0.0;
  for (int64_t i = 0; i < n; ++i) s += bound[i] == inf ? 0.0 : x[i];
  return s;
}

// A bound adjoint lands per element when the bound varies. A shared bound
// first reduces all terms from zero and then adds that reduction once, which
// preserves stan-math's `.sum()` callback rounding even when the lane already
// has an adjoint from another use.
struct BoundAdj {
  double* p;
  bool varies;
  double lane = 0.0;
  bool touched = false;
  BoundAdj(KernelCtx& ctx, int k)
      : p(ctx.in_adj[k].data), varies(Bound(ctx.in[k]).varies()) {}
  void add(int64_t i, double v) {
    if (!p) return;
    touched = true;
    if (varies)
      p[i] += v;
    else
      lane += v;
  }
  void commit() {
    if (p && !varies && touched) p[0] += lane;
  }
};

// rev lb_constrain(matrix, scalar, lp):
//   exp_x = x.val().array().exp() (strided -> scalar std::exp);
//   ret = exp_x + lb;  lp += x.val().sum() (sequential);
//   bwd: x.adj += ret.adj * exp_x + lp.adj
// The matrix-bound overload differs only in reading lb per element, and in
// giving lb its own adjoint lane instead of the sum of all of them.
void clower_fwd(KernelCtx& ctx) {
  const int64_t n = ctx.in[0].len;
  const double* x = ctx.in[0].data;
  double* exp_x = ctx.scratch;
  const Bound lb(ctx.in[1]);
  if (has_inf(lb, n, kNegInf)) {
    if (!lb.varies()) {
      // One shared -inf: the container is the identity end to end.
      for (int64_t i = 0; i < n; ++i) ctx.out.data[i] = x[i];
      ctx.out2.data[0] = 0.0;
      return;
    }
    for (int64_t i = 0; i < n; ++i) {
      exp_x[i] = std::exp(x[i]);
      ctx.out.data[i] = lb[i] == kNegInf ? x[i] : exp_x[i] + lb[i];
    }
    ctx.out2.data[0] = masked_sum(x, lb.p, n, kNegInf);
    return;
  }
  if (packet_math() && n > 1) {
    MapA(exp_x, n) = CMapA(x, n).exp();
    if (lb.varies())
      MapA(ctx.out.data, n) = MapA(exp_x, n) + CMapA(lb.p, n);
    else
      MapA(ctx.out.data, n) = MapA(exp_x, n) + lb[0];
  } else {
    for (int64_t i = 0; i < n; ++i) {
      exp_x[i] = std::exp(x[i]);
      ctx.out.data[i] = exp_x[i] + lb[i];
    }
  }
  ctx.out2.data[0] = seq_sum(x, n);
}
void clower_bwd(KernelCtx& ctx) {
  const int64_t n = ctx.in[0].len;
  const double* exp_x = ctx.scratch;
  const double* dout = ctx.out_adj_vec.data;
  const Bound lb(ctx.in[1]);
  if (has_inf(lb, n, kNegInf)) {
    // Identity elements pass ret.adj straight through and take no jacobian
    // share; their bound, having stayed out of the value, collects nothing.
    if (ctx.in_adj[0].data) {
      if (!lb.varies())
        for (int64_t i = 0; i < n; ++i) ctx.in_adj[0].data[i] += dout[i];
      else
        for (int64_t i = 0; i < n; ++i)
          ctx.in_adj[0].data[i] +=
              lb[i] == kNegInf ? dout[i] : dout[i] * exp_x[i] + ctx.out2_adj;
    }
    if (lb.varies()) {
      BoundAdj lba(ctx, 1);
      for (int64_t i = 0; i < n; ++i)
        if (lb[i] != kNegInf) lba.add(i, dout[i]);
    }
    return;
  }
  if (ctx.in_adj[0].data) {
    for (int64_t i = 0; i < n; ++i)
      ctx.in_adj[0].data[i] += dout[i] * exp_x[i] + ctx.out2_adj;
  }
  // Parameter-dependent bound: rev lb_constrain adds ret.adj() straight
  // through, summed only where one bound serves the whole container.
  add_bound_adj(ctx, 1, dout, n);
}

// rev ub_constrain(matrix, scalar, lp):
//   exp_x stored; ret = ub - exp_x; lp += x.sum();
//   bwd: x.adj += -ret.adj * exp_x + lp.adj
void cupper_fwd(KernelCtx& ctx) {
  const int64_t n = ctx.in[0].len;
  const double* x = ctx.in[0].data;
  double* exp_x = ctx.scratch;
  const Bound ub(ctx.in[1]);
  if (has_inf(ub, n, kPosInf)) {
    if (!ub.varies()) {
      for (int64_t i = 0; i < n; ++i) ctx.out.data[i] = x[i];
      ctx.out2.data[0] = 0.0;
      return;
    }
    for (int64_t i = 0; i < n; ++i) {
      exp_x[i] = std::exp(x[i]);
      ctx.out.data[i] = ub[i] == kPosInf ? x[i] : ub[i] - exp_x[i];
    }
    ctx.out2.data[0] = masked_sum(x, ub.p, n, kPosInf);
    return;
  }
  if (packet_math() && n > 1) {
    MapA(exp_x, n) = CMapA(x, n).exp();
    if (ub.varies())
      MapA(ctx.out.data, n) = CMapA(ub.p, n) - MapA(exp_x, n);
    else
      MapA(ctx.out.data, n) = ub[0] - MapA(exp_x, n);
  } else {
    for (int64_t i = 0; i < n; ++i) {
      exp_x[i] = std::exp(x[i]);
      ctx.out.data[i] = ub[i] - exp_x[i];
    }
  }
  ctx.out2.data[0] = seq_sum(x, n);
}
void cupper_bwd(KernelCtx& ctx) {
  const int64_t n = ctx.in[0].len;
  const double* exp_x = ctx.scratch;
  const double* dout = ctx.out_adj_vec.data;
  const Bound ub(ctx.in[1]);
  if (has_inf(ub, n, kPosInf)) {
    if (ctx.in_adj[0].data) {
      if (!ub.varies())
        for (int64_t i = 0; i < n; ++i) ctx.in_adj[0].data[i] += dout[i];
      else
        for (int64_t i = 0; i < n; ++i)
          ctx.in_adj[0].data[i] +=
              ub[i] == kPosInf ? dout[i] : -dout[i] * exp_x[i] + ctx.out2_adj;
    }
    if (ub.varies()) {
      BoundAdj uba(ctx, 1);
      for (int64_t i = 0; i < n; ++i)
        if (ub[i] != kPosInf) uba.add(i, dout[i]);
    }
    return;
  }
  if (ctx.in_adj[0].data) {
    for (int64_t i = 0; i < n; ++i)
      ctx.in_adj[0].data[i] += -dout[i] * exp_x[i] + ctx.out2_adj;
  }
  add_bound_adj(ctx, 1, dout, n);
}

// The stored inv_logit, by whichever rev overload this slot's length selects.
inline void clu_inv_logit(const double* x, double* il, int64_t n) {
  if (n == 1) {
    // Scalar rev overload: sign-branching stan::math::inv_logit.
    il[0] = stan::math::inv_logit(x[0]);
    return;
  }
  // Matrix rev overload: Eigen's scalar logistic functor over a strided
  // .val() expression: e/(1+e) with an inf guard, no sign branch.
  if (packet_math()) {
    MapA(il, n) = CMapA(x, n).exp();
    MapA(il, n) =
        (MapA(il, n).isInf()).select(1.0, MapA(il, n) / (1.0 + MapA(il, n)));
  } else {
    for (int64_t i = 0; i < n; ++i) {
      const double e = std::exp(x[i]);
      il[i] = std::isinf(e) ? 1.0 : e / (1.0 + e);
    }
  }
}

// lub_constrain resolves each element by which of its bounds are infinite:
// both -> identity, lower only -> the upper-bound transform, upper only ->
// the lower-bound transform, neither -> the logit map. The jacobian follows,
// contributing nothing, x, x, and log(diff) + logistic respectively.
void clu_fwd_inf(KernelCtx& ctx, int64_t n, const double* x, double* il,
                 const Bound& lb, const Bound& ub) {
  double jac = 0.0;
  for (int64_t i = 0; i < n; ++i) {
    const bool li = lb[i] == kNegInf, ui = ub[i] == kPosInf;
    if (li && ui) continue;
    if (li || ui) {
      jac += x[i];
      continue;
    }
    const double nax = -std::abs(x[i]);
    jac += std::log(ub[i] - lb[i]) + (nax - 2.0 * stan::math::log1p_exp(nax));
  }
  ctx.out2.data[0] = jac;
  clu_inv_logit(x, il, n);
  for (int64_t i = 0; i < n; ++i) {
    const bool li = lb[i] == kNegInf, ui = ub[i] == kPosInf;
    ctx.out.data[i] = li && ui ? x[i]
                      : li     ? ub[i] - std::exp(x[i])
                      : ui     ? std::exp(x[i]) + lb[i]
                               : (ub[i] - lb[i]) * il[i] + lb[i];
  }
}

void clu_bwd_inf(KernelCtx& ctx, int64_t n, const double* x, const double* il,
                 const Bound& lb, const Bound& ub) {
  const double* dout = ctx.out_adj_vec.data;
  const double lp_adj = ctx.out2_adj;
  BoundAdj lba(ctx, 1), uba(ctx, 2);
  for (int64_t i = 0; i < n; ++i) {
    const bool li = lb[i] == kNegInf, ui = ub[i] == kPosInf;
    const double diff = ub[i] - lb[i];
    if (ctx.in_adj[0].data)
      ctx.in_adj[0].data[i] += li && ui ? dout[i]
                               : li     ? -dout[i] * std::exp(x[i]) + lp_adj
                               : ui ? dout[i] * std::exp(x[i]) + lp_adj
                                    : dout[i] * diff * il[i] * (1.0 - il[i]) +
                                          lp_adj * (1.0 - 2.0 * il[i]);
    if (!li)
      lba.add(i,
              ui ? dout[i] : dout[i] * (1.0 - il[i]) - (1.0 / diff) * lp_adj);
    if (!ui) uba.add(i, li ? dout[i] : dout[i] * il[i] + (1.0 / diff) * lp_adj);
  }
  lba.commit();
  uba.commit();
}

// rev lub_constrain(matrix, scalar, scalar, lp):
//   neg_abs_x = -x.abs();
//   lp += (log(diff) + (neg_abs_x - 2*log1p_exp(neg_abs_x))).sum();
//   inv_logit_x = inv_logit(x) stored; ret = diff*inv_logit_x + lb;
//   bwd: x.adj += ret.adj*diff*il*(1-il) + lp.adj*(1-2*il)
void clu_fwd(KernelCtx& ctx) {
  const int64_t n = ctx.in[0].len;
  const double* x = ctx.in[0].data;
  double* il = ctx.scratch;
  const Bound lb(ctx.in[1]), ub(ctx.in[2]);
  if (has_inf(lb, n, kNegInf) || has_inf(ub, n, kPosInf)) {
    clu_fwd_inf(ctx, n, x, il, lb, ub);
    return;
  }
  // Shared bounds make log(diff) loop-invariant, and std::log is opaque
  // enough that only hoisting it by hand keeps a parameter block at one
  // call rather than one per element.
  const bool varies = lb.varies() || ub.varies();
  const double diff0 = ub[0] - lb[0];
  const double log_diff0 = std::log(diff0);
  double jac = 0.0;
  for (int64_t i = 0; i < n; ++i) {
    const double nax = -std::abs(x[i]);
    const double log_diff = varies ? std::log(ub[i] - lb[i]) : log_diff0;
    jac += log_diff + (nax - 2.0 * stan::math::log1p_exp(nax));
  }
  ctx.out2.data[0] = jac;
  clu_inv_logit(x, il, n);
  for (int64_t i = 0; i < n; ++i)
    ctx.out.data[i] = (varies ? ub[i] - lb[i] : diff0) * il[i] + lb[i];
}
void clu_bwd(KernelCtx& ctx) {
  const int64_t n = ctx.in[0].len;
  const double* il = ctx.scratch;
  const double* dout = ctx.out_adj_vec.data;
  const Bound lb(ctx.in[1]), ub(ctx.in[2]);
  if (has_inf(lb, n, kNegInf) || has_inf(ub, n, kPosInf)) {
    clu_bwd_inf(ctx, n, ctx.in[0].data, il, lb, ub);
    return;
  }
  const bool varies = lb.varies() || ub.varies();
  const double diff0 = ub[0] - lb[0];
  if (ctx.in_adj[0].data) {
    for (int64_t i = 0; i < n; ++i)
      ctx.in_adj[0].data[i] +=
          dout[i] * (varies ? ub[i] - lb[i] : diff0) * il[i] * (1.0 - il[i]) +
          ctx.out2_adj * (1.0 - 2.0 * il[i]);
  }
  if (!varies) {
    // rev lub_constrain bound adjoints, both bounds scalar:
    //   lb.adj += (ret.adj*(1-il)).sum() - (1/diff)*lp.adj*N
    //   ub.adj += (ret.adj*il).sum() + (1/diff)*lp.adj*N
    // One diff for the whole container is what lets that overload fold N
    // identical jacobian halves into a single multiply, and the fold does
    // not round like N separate additions -- so this shape keeps exactly
    // the arithmetic it was calibrated against.
    const double nd = static_cast<double>(n);
    const double one_over_diff = 1.0 / diff0;
    if (ctx.in_adj[1].data) {
      double s = 0.0;
      for (int64_t i = 0; i < n; ++i) s += dout[i] * (1.0 - il[i]);
      ctx.in_adj[1].data[0] += s + -one_over_diff * ctx.out2_adj * nd;
    }
    if (ctx.in_adj[2].data) {
      double s = 0.0;
      for (int64_t i = 0; i < n; ++i) s += dout[i] * il[i];
      ctx.in_adj[2].data[0] += s + one_over_diff * ctx.out2_adj * nd;
    }
    return;
  }
  // A diff that varies denies that fold: the overloads that take at least
  // one matrix bound build the term per element,
  //   lb term = ret.adj*(1-il) + -(1/diff)*lp.adj
  //   ub term = ret.adj*il     +  (1/diff)*lp.adj
  // and a bound that is still shared reduces all N of them with .sum().
  for (int k = 1; k <= 2; ++k) {
    if (ctx.in_adj[k].data == nullptr) continue;
    const bool is_lb = k == 1;
    const Bound& b = is_lb ? lb : ub;
    double lane = 0.0;
    for (int64_t i = 0; i < n; ++i) {
      const double half = 1.0 / (ub[i] - lb[i]) * ctx.out2_adj;
      const double t =
          dout[i] * (is_lb ? 1.0 - il[i] : il[i]) + (is_lb ? -half : half);
      if (b.varies())
        ctx.in_adj[k].data[i] += t;
      else
        lane += t;
    }
    if (!b.varies()) ctx.in_adj[k].data[0] += lane;
  }
}

int64_t constrain_scratch(const Op& op, const Slot* slots) {
  return slots[op.in[0]].len;
}

// Every transform below has the same execution protocol: prim forward, then
// a nested REV replay seeded by constrained-value and jacobian adjoints,
// except simplex and the two ordered kinds, whose pullbacks are closed form.
// idata starts with the outer batch count and one leaf's raw width; keeping
// both explicit makes an empty outer batch a plain zero-iteration loop. The
// kind is compile-time data, so registration still installs direct function
// pointers and adds no dispatch to the execution path.
enum class StructuredKind {
  Simplex,
  Ordered,
  PositiveOrdered,
  CholeskyCorr,
  UnitVector,
  SumToZero,
  StochasticColumn,
  StochasticRow,
  CorrMatrix,
  CovMatrix,
  CholeskyCov
};

template <StructuredKind K, typename Y, typename Lp>
auto apply_structured(Y& y, Lp& lp, const KernelCtx& ctx) {
  if constexpr (K == StructuredKind::Simplex)
    return stan::math::simplex_constrain(y, lp);
  else if constexpr (K == StructuredKind::Ordered)
    return stan::math::ordered_constrain(y, lp);
  else if constexpr (K == StructuredKind::PositiveOrdered)
    return stan::math::positive_ordered_constrain(y, lp);
  else if constexpr (K == StructuredKind::CholeskyCorr)
    return stan::math::cholesky_corr_constrain(y, (int)ctx.idata[2], lp);
  else if constexpr (K == StructuredKind::UnitVector)
    return stan::math::unit_vector_constrain(y, lp);
  else if constexpr (K == StructuredKind::SumToZero)
    // Volume preserving: intentionally leave lp untouched.
    return stan::math::sum_to_zero_constrain(y);
  else if constexpr (K == StructuredKind::StochasticColumn)
    return stan::math::stochastic_column_constrain(y, lp);
  else if constexpr (K == StructuredKind::StochasticRow)
    return stan::math::stochastic_row_constrain(y, lp);
  else if constexpr (K == StructuredKind::CorrMatrix)
    return stan::math::corr_matrix_constrain(y, (Eigen::Index)ctx.idata[2], lp);
  else if constexpr (K == StructuredKind::CovMatrix)
    return stan::math::cov_matrix_constrain(y, (Eigen::Index)ctx.idata[2], lp);
  else
    return stan::math::cholesky_factor_constrain(y, (int)ctx.idata[2],
                                                 (int)ctx.idata[3], lp);
}

template <StructuredKind K>
constexpr bool kMatrixStructured =
    K == StructuredKind::CholeskyCorr || K == StructuredKind::CorrMatrix ||
    K == StructuredKind::CovMatrix || K == StructuredKind::CholeskyCov ||
    K == StructuredKind::StochasticColumn || K == StructuredKind::StochasticRow;

template <StructuredKind K>
constexpr bool kMatrixInput =
    K == StructuredKind::StochasticColumn || K == StructuredKind::StochasticRow;

template <StructuredKind K>
int64_t structured_output_width(const KernelCtx& ctx) {
  if constexpr (kMatrixStructured<K>)
    return ctx.idata[2] * ctx.idata[3];
  else
    return ctx.idata[2];
}

// The prim vector overload uses Eigen's packet tanh, whereas Matrix<var>
// calls scalar std::tanh. Follow the pinned cholesky_corr_constrain algorithm
// with scalar transcendentals so the forward values agree with the tape that
// structured_bwd replays. Value-only evaluation retains the prim overload.
Eigen::MatrixXd cholesky_corr_rev_values(
    const Eigen::Map<const Eigen::VectorXd>& y, int K, double& lp) {
  stan::math::check_size_match("cholesky_corr_constrain", "y.size()", y.size(),
                               "k_choose_2", (K * (K - 1)) / 2);
  Eigen::VectorXd z(y.size());
  double jacobian = 0.0;
  for (Eigen::Index i = 0; i < y.size(); ++i) {
    z(i) = std::tanh(y(i));
    jacobian += stan::math::log1m(stan::math::square(z(i)));
  }
  lp += jacobian;
  Eigen::MatrixXd x = Eigen::MatrixXd::Zero(K, K);
  if (K == 0) return x;
  x(0, 0) = 1.0;
  int k = 0;
  for (int i = 1; i < K; ++i) {
    x(i, 0) = z(k++);
    double sum_sqs = stan::math::square(x(i, 0));
    for (int j = 1; j < i; ++j) {
      lp += 0.5 * stan::math::log1m(sum_sqs);
      x(i, j) = z(k++) * std::sqrt(1.0 - sum_sqs);
      sum_sqs += stan::math::square(x(i, j));
    }
    x(i, i) = std::sqrt(1.0 - sum_sqs);
  }
  return x;
}

template <StructuredKind K>
void structured_fwd(KernelCtx& ctx) {
  const int64_t nb = ctx.idata[0], inner_raw = ctx.idata[1];
  const int64_t inner_con = structured_output_width<K>(ctx);
  double lp = 0.0;
  for (int64_t b = 0; b < nb; ++b) {
    if constexpr (kMatrixInput<K>) {
      const int64_t raw_rows =
          ctx.idata[2] - (K == StructuredKind::StochasticColumn ? 1 : 0);
      const int64_t raw_cols =
          ctx.idata[3] - (K == StructuredKind::StochasticRow ? 1 : 0);
      const Eigen::Map<const Eigen::MatrixXd> y(ctx.in[0].data + b * inner_raw,
                                                raw_rows, raw_cols);
      const auto x = apply_structured<K>(y, lp, ctx);
      for (int64_t i = 0; i < inner_con; ++i)
        ctx.out.data[b * inner_con + i] = x.data()[i];
    } else {
      const Eigen::Map<const Eigen::VectorXd> y(ctx.in[0].data + b * inner_raw,
                                                inner_raw);
      if constexpr (K == StructuredKind::CholeskyCorr) {
        if (!values_only()) {
          const auto x = cholesky_corr_rev_values(y, ctx.idata[2], lp);
          std::copy_n(x.data(), inner_con, ctx.out.data + b * inner_con);
          continue;
        }
      }
      const auto x = apply_structured<K>(y, lp, ctx);
      for (int64_t i = 0; i < inner_con; ++i)
        ctx.out.data[b * inner_con + i] = x.data()[i];
      if constexpr (K == StructuredKind::Ordered) {
        double* exp_x = ctx.scratch + b * inner_raw;
        for (int64_t n = 1; n < inner_raw; ++n) exp_x[n - 1] = std::exp(y(n));
      } else if constexpr (K == StructuredKind::PositiveOrdered) {
        double* exp_x = ctx.scratch + b * inner_raw;
        for (int64_t n = 0; n < inner_raw; ++n) exp_x[n] = std::exp(y(n));
      }
    }
  }
  ctx.out2.data[0] = lp;
}

template <StructuredKind K>
void simplex_bwd_batch(KernelCtx& ctx, int64_t b, int64_t inner_raw,
                       int64_t inner_con) {
  const double* res = ctx.out.data + b * inner_con;
  const double* seed = ctx.out_adj_vec.data + b * inner_con;
  double* y_adj = ctx.in_adj[0].data + b * inner_raw;
  double dot = 0.0;
  for (int64_t i = 0; i < inner_con; ++i) dot += seed[i] * res[i];
  const double c = dot + (double)inner_con * ctx.out2_adj;
  const auto z_adj = [&](int64_t i) {
    return -res[i] * c + (res[i] * seed[i] + ctx.out2_adj);
  };
  double sum_u_adj = 0.0;
  for (int64_t i = 0; i < inner_raw; ++i) {
    const double n = (double)(i + 1);
    sum_u_adj += z_adj(i);
    const double v_adj = -z_adj(i + 1) * n;
    y_adj[i] += (v_adj + sum_u_adj) / std::sqrt(n * (n + 1));
  }
}

template <StructuredKind K>
void ordered_bwd_batch(KernelCtx& ctx, int64_t b, int64_t inner_raw,
                       int64_t inner_con) {
  const double* exp_x = ctx.scratch + b * inner_raw;
  const double* seed = ctx.out_adj_vec.data + b * inner_con;
  double* x_adj = ctx.in_adj[0].data + b * inner_raw;
  double rolling = 0.0;
  if constexpr (K == StructuredKind::Ordered) {
    for (int64_t n = inner_raw - 1; n > 0; --n) {
      rolling += seed[n];
      x_adj[n] += exp_x[n - 1] * rolling + ctx.out2_adj;
    }
    if (inner_raw > 0) x_adj[0] += rolling + seed[0];
  } else {
    for (int64_t n = inner_raw - 1; n >= 0; --n) {
      rolling += seed[n];
      x_adj[n] += exp_x[n] * rolling + ctx.out2_adj;
    }
  }
}

template <StructuredKind K>
void structured_bwd(KernelCtx& ctx) {
  if (ctx.in_adj[0].data == nullptr) return;
  const int64_t nb = ctx.idata[0], inner_raw = ctx.idata[1];
  const int64_t inner_con = structured_output_width<K>(ctx);
  if constexpr (K == StructuredKind::Simplex) {
    for (int64_t b = 0; b < nb; ++b)
      simplex_bwd_batch<K>(ctx, b, inner_raw, inner_con);
    return;
  }
  if constexpr (K == StructuredKind::Ordered ||
                K == StructuredKind::PositiveOrdered) {
    for (int64_t b = 0; b < nb; ++b)
      ordered_bwd_batch<K>(ctx, b, inner_raw, inner_con);
    return;
  }
  using stan::math::var;
  for (int64_t b = 0; b < nb; ++b) {
    stan::math::nested_rev_autodiff nested;
    var lp = 0.0;
    const Eigen::Map<const Eigen::VectorXd> seed(
        ctx.out_adj_vec.data + b * inner_con, inner_con);
    if constexpr (kMatrixInput<K>) {
      const int64_t raw_rows =
          ctx.idata[2] - (K == StructuredKind::StochasticColumn ? 1 : 0);
      const int64_t raw_cols =
          ctx.idata[3] - (K == StructuredKind::StochasticRow ? 1 : 0);
      Eigen::Matrix<var, -1, -1> y(raw_rows, raw_cols);
      for (int64_t i = 0; i < inner_raw; ++i)
        y.data()[i] = ctx.in[0].data[b * inner_raw + i];
      const auto x = apply_structured<K>(y, lp, ctx);
      // Eigen storage is column-major here, matching the old explicit
      // j*rows+i flattening and therefore its dot-product construction order.
      Eigen::Matrix<var, -1, 1> flat(inner_con);
      for (int64_t i = 0; i < inner_con; ++i) flat(i) = x.data()[i];
      var objective = stan::math::dot_product(seed, flat) + ctx.out2_adj * lp;
      stan::math::grad(objective.vi_);
      for (int64_t i = 0; i < inner_raw; ++i)
        ctx.in_adj[0].data[b * inner_raw + i] += y.data()[i].adj();
    } else {
      Eigen::Matrix<var, -1, 1> y(inner_raw);
      for (int64_t i = 0; i < inner_raw; ++i)
        y(i) = ctx.in[0].data[b * inner_raw + i];
      const auto x = apply_structured<K>(y, lp, ctx);
      if constexpr (kMatrixStructured<K>) {
        Eigen::Matrix<var, -1, 1> flat(inner_con);
        for (int64_t i = 0; i < inner_con; ++i) flat(i) = x.data()[i];
        var objective = stan::math::dot_product(seed, flat) + ctx.out2_adj * lp;
        stan::math::grad(objective.vi_);
      } else {
        var objective = stan::math::dot_product(seed, x) + ctx.out2_adj * lp;
        stan::math::grad(objective.vi_);
      }
      for (int64_t i = 0; i < inner_raw; ++i)
        ctx.in_adj[0].data[b * inner_raw + i] += y(i).adj();
    }
  }
}

template <StructuredKind K>
void register_structured(uint16_t opcode) {
  constexpr bool kNeedsScratch =
      K == StructuredKind::Ordered || K == StructuredKind::PositiveOrdered;
  register_kernel(opcode, Kernel{structured_fwd<K>, structured_bwd<K>,
                                 kNeedsScratch ? constrain_scratch : nullptr});
}

// offset_multiplier_constrain(x, mu, sigma, lp):
//   ret = fma(sigma, x, mu)  -- std::fma, a real fused multiply-add, not
//   contraction, so -ffp-contract=off does not change it;
//   lp += multiply_log(size(x), sigma) when sigma is a SCALAR, else
//   sum(log(sigma)).
//
// Written natively rather than replayed because this is the modern
// non-centering idiom (`vector<offset=mu, multiplier=tau>[J] theta`), so
// mu and sigma are usually parameters themselves and it sits on the hot
// path. Each of mu and sigma is length 1 (broadcast) or length n.
void offset_mult_fwd(KernelCtx& ctx) {
  const int64_t n = ctx.in[0].len;
  const double* x = ctx.in[0].data;
  const double* mu = ctx.in[1].data;
  const double* sg = ctx.in[2].data;
  const bool mu_v = ctx.in[1].len > 1, sg_v = ctx.in[2].len > 1;
  for (int64_t i = 0; i < n; ++i)
    ctx.out.data[i] = std::fma(sg[sg_v ? i : 0], x[i], mu[mu_v ? i : 0]);
  if (sg_v) {
    double s = 0.0;
    for (int64_t i = 0; i < n; ++i) s += std::log(sg[i]);
    ctx.out2.data[0] = s;
  } else {
    // multiply_log(N, sigma) = N * log(sigma), one call rather than N
    // additions -- which is what stan-math does, and the difference is
    // visible in the last bits.
    ctx.out2.data[0] = (double)n * std::log(sg[0]);
  }
}
void offset_mult_bwd(KernelCtx& ctx) {
  const int64_t n = ctx.in[0].len;
  const double* x = ctx.in[0].data;
  const double* sg = ctx.in[2].data;
  const double* dout = ctx.out_adj_vec.data;
  const bool mu_v = ctx.in[1].len > 1, sg_v = ctx.in[2].len > 1;
  if (ctx.in_adj[0].data)
    for (int64_t i = 0; i < n; ++i)
      ctx.in_adj[0].data[i] += dout[i] * sg[sg_v ? i : 0];
  if (ctx.in_adj[1].data) {
    if (mu_v)
      for (int64_t i = 0; i < n; ++i) ctx.in_adj[1].data[i] += dout[i];
    else
      ctx.in_adj[1].data[0] += seq_sum(dout, n);
  }
  if (ctx.in_adj[2].data) {
    if (sg_v) {
      // TWO accumulations, not one sum of two terms. stan-math builds the
      // lp term (sum(log(sigma))) before the value (fma), so the reverse
      // sweep contracts fma's contribution into sigma.adj first and the
      // jacobian's second -- and a += b += c does not round like a += (b + c).
      for (int64_t i = 0; i < n; ++i) {
        ctx.in_adj[2].data[i] += dout[i] * x[i];
        ctx.in_adj[2].data[i] += ctx.out2_adj / sg[i];
      }
    } else {
      double s = 0.0;
      for (int64_t i = 0; i < n; ++i) s += dout[i] * x[i];
      ctx.in_adj[2].data[0] += s + ctx.out2_adj * (double)n / sg[0];
    }
  }
}

// sum_to_zero_matrix[N, M] is the two-axis form: (N-1)*(M-1) unconstrained
// in, N x M out with every row AND every column summing to zero. Not the
// vector transform applied per row, and not the same free size. Also
// volume-preserving, so out2 stays 0. idata contains batch, raw width, rows,
// and columns; each element is flattened column-major like the other matrix
// transforms.
void sum_to_zero_mat_fwd(KernelCtx& ctx) {
  const int64_t nb = ctx.idata[0], raw = ctx.idata[1];
  const int64_t R = ctx.idata[2], C = ctx.idata[3], con = R * C;
  for (int64_t b = 0; b < nb; ++b) {
    Eigen::MatrixXd y(R - 1, C - 1);
    for (int64_t j = 0; j < C - 1; ++j)
      for (int64_t i = 0; i < R - 1; ++i)
        y(i, j) = ctx.in[0].data[b * raw + j * (R - 1) + i];
    const Eigen::MatrixXd x = stan::math::sum_to_zero_constrain(y);
    for (int64_t j = 0; j < C; ++j)
      for (int64_t i = 0; i < R; ++i)
        ctx.out.data[b * con + j * R + i] = x(i, j);
  }
  ctx.out2.data[0] = 0.0;
}
void sum_to_zero_mat_bwd(KernelCtx& ctx) {
  if (ctx.in_adj[0].data == nullptr || ctx.idata[1] == 0) return;
  const int64_t nb = ctx.idata[0], raw = ctx.idata[1];
  const int64_t R = ctx.idata[2], C = ctx.idata[3], con = R * C;
  using stan::math::var;
  for (int64_t b = 0; b < nb; ++b) {
    stan::math::nested_rev_autodiff nested;
    Eigen::Matrix<var, -1, -1> y(R - 1, C - 1);
    for (int64_t j = 0; j < C - 1; ++j)
      for (int64_t i = 0; i < R - 1; ++i)
        y(i, j) = ctx.in[0].data[b * raw + j * (R - 1) + i];
    auto x = stan::math::sum_to_zero_constrain(y);
    var seeded = 0.0;
    for (int64_t j = 0; j < C; ++j)
      for (int64_t i = 0; i < R; ++i)
        seeded += ctx.out_adj_vec.data[b * con + j * R + i] * x(i, j);
    stan::math::grad(seeded.vi_);
    for (int64_t j = 0; j < C - 1; ++j)
      for (int64_t i = 0; i < R - 1; ++i)
        ctx.in_adj[0].data[b * raw + j * (R - 1) + i] += y(i, j).adj();
  }
}

}  // namespace

void register_constrain_kernels() {
  register_kernel(OP_CONSTRAIN_OFFSET_MULT,
                  Kernel{offset_mult_fwd, offset_mult_bwd, nullptr});
  register_structured<StructuredKind::UnitVector>(OP_CONSTRAIN_UNIT_VECTOR);
  register_structured<StructuredKind::SumToZero>(OP_CONSTRAIN_SUM_TO_ZERO);
  register_kernel(OP_CONSTRAIN_SUM_TO_ZERO_MAT,
                  Kernel{sum_to_zero_mat_fwd, sum_to_zero_mat_bwd, nullptr});
  register_structured<StructuredKind::CorrMatrix>(OP_CONSTRAIN_CORR_MATRIX);
  register_structured<StructuredKind::CovMatrix>(OP_CONSTRAIN_COV_MATRIX);
  register_structured<StructuredKind::CholeskyCov>(OP_CONSTRAIN_CHOL_COV);
  register_kernel(OP_CONSTRAIN_LOWER,
                  Kernel{clower_fwd, clower_bwd, constrain_scratch});
  register_kernel(OP_CONSTRAIN_UPPER,
                  Kernel{cupper_fwd, cupper_bwd, constrain_scratch});
  register_kernel(OP_CONSTRAIN_LU, Kernel{clu_fwd, clu_bwd, constrain_scratch});
  register_structured<StructuredKind::CholeskyCorr>(OP_CONSTRAIN_CHOL_CORR);
  register_structured<StructuredKind::Simplex>(OP_CONSTRAIN_SIMPLEX);
  register_structured<StructuredKind::Ordered>(OP_CONSTRAIN_ORDERED);
  register_structured<StructuredKind::PositiveOrdered>(
      OP_CONSTRAIN_POS_ORDERED);
  register_structured<StructuredKind::StochasticColumn>(
      OP_CONSTRAIN_STOCHASTIC_COLUMN);
  register_structured<StructuredKind::StochasticRow>(
      OP_CONSTRAIN_STOCHASTIC_ROW);
}

}  // namespace stanli
