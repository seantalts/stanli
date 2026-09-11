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
#include <stanli/island.hpp>
#include <stanli/packet.hpp>
#include <stanli/mir_interp.hpp>
#include <stanli/ode.hpp>
#include <stanli/optable.hpp>

#include <stan/math.hpp>

#include <algorithm>
#include <boost/numeric/odeint.hpp>
#include <functional>
#include <limits>
#include <stdexcept>
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
  mutable RhsWorkspace workspace;

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
      run_rhs<T>(spec->prog, t, y, theta.data(), theta.size(), x_r.data(), out,
                 workspace.get<T>());
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

// in = {z_init, theta} for fixed times, or {z_init, theta, t0, ts} when
// modern solve times are runtime operands. Data x_r / x_i and tolerances live
// in the spec.
// out = N_ts * S, array-major (time outer, state inner), matching Stan's
// array[N, S] layout.
// The modern family's functor convention: an Eigen state in and out, and
// the variadic arguments after the message stream. It forwards to the
// same MirRhs, which is where the packed theta/x_r/x_i are split back
// into the right-hand side's declared parameters.
struct VarRhs {
  const OdeSpec* spec;
  mutable RhsWorkspace workspace;

  template <typename T_y, typename T_param>
  Eigen::Matrix<stan::return_type_t<T_y, T_param>, Eigen::Dynamic, 1>
  operator()(const double& t, const Eigen::Matrix<T_y, Eigen::Dynamic, 1>& y,
             std::ostream* msgs, const std::vector<T_param>& theta,
             const std::vector<double>& x_r,
             const std::vector<int>& x_i) const {
    using T = stan::return_type_t<T_y, T_param>;
    if (spec->prog.ok) {
      Eigen::Matrix<T, Eigen::Dynamic, 1> out(
          (Eigen::Index)spec->prog.out_regs.size());
      // Write the register outputs directly into Stan Math's required Eigen
      // return object. The vector-returning MirRhs entry remains the exact
      // interpreter fallback and compatibility path.
      run_rhs_into<T>(spec->prog, t, y.data(), theta.data(), theta.size(),
                      x_r.data(), out.data(), workspace.get<T>());
      return out;
    }
    const std::vector<T> dy =
        MirRhs{spec}.eval(t, y.data(), (size_t)y.size(), theta, x_r, x_i, msgs);
    Eigen::Matrix<T, Eigen::Dynamic, 1> out(dy.size());
    for (size_t i = 0; i < dy.size(); ++i) out(i) = dy[i];
    return out;
  }
};

