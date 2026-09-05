// The GLM kernels. They carry a data matrix and bind their arguments
// explicitly rather than through mask_dispatch; together they were the
// largest single block in densities_common.cpp.
//
// One of the density shards: see densities_impl.hpp for why they
// are split and what they share.
#include "densities_impl.hpp"
#include <stanli/density_registry.hpp>

namespace stanli {
namespace dens {

// bernoulli_logit_glm(y | X, alpha, beta): X is a data matrix, mapped
// column-major like every other matrix slot.
// idata = [y..., rows, cols]. Edges are (x, alpha, beta); X is arg 0.
void bernoulli_logit_glm_fwd(KernelCtx& ctx) {
  const bool scalar_layout =
      ctx.n_idata >= 4 && ctx.idata[ctx.n_idata - 2] == kGlmScalarLayoutMarker;
  const int tail = scalar_layout ? 4 : 2;
  const int64_t rows = ctx.idata[ctx.n_idata - tail];
  const int64_t cols = ctx.idata[ctx.n_idata - tail + 1];
  const bool scalar_y = scalar_layout && (ctx.idata[ctx.n_idata - 1] & 1);
  Eigen::Map<const Eigen::VectorXi> y(ctx.idata, rows);
  Eigen::Map<const Eigen::MatrixXd> X(ctx.in[0].data, rows, cols);
  sink s = sink_for_args(ctx, 3);
  sink_scope active(s);
  // beta is a vector regardless of its length; alpha scalar. Honour the
  // propto bit: bernoulli has no constant to drop, so the two forms agree
  // here, but hardcoding one is what made poisson_log_glm's lp land
  // sum(log(y!)) away from CmdStan's.
  const auto with_y = [&](const auto& outcome, const auto& alpha) {
    if (ctx.variant & 0x80u)
      return stan::math::bernoulli_logit_glm_lpmf<true>(outcome, X, alpha,
                                                        as_rvar(ctx.in[2]));
    return stan::math::bernoulli_logit_glm_lpmf<false>(outcome, X, alpha,
                                                       as_rvar(ctx.in[2]));
  };
  const auto call = [&](const auto& alpha) {
    return scalar_y ? with_y(y(0), alpha) : with_y(y, alpha);
  };
  if (ctx.in[1].len == 1)
    record_probability_call([&] { return call(rvar(ctx.in[1].data[0])); });
  else
    record_probability_call([&] { return call(as_rvar(ctx.in[1])); });
  ctx.out.data[0] = s.value;
}
// Edge order (x, alpha, beta): X is data, and edge 0 is skipped by its null
// adjoint.
void bernoulli_logit_glm_bwd(KernelCtx& ctx) { density_bwd<3>(ctx); }

// The other GLMs brms and rstanarm emit directly. A model that writes one
// of these does not merely run slower without it, it does not run. Alpha
// is the intercept: stan-math takes it as a scalar or a per-row vector,
// and may be a scalar or a per-row vector. The input range already retains
// that distinction, so all three kernels share the same length dispatch.
void poisson_log_glm_fwd(KernelCtx& ctx) {
  const bool scalar_layout =
      ctx.n_idata >= 4 && ctx.idata[ctx.n_idata - 2] == kGlmScalarLayoutMarker;
  const int tail = scalar_layout ? 4 : 2;
  const int64_t rows = ctx.idata[ctx.n_idata - tail];
  const int64_t cols = ctx.idata[ctx.n_idata - tail + 1];
  const bool scalar_y = scalar_layout && (ctx.idata[ctx.n_idata - 1] & 1);
  Eigen::Map<const Eigen::VectorXi> y(ctx.idata, rows);
  Eigen::Map<const Eigen::MatrixXd> X(ctx.in[0].data, rows, cols);
  sink s = sink_for_args(ctx, 3);
  sink_scope active(s);
  // propto drops -lgamma(y+1), which is constant in the parameters but is
  // 10.45 on a six-observation test -- a constant offset, not noise.
  const auto with_y = [&](const auto& outcome, const auto& alpha) {
    if (ctx.variant & 0x80u)
      return stan::math::poisson_log_glm_lpmf<true>(outcome, X, alpha,
                                                    as_rvar(ctx.in[2]));
    return stan::math::poisson_log_glm_lpmf<false>(outcome, X, alpha,
                                                   as_rvar(ctx.in[2]));
  };
  const auto call = [&](const auto& alpha) {
    return scalar_y ? with_y(y(0), alpha) : with_y(y, alpha);
  };
  if (ctx.in[1].len == 1)
    record_probability_call([&] { return call(rvar(ctx.in[1].data[0])); });
  else
    record_probability_call([&] { return call(as_rvar(ctx.in[1])); });
  ctx.out.data[0] = s.value;
}

void poisson_log_glm_bwd(KernelCtx& ctx) { density_bwd<3>(ctx); }

// Same, with the overdispersion argument on the end.
void neg_binomial_2_log_glm_fwd(KernelCtx& ctx) {
  const bool scalar_layout =
      ctx.n_idata >= 4 && ctx.idata[ctx.n_idata - 2] == kGlmScalarLayoutMarker;
  const int tail = scalar_layout ? 4 : 2;
  const int64_t rows = ctx.idata[ctx.n_idata - tail];
  const int64_t cols = ctx.idata[ctx.n_idata - tail + 1];
  const bool scalar_y = scalar_layout && (ctx.idata[ctx.n_idata - 1] & 1);
  Eigen::Map<const Eigen::VectorXi> y(ctx.idata, rows);
  Eigen::Map<const Eigen::MatrixXd> X(ctx.in[0].data, rows, cols);
  sink s = sink_for_args(ctx, 4);
  sink_scope active(s);
  const bool propto = (ctx.variant & 0x80u) != 0;
  const auto with_y = [&](const auto& outcome, const auto& alpha) {
    if (ctx.in[3].len == 1) {
      const rvar phi(ctx.in[3].data[0]);
      if (propto)
        return stan::math::neg_binomial_2_log_glm_lpmf<true>(
            outcome, X, alpha, as_rvar(ctx.in[2]), phi);
      return stan::math::neg_binomial_2_log_glm_lpmf<false>(
          outcome, X, alpha, as_rvar(ctx.in[2]), phi);
    }
    if (propto)
      return stan::math::neg_binomial_2_log_glm_lpmf<true>(
          outcome, X, alpha, as_rvar(ctx.in[2]), as_rvar(ctx.in[3]));
    return stan::math::neg_binomial_2_log_glm_lpmf<false>(
        outcome, X, alpha, as_rvar(ctx.in[2]), as_rvar(ctx.in[3]));
  };
  const auto call = [&](const auto& alpha) {
    return scalar_y ? with_y(y(0), alpha) : with_y(y, alpha);
  };
  if (ctx.in[1].len == 1)
    record_probability_call([&] { return call(rvar(ctx.in[1].data[0])); });
  else
    record_probability_call([&] { return call(as_rvar(ctx.in[1])); });
  ctx.out.data[0] = s.value;
}

void neg_binomial_2_log_glm_bwd(KernelCtx& ctx) { density_bwd<4>(ctx); }

}  // namespace dens
}  // namespace stanli
