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

#include <cmath>
#include <type_traits>
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

// A data argument binds as double, not a promoted var: stan-math's mixed
// overloads are what CmdStan's generated code calls when one side is
// data, and for the selection functions the distinction is observable --
// fmax's var,double overload gives ties to the var, its var,var overload
// gives them to b. Whether a side is data comes from the adjoint slot:
// no in_adj means nothing upstream ever propagates to it.
template <bool A0, bool A1, typename F>
void binary_bwd_typed(KernelCtx& ctx, F&& f) {
  using T0 = std::conditional_t<A0, var, double>;
  using T1 = std::conditional_t<A1, var, double>;
  const bool s0 = ctx.in[0].len == 1, s1 = ctx.in[1].len == 1;
  const int64_t n = ctx.out.len;
  stan::math::nested_rev_autodiff nested;
  // A broadcast scalar is ONE var shared across lanes, so per-lane
  // partials accumulate into a single adjoint, exactly as stan-math's
  // scalar-vs-vector overloads accumulate theirs.
  std::vector<T0> a;
  std::vector<T1> b;
  VecVar y;
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
  if constexpr (A0) {
    for (size_t i = 0; i < a.size(); ++i) ctx.in_adj[0].data[i] += a[i].adj();
  }
  if constexpr (A1) {
    for (size_t i = 0; i < b.size(); ++i) ctx.in_adj[1].data[i] += b[i].adj();
  }
}

template <typename F>
void binary_bwd(KernelCtx& ctx, F&& f) {
  const bool a0 = ctx.in_adj[0].data != nullptr;
  const bool a1 = ctx.in_adj[1].data != nullptr;
  if (a0 && a1) {
    binary_bwd_typed<true, true>(ctx, f);
  } else if (a0) {
    binary_bwd_typed<true, false>(ctx, f);
  } else if (a1) {
    binary_bwd_typed<false, true>(ctx, f);
  }
}

#define STANLI_DEFINE_BINARY(code, name, fn)                                  \
  void name##_2fwd(KernelCtx& ctx) {                                          \
    binary_fwd(ctx, [](double x, double z) { return stan::math::fn(x, z); }); \
  }                                                                           \
  void name##_2bwd(KernelCtx& ctx) {                                          \
    /* Generic on purpose: each side is var or double per the dispatch     */ \
    /* above, so stan-math picks the same overload CmdStan's C++ would.    */ \
    binary_bwd(ctx, [](const auto& x, const auto& z) -> var {                 \
      return stan::math::fn(x, z);                                            \
    });                                                                       \
  }
STANLI_SCALAR_BINARY_LIST(STANLI_DEFINE_BINARY)
#undef STANLI_DEFINE_BINARY

// An int argument arrives in an ordinary double slot, holding the exact
// integer it was declared with. Rounding rather than truncating keeps a
// value that took a detour through arithmetic (a loop bound, a size)
// landing on the integer it names.
int as_int(double v) { return static_cast<int>(std::llround(v)); }

// Which lane of the int argument pairs with output lane i.
//
// Flat order agrees between the two sides everywhere but one place: an
// int array's last two extents are row-major, a matrix's are
// column-major. stan-math's apply_scalar_binary walks the matrix a row at
// a time against the array's outer index, so `ldexp(m, n)` on a 2x2
// matrix and an array[2, 2] int pairs n[i][j] with m(i, j) -- the
// off-diagonals would swap under a flat pairing. The lowering passes rows
// and cols in idata for exactly that combination; everywhere else the
// orders coincide and there is nothing to undo.
struct IntLane {
  bool broadcast = false;
  int64_t rows = 0, cols = 0;  // 0 when the flat orders already agree

  static IntLane of(const KernelCtx& ctx, const Desc& ints) {
    IntLane lane;
    lane.broadcast = ints.len == 1;
    if (ctx.n_idata == 2) {
      lane.rows = ctx.idata[0];
      lane.cols = ctx.idata[1];
    }
    return lane;
  }
  int64_t operator()(int64_t i) const {
    if (broadcast) return 0;
    if (rows == 0) return i;
    const int64_t elem = rows * cols, block = i / elem, off = i % elem;
    return block * elem + (off % rows) * cols + off / rows;
  }
};

