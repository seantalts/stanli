// Probability functions with algorithmic reverse-mode implementations in Math.
#include <stanli/graph.hpp>
#include <stanli/optable.hpp>
#include <stan/math.hpp>

#include <vector>

namespace stanli {
namespace {
using stan::math::var;

void student_t_qf_fwd(KernelCtx& ctx) {
  for (int64_t i = 0; i < ctx.out.len; ++i) {
    double a[4];
    for (int k = 0; k < 4; ++k)
      a[k] = ctx.in[k].data[ctx.in[k].len == 1 ? 0 : i];
    ctx.out.data[i] = stan::math::student_t_qf(a[0], a[1], a[2], a[3]);
  }
}

// Bind each input once, including broadcast scalars. Matching argument
// activity avoids computing expensive or undefined partials for data inputs.
template <typename... Args>
void quantile_bind(KernelCtx& ctx, const Args&... args) {
  constexpr int k = sizeof...(Args);
  if constexpr (k == 4) {
    std::vector<var> result;
    for (int64_t i = 0; i < ctx.out.len; ++i)
      result.push_back(
          stan::math::student_t_qf(args[args.size() == 1 ? 0 : i]...));
    var seeded = 0.0;
    for (int64_t i = 0; i < ctx.out.len; ++i)
      seeded += result[i] *
                (ctx.out.len == 1 ? ctx.out_adj : ctx.out_adj_vec.data[i]);
    stan::math::grad(seeded.vi_);
  } else if (ctx.in_adj[k].data) {
    std::vector<var> input;
    for (int64_t i = 0; i < ctx.in[k].len; ++i)
      input.emplace_back(ctx.in[k].data[i]);
    quantile_bind(ctx, args..., input);
    for (int64_t i = 0; i < ctx.in[k].len; ++i)
      ctx.in_adj[k].data[i] += input[i].adj();
  } else {
    std::vector<double> input;
    for (int64_t i = 0; i < ctx.in[k].len; ++i)
      input.push_back(ctx.in[k].data[i]);
    quantile_bind(ctx, args..., input);
  }
}

void student_t_qf_bwd(KernelCtx& ctx) {
  stan::math::nested_rev_autodiff nested;
  quantile_bind(ctx);
}

template <typename Y, typename Theta>
auto poisson_binomial_call(uint8_t variant, const Y& y, const Theta& theta) {
  switch (variant & 6u) {
    case 0:
      return stan::math::poisson_binomial_lpmf(y, theta);
    case 2:
      return stan::math::poisson_binomial_cdf(y, theta);
    case 4:
      return stan::math::poisson_binomial_lcdf(y, theta);
    default:
      return stan::math::poisson_binomial_lccdf(y, theta);
  }
}

// Payload: probability width, scalar-outcome flag, then integer outcomes.
// Variant bits 1-2 select the probability function; bit 0 records activity.
template <typename Theta>
auto poisson_binomial_outcome(KernelCtx& ctx, const Theta& theta) {
  if (ctx.idata[1])
    return poisson_binomial_call(ctx.variant, ctx.idata[2], theta);
  const std::vector<int> y(ctx.idata + 2, ctx.idata + ctx.n_idata);
  return poisson_binomial_call(ctx.variant, y, theta);
}

template <typename T>
auto poisson_binomial_eval(KernelCtx& ctx, const std::vector<T>& input) {
  using Vec = Eigen::Matrix<T, -1, 1>;
  const int width = ctx.idata[0];
  Vec theta(width);
  for (int j = 0; j < width; ++j) theta[j] = input[j];
  return poisson_binomial_outcome(ctx, theta);
}

void poisson_binomial_fwd(KernelCtx& ctx) {
  std::vector<double> input;
  for (int64_t i = 0; i < ctx.in[0].len; ++i)
    input.push_back(ctx.in[0].data[i]);
  ctx.out.data[0] = poisson_binomial_eval(ctx, input);
}

void poisson_binomial_bwd(KernelCtx& ctx) {
  if (!ctx.in_adj[0].data) return;
  stan::math::nested_rev_autodiff nested;
  std::vector<var> input;
  for (int64_t i = 0; i < ctx.in[0].len; ++i)
    input.emplace_back(ctx.in[0].data[i]);
  var result = poisson_binomial_eval(ctx, input);
  result.grad();
  for (int64_t i = 0; i < ctx.in[0].len; ++i)
    ctx.in_adj[0].data[i] += ctx.out_adj * input[i].adj();
}
}  // namespace

void register_probability_fns_kernels() {
  register_kernel(OP_STUDENT_T_QF,
                  {student_t_qf_fwd, student_t_qf_bwd, nullptr});
  register_kernel(OP_POISSON_BINOMIAL,
                  {poisson_binomial_fwd, poisson_binomial_bwd, nullptr});
}
}  // namespace stanli
