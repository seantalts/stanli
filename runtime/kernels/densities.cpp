// Recorder density kernels: unmodified stan-math prim/prob templates
// instantiated on rvar, depositing value + partials via the active sink.
//
// One kernel per signature. Every real argument is bound as rvar (zero-copy
// for vectors), so all partials are computed and the data/parameter
// distinction is runtime-only: backward contracts an argument's partials
// only if the executor gave it an adjoint buffer. Shape (scalar vs vector)
// is the one compile-time axis; bind_args expands the 2^N combinations and
// the argument lengths select one per call.
#include <stanli/graph.hpp>
#include <stanli/optable.hpp>
#include <stanli/recorder.hpp>

// The full prim aggregate, not per-density headers: densities call helpers
// like square() through two-phase lookup and rely on the whole overload set
// being visible at instantiation, exactly as today's generated C++ does.
#include <stan/math/prim.hpp>

#include <algorithm>
#include <stdexcept>

namespace stanli {
namespace {

// Compile-time recursion over per-arg ACTIVITY (Mask bit: 1 = autodiff
// rvar, 0 = plain double) with runtime shape branching inside. Matching the
// data/parameter instantiation CmdStan's generated code uses is what makes
// propto term-dropping and evaluation order line up exactly.
template <int NArgs, unsigned Mask, typename F, typename... Bound>
void bind_args_m(KernelCtx& ctx, F&& f, const Bound&... bound) {
  if constexpr (sizeof...(Bound) == NArgs) {
    f(bound...);
  } else {
    constexpr int i = sizeof...(Bound);
    constexpr bool active = ((Mask >> i) & 1u) != 0;
    if (ctx.in[i].len == 1) {
      if constexpr (active) {
        bind_args_m<NArgs, Mask>(ctx, f, bound..., rvar(ctx.in[i].data[0]));
      } else {
        bind_args_m<NArgs, Mask>(ctx, f, bound..., ctx.in[i].data[0]);
      }
    } else {
      if constexpr (active) {
        bind_args_m<NArgs, Mask>(ctx, f, bound..., as_rvar(ctx.in[i]));
      } else {
        bind_args_m<NArgs, Mask>(
            ctx, f, bound...,
            Eigen::Map<const Eigen::VectorXd>(ctx.in[i].data, ctx.in[i].len));
      }
    }
  }
}

// Runtime mask -> compile-time Mask instantiation.
template <int NArgs, typename F, unsigned M = 0>
void mask_dispatch(unsigned mask, KernelCtx& ctx, F&& f) {
  if (mask == M) {
    bind_args_m<NArgs, M>(ctx, f);
    return;
  }
  if constexpr (M + 1 < (1u << NArgs)) {
    mask_dispatch<NArgs, F, M + 1>(mask, ctx, std::forward<F>(f));
  }
}

// Element binding for the elementwise variant (bit 6): every argument is
// bound as a SCALAR -- element n of a len-N argument, the single value of a
// len-1 one. This is the same instantiation the per-lane scalar ops use, so
// element n's value and partials are bit-identical to a lone scalar op.
// f receives the element index first (idata-outcome lpmfs select their
// integer outcome with it; lpdfs ignore it).
template <int NArgs, unsigned Mask, typename F, typename... Bound>
void bind_args_elt_m(KernelCtx& ctx, int64_t n, F&& f, const Bound&... bound) {
  if constexpr (sizeof...(Bound) == NArgs) {
    f(n, bound...);
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
template <int NArgs, int Tier, typename F>
void full_form(unsigned mask, KernelCtx& ctx, F&& ff) {
  if constexpr ((Tier & STANLI_DENSITY_FULL_MASKS) != 0) {
    mask_dispatch<NArgs>(mask, ctx, ff);
  } else {
    bind_args_m<NArgs, (1u << NArgs) - 1>(ctx, ff);
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

// Elementwise-lp forward (variant bit 6): out is len N, out[n] is element
// n's lp instead of the sum. One recorder call per element; the sink's
// buffers aim each argument's single-double partial straight at its slot in
// the [k * N + n] scratch layout, so nothing is copied afterward.
template <int NArgs, int Tier, typename FProp, typename FFull>
void density_fwd_elt(KernelCtx& ctx, FProp&& fp, FFull&& ff) {
  sink s;
  const int64_t N = ctx.out.len;
  const unsigned mask = ctx.variant & 0x3fu;
  for (int64_t n = 0; n < N; ++n) {
    for (int k = 0; k < NArgs; ++k)
      s.buf[k] = ctx.scratch + static_cast<int64_t>(k) * N + n;
    active_sink() = &s;
    if constexpr ((Tier & STANLI_DENSITY_PROPTO) != 0) {
      if (ctx.variant & 0x80u) {
        mask_dispatch_elt<NArgs>(mask, ctx, n, fp);
      } else {
        full_form_elt<NArgs, Tier>(mask, ctx, n, ff);
      }
    } else {
      full_form_elt<NArgs, Tier>(mask, ctx, n, ff);
    }
    active_sink() = nullptr;
    ctx.out.data[n] = s.value;
  }
}

// Tier bits from optable.hpp decide which of the four instantiation
// families this density actually has. A missing propto family is not a
// refusal: the full form is a correct log density, just not the one
// CmdStan's `~` computes, so it lands a constant higher.
template <int NArgs, int Listed, typename FProp, typename FFull>
void density_fwd_v(KernelCtx& ctx, FProp&& fp, FFull&& ff) {
  constexpr int Tier = density_tier(Listed);
  if (ctx.variant & 0x40u) {
    // Real-argument densities reuse the same lambdas: scalar bindings
    // instantiate the same generic templates.
    density_fwd_elt<NArgs, Tier>(
        ctx, [&](int64_t, const auto&... a) { fp(a...); },
        [&](int64_t, const auto&... a) { ff(a...); });
    return;
  }
  sink s;
  int64_t off = 0;
  for (int k = 0; k < NArgs; ++k) {
    s.buf[k] = ctx.scratch + off;
    off += ctx.in[k].len;
  }
  const unsigned mask = ctx.variant == 0
                            ? (1u << NArgs) - 1  // default: all active
                            : (ctx.variant & 0x3fu);
  active_sink() = &s;
  if constexpr ((Tier & STANLI_DENSITY_PROPTO) != 0) {
    if (ctx.variant & 0x80u) {
      mask_dispatch<NArgs>(mask, ctx, fp);
    } else {
      full_form<NArgs, Tier>(mask, ctx, ff);
    }
  } else {
    full_form<NArgs, Tier>(mask, ctx, ff);
  }
  active_sink() = nullptr;
  ctx.out.data[0] = s.value;
}

// Partials for argument k live at scratch[sum of lens of args < k]. A scalar
// argument paired with vector ones holds the already-summed partial.
// Elementwise variant: column k is scratch[k * N .. k * N + N), each element
// scaled by its own adjoint; a broadcast (len-1) argument sums its column,
// descending to match the executor's reverse sweep over the scalar ops the
// fused op replaces (bit-identical accumulation).
template <int NArgs>
void density_bwd(KernelCtx& ctx) {
  const unsigned mask = ctx.variant == 0 ? (1u << NArgs) - 1
                                         : (ctx.variant & 0x3fu);
  if (ctx.variant & 0x40u) {
    const int64_t N = ctx.out.len;
    for (int k = 0; k < NArgs; ++k) {
      if (((mask >> k) & 1u) == 0 || ctx.in_adj[k].data == nullptr) continue;
      const double* col = ctx.scratch + static_cast<int64_t>(k) * N;
      if (ctx.in_adj[k].len == 1 && N > 1) {
        double acc = 0;
        for (int64_t n = N; n-- > 0;)
          acc += ctx.out_adj_vec.data[n] * col[n];
        ctx.in_adj[k].data[0] += acc;
      } else {
        for (int64_t n = 0; n < N; ++n)
          ctx.in_adj[k].data[n] += ctx.out_adj_vec.data[n] * col[n];
      }
    }
    return;
  }
  int64_t off = 0;
  for (int k = 0; k < NArgs; ++k) {
    if (((mask >> k) & 1u) != 0 && ctx.in_adj[k].data != nullptr) {
      for (int64_t i = 0; i < ctx.in[k].len; ++i)
        ctx.in_adj[k].data[i] += ctx.out_adj * ctx.scratch[off + i];
    }
    off += ctx.in[k].len;
  }
}

int64_t sum_in_lens(const Op& op, const Slot* slots) {
  int64_t t = 0;
  for (int i = 0; i < op.n_in; ++i) t += slots[op.in[i]].len;
  return t;
}

// Elementwise ops need one partial per argument per element, even for
// broadcast scalars (each element scales by its own adjoint).
template <int NArgs>
int64_t density_scratch(const Op& op, const Slot* slots) {
  if (op.variant & 0x40u) return NArgs * slots[op.out].len;
  return sum_in_lens(op, slots);
}

// ---- lpdfs: real args only -------------------------------------------------


// ---- lpmfs: integer outcome from idata, not a propagator edge --------------
// The elementwise variant hands each recorder call outcome element n; the
// summed path keeps the whole-vector call. STANLI_LPMF_FWD expands both.
#define STANLI_LPMF_FWD_T(fname, dist, NARGS, TIER)                            \
  void fname(KernelCtx& ctx) {                                                 \
    if (ctx.variant & 0x40u) {                                                 \
      density_fwd_elt<NARGS, density_tier(TIER)>(                              \
          ctx,                                                                 \
          [&](int64_t n, const auto&... a) {                                   \
            stan::math::dist<true>(ctx.idata[n], a...);                        \
          },                                                                   \
          [&](int64_t n, const auto&... a) {                                   \
            stan::math::dist<false>(ctx.idata[n], a...);                       \
          });                                                                  \
      return;                                                                  \
    }                                                                          \
    Eigen::Map<const Eigen::VectorXi> y(                                       \
        ctx.idata, static_cast<Eigen::Index>(ctx.n_idata));                    \
    density_fwd_v<NARGS, TIER>(                                                \
        ctx, [&](const auto&... a) { stan::math::dist<true>(y, a...); },       \
        [&](const auto&... a) { stan::math::dist<false>(y, a...); });          \
  }
#define STANLI_LPMF_FWD(fname, dist, NARGS)                                    \
  STANLI_LPMF_FWD_T(fname, dist, NARGS, 3)

STANLI_LPMF_FWD(poisson_log_fwd, poisson_log_lpmf, 1)
STANLI_LPMF_FWD(bernoulli_logit_fwd, bernoulli_logit_lpmf, 1)
STANLI_LPMF_FWD(bernoulli_fwd, bernoulli_lpmf, 1)
STANLI_LPMF_FWD(poisson_fwd, poisson_lpmf, 1)
STANLI_LPMF_FWD(neg_binomial_2_fwd, neg_binomial_2_lpmf, 2)
#undef STANLI_LPMF_FWD

// The same shape, one line per distribution (STANLI_INT_DENSITY_LIST).
#define STANLI_DEFINE_INT_DENSITY(code, fn, nreal, tier)                       \
  STANLI_LPMF_FWD_T(fn##_fwd_gen, fn, nreal, tier)
STANLI_INT_DENSITY_LIST(STANLI_DEFINE_INT_DENSITY)
#undef STANLI_DEFINE_INT_DENSITY
#undef STANLI_LPMF_FWD_T

// Binomials carry two int groups; idata = [len_n, n..., len_N, N...].
// A length of -1 marks a language-level int scalar (stan-math broadcasts
// scalars; a size-1 vector would be a size error against a longer group).
template <typename F>
void with_int_group(const int* p, F&& f) {
  const int len = static_cast<int>(p[0]);
  if (len == -1)
    f(p[1], p + 2);
  else
    f(Eigen::Map<const Eigen::VectorXi>(p + 1, len), p + 1 + len);
}
// Element n of a group: the scalar for all n, or the vector's n-th entry.
int int_group_elem(const int* p, int64_t n) {
  return p[0] == -1 ? p[1] : p[1 + n];
}
const int* int_group_next(const int* p) {
  return p + (p[0] == -1 ? 2 : 1 + p[0]);
}
#define STANLI_BINOMIAL_FWD(fname, dist)                                       \
  void fname(KernelCtx& ctx) {                                                 \
    if (ctx.variant & 0x40u) {                                                 \
      const int* g1 = ctx.idata;                                               \
      const int* g2 = int_group_next(g1);                                      \
      density_fwd_elt<1, 3>(                                                      \
          ctx,                                                                 \
          [&](int64_t n, const auto& theta) {                                  \
            stan::math::dist<true>(int_group_elem(g1, n),                      \
                                   int_group_elem(g2, n), theta);              \
          },                                                                   \
          [&](int64_t n, const auto& theta) {                                  \
            stan::math::dist<false>(int_group_elem(g1, n),                     \
                                    int_group_elem(g2, n), theta);             \
          });                                                                  \
      return;                                                                  \
    }                                                                          \
    with_int_group(ctx.idata, [&](const auto& n, const int* rest) {            \
      with_int_group(rest, [&](const auto& N, const int*) {                    \
        density_fwd_v<1, 3>(                                                      \
            ctx,                                                               \
            [&](const auto& theta) { stan::math::dist<true>(n, N, theta); },   \
            [&](const auto& theta) { stan::math::dist<false>(n, N, theta); }); \
      });                                                                      \
    });                                                                        \
  }

STANLI_BINOMIAL_FWD(binomial_fwd, binomial_lpmf)
STANLI_BINOMIAL_FWD(binomial_logit_fwd, binomial_logit_lpmf)
#undef STANLI_BINOMIAL_FWD
// bernoulli_logit_glm(y | X, alpha, beta): X data matrix (row-major slot),
// idata = [y..., rows, cols]. Edges are (x, alpha, beta); X is arg 0.
void bernoulli_logit_glm_fwd(KernelCtx& ctx) {
  const int64_t rows = ctx.idata[ctx.n_idata - 2];
  const int64_t cols = ctx.idata[ctx.n_idata - 1];
  Eigen::Map<const Eigen::VectorXi> y(ctx.idata, rows);
  Eigen::Map<const Eigen::MatrixXd> X(ctx.in[0].data, rows, cols);
  sink s;
  int64_t off = 0;
  for (int k = 0; k < 3; ++k) {
    s.buf[k] = ctx.scratch + off;
    off += ctx.in[k].len;
  }
  active_sink() = &s;
  if (ctx.in[1].len == 1) {
    // beta is a vector regardless of its length; alpha scalar.
    stan::math::bernoulli_logit_glm_lpmf<false>(
        y, X, rvar(ctx.in[1].data[0]), as_rvar(ctx.in[2]));
  } else {
    active_sink() = nullptr;
    throw std::runtime_error("glm: vector alpha unsupported");
  }
  active_sink() = nullptr;
  ctx.out.data[0] = s.value;
}
// Edge order (x, alpha, beta): X data (edge 0 skipped by null adjoint),
// alpha scalar, beta vector.
void bernoulli_logit_glm_bwd(KernelCtx& ctx) { density_bwd<3>(ctx); }

void uniform_fwd(KernelCtx& ctx) {
  // stan-math reports out-of-support y with an early `return LOG_ZERO`
  // that never reaches the partials sink, so the recorder would leave the
  // value at 0 and the point would silently count as in support (caught
  // by the dogs_log reference: CmdStan -inf, stanli finite). Handle
  // support here; the density call then only ever runs on points where
  // every stan-math path deposits through the sink.
  const auto at = [&](int k, int64_t n) {
    return ctx.in[k].data[ctx.in[k].len == 1 ? 0 : n];
  };
  const int64_t N =
      std::max(ctx.in[0].len, std::max(ctx.in[1].len, ctx.in[2].len));
  const bool elt = (ctx.variant & 0x40u) != 0;
  bool any_out = false;
  for (int64_t n = 0; n < N && !any_out; ++n)
    any_out = at(0, n) < at(1, n) || at(0, n) > at(2, n);
  if (any_out && !elt) {
    ctx.out.data[0] = stan::math::LOG_ZERO;
    const int64_t plen = ctx.in[0].len + ctx.in[1].len + ctx.in[2].len;
    std::fill(ctx.scratch, ctx.scratch + plen, 0.0);
    return;
  }
  density_fwd_v<3, 3>(
      ctx, [](const auto&... a) { stan::math::uniform_lpdf<true>(a...); },
      [](const auto&... a) { stan::math::uniform_lpdf<false>(a...); });
  if (any_out && elt) {
    // Elementwise variant: only the offending lanes are LOG_ZERO, and
    // their partials contribute nothing.
    const int64_t M = ctx.out.len;
    for (int64_t n = 0; n < M; ++n)
      if (at(0, n) < at(1, n) || at(0, n) > at(2, n)) {
        ctx.out.data[n] = stan::math::LOG_ZERO;
        for (int k = 0; k < 3; ++k)
          ctx.scratch[static_cast<int64_t>(k) * M + n] = 0.0;
      }
  }
}

// One line per density in STANLI_SCALAR_DENSITY_LIST (optable.hpp) makes
// the forward, and the registration below makes the rest. Everything they
// share -- propto and per-argument activity from the variant byte,
// elementwise output, the partials stashed for density_bwd to contract --
// lives in density_fwd_v.
#define STANLI_DEFINE_DENSITY_FWD(code, fn, n, tier)                             \
  void fn##_fwd_gen(KernelCtx& ctx) {                                      \
    density_fwd_v<n, tier>(                                                      \
        ctx, [](const auto&... a) { stan::math::fn<true>(a...); },         \
        [](const auto&... a) { stan::math::fn<false>(a...); });            \
  }
STANLI_SCALAR_DENSITY_LIST(STANLI_DEFINE_DENSITY_FWD)
#undef STANLI_DEFINE_DENSITY_FWD

// Distribution functions: one form, no propto, and no elementwise
// variant either. reroll.cpp only ever fuses opcodes it opts in by name,
// and cdfs are not among them, so variant bit 6 cannot arrive -- writing
// this separately from density_fwd_v is what keeps the per-lane binding
// from being instantiated for 75 functions that will never use it. It is
// not merely wasted code: some cdfs do not survive being handed a bare
// scalar rvar where their vector overload expects a container.
template <int NArgs, int Tier, typename F>
void cdf_fwd(KernelCtx& ctx, F&& f) {
  // Cannot happen today -- reroll.cpp opts densities into the fusion by
  // opcode and cdfs are in neither list -- but the failure if it ever did
  // would be silent: out would be len N and only element 0 written. Say
  // so instead. The branch costs nothing next to an incomplete beta.
  if (ctx.variant & 0x40u)
    throw std::runtime_error("cdf: elementwise variant not implemented");
  sink s;
  int64_t off = 0;
  for (int k = 0; k < NArgs; ++k) {
    s.buf[k] = ctx.scratch + off;
    off += ctx.in[k].len;
  }
  const unsigned mask = ctx.variant == 0 ? (1u << NArgs) - 1
                                         : (ctx.variant & 0x3fu);
  active_sink() = &s;
  full_form<NArgs, Tier>(mask, ctx, f);
  active_sink() = nullptr;
  ctx.out.data[0] = s.value;
}

#define STANLI_DEFINE_CDF_FWD(code, fn, n, tier)                         \
  void fn##_fwd_gen(KernelCtx& ctx) {                                    \
    cdf_fwd<n, density_tier(tier) & STANLI_DENSITY_FULL_MASKS>(          \
        ctx, [](const auto&... a) { stan::math::fn(a...); });            \
  }
STANLI_SCALAR_CDF_LIST(STANLI_DEFINE_CDF_FWD)
#undef STANLI_DEFINE_CDF_FWD

// Integer-outcome cdfs. Same as above with the count read from idata,
// the way the lpmfs read theirs -- a whole vector of outcomes in one
// call, since the summed form is the only one these have.
#define STANLI_DEFINE_INT_CDF_FWD(code, fn, nreal, tier)                 \
  void fn##_fwd_gen(KernelCtx& ctx) {                                    \
    Eigen::Map<const Eigen::VectorXi> y(                                 \
        ctx.idata, static_cast<Eigen::Index>(ctx.n_idata));              \
    cdf_fwd<nreal, density_tier(tier) & STANLI_DENSITY_FULL_MASKS>(      \
        ctx, [&](const auto&... a) { stan::math::fn(y, a...); });        \
  }
STANLI_INT_CDF_LIST(STANLI_DEFINE_INT_CDF_FWD)
#undef STANLI_DEFINE_INT_CDF_FWD

}  // namespace

void register_density_kernels() {
#define STANLI_REGISTER_DENSITY(code, fn, n, tier) \
  register_kernel(code, Kernel{fn##_fwd_gen, density_bwd<n>, density_scratch<n>});
  STANLI_SCALAR_DENSITY_LIST(STANLI_REGISTER_DENSITY)
#undef STANLI_REGISTER_DENSITY
  // Discrete and ordered densities: the outcome sits in idata, so the
  // real-argument count is what the shared backward contracts.
#define STANLI_REGISTER_INT_DENSITY(code, fn, nreal, tier) \
  register_kernel(code,                                    \
                  Kernel{fn##_fwd_gen, density_bwd<nreal>, density_scratch<nreal>});
  STANLI_INT_DENSITY_LIST(STANLI_REGISTER_INT_DENSITY)
#undef STANLI_REGISTER_INT_DENSITY
#define STANLI_REGISTER_CDF(code, fn, n, tier) \
  register_kernel(code, Kernel{fn##_fwd_gen, density_bwd<n>, density_scratch<n>});
  STANLI_SCALAR_CDF_LIST(STANLI_REGISTER_CDF)
  STANLI_INT_CDF_LIST(STANLI_REGISTER_CDF)
#undef STANLI_REGISTER_CDF
  // The list is the default. A density whose forward needs more than the
  // shared one registers after it and wins: uniform_lpdf has to decide
  // support itself, because stan-math returns LOG_ZERO out of support
  // through an early return that never reaches the partials sink, and the
  // recorder would leave the value at 0 (CmdStan -inf, stanli finite --
  // caught by the dogs_log reference).
  register_kernel(OP_UNIFORM_LPDF,
                  Kernel{uniform_fwd, density_bwd<3>, density_scratch<3>});
  register_kernel(OP_POISSON_LOG_LPMF,
                  Kernel{poisson_log_fwd, density_bwd<1>, density_scratch<1>});
  register_kernel(OP_BERNOULLI_LOGIT_LPMF,
                  Kernel{bernoulli_logit_fwd, density_bwd<1>, density_scratch<1>});
  register_kernel(OP_BERNOULLI_LPMF,
                  Kernel{bernoulli_fwd, density_bwd<1>, density_scratch<1>});
  register_kernel(OP_POISSON_LPMF,
                  Kernel{poisson_fwd, density_bwd<1>, density_scratch<1>});
  register_kernel(OP_NEG_BINOMIAL_2_LPMF,
                  Kernel{neg_binomial_2_fwd, density_bwd<2>, density_scratch<2>});
  register_kernel(OP_BINOMIAL_LPMF,
                  Kernel{binomial_fwd, density_bwd<1>, density_scratch<1>});
  register_kernel(OP_BINOMIAL_LOGIT_LPMF,
                  Kernel{binomial_logit_fwd, density_bwd<1>, density_scratch<1>});
  register_kernel(OP_BERNOULLI_LOGIT_GLM_LPMF,
                  Kernel{bernoulli_logit_glm_fwd, bernoulli_logit_glm_bwd,
                         sum_in_lens});
}

}  // namespace stanli