// The int side never carries an adjoint -- Stan integers are data -- so
// unlike binary_bwd there is nothing to dispatch on: one nested tape, one
// var per real lane. Lanes are created ascending for the same reason the
// var,var kernel does it.
template <bool IntFirst, typename F>
void binary_int_bwd(KernelCtx& ctx, F&& f) {
  const Desc& re = ctx.in[IntFirst ? 1 : 0];
  const Desc& iv = ctx.in[IntFirst ? 0 : 1];
  double* re_adj = ctx.in_adj[IntFirst ? 1 : 0].data;
  if (!re_adj) return;
  const IntLane lane = IntLane::of(ctx, iv);
  const bool rs = re.len == 1;
  const int64_t n = ctx.out.len;
  stan::math::nested_rev_autodiff nested;
  VecVar a, y;
  a.reserve(rs ? 1 : n);
  y.reserve(n);
  if (rs) a.emplace_back(re.data[0]);
  for (int64_t i = 0; i < n; ++i) {
    if (!rs) a.emplace_back(re.data[i]);
    y.push_back(f(a[rs ? 0 : i], as_int(iv.data[lane(i)])));
  }
  var seeded = 0.0;
  const double* dout = n == 1 ? &ctx.out_adj : ctx.out_adj_vec.data;
  for (int64_t i = 0; i < n; ++i) seeded += y[i] * dout[i];
  stan::math::grad(seeded.vi_);
  for (size_t i = 0; i < a.size(); ++i) re_adj[i] += a[i].adj();
}

template <bool IntFirst, typename F>
void binary_int_fwd(KernelCtx& ctx, F&& f) {
  const Desc& re = ctx.in[IntFirst ? 1 : 0];
  const Desc& iv = ctx.in[IntFirst ? 0 : 1];
  const IntLane lane = IntLane::of(ctx, iv);
  const bool rs = re.len == 1;
  for (int64_t i = 0; i < ctx.out.len; ++i)
    ctx.out.data[i] = f(re.data[rs ? 0 : i], as_int(iv.data[lane(i)]));
}

// Both macros hand their lambda (real, int) and put the two back in the
// function's own order, so the kernel templates stay ignorant of it.
#define STANLI_DEFINE_BINARY_INT_FIRST(code, name, fn)                  \
  void name##_2fwd(KernelCtx& ctx) {                                    \
    binary_int_fwd<true>(                                               \
        ctx, [](double x, int k) { return stan::math::fn(k, x); });     \
  }                                                                     \
  void name##_2bwd(KernelCtx& ctx) {                                    \
    binary_int_bwd<true>(                                               \
        ctx, [](const var& x, int k) { return stan::math::fn(k, x); }); \
  }
STANLI_SCALAR_BINARY_INT_FIRST_LIST(STANLI_DEFINE_BINARY_INT_FIRST)
#undef STANLI_DEFINE_BINARY_INT_FIRST

#define STANLI_DEFINE_BINARY_INT_SECOND(code, name, fn)                 \
  void name##_2fwd(KernelCtx& ctx) {                                    \
    binary_int_fwd<false>(                                              \
        ctx, [](double x, int k) { return stan::math::fn(x, k); });     \
  }                                                                     \
  void name##_2bwd(KernelCtx& ctx) {                                    \
    binary_int_bwd<false>(                                              \
        ctx, [](const var& x, int k) { return stan::math::fn(x, k); }); \
  }
STANLI_SCALAR_BINARY_INT_SECOND_LIST(STANLI_DEFINE_BINARY_INT_SECOND)
#undef STANLI_DEFINE_BINARY_INT_SECOND

}  // namespace

void register_scalar_binary_kernels() {
#define STANLI_REGISTER_BINARY(code, name, fn) \
  register_kernel(code, Kernel{name##_2fwd, name##_2bwd, nullptr});
  STANLI_SCALAR_BINARY_LIST(STANLI_REGISTER_BINARY)
  STANLI_SCALAR_BINARY_INT_FIRST_LIST(STANLI_REGISTER_BINARY)
  STANLI_SCALAR_BINARY_INT_SECOND_LIST(STANLI_REGISTER_BINARY)
#undef STANLI_REGISTER_BINARY
}

}  // namespace stanli