// in = {z_init, theta} for fixed times, or {z_init, theta, t0, ts} when
// modern solve times are runtime operands. Data x_r / x_i and tolerances live
// in the spec.
// out = N_ts * S, array-major (time outer, state inner), matching Stan's
// array[N, S] layout.
template <typename T_y0, typename T_theta, typename T_t0, typename T_ts>
std::vector<std::vector<stan::return_type_t<T_y0, T_theta, T_t0, T_ts>>> solve(
    const OdeSpec& s, const std::vector<T_y0>& z0,
    const std::vector<T_theta>& theta, const T_t0& t0,
    const std::vector<T_ts>& ts) {
  using T = stan::return_type_t<T_y0, T_theta, T_t0, T_ts>;
  VarRhs f{&s};
  Eigen::Matrix<T_y0, Eigen::Dynamic, 1> y0((Eigen::Index)z0.size());
  for (size_t i = 0; i < z0.size(); ++i) y0((Eigen::Index)i) = z0[i];

  // The public integrate_ode_* wrappers adapt Eigen state to std::vector for
  // every RHS call and convert the result back. Our RHS already implements
  // the Eigen convention, so call the same underlying solver implementation
  // with the legacy function name retained for validation and error messages.
  // This changes only container marshalling, not the solver or its defaults.
  if (s.legacy) {
    std::vector<Eigen::Matrix<T, Eigen::Dynamic, 1>> res;
    switch (s.solver) {
      case OdeSpec::BDF:
        res = stan::math::ode_bdf_tol_impl("integrate_ode_bdf", f, y0, t0, ts,
                                           s.rtol, s.atol, s.max_steps, nullptr,
                                           theta, s.x_r, s.x_i);
        break;
      case OdeSpec::ADAMS:
        res = stan::math::ode_adams_tol_impl("integrate_ode_adams", f, y0, t0,
                                             ts, s.rtol, s.atol, s.max_steps,
                                             nullptr, theta, s.x_r, s.x_i);
        break;
      default:
        res = stan::math::ode_rk45_tol_impl("integrate_ode_rk45", f, y0, t0, ts,
                                            s.rtol, s.atol, s.max_steps,
                                            nullptr, theta, s.x_r, s.x_i);
        break;
    }
    std::vector<std::vector<T>> out;
    out.reserve(res.size());
    for (const auto& r : res) out.emplace_back(r.data(), r.data() + r.size());
    return out;
  }

  // Dispatch on the solver the model actually named. Mapping adams onto
  // bdf (or ckrk onto rk45) agrees to tolerance on an easy system and is
  // still the wrong integrator for the user who picked one for its
  // stability, so each gets its own call.
  std::vector<Eigen::Matrix<T, Eigen::Dynamic, 1>> res;
  switch (s.solver) {
    case OdeSpec::BDF:
      res = stan::math::ode_bdf_tol(f, y0, t0, ts, s.rtol, s.atol, s.max_steps,
                                    nullptr, theta, s.x_r, s.x_i);
      break;
    case OdeSpec::ADAMS:
      res =
          stan::math::ode_adams_tol(f, y0, t0, ts, s.rtol, s.atol, s.max_steps,
                                    nullptr, theta, s.x_r, s.x_i);
      break;
    case OdeSpec::CKRK:
      res = stan::math::ode_ckrk_tol(f, y0, t0, ts, s.rtol, s.atol, s.max_steps,
                                     nullptr, theta, s.x_r, s.x_i);
      break;
    default:
      res = stan::math::ode_rk45_tol(f, y0, t0, ts, s.rtol, s.atol, s.max_steps,
                                     nullptr, theta, s.x_r, s.x_i);
      break;
  }
  std::vector<std::vector<T>> out;
  out.reserve(res.size());
  for (const auto& r : res) out.emplace_back(r.data(), r.data() + r.size());
  return out;
}

// Mutable storage owned by one direct solve and reused by its RK callbacks.
// OdeSpec stays immutable and can be shared by concurrent solves.
struct DirectRkWorkspace {
  std::vector<double> values;
  std::vector<double> adjoints;
  std::vector<double> f;
  std::vector<double> J_y;
  std::vector<double> J_theta;
  std::vector<double> state;
  std::vector<double> times;
};

// Evaluate f, J_y and (when needed) J_theta without constructing a nested
// autodiff tape. The immutable payload is a clone of the canonical RHS with
// checkpoint saves for its generated reverse; the exact canonical program
// remains available to solve() as the oracle.
class DirectRkDerivative {
 public:
  DirectRkDerivative(const OdeSpec& spec, size_t theta_source,
                     DirectRkWorkspace& workspace)
      : rhs_(spec.prog),
        generated_(*spec.direct_rk),
        theta_source_(theta_source),
        workspace_(workspace) {}

