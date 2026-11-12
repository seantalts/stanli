#include <stanli/graph.hpp>
#include <stanli/optable.hpp>
#include <stanli/structured_loop.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>

using namespace stanli;

static int failures = 0;
static void check(bool ok, const std::string& what) {
  if (!ok) {
    ++failures;
    std::printf("FAIL %s\n", what.c_str());
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
  direct_index_kernel_tests();
  test_dynamic_slice_kernel();
  if (failures == 0) std::printf("test_dynamic_index OK\n");
  return failures == 0 ? 0 : 1;
}
