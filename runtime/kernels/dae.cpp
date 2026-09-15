// DAE solve op. Stan Math's IDAS wrapper performs the state solve and forward
// sensitivities; the retained callback is evaluated by the shared register
// program, with the MIR interpreter as the coverage-preserving fallback.
#include <stanli/dae.hpp>
#include <stanli/graph.hpp>
#include <stanli/mir_interp.hpp>
#include <stanli/optable.hpp>
#include <stanli/packet.hpp>

#include <stan/math.hpp>

#include <type_traits>
#include <vector>

namespace stanli {
namespace {

using stan::math::var;

struct DaeResidual {
  const DaeSpec* spec;
  mutable RhsWorkspace workspace;

  template <typename T_y, typename T_yp, typename T_param>
  Eigen::Matrix<stan::return_type_t<T_y, T_yp, T_param>, Eigen::Dynamic, 1>
  operator()(double t, const Eigen::Matrix<T_y, Eigen::Dynamic, 1>& y,
             const Eigen::Matrix<T_yp, Eigen::Dynamic, 1>& yp,
             std::ostream* msgs, const std::vector<T_param>& theta,
             const std::vector<double>& x_r,
             const std::vector<int>& x_i) const {
    using T = stan::return_type_t<T_y, T_yp, T_param>;
    Eigen::Matrix<T, Eigen::Dynamic, 1> out(y.size());
    if (spec->prog.ok) {
      run_dae_into<T>(spec->prog, t, y.data(), yp.data(), theta.data(),
                      theta.size(), x_r.data(), out.data(), workspace.get<T>());
      return out;
    }

    std::vector<std::vector<T>> reals{
        {T(t)},
        std::vector<T>(y.data(), y.data() + y.size()),
        std::vector<T>(yp.data(), yp.data() + yp.size())};
    std::vector<std::vector<int>> ints;
    size_t th_at = 0, xr_at = 0;
    for (const RhsArg& arg : spec->args) {
      if (arg.is_int) {
        ints.push_back(arg.ints);
      } else if (arg.is_param) {
        reals.emplace_back(theta.begin() + th_at,
                           theta.begin() + th_at + arg.len);
        th_at += (size_t)arg.len;
      } else {
        reals.emplace_back(x_r.begin() + xr_at, x_r.begin() + xr_at + arg.len);
        xr_at += (size_t)arg.len;
      }
    }
    MirInterp<T> interpreter(*spec->funs(), "DAE residual");
    const std::vector<T> residual =
        interpreter.call(*spec->residual(), reals, ints);
    for (size_t i = 0; i < residual.size(); ++i)
      out((Eigen::Index)i) = residual[i];
    return out;
  }
};

template <typename T_y0, typename T_yp0, typename T_theta>
auto solve(const DaeSpec& spec, const double* y_values, const double* yp_values,
           int64_t states, const double* theta_values, int64_t parameters) {
  Eigen::Matrix<T_y0, Eigen::Dynamic, 1> y0(states);
  Eigen::Matrix<T_yp0, Eigen::Dynamic, 1> yp0(states);
  for (int64_t i = 0; i < states; ++i) {
    y0((Eigen::Index)i) = y_values[i];
    yp0((Eigen::Index)i) = yp_values[i];
  }
  std::vector<T_theta> theta(theta_values, theta_values + parameters);
  return stan::math::dae_tol(DaeResidual{&spec}, y0, yp0, spec.t0, spec.ts,
                             spec.rtol, spec.atol, spec.max_steps, nullptr,
                             theta, spec.x_r, spec.x_i);
}

void dae_fwd_data(KernelCtx& ctx, const DaeSpec& spec) {
  const int64_t S = ctx.in[0].len;
  const int64_t P = ctx.in[2].len;
  const int64_t W = 2 * S + P;
  const auto solution = solve<double, double, double>(
      spec, ctx.in[0].data, ctx.in[1].data, S, ctx.in[2].data, P);
  for (size_t n = 0; n < solution.size(); ++n)
    for (int64_t i = 0; i < S; ++i)
      ctx.out.data[(int64_t)n * S + i] = solution[n][i];
  for (int64_t i = 0; i < ctx.out.len * W; ++i) ctx.scratch[i] = 0.0;
}

// Keep the typed input containers alive while harvesting their adjoints.
template <bool YAutodiff, bool YpAutodiff, bool ThetaAutodiff>
void dae_fwd_active(KernelCtx& ctx, const DaeSpec& spec) {
  using T_y0 = std::conditional_t<YAutodiff, var, double>;
  using T_yp0 = std::conditional_t<YpAutodiff, var, double>;
  using T_theta = std::conditional_t<ThetaAutodiff, var, double>;
  const int64_t S = ctx.in[0].len, P = ctx.in[2].len, W = 2 * S + P;
  Eigen::Matrix<T_y0, Eigen::Dynamic, 1> y0(S);
  Eigen::Matrix<T_yp0, Eigen::Dynamic, 1> yp0(S);
  for (int64_t i = 0; i < S; ++i) {
    y0(i) = ctx.in[0].data[i];
    yp0(i) = ctx.in[1].data[i];
  }
  std::vector<T_theta> theta(ctx.in[2].data, ctx.in[2].data + P);
  stan::math::nested_rev_autodiff nested;
  const auto solution = stan::math::dae_tol(
      DaeResidual{&spec}, y0, yp0, spec.t0, spec.ts, spec.rtol, spec.atol,
      spec.max_steps, nullptr, theta, spec.x_r, spec.x_i);
  for (size_t n = 0; n < solution.size(); ++n)
    for (int64_t i = 0; i < S; ++i)
      ctx.out.data[(int64_t)n * S + i] = solution[n][i].val();
  double* J = ctx.scratch;
  stan::math::set_zero_all_adjoints_nested();
  for (int64_t o = ctx.out.len; o-- > 0;) {
    auto* output = solution[(size_t)(o / S)][(Eigen::Index)(o % S)].vi_;
    output->adj_ = 1.0;
    output->chain();
    for (int64_t i = 0; i < S; ++i) {
      if constexpr (YAutodiff) {
        J[o * W + i] = y0(i).adj();
        y0(i).vi_->adj_ = 0.0;
      } else {
        J[o * W + i] = 0.0;
      }
      if constexpr (YpAutodiff) {
        J[o * W + S + i] = yp0(i).adj();
        yp0(i).vi_->adj_ = 0.0;
      } else {
        J[o * W + S + i] = 0.0;
      }
    }
    for (int64_t i = 0; i < P; ++i) {
      if constexpr (ThetaAutodiff) {
        J[o * W + 2 * S + i] = theta[(size_t)i].adj();
        theta[(size_t)i].vi_->adj_ = 0.0;
      } else {
        J[o * W + 2 * S + i] = 0.0;
      }
    }
    output->adj_ = 0.0;
  }
}

void dae_fwd(KernelCtx& ctx) {
  const DaeSpec& spec = *static_cast<const DaeSpec*>(ctx.udata);
  const int64_t S = ctx.in[0].len, P = ctx.in[2].len;
  if (ctx.in[1].len != S)
    throw std::invalid_argument(
        "dae: initial state and derivative differ in size");
  if (values_only()) {
    const auto solution = solve<double, double, double>(
        spec, ctx.in[0].data, ctx.in[1].data, S, ctx.in[2].data, P);
    for (size_t n = 0; n < solution.size(); ++n)
      for (int64_t i = 0; i < S; ++i)
        ctx.out.data[(int64_t)n * S + i] = solution[n][i];
    return;
  }
  const uint8_t mask = (ctx.variant & 0x8u) != 0 ? (ctx.variant & 0x7u) : 0x7u;
  switch (mask) {
    case 0:
      dae_fwd_data(ctx, spec);
      break;
    case 1:
      dae_fwd_active<true, false, false>(ctx, spec);
      break;
    case 2:
      dae_fwd_active<false, true, false>(ctx, spec);
      break;
    case 3:
      dae_fwd_active<true, true, false>(ctx, spec);
      break;
    case 4:
      dae_fwd_active<false, false, true>(ctx, spec);
      break;
    case 5:
      dae_fwd_active<true, false, true>(ctx, spec);
      break;
    case 6:
      dae_fwd_active<false, true, true>(ctx, spec);
      break;
    default:
      dae_fwd_active<true, true, true>(ctx, spec);
      break;
  }
}

void dae_bwd(KernelCtx& ctx) {
  const uint8_t mask = (ctx.variant & 0x8u) != 0 ? (ctx.variant & 0x7u) : 0x7u;
  const int64_t S = ctx.in[0].len, P = ctx.in[2].len, W = 2 * S + P;
  for (int64_t o = ctx.out.len; o-- > 0;) {
    const double adjoint = ctx.out_adj_vec.data[o];
    if ((mask & 1u) && ctx.in_adj[0].data)
      for (int64_t i = 0; i < S; ++i)
        ctx.in_adj[0].data[i] += adjoint * ctx.scratch[o * W + i];
    if ((mask & 2u) && ctx.in_adj[1].data)
      for (int64_t i = 0; i < S; ++i)
        ctx.in_adj[1].data[i] += adjoint * ctx.scratch[o * W + S + i];
    if ((mask & 4u) && ctx.in_adj[2].data)
      for (int64_t i = 0; i < P; ++i)
        ctx.in_adj[2].data[i] += adjoint * ctx.scratch[o * W + 2 * S + i];
  }
}

int64_t dae_scratch(const Op& op, const Slot* slots) {
  const int64_t S = slots[op.in[0]].len;
  return slots[op.out].len * (2 * S + slots[op.in[2]].len);
}

}  // namespace

void register_dae_kernels() {
  register_kernel(OP_DAE, Kernel{dae_fwd, dae_bwd, dae_scratch});
}

}  // namespace stanli