  template <bool ThetaAutodiff>
  void evaluate(double t, const double* y, const double* theta,
                const double* x_r) {
    workspace_.values.resize((size_t)generated_.n_regs);
    workspace_.adjoints.resize((size_t)generated_.adj.n_regs);
    workspace_.f.resize((size_t)rhs_.n_y);
    workspace_.J_y.resize((size_t)rhs_.n_y * (size_t)rhs_.n_y);
    if constexpr (ThetaAutodiff) {
      workspace_.J_theta.resize((size_t)rhs_.n_y * theta_source_);
      std::fill(workspace_.J_theta.begin(), workspace_.J_theta.end(), 0.0);
    }

    detail::seed_rhs_regs<double>(rhs_, t, y, theta, theta_source_, x_r,
                                  workspace_.values);

    run_program(static_cast<const Program&>(generated_), workspace_.values);
    for (size_t i = 0; i < generated_.out_regs.size(); ++i)
      workspace_.f[i] = workspace_.values[(size_t)generated_.out_regs[i]];

    const AdjProgram& reverse = generated_.adj;
    for (size_t output = 0; output < generated_.out_regs.size(); ++output) {
      std::fill(workspace_.adjoints.begin(), workspace_.adjoints.end(), 0.0);
      const int output_reg = generated_.out_regs[output];
      workspace_.adjoints[(size_t)reverse.adj_reg[(size_t)output_reg]] = 1.0;
      run_adjoint(generated_, reverse, workspace_.values.data(),
                  workspace_.adjoints.data());
      for (int input = 0; input < rhs_.n_y; ++input) {
        const int reg = rhs_.y0 + input;
        workspace_.J_y[output * (size_t)rhs_.n_y + (size_t)input] =
            workspace_.adjoints[(size_t)reverse.adj_reg[(size_t)reg]];
      }
      if constexpr (ThetaAutodiff) {
        for (int input = 0; input < rhs_.n_th; ++input) {
          const int reg = rhs_.th0 + input;
          workspace_.J_theta[output * theta_source_ + (size_t)input] =
              workspace_.adjoints[(size_t)reverse.adj_reg[(size_t)reg]];
        }
      }
    }
  }

 private:
  const RhsProgram& rhs_;
  const IslandProg& generated_;
  size_t theta_source_;
  DirectRkWorkspace& workspace_;
};

// The state layout and arithmetic grouping are the pinned Stan Math coupled
// system's: base states, one S-vector per active y0 lane, then one per active
// theta lane. In particular the parameter lane starts with J_theta and adds
// the J_y product left-to-right, which is observable in the last bit.
template <bool YAutodiff, bool ThetaAutodiff>
class DirectRkSystem {
 public:
  DirectRkSystem(const OdeSpec& spec, size_t states, size_t theta_source,
                 const double* theta, DirectRkWorkspace& workspace)
      : derivative_(spec, theta_source, workspace),
        states_(states),
        theta_source_(theta_source),
        theta_(theta),
        x_r_(spec.x_r.data()),
        workspace_(workspace) {}

  void operator()(const std::vector<double>& z, std::vector<double>& dz,
                  double t) const {
    const size_t y_lanes = YAutodiff ? states_ : 0;
    const size_t theta_lanes = ThetaAutodiff ? theta_source_ : 0;
    const size_t expected = states_ * (1 + y_lanes + theta_lanes);
    if (z.size() != expected)
      throw std::runtime_error("coupled RK state has unexpected size");
    dz.resize(expected);

    derivative_.template evaluate<ThetaAutodiff>(t, z.data(), theta_, x_r_);
    for (size_t output = 0; output < states_; ++output) {
      dz[output] = workspace_.f[output];
      if constexpr (YAutodiff) {
        for (size_t lane = 0; lane < states_; ++lane) {
          double derivative = 0.0;
          for (size_t input = 0; input < states_; ++input)
            derivative += z[states_ + states_ * lane + input] *
                          workspace_.J_y[output * states_ + input];
          dz[states_ + states_ * lane + output] = derivative;
        }
      }
      if constexpr (ThetaAutodiff) {
        for (size_t lane = 0; lane < theta_source_; ++lane) {
          double derivative = workspace_.J_theta[output * theta_source_ + lane];
          for (size_t input = 0; input < states_; ++input)
            derivative +=
                z[states_ + states_ * y_lanes + states_ * lane + input] *
                workspace_.J_y[output * states_ + input];
          dz[states_ + states_ * y_lanes + states_ * lane + output] =
              derivative;
        }
      }
    }
  }

