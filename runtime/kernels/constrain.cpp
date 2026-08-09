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

// rev lb_constrain(matrix, scalar, lp):
//   exp_x = x.val().array().exp() (strided -> scalar std::exp);
//   ret = exp_x + lb;  lp += x.val().sum() (sequential);
//   bwd: x.adj += ret.adj * exp_x + lp.adj
void clower_fwd(KernelCtx& ctx) {
  const int64_t n = ctx.in[0].len;
  const double* x = ctx.in[0].data;
  double* exp_x = ctx.scratch;
  const double lb = ctx.in[1].data[0];
  if (packet_math() && n > 1) {
    MapA(exp_x, n) = CMapA(x, n).exp();
    MapA(ctx.out.data, n) = MapA(exp_x, n) + lb;
  } else {
    for (int64_t i = 0; i < n; ++i) {
      exp_x[i] = std::exp(x[i]);
      ctx.out.data[i] = exp_x[i] + lb;
    }
  }
  ctx.out2.data[0] = seq_sum(x, n);
}
void clower_bwd(KernelCtx& ctx) {
  const int64_t n = ctx.in[0].len;
  const double* exp_x = ctx.scratch;
  const double* dout = ctx.out_adj_vec.data;
  if (ctx.in_adj[0].data) {
    for (int64_t i = 0; i < n; ++i)
      ctx.in_adj[0].data[i] += dout[i] * exp_x[i] + ctx.out2_adj;
  }
  // Parameter-dependent bound: rev lb_constrain adds ret.adj().sum().
  if (ctx.in_adj[1].data) ctx.in_adj[1].data[0] += seq_sum(dout, n);
}

