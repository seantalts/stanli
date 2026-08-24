// Scalar generated-quantities draws on the compiled write_array graph.
//
// OP_RNG is deliberately one effectful opcode. Its variant names the family;
// every argument and the result are scalar doubles in graph storage (Stan int
// draws are represented exactly there too). The stream is evaluation state,
// not graph/model state, so callers can interleave independent chains through
// one compiled model without sharing or resetting a stream.
#include <stanli/graph.hpp>
#include <stanli/optable.hpp>
#include <stanli/wa_interp.hpp>

#include <cstddef>
#include <stdexcept>

namespace stanli {
namespace {

void rng_fwd(KernelCtx& ctx) {
  if (ctx.out.len != 1 ||
      ctx.variant > static_cast<uint8_t>(ScalarRng::Lognormal))
    throw std::logic_error("malformed scalar RNG op");
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
