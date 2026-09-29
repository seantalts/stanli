// Differential oracle is the preceding kernel's unit-seeded Stan Math tape.
// Independent CmdStan corpus checks complement these exact internal checks.
#include <stanli/graph.hpp>
#include <stanli/optable.hpp>
#include <stanli/packet.hpp>
#include <stan/math.hpp>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace stanli;
using MatD = Eigen::MatrixXd;
using VecD = Eigen::VectorXd;
using CMapM = Eigen::Map<const MatD>;
using CMapV = Eigen::Map<const VecD>;
using VarV = Eigen::Matrix<stan::math::var, -1, 1>;
static int failures = 0;
static void exact(const std::string& tag, double got, double want) {
  if (std::memcmp(&got, &want, sizeof(double)) != 0 &&
      !(std::isnan(got) && std::isnan(want))) {
    ++failures;
    if (failures < 20)
      std::printf("FAIL %s %.17g != %.17g\n", tag.c_str(), got, want);
  }
}
double reference_normal_glm(KernelCtx& ctx) {
  const int64_t rows = ctx.idata[0], cols = ctx.idata[1];
  const bool propto = (ctx.variant & 0x80u) != 0;
  // y is a parameter when stanc3's --O1 partial evaluator built the GLM
  // out of `theta ~ normal(X * b, s)`: the outcome needs its gradient
  // back. y-as-data (every hand-written GLM) keeps the double fast path.
  const bool y_var = (ctx.variant & 0x1u) != 0;
  // X is a parameter when the model reads `y ~ normal_id_glm(X, ...)` with a
  // parameter design matrix; a data X (the common case) keeps the map.
  const bool x_var = (ctx.variant & 0x2u) != 0;
  stan::math::nested_rev_autodiff nested;
  using stan::math::var;
  using VarM = Eigen::Matrix<var, -1, -1>;
  const bool one_y = ctx.in[0].len == 1;
  CMapV yd(ctx.in[0].data, ctx.in[0].len);
  CMapM Xd(ctx.in[1].data, rows, cols);
  VarM Xv(x_var ? rows : 0, x_var ? cols : 0);
  for (int64_t j = 0; j < Xv.cols(); ++j)
    for (int64_t i = 0; i < Xv.rows(); ++i)
      Xv(i, j) = ctx.in[1].data[j * rows + i];
  var y_scalar = ctx.in[0].len ? ctx.in[0].data[0] : 0.0;
  VarV yv(y_var && !one_y ? ctx.in[0].len : 0);
  for (int64_t i = 0; i < yv.size(); ++i) yv(i) = ctx.in[0].data[i];
  VarV alpha(ctx.in[2].len), beta(ctx.in[3].len), sigma(ctx.in[4].len);
  for (int64_t i = 0; i < ctx.in[2].len; ++i) alpha(i) = ctx.in[2].data[i];
  for (int64_t i = 0; i < ctx.in[3].len; ++i) beta(i) = ctx.in[3].data[i];
  for (int64_t i = 0; i < ctx.in[4].len; ++i) sigma(i) = ctx.in[4].data[i];
  var out;
  const bool one_a = ctx.in[2].len == 1, one_s = ctx.in[4].len == 1;
  auto call = [&](auto&& y, auto&& x, auto&& a, auto&& s) {
    return propto ? stan::math::normal_id_glm_lpdf<true>(y, x, a, beta, s)
                  : stan::math::normal_id_glm_lpdf<false>(y, x, a, beta, s);
  };
  auto dispatch = [&](auto&& y, auto&& x) {
    if (one_a && one_s)
      out = call(y, x, alpha(0), sigma(0));
    else if (one_a)
      out = call(y, x, alpha(0), sigma);
    else if (one_s)
      out = call(y, x, alpha, sigma(0));
    else
      out = call(y, x, alpha, sigma);
  };
  const bool row_x = ctx.n_idata >= 5 && ctx.idata[4] == 1;
  auto pick_x = [&](auto&& y) {
    if (row_x) {
      if (x_var)
        dispatch(y, Xv.row(0));
      else
        dispatch(y, Xd.row(0));
    } else if (x_var) {
      dispatch(y, Xv);
    } else {
      dispatch(y, Xd);
    }
  };
  if (y_var) {
    if (one_y)
      pick_x(y_scalar);
    else
      pick_x(yv);
  } else if (one_y) {
    pick_x(ctx.in[0].data[0]);
  } else {
    pick_x(yd);
  }
  const double v = out.val();
  // One tape per gradient, not two. The forward differentiates it once
  // with a seed of 1 and keeps the partials; the backward is then the
  // contraction every other native kernel does. This used to build the
  // whole var tape in the forward, throw it away, and build it again in
  // the backward to call grad() -- 90.9% of diamonds' gradient, and more
  // work than CmdStan does for the same statement.
  if (!values_only()) {
    stan::math::grad(out.vi_);
    double* s = ctx.scratch;
    if (y_var) {
      if (one_y)
        *s++ = y_scalar.adj();
      else
        for (int64_t i = 0; i < yv.size(); ++i) *s++ = yv(i).adj();
    }
    // Column-major, matching the slot layout the backward scatters into.
    for (int64_t j = 0; j < Xv.cols(); ++j)
      for (int64_t i = 0; i < Xv.rows(); ++i) *s++ = Xv(i, j).adj();
    for (int64_t i = 0; i < ctx.in[2].len; ++i) *s++ = alpha(i).adj();
    for (int64_t i = 0; i < ctx.in[3].len; ++i) *s++ = beta(i).adj();
    for (int64_t i = 0; i < ctx.in[4].len; ++i) *s++ = sigma(i).adj();
  }
  return v;
}