// rev ub_constrain(matrix, scalar, lp):
//   exp_x stored; ret = ub - exp_x; lp += x.sum();
//   bwd: x.adj += -ret.adj * exp_x + lp.adj
void cupper_fwd(KernelCtx& ctx) {
  const int64_t n = ctx.in[0].len;
  const double* x = ctx.in[0].data;
  double* exp_x = ctx.scratch;
  const double ub = ctx.in[1].data[0];
  if (packet_math() && n > 1) {
    MapA(exp_x, n) = CMapA(x, n).exp();
    MapA(ctx.out.data, n) = ub - MapA(exp_x, n);
  } else {
    for (int64_t i = 0; i < n; ++i) {
      exp_x[i] = std::exp(x[i]);
      ctx.out.data[i] = ub - exp_x[i];
    }
  }
  ctx.out2.data[0] = seq_sum(x, n);
}
void cupper_bwd(KernelCtx& ctx) {
  const int64_t n = ctx.in[0].len;
  const double* exp_x = ctx.scratch;
  const double* dout = ctx.out_adj_vec.data;
  if (ctx.in_adj[0].data) {
    for (int64_t i = 0; i < n; ++i)
      ctx.in_adj[0].data[i] += -dout[i] * exp_x[i] + ctx.out2_adj;
  }
  if (ctx.in_adj[1].data) ctx.in_adj[1].data[0] += seq_sum(dout, n);
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
  const double lb = ctx.in[1].data[0], ub = ctx.in[2].data[0];
  const double diff = ub - lb;
  const double log_diff = std::log(diff);
  double jac = 0.0;
  for (int64_t i = 0; i < n; ++i) {
    const double nax = -std::abs(x[i]);
    jac += log_diff + (nax - 2.0 * stan::math::log1p_exp(nax));
  }
  ctx.out2.data[0] = jac;
  if (n == 1) {
    // Scalar rev overload: sign-branching stan::math::inv_logit.
    il[0] = stan::math::inv_logit(x[0]);
  } else {
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
  for (int64_t i = 0; i < n; ++i) ctx.out.data[i] = diff * il[i] + lb;
}
void clu_bwd(KernelCtx& ctx) {
  const int64_t n = ctx.in[0].len;
  const double* il = ctx.scratch;
  const double* dout = ctx.out_adj_vec.data;
  const double lb = ctx.in[1].data[0], ub = ctx.in[2].data[0];
  const double diff = ub - lb;
  if (ctx.in_adj[0].data) {
    for (int64_t i = 0; i < n; ++i)
      ctx.in_adj[0].data[i] += dout[i] * diff * il[i] * (1.0 - il[i]) +
                               ctx.out2_adj * (1.0 - 2.0 * il[i]);
  }
  // rev lub_constrain bound adjoints (matrix-with-lp form):
  //   lb.adj += (ret.adj*(1-il)).sum() - (1/diff)*lp.adj*N
  //   ub.adj += (ret.adj*il).sum() + (1/diff)*lp.adj*N
  const double nd = static_cast<double>(n);
  const double one_over_diff = 1.0 / diff;
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
}

int64_t constrain_scratch(const Op& op, const Slot* slots) {
  return slots[op.in[0]].len;
}

// Structured transforms (simplex / ordered / positive_ordered): forward runs
// the prim double implementation; backward replays the actual REV constrain
// on a nested tape with output + jacobian adjoints seeded via the dot trick.
// Correct by construction against CmdStan's own code path.
// Batched for array-of-simplex etc.: idata = {n_batch, inner_con}; each
// batch element constrains independently, jacobians summed.
template <typename FwdF>
void structured_fwd(KernelCtx& ctx, FwdF&& f) {
  const int64_t nb = ctx.n_idata >= 2 ? ctx.idata[0] : 1;
  const int64_t inner_con = ctx.n_idata >= 2 ? ctx.idata[1] : ctx.out.len;
  const int64_t inner_raw = ctx.in[0].len / nb;
  double lp = 0.0;
  for (int64_t b = 0; b < nb; ++b) {
    Eigen::Map<const Eigen::VectorXd> y(ctx.in[0].data + b * inner_raw,
                                        inner_raw);
    Eigen::VectorXd x = f(y, lp);
    for (int64_t i = 0; i < inner_con; ++i)
      ctx.out.data[b * inner_con + i] = x(i);
  }
  ctx.out2.data[0] = lp;
}
template <typename RevF>
void structured_bwd(KernelCtx& ctx, RevF&& f) {
  if (ctx.in_adj[0].data == nullptr) return;
  const int64_t nb = ctx.n_idata >= 2 ? ctx.idata[0] : 1;
  const int64_t inner_con = ctx.n_idata >= 2 ? ctx.idata[1] : ctx.out.len;
  const int64_t inner_raw = ctx.in[0].len / nb;
  using stan::math::var;
  for (int64_t b = 0; b < nb; ++b) {
    stan::math::nested_rev_autodiff nested;
    Eigen::Matrix<var, -1, 1> y(inner_raw);
    for (int64_t i = 0; i < inner_raw; ++i)
      y(i) = ctx.in[0].data[b * inner_raw + i];
    var lp = 0.0;
    auto x = f(y, lp);
    Eigen::Map<const Eigen::VectorXd> seed(ctx.out_adj_vec.data + b * inner_con,
                                           inner_con);
    var j = stan::math::dot_product(seed, x) + ctx.out2_adj * lp;
    stan::math::grad(j.vi_);
    for (int64_t i = 0; i < inner_raw; ++i)
      ctx.in_adj[0].data[b * inner_raw + i] += y(i).adj();
  }
}

// cholesky_corr_constrain(y, K) returns a K x K lower-triangular factor;
// idata = {K}. Same nested-replay backward as the vector transforms, with
// the output flattened column-major to match slot layout.
void chol_corr_fwd(KernelCtx& ctx) {
  const int64_t K = ctx.idata[0];
  Eigen::Map<const Eigen::VectorXd> y(ctx.in[0].data, ctx.in[0].len);
  double lp = 0.0;
  Eigen::MatrixXd x = stan::math::cholesky_corr_constrain(y, (int)K, lp);
  for (int64_t j = 0; j < K; ++j)
    for (int64_t i = 0; i < K; ++i) ctx.out.data[j * K + i] = x(i, j);
  ctx.out2.data[0] = lp;
}
void chol_corr_bwd(KernelCtx& ctx) {
  if (ctx.in_adj[0].data == nullptr) return;
  const int64_t K = ctx.idata[0];
  using stan::math::var;
  stan::math::nested_rev_autodiff nested;
  Eigen::Matrix<var, -1, 1> y(ctx.in[0].len);
  for (int64_t i = 0; i < ctx.in[0].len; ++i) y(i) = ctx.in[0].data[i];
  var lp = 0.0;
  auto x = stan::math::cholesky_corr_constrain(y, (int)K, lp);
  Eigen::Matrix<var, -1, 1> flat(K * K);
  for (int64_t j = 0; j < K; ++j)
    for (int64_t i = 0; i < K; ++i) flat(j * K + i) = x(i, j);
  Eigen::Map<const Eigen::VectorXd> seed(ctx.out_adj_vec.data, K * K);
  var j2 = stan::math::dot_product(seed, flat) + ctx.out2_adj * lp;
  stan::math::grad(j2.vi_);
  for (int64_t i = 0; i < ctx.in[0].len; ++i)
    ctx.in_adj[0].data[i] += y(i).adj();
}

void simplex_fwd(KernelCtx& ctx) {
  structured_fwd(ctx, [](const auto& y, double& lp) {
    return stan::math::simplex_constrain(y, lp);
  });
}
void simplex_bwd(KernelCtx& ctx) {
  structured_bwd(ctx, [](auto& y, stan::math::var& lp) {
    return stan::math::simplex_constrain(y, lp);
  });
}
void ordered_fwd(KernelCtx& ctx) {
  structured_fwd(ctx, [](const auto& y, double& lp) {
    return stan::math::ordered_constrain(y, lp);
  });
}
void ordered_bwd(KernelCtx& ctx) {
  structured_bwd(ctx, [](auto& y, stan::math::var& lp) {
    return stan::math::ordered_constrain(y, lp);
  });
}
void pos_ordered_fwd(KernelCtx& ctx) {
  structured_fwd(ctx, [](const auto& y, double& lp) {
    return stan::math::positive_ordered_constrain(y, lp);
  });
}
void pos_ordered_bwd(KernelCtx& ctx) {
  structured_bwd(ctx, [](auto& y, stan::math::var& lp) {
    return stan::math::positive_ordered_constrain(y, lp);
  });
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
      for (int64_t i = 0; i < n; ++i) ctx.in_adj[2].data[i] += dout[i] * x[i];
      for (int64_t i = 0; i < n; ++i)
        ctx.in_adj[2].data[i] += ctx.out2_adj / sg[i];
    } else {
      double s = 0.0;
      for (int64_t i = 0; i < n; ++i) s += dout[i] * x[i];
      ctx.in_adj[2].data[0] += s + ctx.out2_adj * (double)n / sg[0];
    }
  }
}

