// CVODES adjoint-sensitivity ODE op. The value pass is a plain double solve.
// Reverse mode reruns the solve on a nested tape and contracts every solution
// with the graph's output adjoints, allowing Stan Math to perform one adjoint
// integration instead of storing a dense forward-sensitivity Jacobian.
#include <stanli/graph.hpp>
#include <stanli/mir_interp.hpp>
#include <stanli/ode_adjoint.hpp>
#include <stanli/optable.hpp>

#include <stan/math.hpp>

#include <type_traits>
#include <vector>

namespace stanli {
namespace {

using stan::math::var;

struct AdjointRhs {
  const OdeAdjointSpec* spec;
  mutable RhsWorkspace workspace;

  template <typename T_time, typename T_y, typename T_param>
  Eigen::Matrix<
      stan::return_type_t<T_time, typename std::decay_t<T_y>::Scalar, T_param>,
      Eigen::Dynamic, 1>
  operator()(const T_time& t, const T_y& y, std::ostream* msgs,
             const std::vector<T_param>& theta, const std::vector<double>& x_r,
             const std::vector<int>& x_i) const {
    using T = stan::return_type_t<T_time, typename std::decay_t<T_y>::Scalar,
                                  T_param>;
    using T_state = typename std::decay_t<T_y>::Scalar;
    const Eigen::Matrix<T_state, Eigen::Dynamic, 1> state = y;
    Eigen::Matrix<T, Eigen::Dynamic, 1> out(state.size());
    if (spec->prog.ok) {
      run_rhs_into<T>(spec->prog, t, state.data(), theta.data(), theta.size(),
                      x_r.data(), out.data(), workspace.get<T>());
      return out;
    }

    std::vector<std::vector<T>> reals{
        {T(t)}, std::vector<T>(state.data(), state.data() + state.size())};
    std::vector<std::vector<int>> ints;
    size_t theta_at = 0, xr_at = 0;
    for (const RhsArg& arg : spec->args) {
      if (arg.is_int) {
        ints.push_back(arg.ints);
      } else if (arg.is_param) {
        reals.emplace_back(theta.begin() + theta_at,
                           theta.begin() + theta_at + arg.len);
        theta_at += (size_t)arg.len;
      } else {
        reals.emplace_back(x_r.begin() + xr_at, x_r.begin() + xr_at + arg.len);
        xr_at += (size_t)arg.len;
      }
    }
    MirInterp<T> interpreter(*spec->funs(), "adjoint ODE function");
    const std::vector<T> rhs = interpreter.call(*spec->rhs(), reals, ints);
    for (size_t i = 0; i < rhs.size(); ++i) out((Eigen::Index)i) = rhs[i];
    return out;
  }
};

template <typename T_y0, typename T_t0, typename T_ts, typename T_theta>
auto solve(const OdeAdjointSpec& spec, const T_y0& y0, const T_t0& t0,
           const std::vector<T_ts>& ts, const std::vector<T_theta>& theta) {
  Eigen::Map<const Eigen::VectorXd> atol_forward(
      spec.absolute_tolerance_forward.data(),
      (Eigen::Index)spec.absolute_tolerance_forward.size());
  Eigen::Map<const Eigen::VectorXd> atol_backward(
      spec.absolute_tolerance_backward.data(),
      (Eigen::Index)spec.absolute_tolerance_backward.size());
  return stan::math::ode_adjoint_tol_ctl(
      AdjointRhs{&spec}, y0, t0, ts, spec.relative_tolerance_forward,
      atol_forward, spec.relative_tolerance_backward, atol_backward,
      spec.relative_tolerance_quadrature, spec.absolute_tolerance_quadrature,
      spec.max_num_steps, spec.num_steps_between_checkpoints,
      spec.interpolation_polynomial, spec.solver_forward, spec.solver_backward,
      nullptr, theta, spec.x_r, spec.x_i);
}

void ode_adjoint_fwd(KernelCtx& ctx) {
  const OdeAdjointSpec& spec = *static_cast<const OdeAdjointSpec*>(ctx.udata);
  const int64_t S = ctx.in[0].len;
  Eigen::Map<const Eigen::VectorXd> y0(ctx.in[0].data, S);
  const double t0 = ctx.in[1].data[0];
  std::vector<double> ts(ctx.in[2].data, ctx.in[2].data + ctx.in[2].len);
  std::vector<double> theta(ctx.in[3].data, ctx.in[3].data + ctx.in[3].len);
  const auto solution = solve(spec, y0, t0, ts, theta);
  if ((int64_t)solution.size() * S != ctx.out.len)
    throw std::runtime_error(
        "ode_adjoint_tol_ctl: result shape disagrees with output times");
  for (size_t n = 0; n < solution.size(); ++n)
    for (int64_t i = 0; i < S; ++i)
      ctx.out.data[(int64_t)n * S + i] = solution[n][i];
}

void ode_adjoint_bwd(KernelCtx& ctx) {
  const uint8_t mask =
      (ctx.variant & 0x10u) != 0 ? (ctx.variant & 0x0fu) : 0x0fu;
  if (mask == 0) return;
  const OdeAdjointSpec& spec = *static_cast<const OdeAdjointSpec*>(ctx.udata);
  const int64_t S = ctx.in[0].len;
  const int64_t N = ctx.in[2].len;
  const int64_t P = ctx.in[3].len;

  stan::math::nested_rev_autodiff nested;
  Eigen::Matrix<var, Eigen::Dynamic, 1> y0(S);
  for (int64_t i = 0; i < S; ++i) y0(i) = ctx.in[0].data[i];
  var t0 = ctx.in[1].data[0];
  std::vector<var> ts;
  ts.reserve((size_t)N);
  for (int64_t i = 0; i < N; ++i) ts.emplace_back(ctx.in[2].data[i]);
  std::vector<var> theta;
  theta.reserve((size_t)P);
  for (int64_t i = 0; i < P; ++i) theta.emplace_back(ctx.in[3].data[i]);

  const auto solution = solve(spec, y0, t0, ts, theta);
  var weighted = 0.0;
  for (int64_t n = 0; n < N; ++n)
    for (int64_t i = 0; i < S; ++i)
      weighted += ctx.out_adj_vec.data[n * S + i] * solution[(size_t)n][i];
  weighted.grad();

  if ((mask & 0x1u) && ctx.in_adj[0].data)
    for (int64_t i = 0; i < S; ++i) ctx.in_adj[0].data[i] += y0(i).adj();
  if ((mask & 0x2u) && ctx.in_adj[1].data) ctx.in_adj[1].data[0] += t0.adj();
  if ((mask & 0x4u) && ctx.in_adj[2].data)
    for (int64_t i = 0; i < N; ++i)
      ctx.in_adj[2].data[i] += ts[(size_t)i].adj();
  if ((mask & 0x8u) && ctx.in_adj[3].data)
    for (int64_t i = 0; i < P; ++i)
      ctx.in_adj[3].data[i] += theta[(size_t)i].adj();
}

}  // namespace

void register_ode_adjoint_kernels() {
  register_kernel(OP_ODE_ADJOINT,
                  Kernel{ode_adjoint_fwd, ode_adjoint_bwd, nullptr});
}

}  // namespace stanli