 private:
  mutable DirectRkDerivative derivative_;
  size_t states_;
  size_t theta_source_;
  const double* theta_;
  const double* x_r_;
  DirectRkWorkspace& workspace_;
};

const char* direct_rk_function_name(const OdeSpec& spec) {
  if (spec.legacy) return "integrate_ode_rk45";
  return spec.solver == OdeSpec::CKRK ? "ode_ckrk_tol" : "ode_rk45_tol";
}

void validate_direct_rk(const char* function_name, const OdeSpec& spec,
                        const double* y0, size_t states, const double* theta,
                        size_t theta_source) {
  const Eigen::Map<const Eigen::VectorXd> y0_map(y0, (Eigen::Index)states);
  const Eigen::Map<const Eigen::VectorXd> theta_map(theta,
                                                    (Eigen::Index)theta_source);
  stan::math::check_finite(function_name, "initial state", y0_map);
  stan::math::check_finite(function_name, "initial time", spec.t0);
  stan::math::check_finite(function_name, "times", spec.ts);
  stan::math::check_finite(function_name, "ode parameters and data", theta_map);
  stan::math::check_finite(function_name, "ode parameters and data", spec.x_r);
  stan::math::check_finite(function_name, "ode parameters and data", spec.x_i);
  stan::math::check_nonzero_size(function_name, "initial state", y0_map);
  stan::math::check_nonzero_size(function_name, "times", spec.ts);
  stan::math::check_sorted(function_name, "times", spec.ts);
  stan::math::check_less(function_name, "initial time", spec.t0, spec.ts[0]);
  stan::math::check_positive_finite(function_name, "relative_tolerance",
                                    spec.rtol);
  stan::math::check_positive_finite(function_name, "absolute_tolerance",
                                    spec.atol);
  stan::math::check_positive(function_name, "max_num_steps", spec.max_steps);
}

template <bool YAutodiff, bool ThetaAutodiff>
bool direct_rk_shape_ok(const KernelCtx& ctx, const OdeSpec& spec) {
  if (ctx.in[0].len <= 0 || ctx.in[1].len < 0 || ctx.out.len < 0 ||
      spec.prog.n_y < 0 || spec.prog.n_th < 0 || spec.prog.n_xr < 0 ||
      spec.direct_rk->n_regs < 0 || spec.direct_rk->adj.n_regs < 0)
    return false;
  const size_t states = (size_t)ctx.in[0].len;
  const size_t theta_source = (size_t)ctx.in[1].len;
  const size_t max = std::numeric_limits<size_t>::max();
  if ((size_t)spec.prog.n_y != states ||
      (size_t)spec.prog.n_th > theta_source ||
      (size_t)spec.prog.n_xr != spec.x_r.size() ||
      spec.prog.out_regs.size() != states ||
      spec.direct_rk->out_regs.size() != states ||
      (spec.ts.size() != 0 && states > max / spec.ts.size()) ||
      (size_t)ctx.out.len != spec.ts.size() * states ||
      states > max - theta_source)
    return false;
  const size_t width = states + theta_source;
  const size_t y_lanes = YAutodiff ? states : 0;
  const size_t theta_lanes = ThetaAutodiff ? theta_source : 0;
  if (y_lanes > max - theta_lanes || 1 > max - y_lanes - theta_lanes)
    return false;
  const size_t lane_count = 1 + y_lanes + theta_lanes;
  if (states > max / states || states > max / lane_count ||
      (theta_source != 0 && states > max / theta_source) ||
      (size_t)ctx.out.len > max / std::max<size_t>(1, width))
    return false;
  return true;
}