// The remaining structured transforms. unit_vector and sum_to_zero are
// vector -> vector and go straight through the batched helpers; the three
// matrix-valued ones flatten column-major to match slot layout, like
// cholesky_corr above.
void unit_vector_fwd(KernelCtx& ctx) {
  structured_fwd(ctx, [](const auto& y, double& lp) {
    return stan::math::unit_vector_constrain(y, lp);
  });
}
void unit_vector_bwd(KernelCtx& ctx) {
  structured_bwd(ctx, [](auto& y, stan::math::var& lp) {
    return stan::math::unit_vector_constrain(y, lp);
  });
}

// sum_to_zero_constrain has no lp overload: the transform is
// volume-preserving, so its log-jacobian is exactly 0 and stan-math does
// not offer somewhere to add one. out2 stays 0.
void sum_to_zero_fwd(KernelCtx& ctx) {
  structured_fwd(ctx, [](const auto& y, double&) {
    return Eigen::VectorXd(stan::math::sum_to_zero_constrain(y));
  });
}
void sum_to_zero_bwd(KernelCtx& ctx) {
  structured_bwd(ctx, [](auto& y, stan::math::var&) {
    return stan::math::sum_to_zero_constrain(y);
  });
}

// Vector -> K x K (or M x N), flattened column-major. idata carries the
// dimensions the size alone cannot recover.
template <typename F>
void matrix_constrain_fwd(KernelCtx& ctx, int64_t rows, int64_t cols, F&& f) {
  Eigen::Map<const Eigen::VectorXd> y(ctx.in[0].data, ctx.in[0].len);
  double lp = 0.0;
  Eigen::MatrixXd x = f(y, lp);
  for (int64_t j = 0; j < cols; ++j)
    for (int64_t i = 0; i < rows; ++i) ctx.out.data[j * rows + i] = x(i, j);
  ctx.out2.data[0] = lp;
}
template <typename F>
void matrix_constrain_bwd(KernelCtx& ctx, int64_t rows, int64_t cols, F&& f) {
  if (ctx.in_adj[0].data == nullptr) return;
  using stan::math::var;
  stan::math::nested_rev_autodiff nested;
  Eigen::Matrix<var, -1, 1> y(ctx.in[0].len);
  for (int64_t i = 0; i < ctx.in[0].len; ++i) y(i) = ctx.in[0].data[i];
  var lp = 0.0;
  auto x = f(y, lp);
  Eigen::Matrix<var, -1, 1> flat(rows * cols);
  for (int64_t j = 0; j < cols; ++j)
    for (int64_t i = 0; i < rows; ++i) flat(j * rows + i) = x(i, j);
  Eigen::Map<const Eigen::VectorXd> seed(ctx.out_adj_vec.data, rows * cols);
  var j2 = stan::math::dot_product(seed, flat) + ctx.out2_adj * lp;
  stan::math::grad(j2.vi_);
  for (int64_t i = 0; i < ctx.in[0].len; ++i)
    ctx.in_adj[0].data[i] += y(i).adj();
}