static void check(int n, int cols, unsigned mask, unsigned scalar, bool row_x,
                  bool propto, double weight, int invalid = -1) {
  const int rows = row_x ? 1 : n;
  const int lens[] = {(scalar & 1u) ? 1 : n, rows * cols, (scalar & 2u) ? 1 : n,
                      cols, (scalar & 4u) ? 1 : n};
  std::array<std::vector<double>, 5> inputs, adjoints;
  int total = 0;
  for (int k = 0; k < 5; ++k) {
    total += lens[k];
    inputs[k].resize(lens[k]);
    adjoints[k].resize(lens[k], -0.25);
    for (int i = 0; i < lens[k]; ++i)
      inputs[k][i] =
          k == 4 ? 0.7 + 0.05 * (i % 5) : std::sin(0.23 * i + 0.31 * k);
  }
  if (invalid >= 0 && !inputs[invalid].empty())
    inputs[invalid][0] =
        invalid == 4 ? -1.0 : std::numeric_limits<double>::infinity();
  std::vector<double> scratch(total, 123.0), expected(total, 456.0);
  int idata[] = {rows, cols, 0, 0, row_x ? 1 : 0};
  double value = 0;
  KernelCtx ctx;
  ctx.n_in = 5;
  ctx.idata = idata;
  ctx.n_idata = 5;
  ctx.variant = mask | (propto ? 0x80u : 0u);
  ctx.out = {&value, 1};
  ctx.out_adj = weight;
  ctx.scratch = scratch.data();
  for (int k = 0; k < 5; ++k) {
    ctx.in[k] = {inputs[k].data(), lens[k]};
    ctx.in_adj[k] = {(mask & (1u << k)) ? adjoints[k].data() : nullptr,
                     lens[k]};
  }
  const auto* kernel = find_kernel(OP_NORMAL_ID_GLM_LPDF);
  const auto bytes =
      stan::math::ChainableStack::instance_->memalloc_.bytes_allocated();
  std::string error, reference_error;
  try {
    kernel->forward(ctx);
  } catch (const std::exception& e) {
    error = e.what();
  }
  const auto after =
      stan::math::ChainableStack::instance_->memalloc_.bytes_allocated();
  if (after != bytes) {
    ++failures;
    std::printf("FAIL recorder grew Stan arena %zu -> %zu\n", bytes, after);
  }
  KernelCtx reference = ctx;
  reference.scratch = expected.data();
  double expected_value = 0;
  try {
    expected_value = reference_normal_glm(reference);
  } catch (const std::exception& e) {
    reference_error = e.what();
  }
  if (error != reference_error) {
    ++failures;
    std::printf("FAIL rejection mismatch: %s / %s\n", error.c_str(),
                reference_error.c_str());
  }
  if (!error.empty() || !reference_error.empty()) return;
  const std::string tag =
      "normal glm n=" + std::to_string(n) + " cols=" + std::to_string(cols) +
      " mask=" + std::to_string(mask) + " scalar=" + std::to_string(scalar) +
      " row=" + std::to_string(row_x);
  exact(tag + " value", value, expected_value);
  kernel->backward(ctx);
  size_t at = 0;
  for (int k = 0; k < 5; ++k) {
    if (k < 2 && !(mask & (1u << k))) continue;
    for (int i = 0; i < lens[k]; ++i, ++at) {
      exact(tag + " partial " + std::to_string(at), scratch[at], expected[at]);
      const double want =
          (mask & (1u << k)) ? -0.25 + weight * expected[at] : -0.25;
      exact(tag + " scatter", adjoints[k][i], want);
    }
  }
  if (n == 5 && cols == 3 && mask == 31 && scalar == 6 && !row_x) {
    Graph graph;
    std::vector<int> slots;
    for (int k = 0; k < 5; ++k) slots.push_back(graph.add_slot(lens[k], true));
    const int out = graph.add_slot(1, false);
    graph.add_op(OP_NORMAL_ID_GLM_LPDF,
                 {slots[0], slots[1], slots[2], slots[3], slots[4]}, out,
                 {rows, cols});
    graph.ops.back().variant = ctx.variant;
    graph.result_slot = out;
    Executor ex(std::move(graph));
    for (int k = 0; k < 5; ++k)
      std::copy(inputs[k].begin(), inputs[k].end(), ex.param_ptr(slots[k]));
    exact(tag + " value only", ex.forward_value_only(), expected_value);
    std::vector<double> gradients(total);
    exact(tag + " after value only", ex.gradient(gradients.data()),
          expected_value);
    for (int i = 0; i < total; ++i)
      exact(tag + " gradient after value only", gradients[i],
            0.0 + expected[i]);
  }
  kernel->forward(ctx);
  exact(tag + " reused", value, expected_value);
}

