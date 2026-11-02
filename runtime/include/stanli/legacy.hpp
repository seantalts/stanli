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

// Promote every input to var on a nested tape, call f, seed the output
// adjoints, and copy back the adjoints of inputs that carry one. Inputs
// arrive as flat vectors; f reshapes what it needs.
template <typename F>
void nary_bwd(KernelCtx& ctx, F&& f) {
  stan::math::nested_rev_autodiff nested;
  using stan::math::var;
  std::vector<Eigen::Matrix<var, -1, 1>> xs(ctx.n_in);
  for (int k = 0; k < ctx.n_in; ++k) {
    xs[k].resize(ctx.in[k].len);
    for (int64_t i = 0; i < ctx.in[k].len; ++i) xs[k](i) = ctx.in[k].data[i];
  }
  auto out = f(xs);
  // Seed without copying the result: several rev overloads return a
  // var_value<Matrix> (SoA), and assigning that into a Matrix<var> copies
  // values into fresh vars, silently dropping the tape connection. Seeding
  // through stan-math ops on whatever type came back keeps it.
  var j;
  if constexpr (std::is_same_v<std::decay_t<decltype(out)>, var>) {
    j = out * ctx.out_adj;
  } else {
    Eigen::MatrixXd seed(out.rows(), out.cols());
    for (Eigen::Index c = 0, k = 0; c < out.cols(); ++c)
      for (Eigen::Index r = 0; r < out.rows(); ++r, ++k)
        seed(r, c) = ctx.out_adj_vec.data[k];
    j = stan::math::sum(stan::math::elt_multiply(out, seed));
  }
  stan::math::grad(j.vi_);
  for (int k = 0; k < ctx.n_in; ++k) {
    if (ctx.in_adj[k].data == nullptr) continue;
    for (int64_t i = 0; i < ctx.in[k].len; ++i)
      ctx.in_adj[k].data[i] += xs[k](i).adj();
  }
}

}  // namespace stanli

#endif
