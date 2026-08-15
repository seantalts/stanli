// Two-argument scalar math through stan-math's own var overloads, from
// STANLI_SCALAR_BINARY_LIST (optable.hpp). One nested tape per backward
// call; the forward is the prim double call, which is the identical
// computation the var overload's value constructor runs.
//
// Elementwise with scalar broadcast, like the arithmetic binaries in
// eltwise_expr.cpp. Lanes are created ascending, so the reverse sweep
// fires their callbacks descending -- the order the per-element varis of
// stan-math's apply_scalar_binary fire in CmdStan's generated C++, which
// is what makes a broadcast scalar's adjoint accumulate in the same
// order there and here.
#include <stanli/graph.hpp>
#include <stanli/optable.hpp>

#include <stan/math.hpp>

#include <vector>

namespace stanli {
namespace {

using stan::math::var;
using VecVar = std::vector<var>;

template <typename F>
void binary_fwd(KernelCtx& ctx, F&& f) {
  const bool s0 = ctx.in[0].len == 1, s1 = ctx.in[1].len == 1;
  for (int64_t i = 0; i < ctx.out.len; ++i)
    ctx.out.data[i] = f(ctx.in[0].data[s0 ? 0 : i], ctx.in[1].data[s1 ? 0 : i]);
}

template <typename F>
void binary_bwd(KernelCtx& ctx, F&& f) {
  const bool s0 = ctx.in[0].len == 1, s1 = ctx.in[1].len == 1;
  const int64_t n = ctx.out.len;
  stan::math::nested_rev_autodiff nested;
  // A broadcast scalar is ONE var shared across lanes, so per-lane
  // partials accumulate into a single adjoint, exactly as stan-math's
  // scalar-vs-vector overloads accumulate theirs.
  VecVar a, b, y;
  a.reserve(s0 ? 1 : n);
  b.reserve(s1 ? 1 : n);
  y.reserve(n);
  if (s0) a.emplace_back(ctx.in[0].data[0]);
  if (s1) b.emplace_back(ctx.in[1].data[0]);
  for (int64_t i = 0; i < n; ++i) {
    if (!s0) a.emplace_back(ctx.in[0].data[i]);
    if (!s1) b.emplace_back(ctx.in[1].data[i]);
    y.push_back(f(a[s0 ? 0 : i], b[s1 ? 0 : i]));
  }
  // Seed each lane with its upstream adjoint. The products and the sum
  // deposit exactly dout[i] into y[i]'s adjoint (their callbacks multiply
  // by an adjoint of 1.0), then the lane callbacks run.
  var seeded = 0.0;
  const double* dout = n == 1 ? &ctx.out_adj : ctx.out_adj_vec.data;
  for (int64_t i = 0; i < n; ++i) seeded += y[i] * dout[i];
  stan::math::grad(seeded.vi_);
  if (ctx.in_adj[0].data)
    for (size_t i = 0; i < a.size(); ++i) ctx.in_adj[0].data[i] += a[i].adj();
  if (ctx.in_adj[1].data)
    for (size_t i = 0; i < b.size(); ++i) ctx.in_adj[1].data[i] += b[i].adj();
}

#define STANLI_DEFINE_BINARY(code, name, fn)                                   \
  void name##_2fwd(KernelCtx& ctx) {                                           \
    binary_fwd(ctx, [](double x, double z) { return stan::math::fn(x, z); });  \
  }                                                                            \
  void name##_2bwd(KernelCtx& ctx) {                                           \
    binary_bwd(                                                                \
        ctx, [](const var& x, const var& z) { return stan::math::fn(x, z); }); \
  }
STANLI_SCALAR_BINARY_LIST(STANLI_DEFINE_BINARY)
#undef STANLI_DEFINE_BINARY

}  // namespace

void register_scalar_binary_kernels() {
#define STANLI_REGISTER_BINARY(code, name, fn) \
  register_kernel(code, Kernel{name##_2fwd, name##_2bwd, nullptr});
  STANLI_SCALAR_BINARY_LIST(STANLI_REGISTER_BINARY)
#undef STANLI_REGISTER_BINARY
}

}  // namespace stanli