template <bool YAutodiff, bool ThetaAutodiff>
void solve_direct_rk(KernelCtx& ctx, const OdeSpec& spec) {
  using boost::numeric::odeint::integrate_times;
  using boost::numeric::odeint::make_controlled;
  using boost::numeric::odeint::make_dense_output;
  using boost::numeric::odeint::max_step_checker;
  using boost::numeric::odeint::no_progress_error;
  using boost::numeric::odeint::runge_kutta_cash_karp54;
  using boost::numeric::odeint::runge_kutta_dopri5;

  const size_t states = (size_t)ctx.in[0].len;
  const size_t theta_source = (size_t)ctx.in[1].len;
  const size_t width = states + theta_source;
  const size_t y_lanes = YAutodiff ? states : 0;
  const size_t theta_lanes = ThetaAutodiff ? theta_source : 0;
  const size_t coupled_size = states * (1 + y_lanes + theta_lanes);
  const char* function_name = direct_rk_function_name(spec);

  validate_direct_rk(function_name, spec, ctx.in[0].data, states,
                     ctx.in[1].data, theta_source);
  if ((size_t)ctx.out.len != spec.ts.size() * states)
    throw std::runtime_error("OP_ODE output shape does not match solve times");

  DirectRkWorkspace workspace;
  workspace.state.assign(coupled_size, 0.0);
  for (size_t i = 0; i < states; ++i) workspace.state[i] = ctx.in[0].data[i];
  if constexpr (YAutodiff)
    for (size_t i = 0; i < states; ++i)
      workspace.state[states + states * i + i] = 1.0;

  // Inactive columns are observable scratch, even though ode_bwd will not
  // scatter them. Zeroing the complete matrix keeps the established contract.
  std::fill(ctx.scratch, ctx.scratch + (size_t)ctx.out.len * width, 0.0);

  workspace.times.resize(spec.ts.size() + 1);
  workspace.times[0] = spec.t0;
  std::copy(spec.ts.begin(), spec.ts.end(), workspace.times.begin() + 1);
  DirectRkSystem<YAutodiff, ThetaAutodiff> system(spec, states, theta_source,
                                                  ctx.in[1].data, workspace);
  bool initial_observed = false;
  size_t time_index = 0;
  auto observer = [&](const std::vector<double>& state, double) {
    if (!initial_observed) {
      initial_observed = true;
      return;
    }
    if (time_index >= spec.ts.size())
      throw std::runtime_error("direct RK observer produced too many states");
    for (size_t output_state = 0; output_state < states; ++output_state) {
      const size_t output = time_index * states + output_state;
      ctx.out.data[output] = state[output_state];
      if constexpr (YAutodiff)
        for (size_t lane = 0; lane < states; ++lane)
          ctx.scratch[output * width + lane] =
              state[states + states * lane + output_state];
      if constexpr (ThetaAutodiff)
        for (size_t lane = 0; lane < theta_source; ++lane)
          ctx.scratch[output * width + states + lane] =
              state[states + states * y_lanes + states * lane + output_state];
    }
    ++time_index;
  };

  try {
    if (spec.solver == OdeSpec::RK45) {
      integrate_times(
          make_dense_output(spec.atol, spec.rtol,
                            runge_kutta_dopri5<std::vector<double>, double,
                                               std::vector<double>, double>()),
          std::ref(system), workspace.state, workspace.times.begin(),
          workspace.times.end(), 0.1, observer,
          max_step_checker(spec.max_steps));
    } else {
      integrate_times(
          make_controlled(
              spec.atol, spec.rtol,
              runge_kutta_cash_karp54<std::vector<double>, double,
                                      std::vector<double>, double>()),
          std::ref(system), workspace.state, workspace.times.begin(),
          workspace.times.end(), 0.1, observer,
          max_step_checker(spec.max_steps));
    }
  } catch (const no_progress_error&) {
    stan::math::throw_domain_error(function_name, "",
                                   workspace.times[time_index + 1],
                                   "Failed to integrate to next output time (",
                                   ") in less than max_num_steps steps");
  }
  if (time_index != spec.ts.size())
    throw std::runtime_error("direct RK observer produced too few states");
}

