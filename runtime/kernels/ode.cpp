// ODE integrator ops. stan-math does the solving and the sensitivities; the
// right-hand side is the model's own user-defined function, evaluated at
// runtime by the MIR interpreter (see mir_interp.hpp) because the integrator
// picks the times, so the body cannot be inlined at compile time.
//
// One solve per gradient, in the forward sweep, with the solution's jacobian
// stashed for the backward one.
//
// The forward pass has to solve the coupled system (states plus
// sensitivities) rather than the plain state system: the adaptive step
// controller sees the coupled error estimate, so at the loose tolerances
// these models use a states-only solve lands on visibly different values
// (measured 3e-2 relative on lotka_volterra, whose atol is 1e-3). Since it
// pays for the sensitivities anyway, it may as well keep them -- the
// backward is then a matrix-vector product instead of a second solve.
//
// Reading them out is cheap. stan-math integrates the coupled system on
// doubles and only builds precomputed-gradient varis for the solution
// itself, so the nested tape left standing after a solve holds the inputs
// and the outputs and nothing else; one reverse sweep per output element
// walks a few dozen varis, against several hundred right-hand-side
// evaluations for a solve.
#include <stanli/graph.hpp>
#include <stanli/packet.hpp>
#include <stanli/mir_interp.hpp>
#include <stanli/ode.hpp>
#include <stanli/optable.hpp>

#include <stan/math.hpp>

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
  std::vector<stan::return_type_t<T_y, T_param>> operator()(
      const double& t, const std::vector<T_y>& y,
      const std::vector<T_param>& theta, const std::vector<double>& x_r,
      const std::vector<int>& x_i, std::ostream* = nullptr) const {
    using T = stan::return_type_t<T_y, T_param>;
    if (spec->prog.ok) {
      // y and theta arrive as T_y / T_param, which are T or double; the
      // register file is T, so promote through a small staging buffer only
      // when they differ.
      std::vector<T> out, ys(y.begin(), y.end()),
          ths(theta.begin(), theta.end());
      run_rhs<T>(spec->prog, T(t), ys.data(), ths.data(), x_r.data(), out);
      return out;
    }
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
    const std::vector<T_y> yv(y.data(), y.data() + y.size());
    const std::vector<T> dy = MirRhs{spec}(t, yv, theta, x_r, x_i);
    Eigen::Matrix<T, Eigen::Dynamic, 1> out(dy.size());
    for (size_t i = 0; i < dy.size(); ++i) out(i) = dy[i];
    return out;
  }
};

// in = {z_init, theta}; data ts / x_r / x_i and tolerances live in the spec.
// out = N_ts * S, array-major (time outer, state inner), matching Stan's
// array[N, S] layout.
template <typename T>
std::vector<std::vector<T>> solve(const OdeSpec& s, const std::vector<T>& z0,
                                  const std::vector<T>& theta) {
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
  Eigen::Matrix<T, Eigen::Dynamic, 1> y0((Eigen::Index)z0.size());
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

void ode_fwd(KernelCtx& ctx) {
  const OdeSpec& s = *static_cast<const OdeSpec*>(ctx.udata);
  const int64_t S = ctx.in[0].len, P = ctx.in[1].len, W = S + P;
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
  stan::math::nested_rev_autodiff nested;
  std::vector<var> z0(ctx.in[0].data, ctx.in[0].data + S);
  std::vector<var> th(ctx.in[1].data, ctx.in[1].data + P);
  auto solv = solve(s, z0, th);
  for (size_t n = 0; n < solv.size(); ++n)
    for (int64_t k = 0; k < S; ++k)
      ctx.out.data[(int64_t)n * S + k] = solv[n][k].val();

  // d(solution)/d(z_init, theta), row per flattened solution element. Swept
  // last element first so the adjoint accumulation in ode_bwd runs in the
  // same order the reverse sweep over these varis used to, which keeps the
  // gradient bit-identical to what a second solve produced.
  double* J = ctx.scratch;
  for (int64_t o = ctx.out.len; o-- > 0;) {
    stan::math::set_zero_all_adjoints_nested();
    stan::math::grad(solv[(size_t)(o / S)][(size_t)(o % S)].vi_);
    for (int64_t i = 0; i < S; ++i) J[o * W + i] = z0[(size_t)i].adj();
    for (int64_t i = 0; i < P; ++i) J[o * W + S + i] = th[(size_t)i].adj();
  }
}

void ode_bwd(KernelCtx& ctx) {
  if (ctx.in_adj[0].data == nullptr && ctx.in_adj[1].data == nullptr) return;
  const int64_t S = ctx.in[0].len, P = ctx.in[1].len, W = S + P;
  const double* J = ctx.scratch;
  for (int64_t o = ctx.out.len; o-- > 0;) {
    const double a = ctx.out_adj_vec.data[o];
    if (ctx.in_adj[0].data)
      for (int64_t i = 0; i < S; ++i) ctx.in_adj[0].data[i] += a * J[o * W + i];
    if (ctx.in_adj[1].data)
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