void corr_matrix_fwd(KernelCtx& ctx) {
  const int64_t K = ctx.idata[0];
  matrix_constrain_fwd(ctx, K, K, [K](const auto& y, auto& lp) {
    return stan::math::corr_matrix_constrain(y, (Eigen::Index)K, lp);
  });
}
void corr_matrix_bwd(KernelCtx& ctx) {
  const int64_t K = ctx.idata[0];
  matrix_constrain_bwd(ctx, K, K, [K](auto& y, auto& lp) {
    return stan::math::corr_matrix_constrain(y, (Eigen::Index)K, lp);
  });
}
void cov_matrix_fwd(KernelCtx& ctx) {
  const int64_t K = ctx.idata[0];
  matrix_constrain_fwd(ctx, K, K, [K](const auto& y, auto& lp) {
    return stan::math::cov_matrix_constrain(y, (Eigen::Index)K, lp);
  });
}
void cov_matrix_bwd(KernelCtx& ctx) {
  const int64_t K = ctx.idata[0];
  matrix_constrain_bwd(ctx, K, K, [K](auto& y, auto& lp) {
    return stan::math::cov_matrix_constrain(y, (Eigen::Index)K, lp);
  });
}
void chol_cov_fwd(KernelCtx& ctx) {
  const int64_t M = ctx.idata[0], N = ctx.idata[1];
  matrix_constrain_fwd(ctx, M, N, [M, N](const auto& y, auto& lp) {
    return stan::math::cholesky_factor_constrain(y, (int)M, (int)N, lp);
  });
}
void chol_cov_bwd(KernelCtx& ctx) {
  const int64_t M = ctx.idata[0], N = ctx.idata[1];
  matrix_constrain_bwd(ctx, M, N, [M, N](auto& y, auto& lp) {
    return stan::math::cholesky_factor_constrain(y, (int)M, (int)N, lp);
  });
}

}  // namespace

void register_constrain_kernels() {
  register_kernel(OP_CONSTRAIN_OFFSET_MULT,
                  Kernel{offset_mult_fwd, offset_mult_bwd, nullptr});
  register_kernel(OP_CONSTRAIN_UNIT_VECTOR,
                  Kernel{unit_vector_fwd, unit_vector_bwd, nullptr});
  register_kernel(OP_CONSTRAIN_SUM_TO_ZERO,
                  Kernel{sum_to_zero_fwd, sum_to_zero_bwd, nullptr});
  register_kernel(OP_CONSTRAIN_CORR_MATRIX,
                  Kernel{corr_matrix_fwd, corr_matrix_bwd, nullptr});
  register_kernel(OP_CONSTRAIN_COV_MATRIX,
                  Kernel{cov_matrix_fwd, cov_matrix_bwd, nullptr});
  register_kernel(OP_CONSTRAIN_CHOL_COV,
                  Kernel{chol_cov_fwd, chol_cov_bwd, nullptr});
  register_kernel(OP_CONSTRAIN_LOWER,
                  Kernel{clower_fwd, clower_bwd, constrain_scratch});
  register_kernel(OP_CONSTRAIN_UPPER,
                  Kernel{cupper_fwd, cupper_bwd, constrain_scratch});
  register_kernel(OP_CONSTRAIN_LU, Kernel{clu_fwd, clu_bwd, constrain_scratch});
  register_kernel(OP_CONSTRAIN_CHOL_CORR,
                  Kernel{chol_corr_fwd, chol_corr_bwd, nullptr});
  register_kernel(OP_CONSTRAIN_SIMPLEX,
                  Kernel{simplex_fwd, simplex_bwd, nullptr});
  register_kernel(OP_CONSTRAIN_ORDERED,
                  Kernel{ordered_fwd, ordered_bwd, nullptr});
  register_kernel(OP_CONSTRAIN_POS_ORDERED,
                  Kernel{pos_ordered_fwd, pos_ordered_bwd, nullptr});
}

}  // namespace stanli
