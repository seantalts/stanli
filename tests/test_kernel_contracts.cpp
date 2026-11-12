#include <stanli/compile.hpp>
#include <stanli/graph.hpp>
#include <stanli/mir.hpp>
#include <stanli/mir_interp.hpp>
#include <stanli/optable.hpp>
#include <stanli/structured_loop.hpp>
#include <stanli/wa_interp.hpp>

#include <stan/math.hpp>
#include <stan/model/indexing.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace stanli;

static int failures = 0;
static void check(bool ok, const std::string& what) {
  if (!ok) {
    ++failures;
    std::printf("FAIL %s\n", what.c_str());
  }
}
static void expect_eq(const std::string& what, double got, double want) {
  if (got != want) {
    ++failures;
    std::printf("FAIL %-16s got %.17g want %.17g\n", what.c_str(), got, want);
  }
}

static bool same_double_bytes(const std::vector<double>& a,
                              const std::vector<double>& b) {
  return a.size() == b.size() &&
         (a.empty() ||
          std::memcmp(a.data(), b.data(), a.size() * sizeof(double)) == 0);
}

// A scalar add_diag argument receives one contribution per diagonal entry.
// The additions are created from the first coefficient to the last, so the
// reverse tape accumulates a deliberately cancellation-sensitive seed in
// the opposite order.
static void test_add_diag_scalar_reverse_order() {
  const Kernel* add_diag = find_kernel(OP_ADD_DIAG);
  check(add_diag && add_diag->forward && add_diag->backward,
        "add_diag scalar kernel registration");
  if (add_diag && add_diag->forward && add_diag->backward) {
    constexpr int n = 3;
    double a[n * n] = {};
    double diagonal = 0.0;
    double out[n * n] = {};
    double out_adj[n * n] = {};
    out_adj[0] = 1e16;
    out_adj[4] = -1e16;
    out_adj[8] = 1.0;
    double diagonal_adj = 0.0;
    const int dims[] = {n, n};
    KernelCtx ctx;
    ctx.in[0] = Desc{a, n * n};
    ctx.in[1] = Desc{&diagonal, 1};
    ctx.n_in = 2;
    ctx.out = Desc{out, n * n};
    ctx.variant = 1;
    ctx.idata = dims;
    ctx.n_idata = 2;
    add_diag->forward(ctx);
    ctx.in_adj[0] = Desc{nullptr, n * n};
    ctx.in_adj[1] = Desc{&diagonal_adj, 1};
    ctx.out_adj_vec = Desc{out_adj, n * n};
    add_diag->backward(ctx);

    Eigen::MatrixXd matrix = Eigen::MatrixXd::Zero(n, n);
    Eigen::MatrixXd seed = Eigen::MatrixXd::Zero(n, n);
    seed(0, 0) = 1e16;
    seed(1, 1) = -1e16;
    seed(2, 2) = 1.0;
    stan::math::var d_ref = 0.0;
    stan::math::var reference = stan::math::sum(
        stan::math::elt_multiply(stan::math::add_diag(matrix, d_ref), seed));
    reference.grad();
    expect_eq("add_diag scalar reverse order", diagonal_adj, d_ref.adj());
    stan::math::recover_memory();
  }
}

// Reverse replay must accumulate into an adjoint already written by a
// later consumer in the same order as Stan's tape. Grouping the density's
// two logit contributions before adding that existing 0.1 changes one bit.
static void test_categorical_existing_adjoint() {
  Graph g;
  const int beta = g.add_slot(3, true);
  const int outcome = g.add_slot(1, false);
  const int cat = g.add_slot(1, false);
  const int cat_op = g.add_op(OP_CATEGORICAL, {outcome, beta}, cat);
  g.ops[(size_t)cat_op].variant =
      kCategoricalLogit | kCategoricalScalarOutcome | kCategoricalArgAutodiff;
  const int beta0 = g.add_slot(1, false);
  g.add_op(OP_INDEX, {beta}, beta0, {0});
  const int scale = g.add_slot(1, false);
  const int linear = g.add_slot(1, false);
  g.add_op(OP_MUL, {beta0, scale}, linear);
  const int total = g.add_slot(1, false);
  g.add_op(OP_ADD, {cat, linear}, total);
  g.result_slot = total;

  Executor ex(std::move(g));
  std::fill(ex.params_data(), ex.params_data() + 3, -2.0);
  ex.value_ptr(outcome)[0] = 1.0;
  ex.value_ptr(scale)[0] = 0.1;
  double got_grad[3];
  const double got = ex.gradient(got_grad);

  Eigen::Matrix<stan::math::var, -1, 1> ref_beta(3);
  ref_beta << -2.0, -2.0, -2.0;
  stan::math::var ref_cat =
      stan::math::categorical_logit_lpmf<false>(1, ref_beta);
  stan::math::var ref_linear = ref_beta(0) * 0.1;
  stan::math::var ref = ref_cat + ref_linear;
  ref.grad();
  expect_eq("categorical existing-adjoint value", got, ref.val());
  for (int i = 0; i < 3; ++i)
    expect_eq("categorical existing-adjoint g" + std::to_string(i), got_grad[i],
              ref_beta(i).adj());
  stan::math::recover_memory();
}

// The native scalar probability rule has one selected reciprocal. Pin its
// non-unit upstream seed, += into an adjoint a later consumer already
// wrote, propto/type behavior, and value-only -> gradient reuse against the
// exact Stan tape it replaces.
static void test_categorical_native_scalar() {
  Graph g;
  const int theta = g.add_slot(3, true);
  const int outcome = g.add_slot(1, false);
  const int cat = g.add_slot(1, false);
  const int cat_op = g.add_op(OP_CATEGORICAL, {outcome, theta}, cat);
  g.ops[(size_t)cat_op].variant =
      kCategoricalScalarOutcome | kCategoricalArgAutodiff | 0x80u;
  const int cat_scale = g.add_slot(1, false);
  const int scaled_cat = g.add_slot(1, false);
  g.add_op(OP_MUL, {cat, cat_scale}, scaled_cat);
  const int selected = g.add_slot(1, false);
  g.add_op(OP_INDEX, {theta}, selected, {1});
  const int linear_scale = g.add_slot(1, false);
  const int linear = g.add_slot(1, false);
  g.add_op(OP_MUL, {selected, linear_scale}, linear);
  const int total = g.add_slot(1, false);
  g.add_op(OP_ADD, {scaled_cat, linear}, total);
  g.result_slot = total;

  Executor ex(std::move(g));
  const double point[3] = {0.2, 0.7, 0.1};
  std::copy(std::begin(point), std::end(point), ex.params_data());
  ex.value_ptr(outcome)[0] = 2.0;
  ex.value_ptr(cat_scale)[0] = 0.3;
  ex.value_ptr(linear_scale)[0] = -0.2;
  expect_eq("categorical native value-only propto", ex.forward_value_only(),
            point[1] * -0.2);
  double got_grad[3];
  const double got = ex.gradient(got_grad);

  Eigen::Matrix<stan::math::var, -1, 1> ref_theta(3);
  ref_theta << point[0], point[1], point[2];
  stan::math::var ref = stan::math::categorical_lpmf<true>(2, ref_theta) * 0.3 +
                        ref_theta(1) * -0.2;
  ref.grad();
  expect_eq("categorical native scalar value", got, ref.val());
  for (int i = 0; i < 3; ++i)
    expect_eq("categorical native scalar g" + std::to_string(i), got_grad[i],
              ref_theta(i).adj());
  stan::math::recover_memory();
}

// Direct kernel edges for the same rule. A scalar categorical log only
// connects the selected probability: an infinite seed must leave an
// unselected signed zero untouched, while a zero seed over a selected zero
// still follows the connected log edge and forms 0 / 0. Reusing the context
// also pins that the native path carries no hidden scratch state.
static void test_categorical_native_kernel_edges() {
  const Kernel* kernel = find_kernel(OP_CATEGORICAL);
  check(kernel != nullptr, "categorical native kernel registered");
  check(kernel && kernel->scratch_size == nullptr,
        "categorical native kernel scratchless");
  if (kernel) {
    double outcome = 1.0;
    double theta[2] = {1.0, 0.0};
    double theta_adj[2] = {0.0, -0.0};
    double out = 0.0;
    KernelCtx ctx{};
    ctx.n_in = 2;
    ctx.in[0] = Desc{&outcome, 1};
    ctx.in[1] = Desc{theta, 2};
    ctx.in_adj[0] = Desc{nullptr, 1};
    ctx.in_adj[1] = Desc{theta_adj, 2};
    ctx.out = Desc{&out, 1};
    ctx.variant = kCategoricalScalarOutcome | kCategoricalArgAutodiff | 0x80u;

    kernel->forward(ctx);
    ctx.out_adj = std::numeric_limits<double>::infinity();
    kernel->backward(ctx);
    check(std::isinf(theta_adj[0]) && theta_adj[0] > 0.0,
          "categorical native infinite selected adjoint");
    check(theta_adj[1] == 0.0 && std::signbit(theta_adj[1]),
          "categorical native unselected signed zero");

    theta[0] = 0.0;
    theta[1] = 1.0;
    theta_adj[0] = 0.0;
    theta_adj[1] = -0.0;
    kernel->forward(ctx);
    check(std::isinf(out) && out < 0.0,
          "categorical native selected zero value");
    ctx.out_adj = 0.0;
    kernel->backward(ctx);
    check(std::isnan(theta_adj[0]),
          "categorical native selected zero topology");
    check(theta_adj[1] == 0.0 && std::signbit(theta_adj[1]),
          "categorical native repeated unselected zero");

    outcome = 2.0;
    theta[0] = 0.2;
    theta[1] = 0.8;
    ctx.in_adj[1] = Desc{nullptr, 2};
    kernel->forward(ctx);
    expect_eq("categorical native null-adjoint value", out, std::log(0.8));
    ctx.out_adj = 1.0;
    kernel->backward(ctx);  // active source type, but no graph adjoint edge
  }
}

