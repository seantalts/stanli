// log_sum_exp / log_mix.
//
// The forward computes the value and stashes every partial; the backward is
// a scale of what the forward left in scratch. A backward that reads only
// scratch cannot be broken by a destructive write to its input, which is
// what gives these ops the value-free trait in optable.hpp and lets the HMM
// forward algorithm -- fill `acc` element by element, read it whole --
// take the in-place path.
//
// The partial forms are stan-math's own reverse-mode chains, written out.
#include <stanli/graph.hpp>
#include <stanli/optable.hpp>

#include <stan/math.hpp>

#include <cassert>
#include <cmath>

namespace stanli {
namespace {

// ---- log_sum_exp over a vector --------------------------------------------
int64_t lse_scratch(const Op& op, const Slot* slots) {
  return slots[op.in[0]].len;
}

// Packet exp rounds up to a last bit away from the scalar-libm var reference.
double lse_fwd_partials(const double* data, int64_t len, double* partials) {
  const double value =
      stan::math::log_sum_exp(Eigen::Map<const Eigen::VectorXd>(data, len));
  Eigen::Map<Eigen::ArrayXd>(partials, len) =
      (Eigen::Map<const Eigen::ArrayXd>(data, len) - value).exp();
  return value;
}

void lse_fwd(KernelCtx& ctx) {
  ctx.out.data[0] =
      lse_fwd_partials(ctx.in[0].data, ctx.in[0].len, ctx.scratch);
}

void lse_bwd(KernelCtx& ctx) {
  if (!ctx.in_adj[0].data) return;
  const int64_t n = ctx.in[0].len;
  for (int64_t i = 0; i < n; ++i)
    ctx.in_adj[0].data[i] += ctx.out_adj * ctx.scratch[i];
}

// ---- log_sum_exp over packed rows -----------------------------------------
// One flat row-major input, idata[0] = row width K, and one scalar output per
// row. Each row takes the exact scalar OP_LOG_SUM_EXP path above. Reduction is
// deliberately a separate op: this kernel maps rows and does not choose how
// the caller groups their target terms.
void lse_rows_fwd(KernelCtx& ctx) {
  assert(ctx.n_idata == 1);
  const int64_t K = ctx.idata[0];
  assert(K > 0 && ctx.in[0].len == ctx.out.len * K);
  for (int64_t r = 0; r < ctx.out.len; ++r) {
    const int64_t off = r * K;
    ctx.out.data[r] =
        lse_fwd_partials(ctx.in[0].data + off, K, ctx.scratch + off);
  }
}

void lse_rows_bwd(KernelCtx& ctx) {
  if (!ctx.in_adj[0].data) return;
  assert(ctx.n_idata == 1);
  const int64_t K = ctx.idata[0];
  assert(K > 0 && ctx.in[0].len == ctx.out.len * K);
  for (int64_t r = 0; r < ctx.out.len; ++r) {
    const int64_t off = r * K;
    const double scale = ctx.out_adj_vec.data[r];
    for (int64_t k = 0; k < K; ++k)
      ctx.in_adj[0].data[off + k] += scale * ctx.scratch[off + k];
  }
}

// ---- log_sum_exp(a, b) / log_mix(theta, a, b) -----------------------------
// Shape dispatch matches the elementwise binaries: each argument is len 1
// (broadcast) or len N, out is len N, out[n] applies the scalar stan-math
// function to element n. N == 1 is exactly the old scalar kernel; the len-N
// form is the re-rolled body of a per-observation mixture loop.
//
// Scratch is NArgs partials per element, [n * NArgs + k]. `f` writes that
// element's partials and returns its value.
template <int NArgs>
int64_t mix_scratch(const Op& op, const Slot* slots) {
  return NArgs * slots[op.out].len;
}

template <int NArgs, typename F>
void mix_fwd(KernelCtx& ctx, F&& f) {
  const int64_t n = ctx.out.len;
  for (int64_t i = 0; i < n; ++i) {
    double a[NArgs];
    for (int k = 0; k < NArgs; ++k)
      a[k] = ctx.in[k].data[ctx.in[k].len == 1 ? 0 : i];
    ctx.out.data[i] = f(a, ctx.scratch + i * NArgs);
  }
}

template <int NArgs>
void mix_bwd(KernelCtx& ctx) {
  const int64_t n = ctx.out.len;
  for (int k = 0; k < NArgs; ++k) {
    if (!ctx.in_adj[k].data) continue;
    if (ctx.in_adj[k].len == 1 && n > 1) {
      // Broadcast argument: every element contributes. Accumulate with i
      // descending -- the order N scalar ops would add to this adjoint in
      // the executor's reverse sweep -- so the fused op is bit-identical
      // to the loop it replaces.
      double acc = 0;
      for (int64_t i = n; i-- > 0;)
        acc += ctx.out_adj_vec.data[i] * ctx.scratch[i * NArgs + k];
      ctx.in_adj[k].data[0] += acc;
    } else {
      for (int64_t i = 0; i < n; ++i)
        ctx.in_adj[k].data[i] +=
            ctx.out_adj_vec.data[i] * ctx.scratch[i * NArgs + k];
    }
  }
}

void lse2_fwd(KernelCtx& ctx) {
  mix_fwd<2>(ctx, [](const double* a, double* p) {
    p[0] = stan::math::inv_logit(a[0] - a[1]);
    p[1] = stan::math::inv_logit(a[1] - a[0]);
    return stan::math::log_sum_exp(a[0], a[1]);
  });
}

// log_diff_exp(a, b) = log(exp(a) - exp(b)). Truncation is what wants
// it: stanc3 lowers `y ~ foo(...) T[l, u]` into the density minus
// log_diff_exp(foo_lcdf(u|...), foo_lcdf(l|...)).
void log_diff_exp_fwd(KernelCtx& ctx) {
  mix_fwd<2>(ctx, [](const double* a, double* p) {
    p[0] = -1.0 / stan::math::expm1(a[1] - a[0]);
    p[1] = -1.0 / stan::math::expm1(a[0] - a[1]);
    return stan::math::log_diff_exp(a[0], a[1]);
  });
}

void log_mix_fwd(KernelCtx& ctx) {
  mix_fwd<3>(ctx, [](const double* a, double* p) {
    const double value = stan::math::log_mix(a[0], a[1], a[2]);
    double theta = a[0];
    double one_m_exp_lam2_m_lam1 = 0.0;
    double one_m_t_prod_exp_lam2_m_lam1 = 0.0;
    double one_d = 0.0;
    if (a[1] > a[2]) {
      stan::math::log_mix_partial_helper(theta, a[1], a[2],
                                         one_m_exp_lam2_m_lam1,
                                         one_m_t_prod_exp_lam2_m_lam1, one_d);
    } else {
      stan::math::log_mix_partial_helper(1.0 - theta, a[2], a[1],
                                         one_m_exp_lam2_m_lam1,
                                         one_m_t_prod_exp_lam2_m_lam1, one_d);
      one_m_exp_lam2_m_lam1 = -one_m_exp_lam2_m_lam1;
      theta = one_m_t_prod_exp_lam2_m_lam1;
      one_m_t_prod_exp_lam2_m_lam1 = 1.0 - a[0];
    }
    p[0] = one_m_exp_lam2_m_lam1 * one_d;
    p[1] = theta * one_d;
    p[2] = one_m_t_prod_exp_lam2_m_lam1 * one_d;
    return value;
  });
}

}  // namespace

void register_mixture_kernels() {
  register_kernel(OP_LOG_SUM_EXP, Kernel{lse_fwd, lse_bwd, lse_scratch});
  register_kernel(OP_LOG_SUM_EXP_ROWS,
                  Kernel{lse_rows_fwd, lse_rows_bwd, lse_scratch});
  register_kernel(OP_LSE2, Kernel{lse2_fwd, mix_bwd<2>, mix_scratch<2>});
  register_kernel(OP_LOG_DIFF_EXP,
                  Kernel{log_diff_exp_fwd, mix_bwd<2>, mix_scratch<2>});
  register_kernel(OP_LOG_MIX, Kernel{log_mix_fwd, mix_bwd<3>, mix_scratch<3>});
}

}  // namespace stanli
