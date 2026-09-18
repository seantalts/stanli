// Legacy ops: kernels of exactly this shape wrap any stan-math signature
// that has no native port yet.
#include <stanli/graph.hpp>
#include <stanli/recorder.hpp>
#include <stanli/legacy.hpp>
#include <stanli/optable.hpp>
#include <stanli/packet.hpp>

namespace stanli {
namespace {

// Apply one vector operation per array leaf. Interpreted arrays use
// first-index-fast storage; graph and register arrays use outer-major.
template <typename F>
void grouped_unary(KernelCtx& ctx, F&& f, bool backward) {
  const int64_t groups = ctx.idata[0], width = ctx.idata[1];
  const bool interleaved = ctx.idata[2] != 0;
  const auto cell = [&](int64_t g, int64_t j) {
    return interleaved ? j * groups + g : g * width + j;
  };
  for (int64_t g = 0; g < groups; ++g) {
    Eigen::VectorXd input(width), output(width), adjoint(width),
        upstream(width);
    for (int64_t j = 0; j < width; ++j) input[j] = ctx.in[0].data[cell(g, j)];
    KernelCtx leaf = ctx;
    leaf.idata = nullptr;
    leaf.n_idata = 0;
    leaf.in[0] = {input.data(), width};
    leaf.out = {output.data(), width};
    if (backward) {
      if (!ctx.in_adj[0].data) return;
      adjoint.setZero();
      for (int64_t j = 0; j < width; ++j)
        upstream[j] =
            ctx.out.len == 1 ? ctx.out_adj : ctx.out_adj_vec.data[cell(g, j)];
      leaf.in_adj[0] = {adjoint.data(), width};
      leaf.out_adj_vec = {upstream.data(), width};
      if (width == 1) leaf.out_adj = upstream[0];
      legacy_bwd_vec_in(leaf, f);
      for (int64_t j = 0; j < width; ++j)
        ctx.in_adj[0].data[cell(g, j)] += adjoint[j];
    } else {
      output = f(input);
      for (int64_t j = 0; j < width; ++j) ctx.out.data[cell(g, j)] = output[j];
    }
  }
}

void softmax_fwd(KernelCtx& ctx) {
  if (ctx.n_idata == 3) {
    grouped_unary(
        ctx, [](const auto& x) { return stan::math::softmax(x); }, false);
    return;
  }
  Eigen::Map<const Eigen::VectorXd> x(ctx.in[0].data, ctx.in[0].len);
  Eigen::Map<Eigen::VectorXd> out(ctx.out.data, ctx.out.len);
  out = stan::math::softmax(x);
}
void softmax_bwd(KernelCtx& ctx) {
  if (ctx.n_idata == 3) {
    grouped_unary(
        ctx, [](const auto& x) { return stan::math::softmax(x); }, true);
    return;
  }
  legacy_bwd_vec_in(ctx, [](const auto& x) { return stan::math::softmax(x); });
}

// Multivariate density via nested replay: dirichlet_lpdf(theta | alpha).
// Arrays of vectors retain the nested-tape path. Shared vectors use the
// recorder's broadcast vector edge, avoiding a temporary autodiff tape.
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
void dirichlet_fwd(KernelCtx& ctx) {
  const bool shared_vectors = ctx.n_idata >= 3
                                  ? ctx.idata[1] < 0 && ctx.idata[2] < 0 &&
                                        ctx.idata[0] == ctx.in[0].len &&
                                        ctx.idata[0] == ctx.in[1].len
                                  : ctx.in[0].len == ctx.in[1].len;
  if (!shared_vectors) {
    ctx.out.data[0] = dirichlet_eval(ctx);
    return;
  }
  const bool propto = (ctx.variant & 0x80u) != 0;
  const unsigned mask = ctx.variant == 0 ? 3u : (ctx.variant & 3u);
  sink s;
  s.buf[0] = ctx.scratch;
  s.buf[1] = ctx.scratch + ctx.in[0].len;
  s.len[0] = ctx.in[0].len;
  s.len[1] = ctx.in[1].len;
  std::fill_n(ctx.scratch, s.len[0] + s.len[1], 0.0);
  if (propto && mask == 0) {
    // Preserve the legacy all-data propto shortcut, including validation.
    ctx.out.data[0] = 0.0;
    return;
  }
  sink_scope active(s);
  const auto call = [&](const auto& theta, const auto& alpha) {
    record_probability_call([&] {
      return propto ? stan::math::dirichlet_lpdf<true>(theta, alpha)
                    : stan::math::dirichlet_lpdf<false>(theta, alpha);
    });
  };
  const auto with_alpha = [&](const auto& theta) {
    if (mask & 2u)
      call(theta, as_rvar(ctx.in[1]));
    else
      call(theta,
           Eigen::Map<const Eigen::VectorXd>(ctx.in[1].data, ctx.in[1].len));
  };
  if (mask & 1u)
    with_alpha(as_rvar(ctx.in[0]));
  else
    with_alpha(
        Eigen::Map<const Eigen::VectorXd>(ctx.in[0].data, ctx.in[0].len));
  ctx.out.data[0] = s.value;
}

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
  if (ctx.n_idata == 3) {
    grouped_unary(
        ctx, [](const auto& x) { return stan::math::log_softmax(x); }, false);
    return;
  }
  Eigen::Map<const Eigen::VectorXd> x(ctx.in[0].data, ctx.in[0].len);
  Eigen::Map<Eigen::VectorXd> out(ctx.out.data, ctx.out.len);
  out = stan::math::log_softmax(x);
}
void log_softmax_bwd(KernelCtx& ctx) {
  if (ctx.n_idata == 3) {
    grouped_unary(
        ctx, [](const auto& x) { return stan::math::log_softmax(x); }, true);
    return;
  }
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