// Scalar logits can bypass the var tape, but must retain the subtraction
// and scalar-exp reverse order even for prefilled or signed-zero adjoints.
static void test_categorical_logit_scalar_bits() {
  const Kernel* kernel = find_kernel(OP_CATEGORICAL);
  const double inf = std::numeric_limits<double>::infinity();
  const auto same_bits = [](double a, double b) {
    return (std::isnan(a) && std::isnan(b)) ||
           std::memcmp(&a, &b, sizeof(double)) == 0;
  };
  for (int n : {1, 2, 3, 8, 32})
    for (int selected = 1; selected <= n; ++selected)
      for (double scale : {0.0, 1.0, 1000.0, 1e308})
        for (double weight : {0.0, -0.0, 1.0, -1.3, 1e308, 1e-308, inf})
          for (bool propto : {false, true}) {
            stan::math::nested_rev_autodiff nested;
            Eigen::VectorXd beta(n), adj(n);
            Eigen::Matrix<stan::math::var, -1, 1> ref_beta(n);
            for (int i = 0; i < n; ++i) {
              beta(i) = scale * std::sin(i + 1.0);
              ref_beta(i) = beta(i);
              adj(i) = i % 2 ? -0.0 : 0.3125;
              ref_beta(i).adj() = adj(i);
            }
            double outcome = selected, out = 0;
            KernelCtx ctx{};
            ctx.n_in = 2;
            ctx.in[0] = Desc{&outcome, 1};
            ctx.in[1] = Desc{beta.data(), n};
            ctx.in_adj[1] = Desc{adj.data(), n};
            ctx.out = Desc{&out, 1};
            ctx.out_adj = weight;
            ctx.variant = kCategoricalScalarOutcome | kCategoricalLogit |
                          kCategoricalArgAutodiff | (propto ? 0x80u : 0u);
            kernel->forward(ctx);
            kernel->backward(ctx);
            stan::math::var lp =
                propto ? stan::math::categorical_logit_lpmf<true>(selected,
                                                                  ref_beta)
                       : stan::math::categorical_logit_lpmf<false>(selected,
                                                                   ref_beta);
            stan::math::var ref = lp * weight;
            stan::math::grad(ref.vi_);
            check(same_bits(out * weight, ref.val()),
                  "native scalar categorical logit value bits");
            for (int i = 0; i < n; ++i)
              check(same_bits(adj(i), ref_beta(i).adj()),
                    "native scalar categorical logit gradient bits");
          }
}

// Array outcomes add one callback contribution per observation. Preserve
// that exact accumulation order under a non-unit upstream adjoint; replacing
// it with count * adjoint changes the selected gradient's low bit. The zero
// unselected probability also pins Stan Math's disconnected-log topology.
static void test_categorical_array_scaled() {
  Graph g;
  const int theta = g.add_slot(2, true);
  const int outcomes = g.add_slot(6, false);
  const int cat = g.add_slot(1, false);
  const int cat_op = g.add_op(OP_CATEGORICAL, {outcomes, theta}, cat);
  g.ops[(size_t)cat_op].variant = kCategoricalArgAutodiff;
  const int scale = g.add_slot(1, false);
  const int total = g.add_slot(1, false);
  g.add_op(OP_MUL, {cat, scale}, total);
  g.result_slot = total;

  Executor ex(std::move(g));
  ex.params_data()[0] = 1.0;
  ex.params_data()[1] = 0.0;
  std::fill(ex.value_ptr(outcomes), ex.value_ptr(outcomes) + 6, 1.0);
  ex.value_ptr(scale)[0] = 0.1;
  double got_grad[2];
  const double got = ex.gradient(got_grad);

  Eigen::Matrix<stan::math::var, -1, 1> ref_theta(2);
  ref_theta << 1.0, 0.0;
  const std::vector<int> ref_outcomes(6, 1);
  stan::math::var ref =
      stan::math::categorical_lpmf<false>(ref_outcomes, ref_theta) * 0.1;
  ref.grad();
  expect_eq("categorical array scaled value", got, ref.val());
  expect_eq("categorical array scaled selected", got_grad[0],
            ref_theta(0).adj());
  check(std::isnan(got_grad[1]) && std::isnan(ref_theta(1).adj()),
        "categorical array scaled unselected topology");
  stan::math::recover_memory();
}

// The triangular pullback must match the old nested-tape weighted sum,
// including rectangular matrices, ignored upper entries, asymmetric seeds,
// overflow, and accumulation into existing input adjoints.
static void test_mult_lower_tri_self_transpose_pullback() {
  const Kernel* kernel = find_kernel(OP_MULT_LOWER_TRI_SELF_TRANSPOSE);
  for (int nr : {1, 2, 3, 8})
    for (int nc : {1, 2, 3, 8})
      for (double scale : {0.0, 1.0, 1e-100, 1e100})
        for (double weight : {0.0, -0.0, 1.0, -1.3, 1e308, 1e-308,
                              std::numeric_limits<double>::infinity()}) {
          stan::math::nested_rev_autodiff nested;
          Eigen::MatrixXd a(nr, nc), seed(nr, nr), adj(nr, nc);
          for (int i = 0; i < a.size(); ++i) {
            a.data()[i] = scale * std::sin(0.3 * (i + 1));
            adj.data()[i] = i % 2 ? -0.0 : 0.3125;
          }
          for (int i = 0; i < seed.size(); ++i)
            seed.data()[i] = (i % 3 == 0 ? -1.0 : 1.0) * weight;
          auto ref_a = a.cast<stan::math::var>().eval();
          auto result = stan::math::multiply_lower_tri_self_transpose(ref_a);
          stan::math::var loss =
              stan::math::sum(stan::math::elt_multiply(result, seed));
          stan::math::grad(loss.vi_);
          std::vector<double> expected(a.size());
          for (int i = 0; i < a.size(); ++i)
            expected[i] = adj.data()[i] + ref_a.data()[i].adj();
          int idata[] = {nr, nc};
          KernelCtx ctx{};
          ctx.n_in = 1;
          ctx.in[0] = Desc{a.data(), a.size()};
          ctx.in_adj[0] = Desc{adj.data(), adj.size()};
          ctx.out_adj_vec = Desc{seed.data(), seed.size()};
          ctx.idata = idata;
          ctx.n_idata = 2;
          kernel->backward(ctx);
          for (int i = 0; i < a.size(); ++i)
            check((std::isnan(adj.data()[i]) && std::isnan(expected[i])) ||
                      std::memcmp(&adj.data()[i], &expected[i],
                                  sizeof(double)) == 0,
                  "triangular native pullback bits");
          ctx.in_adj[0].data = nullptr;
          kernel->backward(ctx);
        }
}

static void fixed_scalar_index_tests() {
  const Kernel& kernel = *find_kernel(OP_INDEX_DYNAMIC);
  for (int mutation = 0; mutation < 5; ++mutation) {
    for (double at : {-1.0, 0.0, 1.0, 2.0, 4.0, 4.5, 5.0,
                      std::numeric_limits<double>::infinity(),
                      std::numeric_limits<double>::quiet_NaN()}) {
      DynamicIndexSpec spec;
      spec.axes = {{DynamicIndexSpec::Axis::Single, 4, 1, 1, 0}};
      spec.selected_size = 1;
      if (mutation == 1) spec.axes[0].stride = 2;
      if (mutation == 2) spec.axes[0].input_offset = 1;
      if (mutation == 3) spec.axes[0].extent = 3;
      if (mutation == 4) spec.selected_size = 2;
      double base[] = {0.2, -0.4, 1.7, 3.1};
      double seed = -2.75;
      double outputs[2] = {};
      double adjoints[2][4] = {};
      std::string errors[2];
      for (int path = 0; path < 2; ++path) {
        // An extra input forces the general descriptor path but has no
        // numerical role. The selector, shape and error obligations agree.
        spec.input_count = path ? 3 : 2;
        KernelCtx c{};
        c.n_in = spec.input_count;
        c.in[0] = {base, 4};
        c.in[1] = {&at, 1};
        c.in[2] = {&at, 1};
        c.in_adj[0] = {adjoints[path], 4};
        c.out = {outputs + path, 1};
        c.out_adj_vec = {&seed, 1};
        c.udata = &spec;
        try {
          kernel.forward(c);
          kernel.backward(c);
        } catch (const std::out_of_range& error) {
          errors[path] = std::string("range:") + error.what();
        } catch (const std::logic_error& error) {
          errors[path] = std::string("logic:") + error.what();
        }
      }
      check(errors[0] == errors[1], "scalar index preserves validation errors");
      check(outputs[0] == outputs[1], "scalar index preserves values");
      check(std::memcmp(adjoints[0], adjoints[1], sizeof(adjoints[0])) == 0,
            "scalar index preserves adjoints");
    }
  }
}