int main() {
  if (std::string(find_kernel(OP_NORMAL_ID_GLM_LPDF)->derivative_mechanism) !=
      "recorded_partials") {
    ++failures;
    std::printf("FAIL normal GLM derivative classification\n");
  }
  // Before the oracle can grow the arena, a large active design proves this
  // candidate does not allocate the scalar-var matrix of the former path.
  check(2048, 8, 31, 6, false, false, -0.73);
  for (int n : {0, 1, 5, 32})
    for (int cols : {0, 1, 3})
      for (unsigned mask : {0u, 4u, 8u, 16u, 28u, 29u, 30u, 31u})
        for (unsigned scalar = 0; scalar < 8; ++scalar)
          for (bool row : {false, true}) {
            if (row && (scalar & 1u) && n != 1) continue;
            for (bool propto : {false, true})
              check(n, cols, mask, scalar, row, propto, -0.73);
          }
  for (double seed : {0.0, -0.0, 1.0, 1e-308, 1e308,
                      std::numeric_limits<double>::infinity()}) {
    check(5, 3, 31, 0, false, false, seed);
    check(0, 3, 28, 6, false, false, seed);
  }
  for (int invalid = 0; invalid < 5; ++invalid)
    for (bool propto : {false, true})
      check(5, 3, 31, 6, false, propto, 1.0, invalid);
  if (!failures) std::printf("test_normal_glm OK\n");
  return failures ? 1 : 0;
}
