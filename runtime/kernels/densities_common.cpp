// The densities models actually lean on, and the hand-written lpmfs.
//
// One of the density shards: see densities_impl.hpp for why they
// are split and what they share.
#include "densities_impl.hpp"

namespace stanli {
namespace dens {

STANLI_SCALAR_DENSITY_LIST_COMMON(STANLI_DEFINE_DENSITY_FWD)

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


// Ordinal regression. The outcome goes over as a std::vector<int>, not
// the Eigen map the other lpmfs use: ordered_logistic hands its outcome
// to scalar_seq_view and then asks for data(), whose non-const overload
// wants a mutable pointer that a Map<const VectorXi> cannot supply. A
// std::vector is also exactly what CmdStan's generated code passes, so
// this is the instantiation the references were produced from. The copy
// is n ints per evaluation, against a density over the same n.
STANLI_ORDERED_DENSITY_LIST(STANLI_DEFINE_ORDERED_FWD)

}  // namespace dens
}  // namespace stanli