// New calls carry {y0, theta, t0, ts}. Bit 4 marks the four-bit scalar-type
// mask: y0, theta, t0, and ts. Older two-input graphs retain their historical
// bit-2 encoding and read times from OdeSpec.
template <bool YAutodiff, bool ThetaAutodiff, bool T0Autodiff, bool TsAutodiff>
void ode_fwd_typed(KernelCtx& ctx, const OdeSpec& s) {
  using T_y0 = std::conditional_t<YAutodiff, var, double>;
  using T_theta = std::conditional_t<ThetaAutodiff, var, double>;
  using T_t0 = std::conditional_t<T0Autodiff, var, double>;
  using T_ts = std::conditional_t<TsAutodiff, var, double>;
  const bool runtime_times = ctx.n_in >= 4;
  const int64_t S = ctx.in[0].len, P = ctx.in[1].len;
  const int64_t N = runtime_times ? ctx.in[3].len : (int64_t)s.ts.size();
  const int64_t W = S + P + (runtime_times ? 1 + N : 0);
  double* J = ctx.scratch;
  const double t0_value = runtime_times ? ctx.in[2].data[0] : s.t0;
  const double* ts_values = runtime_times ? ctx.in[3].data : s.ts.data();

  if constexpr (!YAutodiff && !ThetaAutodiff && !T0Autodiff && !TsAutodiff) {
    // A data-only solve has no reason to construct a nested reverse-mode tape.
    std::vector<T_y0> z0(ctx.in[0].data, ctx.in[0].data + S);
    std::vector<T_theta> th(ctx.in[1].data, ctx.in[1].data + P);
    const std::vector<double> ts(ts_values, ts_values + N);
    const auto solv = solve(s, z0, th, t0_value, ts);
    for (size_t n = 0; n < solv.size(); ++n)
      for (int64_t k = 0; k < S; ++k)
        ctx.out.data[(int64_t)n * S + k] = solv[n][k];
    for (int64_t i = 0; i < ctx.out.len * W; ++i) J[i] = 0.0;
  } else {
    // The lowering-time switch keeps the exact current solve as a same-binary
    // oracle without an environment lookup in this repeated kernel. A payload
    // is present only when generated differentiation refused no opcode.
    if constexpr (!T0Autodiff && !TsAutodiff) {
      if (s.direct_rk_enabled && s.direct_rk && !runtime_times &&
          (s.solver == OdeSpec::RK45 || s.solver == OdeSpec::CKRK) &&
          direct_rk_shape_ok<YAutodiff, ThetaAutodiff>(ctx, s)) {
        solve_direct_rk<YAutodiff, ThetaAutodiff>(ctx, s);
        return;
      }
    }
    stan::math::nested_rev_autodiff nested;
    std::vector<T_y0> z0(ctx.in[0].data, ctx.in[0].data + S);
    std::vector<T_theta> th(ctx.in[1].data, ctx.in[1].data + P);
    T_t0 t0 = t0_value;
    std::vector<T_ts> ts(ts_values, ts_values + N);
    const auto solv = solve(s, z0, th, t0, ts);
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
      if (runtime_times) {
        if constexpr (T0Autodiff) {
          J[o * W + S + P] = t0.adj();
          t0.vi_->adj_ = 0.0;
        } else {
          J[o * W + S + P] = 0.0;
        }
        if constexpr (TsAutodiff) {
          for (int64_t i = 0; i < N; ++i) {
            J[o * W + S + P + 1 + i] = ts[(size_t)i].adj();
            ts[(size_t)i].vi_->adj_ = 0.0;
          }
        } else {
          for (int64_t i = 0; i < N; ++i) J[o * W + S + P + 1 + i] = 0.0;
        }
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
    const double t0 = ctx.n_in >= 4 ? ctx.in[2].data[0] : s.t0;
    const std::vector<double> ts =
        ctx.n_in >= 4 ? std::vector<double>(ctx.in[3].data,
                                            ctx.in[3].data + ctx.in[3].len)
                      : s.ts;
    const auto solv = solve(s, z0, th, t0, ts);
    for (size_t n = 0; n < solv.size(); ++n)
      for (int64_t k = 0; k < S; ++k)
        ctx.out.data[(int64_t)n * S + k] = solv[n][k];
    return;
  }
  const uint8_t type_mask =
      (ctx.variant & 0x10u) != 0
          ? (ctx.variant & 0x0fu)
          : ((ctx.variant & 0x4u) != 0 ? (ctx.variant & 0x3u) : 0x3u);
  const bool t0_autodiff = (type_mask & 0x4u) != 0;
  const bool ts_autodiff = (type_mask & 0x8u) != 0;
#define STANLI_ODE_FWD_TIMES(Y, TH)               \
  if (t0_autodiff) {                              \
    if (ts_autodiff)                              \
      ode_fwd_typed<Y, TH, true, true>(ctx, s);   \
    else                                          \
      ode_fwd_typed<Y, TH, true, false>(ctx, s);  \
  } else {                                        \
    if (ts_autodiff)                              \
      ode_fwd_typed<Y, TH, false, true>(ctx, s);  \
    else                                          \
      ode_fwd_typed<Y, TH, false, false>(ctx, s); \
  }
  if ((type_mask & 0x3u) == 0x3u) {
    STANLI_ODE_FWD_TIMES(true, true);
  } else if ((type_mask & 0x3u) == 0x1u) {
    STANLI_ODE_FWD_TIMES(true, false);
  } else if ((type_mask & 0x3u) == 0x2u) {
    STANLI_ODE_FWD_TIMES(false, true);
  } else {
    STANLI_ODE_FWD_TIMES(false, false);
  }
#undef STANLI_ODE_FWD_TIMES
}

void ode_bwd(KernelCtx& ctx) {
  const uint8_t type_mask =
      (ctx.variant & 0x10u) != 0
          ? (ctx.variant & 0x0fu)
          : ((ctx.variant & 0x4u) != 0 ? (ctx.variant & 0x3u) : 0x3u);
  const bool y_active = (type_mask & 0x1u) != 0 && ctx.in_adj[0].data;
  const bool theta_active = (type_mask & 0x2u) != 0 && ctx.in_adj[1].data;
  const bool t0_active =
      ctx.n_in >= 4 && (type_mask & 0x4u) != 0 && ctx.in_adj[2].data;
  const bool ts_active =
      ctx.n_in >= 4 && (type_mask & 0x8u) != 0 && ctx.in_adj[3].data;
  if (!y_active && !theta_active && !t0_active && !ts_active) return;
  const int64_t S = ctx.in[0].len, P = ctx.in[1].len;
  const int64_t N = ctx.n_in >= 4 ? ctx.in[3].len : 0;
  const int64_t W = S + P + (ctx.n_in >= 4 ? 1 + N : 0);
  const double* J = ctx.scratch;
  for (int64_t o = ctx.out.len; o-- > 0;) {
    const double a = ctx.out_adj_vec.data[o];
    if (y_active)
      for (int64_t i = 0; i < S; ++i) ctx.in_adj[0].data[i] += a * J[o * W + i];
    if (theta_active)
      for (int64_t i = 0; i < P; ++i)
        ctx.in_adj[1].data[i] += a * J[o * W + S + i];
    if (t0_active) ctx.in_adj[2].data[0] += a * J[o * W + S + P];
    if (ts_active)
      for (int64_t i = 0; i < N; ++i)
        ctx.in_adj[3].data[i] += a * J[o * W + S + P + 1 + i];
  }
}

int64_t ode_scratch(const Op& op, const Slot* slots) {
  int64_t width = slots[op.in[0]].len + slots[op.in[1]].len;
  if (op.n_in >= 4) width += 1 + slots[op.in[3]].len;
  return slots[op.out].len * width;
}

}  // namespace

void register_ode_kernels() {
  register_kernel(OP_ODE, Kernel{ode_fwd, ode_bwd, ode_scratch});
}

}  // namespace stanli