static void direct_index_kernel_tests() {
  fixed_scalar_index_tests();
  const Kernel* dynamic_index = find_kernel(OP_INDEX_DYNAMIC);
  check(dynamic_index && dynamic_index->forward && dynamic_index->backward,
        "dynamic index kernel registered");
  if (!dynamic_index || !dynamic_index->forward || !dynamic_index->backward)
    return;

  DynamicIndexSpec packed_spec;
  packed_spec.matrix_leaf = true;
  packed_spec.axes = {
      {DynamicIndexSpec::Axis::Single, 2, 1, 1, 0},
      {DynamicIndexSpec::Axis::Range, 3, 2, 3, 1},
  };
  packed_spec.axes[1].count_input_offset = 2;
  packed_spec.selected_size = 3;
  double base[] = {1, 2, 3, 4, 5, 6};
  double selectors[] = {2, 1, 3};
  double packed_output[3] = {};
  double packed_base_adj[6] = {};
  double seed[] = {1, 2, 3};
  KernelCtx packed{};
  packed.n_in = 2;
  packed.in[0] = {base, 6};
  packed.in[1] = {selectors, 3};
  packed.in_adj[0] = {packed_base_adj, 6};
  packed.in_adj[1] = {nullptr, 3};
  packed.out = {packed_output, 3};
  packed.out_adj_vec = {seed, 3};
  packed.udata = &packed_spec;
  dynamic_index->forward(packed);
  dynamic_index->backward(packed);

  DynamicIndexSpec direct_spec = packed_spec;
  direct_spec.input_count = 4;
  direct_spec.axes[0].selector_input = 1;
  direct_spec.axes[0].input_offset = 0;
  direct_spec.axes[1].selector_input = 2;
  direct_spec.axes[1].input_offset = 0;
  direct_spec.axes[1].count_input = 3;
  direct_spec.axes[1].count_input_offset = 0;
  double row = 2, lower = 1, upper = 3;
  double direct_output[3] = {};
  double direct_base_adj[6] = {};
  KernelCtx direct{};
  direct.n_in = 4;
  direct.in[0] = {base, 6};
  direct.in[1] = {&row, 1};
  direct.in[2] = {&lower, 1};
  direct.in[3] = {&upper, 1};
  direct.in_adj[0] = {direct_base_adj, 6};
  direct.in_adj[1] = {nullptr, 1};
  direct.in_adj[2] = {nullptr, 1};
  direct.in_adj[3] = {nullptr, 1};
  direct.out = {direct_output, 3};
  direct.out_adj_vec = {seed, 3};
  direct.udata = &direct_spec;
  dynamic_index->forward(direct);
  dynamic_index->backward(direct);
  check(std::memcmp(packed_output, direct_output, sizeof(packed_output)) == 0,
        "direct index matches packed values bitwise");
  check(std::memcmp(packed_base_adj, direct_base_adj,
                    sizeof(packed_base_adj)) == 0,
        "direct index matches packed adjoints bitwise");

  upper = 0;
  dynamic_index->forward(direct);
  check(direct_output[0] == 0 && direct_output[1] == 0 && direct_output[2] == 0,
        "direct dynamic range supports zero selected values");
  upper = 4;
  lower = 4;
  try {
    dynamic_index->forward(direct);
    check(false, "direct index validates selectors");
  } catch (const std::out_of_range& error) {
    check(std::string(error.what()) == "structured index out of range",
          "direct index selector message");
  }
  lower = 1;
  direct.n_in = 3;
  try {
    dynamic_index->forward(direct);
    check(false, "direct index validates input arity");
  } catch (const std::logic_error& error) {
    check(std::string(error.what()) == "invalid structured index descriptor",
          "direct index arity message");
  }
  direct.n_in = 4;
  lower = 1;
  upper = 3;

  DynamicIndexSpec invalid_route = direct_spec;
  invalid_route.axes[0].selector_input = 0;
  direct.udata = &invalid_route;
  try {
    dynamic_index->forward(direct);
    check(false, "direct index rejects the base as a selector descriptor");
  } catch (const std::logic_error&) {
  }
  invalid_route = direct_spec;
  invalid_route.axes[1].count_input = 4;
  direct.udata = &invalid_route;
  try {
    dynamic_index->forward(direct);
    check(false, "direct index rejects an out-of-range count descriptor");
  } catch (const std::logic_error&) {
  }
  direct.udata = &direct_spec;

  DynamicIndexSpec extent_spec;
  extent_spec.input_count = 2;
  extent_spec.axes = {{DynamicIndexSpec::Axis::All, 4, 1, 4, 0}};
  extent_spec.axes[0].extent_input = 1;
  extent_spec.axes[0].extent_input_offset = 0;
  extent_spec.axes[0].count_input = 1;
  extent_spec.axes[0].count_input_offset = 0;
  extent_spec.selected_size = 4;
  double logical_extent = 2;
  double extent_output[4] = {-1, -1, -1, -1};
  KernelCtx extent{};
  extent.n_in = 2;
  extent.in[0] = {base, 4};
  extent.in[1] = {&logical_extent, 1};
  extent.out = {extent_output, 4};
  extent.udata = &extent_spec;
  dynamic_index->forward(extent);
  check(extent_output[0] == 1 && extent_output[1] == 2 &&
            extent_output[2] == 0 && extent_output[3] == 0,
        "direct logical extent preserves output capacity and zero fill");

  const Kernel* dynamic_update = find_kernel(OP_SET_INDEX_DYNAMIC);
  check(dynamic_update && dynamic_update->forward && dynamic_update->backward,
        "dynamic update kernel registered");
  if (!dynamic_update || !dynamic_update->forward || !dynamic_update->backward)
    return;

  DynamicIndexSpec packed_update;
  packed_update.matrix_leaf = true;
  packed_update.axes = {
      {DynamicIndexSpec::Axis::Single, 2, 1, 1, 0},
      {DynamicIndexSpec::Axis::Range, 3, 2, 3, 1},
  };
  packed_update.axes[1].count_input_offset = 2;
  packed_update.axes[1].extent_input_offset = 3;
  packed_update.selected_size = 3;
  double update_selectors[] = {2, 1, 3, 3};
  double update_rhs[] = {10, 20, 30};
  double update_seed[] = {1, 2, 3, 4, 5, 6};
  double packed_update_output[6] = {};
  double packed_update_base_adj[6] = {};
  double packed_update_rhs_adj[3] = {};
  KernelCtx packed_update_context{};
  packed_update_context.n_in = 3;
  packed_update_context.in[0] = {base, 6};
  packed_update_context.in[1] = {update_selectors, 4};
  packed_update_context.in[2] = {update_rhs, 3};
  packed_update_context.in_adj[0] = {packed_update_base_adj, 6};
  packed_update_context.in_adj[1] = {nullptr, 4};
  packed_update_context.in_adj[2] = {packed_update_rhs_adj, 3};
  packed_update_context.out = {packed_update_output, 6};
  packed_update_context.out_adj_vec = {update_seed, 6};
  packed_update_context.udata = &packed_update;
  dynamic_update->forward(packed_update_context);
  dynamic_update->backward(packed_update_context);

  DynamicIndexSpec direct_update = packed_update;
  direct_update.input_count = 6;
  direct_update.rhs_input = 5;
  direct_update.axes[0].selector_input = 1;
  direct_update.axes[0].input_offset = 0;
  direct_update.axes[1].selector_input = 2;
  direct_update.axes[1].input_offset = 0;
  direct_update.axes[1].count_input = 3;
  direct_update.axes[1].count_input_offset = 0;
  direct_update.axes[1].extent_input = 4;
  direct_update.axes[1].extent_input_offset = 0;
  double update_row = 2, update_lower = 1, update_upper = 3, update_extent = 3;
  double direct_update_output[6] = {};
  double direct_update_base_adj[6] = {};
  double direct_update_rhs_adj[3] = {};
  KernelCtx direct_update_context{};
  direct_update_context.n_in = 6;
  direct_update_context.in[0] = {base, 6};
  direct_update_context.in[1] = {&update_row, 1};
  direct_update_context.in[2] = {&update_lower, 1};
  direct_update_context.in[3] = {&update_upper, 1};
  direct_update_context.in[4] = {&update_extent, 1};
  direct_update_context.in[5] = {update_rhs, 3};
  direct_update_context.in_adj[0] = {direct_update_base_adj, 6};
  direct_update_context.in_adj[1] = {nullptr, 1};
  direct_update_context.in_adj[2] = {nullptr, 1};
  direct_update_context.in_adj[3] = {nullptr, 1};
  direct_update_context.in_adj[4] = {nullptr, 1};
  direct_update_context.in_adj[5] = {direct_update_rhs_adj, 3};
  direct_update_context.out = {direct_update_output, 6};
  direct_update_context.out_adj_vec = {update_seed, 6};
  direct_update_context.udata = &direct_update;
  dynamic_update->forward(direct_update_context);
  dynamic_update->backward(direct_update_context);
  check(std::memcmp(packed_update_output, direct_update_output,
                    sizeof(packed_update_output)) == 0,
        "direct update matches packed values bitwise");
  check(std::memcmp(packed_update_base_adj, direct_update_base_adj,
                    sizeof(packed_update_base_adj)) == 0 &&
            std::memcmp(packed_update_rhs_adj, direct_update_rhs_adj,
                        sizeof(packed_update_rhs_adj)) == 0,
        "direct update matches packed adjoints bitwise");

  direct_update_context.n_in = 5;
  try {
    dynamic_update->forward(direct_update_context);
    check(false, "direct update validates input arity");
  } catch (const std::logic_error&) {
  }
  direct_update_context.n_in = 6;
  DynamicIndexSpec invalid_update = direct_update;
  invalid_update.axes[1].count_input = invalid_update.rhs_input;
  direct_update_context.udata = &invalid_update;
  try {
    dynamic_update->forward(direct_update_context);
    check(false, "direct update rejects the RHS as a selector descriptor");
  } catch (const std::logic_error&) {
  }
  invalid_update = direct_update;
  invalid_update.rhs_input = -1;
  direct_update_context.udata = &invalid_update;
  try {
    dynamic_update->forward(direct_update_context);
    check(false, "direct update requires an explicit direct RHS");
  } catch (const std::logic_error&) {
  }
  DynamicIndexSpec invalid_read = direct_spec;
  invalid_read.rhs_input = direct_spec.input_count - 1;
  direct.udata = &invalid_read;
  try {
    dynamic_index->forward(direct);
    check(false, "direct read rejects an RHS descriptor");
  } catch (const std::logic_error&) {
  }
  direct.udata = &direct_spec;

  DynamicIndexSpec packed_duplicate;
  packed_duplicate.axes = {{DynamicIndexSpec::Axis::Multi, 4, 1, 3, 0}};
  packed_duplicate.axes[0].count_input_offset = 3;
  packed_duplicate.selected_size = 3;
  double duplicate_base[] = {1, 2, 3, 4};
  double duplicate_selector[] = {2, 2, 4, 3};
  double duplicate_values[] = {10, 20, 30};
  double duplicate_seed[] = {1, 2, 3, 4};
  double packed_duplicate_output[4] = {};
  double packed_duplicate_scratch[4] = {};
  double packed_duplicate_base_adj[4] = {};
  double packed_duplicate_rhs_adj[3] = {};
  KernelCtx packed_duplicate_context{};
  packed_duplicate_context.n_in = 3;
  packed_duplicate_context.in[0] = {duplicate_base, 4};
  packed_duplicate_context.in[1] = {duplicate_selector, 4};
  packed_duplicate_context.in[2] = {duplicate_values, 3};
  packed_duplicate_context.in_adj[0] = {packed_duplicate_base_adj, 4};
  packed_duplicate_context.in_adj[1] = {nullptr, 4};
  packed_duplicate_context.in_adj[2] = {packed_duplicate_rhs_adj, 3};
  packed_duplicate_context.out = {packed_duplicate_output, 4};
  packed_duplicate_context.out_adj_vec = {duplicate_seed, 4};
  packed_duplicate_context.scratch = packed_duplicate_scratch;
  packed_duplicate_context.udata = &packed_duplicate;
  dynamic_update->forward(packed_duplicate_context);
  dynamic_update->backward(packed_duplicate_context);

  DynamicIndexSpec direct_duplicate = packed_duplicate;
  direct_duplicate.input_count = 4;
  direct_duplicate.rhs_input = 3;
  direct_duplicate.axes[0].selector_input = 1;
  direct_duplicate.axes[0].input_offset = 0;
  direct_duplicate.axes[0].count_input = 2;
  direct_duplicate.axes[0].count_input_offset = 0;
  double duplicate_count = 3;
  double direct_duplicate_output[4] = {};
  double direct_duplicate_scratch[4] = {};
  double direct_duplicate_base_adj[4] = {};
  double direct_duplicate_rhs_adj[3] = {};
  KernelCtx direct_duplicate_context{};
  direct_duplicate_context.n_in = 4;
  direct_duplicate_context.in[0] = {duplicate_base, 4};
  direct_duplicate_context.in[1] = {duplicate_selector, 3};
  direct_duplicate_context.in[2] = {&duplicate_count, 1};
  direct_duplicate_context.in[3] = {duplicate_values, 3};
  direct_duplicate_context.in_adj[0] = {direct_duplicate_base_adj, 4};
  direct_duplicate_context.in_adj[1] = {nullptr, 3};
  direct_duplicate_context.in_adj[2] = {nullptr, 1};
  direct_duplicate_context.in_adj[3] = {direct_duplicate_rhs_adj, 3};
  direct_duplicate_context.out = {direct_duplicate_output, 4};
  direct_duplicate_context.out_adj_vec = {duplicate_seed, 4};
  direct_duplicate_context.scratch = direct_duplicate_scratch;
  direct_duplicate_context.udata = &direct_duplicate;
  dynamic_update->forward(direct_duplicate_context);
  dynamic_update->backward(direct_duplicate_context);
  check(std::memcmp(packed_duplicate_output, direct_duplicate_output,
                    sizeof(packed_duplicate_output)) == 0 &&
            std::memcmp(packed_duplicate_base_adj, direct_duplicate_base_adj,
                        sizeof(packed_duplicate_base_adj)) == 0 &&
            std::memcmp(packed_duplicate_rhs_adj, direct_duplicate_rhs_adj,
                        sizeof(packed_duplicate_rhs_adj)) == 0,
        "direct duplicate update matches packed value and adjoints bitwise");
  check(
      direct_duplicate_output[0] == 1 && direct_duplicate_output[1] == 20 &&
          direct_duplicate_output[2] == 3 && direct_duplicate_output[3] == 30 &&
          direct_duplicate_rhs_adj[0] == 0 &&
          direct_duplicate_rhs_adj[1] == 2 && direct_duplicate_rhs_adj[2] == 4,
      "direct duplicate update preserves last-write semantics");
}

