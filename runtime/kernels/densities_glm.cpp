// The GLM kernels. They carry a data matrix and bind their arguments
// explicitly rather than through mask_dispatch; together they were the
// largest single block in densities_common.cpp.
//
// One of the density shards: see densities_impl.hpp for why they
// are split and what they share.
#include "densities_impl.hpp"

namespace stanli {
namespace dens {

// bernoulli_logit_glm(y | X, alpha, beta): X is a data matrix, mapped
// column-major like every other matrix slot.
// idata = [y..., rows, cols]. Edges are (x, alpha, beta); X is arg 0.
void bernoulli_logit_glm_fwd(KernelCtx& ctx) {
  const int64_t rows = ctx.idata[ctx.n_idata - 2];
  const int64_t cols = ctx.idata[ctx.n_idata - 1];
  Eigen::Map<const Eigen::VectorXi> y(ctx.idata, rows);
  Eigen::Map<const Eigen::MatrixXd> X(ctx.in[0].data, rows, cols);
  sink s = sink_for_args(ctx, 3);
  if (ctx.in[1].len != 1)
    throw std::runtime_error("glm: vector alpha unsupported");
  active_sink() = &s;
  // beta is a vector regardless of its length; alpha scalar. Honour the
  // propto bit: bernoulli has no constant to drop, so the two forms agree
  // here, but hardcoding one is what made poisson_log_glm's lp land
  // sum(log(y!)) away from CmdStan's.
  if (ctx.variant & 0x80u) {
    stan::math::bernoulli_logit_glm_lpmf<true>(
        y, X, rvar(ctx.in[1].data[0]), as_rvar(ctx.in[2]));
  } else {
    stan::math::bernoulli_logit_glm_lpmf<false>(
        y, X, rvar(ctx.in[1].data[0]), as_rvar(ctx.in[2]));
  }
  active_sink() = nullptr;
  ctx.out.data[0] = s.value;
}
// Edge order (x, alpha, beta): X data (edge 0 skipped by null adjoint),
// alpha scalar, beta vector.
void bernoulli_logit_glm_bwd(KernelCtx& ctx) { density_bwd<3>(ctx); }


// The other GLMs brms and rstanarm emit directly. A model that writes one
// of these does not merely run slower without it, it does not run. Alpha
// is the intercept: stan-math takes it as a scalar or a per-row vector,
// and only the scalar form is wired (the vector form needs a second
// length to plumb, and no corpus model asks for it).
void poisson_log_glm_fwd(KernelCtx& ctx) {
  const int64_t rows = ctx.idata[ctx.n_idata - 2];
  const int64_t cols = ctx.idata[ctx.n_idata - 1];
  Eigen::Map<const Eigen::VectorXi> y(ctx.idata, rows);
  Eigen::Map<const Eigen::MatrixXd> X(ctx.in[0].data, rows, cols);
  sink s = sink_for_args(ctx, 3);
  if (ctx.in[1].len != 1)
    throw std::runtime_error("poisson_log_glm: vector alpha unsupported");
  active_sink() = &s;
  // propto drops -lgamma(y+1), which is constant in the parameters but is
  // 10.45 on a six-observation test -- a constant offset, not noise.
  if (ctx.variant & 0x80u) {
    stan::math::poisson_log_glm_lpmf<true>(y, X, rvar(ctx.in[1].data[0]),
                                           as_rvar(ctx.in[2]));
  } else {
    stan::math::poisson_log_glm_lpmf<false>(y, X, rvar(ctx.in[1].data[0]),
                                            as_rvar(ctx.in[2]));
  }
  active_sink() = nullptr;
  ctx.out.data[0] = s.value;
}

void poisson_log_glm_bwd(KernelCtx& ctx) { density_bwd<3>(ctx); }

// Same, with the overdispersion argument on the end.
void neg_binomial_2_log_glm_fwd(KernelCtx& ctx) {
  const int64_t rows = ctx.idata[ctx.n_idata - 2];
  const int64_t cols = ctx.idata[ctx.n_idata - 1];
  Eigen::Map<const Eigen::VectorXi> y(ctx.idata, rows);
  Eigen::Map<const Eigen::MatrixXd> X(ctx.in[0].data, rows, cols);
  sink s = sink_for_args(ctx, 4);
  if (ctx.in[1].len != 1)
    throw std::runtime_error(
        "neg_binomial_2_log_glm: vector alpha unsupported");
  active_sink() = &s;
  const bool propto = (ctx.variant & 0x80u) != 0;
  if (ctx.in[3].len == 1) {
    const rvar phi(ctx.in[3].data[0]);
    if (propto) {
      stan::math::neg_binomial_2_log_glm_lpmf<true>(
          y, X, rvar(ctx.in[1].data[0]), as_rvar(ctx.in[2]), phi);
    } else {
      stan::math::neg_binomial_2_log_glm_lpmf<false>(
          y, X, rvar(ctx.in[1].data[0]), as_rvar(ctx.in[2]), phi);
    }
  } else if (propto) {
    stan::math::neg_binomial_2_log_glm_lpmf<true>(
        y, X, rvar(ctx.in[1].data[0]), as_rvar(ctx.in[2]), as_rvar(ctx.in[3]));
  } else {
    stan::math::neg_binomial_2_log_glm_lpmf<false>(
        y, X, rvar(ctx.in[1].data[0]), as_rvar(ctx.in[2]), as_rvar(ctx.in[3]));
  }
  active_sink() = nullptr;
  ctx.out.data[0] = s.value;
}

void neg_binomial_2_log_glm_bwd(KernelCtx& ctx) { density_bwd<4>(ctx); }

}  // namespace dens
}  // namespace stanli
