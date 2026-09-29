// Scalar generated-quantities draws on the compiled write_array graph.
//
// OP_RNG is deliberately one effectful opcode. Its variant names the family;
// the ScalarRng range takes scalar-double arguments (one int-typed, cast in
// scalar_rng_draw, for the two families whose first argument is a
// population count); categorical, categorical-logit and Poisson-binomial
// take one vector slot and return one integer.
// Multi-normal takes a mean plus a square covariance or Cholesky factor and
// produces a
// vector, and dirichlet takes one concentration vector and produces a
// same-length simplex draw. The stream is evaluation state, not graph/model
// state, so callers can interleave independent chains through one compiled
// model without sharing or resetting a stream.
#include <stanli/graph.hpp>
#include <stanli/optable.hpp>
#include <stanli/wa_interp.hpp>

#include <cstddef>
#include <stdexcept>

namespace stanli {
namespace {

void rng_fwd(KernelCtx& ctx) {
  if (ctx.variant == kCategoricalRngVariant ||
      ctx.variant == kCategoricalLogitRngVariant ||
      ctx.variant == kPoissonBinomialRngVariant) {
    if (ctx.out.len != 1 || ctx.n_in != 1 || ctx.in[0].len < 0)
      throw std::logic_error("malformed categorical RNG op");
    if (ctx.eval_state == nullptr || ctx.eval_state->wa_rng == nullptr)
      throw std::logic_error(
          "OP_RNG requires caller-owned evaluation RNG state");
    ctx.out.data[0] = static_cast<double>(vector_integer_rng_draw(
        ctx.in[0].data, static_cast<size_t>(ctx.in[0].len),
        *ctx.eval_state->wa_rng, ctx.variant));
    return;
  }
  if (ctx.variant == kMultiNormalRngVariant ||
      ctx.variant == kMultiNormalCholeskyRngVariant) {
    if (ctx.n_in != 2 || ctx.n_idata != 1 || ctx.idata == nullptr ||
        ctx.idata[0] < 0)
      throw std::logic_error("malformed multi-normal RNG op");
    const int64_t k = ctx.idata[0];
    if (ctx.in[0].len != k || ctx.in[1].len != k * k || ctx.out.len != k)
      throw std::logic_error("malformed multi-normal RNG op");
    if (ctx.eval_state == nullptr || ctx.eval_state->wa_rng == nullptr)
      throw std::logic_error(
          "OP_RNG requires caller-owned evaluation RNG state");
    multi_normal_rng_draw(
        ctx.in[0].data, static_cast<size_t>(ctx.in[0].len), ctx.in[1].data,
        static_cast<size_t>(ctx.in[1].len), static_cast<size_t>(k),
        static_cast<size_t>(k), ctx.out.data, static_cast<size_t>(ctx.out.len),
        *ctx.eval_state->wa_rng, ctx.variant == kMultiNormalCholeskyRngVariant);
    return;
  }
  if (ctx.variant == kDirichletRngVariant) {
    if (ctx.n_in != 1 || ctx.in[0].len < 0 || ctx.out.len != ctx.in[0].len)
      throw std::logic_error("malformed dirichlet RNG op");
    if (ctx.eval_state == nullptr || ctx.eval_state->wa_rng == nullptr)
      throw std::logic_error(
          "OP_RNG requires caller-owned evaluation RNG state");
    dirichlet_rng_draw(ctx.in[0].data, static_cast<size_t>(ctx.in[0].len),
                       ctx.out.data, static_cast<size_t>(ctx.out.len),
                       *ctx.eval_state->wa_rng);
    return;
  }
  const ScalarRng family = static_cast<ScalarRng>(ctx.variant);
  const size_t nargs = scalar_rng_arity(family);
  if (ctx.n_in != static_cast<int>(nargs))
    throw std::logic_error("malformed scalar RNG op");
  if (ctx.n_idata != 0) {
    if (ctx.n_idata != 1 || ctx.idata == nullptr || ctx.idata[0] <= 0 ||
        ctx.idata[0] >= (1 << nargs) || ctx.out.len < 0)
      throw std::logic_error("malformed container RNG op");
    RngArgument args[3]{};
    for (size_t i = 0; i < nargs; ++i) {
      if (ctx.in[i].len < 0)
        throw std::logic_error("malformed container RNG argument");
      args[i] = {ctx.in[i].data, static_cast<size_t>(ctx.in[i].len),
                 (ctx.idata[0] & (1 << i)) == 0};
    }
    if (ctx.eval_state == nullptr || ctx.eval_state->wa_rng == nullptr)
      throw std::logic_error(
          "OP_RNG requires caller-owned evaluation RNG state");
    container_rng_draw(family, args, nargs, ctx.out.data,
                       static_cast<size_t>(ctx.out.len),
                       *ctx.eval_state->wa_rng);
    return;
  }
  if (ctx.out.len != 1) throw std::logic_error("malformed scalar RNG op");
  double args[3]{};
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