static void probe_mode_tests() {
  static const std::map<std::string, const mir::FunDef*> no_funs;
  auto var_expr = [](std::string name, mir::UnsizedLeaf leaf) {
    mir::Expr e;
    e.kind = mir::Expr::Var;
    e.name = std::move(name);
    e.unsized = {0, leaf};
    e.data_only = true;
    return e;
  };
  auto index_single = [](mir::Expr idx) {
    mir::Expr e;
    e.kind = mir::Expr::FunApp;
    e.name = "IndexSingle";
    e.args = {std::move(idx)};
    return e;
  };
  mir::Expr expr;
  expr.kind = mir::Expr::Indexed;
  expr.args = {var_expr("x", mir::UnsizedLeaf::Vector),
               index_single(var_expr("i", mir::UnsizedLeaf::Int))};
  expr.type_ = "UReal";
  expr.unsized = {0, mir::UnsizedLeaf::Real};
  expr.data_only = true;

  DataMap::Entry x;
  x.r = {10.0, 20.0, 30.0};
  x.dims = {3};

  auto evaluate = [&](bool probe) {
    MirInterp<double> m(no_funs, "probe mode test");
    m.env()["x"] = x;
    std::optional<double> lo, hi;
    for (long i = -1; i <= 5; ++i) {
      DataMap::Entry iv;
      iv.is_int = true;
      iv.i = {static_cast<int>(i)};
      iv.r = {static_cast<double>(i)};
      m.env()["i"] = iv;
      bool ok = true;
      DataMap::Entry result;
      if (probe) {
        m.set_probe(true);
        result = m.eval(expr);
        if (m.probe_failed()) {
          m.clear_probe_failed();
          ok = false;
        }
        m.set_probe(false);
      } else {
        try {
          result = m.eval(expr);
        } catch (const std::exception&) {
          ok = false;
        }
      }
      if (ok && result.r.size() == 1) {
        if (!lo) {
          lo = hi = result.r[0];
        } else {
          lo = std::min(*lo, result.r[0]);
          hi = std::max(*hi, result.r[0]);
        }
      }
    }
    return std::make_pair(lo, hi);
  };

  const auto with_throw = evaluate(false);
  const auto with_probe = evaluate(true);
  check(with_throw == with_probe,
        "probe mode matches throw-and-catch over out-of-bounds indices");
  check(with_throw.first.has_value() && *with_throw.first == 10.0 &&
            *with_throw.second == 30.0,
        "in-bounds combinations still contribute");

  const mir::Expr unknown = var_expr("y", mir::UnsizedLeaf::Real);
  MirInterp<double> probing(no_funs, "probe mode test");
  probing.set_probe(true);
  probing.eval(unknown);
  check(probing.probe_failed(), "unresolved variable sets the probe flag");
  probing.clear_probe_failed();

  bool threw = false;
  MirInterp<double> throwing(no_funs, "probe mode test");
  try {
    throwing.eval(unknown);
  } catch (const std::exception&) {
    threw = true;
  }
  check(threw, "the same expression still throws outside probe mode");
}

// WaRng is the BridgeStan-compatible public stream: it must track Stan's
// current rng_t and its create_rng(seed, chain=0) seeding convention. This
// catches the historical drift where sampling moved to mixmax but generated
// quantities silently stayed on ecuyer1988.
static void test_stan_rng_stream_contract() {
  using namespace stanli;
  constexpr unsigned seed = 1234;
  for (const unsigned chain : {0u, 1u, 17u}) {
    WaRng got(seed, chain);
    stan::rng_t want = stan::services::util::create_rng(seed, chain);
    for (int i = 0; i < 32; ++i) {
      if (got.gen()() != want()) {
        ++failures;
        std::printf(
            "FAIL WaRng does not match Stan rng_t at chain %u draw %d\n", chain,
            i);
        return;
      }
    }
    got.seed(seed, chain);
    want = stan::services::util::create_rng(seed, chain);
    if (got.gen()() != want()) {
      ++failures;
      std::printf(
          "FAIL WaRng reseed does not match Stan create_rng at chain %u\n",
          chain);
    }
  }
}

// Keep the shared helper a transparent call into Stan Math. In particular,
// endpoint results must not become shortcuts: Stan Math owns validation and
// however much of the engine it consumes, including at N=0 and p in {0,1}.
static void test_binomial_rng_helper_contract() {
  using namespace stanli;
  const auto helper_draw = [](int n, double p, WaRng& rng) {
    const double args[] = {static_cast<double>(n), p};
    return scalar_rng_draw(ScalarRng::Binomial, args, 2, rng);
  };
  const auto next = [](WaRng& rng) {
    return stan::math::uniform_rng(0.0, 1.0, rng.gen());
  };

  struct ValidCase {
    int n;
    double p;
  };
  const ValidCase valid[] = {
      {0, 0.0}, {0, 0.37}, {0, 1.0},  {9, -0.0},   {9, 0.0},
      {9, 1.0}, {20, 0.5}, {21, 0.5}, {100, 0.49}, {100, 0.51},
  };
  for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); ++i) {
    WaRng got_rng(static_cast<unsigned>(101 + i));
    WaRng want_rng(static_cast<unsigned>(101 + i));
    const double got = helper_draw(valid[i].n, valid[i].p, got_rng);
    const double want = static_cast<double>(
        stan::math::binomial_rng(valid[i].n, valid[i].p, want_rng.gen()));
    if (got != want || next(got_rng) != next(want_rng)) {
      ++failures;
      std::printf("FAIL binomial helper valid case %zu\n", i);
    }
  }

  // Both-invalid cases pin Stan Math's validation priority. The remaining
  // cases cover finite values just outside the interval, NaN, and infinities.
  const ValidCase invalid[] = {
      {-1, 0.4},
      {-1, -0.2},
      {4, -0.1},
      {4, std::nextafter(1.0, std::numeric_limits<double>::infinity())},
      {4, std::numeric_limits<double>::quiet_NaN()},
      {4, std::numeric_limits<double>::infinity()},
      {4, -std::numeric_limits<double>::infinity()},
  };
  for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
    WaRng got_rng(static_cast<unsigned>(211 + i));
    WaRng want_rng(static_cast<unsigned>(211 + i));
    std::string got_message, want_message;
    try {
      (void)helper_draw(invalid[i].n, invalid[i].p, got_rng);
    } catch (const std::domain_error& e) {
      got_message = e.what();
    }
    try {
      (void)stan::math::binomial_rng(invalid[i].n, invalid[i].p,
                                     want_rng.gen());
    } catch (const std::domain_error& e) {
      want_message = e.what();
    }
    if (got_message.empty() || got_message != want_message ||
        next(got_rng) != next(want_rng)) {
      ++failures;
      std::printf("FAIL binomial helper invalid case %zu\n", i);
    }
  }
}

struct RngException {
  int kind = 0;
  std::string message;
};

template <typename F>
RngException capture_rng_exception(F&& f) {
  try {
    f();
  } catch (const std::invalid_argument& e) {
    return {1, e.what()};
  } catch (const std::domain_error& e) {
    return {2, e.what()};
  } catch (const std::logic_error& e) {
    return {3, e.what()};
  } catch (const std::exception& e) {
    return {4, e.what()};
  }
  return {};
}

