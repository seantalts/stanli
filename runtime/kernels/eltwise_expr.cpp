// Elementwise expression ops for lowered MIR arithmetic. Kernels mirror the
// stan-math REV overloads' forward Eigen expressions and their backward
// accumulation shapes (see constrain.cpp for why: packet math vs libm).
// Shape dispatch is runtime: len==1 broadcasts.
#include <stanli/graph.hpp>
#include <stanli/optable.hpp>
#include <stanli/packet.hpp>

#include <stan/math/prim.hpp>

#include <cmath>

namespace stanli {
namespace {

using Arr = Eigen::Array<double, -1, 1>;
using MapA = Eigen::Map<Arr>;
using CMapA = Eigen::Map<const Arr>;

inline CMapA in_a(const KernelCtx& ctx, int i) {
  return CMapA(ctx.in[i].data, ctx.in[i].len);
}
inline MapA out_a(KernelCtx& ctx) { return MapA(ctx.out.data, ctx.out.len); }
inline CMapA dout_a(const KernelCtx& ctx) {
  return CMapA(ctx.out_adj_vec.data, ctx.out_adj_vec.len);
}
inline MapA dx_a(KernelCtx& ctx, int i) {
  return MapA(ctx.in_adj[i].data, ctx.in_adj[i].len);
}
inline bool scal(const KernelCtx& ctx, int i) { return ctx.in[i].len == 1; }

// ---- binaries --------------------------------------------------------------
void add_fwd(KernelCtx& ctx) {
  if (scal(ctx, 0) && scal(ctx, 1))
    ctx.out.data[0] = ctx.in[0].data[0] + ctx.in[1].data[0];
  else if (scal(ctx, 1))
    out_a(ctx) = in_a(ctx, 0) + ctx.in[1].data[0];
  else if (scal(ctx, 0))
    out_a(ctx) = ctx.in[0].data[0] + in_a(ctx, 1);
  else
    out_a(ctx) = in_a(ctx, 0) + in_a(ctx, 1);
}
// Scalar-broadcast adjoints accumulate ascending, directly into the
// accumulator: the rev overloads' reverse callbacks loop coefficients in
// ascending order (measured against add(var, Matrix<var>); a local Eigen
// sum added once differs by 1 ULP).
void add_bwd(KernelCtx& ctx) {
  for (int k = 0; k < 2; ++k) {
    if (!ctx.in_adj[k].data) continue;
    if (scal(ctx, k)) {
      if (ctx.out.len == 1) {
        ctx.in_adj[k].data[0] += ctx.out_adj;
      } else {
        for (int64_t i = 0; i < ctx.out.len; ++i)
          ctx.in_adj[k].data[0] += ctx.out_adj_vec.data[i];
      }
    } else {
      dx_a(ctx, k) += dout_a(ctx);
    }
  }
}

void sub_fwd(KernelCtx& ctx) {
  if (scal(ctx, 0) && scal(ctx, 1))
    ctx.out.data[0] = ctx.in[0].data[0] - ctx.in[1].data[0];
  else if (scal(ctx, 1))
    out_a(ctx) = in_a(ctx, 0) - ctx.in[1].data[0];
  else if (scal(ctx, 0))
    out_a(ctx) = ctx.in[0].data[0] - in_a(ctx, 1);
  else
    out_a(ctx) = in_a(ctx, 0) - in_a(ctx, 1);
}
void sub_bwd(KernelCtx& ctx) {
  if (ctx.in_adj[0].data) {
    if (scal(ctx, 0)) {
      if (ctx.out.len == 1) {
        ctx.in_adj[0].data[0] += ctx.out_adj;
      } else {
        for (int64_t i = 0; i < ctx.out.len; ++i)
          ctx.in_adj[0].data[0] += ctx.out_adj_vec.data[i];
      }
    } else {
      dx_a(ctx, 0) += dout_a(ctx);
    }
  }
  if (ctx.in_adj[1].data) {
    if (scal(ctx, 1)) {
      if (ctx.out.len == 1) {
        ctx.in_adj[1].data[0] -= ctx.out_adj;
      } else {
        for (int64_t i = 0; i < ctx.out.len; ++i)
          ctx.in_adj[1].data[0] -= ctx.out_adj_vec.data[i];
      }
    } else {
      dx_a(ctx, 1) -= dout_a(ctx);
    }
  }
}

// fma(a, b, c) elementwise with scalar broadcast on any argument, FUSED
// (std::fma, one rounding) -- the arithmetic stanc3's --O1 partial
// evaluator asks for and what CmdStan computes for an explicit fma().
void fma_fwd(KernelCtx& ctx) {
  const auto v = [&](int k, int64_t i) {
    return ctx.in[k].data[ctx.in[k].len == 1 ? 0 : i];
  };
  for (int64_t i = 0; i < ctx.out.len; ++i)
    ctx.out.data[i] = std::fma(v(0, i), v(1, i), v(2, i));
}
void fma_bwd(KernelCtx& ctx) {
  const auto v = [&](int k, int64_t i) {
    return ctx.in[k].data[ctx.in[k].len == 1 ? 0 : i];
  };
  const auto adj = [&](int64_t i) {
    return ctx.out.len == 1 ? ctx.out_adj : ctx.out_adj_vec.data[i];
  };
  for (int k = 0; k < 2; ++k) {
    if (!ctx.in_adj[k].data) continue;
    const int other = 1 - k;
    for (int64_t i = 0; i < ctx.out.len; ++i)
      ctx.in_adj[k].data[ctx.in[k].len == 1 ? 0 : i] += adj(i) * v(other, i);
  }
  if (ctx.in_adj[2].data)
    for (int64_t i = 0; i < ctx.out.len; ++i)
      ctx.in_adj[2].data[ctx.in[2].len == 1 ? 0 : i] += adj(i);
}

void mul_fwd(KernelCtx& ctx) {
  if (scal(ctx, 0) && scal(ctx, 1))
    ctx.out.data[0] = ctx.in[0].data[0] * ctx.in[1].data[0];
  else if (scal(ctx, 1))
    out_a(ctx) = in_a(ctx, 0) * ctx.in[1].data[0];
  else if (scal(ctx, 0))
    out_a(ctx) = ctx.in[0].data[0] * in_a(ctx, 1);
  else
    out_a(ctx) = in_a(ctx, 0) * in_a(ctx, 1);  // elt_multiply
}
void mul_bwd(KernelCtx& ctx) {
  const bool s0 = scal(ctx, 0), s1 = scal(ctx, 1);
  if (ctx.out.len == 1) {
    if (ctx.in_adj[0].data)
      ctx.in_adj[0].data[0] += ctx.out_adj * ctx.in[1].data[0];
    if (ctx.in_adj[1].data)
      ctx.in_adj[1].data[0] += ctx.out_adj * ctx.in[0].data[0];
    return;
  }
  if (ctx.in_adj[0].data) {
    if (s0) {
      for (int64_t i = 0; i < ctx.out.len; ++i)
        ctx.in_adj[0].data[0] += ctx.out_adj_vec.data[i] * ctx.in[1].data[i];
    } else if (s1) {
      dx_a(ctx, 0) += dout_a(ctx) * ctx.in[1].data[0];
    } else {
      dx_a(ctx, 0) += dout_a(ctx) * in_a(ctx, 1);
    }
  }
  if (ctx.in_adj[1].data) {
    if (s1) {
      for (int64_t i = 0; i < ctx.out.len; ++i)
        ctx.in_adj[1].data[0] += ctx.out_adj_vec.data[i] * ctx.in[0].data[i];
    } else if (s0) {
      dx_a(ctx, 1) += dout_a(ctx) * ctx.in[0].data[0];
    } else {
      dx_a(ctx, 1) += dout_a(ctx) * in_a(ctx, 0);
    }
  }
}

void div_fwd(KernelCtx& ctx) {
  if (scal(ctx, 0) && scal(ctx, 1))
    ctx.out.data[0] = ctx.in[0].data[0] / ctx.in[1].data[0];
  else if (scal(ctx, 1))
    out_a(ctx) = in_a(ctx, 0) / ctx.in[1].data[0];
  else if (scal(ctx, 0))
    out_a(ctx) = ctx.in[0].data[0] / in_a(ctx, 1);
  else
    out_a(ctx) = in_a(ctx, 0) / in_a(ctx, 1);  // elt_divide
}
void div_bwd(KernelCtx& ctx) {
  const bool s0 = scal(ctx, 0), s1 = scal(ctx, 1);
  if (ctx.out.len == 1) {
    const double b = ctx.in[1].data[0];
    if (ctx.in_adj[0].data) ctx.in_adj[0].data[0] += ctx.out_adj / b;
    if (ctx.in_adj[1].data)
      ctx.in_adj[1].data[0] += -ctx.out_adj * ctx.out.data[0] / b;
    return;
  }
  CMapA out_v(ctx.out.data, ctx.out.len);
  if (!s0 && !s1) {
    // rev elt_divide: ret_div = dout/b; a.adj += ret_div;
    // b.adj -= out * ret_div (same grouping, same reuse).
    Arr ret_div = dout_a(ctx) / in_a(ctx, 1);
    if (ctx.in_adj[0].data) dx_a(ctx, 0) += ret_div;
    if (ctx.in_adj[1].data) dx_a(ctx, 1) -= out_v * ret_div;
    return;
  }
  if (ctx.in_adj[0].data) {
    if (s0) {
      for (int64_t i = 0; i < ctx.out.len; ++i)
        ctx.in_adj[0].data[0] += ctx.out_adj_vec.data[i] / ctx.in[1].data[i];
    } else {
      dx_a(ctx, 0) += dout_a(ctx) / ctx.in[1].data[0];
    }
  }
  if (ctx.in_adj[1].data) {
    if (s1) {
      for (int64_t i = 0; i < ctx.out.len; ++i)
        ctx.in_adj[1].data[0] +=
            -ctx.out_adj_vec.data[i] * ctx.out.data[i] / ctx.in[1].data[0];
    } else {
      dx_a(ctx, 1) += -dout_a(ctx) * out_v / in_a(ctx, 1);
    }
  }
}

// Scalar libm per element, like the transcendental unaries: stan-math's
// vectorized pow applies its scalar op elementwise, so packet math would
// break bitwise parity. Partials keep the scalar grouping (b*v/a and
// log(a)*v), which is what the ss case has always matched.
void pow_fwd(KernelCtx& ctx) {
  const bool s0 = scal(ctx, 0), s1 = scal(ctx, 1);
  for (int64_t i = 0; i < ctx.out.len; ++i)
    ctx.out.data[i] =
        std::pow(ctx.in[0].data[s0 ? 0 : i], ctx.in[1].data[s1 ? 0 : i]);
}
// A zero base contributes nothing to either partial. Every one of
// stan-math's four rev pow overloads says so -- the scalar and the
// (scalar base, matrix exponent) ones return early on
// `value_of(base) == 0.0`, the two matrix ones `select` on
// `value_of(base) != 0.0` -- and both partials would otherwise be
// nonfinite there: b*v/a is 0/0 when a is 0, and log(a) is -inf meeting
// v = 0. The skip is elementwise because the base is, and it is a skip
// rather than a multiply by a zero mask for the reason sqrtv_bwd's is:
// multiplying an inf by zero is the NaN being avoided. A NaN base still
// propagates, since `NaN == 0.0` is false.
void pow_bwd(KernelCtx& ctx) {
  const bool s0 = scal(ctx, 0), s1 = scal(ctx, 1);
  if (ctx.out.len == 1) {
    const double a = ctx.in[0].data[0], b = ctx.in[1].data[0];
    const double v = ctx.out.data[0];
    if (a == 0.0) return;
    if (ctx.in_adj[0].data) ctx.in_adj[0].data[0] += ctx.out_adj * b * v / a;
    if (ctx.in_adj[1].data)
      ctx.in_adj[1].data[0] += ctx.out_adj * std::log(a) * v;
    return;
  }
  for (int64_t i = 0; i < ctx.out.len; ++i) {
    const double a = ctx.in[0].data[s0 ? 0 : i];
    const double b = ctx.in[1].data[s1 ? 0 : i];
    const double v = ctx.out.data[i];
    const double dout = ctx.out_adj_vec.data[i];
    if (a == 0.0) continue;
    if (ctx.in_adj[0].data) ctx.in_adj[0].data[s0 ? 0 : i] += dout * b * v / a;
    if (ctx.in_adj[1].data)
      ctx.in_adj[1].data[s1 ? 0 : i] += dout * std::log(a) * v;
  }
}

void dot_fwd(KernelCtx& ctx) {
  ctx.out.data[0] = (in_a(ctx, 0) * in_a(ctx, 1)).sum();
}
void dot_bwd(KernelCtx& ctx) {
  if (ctx.in_adj[0].data) dx_a(ctx, 0) += ctx.out_adj * in_a(ctx, 1);
  if (ctx.in_adj[1].data) dx_a(ctx, 1) += ctx.out_adj * in_a(ctx, 0);
}

// ---- unaries ---------------------------------------------------------------
// AoS Matrix<var> unaries route through apply_scalar_unary: scalar libm per
// element, NOT Eigen packet math. Transcendental kernels therefore use
// scalar loops; sqrt (IEEE-exact) and neg/square (exact) may vectorize.
void negu_fwd(KernelCtx& ctx) { out_a(ctx) = -in_a(ctx, 0); }
void negu_bwd(KernelCtx& ctx) {
  if (ctx.in_adj[0].data) {
    if (ctx.out.len == 1)
      ctx.in_adj[0].data[0] -= ctx.out_adj;
    else
      dx_a(ctx, 0) -= dout_a(ctx);
  }
}
void expv_fwd(KernelCtx& ctx) {
  for (int64_t i = 0; i < ctx.out.len; ++i)
    ctx.out.data[i] = std::exp(ctx.in[0].data[i]);
}
void expv_bwd(KernelCtx& ctx) {
  if (!ctx.in_adj[0].data) return;
  CMapA out_v(ctx.out.data, ctx.out.len);
  if (ctx.out.len == 1)
    ctx.in_adj[0].data[0] += ctx.out_adj * ctx.out.data[0];
  else
    dx_a(ctx, 0) += dout_a(ctx) * out_v;
}
// AoS Matrix<var> tanh goes through apply_scalar_unary: per-element
// std::tanh forward, adjoint dout / cosh(x)^2 with cosh recomputed, exactly
// as the scalar rev overload's callback does.
void tanhv_fwd(KernelCtx& ctx) {
  for (int64_t i = 0; i < ctx.out.len; ++i)
    ctx.out.data[i] = std::tanh(ctx.in[0].data[i]);
}
void tanhv_bwd(KernelCtx& ctx) {
  if (!ctx.in_adj[0].data) return;
  const double* dout = ctx.out.len == 1 ? &ctx.out_adj : ctx.out_adj_vec.data;
  for (int64_t i = 0; i < ctx.out.len; ++i) {
    const double c = std::cosh(ctx.in[0].data[i]);
    ctx.in_adj[0].data[i] += dout[i] / (c * c);
  }
}

// rev cumulative_sum: sequential prefix sums forward; backward walks
// descending, accumulating a running suffix into the result adjoint itself
// (safe here: the output slot's adjoint is dead after this op).
void cumsum_fwd(KernelCtx& ctx) {
  double acc = 0.0;
  for (int64_t i = 0; i < ctx.out.len; ++i) {
    acc += ctx.in[0].data[i];
    ctx.out.data[i] = acc;
  }
}
void cumsum_bwd(KernelCtx& ctx) {
  if (!ctx.in_adj[0].data) return;
  if (ctx.out.len == 1) {
    ctx.in_adj[0].data[0] += ctx.out_adj;
    return;
  }
  double* radj = ctx.out_adj_vec.data;
  const int64_t n = ctx.out.len;
  for (int64_t i = n - 1; i > 0; --i) {
    ctx.in_adj[0].data[i] += radj[i];
    radj[i - 1] += radj[i];
  }
  ctx.in_adj[0].data[0] += radj[0];
}

void logv_fwd(KernelCtx& ctx) {
  for (int64_t i = 0; i < ctx.out.len; ++i)
    ctx.out.data[i] = std::log(ctx.in[0].data[i]);
}
void logv_bwd(KernelCtx& ctx) {
  if (!ctx.in_adj[0].data) return;
  if (ctx.out.len == 1)
    ctx.in_adj[0].data[0] += ctx.out_adj / ctx.in[0].data[0];
  else
    dx_a(ctx, 0) += dout_a(ctx) / in_a(ctx, 0);
}
// The one unary here whose two stan-math overloads compute DIFFERENT
// expressions, so its two shapes cannot share a formula -- exactly the split
// clu_fwd makes, and for the same reason (see constrain.cpp's header):
//   scalar var  -> stan's `inv_logit(double)`, which branches on sign;
//   Matrix<var> -> `x.val().array().logistic()`, Eigen's logistic functor,
//                  `e/(1+e)` with an inf guard, no sign branch.
// They disagree by a ulp on about a third of positive arguments.
//
// The len > 1 branch used to hand `logistic()` a CONTIGUOUS temporary, on the
// belief that the Matrix<var> overload vectorized. It does not -- `.val()` is
// strided, so Eigen runs the functor's SCALAR body with libm exp. Contiguous
// doubles select Eigen's `pexp` instead, a ulp off libm on ~7% of arguments,
// and that was the entire divergence: forward only, with the backward's exact
// multiplies inheriting it. So the default spells the scalar functor out and
// the vectorized form stays behind packet_math(), whose reference is varmat.
void invlogit_fwd(KernelCtx& ctx) {
  const int64_t n = ctx.out.len;
  if (n == 1) {
    ctx.out.data[0] = stan::math::inv_logit(ctx.in[0].data[0]);
  } else if (packet_math()) {
    out_a(ctx) = stan::math::inv_logit(in_a(ctx, 0).matrix().eval().array());
  } else {
    const double* x = ctx.in[0].data;
    for (int64_t i = 0; i < n; ++i) {
      const double e = std::exp(x[i]);
      ctx.out.data[i] = std::isinf(e) ? 1.0 : e / (1.0 + e);
    }
  }
}
void invlogit_bwd(KernelCtx& ctx) {
  if (!ctx.in_adj[0].data) return;
  CMapA out_v(ctx.out.data, ctx.out.len);
  if (ctx.out.len == 1)
    ctx.in_adj[0].data[0] +=
        ctx.out_adj * ctx.out.data[0] * (1.0 - ctx.out.data[0]);
  else
    dx_a(ctx, 0) += dout_a(ctx) * out_v * (1.0 - out_v);
}
void sqrtv_fwd(KernelCtx& ctx) { out_a(ctx) = in_a(ctx, 0).sqrt(); }
void sqrtv_bwd(KernelCtx& ctx) {
  if (!ctx.in_adj[0].data) return;
  CMapA out_v(ctx.out.data, ctx.out.len);
  // sqrt(0) contributes nothing rather than 1/(2*0) = inf. That is what
  // stan-math's rev overload does (rev/fun/sqrt.hpp guards on
  // `vi.val() != 0.0`), and matching it is what keeps a model whose input
  // underflows to exact zero differentiable: the inf would meet the zero
  // value on the way back through the op that produced it and become NaN.
  // accel_gp is the corpus case -- spd_cov_exp_quad's exp() underflows for
  // the largest Laplacian eigenvalue. select, not a mask multiply, because
  // multiplying an inf by zero is the NaN we are avoiding.
  if (ctx.out.len == 1) {
    if (ctx.out.data[0] != 0.0)
      ctx.in_adj[0].data[0] += ctx.out_adj / (2.0 * ctx.out.data[0]);
  } else {
    dx_a(ctx, 0) += (out_v != 0.0).select(dout_a(ctx) / (2.0 * out_v), 0.0);
  }
}
void squarev_fwd(KernelCtx& ctx) { out_a(ctx) = in_a(ctx, 0).square(); }
void squarev_bwd(KernelCtx& ctx) {
  if (!ctx.in_adj[0].data) return;
  if (ctx.out.len == 1)
    ctx.in_adj[0].data[0] += ctx.out_adj * 2.0 * ctx.in[0].data[0];
  else
    dx_a(ctx, 0) += dout_a(ctx) * 2.0 * in_a(ctx, 0);
}
void log1mv_fwd(KernelCtx& ctx) {
  if (ctx.out.len == 1) {
    ctx.out.data[0] = stan::math::log1m(ctx.in[0].data[0]);
  } else {
    out_a(ctx) = stan::math::log1m(in_a(ctx, 0).matrix().eval().array());
  }
}
void log1mv_bwd(KernelCtx& ctx) {
  if (!ctx.in_adj[0].data) return;
  if (ctx.out.len == 1)
    ctx.in_adj[0].data[0] += ctx.out_adj / (ctx.in[0].data[0] - 1.0);
  else
    dx_a(ctx, 0) += dout_a(ctx) / (in_a(ctx, 0) - 1.0);
}

// logit(x) = log(x/(1-x)); rev scalar: adj / (x*(1-x)) per element.
void logitv_fwd(KernelCtx& ctx) {
  for (int64_t i = 0; i < ctx.out.len; ++i)
    ctx.out.data[i] = stan::math::logit(ctx.in[0].data[i]);
}
void logitv_bwd(KernelCtx& ctx) {
  if (!ctx.in_adj[0].data) return;
  for (int64_t i = 0; i < ctx.out.len; ++i) {
    const double x = ctx.in[0].data[i];
    const double d = ctx.out.len == 1 ? ctx.out_adj : ctx.out_adj_vec.data[i];
    ctx.in_adj[0].data[i] += d / (x * (1.0 - x));
  }
}

void mean_fwd(KernelCtx& ctx) { ctx.out.data[0] = in_a(ctx, 0).mean(); }
void mean_bwd(KernelCtx& ctx) {
  if (!ctx.in_adj[0].data) return;
  const double d = ctx.out_adj / static_cast<double>(ctx.in[0].len);
  for (int64_t i = 0; i < ctx.in[0].len; ++i) ctx.in_adj[0].data[i] += d;
}

// rep_vector(x, n): out[i] = x; scalar adjoint accumulates ascending.
void repv_fwd(KernelCtx& ctx) {
  for (int64_t i = 0; i < ctx.out.len; ++i) ctx.out.data[i] = ctx.in[0].data[0];
}
void repv_bwd(KernelCtx& ctx) {
  if (!ctx.in_adj[0].data) return;
  for (int64_t i = 0; i < ctx.out.len; ++i)
    ctx.in_adj[0].data[0] += ctx.out_adj_vec.data[i];
}

// Generated from STANLI_SCALAR_UNARY_LIST (optable.hpp): the value in the
// forward, the ordered delta and its pullback topology in the backward.
// Shape-preserving and elementwise, so a re-rolled vector arrives here as one
// op. Skipping disconnected pullbacks is observably different from 0*dout for
// non-finite upstream adjoints.
#define STANLI_DEFINE_UNARY(code, name, VAL, DELTA, TOPOLOGY)                \
  void name##_ufwd(KernelCtx& ctx) {                                         \
    for (int64_t i = 0; i < ctx.out.len; ++i) {                              \
      const double x = ctx.in[0].data[i];                                    \
      ctx.out.data[i] = (VAL);                                               \
    }                                                                        \
  }                                                                          \
  void name##_ubwd(KernelCtx& ctx) {                                         \
    if (!ctx.in_adj[0].data) return;                                         \
    const double* dout =                                                     \
        ctx.out.len == 1 ? &ctx.out_adj : ctx.out_adj_vec.data;              \
    for (int64_t i = 0; i < ctx.out.len; ++i) {                              \
      const double x = ctx.in[0].data[i];                                    \
      const double y = ctx.out.data[i];                                      \
      const double seed = dout[i];                                           \
      if (unary_has_pullback(TOPOLOGY, x)) ctx.in_adj[0].data[i] += (DELTA); \
    }                                                                        \
  }
