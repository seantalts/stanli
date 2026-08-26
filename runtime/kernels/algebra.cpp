// Legacy algebra_solver.  stan-math performs the Powell solve and its
// implicit differentiation; the model's algebraic system is evaluated from
// retained MIR, through the ODE register program when it compiled and the
// MIR interpreter otherwise.
//
// Stan's legacy signature intentionally differentiates only with respect to
// `y`, the parameter vector.  The initial guess `x` selects a root but its
// adjoint is always zero, even when its C++ scalar type is var.  The lowering
// records y's scalar activity in the variant and this kernel never scatters
// into input 0.
#include <stanli/algebra.hpp>
#include <stanli/graph.hpp>
#include <stanli/mir_interp.hpp>
#include <stanli/ode_prog.hpp>
#include <stanli/optable.hpp>
#include <stanli/packet.hpp>

#include <stan/math.hpp>

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace stanli {
namespace {

struct MirSystem {
  const AlgebraSpec* spec;

  template <typename T_x, typename T_y>
  Eigen::Matrix<stan::return_type_t<typename T_x::Scalar, typename T_y::Scalar>,
                Eigen::Dynamic, 1>
  operator()(const T_x& x, const T_y& y, const std::vector<double>& x_r,
             const std::vector<int>& x_i, std::ostream* = nullptr) const {
    using T = stan::return_type_t<typename T_x::Scalar, typename T_y::Scalar>;
    std::vector<T> result;
    if (spec->prog.ok) {
      // RhsProgram's leading time register is the synthetic unused formal
      // installed by lowering.  The remaining regions are exactly unknown,
      // parameters, and real data.
      run_rhs<T>(spec->prog, 0.0, x.data(), y.data(), (size_t)y.size(),
                 x_r.data(), result);
    } else {
      const mir::FunDef* system = spec->system();
      if (!system)
        throw std::runtime_error("algebra_solver: missing algebraic system");
      std::vector<std::vector<T>> reals(3);
      reals[0].reserve((size_t)x.size());
      reals[1].reserve((size_t)y.size());
      for (Eigen::Index i = 0; i < x.size(); ++i) reals[0].push_back(T(x(i)));
      for (Eigen::Index i = 0; i < y.size(); ++i) reals[1].push_back(T(y(i)));
      reals[2].reserve(x_r.size());
      for (double value : x_r) reals[2].push_back(T(value));
      std::vector<std::vector<int>> ints{x_i};
      MirInterp<T> ev(*spec->funs(), "algebraic system");
      result = ev.call(*system, reals, ints);
    }
    Eigen::Matrix<T, Eigen::Dynamic, 1> out((Eigen::Index)result.size());
    for (size_t i = 0; i < result.size(); ++i) out((Eigen::Index)i) = result[i];
    return out;
  }
};

Eigen::VectorXd solve_double(const AlgebraSpec& spec, const Desc& x_desc,
                             const Desc& y_desc) {
  Eigen::Map<const Eigen::VectorXd> x(x_desc.data, x_desc.len);
  Eigen::Map<const Eigen::VectorXd> y(y_desc.data, y_desc.len);
  return stan::math::algebra_solver(
      MirSystem{&spec}, x, y, spec.x_r, spec.x_i, nullptr,
      spec.relative_tolerance, spec.function_tolerance, spec.max_num_steps);
}

void algebra_fwd(KernelCtx& ctx) {
  const AlgebraSpec& spec = *static_cast<const AlgebraSpec*>(ctx.udata);
  const int64_t P = ctx.in[1].len;
  const bool y_autodiff = (ctx.variant & 0x1u) != 0;

  if (values_only() || !y_autodiff) {
    const Eigen::VectorXd solved = solve_double(spec, ctx.in[0], ctx.in[1]);
    if (solved.size() != ctx.out.len)
      throw std::runtime_error("algebra_solver: result size mismatch");
    std::copy(solved.data(), solved.data() + solved.size(), ctx.out.data);
    if (!values_only() && ctx.out.len != 0 && P != 0)
      std::fill(ctx.scratch, ctx.scratch + ctx.out.len * P, 0.0);
    return;
  }

  // Evaluate the exact legacy var overload inside stan-math's Jacobian
  // helper.  Besides keeping the root solve/checks in one implementation,
  // this invokes stan-math's implicit-function pullback rather than a second
  // local derivation of the solver semantics.
  Eigen::Map<const Eigen::VectorXd> x(ctx.in[0].data, ctx.in[0].len);
  Eigen::Map<const Eigen::VectorXd> y(ctx.in[1].data, ctx.in[1].len);
  Eigen::VectorXd solved;
  Eigen::MatrixXd jacobian;
  stan::math::jacobian(
      [&](const auto& y_var) {
        return stan::math::algebra_solver(
            MirSystem{&spec}, x, y_var, spec.x_r, spec.x_i, nullptr,
            spec.relative_tolerance, spec.function_tolerance,
            spec.max_num_steps);
      },
      y, solved, jacobian);
  if (solved.size() != ctx.out.len || jacobian.rows() != ctx.out.len ||
      jacobian.cols() != P)
    throw std::runtime_error("algebra_solver: result Jacobian size mismatch");
  std::copy(solved.data(), solved.data() + solved.size(), ctx.out.data);
  for (int64_t o = 0; o < ctx.out.len; ++o)
    for (int64_t i = 0; i < P; ++i) ctx.scratch[o * P + i] = jacobian(o, i);
}

void algebra_bwd(KernelCtx& ctx) {
  // The guess is deliberately absent: legacy algebra_solver's return scalar
  // type and pullback depend only on y.
  if ((ctx.variant & 0x1u) == 0 || ctx.in_adj[1].data == nullptr) return;
  const int64_t P = ctx.in[1].len;
  for (int64_t o = ctx.out.len; o-- > 0;) {
    const double adj = ctx.out_adj_vec.data[o];
    for (int64_t i = 0; i < P; ++i)
      ctx.in_adj[1].data[i] += adj * ctx.scratch[o * P + i];
  }
}

int64_t algebra_scratch(const Op& op, const Slot* slots) {
  return slots[op.out].len * slots[op.in[1]].len;
}

}  // namespace

void register_algebra_kernels() {
  register_kernel(OP_ALGEBRA_SOLVER,
                  Kernel{algebra_fwd, algebra_bwd, algebra_scratch});
}

}  // namespace stanli