static Eigen::VectorXd categorical_theta(const std::vector<double>& p) {
  Eigen::VectorXd theta(static_cast<Eigen::Index>(p.size()));
  for (size_t i = 0; i < p.size(); ++i)
    theta[static_cast<Eigen::Index>(i)] = p[i];
  return theta;
}

static Eigen::VectorXd multi_normal_location(const std::vector<double>& x) {
  Eigen::VectorXd out(static_cast<Eigen::Index>(x.size()));
  for (size_t i = 0; i < x.size(); ++i)
    out[static_cast<Eigen::Index>(i)] = x[i];
  return out;
}

static Eigen::MatrixXd multi_normal_covariance(const std::vector<double>& x,
                                               size_t rows, size_t cols) {
  Eigen::MatrixXd out(static_cast<Eigen::Index>(rows),
                      static_cast<Eigen::Index>(cols));
  for (size_t j = 0; j < cols; ++j)
    for (size_t i = 0; i < rows; ++i)
      out(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(j)) =
          x.at(j * rows + i);
  return out;
}

static std::vector<double> direct_multi_normal_rng(
    const std::vector<double>& mu, const std::vector<double>& covariance,
    size_t rows, size_t cols, stanli::WaRng& rng) {
  const Eigen::VectorXd draw = stan::math::multi_normal_rng(
      multi_normal_location(mu),
      multi_normal_covariance(covariance, rows, cols), rng.gen());
  return std::vector<double>(draw.data(), draw.data() + draw.size());
}

// categorical_rng is unusual in this opcode family: one logical argument is
// a variable-length vector, while the result is still one Stan int. Keep the
// shared helper byte-for-byte on Stan Math's validation and engine schedule.
static void test_categorical_rng_helper_contract() {
  using namespace stanli;
  const std::vector<std::vector<double>> valid = {
      {1.0},
      {0.0, 1.0, 0.0},
      {-0.0, 0.0, 1.0},
      {0.125, 0.25, 0.375, 0.25},
      {0.5, 0.5, 0.0},
      {1.0 + 5e-9},  // inside Stan Math's simplex tolerance
  };
  for (size_t c = 0; c < valid.size(); ++c) {
    for (unsigned seed : {1u, 17u, 2026u}) {
      WaRng got_rng(seed + static_cast<unsigned>(c));
      WaRng want_rng(seed + static_cast<unsigned>(c));
      const int got =
          vector_integer_rng_draw(valid[c].data(), valid[c].size(), got_rng);
      const int want = stan::math::categorical_rng(categorical_theta(valid[c]),
                                                   want_rng.gen());
      const auto got_next = got_rng.gen()();
      const auto want_next = want_rng.gen()();
      if (got != want || got_next != want_next) {
        ++failures;
        std::printf("FAIL categorical helper valid case %zu seed %u\n", c,
                    seed);
      }
    }
  }

  // Sum validation precedes the element scan in check_simplex. The two
  // negative cases distinguish those paths; empty also pins invalid_argument
  // rather than domain_error. No rejected input may advance the stream.
  const std::vector<std::vector<double>> invalid = {
      {},
      {0.0},
      {0.2, 0.2},
      {1.0, -0.25, 0.25},
      {1.0, -0.25, 0.5},
      {std::numeric_limits<double>::quiet_NaN()},
      {std::numeric_limits<double>::infinity()},
  };
  for (size_t c = 0; c < invalid.size(); ++c) {
    WaRng got_rng(static_cast<unsigned>(401 + c));
    WaRng want_rng(static_cast<unsigned>(401 + c));
    const RngException got = capture_rng_exception([&] {
      (void)vector_integer_rng_draw(
          invalid[c].empty() ? nullptr : invalid[c].data(), invalid[c].size(),
          got_rng);
    });
    const RngException want = capture_rng_exception([&] {
      (void)stan::math::categorical_rng(categorical_theta(invalid[c]),
                                        want_rng.gen());
    });
    const auto got_next = got_rng.gen()();
    const auto want_next = want_rng.gen()();
    if (got.kind == 0 || got.kind != want.kind || got.message != want.message ||
        got_next != want_next) {
      ++failures;
      std::printf("FAIL categorical helper invalid case %zu\n", c);
    }
  }

  // This is the graph/helper ABI guard, not a Stan language input: a nonempty
  // descriptor must carry storage. It must fail before touching the engine.
  WaRng malformed(991), untouched(991);
  const RngException null_input = capture_rng_exception(
      [&] { (void)vector_integer_rng_draw(nullptr, 1, malformed); });
  if (null_input.kind != 3 || malformed.gen()() != untouched.gen()()) {
    ++failures;
    std::printf("FAIL categorical helper malformed pointer contract\n");
  }
}

static void test_multi_normal_rng_helper_contract() {
  using namespace stanli;
  struct Valid {
    std::vector<double> mu;
    std::vector<double> covariance;
  };
  const Valid valid[] = {
      {{-0.25}, {1.75}},
      {{0.5, -1.25}, {1.69, 0.364, 0.364, 0.64}},
      {{0.25, -0.5, 1.75}, {2.0, 0.25, -0.4, 0.25, 1.5, 0.3, -0.4, 0.3, 1.25}},
  };
  for (size_t c = 0; c < sizeof(valid) / sizeof(valid[0]); ++c) {
    for (unsigned seed : {1u, 29u, 2026u}) {
      const size_t k = valid[c].mu.size();
      std::vector<double> got(k);
      WaRng got_rng(seed + static_cast<unsigned>(c));
      WaRng want_rng(seed + static_cast<unsigned>(c));
      multi_normal_rng_draw(valid[c].mu.data(), k, valid[c].covariance.data(),
                            valid[c].covariance.size(), k, k, got.data(),
                            got.size(), got_rng);
      const std::vector<double> want = direct_multi_normal_rng(
          valid[c].mu, valid[c].covariance, k, k, want_rng);
      if (!same_double_bytes(got, want) ||
          got_rng.gen()() != want_rng.gen()()) {
        ++failures;
        std::printf("FAIL multi-normal helper valid case %zu seed %u\n", c,
                    seed);
      }
    }
  }

  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  struct Invalid {
    std::vector<double> mu;
    std::vector<double> covariance;
    size_t rows;
    size_t cols;
    const char* label;
  };
  // The asymmetric case uses four distinct column-major positions. Its Stan
  // message names both off-diagonal values, catching a transposed rebuild.
  const Invalid invalid[] = {
      {{nan, 0.0}, {1.0, 0.0, 0.0, 1.0}, 2, 2, "NaN mean"},
      {{inf, 0.0}, {1.0, 0.0, 0.0, 1.0}, 2, 2, "infinite mean"},
      {{0.0, 0.0}, {1.0, 0.0, nan, 1.0}, 2, 2, "NaN covariance"},
      {{0.0, 0.0}, {1.25, 0.75, 0.125, 0.8}, 2, 2, "asymmetric covariance"},
      {{0.0, 0.0}, {1.0, 2.0, 2.0, 1.0}, 2, 2, "non-PD covariance"},
      {{0.0, 0.0}, {1.0, 0.0, 0.0, 1.0}, 1, 4, "dimension mismatch"},
      {{}, {}, 0, 0, "empty covariance"},
  };
  for (size_t c = 0; c < sizeof(invalid) / sizeof(invalid[0]); ++c) {
    std::vector<double> got(invalid[c].mu.size());
    WaRng got_rng(static_cast<unsigned>(501 + c));
    WaRng want_rng(static_cast<unsigned>(501 + c));
    const RngException got_error = capture_rng_exception([&] {
      multi_normal_rng_draw(invalid[c].mu.data(), invalid[c].mu.size(),
                            invalid[c].covariance.data(),
                            invalid[c].covariance.size(), invalid[c].rows,
                            invalid[c].cols, got.data(), got.size(), got_rng);
    });
    const RngException want_error = capture_rng_exception([&] {
      (void)direct_multi_normal_rng(invalid[c].mu, invalid[c].covariance,
                                    invalid[c].rows, invalid[c].cols, want_rng);
    });
    if (got_error.kind == 0 || got_error.kind != want_error.kind ||
        got_error.message != want_error.message ||
        got_rng.gen()() != want_rng.gen()()) {
      ++failures;
      std::printf("FAIL multi-normal helper %s contract\n", invalid[c].label);
    }
  }

  const std::vector<double> mu{0.5, -1.25};
  const std::vector<double> covariance{1.69, 0.364, 0.364, 0.64};
  std::vector<double> output(2);
  WaRng malformed(991), untouched(991);
  const RngException mismatch = capture_rng_exception([&] {
    multi_normal_rng_draw(mu.data(), mu.size(), covariance.data(),
                          covariance.size(), 1, 3, output.data(), output.size(),
                          malformed);
  });
  const RngException null_location = capture_rng_exception([&] {
    multi_normal_rng_draw(nullptr, mu.size(), covariance.data(),
                          covariance.size(), 2, 2, output.data(), output.size(),
                          malformed);
  });
  if (mismatch.kind != 3 || null_location.kind != 3 ||
      malformed.gen()() != untouched.gen()()) {
    ++failures;
    std::printf("FAIL multi-normal malformed helper contract\n");
  }

  // A rejected helper call is reusable at the exact untouched draw.
  WaRng recovered(811), direct(811);
  std::vector<double> recovered_draw(2);
  (void)capture_rng_exception([&] {
    const std::vector<double> bad{1.0, 2.0, 2.0, 1.0};
    multi_normal_rng_draw(mu.data(), 2, bad.data(), 4, 2, 2,
                          recovered_draw.data(), 2, recovered);
  });
  multi_normal_rng_draw(mu.data(), 2, covariance.data(), 4, 2, 2,
                        recovered_draw.data(), 2, recovered);
  const std::vector<double> direct_draw =
      direct_multi_normal_rng(mu, covariance, 2, 2, direct);
  if (recovered_draw != direct_draw || recovered.gen()() != direct.gen()()) {
    ++failures;
    std::printf("FAIL multi-normal helper rejected-call reuse\n");
  }
}