STANLI_SCALAR_UNARY_LIST(STANLI_DEFINE_UNARY)
#undef STANLI_DEFINE_UNARY

}  // namespace

void register_eltwise_kernels() {
#define STANLI_REGISTER_UNARY(code, name, value, delta, topology) \
  register_kernel(code, Kernel{name##_ufwd, name##_ubwd, nullptr});
  STANLI_SCALAR_UNARY_LIST(STANLI_REGISTER_UNARY)
#undef STANLI_REGISTER_UNARY
  register_kernel(OP_ADD, Kernel{add_fwd, add_bwd, nullptr});
  register_kernel(OP_SUB, Kernel{sub_fwd, sub_bwd, nullptr});
  register_kernel(OP_MUL, Kernel{mul_fwd, mul_bwd, nullptr});
  register_kernel(OP_FMA, Kernel{fma_fwd, fma_bwd, nullptr});
  register_kernel(OP_DIV, Kernel{div_fwd, div_bwd, nullptr});
  register_kernel(OP_POW, Kernel{pow_fwd, pow_bwd, nullptr});
  register_kernel(OP_DOT, Kernel{dot_fwd, dot_bwd, nullptr});
  register_kernel(OP_NEG, Kernel{negu_fwd, negu_bwd, nullptr});
  register_kernel(OP_EXPV, Kernel{expv_fwd, expv_bwd, nullptr});
  register_kernel(OP_TANHV, Kernel{tanhv_fwd, tanhv_bwd, nullptr});
  register_kernel(OP_CUMSUM, Kernel{cumsum_fwd, cumsum_bwd, nullptr});
  register_kernel(OP_LOGV, Kernel{logv_fwd, logv_bwd, nullptr});
  register_kernel(OP_INV_LOGIT, Kernel{invlogit_fwd, invlogit_bwd, nullptr});
  register_kernel(OP_SQRT, Kernel{sqrtv_fwd, sqrtv_bwd, nullptr});
  register_kernel(OP_SQUARE, Kernel{squarev_fwd, squarev_bwd, nullptr});
  register_kernel(OP_LOG1M, Kernel{log1mv_fwd, log1mv_bwd, nullptr});
  register_kernel(OP_LOGIT, Kernel{logitv_fwd, logitv_bwd, nullptr});
  register_kernel(OP_MEAN, Kernel{mean_fwd, mean_bwd, nullptr});
  register_kernel(OP_REP_VEC, Kernel{repv_fwd, repv_bwd, nullptr});
}

}  // namespace stanli
