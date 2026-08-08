// The integer-outcome densities: the hand-written lpmfs, the ones
// generated from STANLI_INT_DENSITY_LIST, the binomials with their two
// int groups, beta_binomial, and the ordered pair.
//
// One of the density shards: see densities_impl.hpp for why they
// are split and what they share.
#include "densities_impl.hpp"

namespace stanli {
namespace dens {

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


// beta_binomial(n | N, alpha, beta): two integer groups, then two real
// arguments that behave like any other density's. with_int_group unpacks
// the [len, vals...] pairs the lowering wrote, exactly as binomial does.
void beta_binomial_fwd(KernelCtx& ctx) {
  with_int_group(ctx.idata, [&](const auto& n, const int* rest) {
    with_int_group(rest, [&](const auto& N, const int*) {
      density_fwd_sum<2, density_tier(2), 0>(
          ctx,
          [&](const auto&... a) {
            stan::math::beta_binomial_lpmf<true>(n, N, a...);
          },
          [&](const auto&... a) {
            stan::math::beta_binomial_lpmf<false>(n, N, a...);
          });
    });
  });
}

// is n ints per evaluation, against a density over the same n.
STANLI_ORDERED_DENSITY_LIST(STANLI_DEFINE_ORDERED_FWD)

}  // namespace dens
}  // namespace stanli
