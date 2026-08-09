// log_sum_exp / log_mix.
//
// These were legacy ops whose backward replayed on `Matrix<var>`: N varis
// reached through N pointers, built and torn down per op per gradient.
// That is the AoS var tape stanli exists to avoid, sitting in the hot loop
// of every mixture and HMM model (40,636 ops across 9 corpus models).
//
// stan-math still computes every derivative here -- nothing is
// hand-differentiated. Two things changed:
//
//   * the operand is `var_value<VectorXd>` (varmat, SoA) rather than
//     `Matrix<var>`, which is what `stanc --O1` reaches for and roughly
//     twice as fast: 3.39 vs 6.68 ns/element on Apple M-series, and
//   * the replay runs in the FORWARD sweep and stashes the partials, so
//     the backward is a scale of what stan-math already returned.
//
// The second point is not just bookkeeping: a backward that reads only
// scratch cannot be broken by a destructive write to its input, which is
// what puts these ops on the whitelist in inplace.cpp and lets the HMM
// forward algorithm -- fill `acc` element by element, read it whole --
// take the in-place path.
#include <stanli/graph.hpp>
#include <stanli/legacy.hpp>
#include <stanli/optable.hpp>

#include <stan/math.hpp>

#include <array>

namespace stanli {
namespace {

// ---- log_sum_exp over a vector --------------------------------------------
int64_t lse_scratch(const Op& op, const Slot* slots) {
  return slots[op.in[0]].len;
}

void lse_fwd(KernelCtx& ctx) {
  ctx.out.data[0] = legacy_fwd_partials_vec(
      ctx, [](const auto& x) { return stan::math::log_sum_exp(x); });
}

void lse_bwd(KernelCtx& ctx) {
  if (!ctx.in_adj[0].data) return;
  const int64_t n = ctx.in[0].len;
  for (int64_t i = 0; i < n; ++i)
    ctx.in_adj[0].data[i] += ctx.out_adj * ctx.scratch[i];
}

// ---- log_sum_exp(a, b) / log_mix(theta, a, b) -----------------------------
// Shape dispatch matches the elementwise binaries: each argument is len 1
// (broadcast) or len N, out is len N, out[n] applies the scalar stan-math
// function to element n. N == 1 is exactly the old scalar kernel; the len-N
// form is the re-rolled body of a per-observation mixture loop. Each element
// replays on its own nested tape, so element n's value and partials are
// bit-identical to a lone scalar op over the same inputs.
//
// Scratch is NArgs partials per element, [n * NArgs + k].
template <int NArgs>
int64_t mix_scratch(const Op& op, const Slot* slots) {
  return NArgs * slots[op.out].len;
}

template <int NArgs, typename F>
void mix_fwd(KernelCtx& ctx, F&& f) {
  const int64_t n = ctx.out.len;
  for (int64_t i = 0; i < n; ++i) {
    stan::math::nested_rev_autodiff nested;
    std::array<stan::math::var, NArgs> a;
    for (int k = 0; k < NArgs; ++k)
      a[k] = ctx.in[k].data[ctx.in[k].len == 1 ? 0 : i];
    stan::math::var j = f(a);
    ctx.out.data[i] = j.val();
    stan::math::grad(j.vi_);
    for (int k = 0; k < NArgs; ++k) ctx.scratch[i * NArgs + k] = a[k].adj();
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
  mix_fwd<2>(ctx,
             [](const auto& a) { return stan::math::log_sum_exp(a[0], a[1]); });
}

// log_diff_exp(a, b) = log(exp(a) - exp(b)). Truncation is what wants
// it: stanc3 lowers `y ~ foo(...) T[l, u]` into the density minus
// log_diff_exp(foo_lcdf(u|...), foo_lcdf(l|...)).
void log_diff_exp_fwd(KernelCtx& ctx) {
  mix_fwd<2>(
      ctx, [](const auto& a) { return stan::math::log_diff_exp(a[0], a[1]); });
}

void log_mix_fwd(KernelCtx& ctx) {
  mix_fwd<3>(
      ctx, [](const auto& a) { return stan::math::log_mix(a[0], a[1], a[2]); });
}

}  // namespace

void register_mixture_kernels() {
  register_kernel(OP_LOG_SUM_EXP, Kernel{lse_fwd, lse_bwd, lse_scratch});
  register_kernel(OP_LSE2, Kernel{lse2_fwd, mix_bwd<2>, mix_scratch<2>});
  register_kernel(OP_LOG_DIFF_EXP,
                  Kernel{log_diff_exp_fwd, mix_bwd<2>, mix_scratch<2>});
  register_kernel(OP_LOG_MIX, Kernel{log_mix_fwd, mix_bwd<3>, mix_scratch<3>});
}

}  // namespace stanli
