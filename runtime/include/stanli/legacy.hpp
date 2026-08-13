// Legacy op mechanism: run today's stan-math rev code, unmodified, behind
// the op interface. Forward evaluates on plain doubles. Backward opens a
// nested var tape, replays the function on promoted inputs, seeds the output
// adjoints, propagates, and copies input adjoints out.
//
// Seeding uses the dot trick: grad of dot(seed, f(x)) with respect to x is
// seed^T J_f, the vjp we need, and it only touches public stan-math API.
//
// Correct by construction (it IS the current code path), expensive per call:
// the nested tape allocates from the arena, so the native-op zero-allocation
// property does not hold for legacy ops. Each one disappears from profiles
// when its function gets a native port.
#ifndef STANLI_LEGACY_HPP
#define STANLI_LEGACY_HPP

#include <stanli/graph.hpp>

#include <stan/math.hpp>

namespace stanli {

// Replays f on a varmat operand -- `var_value<VectorXd>`, one vari over a
// contiguous value/adjoint pair -- rather than `Matrix<var>`, which is N
// separate varis reached through N pointers. This is stan-math's own SoA
// representation, the one `stanc --O1` exists to reach, and its rev
// overloads are the vectorized implementations. Measured on log_sum_exp:
// 3.39 ns/element against 6.68 for the AoS form (Apple M-series).
//
// F: var_value<VectorXd> -> var (scalar out) or a var vector (vector out).
template <typename F>
void legacy_bwd_vec_in(KernelCtx& ctx, F&& f) {
  if (ctx.in_adj[0].data == nullptr) return;
  stan::math::nested_rev_autodiff nested;
  using stan::math::var;
  stan::math::var_value<Eigen::VectorXd> x(
      Eigen::Map<const Eigen::VectorXd>(ctx.in[0].data, ctx.in[0].len));
  auto out = f(x);
  var j;
  if constexpr (std::is_same_v<std::decay_t<decltype(out)>, var>) {
    j = out * ctx.out_adj;
  } else {
    Eigen::Map<const Eigen::VectorXd> seed(ctx.out_adj_vec.data,
                                           ctx.out_adj_vec.len);
    j = stan::math::dot_product(seed, out);
  }
  stan::math::grad(j.vi_);
  Eigen::Map<Eigen::VectorXd>(ctx.in_adj[0].data, ctx.in[0].len) += x.adj();
}

// Value + partials for a SCALAR-output function, computed by stan-math in
// the FORWARD sweep and stashed in scratch. The chain rule for a scalar
// output is just a scale, so seeding with 1.0 here and multiplying by the
// output adjoint later is equivalent to replaying with the real seed --
// and it leaves the backward reading only scratch, never the input
// values. That is what lets these ops opt into the destructive-update
// trait in optable.hpp (see backward_ignores_input_values).
template <typename F>
double legacy_fwd_partials_vec(KernelCtx& ctx, F&& f) {
  stan::math::nested_rev_autodiff nested;
  stan::math::var_value<Eigen::VectorXd> x(
      Eigen::Map<const Eigen::VectorXd>(ctx.in[0].data, ctx.in[0].len));
  stan::math::var j = f(x);
  const double value = j.val();
  stan::math::grad(j.vi_);
  Eigen::Map<Eigen::VectorXd>(ctx.scratch, ctx.in[0].len) = x.adj();
  return value;
}

}  // namespace stanli

#endif