static void test_multi_normal_rng_kernel_contract() {
  using namespace stanli;
  const Kernel* kernel = find_kernel(OP_RNG);
  if (kernel == nullptr || kernel->forward == nullptr) {
    ++failures;
    std::printf("FAIL multi-normal RNG kernel is not registered\n");
    return;
  }
  double mu[] = {0.5, -1.25};
  double covariance[] = {1.69, 0.364, 0.364, 0.64};
  double output[] = {-99.0, -99.0};
  int k = 2;
  WaRng rng(123), direct_rng(123);
  EvalState state{&rng};
  KernelCtx valid;
  valid.variant = kMultiNormalRngVariant;
  valid.n_in = 2;
  valid.in[0] = Desc{mu, 2};
  valid.in[1] = Desc{covariance, 4};
  valid.out = Desc{output, 2};
  valid.idata = &k;
  valid.n_idata = 1;
  valid.eval_state = &state;
  kernel->forward(valid);
  const std::vector<double> want = direct_multi_normal_rng(
      {mu[0], mu[1]}, {1.69, 0.364, 0.364, 0.64}, 2, 2, direct_rng);
  if (std::vector<double>(output, output + 2) != want ||
      rng.gen()() != direct_rng.gen()()) {
    ++failures;
    std::printf("FAIL multi-normal RNG valid kernel contract\n");
  }

  struct Mutation {
    const char* label;
    void (*apply)(KernelCtx&);
  };
  const Mutation malformed[] = {
      {"unknown variant", [](KernelCtx& c) { c.variant = 255; }},
      {"arity", [](KernelCtx& c) { c.n_in = 1; }},
      {"missing idata", [](KernelCtx& c) { c.n_idata = 0; }},
      {"null idata", [](KernelCtx& c) { c.idata = nullptr; }},
      {"mean length", [](KernelCtx& c) { c.in[0].len = 1; }},
      {"covariance length", [](KernelCtx& c) { c.in[1].len = 3; }},
      {"output length", [](KernelCtx& c) { c.out.len = 1; }},
      {"missing evaluation state",
       [](KernelCtx& c) { c.eval_state = nullptr; }},
  };
  for (size_t i = 0; i < sizeof(malformed) / sizeof(malformed[0]); ++i) {
    WaRng got(static_cast<unsigned>(900 + i));
    WaRng untouched(static_cast<unsigned>(900 + i));
    EvalState got_state{&got};
    KernelCtx ctx = valid;
    ctx.eval_state = &got_state;
    output[0] = output[1] = -99.0;
    malformed[i].apply(ctx);
    const RngException error =
        capture_rng_exception([&] { kernel->forward(ctx); });
    if (error.kind != 3 || got.gen()() != untouched.gen()() ||
        output[0] != -99.0 || output[1] != -99.0) {
      ++failures;
      std::printf("FAIL multi-normal malformed kernel %s\n",
                  malformed[i].label);
    }
  }
}

static uint64_t reduction_bits(double x) {
  uint64_t out = 0;
  std::memcpy(&out, &x, sizeof(out));
  return out;
}

static bool same_reduction_value(double got, double want) {
  // Eigen deliberately leaves the sign and payload of a propagated NaN
  // unspecified.  Sanitizer instrumentation can therefore change those bits
  // without changing the reduction's value contract.  Everything else --
  // including signed zero and infinity -- remains a bitwise comparison.
  if (std::isnan(got) || std::isnan(want))
    return std::isnan(got) && std::isnan(want);
  return reduction_bits(got) == reduction_bits(want);
}

