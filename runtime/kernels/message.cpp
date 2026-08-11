// Runtime checks, reject(), and print(): statements whose whole purpose is a
// side effect, so they are ops rather than anything the graph optimizes.
//
// `reject` has real semantics. It throws std::domain_error, which the
// sampler treats as a rejected proposal exactly as CmdStan's generated
// code does -- the same exception type, from the same place in the
// evaluation. `print` has none; it hands the line to the message sink
// (stdout unless the host installed something else) and returns.
//
// Neither has a backward. `reject` never reaches one (the forward threw)
// and `print` contributes nothing to the target, so the executor's
// reverse sweep skips both.
//
// The message is a template: a list of literal chunks interleaved with
// the runtime values of the op's inputs, assembled at forward time
// because that is the only time the values exist. CmdStan formats a
// vector as `[1,2,3]` and a scalar bare, and so does this.
#include <stanli/graph.hpp>
#include <stanli/message_sink.hpp>
#include <stanli/optable.hpp>

#include <stan/math/prim/err/check_greater_or_equal.hpp>
#include <stan/math/prim/err/check_less_or_equal.hpp>

#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace stanli {

namespace {

// Build the message: chunk 0, then input 0, then chunk 1, then input 1,
// ... Trailing chunks with no matching input are appended as-is, which is
// what a call ending in a string literal produces.
std::string render(const KernelCtx& ctx) {
  const auto* msg = static_cast<const MessageSpec*>(ctx.udata);
  std::ostringstream out;
  for (size_t k = 0; k < msg->chunks.size(); ++k) {
    out << msg->chunks[k];
    if ((int)k >= ctx.n_in) continue;
    const auto& in = ctx.in[k];
    // CmdStan prints a container in brackets and a scalar bare.
    if (in.len == 1) {
      out << in.data[0];
    } else {
      out << '[';
      for (int64_t i = 0; i < in.len; ++i) {
        if (i) out << ',';
        out << in.data[i];
      }
      out << ']';
    }
  }
  return out.str();
}

void reject_fwd(KernelCtx& ctx) {
  // std::domain_error, not a stanli-specific type: this is the exception
  // stan-math's own reject throws, and it is what the executor's callers
  // and the sampler already treat as "this draw is not valid" rather than
  // as a failure of the run.
  throw std::domain_error(render(ctx));
}

void print_fwd(KernelCtx& ctx) { emit_message(render(ctx)); }

void check_matching_dims_fwd(KernelCtx& ctx) {
  if (ctx.n_in != 2 || ctx.out.len != 1 || ctx.udata == nullptr)
    throw std::logic_error("malformed dimension-check op");
  const auto* spec = static_cast<const BoundCheckSpec*>(ctx.udata);
  if (!spec->shapes_match)
    throw std::invalid_argument(
        "stanli MIR check: constraint shapes do not match for " + spec->name);
  ctx.out.data[0] = 0.0;
}

template <bool Lower>
void check_fwd(KernelCtx& ctx) {
  if (ctx.n_in != 2 || ctx.out.len != 1 || ctx.udata == nullptr)
    throw std::logic_error("malformed bound-check op");
  const Desc& value = ctx.in[0];
  const Desc& bound = ctx.in[1];
  const auto* spec = static_cast<const BoundCheckSpec*>(ctx.udata);
  if (!spec->shapes_match)
    throw std::invalid_argument(
        "stanli MIR check: constraint shapes do not "
        "match for " +
        spec->name);
  if ((spec->bound_is_scalar && bound.len != 1) ||
      (!spec->bound_is_scalar && bound.len != value.len))
    throw std::logic_error("bound-check shape metadata is inconsistent");
  for (int64_t i = 0; i < value.len; ++i) {
    const double b = bound.data[spec->bound_is_scalar ? 0 : i];
    if constexpr (Lower)
      stan::math::check_greater_or_equal("stanli MIR check", spec->name.c_str(),
                                         value.data[i], b);
    else
      stan::math::check_less_or_equal("stanli MIR check", spec->name.c_str(),
                                      value.data[i], b);
  }
  ctx.out.data[0] = 0.0;
}

}  // namespace

void register_message_kernels() {
  register_kernel(OP_CHECK_MATCHING_DIMS,
                  Kernel{check_matching_dims_fwd, nullptr, nullptr});
  register_kernel(OP_CHECK_LOWER, Kernel{check_fwd<true>, nullptr, nullptr});
  register_kernel(OP_CHECK_UPPER, Kernel{check_fwd<false>, nullptr, nullptr});
  register_kernel(OP_REJECT, Kernel{reject_fwd, nullptr, nullptr});
  register_kernel(OP_PRINT, Kernel{print_fwd, nullptr, nullptr});
}

}  // namespace stanli
