// The one-argument functions whose derivative is not a formula, evaluated
// through stan-math's own var overload on a nested tape.
//
// Everything else with one argument lives in STANLI_SCALAR_UNARY_LIST
// (optable.hpp), where the pullback is written out and the backward is a
// plain loop. trigamma cannot join them: Math computes it with Schneider's
// AS121 -- a recurrence whose length depends on the argument, followed by a
// Laurent tail -- and differentiates that algorithm through the tape rather
// than evaluating a closed form. Transcribing its derivative would mean
// reproducing the tape's accumulation order term by term, which is the
// class of error the conformance sweep exists to catch, so the tape is what
// runs here instead. The forward is the prim double call, the identical
// computation the var overload's value constructor performs.
//
// The cost is real and it is the point: one nested tape per backward call,
// batched across all lanes of the op the way scalar_binary.cpp batches its
// own, against a straight loop for every other unary.
#include <stanli/graph.hpp>
#include <stanli/optable.hpp>

#include <stan/math.hpp>

#include <vector>

namespace stanli {
namespace {

using stan::math::var;

void trigamma_ufwd(KernelCtx& ctx) {
  for (int64_t i = 0; i < ctx.out.len; ++i)
    ctx.out.data[i] = stan::math::trigamma(ctx.in[0].data[i]);
}

void trigamma_ubwd(KernelCtx& ctx) {
  if (!ctx.in_adj[0].data) return;
  const int64_t n = ctx.out.len;
  stan::math::nested_rev_autodiff nested;
  std::vector<var> a;
  std::vector<var> y;
  a.reserve(n);
  y.reserve(n);
  for (int64_t i = 0; i < n; ++i) {
    a.emplace_back(ctx.in[0].data[i]);
    y.push_back(stan::math::trigamma(a[i]));
  }
  // Lanes are independent, so seeding each with its upstream adjoint and
  // taking one sweep gives each input the adjoint its own callback would
  // have deposited. The products deposit exactly dout[i] into y[i]'s
  // adjoint, which is what the elementwise pullbacks want.
  var seeded = 0.0;
  const double* dout = n == 1 ? &ctx.out_adj : ctx.out_adj_vec.data;
  for (int64_t i = 0; i < n; ++i) seeded += y[i] * dout[i];
  stan::math::grad(seeded.vi_);
  for (int64_t i = 0; i < n; ++i) ctx.in_adj[0].data[i] += a[i].adj();
}

}  // namespace

void register_scalar_unary_ad_kernels() {
  register_kernel(OP_TRIGAMMA, Kernel{trigamma_ufwd, trigamma_ubwd, nullptr});
}

}  // namespace stanli
