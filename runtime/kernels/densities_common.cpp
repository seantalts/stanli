// Half the densities models lean on, plus the hand-written kernels
// (two int groups, a data matrix, a support test) that predate the
// lists. The other half is densities_common_b.cpp.
//
// One of the density shards: see densities_impl.hpp for why they
// are split and what they share.
#include "densities_impl.hpp"

namespace stanli {
namespace dens {

STANLI_SCALAR_DENSITY_LIST_COMMON_A(STANLI_DEFINE_DENSITY_FWD)


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
  // The list already generates exactly this call in densities_common_b.cpp.
  // Writing it out again here would instantiate the whole three-argument
  // family a second time (the lambdas are distinct closure types), so call
  // the generated one; densities.cpp registers this wrapper over it.
  uniform_lpdf_fwd_gen(ctx);
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

}  // namespace dens
}  // namespace stanli
