// Shared machinery for the density kernel shards.
//
// One translation unit used to hold all of this and every instantiation
// it drives: every list entry times every activity mask times every
// scalar-or-vector shape of its arguments, which peaked at 7.6 GB of
// compiler memory and took most of an hour on a CI runner while the other
// cores idled. The definitions now
// live in densities_*.cpp, one shard per group, and this header is what
// they share. Nothing here instantiates anything on its own.
//
// The generated forwards are `<fn>_fwd_gen` in namespace stanli::dens,
// which is a named namespace rather than an anonymous one because
// densities.cpp registers symbols the shards define.
#ifndef STANLI_KERNELS_DENSITIES_IMPL_HPP
#define STANLI_KERNELS_DENSITIES_IMPL_HPP

// Deliberately depend on the kernel-facing layouts rather than graph.hpp:
// otherwise any Graph or Executor edit reinstantiates every density shard.
#include "density_fused.hpp"

#include <stanli/kernel_types.hpp>
#include <stanli/optable.hpp>
#include <stanli/pairwise_sum.hpp>
#include <stanli/recorder.hpp>

// The full prim aggregate, not per-density headers: densities call helpers
// like square() through two-phase lookup and rely on the whole overload set
// being visible at instantiation, exactly as today's generated C++ does.
#include <stan/math/prim.hpp>

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace stanli {
namespace dens {

// The scratch layout density_bwd<N> reads back: the partials for argument k
// live at scratch[sum of the lens of the arguments before k]. Writing it out
// per kernel is how a forward comes to disagree with its own backward
// silently, so both ends go through this.
inline sink sink_for_args(KernelCtx& ctx, int nargs) {
  sink s;
  int64_t off = 0;
  for (int k = 0; k < nargs; ++k) {
    s.buf[k] = ctx.scratch + off;
    s.len[k] = ctx.in[k].len;
    off += ctx.in[k].len;
  }
  s.connected = ctx.scratch + off;
  return s;
}

// Compile-time recursion over per-arg ACTIVITY (Mask bit: 1 = autodiff
// rvar, 0 = plain double) with runtime shape branching inside. Matching the
// data/parameter instantiation CmdStan's generated code uses is what makes
// propto term-dropping and evaluation order line up exactly.
// VecMask marks arguments that are a VECTOR whatever their length: a
// cutpoint set of one element is a one-element vector, not a scalar, and
// binding it as a scalar does not merely mis-shape the call -- it picks a
// stan-math overload that reaches for members the recorder's scalar edge
// does not have, and fails to compile. Ordinary arguments (bit clear)
// keep the runtime length branch, which is what lets one instantiation
// serve a broadcast scalar and a full vector.
template <int NArgs, unsigned Mask, unsigned VecMask, typename F,
          typename... Bound>
void bind_args_m(KernelCtx& ctx, F&& f, const Bound&... bound) {
  if constexpr (sizeof...(Bound) == NArgs) {
    record_probability_call([&] { return f(bound...); });
  } else {
    constexpr int i = sizeof...(Bound);
    constexpr bool active = ((Mask >> i) & 1u) != 0;
    constexpr bool always_vec = ((VecMask >> i) & 1u) != 0;
    const auto bind_vector = [&] {
      if constexpr (active) {
        bind_args_m<NArgs, Mask, VecMask>(ctx, f, bound..., as_rvar(ctx.in[i]));
      } else {
        bind_args_m<NArgs, Mask, VecMask>(
            ctx, f, bound...,
            Eigen::Map<const Eigen::VectorXd>(ctx.in[i].data, ctx.in[i].len));
      }
    };
    if constexpr (always_vec) {
      bind_vector();  // no length test: a one-element set is still a vector
    } else if (ctx.in[i].len == 1) {
      if constexpr (active) {
        bind_args_m<NArgs, Mask, VecMask>(ctx, f, bound...,
                                          rvar(ctx.in[i].data[0]));
      } else {
        bind_args_m<NArgs, Mask, VecMask>(ctx, f, bound..., ctx.in[i].data[0]);
      }
    } else {
      bind_vector();
    }
  }
}

// Runtime mask -> compile-time Mask instantiation.
template <int NArgs, unsigned VecMask, typename F, unsigned M = 0>
void mask_dispatch(unsigned mask, KernelCtx& ctx, F&& f) {
  if (mask == M) {
    bind_args_m<NArgs, M, VecMask>(ctx, f);
    return;
  }
  if constexpr (M + 1 < (1u << NArgs)) {
    mask_dispatch<NArgs, VecMask, F, M + 1>(mask, ctx, std::forward<F>(f));
  }
}

// Element binding for the elementwise mode: every argument is
// bound as a SCALAR -- element n of a len-N argument, the single value of a
// len-1 one. This is the same instantiation the per-lane scalar ops use, so
// element n's value and partials are bit-identical to a lone scalar op.
// f receives the element index first (idata-outcome lpmfs select their
// integer outcome with it; lpdfs ignore it).
template <int NArgs, unsigned Mask, typename F, typename... Bound>
void bind_args_elt_m(KernelCtx& ctx, int64_t n, F&& f, const Bound&... bound) {
  if constexpr (sizeof...(Bound) == NArgs) {
    record_probability_call([&] { return f(n, bound...); });
  } else {
    constexpr int i = sizeof...(Bound);
    constexpr bool active = ((Mask >> i) & 1u) != 0;
    const double v = ctx.in[i].data[ctx.in[i].len == 1 ? 0 : n];
    if constexpr (active) {
      bind_args_elt_m<NArgs, Mask>(ctx, n, f, bound..., rvar(v));
    } else {
      bind_args_elt_m<NArgs, Mask>(ctx, n, f, bound..., v);
    }
  }
}

template <int NArgs, typename F, unsigned M = 0>
void mask_dispatch_elt(unsigned mask, KernelCtx& ctx, int64_t n, F&& f) {
  if (mask == M) {
    bind_args_elt_m<NArgs, M>(ctx, n, f);
    return;
  }
  if constexpr (M + 1 < (1u << NArgs)) {
    mask_dispatch_elt<NArgs, F, M + 1>(mask, ctx, n, std::forward<F>(f));
  }
}

// The propto-off form. Without bit 0 the mask is ignored: with no terms
// dropped the value is the same for every mask, so one all-active binding
// covers them all and 2^NArgs - 1 instantiations stop existing. The
// partials computed for data arguments are then discarded (density_bwd
// skips an argument the executor gave no adjoint buffer).
template <int NArgs, int Tier, unsigned VecMask, typename F>
void full_form(unsigned mask, KernelCtx& ctx, F&& ff) {
  if constexpr ((Tier & STANLI_DENSITY_FULL_MASKS) != 0) {
    mask_dispatch<NArgs, VecMask>(mask, ctx, ff);
  } else {
    bind_args_m<NArgs, (1u << NArgs) - 1, VecMask>(ctx, ff);
  }
}

template <int NArgs, int Tier, typename F>
void full_form_elt(unsigned mask, KernelCtx& ctx, int64_t n, F&& ff) {
  if constexpr ((Tier & STANLI_DENSITY_FULL_MASKS) != 0) {
    mask_dispatch_elt<NArgs>(mask, ctx, n, ff);
  } else {
    bind_args_elt_m<NArgs, (1u << NArgs) - 1>(ctx, n, ff);
  }
}

// Elementwise-lp forward (DensityMode::elementwise): out is len N, out[n] is
// element n's lp instead of the sum. One recorder call per element; the sink's
// buffers aim each argument's single-double partial straight at its slot in
// the [k * N + n] scratch layout, so nothing is copied afterward.
template <int NArgs, int Tier, typename FProp, typename FFull>
void density_fwd_elt(KernelCtx& ctx, FProp&& fp, FFull&& ff) {
  sink s;
  const int64_t N = ctx.out.len;
  const DensityMode mode = density_mode(ctx);
  const unsigned mask = mode.active;
  for (int64_t n = 0; n < N; ++n) {
    for (int k = 0; k < NArgs; ++k) {
      s.buf[k] = ctx.scratch + static_cast<int64_t>(k) * N + n;
      s.len[k] = 1;
    }
    s.connected = ctx.scratch + static_cast<int64_t>(NArgs) * N + n;
    {
      sink_scope active(s);
      if constexpr ((Tier & STANLI_DENSITY_PROPTO) != 0) {
        if (mode.propto) {
          mask_dispatch_elt<NArgs>(mask, ctx, n, fp);
        } else {
          full_form_elt<NArgs, Tier>(mask, ctx, n, ff);
        }
      } else {
        full_form_elt<NArgs, Tier>(mask, ctx, n, ff);
      }
    }
    ctx.out.data[n] = s.value;
  }
}

// Tier bits from optable.hpp decide which of the four instantiation
// families this density actually has. A missing propto family is not a
// refusal: the full form is a correct log density, just not the one
// CmdStan's `~` computes, so it lands a constant higher.
// The summed forward, and the only one most densities have. Partials for
// argument k land at scratch[sum of lens of args < k]; the mask decides
// which arguments are autodiff, the tier which instantiation families
// exist. Whatever the outcome looks like -- a propagator edge, one idata
// group, two, an integer array -- it is bound by the caller's lambdas, so
// this is shared by every density shape in these shards.
template <int NArgs, int Tier, unsigned VecMask, typename FProp, typename FFull>
void density_fwd_sum(KernelCtx& ctx, FProp&& fp, FFull&& ff) {
  sink s = sink_for_args(ctx, NArgs);
  const DensityMode mode = density_mode(ctx);
  const unsigned mask = mode.active;
  {
    sink_scope active(s);
    if constexpr ((Tier & STANLI_DENSITY_PROPTO) != 0) {
      if (mode.propto) {
        mask_dispatch<NArgs, VecMask>(mask, ctx, fp);
      } else {
        full_form<NArgs, Tier, VecMask>(mask, ctx, ff);
      }
    } else {
      full_form<NArgs, Tier, VecMask>(mask, ctx, ff);
    }
  }
  ctx.out.data[0] = s.value;
}

template <int NArgs, int Tier, unsigned VecMask = 0, typename FProp,
          typename FFull>
void density_fwd_v(KernelCtx& ctx, FProp&& fp, FFull&& ff) {
  // A density with a vector-only argument has no elementwise form to
  // instantiate -- the per-lane binding hands every argument over as a
  // scalar, which is exactly what VecMask exists to forbid. reroll.cpp
  // never fuses these (they are in neither of its opt-in lists), so the
  // bit cannot arrive; refuse rather than write one element of N.
  if constexpr (VecMask != 0) {
    if (density_mode(ctx).elementwise)
      throw std::runtime_error(
          "density: elementwise variant on a vector-argument density");
  } else if (density_mode(ctx).elementwise) {
    // Real-argument densities reuse the same lambdas: scalar bindings
    // instantiate the same generic templates.
    density_fwd_elt<NArgs, Tier>(
        ctx, [&](int64_t, const auto&... a) { return fp(a...); },
        [&](int64_t, const auto&... a) { return ff(a...); });
    return;
  }
  density_fwd_sum<NArgs, Tier, VecMask>(ctx, fp, ff);
}

#if defined(_MSC_VER) && !defined(__clang__)
#define STANLI_NOINLINE __declspec(noinline)
#else
#define STANLI_NOINLINE __attribute__((noinline))
#endif

// One adjoint update for a scalar call. Not inlined, so that every caller
// multiplies in the same operand order: on x86-64 and arm64 a NaN meeting a
// NaN keeps the payload of the first operand (6d9af6ecf).
STANLI_NOINLINE inline void accumulate_partial(double* adj, double out_adj,
                                               double partial) {
  *adj += out_adj * partial;
}

// accumulate_partial over the connected elements of a column. A result that
// is not NaN does not depend on the operand order, so it is computed in
// place; a NaN result is computed again by accumulate_partial itself, so
// its payload is the one a scalar call gives.
inline void accumulate_partials(double* adj, const double* out_adj,
                                const double* partial, const double* connected,
                                int64_t n) {
  for (int64_t i = 0; i < n; ++i) {
    if (connected[i] == 0.0) continue;
    const double r = adj[i] + out_adj[i] * partial[i];
    if (r == r)
      adj[i] = r;
    else
      accumulate_partial(adj + i, out_adj[i], partial[i]);
  }
}

// Partials for argument k live at scratch[sum of lens of args < k]. A scalar
// argument paired with vector ones holds the already-summed partial.
// Elementwise variant: column k is scratch[k * N .. k * N + N), each element
// scaled by its own adjoint; a broadcast (len-1) argument sums its column.
// A short column is added descending, as the executor's reverse sweep over
// the scalar ops that the fused op replaces adds it; a long one pairwise.
template <int NArgs>
void density_bwd(KernelCtx& ctx) {
  const DensityMode mode = density_mode(ctx);
  const unsigned mask = mode.active;
  if (mode.elementwise) {
    const int64_t N = ctx.out.len;
    const double* connected = ctx.scratch + static_cast<int64_t>(NArgs) * N;
    for (int k = 0; k < NArgs; ++k) {
      if (((mask >> k) & 1u) == 0 || ctx.in_adj[k].data == nullptr) continue;
      const double* col = ctx.scratch + static_cast<int64_t>(k) * N;
      if (ctx.in_adj[k].len == 1 && N > 1) {
        double acc = 0;
        add_sum(acc, N, [&](int64_t j) {
          const int64_t n = N - 1 - j;
          return connected[n] != 0.0 ? ctx.out_adj_vec.data[n] * col[n] : 0.0;
        });
        ctx.in_adj[k].data[0] += acc;
      } else {
        accumulate_partials(ctx.in_adj[k].data, ctx.out_adj_vec.data, col,
                            connected, N);
      }
    }
    return;
  }
  int64_t n_partials = 0;
  for (int k = 0; k < NArgs; ++k) n_partials += ctx.in[k].len;
  if (ctx.scratch[n_partials] == 0.0) return;
  int64_t off = 0;
  for (int k = 0; k < NArgs; ++k) {
    if (((mask >> k) & 1u) != 0 && ctx.in_adj[k].data != nullptr) {
      if (ctx.in[k].len == 1)
        accumulate_partial(ctx.in_adj[k].data, ctx.out_adj, ctx.scratch[off]);
      else
        Eigen::Map<Eigen::ArrayXd>(ctx.in_adj[k].data, ctx.in[k].len) +=
            ctx.out_adj *
            Eigen::Map<const Eigen::ArrayXd>(ctx.scratch + off, ctx.in[k].len);
    }
    off += ctx.in[k].len;
  }
}

template <int NArgs>
bool density_micro(const KernelCtx& ctx, std::vector<BwdMicro>& out) {
  const DensityMode mode = density_mode(ctx);
  if (mode.elementwise || ctx.out.len != 1) return false;
  const unsigned mask = mode.active;
  int64_t n_partials = 0;
  for (int k = 0; k < NArgs; ++k) n_partials += ctx.in[k].len;
  for (int k = 0; k < NArgs; ++k)
    if (((mask >> k) & 1u) != 0 && ctx.in_adj[k].data != nullptr &&
        ctx.in[k].len != 1)
      return false;
  const double* connected = ctx.scratch + n_partials;
  int64_t off = 0;
  for (int k = 0; k < NArgs; ++k) {
    if (((mask >> k) & 1u) != 0 && ctx.in_adj[k].data != nullptr)
      out.push_back(bwd_micro(kMicroScaled, ctx.in_adj[k].data,
                              ctx.out_adj_vec.data, ctx.scratch + off,
                              connected));
    off += ctx.in[k].len;
  }
  return true;
}

// Elementwise ops need one partial per argument per element, even for
// broadcast scalars (each element scales by its own adjoint).
template <int NArgs>
int64_t density_scratch(const Op& op, const Slot* slots) {
  if (density_mode(op).elementwise) return (NArgs + 1) * slots[op.out].len;
  return sum_in_lens(op, slots) + 1;
}

// ---- lpmfs: integer outcome from idata, not a propagator edge --------------
// The elementwise variant hands each recorder call outcome element n; the
// summed path keeps the whole-vector call. STANLI_LPMF_FWD expands both.
#define STANLI_LPMF_FWD_T(fname, dist, NARGS, TIER)                          \
  void fname(KernelCtx& ctx) {                                               \
    if (density_mode(ctx).elementwise) {                                     \
      density_fwd_elt<NARGS, TIER>(                                          \
          ctx,                                                               \
          [&](int64_t n, const auto&... a) {                                 \
            return stan::math::dist<true>(ctx.idata[n], a...);               \
          },                                                                 \
          [&](int64_t n, const auto&... a) {                                 \
            return stan::math::dist<false>(ctx.idata[n], a...);              \
          });                                                                \
      return;                                                                \
    }                                                                        \
    Eigen::Map<const Eigen::VectorXi> y(                                     \
        ctx.idata, static_cast<Eigen::Index>(ctx.n_idata));                  \
    density_fwd_v<NARGS, TIER>(                                              \
        ctx,                                                                 \
        [&](const auto&... a) { return stan::math::dist<true>(y, a...); },   \
        [&](const auto&... a) { return stan::math::dist<false>(y, a...); }); \
  }
#define STANLI_LPMF_FWD(fname, dist, NARGS) \
  STANLI_LPMF_FWD_T(fname, dist, NARGS, 3)

// One line per density in STANLI_SCALAR_DENSITY_LIST (optable.hpp) makes
// the forward, and the registration below makes the rest. Everything they
// share -- propto and per-argument activity from the variant byte,
// elementwise output, the partials stashed for density_bwd to contract --
// lives in density_fwd_v.
#define STANLI_DEFINE_DENSITY_FWD(code, fn, n, tier)                        \
  void fn##_fwd_gen(KernelCtx& ctx) {                                       \
    if constexpr (has_fused_kernel(code)) {                                 \
      if (fused_density_active()) {                                         \
        fused_kernel_for(code)(ctx);                                        \
        return;                                                             \
      }                                                                     \
    }                                                                       \
    if constexpr (!(has_fused_kernel(code) && kFusedOnly)) {                \
      density_fwd_v<n, tier>(                                               \
          ctx, [](const auto&... a) { return stan::math::fn<true>(a...); }, \
          [](const auto&... a) { return stan::math::fn<false>(a...); });    \
    }                                                                       \
  }

// Distribution functions: one form, no propto, and no elementwise
// variant either. reroll.cpp only ever fuses opcodes it opts in by name,
// and cdfs are not among them, so the elementwise mode cannot arrive -- writing
// this separately from density_fwd_v is what keeps the per-lane binding
// from being instantiated for 75 functions that will never use it. It is
// not merely wasted code: some cdfs do not survive being handed a bare
// scalar rvar where their vector overload expects a container.
template <int NArgs, typename F>
void cdf_fwd(KernelCtx& ctx, F&& f) {
  // Cannot happen today -- reroll.cpp opts densities into the fusion by
  // opcode and cdfs are in neither list -- but the failure if it ever did
  // would be silent: out would be len N and only element 0 written. Say
  // so instead. The branch costs nothing next to an incomplete beta.
  if (density_mode(ctx).elementwise)
    throw std::runtime_error("cdf: elementwise variant not implemented");
  // Tier 0: no propto family, so the first lambda is never instantiated; it
  // exists to satisfy the shared signature.
  density_fwd_sum<NArgs, 0, 0>(
      ctx, [](const auto&...) { return 0.0; }, std::forward<F>(f));
}

#define STANLI_DEFINE_CDF_FWD(code, fn, n)                                  \
  void fn##_fwd_gen(KernelCtx& ctx) {                                       \
    cdf_fwd<n>(ctx, [](const auto&... a) { return stan::math::fn(a...); }); \
  }

