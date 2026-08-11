// reject() and print(): the two statements whose whole purpose is a side
// effect, so they are ops rather than anything the graph optimizes.
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

}  // namespace

void register_message_kernels() {
  register_kernel(OP_REJECT, Kernel{reject_fwd, nullptr, nullptr});
  register_kernel(OP_PRINT, Kernel{print_fwd, nullptr, nullptr});
}

}  // namespace stanli
