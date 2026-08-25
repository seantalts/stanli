// Scalar generated-quantities draws on the compiled write_array graph.
//
// OP_RNG is deliberately one effectful opcode. Its variant names the family;
// variants 0..5 take scalar-double arguments, categorical takes one
// probability-vector slot, and multi-normal takes a mean vector plus a square
// covariance and produces a vector. The stream is evaluation state, not
// graph/model state, so callers can interleave independent chains through one
// compiled model without sharing or resetting a stream.
#include <stanli/graph.hpp>
#include <stanli/optable.hpp>
#include <stanli/wa_interp.hpp>

#include <cstddef>
#include <stdexcept>

namespace stanli {
namespace {

void rng_fwd(KernelCtx& ctx) {
  if (ctx.variant > static_cast<uint8_t>(ScalarRng::Binomial) &&
      ctx.variant != kCategoricalRngVariant &&
      ctx.variant != kMultiNormalRngVariant)
    throw std::logic_error("malformed RNG op");
  if (ctx.variant == kCategoricalRngVariant) {
    if (ctx.out.len != 1 || ctx.n_in != 1 || ctx.in[0].len < 0)
      throw std::logic_error("malformed categorical RNG op");
    if (ctx.eval_state == nullptr || ctx.eval_state->wa_rng == nullptr)
      throw std::logic_error(
          "OP_RNG requires caller-owned evaluation RNG state");
    ctx.out.data[0] = static_cast<double>(
        categorical_rng_draw(ctx.in[0].data, static_cast<size_t>(ctx.in[0].len),
                             *ctx.eval_state->wa_rng));
    return;
  }
  if (ctx.variant == kMultiNormalRngVariant) {
    if (ctx.n_in != 2 || ctx.n_idata != 1 || ctx.idata == nullptr ||
        ctx.idata[0] < 0)
      throw std::logic_error("malformed multi-normal RNG op");
    const int64_t k = ctx.idata[0];
    if (ctx.in[0].len != k || ctx.in[1].len != k * k || ctx.out.len != k)
      throw std::logic_error("malformed multi-normal RNG op");
    if (ctx.eval_state == nullptr || ctx.eval_state->wa_rng == nullptr)
      throw std::logic_error(
          "OP_RNG requires caller-owned evaluation RNG state");
    multi_normal_rng_draw(ctx.in[0].data, static_cast<size_t>(ctx.in[0].len),
                          ctx.in[1].data, static_cast<size_t>(ctx.in[1].len),
                          static_cast<size_t>(k), static_cast<size_t>(k),
                          ctx.out.data, static_cast<size_t>(ctx.out.len),
                          *ctx.eval_state->wa_rng);
    return;
  }
  if (ctx.out.len != 1) throw std::logic_error("malformed scalar RNG op");
  const ScalarRng family = static_cast<ScalarRng>(ctx.variant);
  const size_t nargs = scalar_rng_arity(family);
  if (ctx.n_in != static_cast<int>(nargs))
    throw std::logic_error("malformed scalar RNG op");
  double args[2]{};
  for (size_t i = 0; i < nargs; ++i) {
    if (ctx.in[i].len != 1)
      throw std::logic_error("scalar RNG received a container argument");
    args[i] = ctx.in[i].data[0];
  }
  if (ctx.eval_state == nullptr || ctx.eval_state->wa_rng == nullptr)
    throw std::logic_error("OP_RNG requires caller-owned evaluation RNG state");
  ctx.out.data[0] =
      scalar_rng_draw(family, args, nargs, *ctx.eval_state->wa_rng);
}

}  // namespace

void register_rng_kernel() {
  register_kernel(OP_RNG, Kernel{rng_fwd, nullptr, nullptr});
}

}  // namespace stanli