// Integer-outcome cdfs. Same as above with the count read from idata,
// the way the lpmfs read theirs -- a whole vector of outcomes in one
// call, since the summed form is the only one these have.
// A singleton Poisson log-CDF with a scalar rate can use the scalar Stan
// overload: identical value/partial arithmetic, without allocating Eigen
// temporaries for its incomplete-gamma expression. Other CDFs and broadcast
// shapes retain their separately verified container instantiations.
#define STANLI_DEFINE_INT_CDF_FWD(code, fn, nreal)                             \
  void fn##_fwd_gen(KernelCtx& ctx) {                                          \
    if constexpr (code == OP_POISSON_LCDF) {                                   \
      if (ctx.n_idata == 1 && ctx.in[0].len == 1) {                            \
        cdf_fwd<nreal>(ctx, [&](const auto&... a) {                            \
          return stan::math::fn(ctx.idata[0], a...);                           \
        });                                                                    \
        return;                                                                \
      }                                                                        \
    }                                                                          \
    Eigen::Map<const Eigen::VectorXi> y(                                       \
        ctx.idata, static_cast<Eigen::Index>(ctx.n_idata));                    \
    cdf_fwd<nreal>(ctx,                                                        \
                   [&](const auto&... a) { return stan::math::fn(y, a...); }); \
  }

