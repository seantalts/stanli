// ODE integrator ops. stan-math does the solving and the sensitivities; the
// right-hand side is the model's own user-defined function, evaluated at
// runtime by a compiled register program (with the MIR interpreter as its
// fallback) because the integrator picks the times, so the body cannot be
// inlined at compile time.
//
// One solve per gradient, in the forward sweep, with the solution's jacobian
// stashed for the backward one.
//
// A differentiated forward pass has to solve the states plus the active
// sensitivities rather than the plain state system: the adaptive step
// controller sees the coupled error estimate, so at the loose tolerances
// these models use a states-only solve lands on visibly different values
// (measured 3e-2 relative on lotka_volterra, whose atol is 1e-3). Since it
// pays for those sensitivities anyway, it keeps them -- the backward is then
// a matrix-vector product instead of a second solve. A fully data-only call
// has no sensitivities and takes the plain double solve.
//
// Reading them out is cheap. stan-math integrates the coupled system on
// doubles and builds each solution element as one precomputed-gradient vari
// directly connected to the active inputs. Chaining that selected output node
// yields one Jacobian row without walking its sibling output nodes. After one
// initial tape reset, only that output and those inputs need clearing between
// rows; the graph backward later applies the rows in Stan's reverse order.
#include <stanli/graph.hpp>
#include <stanli/packet.hpp>
#include <stanli/mir_interp.hpp>
#include <stanli/ode.hpp>
#include <stanli/optable.hpp>

#include <stan/math.hpp>

#include <type_traits>
#include <vector>

namespace stanli {
namespace {

using stan::math::var;

// Adapter presented to stan-math: evaluates the right-hand side for whatever
// scalar type the integrator instantiates. The compiled program when there is
// one, the MIR interpreter when there is not.
struct MirRhs {
  const OdeSpec* spec;

  template <typename T_y, typename T_param>
  std::vector<stan::return_type_t<T_y, T_param>> eval(
      const double& t, const T_y* y, size_t n_y,
      const std::vector<T_param>& theta, const std::vector<double>& x_r,
      const std::vector<int>& x_i, std::ostream* msgs = nullptr) const {
    using T = stan::return_type_t<T_y, T_param>;
    if (spec->prog.ok) {
      // Seed mixed scalar inputs directly into the result-typed register file.
      // theta.size(), rather than prog.n_th, retains promotion of lowering's
      // unread no-parameter placeholder in the old tape position.
      std::vector<T> out;
      run_rhs<T>(spec->prog, t, y, theta.data(), theta.size(), x_r.data(), out);
      return out;
    }
    // Preserve the interpreter adapter exactly. The modern caller used to
    // make this state vector before entering MirRhs; doing it here keeps the
    // fallback's ownership and evaluation path unchanged.
    std::vector<T_y> state;
    if (n_y != 0) state.assign(y, y + n_y);
    return (*this)(t, state, theta, x_r, x_i, msgs);
  }

  template <typename T_y, typename T_param>
  std::vector<stan::return_type_t<T_y, T_param>> operator()(
      const double& t, const std::vector<T_y>& y,
      const std::vector<T_param>& theta, const std::vector<double>& x_r,
      const std::vector<int>& x_i, std::ostream* msgs = nullptr) const {
    using T = stan::return_type_t<T_y, T_param>;
    if (spec->prog.ok)
      return eval(t, y.data(), y.size(), theta, x_r, x_i, msgs);
    // Rebuild the formal argument list the right-hand side declares.
    // MirInterp::call binds positionally by declared type, so the real
    // arguments have to arrive already split out of the packed theta and
    // x_r -- in the same order compile_rhs_args assigned their register
    // ranges, which is the order spec->args records.
    std::vector<std::vector<T>> reals{{T(t)},
                                      std::vector<T>(y.begin(), y.end())};
    std::vector<std::vector<int>> ints;
    size_t th_at = 0, xr_at = 0;
    for (const RhsArg& a : spec->args) {
      if (a.is_int) {
        ints.push_back(a.ints);
      } else if (a.is_param) {
        reals.emplace_back(theta.begin() + th_at,
                           theta.begin() + th_at + a.len);
        th_at += (size_t)a.len;
      } else {
        reals.emplace_back(x_r.begin() + xr_at, x_r.begin() + xr_at + a.len);
        xr_at += (size_t)a.len;
      }
    }
    MirInterp<T> ev(*spec->funs(), "ODE function");
    return ev.call(*spec->rhs(), reals, ints);
  }
};

// in = {z_init, theta}; data ts / x_r / x_i and tolerances live in the spec.
// out = N_ts * S, array-major (time outer, state inner), matching Stan's
// array[N, S] layout.
// The modern family's functor convention: an Eigen state in and out, and
// the variadic arguments after the message stream. It forwards to the
// same MirRhs, which is where the packed theta/x_r/x_i are split back
// into the right-hand side's declared parameters.
struct VarRhs {
  const OdeSpec* spec;

