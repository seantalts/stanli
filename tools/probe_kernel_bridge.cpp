// Experimental kernel boundary for the developer-only generated-Wasm probe.
// Not linked into the shipped runtime. Handles own metadata; callers own all
// value/adjoint buffers in the importing module's shared linear memory.
#include <stanli/optable.hpp>
#include <stdexcept>
#include <algorithm>

namespace {
struct Handle {
  stanli::KernelCtx ctx;
  const stanli::Kernel* kernel;
  int shape = 2;
  Handle(double* input, double* output, double* in_adj, double* out_adj)
      : kernel(stanli::find_kernel(stanli::OP_CHOLESKY)) {
    if (!kernel) throw std::runtime_error("missing Cholesky kernel");
    ctx.n_in = 1;
    ctx.in[0] = {input, 4};
    ctx.out = {output, 4};
    ctx.in_adj[0] = {in_adj, 4};
    ctx.out_adj_vec = {out_adj, 4};
    ctx.idata = &shape;
    ctx.n_idata = 1;
  }
};
}  // namespace
extern "C" {
void* probe_create(double* input, double* output, double* in_adj,
                   double* out_adj) {
  return new Handle(input, output, in_adj, out_adj);
}
void probe_forward(void* handle) {
  auto& h = *static_cast<Handle*>(handle);
  h.kernel->forward(h.ctx);
}
void probe_reverse(void* handle) {
  auto& h = *static_cast<Handle*>(handle);
  h.kernel->backward(h.ctx);
  std::fill_n(h.ctx.out_adj_vec.data, h.ctx.out_adj_vec.len, 0.0);
}
void probe_free(void* handle) { delete static_cast<Handle*>(handle); }
}
