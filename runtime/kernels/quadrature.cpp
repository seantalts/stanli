// One runtime kernel for the complete one-dimensional quadrature family.
// Algorithm selection and tolerances live in QuadratureSpec; callback
// execution is shared with ODE/algebra through RetainedCallback/RhsProgram.
#include <stanli/graph.hpp>
#include <stanli/mir_interp.hpp>
#include <stanli/ode_prog.hpp>
#include <stanli/optable.hpp>
#include <stanli/quadrature.hpp>

#include <stan/math.hpp>

#include <type_traits>
#include <vector>

namespace stanli {
namespace {

struct MirIntegrand {
  const QuadratureSpec* spec;
  mutable RhsWorkspace workspace;

  template <typename Theta>
  auto operator()(double x, double xc, std::ostream*,
                  const Theta& theta_expr) const {
    using T = typename std::decay_t<Theta>::Scalar;
    Eigen::Matrix<T, Eigen::Dynamic, 1> theta(theta_expr.size());
    for (Eigen::Index i = 0; i < theta_expr.size(); ++i)
      theta(i) = theta_expr.coeff(i);
    std::vector<T> result;
    if (spec->prog.ok) {
      run_rhs<T>(spec->prog, x, &xc, theta.data(),
                 static_cast<size_t>(spec->parameter_count), spec->x_r.data(),
                 result, workspace.get<T>());
    } else {
      const mir::FunDef* callback = spec->callback();
      if (!callback)
        throw std::runtime_error("quadrature: missing integrand callback");
      std::vector<std::vector<T>> reals{{T(x)}, {T(xc)}};
      std::vector<std::vector<int>> ints;
      size_t theta_at = 0, xr_at = 0;
      for (const RhsArg& arg : spec->args) {
        if (arg.is_int) {
          ints.push_back(arg.ints);
          continue;
        }
        std::vector<T> values;
        values.reserve(static_cast<size_t>(arg.len));
        if (arg.is_param) {
          for (int i = 0; i < arg.len; ++i)
            values.push_back(theta(static_cast<Eigen::Index>(theta_at++)));
        } else {
          for (int i = 0; i < arg.len; ++i)
            values.push_back(T(spec->x_r[xr_at++]));
        }
        reals.push_back(std::move(values));
      }
      MirInterp<T> interpreter(*spec->funs(), "quadrature integrand");
      result = interpreter.call(*callback, reals, ints);
    }
    if (result.size() != 1)
      throw std::runtime_error("quadrature: integrand must return one real");
    return result[0];
  }
};

template <typename T_a, typename T_b, typename T_theta>
auto integrate(const QuadratureSpec& spec, const T_a& a, const T_b& b,
               const Eigen::Matrix<T_theta, Eigen::Dynamic, 1>& theta) {
  const MirIntegrand f{&spec};
  switch (spec.method) {
    case mir::QuadratureMethod::Integrate1D:
      return stan::math::integrate_1d_impl(f, a, b, spec.relative_tolerance,
                                           nullptr, theta);
    case mir::QuadratureMethod::DoubleExponential:
      return stan::math::integrate_1d_double_exponential_tol(
          f, a, b, spec.relative_tolerance, spec.absolute_tolerance,
          spec.max_steps, nullptr, theta);
    case mir::QuadratureMethod::GaussKronrod:
      return stan::math::integrate_1d_gauss_kronrod_tol(
          f, a, b, spec.relative_tolerance, spec.absolute_tolerance,
          spec.max_steps, nullptr, theta);
  }
  throw std::logic_error("quadrature: unknown method");
}

template <bool ActiveA, bool ActiveB, bool ActiveTheta>
void quadrature_eval(KernelCtx& ctx, const QuadratureSpec& spec) {
  using A = std::conditional_t<ActiveA, stan::math::var, double>;
  using B = std::conditional_t<ActiveB, stan::math::var, double>;
  using T = std::conditional_t<ActiveTheta, stan::math::var, double>;
  stan::math::nested_rev_autodiff nested;
  const A a = A(ctx.in[0].data[0]);
  const B b = B(ctx.in[1].data[0]);
  Eigen::Matrix<T, Eigen::Dynamic, 1> theta(spec.parameter_count);
  for (int i = 0; i < spec.parameter_count; ++i)
    theta(i) = T(ctx.in[2].data[i]);

  auto integral = integrate(spec, a, b, theta);
  ctx.out.data[0] = stan::math::value_of(integral);
  const int64_t scratch_len = 2 + ctx.in[2].len;
  std::fill(ctx.scratch, ctx.scratch + scratch_len, 0.0);
  if constexpr (ActiveA || ActiveB || ActiveTheta) {
    integral.grad();
    if constexpr (ActiveA) ctx.scratch[0] = a.adj();
    if constexpr (ActiveB) ctx.scratch[1] = b.adj();
    if constexpr (ActiveTheta)
      for (int i = 0; i < spec.parameter_count; ++i)
        ctx.scratch[2 + i] = theta(i).adj();
  }
}

void quadrature_fwd(KernelCtx& ctx) {
  if (ctx.n_in != 3 || ctx.out.len != 1 || ctx.in[0].len != 1 ||
      ctx.in[1].len != 1)
    throw std::runtime_error("quadrature: malformed kernel arguments");
  const auto& spec = *static_cast<const QuadratureSpec*>(ctx.udata);
  switch (ctx.variant & 0x7u) {
    case 0:
      quadrature_eval<false, false, false>(ctx, spec);
      break;
    case 1:
      quadrature_eval<true, false, false>(ctx, spec);
      break;
    case 2:
      quadrature_eval<false, true, false>(ctx, spec);
      break;
    case 3:
      quadrature_eval<true, true, false>(ctx, spec);
      break;
    case 4:
      quadrature_eval<false, false, true>(ctx, spec);
      break;
    case 5:
      quadrature_eval<true, false, true>(ctx, spec);
      break;
    case 6:
      quadrature_eval<false, true, true>(ctx, spec);
      break;
    case 7:
      quadrature_eval<true, true, true>(ctx, spec);
      break;
  }
}

void quadrature_bwd(KernelCtx& ctx) {
  int64_t at = 0;
  for (int k = 0; k < ctx.n_in; ++k) {
    for (int64_t i = 0; i < ctx.in[k].len; ++i, ++at)
      if (ctx.in_adj[k].data != nullptr)
        ctx.in_adj[k].data[i] += ctx.out_adj * ctx.scratch[at];
  }
}

int64_t quadrature_scratch(const Op& op, const Slot* slots) {
  return sum_in_lens(op, slots);
}

}  // namespace

void register_quadrature_kernels() {
  register_kernel(OP_QUADRATURE,
                  Kernel{quadrature_fwd, quadrature_bwd, quadrature_scratch});
}

}  // namespace stanli