  template <typename T_y, typename T_param>
  Eigen::Matrix<stan::return_type_t<T_y, T_param>, Eigen::Dynamic, 1>
  operator()(const double& t, const Eigen::Matrix<T_y, Eigen::Dynamic, 1>& y,
             std::ostream*, const std::vector<T_param>& theta,
             const std::vector<double>& x_r,
             const std::vector<int>& x_i) const {
    using T = stan::return_type_t<T_y, T_param>;
    const std::vector<T> dy =
        MirRhs{spec}.eval(t, y.data(), (size_t)y.size(), theta, x_r, x_i);
    Eigen::Matrix<T, Eigen::Dynamic, 1> out(dy.size());
    for (size_t i = 0; i < dy.size(); ++i) out(i) = dy[i];
    return out;
  }
};

// in = {z_init, theta}; data ts / x_r / x_i and tolerances live in the spec.
// out = N_ts * S, array-major (time outer, state inner), matching Stan's
// array[N, S] layout.
template <typename T_y0, typename T_theta>
std::vector<std::vector<stan::return_type_t<T_y0, T_theta>>> solve(
    const OdeSpec& s, const std::vector<T_y0>& z0,
    const std::vector<T_theta>& theta) {
  using T = stan::return_type_t<T_y0, T_theta>;
  // The deprecated entry points, unchanged: the four corpus ODE models
  // are verified against exactly this call.
  if (s.legacy) {
    MirRhs f{&s};
    if (s.stiff)
      return stan::math::integrate_ode_bdf(f, z0, s.t0, s.ts, theta, s.x_r,
                                           s.x_i, nullptr, s.rtol, s.atol,
                                           s.max_steps);
    return stan::math::integrate_ode_rk45(f, z0, s.t0, s.ts, theta, s.x_r,
                                          s.x_i, nullptr, s.rtol, s.atol,
                                          s.max_steps);
  }

  VarRhs f{&s};
  Eigen::Matrix<T_y0, Eigen::Dynamic, 1> y0((Eigen::Index)z0.size());
  for (size_t i = 0; i < z0.size(); ++i) y0((Eigen::Index)i) = z0[i];
  // Dispatch on the solver the model actually named. Mapping adams onto
  // bdf (or ckrk onto rk45) agrees to tolerance on an easy system and is
  // still the wrong integrator for the user who picked one for its
  // stability, so each gets its own call.
  std::vector<Eigen::Matrix<T, Eigen::Dynamic, 1>> res;
  switch (s.solver) {
    case OdeSpec::BDF:
      res = stan::math::ode_bdf_tol(f, y0, s.t0, s.ts, s.rtol, s.atol,
                                    s.max_steps, nullptr, theta, s.x_r, s.x_i);
      break;
    case OdeSpec::ADAMS:
      res =
          stan::math::ode_adams_tol(f, y0, s.t0, s.ts, s.rtol, s.atol,
                                    s.max_steps, nullptr, theta, s.x_r, s.x_i);
      break;
    case OdeSpec::CKRK:
      res = stan::math::ode_ckrk_tol(f, y0, s.t0, s.ts, s.rtol, s.atol,
                                     s.max_steps, nullptr, theta, s.x_r, s.x_i);
      break;
    default:
      res = stan::math::ode_rk45_tol(f, y0, s.t0, s.ts, s.rtol, s.atol,
                                     s.max_steps, nullptr, theta, s.x_r, s.x_i);
      break;
  }
  std::vector<std::vector<T>> out;
  out.reserve(res.size());
  for (const auto& r : res) out.emplace_back(r.data(), r.data() + r.size());
  return out;
}

// Solve with the scalar types selected at lowering. OP_ODE variant bit 2 marks
// an explicit type mask: low bit y0, next bit theta (1 = var). Variant zero is
// the compatibility encoding for hand-built graphs and means the former
// both-var behavior.
// The scratch layout remains [y0 columns, theta columns] for every activity
// combination; inactive columns are explicit zeros for deterministic scratch,
// while ode_bwd gates their scatter with the same type mask.
template <bool YAutodiff, bool ThetaAutodiff>
void ode_fwd_typed(KernelCtx& ctx, const OdeSpec& s) {
  using T_y0 = std::conditional_t<YAutodiff, var, double>;
  using T_theta = std::conditional_t<ThetaAutodiff, var, double>;
  const int64_t S = ctx.in[0].len, P = ctx.in[1].len, W = S + P;
  double* J = ctx.scratch;

  if constexpr (!YAutodiff && !ThetaAutodiff) {
    // A data-only solve has no reason to construct a nested reverse-mode tape.
    std::vector<T_y0> z0(ctx.in[0].data, ctx.in[0].data + S);
    std::vector<T_theta> th(ctx.in[1].data, ctx.in[1].data + P);
    const auto solv = solve(s, z0, th);
    for (size_t n = 0; n < solv.size(); ++n)
      for (int64_t k = 0; k < S; ++k)
        ctx.out.data[(int64_t)n * S + k] = solv[n][k];
    for (int64_t i = 0; i < ctx.out.len * W; ++i) J[i] = 0.0;
  } else {
    stan::math::nested_rev_autodiff nested;
    std::vector<T_y0> z0(ctx.in[0].data, ctx.in[0].data + S);
    std::vector<T_theta> th(ctx.in[1].data, ctx.in[1].data + P);
    const auto solv = solve(s, z0, th);
    for (size_t n = 0; n < solv.size(); ++n)
      for (int64_t k = 0; k < S; ++k)
        ctx.out.data[(int64_t)n * S + k] = solv[n][k].val();

    // d(solution)/d(z_init, theta), row per flattened solution element.
    // Harvest last-to-first to retain the row order ode_bwd and the former
    // all-var solve used. Each solution is a precomputed-gradient node whose
    // one chain call writes this raw row directly to the active inputs.
    stan::math::set_zero_all_adjoints_nested();
    for (int64_t o = ctx.out.len; o-- > 0;) {
      auto* output = solv[(size_t)(o / S)][(size_t)(o % S)].vi_;
      output->adj_ = 1.0;
      output->chain();
      if constexpr (YAutodiff) {
        for (int64_t i = 0; i < S; ++i) {
          J[o * W + i] = z0[(size_t)i].adj();
          z0[(size_t)i].vi_->adj_ = 0.0;
        }
      } else {
        for (int64_t i = 0; i < S; ++i) J[o * W + i] = 0.0;
      }
      if constexpr (ThetaAutodiff) {
        for (int64_t i = 0; i < P; ++i) {
          J[o * W + S + i] = th[(size_t)i].adj();
          th[(size_t)i].vi_->adj_ = 0.0;
        }
      } else {
        for (int64_t i = 0; i < P; ++i) J[o * W + S + i] = 0.0;
      }
      output->adj_ = 0.0;
    }
  }
}

void ode_fwd(KernelCtx& ctx) {
  const OdeSpec& s = *static_cast<const OdeSpec*>(ctx.udata);
  const int64_t S = ctx.in[0].len, P = ctx.in[1].len;
  // The value alone: solve the states, skip the sensitivities and the
  // jacobian nobody is going to read. This is what CmdStan's
  // log_prob<double> does, and at a solution grazing zero it is a
  // different answer from the coupled solve below -- the step controller
  // sees different error estimates. Matching it is the point: it decides
  // which initial points are valid.
  if (values_only()) {
    const std::vector<double> z0(ctx.in[0].data, ctx.in[0].data + S);
    const std::vector<double> th(ctx.in[1].data, ctx.in[1].data + P);
    const auto solv = solve(s, z0, th);
    for (size_t n = 0; n < solv.size(); ++n)
      for (int64_t k = 0; k < S; ++k)
        ctx.out.data[(int64_t)n * S + k] = solv[n][k];
    return;
  }
  const uint8_t type_mask =
      (ctx.variant & 0x4u) != 0 ? (ctx.variant & 0x3u) : 0x3u;
  if (type_mask == 0x3u)
    ode_fwd_typed<true, true>(ctx, s);
  else if (type_mask == 0x1u)
    ode_fwd_typed<true, false>(ctx, s);
  else if (type_mask == 0x2u)
    ode_fwd_typed<false, true>(ctx, s);
  else
    ode_fwd_typed<false, false>(ctx, s);
}

void ode_bwd(KernelCtx& ctx) {
  const uint8_t type_mask =
      (ctx.variant & 0x4u) != 0 ? (ctx.variant & 0x3u) : 0x3u;
  const bool y_active = (type_mask & 0x1u) != 0 && ctx.in_adj[0].data;
  const bool theta_active = (type_mask & 0x2u) != 0 && ctx.in_adj[1].data;
  if (!y_active && !theta_active) return;
  const int64_t S = ctx.in[0].len, P = ctx.in[1].len, W = S + P;
  const double* J = ctx.scratch;
  for (int64_t o = ctx.out.len; o-- > 0;) {
    const double a = ctx.out_adj_vec.data[o];
    if (y_active)
      for (int64_t i = 0; i < S; ++i) ctx.in_adj[0].data[i] += a * J[o * W + i];
    if (theta_active)
      for (int64_t i = 0; i < P; ++i)
        ctx.in_adj[1].data[i] += a * J[o * W + S + i];
  }
}

int64_t ode_scratch(const Op& op, const Slot* slots) {
  return slots[op.out].len * (slots[op.in[0]].len + slots[op.in[1]].len);
}

}  // namespace

void register_ode_kernels() {
  register_kernel(OP_ODE, Kernel{ode_fwd, ode_bwd, ode_scratch});
}

}  // namespace stanli
