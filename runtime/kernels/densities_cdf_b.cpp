// Distribution functions, second half, and the integer ones.
//
// One of the density shards: see densities_impl.hpp for why they
// are split and what they share.
#include "densities_impl.hpp"

namespace stanli {
namespace dens {

STANLI_SCALAR_CDF_LIST_B(STANLI_DEFINE_CDF_FWD)

// Math 5.4 expresses these tails as lcdf(-y, -mu, sigma). Reflect
// plain inputs before recording and apply the chain rule to the recorded
// partials; rvar deliberately has no arithmetic or hidden autodiff tape.
template <int NArgs, int Reflected, typename F>
void reflected_cdf(KernelCtx& ctx, F&& f) {
  KernelCtx reflected = ctx;
  std::vector<double> values[Reflected];
  for (int k = 0; k < Reflected; ++k) {
    values[k].reserve(ctx.in[k].len);
    for (int64_t i = 0; i < ctx.in[k].len; ++i)
      values[k].push_back(-ctx.in[k].data[i]);
    reflected.in[k].data = values[k].data();
  }
  cdf_fwd<NArgs, 0>(reflected, std::forward<F>(f));
  int64_t offset = 0;
  for (int k = 0; k < Reflected; ++k)
    for (int64_t i = 0; i < ctx.in[k].len; ++i, ++offset)
      ctx.scratch[offset] = -ctx.scratch[offset];
}

void normal_lccdf_fwd_gen(KernelCtx& ctx) {
  reflected_cdf<3, 2>(
      ctx, [](const auto& y, const auto& mu, const auto& sigma) {
        return stan::math::normal_lcdf<stan::math::internal::normal_lccdf_func>(
            y, mu, sigma);
      });
}

void std_normal_lccdf_fwd_gen(KernelCtx& ctx) {
  reflected_cdf<1, 1>(ctx, [](const auto& y) {
    return stan::math::std_normal_lcdf<
        stan::math::internal::std_normal_lccdf_func>(y);
  });
}

STANLI_INT_CDF_LIST(STANLI_DEFINE_INT_CDF_FWD)

}  // namespace dens
}  // namespace stanli
