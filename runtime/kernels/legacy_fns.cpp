// Legacy ops: kernels of exactly this shape wrap any stan-math signature
// that has no native port yet.
#include <stanli/graph.hpp>
#include <stanli/legacy.hpp>
#include <stanli/optable.hpp>
#include <stanli/packet.hpp>

namespace stanli {
namespace {

void softmax_fwd(KernelCtx& ctx) {
  Eigen::Map<const Eigen::VectorXd> x(ctx.in[0].data, ctx.in[0].len);
  Eigen::Map<Eigen::VectorXd> out(ctx.out.data, ctx.out.len);
  out = stan::math::softmax(x);
}
void softmax_bwd(KernelCtx& ctx) {
  legacy_bwd_vec_in(ctx, [](const auto& x) { return stan::math::softmax(x); });
}

// Multivariate density via nested replay: dirichlet_lpdf(theta | alpha).
// The recorder's vector edges do not model partials_vec_ (sequence-of-vector
// partials), so this is a legacy op by design.
//
// Propto term-dropping in stan-math is decided by the ARGUMENT TYPES, so a
// legacy propto op must bind each argument var-or-double per the activity
// mask, exactly like the native kernels do; promoting an inactive argument
// to var silently keeps terms CmdStan drops.
double dirichlet_eval(KernelCtx& ctx) {
  stan::math::nested_rev_autodiff nested;
  using stan::math::var;
  const bool propto = (ctx.variant & 0x80u) != 0;
  const unsigned mask = ctx.variant == 0 ? 0x3u : (ctx.variant & 0x3fu);
  Eigen::Matrix<var, -1, 1> theta(ctx.in[0].len), alpha(ctx.in[1].len);
  for (int64_t i = 0; i < ctx.in[0].len; ++i) theta(i) = ctx.in[0].data[i];
  for (int64_t i = 0; i < ctx.in[1].len; ++i) alpha(i) = ctx.in[1].data[i];
  Eigen::Map<const Eigen::VectorXd> theta_d(ctx.in[0].data, ctx.in[0].len);
  Eigen::Map<const Eigen::VectorXd> alpha_d(ctx.in[1].data, ctx.in[1].len);
  var lp;
  const bool a0 = (mask & 1u) != 0, a1 = (mask & 2u) != 0;

  // `p ~ dirichlet(a)` over an array of simplexes. A single dirichlet needs
  // theta and alpha the same length, so a longer theta is unambiguously the
  // vectorized form: reps simplexes of K, element n contiguous in K.
  // stan-math reads both through vector_seq_view, so the only work here is
  // splitting the slot.
  const int64_t K = ctx.in[1].len;
  const int64_t reps = K > 0 ? ctx.in[0].len / K : 1;
  if (reps > 1) {
    std::vector<Eigen::Matrix<var, -1, 1>> th(reps,
                                              Eigen::Matrix<var, -1, 1>(K));
    std::vector<Eigen::VectorXd> thd(reps, Eigen::VectorXd(K));
    for (int64_t r = 0; r < reps; ++r)
      for (int64_t i = 0; i < K; ++i) {
        th[r](i) = ctx.in[0].data[r * K + i];
        thd[r](i) = ctx.in[0].data[r * K + i];
      }
    var lpv;
    if (propto) {
      if (a0 && a1)
        lpv = stan::math::dirichlet_lpdf<true>(th, alpha);
      else if (a0)
        lpv = stan::math::dirichlet_lpdf<true>(th, alpha_d);
      else if (a1)
        lpv = stan::math::dirichlet_lpdf<true>(thd, alpha);
      else
        lpv = 0.0;
    } else {
      if (a0 && a1)
        lpv = stan::math::dirichlet_lpdf<false>(th, alpha);
      else if (a0)
        lpv = stan::math::dirichlet_lpdf<false>(th, alpha_d);
      else if (a1)
        lpv = stan::math::dirichlet_lpdf<false>(thd, alpha);
      else
        lpv = stan::math::dirichlet_lpdf<false>(thd, alpha_d);
    }
    const double vv = lpv.val();
    if (!values_only()) {
      stan::math::grad(lpv.vi_);
      double* s = ctx.scratch;
      for (int64_t r = 0; r < reps; ++r)
        for (int64_t i = 0; i < K; ++i) *s++ = th[r](i).adj();
      for (int64_t i = 0; i < K; ++i) *s++ = alpha(i).adj();
    }
    return vv;
  }
  if (propto) {
    if (a0 && a1)
      lp = stan::math::dirichlet_lpdf<true>(theta, alpha);
    else if (a0)
      lp = stan::math::dirichlet_lpdf<true>(theta, alpha_d);
    else if (a1)
      lp = stan::math::dirichlet_lpdf<true>(theta_d, alpha);
    else
      lp = 0.0;
  } else {
    if (a0 && a1)
      lp = stan::math::dirichlet_lpdf<false>(theta, alpha);
    else if (a0)
      lp = stan::math::dirichlet_lpdf<false>(theta, alpha_d);
    else if (a1)
      lp = stan::math::dirichlet_lpdf<false>(theta_d, alpha);
    else
      lp = stan::math::dirichlet_lpdf<false>(theta_d, alpha_d);
  }
  const double value = lp.val();
  if (!values_only()) {
    stan::math::grad(lp.vi_);
    double* s = ctx.scratch;
    for (int64_t i = 0; i < ctx.in[0].len; ++i) *s++ = theta(i).adj();
    for (int64_t i = 0; i < ctx.in[1].len; ++i) *s++ = alpha(i).adj();
  }
  return value;
}
void dirichlet_fwd(KernelCtx& ctx) { ctx.out.data[0] = dirichlet_eval(ctx); }

// One tape per gradient, not two: the forward grads it with a seed of 1 and
// stashes, the backward scales. dirichlet_lpdf reduces through a partials
// propagator, so the two seedings round identically.
void dirichlet_bwd(KernelCtx& ctx) {
  const double* s = ctx.scratch;
  for (int k = 0; k < 2; ++k) {
    if (ctx.in_adj[k].data)
      Eigen::Map<Eigen::ArrayXd>(ctx.in_adj[k].data, ctx.in[k].len) +=
          ctx.out_adj * Eigen::Map<const Eigen::ArrayXd>(s, ctx.in[k].len);
    s += ctx.in[k].len;
  }
}

void log_softmax_fwd(KernelCtx& ctx) {
  Eigen::Map<const Eigen::VectorXd> x(ctx.in[0].data, ctx.in[0].len);
  Eigen::Map<Eigen::VectorXd> out(ctx.out.data, ctx.out.len);
  out = stan::math::log_softmax(x);
}
void log_softmax_bwd(KernelCtx& ctx) {
  legacy_bwd_vec_in(ctx,
                    [](const auto& x) { return stan::math::log_softmax(x); });
}

}  // namespace

void register_legacy_kernels() {
  register_kernel(OP_LOG_SOFTMAX,
                  Kernel{log_softmax_fwd, log_softmax_bwd, nullptr});
  register_kernel(OP_SOFTMAX, Kernel{softmax_fwd, softmax_bwd, nullptr});
  register_kernel(OP_DIRICHLET_LPDF,
                  Kernel{dirichlet_fwd, dirichlet_bwd, sum_in_lens});
}

}  // namespace stanli