static void test_product_exact_grouping() {
  using namespace stanli;
  // Direct kernel calls need the one-time registration normally performed by
  // Executor construction.
  {
    Graph graph;
    const int in = graph.add_slot(1, true);
    const int out = graph.add_slot(1, false);
    graph.add_op(OP_EXPV, {in}, out);
    graph.result_slot = out;
    Executor warm(std::move(graph));
  }

  const Kernel& product = kernel(OP_PROD_VEC);
  if (product.backward == nullptr || product.scratch_size != nullptr) {
    ++failures;
    std::printf("FAIL product kernel implementation contract\n");
  }

  const double denorm = std::numeric_limits<double>::denorm_min();
  const double inf = std::numeric_limits<double>::infinity();
  const double nan = std::numeric_limits<double>::quiet_NaN();
  std::vector<std::vector<double>> cases = {
      {0.5},
      {-0.0},
      {denorm},
      {inf},
      {nan},
      {0.5, 0.5, 0.5},
      {0.0, inf, 2.0},
      {-0.0, inf, 2.0},
      {inf, 0.0, -2.0},
      {nan, 0.0, inf},
      {1.0 + 0x1p-52, 1.0 - 0x1p-52, 1.0 + 0x1p-51},
      {0.5, 0.5, 0.5, 0.5, 0.5},
      {1e200, 1e200, 1e-200, 1e-200, 3.0},
      {1e200, 1e-200, 1e200, 1e-200, 3.0},
      {denorm, 2.0, 2.0, 2.0, 2.0},
      {-0.0, -1.0, -1.0, 1.0, 1.0},
      {inf, -0.0, nan, -inf, 1.0},
  };

  // Exercise every packet boundary on the build host.  Fixed lengths 1/3/5
  // above pin the public contract; these sizes keep the same test meaningful
  // when Eigen is compiled for SSE, AVX2, or AVX-512.
  const int packet_width = std::max(
      1, static_cast<int>(Eigen::internal::packet_traits<double>::size));
  const int packet_lengths[] = {packet_width - 1, packet_width,
                                packet_width + 1, 2 * packet_width + 1};
  const int exponents[] = {500, -500, 400, -400, 300, -300, 0, 0};
  for (int n : packet_lengths) {
    if (n <= 0) continue;  // Scalar-only Eigen has packet width one.
    std::vector<double> values(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
      const double mantissa =
          1.0 + static_cast<double>((i * 5 + 1) % 7) * 0x1p-52;
      values[static_cast<size_t>(i)] =
          std::ldexp((i % 3 == 0) ? -mantissa : mantissa, exponents[i % 8]);
    }
    cases.push_back(std::move(values));
  }

  std::map<std::string, const mir::FunDef*> funs;
  MirInterp<double> interp(funs, "product grouping test");
  mir::Expr variable;
  variable.kind = mir::Expr::Var;
  variable.name = "x";
  variable.type_ = "UVector";
  variable.unsized.leaf = mir::UnsizedLeaf::Vector;
  mir::Expr call;
  call.kind = mir::Expr::FunApp;
  call.name = "prod";
  call.fn_lib = mir::Expr::Lib::StanLib;
  call.type_ = "UReal";
  call.unsized.leaf = mir::UnsizedLeaf::Real;
  call.args = {variable};

  for (size_t c = 0; c < cases.size(); ++c) {
    const std::vector<double>& values = cases[c];
    Eigen::VectorXd pinned(static_cast<Eigen::Index>(values.size()));
    for (size_t i = 0; i < values.size(); ++i) pinned[i] = values[i];
    const double want = stan::math::prod(pinned);

    DataMap::Entry entry;
    entry.r = values;
    entry.dims = {static_cast<int64_t>(values.size())};
    interp.env()["x"] = std::move(entry);
    const double interpreted = interp.eval(call).r.at(0);
    if (!same_reduction_value(interpreted, want)) {
      ++failures;
      std::printf("FAIL product interpreter case %zu: got %llx want %llx\n", c,
                  static_cast<unsigned long long>(reduction_bits(interpreted)),
                  static_cast<unsigned long long>(reduction_bits(want)));
    }

    // Pin the address-independence seam at every packet-relevant shift.  A
    // direct Eigen::Map differs at odd shifts; OP_PROD_VEC must not.
    for (int offset = 0; offset < packet_width; ++offset) {
      Eigen::VectorXd storage(static_cast<Eigen::Index>(
          values.size() + static_cast<size_t>(packet_width)));
      storage.setZero();
      std::copy(values.begin(), values.end(), storage.data() + offset);
      double got = 0.0;
      KernelCtx ctx;
      ctx.n_in = 1;
      ctx.in[0] =
          Desc{storage.data() + offset, static_cast<int64_t>(values.size())};
      ctx.out = Desc{&got, 1};
      product.forward(ctx);
      if (!same_reduction_value(got, want)) {
        ++failures;
        std::printf(
            "FAIL product kernel case %zu offset %d: got %llx want "
            "%llx\n",
            c, offset, static_cast<unsigned long long>(reduction_bits(got)),
            static_cast<unsigned long long>(reduction_bits(want)));
      }
    }
  }

  // This tranche changes only Eigen vector/row-vector products.  Stan arrays
  // retain MirInterp's existing ascending scalar fold.
  mir::Expr array_variable = variable;
  array_variable.name = "a";
  array_variable.type_ = "(UArray UReal)";
  array_variable.unsized.leaf = mir::UnsizedLeaf::Real;
  array_variable.unsized.depth = 1;
  mir::Expr array_call = call;
  array_call.args = {array_variable};
  const std::vector<double> array_values = {1e200, 1e200, 1e-200, 1e-200, 3.0};
  DataMap::Entry array_entry;
  array_entry.r = array_values;
  array_entry.dims = {static_cast<int64_t>(array_values.size())};
  interp.env()["a"] = std::move(array_entry);
  double array_want = 1.0;
  for (double value : array_values) array_want *= value;
  const double array_got = interp.eval(array_call).r.at(0);
  if (reduction_bits(array_got) != reduction_bits(array_want)) {
    ++failures;
    std::printf("FAIL array product changed its scalar left fold\n");
  }

  // Pin the actual expression surfaces admitted by lower.cpp.  CmdStan
  // gives Eigen the unevaluated CwiseBinary expression; stanli materializes
  // subtraction into a graph slot before OP_PROD_VEC.  Basic subtraction is
  // lane-bit-transparent, so the two product groupings must still agree.
  const auto check_minus_surface = [&](int n, const char* label) {
    std::vector<Eigen::VectorXd> indexed(2, Eigen::VectorXd(n));
    const double xs[] = {0.5,  -0.25, 2.0, -1.0, std::nextafter(1.0, 0.0),
                         -0.5, 0.25,  1.5};
    for (int i = 0; i < n; ++i)
      indexed[1][i] = xs[i % static_cast<int>(sizeof(xs) / sizeof(xs[0]))];

    const uint64_t direct =
        reduction_bits(stan::math::prod(Eigen::VectorXd::Ones(n) - indexed[1]));
    const uint64_t indexed_transpose = reduction_bits(
        stan::math::prod(Eigen::RowVectorXd::Ones(n) - indexed[1].transpose()));
    if (direct != indexed_transpose) {
      ++failures;
      std::printf(
          "FAIL %s pinned outer-minus surfaces disagree: %llx vs "
          "%llx\n",
          label, static_cast<unsigned long long>(direct),
          static_cast<unsigned long long>(indexed_transpose));
    }

    std::vector<double> materialized(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
      materialized[static_cast<size_t>(i)] = 1.0 - indexed[1][i];
    for (int offset = 0; offset < packet_width; ++offset) {
      Eigen::VectorXd storage(n + packet_width);
      storage.setZero();
      std::copy(materialized.begin(), materialized.end(),
                storage.data() + offset);
      double got = 0.0;
      KernelCtx ctx;
      ctx.n_in = 1;
      ctx.in[0] = Desc{storage.data() + offset, n};
      ctx.out = Desc{&got, 1};
      product.forward(ctx);
      const uint64_t bits = reduction_bits(got);
      if (bits != direct || bits != indexed_transpose) {
        ++failures;
        std::printf("FAIL %s offset %d: got %llx want %llx\n", label, offset,
                    static_cast<unsigned long long>(bits),
                    static_cast<unsigned long long>(direct));
      }
    }
  };
  check_minus_surface(5, "len5 outer-minus product");
  check_minus_surface(2 * packet_width + 1,
                      "packet-boundary outer-minus product");

  // The corpus surface indexes a row of a column-major matrix and transposes
  // it.  That Eigen block remains strided, disables packet access on the
  // outer subtraction, and makes prod reduce from coefficient zero in
  // ascending scalar order.  Search deterministic valid probabilities for a
  // case that distinguishes that order from the packet variant, then pin the
  // raw-expression oracle and the materialized graph kernel to the scalar
  // result.  Scalar-only Eigen builds have no distinct packet order.
  const int matrix_n = 2 * packet_width + 1;
  Eigen::MatrixXd matrix(2, matrix_n);
  std::vector<double> factors(static_cast<size_t>(matrix_n));
  bool distinguished = packet_width == 1;
  uint64_t state = 0x4d4154524958524fULL;
  for (int trial = 0; trial < 4096 && !distinguished; ++trial) {
    for (int i = 0; i < matrix_n; ++i) {
      state = state * 6364136223846793005ULL + 1442695040888963407ULL;
      const double p = 0.01 + 0.98 * static_cast<double>(state >> 11) /
                                  static_cast<double>(uint64_t{1} << 53);
      matrix(0, i) = 0.25;
      matrix(1, i) = p;
      factors[static_cast<size_t>(i)] = 1.0 - p;
    }
    double scalar = factors[0];
    for (int i = 1; i < matrix_n; ++i)
      scalar *= factors[static_cast<size_t>(i)];

    double packet = 0.0;
    KernelCtx packet_ctx;
    packet_ctx.n_in = 1;
    packet_ctx.in[0] = Desc{factors.data(), matrix_n};
    packet_ctx.out = Desc{&packet, 1};
    packet_ctx.variant = 0;
    product.forward(packet_ctx);
    if (reduction_bits(packet) == reduction_bits(scalar)) continue;

    const double direct = stan::math::prod(Eigen::VectorXd::Ones(matrix_n) -
                                           matrix.row(1).transpose());
    double got = 0.0;
    KernelCtx scalar_ctx = packet_ctx;
    scalar_ctx.out = Desc{&got, 1};
    scalar_ctx.variant = 1;
    product.forward(scalar_ctx);
    if (reduction_bits(direct) != reduction_bits(scalar) ||
        reduction_bits(got) != reduction_bits(scalar)) {
      ++failures;
      std::printf("FAIL matrix-row product did not use scalar grouping\n");
    }
    distinguished = true;
  }
  if (!distinguished) {
    ++failures;
    std::printf("FAIL matrix-row oracle did not distinguish packet order\n");
  }
}

static void test_reduction_view_grouping() {
  using namespace stanli;
  const Kernel& product = kernel(OP_PROD_VEC);
  const Kernel& extrema = kernel(OP_EXTREMA_VEC);
  const int phase_modulus = static_cast<int>(extrema_phase_modulus());
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double tie_pool[] = {0.0, -0.0, 2.0, -3.0, 2.0, nan, -0.0,
                             0.0, -4.0, 9.0, -4.0, 1.0, nan, -2.0};
  const int tie_pool_size =
      static_cast<int>(sizeof(tie_pool) / sizeof(*tie_pool));

  // A graph value has already been materialized, so the reduction opcode
  // carries the source view's phase explicitly. Compare both native kernels
  // with Stan Math reducing the original Eigen segment.
  for (int n = 1; n <= 257; ++n) {
    for (int offset = 0; offset < phase_modulus; ++offset) {
      Eigen::VectorXd product_base(offset + n + 2);
      Eigen::VectorXd extrema_base(offset + n + 2);
      for (int i = 0; i < product_base.size(); ++i) {
        const int centered = (i * 37 + n * 11) % 29 - 14;
        product_base[i] = 1.0 + centered * 0x1p-16;
        extrema_base[i] = tie_pool[i % tie_pool_size];
      }
      const Eigen::VectorXd product_owned = product_base.segment(offset, n);
      const Eigen::VectorXd extrema_owned = extrema_base.segment(offset, n);
      const double product_want =
          stan::math::prod(product_base.segment(offset, n));
      const double extrema_wants[] = {
          stan::math::min(extrema_base.segment(offset, n)),
          stan::math::max(extrema_base.segment(offset, n))};

      int phase = offset;
      double product_got = 0.0;
      KernelCtx product_ctx;
      product_ctx.n_in = 1;
      product_ctx.in[0] = Desc{const_cast<double*>(product_owned.data()), n};
      product_ctx.out = Desc{&product_got, 1};
      product_ctx.variant = 4;
      product_ctx.idata = &phase;
      product_ctx.n_idata = 1;
      product.forward(product_ctx);
      if (!same_reduction_value(product_got, product_want)) {
        ++failures;
        std::printf("FAIL phased product n=%d offset=%d\n", n, offset);
      }

      for (int maximum = 0; maximum < 2; ++maximum) {
        double got = 0.0;
        KernelCtx ctx;
        ctx.n_in = 1;
        ctx.in[0] = Desc{const_cast<double*>(extrema_owned.data()), n};
        ctx.out = Desc{&got, 1};
        ctx.variant = static_cast<uint8_t>(maximum | 4);
        ctx.idata = &phase;
        ctx.n_idata = 1;
        extrema.forward(ctx);
        if (!same_reduction_value(got, extrema_wants[maximum])) {
          ++failures;
          std::printf("FAIL phased extrema n=%d offset=%d %s\n", n, offset,
                      maximum ? "max" : "min");
        }
      }
    }
  }

  // The contract here is exact parity with the original view. Some Eigen
  // configurations use the same scalar traversal for every tested size, so
  // requiring a value difference from an unphased reduction would be brittle.

  // Gathered arguments have no packet access. Repeated indices are retained
  // in index order and the scalar reduction's reverse path must scatter back
  // through OP_GATHER rather than treating the gather as a copy.
  bool gather_distinguished = false;
  for (int n = 1; n <= 20; ++n) {
    Eigen::VectorXd source(n);
    for (int i = 0; i < n; ++i) source[i] = tie_pool[i % tie_pool_size];
    std::vector<int> positions(static_cast<size_t>(n));
    std::vector<double> gathered(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
      positions[static_cast<size_t>(i)] = n - i;
      gathered[static_cast<size_t>(i)] = source[n - i - 1];
    }
    const stan::model::index_multi index(positions);
    for (int maximum = 0; maximum < 2; ++maximum) {
      const double want =
          maximum
              ? stan::math::max(stan::model::rvalue(source, "source", index))
              : stan::math::min(stan::model::rvalue(source, "source", index));
      double got = 0.0;
      double selected = 0.0;
      KernelCtx ctx;
      ctx.n_in = 1;
      ctx.in[0] = Desc{gathered.data(), n};
      ctx.out = Desc{&got, 1};
      ctx.variant = static_cast<uint8_t>(maximum | 2);
      ctx.scratch = &selected;
      extrema.forward(ctx);
      if (!same_reduction_value(got, want)) {
        ++failures;
        std::printf("FAIL gathered extrema n=%d %s\n", n,
                    maximum ? "max" : "min");
      }
      const Eigen::Map<const Eigen::VectorXd> packet(gathered.data(), n);
      const double packet_want =
          maximum ? stan::math::max(packet) : stan::math::min(packet);
      gather_distinguished |= !same_reduction_value(packet_want, want);
    }
  }
  if (!gather_distinguished) {
    ++failures;
    std::printf(
        "FAIL gathered extrema oracle never distinguished packet order\n");
  }

  {
    Graph graph;
    const int source = graph.add_slot(3, true);
    const int gathered = graph.add_slot(4, false);
    const int out = graph.add_slot(1, false);
    graph.add_op(OP_GATHER, {source}, gathered, {2, 2, 0, 2});
    const int product_op = graph.add_op(OP_PROD_VEC, {gathered}, out);
    graph.ops[product_op].variant = 1;
    graph.result_slot = out;
    Executor executor(std::move(graph));
    const std::array<double, 3> values = {2.0, 3.0, 5.0};
    std::copy(values.begin(), values.end(), executor.params_data());
    double adj[3] = {};
    const double value = executor.gradient(adj);
    if (value != 250.0 || adj[0] != 125.0 || adj[1] != 0.0 || adj[2] != 150.0) {
      ++failures;
      std::printf("FAIL gathered product adjoints were not accumulated\n");
    }
  }
  {
    Graph graph;
    const int source = graph.add_slot(3, true);
    const int gathered = graph.add_slot(4, false);
    const int out = graph.add_slot(1, false);
    graph.add_op(OP_GATHER, {source}, gathered, {1, 1, 2, 0});
    const int extrema_op = graph.add_op(OP_EXTREMA_VEC, {gathered}, out);
    graph.ops[extrema_op].variant = 2;
    graph.result_slot = out;
    Executor executor(std::move(graph));
    const std::array<double, 3> values = {5.0, 2.0, 7.0};
    std::copy(values.begin(), values.end(), executor.params_data());
    double adj[3] = {};
    const double value = executor.gradient(adj);
    if (value != 2.0 || adj[0] != 0.0 || adj[1] != 1.0 || adj[2] != 0.0) {
      ++failures;
      std::printf(
          "FAIL gathered extrema adjoint did not route through gather\n");
    }
  }
}

static void test_extrema_exact_grouping() {
  using namespace stanli;
  const Kernel& extrema = kernel(OP_EXTREMA_VEC);
  if (extrema.backward == nullptr || extrema.scratch_size == nullptr) {
    ++failures;
    std::printf("FAIL extrema kernel has no differentiable implementation\n");
  }

  const double inf = std::numeric_limits<double>::infinity();
  const double nan = std::numeric_limits<double>::quiet_NaN();
  std::vector<std::vector<double>> cases = {
      {},
      {1.0},
      {0.0, -0.0},
      {-0.0, 0.0},
      {inf, -inf, 3.0},
      {-inf, inf, -3.0},
      {2.0, 2.0, -4.0, -4.0, 2.0},
      {nan, 1.0, -2.0},
      {1.0, nan, -2.0},
      {1.0, -2.0, nan},
      {nan, nan, 0.0, -0.0},
  };
  const int packet_width = std::max(
      1, static_cast<int>(Eigen::internal::packet_traits<double>::size));
  for (int n : {packet_width - 1, packet_width, packet_width + 1,
                2 * packet_width - 1, 2 * packet_width, 2 * packet_width + 1}) {
    if (n < 0) continue;
    std::vector<double> values(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
      const double magnitude =
          std::ldexp(1.0 + static_cast<double>((i * 7 + 3) % 11) * 0x1p-52,
                     (i % 7) * 137 - 411);
      values[static_cast<size_t>(i)] = i % 2 == 0 ? magnitude : -magnitude;
    }
    if (n > 2) values[static_cast<size_t>(n - 1)] = values[1];
    cases.push_back(std::move(values));
  }

  for (size_t c = 0; c < cases.size(); ++c) {
    const std::vector<double>& values = cases[c];
    Eigen::VectorXd pinned(static_cast<Eigen::Index>(values.size()));
    std::copy(values.begin(), values.end(), pinned.data());
    const double wants[] = {stan::math::min(pinned), stan::math::max(pinned)};
    for (int variant = 0; variant < 2; ++variant) {
      for (int offset = 0; offset < packet_width; ++offset) {
        Eigen::VectorXd storage(static_cast<Eigen::Index>(
            values.size() + static_cast<size_t>(packet_width)));
        storage.setZero();
        std::copy(values.begin(), values.end(), storage.data() + offset);
        double got = 0.0;
        KernelCtx ctx;
        ctx.n_in = 1;
        ctx.in[0] =
            Desc{storage.data() + offset, static_cast<int64_t>(values.size())};
        ctx.out = Desc{&got, 1};
        ctx.variant = static_cast<uint8_t>(variant);
        extrema.forward(ctx);
        if (!same_reduction_value(got, wants[variant])) {
          ++failures;
          std::printf(
              "FAIL extrema kernel case %zu variant %d offset %d: got %llx "
              "want %llx\n",
              c, variant, offset,
              static_cast<unsigned long long>(reduction_bits(got)),
              static_cast<unsigned long long>(reduction_bits(wants[variant])));
        }
      }

      std::vector<double> active_values = values;
      Eigen::Matrix<stan::math::var, -1, 1> active(
          static_cast<Eigen::Index>(values.size()));
      for (size_t i = 0; i < values.size(); ++i)
        active(static_cast<Eigen::Index>(i)) = values[i];
      stan::math::var want_active =
          variant == 0 ? stan::math::min(active) : stan::math::max(active);
      double got_active = 0.0;
      double selected = 0.0;
      std::vector<double> adj(values.size(), 0.0);
      KernelCtx active_ctx;
      active_ctx.n_in = 1;
      active_ctx.in[0] =
          Desc{active_values.data(), static_cast<int64_t>(values.size())};
      active_ctx.out = Desc{&got_active, 1};
      active_ctx.scratch = &selected;
      active_ctx.variant = static_cast<uint8_t>(variant | 2);
      extrema.forward(active_ctx);
      active_ctx.in_adj[0] = Desc{adj.data(), static_cast<int64_t>(adj.size())};
      active_ctx.out_adj = 1.0;
      extrema.backward(active_ctx);
      want_active.grad();
      if (!same_reduction_value(got_active, want_active.val())) {
        ++failures;
        std::printf("FAIL active extrema value case %zu variant %d\n", c,
                    variant);
      }
      for (size_t i = 0; i < adj.size(); ++i) {
        if (adj[i] == active(static_cast<Eigen::Index>(i)).adj()) continue;
        ++failures;
        std::printf(
            "FAIL active extrema adjoint case %zu variant %d index %zu\n", c,
            variant, i);
      }
      stan::math::recover_memory();
    }
  }

  // The generic interpreter keeps scalar-fold behavior outside the audited
  // direct write_array surface, except that Stan's defined empty-container
  // results/errors must not become an out_of_range accident.
  std::map<std::string, const mir::FunDef*> funs;
  MirInterp<double> interp(funs, "empty extrema legacy test");
  mir::Expr arg;
  arg.kind = mir::Expr::Var;
  arg.name = "a";
  arg.type_ = "(UArray UReal)";
  arg.unsized.leaf = mir::UnsizedLeaf::Real;
  arg.unsized.depth = 1;
  mir::Expr call;
  call.kind = mir::Expr::FunApp;
  call.name = "min";
  call.fn_lib = mir::Expr::Lib::StanLib;
  call.type_ = "UReal";
  call.unsized.leaf = mir::UnsizedLeaf::Real;
  call.args = {arg};
  DataMap::Entry empty_real;
  empty_real.dims = {0};
  interp.env()["a"] = empty_real;
  if (reduction_bits(interp.eval(call).r.at(0)) != reduction_bits(inf)) {
    ++failures;
    std::printf("FAIL empty real legacy min is not +infinity\n");
  }
  call.name = "max";
  if (reduction_bits(interp.eval(call).r.at(0)) != reduction_bits(-inf)) {
    ++failures;
    std::printf("FAIL empty real legacy max is not -infinity\n");
  }
  arg.type_ = "(UArray UInt)";
  arg.unsized.leaf = mir::UnsizedLeaf::Int;
  call.args = {arg};
  DataMap::Entry empty_int;
  empty_int.is_int = true;
  empty_int.dims = {0};
  interp.env()["a"] = empty_int;
  for (const char* name : {"min", "max"}) {
    call.name = name;
    bool invalid = false;
    try {
      (void)interp.eval(call);
    } catch (const std::invalid_argument&) {
      invalid = true;
    }
    if (!invalid) {
      ++failures;
      std::printf(
          "FAIL empty int legacy %s did not preserve invalid_argument\n", name);
    }
  }
}

// The structural kernel selects fixed-width outer-array elements and
// scatters their adjoints back to the chosen block only.
static void test_dynamic_slice_kernel() {
  const Kernel* k = find_kernel(OP_DYNAMIC_SLICE);
  double base[6] = {10, 11, 20, 21, 30, 31};
  double index = 2.0, out[2] = {0, 0};
  double base_adj[6] = {0, 0, 0, 0, 0, 0};
  double out_adj[2] = {3, 5};
  const int idata[1] = {3};
  KernelCtx ctx;
  ctx.n_in = 2;
  ctx.in[0] = Desc{base, 6};
  ctx.in[1] = Desc{&index, 1};
  ctx.out = Desc{out, 2};
  ctx.idata = idata;
  ctx.n_idata = 1;
  k->forward(ctx);
  if (out[0] != 20 || out[1] != 21) {
    ++failures;
    std::printf("FAIL dynamic slice selected the wrong block\n");
  }
  ctx.in_adj[0] = Desc{base_adj, 6};
  ctx.out_adj_vec = Desc{out_adj, 2};
  k->backward(ctx);
  const double want_adj[6] = {0, 0, 3, 5, 0, 0};
  if (!std::equal(std::begin(base_adj), std::end(base_adj), want_adj)) {
    ++failures;
    std::printf("FAIL dynamic slice scattered to the wrong block\n");
  }
  for (double bad : {0.0, 1.5, 4.0, std::numeric_limits<double>::quiet_NaN()}) {
    index = bad;
    bool threw = false;
    try {
      k->forward(ctx);
    } catch (const std::out_of_range&) {
      threw = true;
    }
    if (!threw) {
      ++failures;
      std::printf("FAIL dynamic slice accepted invalid index %.17g\n", bad);
    }
  }
  KernelCtx malformed = ctx;
  malformed.in[0].len = 5;
  bool malformed_threw = false;
  try {
    k->forward(malformed);
  } catch (const std::logic_error&) {
    malformed_threw = true;
  }
  if (!malformed_threw) {
    ++failures;
    std::printf("FAIL dynamic slice accepted malformed geometry\n");
  }
}

int main() {
  {
    Graph graph;
    const int in = graph.add_slot(1, true);
    const int out = graph.add_slot(1, false);
    graph.add_op(OP_EXPV, {in}, out);
    graph.result_slot = out;
    Executor warm(std::move(graph));
  }
  test_add_diag_scalar_reverse_order();
  test_categorical_existing_adjoint();
  test_categorical_native_scalar();
  test_categorical_native_kernel_edges();
  test_categorical_logit_scalar_bits();
  test_categorical_array_scaled();
  test_mult_lower_tri_self_transpose_pullback();
  direct_index_kernel_tests();
  probe_mode_tests();
  test_binomial_rng_helper_contract();
  test_categorical_rng_helper_contract();
  test_multi_normal_rng_helper_contract();
  test_multi_normal_rng_kernel_contract();
  test_product_exact_grouping();
  test_reduction_view_grouping();
  test_extrema_exact_grouping();
  test_dynamic_slice_kernel();
  test_stan_rng_stream_contract();
  if (failures == 0) std::printf("test_kernel_contracts OK\n");
  return failures == 0 ? 0 : 1;
}