// Declarations of everything the shards define, so registration in
// densities.cpp can name them without seeing their bodies.
#define STANLI_DECLARE_FWD(code, fn, ...) void fn##_fwd_gen(KernelCtx&);
STANLI_SCALAR_DENSITY_LIST(STANLI_DECLARE_FWD)
STANLI_INT_DENSITY_LIST(STANLI_DECLARE_FWD)
STANLI_SCALAR_CDF_LIST(STANLI_DECLARE_FWD)
STANLI_INT_CDF_LIST(STANLI_DECLARE_FWD)
STANLI_TWO_INT_CDF_LIST(STANLI_DECLARE_FWD)
#undef STANLI_DECLARE_FWD

// The hand-written ones, which predate the lists and are not the same
// shape (two int groups or a data matrix).
void poisson_log_fwd(KernelCtx&);
void bernoulli_logit_fwd(KernelCtx&);
void bernoulli_fwd(KernelCtx&);
void poisson_fwd(KernelCtx&);
void neg_binomial_2_fwd(KernelCtx&);
void binomial_fwd(KernelCtx&);
void binomial_logit_fwd(KernelCtx&);
void bernoulli_logit_glm_fwd(KernelCtx&);
void bernoulli_logit_glm_bwd(KernelCtx&);
void poisson_log_glm_fwd(KernelCtx&);
void poisson_log_glm_bwd(KernelCtx&);
void neg_binomial_2_log_glm_fwd(KernelCtx&);
void neg_binomial_2_log_glm_bwd(KernelCtx&);
void beta_binomial_fwd(KernelCtx&);
}  // namespace dens
}  // namespace stanli

#endif
