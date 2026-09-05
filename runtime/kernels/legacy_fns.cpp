// Legacy ops: kernels of exactly this shape wrap any stan-math signature
// that has no native port yet.
#include <stanli/graph.hpp>
#include <stanli/legacy.hpp>
#include <stanli/optable.hpp>
#include <stanli/packet.hpp>

namespace stanli {
namespace {

void softmax_fwd(KernelCtx& ctx) {
  Eigen::Map<const Eigen::VectorXd> x(ctx.in[0].data, ctx.in[0].len);
  Eigen::Map<Eigen::VectorXd> out(ctx.out.data, ctx.out.len);
  out = stan::math::softmax(x);
}
void softmax_bwd(KernelCtx& ctx) {
  legacy_bwd_vec_in(ctx, [](const auto& x) { return stan::math::softmax(x); });
}

// Multivariate density via nested replay: dirichlet_lpdf(theta | alpha).
// The recorder's vector edges do not model partials_vec_ (sequence-of-vector
// partials), so this is a legacy op by design.
//
// Propto term-dropping in stan-math is decided by the ARGUMENT TYPES, so a
// legacy propto op must bind each argument var-or-double per the activity
// mask, exactly like the native kernels do; promoting an inactive argument
// to var silently keeps terms CmdStan drops.
inline int64_t vectorized_count(int encoded) {
  return encoded < 0 ? 1 : encoded;
}

template <typename Scalar, typename Input>
std::vector<Eigen::Matrix<Scalar, -1, 1>> vectorized_vectors(const Input& input,
                                                             int64_t width,
                                                             int encoded) {
  using Vector = Eigen::Matrix<Scalar, -1, 1>;
  std::vector<Vector> result((size_t)vectorized_count(encoded), Vector(width));
  for (int64_t n = 0; n < (int64_t)result.size(); ++n)
    for (int64_t k = 0; k < width; ++k)
      result[(size_t)n](k) = input.data[n * width + k];
  return result;
}

template <typename Vectors, typename F>
decltype(auto) with_vectorized_argument(Vectors& values, int encoded, F&& f) {
  return encoded < 0 ? std::forward<F>(f)(values[0])
                     : std::forward<F>(f)(values);
}

double dirichlet_eval(KernelCtx& ctx) {
  stan::math::nested_rev_autodiff nested;
  using stan::math::var;
  const bool propto = (ctx.variant & 0x80u) != 0;
  const unsigned mask = ctx.variant == 0 ? 0x3u : (ctx.variant & 0x3fu);
  // New calls encode {inner width, theta count, alpha count}. Retain the
  // historic unencoded theta-array/alpha-vector layout for direct graphs.
  const int64_t width = ctx.n_idata >= 3 ? ctx.idata[0] : ctx.in[1].len;
  const int theta_encoded =
      ctx.n_idata >= 3
          ? ctx.idata[1]
          : (width > 0 && ctx.in[0].len > width ? (int)(ctx.in[0].len / width)
                                                : -1);
  const int alpha_encoded = ctx.n_idata >= 3 ? ctx.idata[2] : -1;
  auto theta = vectorized_vectors<var>(ctx.in[0], width, theta_encoded);
  auto alpha = vectorized_vectors<var>(ctx.in[1], width, alpha_encoded);
  auto theta_d = vectorized_vectors<double>(ctx.in[0], width, theta_encoded);
  auto alpha_d = vectorized_vectors<double>(ctx.in[1], width, alpha_encoded);
  const bool a0 = (mask & 1u) != 0, a1 = (mask & 2u) != 0;
  const auto call = [&](const auto& theta_arg, const auto& alpha_arg) {
    return propto ? stan::math::dirichlet_lpdf<true>(theta_arg, alpha_arg)
                  : stan::math::dirichlet_lpdf<false>(theta_arg, alpha_arg);
  };
  var lp;
  const auto with_alpha = [&](const auto& theta_arg) {
    if (a1)
      with_vectorized_argument(alpha, alpha_encoded, [&](const auto& arg) {
        lp = call(theta_arg, arg);
      });
    else
      with_vectorized_argument(alpha_d, alpha_encoded, [&](const auto& arg) {
        lp = call(theta_arg, arg);
      });
  };
  if (propto && !a0 && !a1) {
    lp = 0.0;
  } else if (a0) {
    with_vectorized_argument(theta, theta_encoded, with_alpha);
  } else {
    with_vectorized_argument(theta_d, theta_encoded, with_alpha);
  }
  const double value = lp.val();
  if (!values_only()) {
    stan::math::grad(lp.vi_);
    double* s = ctx.scratch;
    for (auto& value : theta)
      for (int64_t i = 0; i < value.size(); ++i) *s++ = value(i).adj();
    for (auto& value : alpha)
      for (int64_t i = 0; i < value.size(); ++i) *s++ = value(i).adj();
  }
  return value;
}
void dirichlet_fwd(KernelCtx& ctx) { ctx.out.data[0] = dirichlet_eval(ctx); }

// One tape per gradient, not two: the forward grads it with a seed of 1 and
// stashes, the backward scales. dirichlet_lpdf reduces through a partials
// propagator, so the two seedings round identically.
void dirichlet_bwd(KernelCtx& ctx) {
  const double* s = ctx.scratch;
  for (int k = 0; k < 2; ++k) {
    if (ctx.in_adj[k].data)
      Eigen::Map<Eigen::ArrayXd>(ctx.in_adj[k].data, ctx.in[k].len) +=
          ctx.out_adj * Eigen::Map<const Eigen::ArrayXd>(s, ctx.in[k].len);
    s += ctx.in[k].len;
  }
}

void log_softmax_fwd(KernelCtx& ctx) {
  Eigen::Map<const Eigen::VectorXd> x(ctx.in[0].data, ctx.in[0].len);
  Eigen::Map<Eigen::VectorXd> out(ctx.out.data, ctx.out.len);
  out = stan::math::log_softmax(x);
}
void log_softmax_bwd(KernelCtx& ctx) {
  legacy_bwd_vec_in(ctx,
                    [](const auto& x) { return stan::math::log_softmax(x); });
}

}  // namespace

void register_legacy_kernels() {
  register_kernel(OP_LOG_SOFTMAX,
                  Kernel{log_softmax_fwd, log_softmax_bwd, nullptr});
  register_kernel(OP_SOFTMAX, Kernel{softmax_fwd, softmax_bwd, nullptr});
  register_kernel(OP_DIRICHLET_LPDF,
                  Kernel{dirichlet_fwd, dirichlet_bwd, sum_in_lens});
}

}  // namespace stanli
