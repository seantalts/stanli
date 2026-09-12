#include "env_helpers.hpp"
#include "stdout_capture.hpp"

#include <stanli/compile.hpp>
#include <stanli/graph.hpp>
#include <stanli/optable.hpp>
#include <stanli/structured_loop.hpp>
#include <stanli/wa_interp.hpp>
#include <stan/math.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace stanli;
using Node = StructuredLoop::Node;

static int failures = 0;

static bool near(double actual, double expected) {
  const double tolerance =
      1e-12 * std::max({1.0, std::fabs(actual), std::fabs(expected)});
  return actual == expected ||
         (std::isfinite(actual) && std::isfinite(expected) &&
          std::fabs(actual - expected) <= tolerance);
}

static void check(bool ok, const char* name) {
  if (!ok) {
    ++failures;
    std::printf("FAIL %s\n", name);
  }
}

static void close(double actual, double expected, const char* name) {
  if (!near(actual, expected)) {
    std::printf("  %.17g != %.17g\n", actual, expected);
    check(false, name);
  }
}

static Node sequence(std::vector<Node> children) {
  Node node;
  node.children = std::move(children);
  return node;
}

static Node call(StructuredLoop& plan, uint16_t opcode,
                 std::initializer_list<int> inputs, int output) {
  Node node;
  node.kind = Node::KernelCall;
  node.op = plan.body.add_op(opcode, inputs, output);
  return node;
}

static Node alias(int destination, int source) {
  Node node;
  node.kind = Node::Alias;
  node.dst = destination;
  node.src = source;
  return node;
}

static Node target(int source) {
  Node node;
  node.kind = Node::Target;
  node.src = source;
  return node;
}

static int scalar(StructuredLoop& plan, double value) {
  const int slot = plan.body.add_slot(1, false);
  plan.fills.push_back({slot, {value}});
  return slot;
}

static Node counted(int lower, int upper, int iterator, Node body) {
  Node node;
  node.kind = Node::For;
  node.lower = lower;
  node.upper = upper;
  node.iterator = iterator;
  node.children.push_back(std::move(body));
  return node;
}

static Node branch(int condition, Node yes, Node no) {
  Node node;
  node.kind = Node::If;
  node.condition = condition;
  node.children = {std::move(yes), std::move(no)};
  return node;
}

static Node while_loop(int condition, Node condition_body, Node body) {
  Node node;
  node.kind = Node::While;
  node.condition = condition;
  node.children = {std::move(condition_body), std::move(body)};
  return node;
}

static Node* find_call(Node& node, int op) {
  if (node.kind == Node::KernelCall && node.op == op) return &node;
  for (auto& child : node.children)
    if (Node* found = find_call(child, op)) return found;
  return nullptr;
}

static bool set_forward(Node& node, int op, void (*forward)(KernelCtx&)) {
  Node* found = find_call(node, op);
  if (found) found->forward = forward;
  return found != nullptr;
}

static bool set_backward(Node& node, int op, void (*backward)(KernelCtx&)) {
  Node* found = find_call(node, op);
  if (found) found->backward = backward;
  return found != nullptr;
}

static void attach(StructuredLoop& plan, int op,
                   std::shared_ptr<DynamicIndexSpec> spec) {
  plan.body.ops[static_cast<size_t>(op)].udata = spec.get();
  plan.body.udata_pool.push_back(std::move(spec));
}

static std::shared_ptr<DynamicIndexSpec> single_spec(int64_t extent) {
  auto spec = std::make_shared<DynamicIndexSpec>();
  spec->axes = {{DynamicIndexSpec::Axis::Single, extent, 1, 1, 0}};
  spec->selected_size = 1;
  return spec;
}

static Graph outer(std::shared_ptr<StructuredLoop> plan,
                   std::vector<int64_t> param_lens = {1, 1},
                   std::vector<int64_t> data_lens = {}) {
  Graph graph;
  std::vector<int> inputs;
  for (int64_t len : param_lens) inputs.push_back(graph.add_slot(len, true));
  for (int64_t len : data_lens) inputs.push_back(graph.add_slot(len, false));
  int64_t output_len = plan->has_target ? 1 : 0;
  for (int slot : plan->outputs) output_len += plan->body.slots[slot].len;
  const int output = graph.add_slot(output_len, false);
  Op op;
  op.opcode = OP_LOOP;
  op.out = output;
  for (int input : inputs) op.in[op.n_in++] = input;
  op.udata = plan.get();
  graph.ops.push_back(op);
  graph.udata_pool.push_back(std::move(plan));
  graph.result_slot = output;
  if (output_len != 1) {
    graph.result_slot = graph.add_slot(1, false);
    graph.add_op(OP_INDEX, {output}, graph.result_slot, {0});
  }
  return graph;
}

// Plans whose tests observe individual kernel calls keep every call as its
// own node; segment_tests covers the register-machine path.
static void prepare_kernels(StructuredLoop& plan) {
  test_setenv("STANLI_NO_STRUCTURED_SEGMENTS", "1");
  plan.prepare();
  test_unsetenv("STANLI_NO_STRUCTURED_SEGMENTS");
}

template <class Build>
static std::shared_ptr<StructuredLoop> unsegmented(Build build) {
  test_setenv("STANLI_NO_STRUCTURED_SEGMENTS", "1");
  auto plan = build();
  test_unsetenv("STANLI_NO_STRUCTURED_SEGMENTS");
  check(plan->segments.empty(), "segments are off under the switch");
  return plan;
}

struct Evaluation {
  double value = 0;
  double gradient[2] = {0, 0};
};

static Evaluation evaluate(Executor& executor, double theta, double beta) {
  executor.params_data()[0] = theta;
  executor.params_data()[1] = beta;
  Evaluation result;
  result.value = executor.gradient(result.gradient);
  return result;
}

static std::atomic<int> counted_forward_calls{0};
static std::atomic<int> counted_backward_calls{0};
static void count_mul_forward(KernelCtx& context) {
  ++counted_forward_calls;
  find_kernel(OP_MUL)->forward(context);
}
static void count_mul_backward(KernelCtx& context) {
  ++counted_backward_calls;
  find_kernel(OP_MUL)->backward(context);
}

static void transient_classification_tests() {
  {
    auto plan = std::make_shared<StructuredLoop>();
    const int theta = plan->body.add_slot(1, false);
    const int lower = scalar(*plan, 1);
    const int upper = scalar(*plan, 3);
    const int iterator = plan->body.add_slot(1, false);
    const int zero = scalar(*plan, 0);
    const int two = scalar(*plan, 2);
    const int condition = plan->body.add_slot(1, false);
    const int result = plan->body.add_slot(1, false);
    const int updated = plan->body.add_slot(1, false);
    plan->imports = {{theta, 0, 0, true}};
    Node compare = call(*plan, OP_COMPARE, {iterator, two}, condition);
    const int compare_op = compare.op;
    Node update = call(*plan, OP_ADD, {result, theta}, updated);
    const int update_op = update.op;
    plan->root =
        sequence({alias(result, zero),
                  counted(lower, upper, iterator,
                          sequence({std::move(compare),
                                    branch(condition,
                                           sequence({std::move(update),
                                                     alias(result, updated)}),
                                           sequence({}))}))});
    plan->outputs = {result};
    plan->prepare();
    const Node* compare_node = find_call(plan->root, compare_op);
    const Node* update_node = find_call(plan->root, update_op);
    check(compare_node && compare_node->storage == Node::Transient &&
              !compare_node->active,
          "compare feeding a branch is transient");
    check(update_node && update_node->storage == Node::Retained &&
              update_node->active,
          "active add feeding an alias is retained");
    Executor executor(outer(plan));
    const Evaluation result_value = evaluate(executor, .25, 0);
    close(result_value.value, .25, "transient compare branch value");
    close(result_value.gradient[0], 1, "transient compare branch gradient");
  }
  {
    auto plan = std::make_shared<StructuredLoop>();
    const int lower = scalar(*plan, 1);
    const int upper = scalar(*plan, 3);
    const int iterator = plan->body.add_slot(1, false);
    const int two = scalar(*plan, 2);
    const int condition = plan->body.add_slot(1, false);
    const int flag = plan->body.add_slot(1, false);
    Node compare = call(*plan, OP_COMPARE, {iterator, two}, condition);
    const int compare_op = compare.op;
    plan->root =
        counted(lower, upper, iterator,
                sequence({std::move(compare), alias(flag, condition)}));
    plan->outputs = {flag};
    plan->prepare();
    const Node* compare_node = find_call(plan->root, compare_op);
    check(compare_node && compare_node->storage == Node::Retained,
          "compare feeding an alias is retained");
    Executor executor(outer(plan));
    close(evaluate(executor, 0, 0).value, 0, "aliased compare final value");
  }
}

static void invariant_active_reuse_tests() {
  auto plan = std::make_shared<StructuredLoop>();
  const int theta = plan->body.add_slot(1, false);
  const int lower = scalar(*plan, 1);
  const int upper = scalar(*plan, 3);
  const int iterator = plan->body.add_slot(1, false);
  const int zero = scalar(*plan, 0);
  const int two = scalar(*plan, 2);
  const int scaled = plan->body.add_slot(1, false);
  const int result = plan->body.add_slot(1, false);
  const int next = plan->body.add_slot(1, false);
  plan->imports = {{theta, 0, 0, true}};
  Node scale = call(*plan, OP_MUL, {theta, two}, scaled);
  const int scale_op = scale.op;
  plan->root =
      sequence({alias(result, zero),
                counted(lower, upper, iterator,
                        sequence({std::move(scale),
                                  call(*plan, OP_ADD, {result, scaled}, next),
                                  alias(result, next)}))});
  plan->outputs = {result};
  plan->prepare();
  const Node* scale_node = find_call(plan->root, scale_op);
  check(scale_node && scale_node->active && scale_node->invariant_loop == 0,
        "active loop-invariant kernel is cached against its loop");
  check(set_forward(plan->root, scale_op, count_mul_forward) &&
            set_backward(plan->root, scale_op, count_mul_backward),
        "find invariant callbacks");
  counted_forward_calls = counted_backward_calls = 0;
  Executor executor(outer(plan));
  const Evaluation result_value = evaluate(executor, .25, 0);
  close(result_value.value, 1.5, "active invariant value");
  close(result_value.gradient[0], 6, "active invariant gradient");
  check(counted_forward_calls == 1 && counted_backward_calls == 1,
        "active invariant runs forward and backward once");
  const Evaluation again = evaluate(executor, .25, 0);
  check(counted_forward_calls == 2 && counted_backward_calls == 2,
        "invariant cache resets for a new forward evaluation");
  check(std::memcmp(&result_value, &again, sizeof(Evaluation)) == 0,
        "repeated invariant evaluation is bitwise stable");
}

static void inplace_import_base_tests() {
  for (int trips : {2, 3}) {
    auto plan = std::make_shared<StructuredLoop>();
    const int base = plan->body.add_slot(3, false);
    const int theta = plan->body.add_slot(1, false);
    const int lower = scalar(*plan, 1);
    const int upper = scalar(*plan, trips);
    const int iterator = plan->body.add_slot(1, false);
    const int rhs = plan->body.add_slot(1, false);
    const int updated = plan->body.add_slot(3, false);
    const int result = plan->body.add_slot(1, false);
    plan->imports = {{base, 0, 0, true}, {theta, 1, 0, true}};
    Node update =
        call(*plan, OP_SET_INDEX_DYNAMIC, {base, iterator, rhs}, updated);
    const int update_op = update.op;
    attach(*plan, update_op, single_spec(3));
    plan->root =
        sequence({counted(lower, upper, iterator,
                          sequence({call(*plan, OP_MUL, {theta, iterator}, rhs),
                                    std::move(update), alias(base, updated)})),
                  call(*plan, OP_SUM_VEC, {base}, result)});
    plan->outputs = {result};
    plan->prepare();
    const Node* update_node = find_call(plan->root, update_op);
    check(update_node && update_node->storage == Node::InPlace,
          "indexed update followed by its alias is in place");
    Executor executor(outer(plan, {3, 1}));
    const double point[] = {10, 20, 30, .5};
    std::copy(std::begin(point), std::end(point), executor.params_data());
    double gradient[4] = {};
    const double value = executor.gradient(gradient);
    close(value, trips == 2 ? 31.5 : 3, "in-place import base value");
    check(
        std::equal(std::begin(point), std::end(point), executor.params_data()),
        "in-place update leaves the parent value untouched");
    const double expected[] = {0, 0, trips == 2 ? 1.0 : 0.0,
                               trips == 2 ? 3.0 : 6.0};
    for (size_t i = 0; i < std::size(expected); ++i)
      close(gradient[i], expected[i], "in-place import base gradient");
  }
}

static void inplace_promotion_tests() {
  auto plan = std::make_shared<StructuredLoop>();
  const int theta = plan->body.add_slot(1, false);
  const int one = scalar(*plan, 1);
  const int two = scalar(*plan, 2);
  const int inactive_rhs = scalar(*plan, 7);
  const int base = plan->body.add_slot(3, false);
  plan->fills.push_back({base, {10, 20, 30}});
  const int current = plan->body.add_slot(3, false);
  const int updated1 = plan->body.add_slot(3, false);
  const int updated2 = plan->body.add_slot(3, false);
  const int observed1 = plan->body.add_slot(1, false);
  const int observed2 = plan->body.add_slot(1, false);
  const int result = plan->body.add_slot(1, false);
  plan->imports = {{theta, 0, 0, true}};
  Node update1 =
      call(*plan, OP_SET_INDEX_DYNAMIC, {current, one, inactive_rhs}, updated1);
  Node update2 =
      call(*plan, OP_SET_INDEX_DYNAMIC, {current, two, theta}, updated2);
  const int update_ops[] = {update1.op, update2.op};
  attach(*plan, update1.op, single_spec(3));
  attach(*plan, update2.op, single_spec(3));
  plan->root = sequence(
      {alias(current, base), std::move(update1), alias(current, updated1),
       call(*plan, OP_SUM_VEC, {current}, observed1), std::move(update2),
       alias(current, updated2), call(*plan, OP_SUM_VEC, {current}, observed2),
       call(*plan, OP_SUM_VEC, {current}, result)});
  plan->outputs = {result};
  plan->prepare();
  for (int op : update_ops) {
    const Node* node = find_call(plan->root, op);
    check(node && node->storage == Node::InPlace,
          "promotion plan updates are in place");
  }
  const Node* first = find_call(plan->root, update_ops[0]);
  const Node* second = find_call(plan->root, update_ops[1]);
  check(first && first->active && second && second->active,
        "static activity covers every update of an active container");
  Executor executor(outer(plan));
  const Evaluation result_value = evaluate(executor, .25, 0);
  close(result_value.value, 37.25, "promoted in-place value");
  close(result_value.gradient[0], 1, "promoted in-place gradient");
}

static void inplace_duplicate_position_tests() {
  auto plan = std::make_shared<StructuredLoop>();
  const int theta = plan->body.add_slot(1, false);
  const int beta = plan->body.add_slot(1, false);
  const int base = plan->body.add_slot(4, false);
  plan->fills.push_back({base, {1, 2, 3, 4}});
  const int current = plan->body.add_slot(4, false);
  const int selectors = plan->body.add_slot(2, false);
  plan->fills.push_back({selectors, {2, 2}});
  const int rhs = plan->body.add_slot(2, false);
  const int updated = plan->body.add_slot(4, false);
  const int result = plan->body.add_slot(1, false);
  plan->imports = {{theta, 0, 0, true}, {beta, 1, 0, true}};
  auto spec = std::make_shared<DynamicIndexSpec>();
  spec->axes = {{DynamicIndexSpec::Axis::Multi, 4, 1, 2, 0}};
  spec->selected_size = 2;
  Node update =
      call(*plan, OP_SET_INDEX_DYNAMIC, {current, selectors, rhs}, updated);
  const int update_op = update.op;
  attach(*plan, update_op, spec);
  plan->root = sequence({alias(current, base),
                         call(*plan, OP_CONCAT2, {theta, beta}, rhs),
                         std::move(update), alias(current, updated),
                         call(*plan, OP_SUM_VEC, {current}, result)});
  plan->outputs = {result};
  plan->prepare();
  const Node* node = find_call(plan->root, update_op);
  check(node && node->storage == Node::InPlace,
        "duplicate-position update is in place");
  Executor executor(outer(plan));
  const Evaluation result_value = evaluate(executor, .25, .75);
  close(result_value.value, 8.75, "duplicate positions keep the last write");
  close(result_value.gradient[0], 0,
        "earlier duplicate write receives no adjoint");
  close(result_value.gradient[1], 1, "last duplicate write receives adjoint");
}

static void inplace_lifo_undo_tests() {
  auto plan = std::make_shared<StructuredLoop>();
  const int base = plan->body.add_slot(3, false);
  const int theta = plan->body.add_slot(1, false);
  const int lower = scalar(*plan, 1);
  const int upper = scalar(*plan, 3);
  const int iterator = plan->body.add_slot(1, false);
  const int zero = scalar(*plan, 0);
  const int squares = plan->body.add_slot(3, false);
  const int sum = plan->body.add_slot(1, false);
  const int total = plan->body.add_slot(1, false);
  const int next = plan->body.add_slot(1, false);
  const int rhs = plan->body.add_slot(1, false);
  const int updated = plan->body.add_slot(3, false);
  plan->imports = {{base, 0, 0, true}, {theta, 1, 0, true}};
  Node update =
      call(*plan, OP_SET_INDEX_DYNAMIC, {base, iterator, rhs}, updated);
  attach(*plan, update.op, single_spec(3));
  plan->root =
      sequence({alias(total, zero),
                counted(lower, upper, iterator,
                        sequence({call(*plan, OP_SQUARE, {base}, squares),
                                  call(*plan, OP_SUM_VEC, {squares}, sum),
                                  call(*plan, OP_ADD, {total, sum}, next),
                                  alias(total, next),
                                  call(*plan, OP_MUL, {theta, iterator}, rhs),
                                  std::move(update), alias(base, updated)}))});
  plan->outputs = {total};
  plan->prepare();
  Executor executor(outer(plan, {3, 1}));
  const double point[] = {1, 2, 3, .5};
  std::copy(std::begin(point), std::end(point), executor.params_data());
  double gradient[4] = {};
  close(executor.gradient(gradient), 37.5,
        "reads before in-place writes value");
  const double expected[] = {2, 8, 18, 6};
  for (size_t i = 0; i < std::size(expected); ++i)
    close(gradient[i], expected[i],
          "LIFO undo restores the values each backward saw");
}

static std::shared_ptr<StructuredLoop> recurrence(int trips) {
  auto plan = std::make_shared<StructuredLoop>();
  for (int i = 0; i < 7; ++i) plan->body.add_slot(1, false);
  plan->imports = {{0, 0, 0, true}, {1, 1, 0, true}};
  const int lower = scalar(*plan, 1);
  const int upper = scalar(*plan, trips);
  plan->root = sequence(
      {alias(2, 0),
       counted(lower, upper, 3,
               sequence({call(*plan, OP_MUL, {2, 1}, 4),
                         call(*plan, OP_ADD, {4, 0}, 5),
                         call(*plan, OP_TANHV, {5}, 6), alias(2, 6)}))});
  plan->outputs = {2};
  plan->prepare();
  return plan;
}

static void runtime_trip_tests() {
  for (int trips : {0, 1, 8}) {
    auto plan = recurrence(trips);
    check(plan->node_count < 16, "loop code size is independent of trips");
    Executor executor(outer(plan));
    for (const auto& point :
         std::vector<std::pair<double, double>>{{.1, .7}, {-.2, .3}}) {
      executor.params_data()[0] = point.first;
      executor.params_data()[1] = point.second;
      double gradient[2];
      const double value = executor.gradient(gradient);

      stan::math::nested_rev_autodiff nested;
      stan::math::var theta = point.first;
      stan::math::var beta = point.second;
      stan::math::var state = theta;
      for (int i = 0; i < trips; ++i)
        state = stan::math::tanh(state * beta + theta);
      state.grad();
      close(value, state.val(), "zero/one/many trip value");
      close(gradient[0], theta.adj(), "zero/one/many theta gradient");
      close(gradient[1], beta.adj(), "zero/one/many beta gradient");
    }
  }
}

static void import_reference_tests() {
  auto plan = std::make_shared<StructuredLoop>();
  const int left = plan->body.add_slot(1, false);
  const int right = plan->body.add_slot(1, false);
  const int repeated = plan->body.add_slot(1, false);
  const int product = plan->body.add_slot(1, false);
  const int result = plan->body.add_slot(1, false);
  plan->imports = {
      {repeated, 0, 1, true}, {left, 1, 2, true}, {right, 0, 1, true}};
  plan->root = sequence({call(*plan, OP_MUL, {left, right}, product),
                         call(*plan, OP_ADD, {product, repeated}, result)});
  plan->outputs = {result};
  plan->prepare();
  Executor executor(outer(plan, {3, 3}));
  const double point[] = {1, 2, 3, 4, 5, 6};
  std::copy(std::begin(point), std::end(point), executor.params_data());
  double gradient[6] = {};
  close(executor.gradient(gradient), 14, "import ordinals preserve value");
  const double expected[] = {0, 7, 0, 0, 0, 2};
  for (size_t i = 0; i < std::size(expected); ++i)
    close(gradient[i], expected[i], "import ordinals preserve outer gradient");
}

static void direct_index_kernel_tests() {
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

static std::string fixture_mir(const std::string& name) {
  std::ifstream input("tests/fixtures/" + name + ".tmir.sexp");
  check(bool(input), "generated MIR fixture is readable");
  std::ostringstream text;
  text << input.rdbuf();
  return text.str();
}

enum class Mode { Auto, Off, Prefer, Force };

static CompiledModel compile_fixture(const std::string& name, DataMap data,
                                     Mode mode) {
  if (mode == Mode::Auto)
    test_unsetenv("STANLI_STRUCTURED_LOOPS");
  else
    test_setenv("STANLI_STRUCTURED_LOOPS", mode == Mode::Off      ? "0"
                                           : mode == Mode::Prefer ? "1"
                                                                  : "force");
  return compile_model(fixture_mir(name), data);
}

static CompiledModel compile_fixture(const std::string& name, int trips,
                                     Mode mode) {
  DataMap data;
  data.set_int("N", trips);
  return compile_fixture(name, std::move(data), mode);
}

static DataMap observed_data(int trips) {
  DataMap data;
  data.set_int("N", trips);
  std::vector<double> y(static_cast<size_t>(trips));
  for (int n = 0; n < trips; ++n) y[static_cast<size_t>(n)] = 2 * std::sin(n);
  data.set_real_array("y", std::move(y));
  return data;
}

static const StructuredLoop* retained(const CompiledModel& model) {
  for (const auto& op : model.graph.ops)
    if (op.opcode == OP_LOOP)
      return static_cast<const StructuredLoop*>(op.udata);
  return nullptr;
}

static bool has_kind(const Node& node, Node::Kind kind) {
  if (node.kind == kind) return true;
  for (const auto& child : node.children)
    if (has_kind(child, kind)) return true;
  return false;
}

static void check_native_only(const StructuredLoop& plan, const char* name) {
  for (const auto& op : plan.body.ops) check(op.opcode != OP_ISLAND, name);
}

static void compare_gradients(const CompiledModel& native,
                              const CompiledModel& legacy,
                              const std::vector<std::vector<double>>& points,
                              const char* value_name,
                              const char* gradient_name) {
  check(native.n_unconstrained == legacy.n_unconstrained,
        "differential parameter count");
  Executor a(native.graph), b(legacy.graph);
  native.bind(a);
  legacy.bind(b);
  std::vector<double> ga(static_cast<size_t>(native.n_unconstrained));
  std::vector<double> gb(static_cast<size_t>(legacy.n_unconstrained));
  for (const auto& point : points) {
    check(point.size() == ga.size(), "differential point width");
    std::copy(point.begin(), point.end(), a.params_data());
    std::copy(point.begin(), point.end(), b.params_data());
    close(a.gradient(ga.data()), b.gradient(gb.data()), value_name);
    for (size_t i = 0; i < ga.size(); ++i) close(ga[i], gb[i], gradient_name);
  }
}

static bool same_graph_shape(const Graph& a, const Graph& b) {
  if (a.slots.size() != b.slots.size() || a.ops.size() != b.ops.size() ||
      a.result_slot != b.result_slot ||
      a.integer_storage_size() != b.integer_storage_size())
    return false;
  for (size_t i = 0; i < a.slots.size(); ++i)
    if (a.slots[i].offset != b.slots[i].offset ||
        a.slots[i].len != b.slots[i].len ||
        a.slots[i].is_param != b.slots[i].is_param)
      return false;
  for (size_t i = 0; i < a.ops.size(); ++i) {
    const auto& x = a.ops[i];
    const auto& y = b.ops[i];
    if (x.opcode != y.opcode || x.variant != y.variant || x.out != y.out ||
        x.out2 != y.out2 || x.n_in != y.n_in || x.n_idata != y.n_idata ||
        bool(x.udata) != bool(y.udata))
      return false;
    for (int input = 0; input < x.n_in; ++input)
      if (x.in[input] != y.in[input]) return false;
    for (int64_t data = 0; data < x.n_idata; ++data)
      if (x.idata[data] != y.idata[data]) return false;
  }
  return true;
}

static void forced_control_tests() {
  const auto nested = compile_fixture("structured_nested", 5, Mode::Force);
  const auto nested_legacy = compile_fixture("structured_nested", 5, Mode::Off);
  const StructuredLoop* nested_plan = retained(nested);
  check(nested_plan != nullptr, "forced nested control retains OP_LOOP");
  if (nested_plan) {
    check(has_kind(nested_plan->root, Node::For), "native nested for");
    check(has_kind(nested_plan->root, Node::While), "native nested while");
    check(has_kind(nested_plan->root, Node::If), "native nested if");
    check_native_only(*nested_plan, "nested body has no replay op");
  }
  compare_gradients(nested, nested_legacy, {{.1, .7}, {-.2, .3}, {0, .5}},
                    "nested target parity", "nested gradient parity");

  const auto exits = compile_fixture("structured_exits", 6, Mode::Force);
  const auto exits_legacy = compile_fixture("structured_exits", 6, Mode::Off);
  const StructuredLoop* exits_plan = retained(exits);
  check(exits_plan != nullptr, "forced exits retain OP_LOOP");
  if (exits_plan) {
    check(has_kind(exits_plan->root, Node::For), "native nested counted loop");
    check(has_kind(exits_plan->root, Node::Break), "native break");
    check(has_kind(exits_plan->root, Node::Continue), "native continue");
    check_native_only(*exits_plan, "exit body has no replay op");
  }
  compare_gradients(exits, exits_legacy, {{.1, .7}, {-.2, .3}},
                    "exit target parity", "exit gradient parity");

  const auto counted = compile_fixture("structured_counted", 4, Mode::Force);
  const auto counted_legacy =
      compile_fixture("structured_counted", 4, Mode::Off);
  check(retained(counted) != nullptr, "forced counted loop retains OP_LOOP");
  compare_gradients(counted, counted_legacy, {{.1, .7}, {-.2, .3}, {0, .5}},
                    "counted-iterator target parity",
                    "counted-iterator gradient parity");
}

static std::vector<double> iterator_forward_values;
static std::vector<double> iterator_reverse_values;
static std::vector<std::array<double, 6>> range_update_reverse_values;
static bool iterator_throw_once = false;
static int duplicate_site_backward_calls = 0;

static void trace_iterator_forward(KernelCtx& context) {
  iterator_forward_values.push_back(context.in[1].data[0]);
  if (iterator_throw_once && context.in[1].data[0] == 0) {
    iterator_throw_once = false;
    throw std::runtime_error("injected iterator callback failure");
  }
  find_kernel(OP_MUL)->forward(context);
}

static void trace_iterator_backward(KernelCtx& context) {
  iterator_reverse_values.push_back(context.in[1].data[0]);
  find_kernel(OP_MUL)->backward(context);
}

static void duplicate_site_backward(KernelCtx& context) {
  ++duplicate_site_backward_calls;
  find_kernel(OP_MUL)->backward(context);
}

static void trace_range_sum_backward(KernelCtx& context) {
  std::array<double, 6> values{};
  std::copy_n(context.in[0].data, values.size(), values.data());
  range_update_reverse_values.push_back(values);
  find_kernel(OP_SUM_VEC)->backward(context);
}

static std::shared_ptr<StructuredLoop> iterator_history_plan() {
  auto plan = std::make_shared<StructuredLoop>();
  const int theta = plan->body.add_slot(1, false);
  const int lower = scalar(*plan, -1);
  const int upper = scalar(*plan, 1);
  const int iterator = plan->body.add_slot(1, false);
  const int zero = scalar(*plan, 0);
  const int result = plan->body.add_slot(1, false);
  const int term = plan->body.add_slot(1, false);
  const int next = plan->body.add_slot(1, false);
  plan->imports = {{theta, 0, 0, true}};
  Node multiply = call(*plan, OP_MUL, {theta, iterator}, term);
  const int multiply_op = multiply.op;
  plan->root =
      sequence({alias(result, zero),
                counted(lower, upper, iterator,
                        sequence({std::move(multiply),
                                  call(*plan, OP_ADD, {result, term}, next),
                                  alias(result, next)}))});
  plan->outputs = {result};
  prepare_kernels(*plan);
  check(set_forward(plan->root, multiply_op, trace_iterator_forward) &&
            set_backward(plan->root, multiply_op, trace_iterator_backward),
        "find iterator trace callbacks");
  return plan;
}

static std::shared_ptr<StructuredLoop> iterator_escape_plan(
    int32_t lower_value, int32_t upper_value) {
  auto plan = std::make_shared<StructuredLoop>();
  const int lower = scalar(*plan, lower_value);
  const int upper = scalar(*plan, upper_value);
  const int iterator = plan->body.add_slot(1, false);
  const int initial = scalar(*plan, 7);
  const int result = plan->body.add_slot(1, false);
  plan->root =
      sequence({alias(result, initial),
                counted(lower, upper, iterator, alias(result, iterator))});
  plan->outputs = {result};
  plan->prepare();
  return plan;
}

static std::shared_ptr<StructuredLoop> iterator_target_plan() {
  auto plan = std::make_shared<StructuredLoop>();
  const int lower = scalar(*plan, 1);
  const int upper = scalar(*plan, 3);
  const int iterator = plan->body.add_slot(1, false);
  plan->root = counted(lower, upper, iterator, target(iterator));
  plan->has_target = true;
  plan->prepare();
  return plan;
}

static std::shared_ptr<StructuredLoop> nested_iterator_plan() {
  auto plan = std::make_shared<StructuredLoop>();
  const int theta = plan->body.add_slot(1, false);
  const int zero = scalar(*plan, 0);
  const int two = scalar(*plan, 2);
  const int outer_iterator = plan->body.add_slot(1, false);
  const int inner_iterator = plan->body.add_slot(1, false);
  const int sum = plan->body.add_slot(1, false);
  const int term = plan->body.add_slot(1, false);
  const int result = plan->body.add_slot(1, false);
  const int next = plan->body.add_slot(1, false);
  plan->imports = {{theta, 0, 0, true}};
  plan->root = sequence(
      {alias(result, zero),
       counted(zero, two, outer_iterator,
               counted(zero, outer_iterator, inner_iterator,
                       sequence({call(*plan, OP_ADD,
                                      {outer_iterator, inner_iterator}, sum),
                                 call(*plan, OP_MUL, {theta, sum}, term),
                                 call(*plan, OP_ADD, {result, term}, next),
                                 alias(result, next)})))});
  plan->outputs = {result};
  plan->prepare();
  return plan;
}

static std::shared_ptr<StructuredLoop> iterator_update_plan(
    bool direct = false, bool separate_extent = false) {
  auto plan = std::make_shared<StructuredLoop>();
  const int theta = plan->body.add_slot(1, false);
  const int lower = scalar(*plan, 1);
  const int upper = scalar(*plan, 3);
  const int iterator = plan->body.add_slot(1, false);
  const int extent = scalar(*plan, 3);
  const int base = plan->body.add_slot(3, false);
  plan->fills.push_back({base, {10, 20, 30}});
  const int current = plan->body.add_slot(3, false);
  const int rhs = plan->body.add_slot(1, false);
  const int updated = plan->body.add_slot(3, false);
  const int result = plan->body.add_slot(1, false);
  plan->imports = {{theta, 0, 0, true}};
  auto spec = single_spec(3);
  spec->input_count = direct ? (separate_extent ? 4 : 3) : 0;
  spec->rhs_input = direct ? spec->input_count - 1 : -1;
  if (separate_extent) {
    spec->axes[0].extent_input = 2;
    spec->axes[0].extent_input_offset = 0;
  }
  Node update = separate_extent
                    ? call(*plan, OP_SET_INDEX_DYNAMIC,
                           {current, iterator, extent, rhs}, updated)
                    : call(*plan, OP_SET_INDEX_DYNAMIC,
                           {current, iterator, rhs}, updated);
  attach(*plan, update.op, spec);
  plan->root =
      sequence({alias(current, base),
                counted(lower, upper, iterator,
                        sequence({call(*plan, OP_MUL, {theta, iterator}, rhs),
                                  std::move(update), alias(current, updated)})),
                call(*plan, OP_SUM_VEC, {current}, result)});
  plan->outputs = {result};
  plan->prepare();
  return plan;
}

static std::shared_ptr<StructuredLoop> range_update_plan(
    bool direct = false, bool separate_extent = false) {
  auto plan = std::make_shared<StructuredLoop>();
  const int base = plan->body.add_slot(6, false);
  const int theta = plan->body.add_slot(1, false);
  const int beta = plan->body.add_slot(1, false);
  const int lower = scalar(*plan, 1);
  const int upper = scalar(*plan, 3);
  const int iterator = plan->body.add_slot(1, false);
  const int selector = scalar(*plan, 1);
  const int extent = scalar(*plan, 6);
  const int weights = plan->body.add_slot(6, false);
  plan->fills.push_back({weights, {1, 2, 4, 8, 16, 32}});
  const int current = plan->body.add_slot(6, false);
  const int theta_term = plan->body.add_slot(1, false);
  const int beta_term = plan->body.add_slot(1, false);
  const int rhs = plan->body.add_slot(2, false);
  const int updated = plan->body.add_slot(6, false);
  const int zero_selector = plan->body.add_slot(2, false);
  plan->fills.push_back({zero_selector, {1, 0}});
  const int zero_updated = plan->body.add_slot(6, false);
  const int observed = plan->body.add_slot(1, false);
  const int result = plan->body.add_slot(1, false);
  plan->imports = {{base, 0, 0, true}, {theta, 1, 0, true}, {beta, 2, 0, true}};

  auto spec = std::make_shared<DynamicIndexSpec>();
  spec->axes = {{DynamicIndexSpec::Axis::Range, 6, 1, 2, 0}};
  spec->selected_size = 2;
  spec->input_count = direct ? (separate_extent ? 4 : 3) : 0;
  spec->rhs_input = direct ? spec->input_count - 1 : -1;
  if (separate_extent) {
    spec->axes[0].extent_input = 2;
    spec->axes[0].extent_input_offset = 0;
  }
  Node update = separate_extent
                    ? call(*plan, OP_SET_INDEX_DYNAMIC,
                           {current, selector, extent, rhs}, updated)
                    : call(*plan, OP_SET_INDEX_DYNAMIC,
                           {current, selector, rhs}, updated);
  attach(*plan, update.op, spec);
  auto zero_spec = std::make_shared<DynamicIndexSpec>();
  zero_spec->axes = {{DynamicIndexSpec::Axis::Range, 6, 1, 2, 0}};
  zero_spec->axes[0].count_input_offset = 1;
  zero_spec->selected_size = 2;
  Node zero_update = call(*plan, OP_SET_INDEX_DYNAMIC,
                          {current, zero_selector, rhs}, zero_updated);
  attach(*plan, zero_update.op, zero_spec);
  Node observe = call(*plan, OP_SUM_VEC, {current}, observed);
  const int observe_op = observe.op;
  plan->root = sequence(
      {alias(current, base),
       counted(lower, upper, iterator,
               sequence({call(*plan, OP_MUL, {theta, iterator}, theta_term),
                         call(*plan, OP_MUL, {beta, iterator}, beta_term),
                         call(*plan, OP_CONCAT2, {theta_term, beta_term}, rhs),
                         std::move(update), alias(current, updated),
                         std::move(observe)})),
       std::move(zero_update), alias(current, zero_updated),
       call(*plan, OP_DOT, {current, weights}, result)});
  plan->outputs = {result};
  plan->prepare();
  check(set_backward(plan->root, observe_op, trace_range_sum_backward),
        "find ordered range observer");
  return plan;
}

static std::shared_ptr<StructuredLoop> inactive_range_update_plan() {
  auto plan = std::make_shared<StructuredLoop>();
  const int theta = plan->body.add_slot(1, false);
  const int lower = scalar(*plan, 1);
  const int upper = scalar(*plan, 3);
  const int iterator = plan->body.add_slot(1, false);
  const int base = plan->body.add_slot(16, false);
  plan->fills.push_back(
      {base, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}});
  const int current = plan->body.add_slot(16, false);
  const int selector = scalar(*plan, 1);
  const int rhs = plan->body.add_slot(2, false);
  plan->fills.push_back({rhs, {10, 20}});
  const int updated = plan->body.add_slot(16, false);
  const int total = plan->body.add_slot(1, false);
  const int result = plan->body.add_slot(1, false);
  plan->imports = {{theta, 0, 0, true}};
  auto spec = std::make_shared<DynamicIndexSpec>();
  spec->axes = {{DynamicIndexSpec::Axis::Range, 16, 1, 2, 0}};
  spec->selected_size = 2;
  Node update =
      call(*plan, OP_SET_INDEX_DYNAMIC, {current, selector, rhs}, updated);
  attach(*plan, update.op, spec);
  plan->root =
      sequence({alias(current, base),
                counted(lower, upper, iterator,
                        sequence({std::move(update), alias(current, updated)})),
                call(*plan, OP_SUM_VEC, {current}, total),
                call(*plan, OP_MUL, {theta, total}, result)});
  plan->outputs = {result};
  plan->prepare();
  return plan;
}

static std::shared_ptr<StructuredLoop> aliased_update_plan() {
  auto plan = std::make_shared<StructuredLoop>();
  const int theta = plan->body.add_slot(1, false);
  const int one = scalar(*plan, 1);
  const int two = scalar(*plan, 2);
  const int three = scalar(*plan, 3);
  const int base = plan->body.add_slot(3, false);
  plan->fills.push_back({base, {10, 20, 30}});
  const int current = plan->body.add_slot(3, false);
  const int snapshot1 = plan->body.add_slot(3, false);
  const int snapshot2 = plan->body.add_slot(3, false);
  const int rhs1 = plan->body.add_slot(1, false);
  const int rhs2 = plan->body.add_slot(1, false);
  const int rhs3 = plan->body.add_slot(1, false);
  const int updated1 = plan->body.add_slot(3, false);
  const int updated2 = plan->body.add_slot(3, false);
  const int updated3 = plan->body.add_slot(3, false);
  const int observed1 = plan->body.add_slot(1, false);
  const int observed2 = plan->body.add_slot(1, false);
  const int observed3 = plan->body.add_slot(1, false);
  const int snapshot_sum1 = plan->body.add_slot(1, false);
  const int snapshot_sum2 = plan->body.add_slot(1, false);
  const int current_sum = plan->body.add_slot(1, false);
  const int snapshot_total = plan->body.add_slot(1, false);
  const int result = plan->body.add_slot(1, false);
  plan->imports = {{theta, 0, 0, true}};
  Node update1 =
      call(*plan, OP_SET_INDEX_DYNAMIC, {current, one, rhs1}, updated1);
  Node update2 =
      call(*plan, OP_SET_INDEX_DYNAMIC, {current, two, rhs2}, updated2);
  Node update3 =
      call(*plan, OP_SET_INDEX_DYNAMIC, {current, three, rhs3}, updated3);
  for (int op : {update1.op, update2.op, update3.op})
    attach(*plan, op, single_spec(3));
  plan->root = sequence(
      {alias(current, base),
       call(*plan, OP_MUL, {theta, one}, rhs1),
       std::move(update1),
       alias(current, updated1),
       call(*plan, OP_SUM_VEC, {current}, observed1),
       alias(snapshot1, current),
       alias(snapshot2, current),
       call(*plan, OP_MUL, {theta, two}, rhs2),
       std::move(update2),
       alias(current, updated2),
       call(*plan, OP_SUM_VEC, {current}, observed2),
       call(*plan, OP_MUL, {theta, three}, rhs3),
       std::move(update3),
       alias(current, updated3),
       call(*plan, OP_SUM_VEC, {current}, observed3),
       call(*plan, OP_SUM_VEC, {snapshot1}, snapshot_sum1),
       call(*plan, OP_SUM_VEC, {snapshot2}, snapshot_sum2),
       call(*plan, OP_SUM_VEC, {current}, current_sum),
       call(*plan, OP_ADD, {snapshot_sum1, snapshot_sum2}, snapshot_total),
       call(*plan, OP_ADD, {snapshot_total, current_sum}, result)});
  plan->outputs = {result};
  plan->prepare();
  return plan;
}

static std::shared_ptr<StructuredLoop> loop_aliased_update_plan() {
  auto plan = std::make_shared<StructuredLoop>();
  const int theta = plan->body.add_slot(1, false);
  const int lower = scalar(*plan, 1);
  const int upper = scalar(*plan, 3);
  const int iterator = plan->body.add_slot(1, false);
  const int base = plan->body.add_slot(3, false);
  plan->fills.push_back({base, {10, 20, 30}});
  const int current = plan->body.add_slot(3, false);
  const int snapshot = plan->body.add_slot(3, false);
  const int rhs = plan->body.add_slot(1, false);
  const int updated = plan->body.add_slot(3, false);
  const int observed = plan->body.add_slot(1, false);
  const int first = plan->body.add_slot(1, false);
  const int snapshot_sum = plan->body.add_slot(1, false);
  const int current_sum = plan->body.add_slot(1, false);
  const int result = plan->body.add_slot(1, false);
  plan->imports = {{theta, 0, 0, true}};
  Node update =
      call(*plan, OP_SET_INDEX_DYNAMIC, {current, iterator, rhs}, updated);
  attach(*plan, update.op, single_spec(3));
  Node is_first = call(*plan, OP_COMPARE, {iterator, lower}, first);
  plan->body.ops[static_cast<size_t>(is_first.op)].variant = 4;
  plan->root = sequence(
      {alias(current, base),
       counted(
           lower, upper, iterator,
           sequence({call(*plan, OP_MUL, {theta, iterator}, rhs),
                     std::move(update), alias(current, updated),
                     call(*plan, OP_SUM_VEC, {current}, observed),
                     std::move(is_first),
                     branch(first, alias(snapshot, current), sequence({}))})),
       call(*plan, OP_SUM_VEC, {snapshot}, snapshot_sum),
       call(*plan, OP_SUM_VEC, {current}, current_sum),
       call(*plan, OP_ADD, {snapshot_sum, current_sum}, result)});
  plan->outputs = {result};
  plan->prepare();
  return plan;
}

static std::shared_ptr<StructuredLoop> duplicate_node_plan() {
  auto plan = std::make_shared<StructuredLoop>();
  const int theta = plan->body.add_slot(1, false);
  const int beta = plan->body.add_slot(1, false);
  const int product = plan->body.add_slot(1, false);
  plan->imports = {{theta, 0, 0, true}, {beta, 1, 0, true}};
  Node first = call(*plan, OP_MUL, {theta, beta}, product);
  Node second = first;
  plan->root = sequence({std::move(first), std::move(second)});
  plan->outputs = {product};
  bool rejected = false;
  try {
    plan->prepare();
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "duplicate output slots are rejected");
  const int second_product = plan->body.add_slot(1, false);
  Node replacement = call(*plan, OP_MUL, {theta, beta}, second_product);
  plan->root = sequence(
      {call(*plan, OP_MUL, {theta, beta}, product), std::move(replacement)});
  plan->outputs = {second_product};
  prepare_kernels(*plan);
  plan->root.children[1].backward = duplicate_site_backward;
  return plan;
}

static void iterator_history_tests() {
  iterator_forward_values.clear();
  iterator_reverse_values.clear();
  Executor history(outer(iterator_history_plan()));
  const Evaluation traced = evaluate(history, .25, 0);
  close(traced.value, 0, "iterator history value");
  close(traced.gradient[0], 0, "iterator history theta gradient");
  check(iterator_forward_values == std::vector<double>({-1, 0, 1}),
        "iterator forward values are exact");
  check(iterator_reverse_values == std::vector<double>({1, 0, -1}),
        "iterator reverse values preserve history");

  Executor retry(outer(iterator_history_plan()));
  iterator_throw_once = true;
  bool threw = false;
  try {
    (void)evaluate(retry, .25, 0);
  } catch (const std::runtime_error& error) {
    threw = std::string(error.what()) == "injected iterator callback failure";
  }
  check(threw, "forward preserves a callback exception");
  iterator_forward_values.clear();
  iterator_reverse_values.clear();
  const Evaluation retried = evaluate(retry, .25, 0);
  close(retried.value, traced.value, "retry after failure value");
  close(retried.gradient[0], traced.gradient[0],
        "retry after failure theta gradient");
  check(iterator_forward_values == std::vector<double>({-1, 0, 1}) &&
            iterator_reverse_values == std::vector<double>({1, 0, -1}),
        "retry rebuilds historical values");

  for (const auto& bounds : std::vector<std::pair<int32_t, int32_t>>{
           {std::numeric_limits<int32_t>::min(),
            std::numeric_limits<int32_t>::min() + 1},
           {std::numeric_limits<int32_t>::max() - 1,
            std::numeric_limits<int32_t>::max()},
           {1, 0}}) {
    Executor escaped(outer(iterator_escape_plan(bounds.first, bounds.second)));
    const Evaluation result = evaluate(escaped, 0, 0);
    const double expected = bounds.second >= bounds.first ? bounds.second : 7;
    close(result.value, expected, "iterator boundary escape value");
    close(result.gradient[0], 0, "iterator boundary escape theta gradient");
  }

  Executor targeted(outer(iterator_target_plan()));
  const Evaluation target_result = evaluate(targeted, 0, 0);
  close(target_result.value, 6, "iterator target keeps reached values");
  close(target_result.gradient[0], 0, "iterator target theta gradient");

  Executor nested(outer(nested_iterator_plan()));
  const Evaluation nested_result = evaluate(nested, .25, 0);
  close(nested_result.value, 3, "nested iterator value");
  close(nested_result.gradient[0], 12, "nested iterator theta gradient");
  close(nested_result.gradient[1], 0, "nested iterator beta gradient");

  Executor update(outer(iterator_update_plan()));
  const Evaluation updated = evaluate(update, .25, 0);
  close(updated.value, 1.5, "iterator update value");
  close(updated.gradient[0], 6, "iterator update theta gradient");
  close(updated.gradient[1], 0, "iterator update beta gradient");
  Executor direct_update(outer(iterator_update_plan(true)));
  Executor extent_update(outer(iterator_update_plan(true, true)));
  const Evaluation direct = evaluate(direct_update, .25, 0);
  const Evaluation extent = evaluate(extent_update, .25, 0);
  check(std::memcmp(&updated, &direct, sizeof(Evaluation)) == 0 &&
            std::memcmp(&updated, &extent, sizeof(Evaluation)) == 0,
        "direct-input updates match the packed update bitwise");
  const Evaluation repeated = evaluate(update, .25, 0);
  check(std::memcmp(&updated, &repeated, sizeof(Evaluation)) == 0,
        "in-place update resets between evaluations");

  struct RangeEvaluation {
    double value = 0;
    double gradient[8] = {};
    std::vector<std::array<double, 6>> reverse_values;
  };
  const auto run_range = [](bool direct, bool separate_extent) {
    range_update_reverse_values.clear();
    Executor executor(
        outer(range_update_plan(direct, separate_extent), {6, 1, 1}));
    const double point[] = {10, 20, 30, 40, 50, 60, .25, -.5};
    std::copy(std::begin(point), std::end(point), executor.params_data());
    RangeEvaluation result;
    result.value = executor.gradient(result.gradient);
    result.reverse_values = range_update_reverse_values;
    return result;
  };
  const RangeEvaluation packed = run_range(false, false);
  const RangeEvaluation direct_range = run_range(true, true);
  check(std::memcmp(&packed.value, &direct_range.value, sizeof(double)) == 0 &&
            std::memcmp(packed.gradient, direct_range.gradient,
                        sizeof(packed.gradient)) == 0,
        "multi-descriptor range update matches packed bitwise");
  close(packed.value, 3157.75, "ordered range update result");
  const double expected_gradient[] = {0, 0, 4, 8, 16, 32, 3, 6};
  for (size_t i = 0; i < std::size(expected_gradient); ++i)
    close(packed.gradient[i], expected_gradient[i],
          "ordered range update gradient");
  const std::vector<std::array<double, 6>> expected_reverse = {
      {{.75, -1.5, 30, 40, 50, 60}},
      {{.5, -1, 30, 40, 50, 60}},
      {{.25, -.5, 30, 40, 50, 60}},
  };
  check(packed.reverse_values == expected_reverse &&
            direct_range.reverse_values == expected_reverse,
        "range update restores historical primals in LIFO order");

  Executor inactive(outer(inactive_range_update_plan()));
  const Evaluation inactive_result = evaluate(inactive, .25, 0);
  close(inactive_result.value, 40.75, "inactive range update result");
  close(inactive_result.gradient[0], 163, "inactive range update gradient");

  Executor aliased(outer(aliased_update_plan()));
  const Evaluation aliased_result = evaluate(aliased, .25, 0);
  close(aliased_result.value, 102, "outgoing aliases preserve snapshot values");
  close(aliased_result.gradient[0], 8,
        "outgoing aliases preserve snapshot gradients");

  Executor loop_aliased(outer(loop_aliased_update_plan()));
  const Evaluation loop_aliased_result = evaluate(loop_aliased, .25, 0);
  close(loop_aliased_result.value, 51.75,
        "loop-backedge alias preserves snapshot value");
  close(loop_aliased_result.gradient[0], 7,
        "loop-backedge alias preserves snapshot gradient");

  duplicate_site_backward_calls = 0;
  Executor duplicate(outer(duplicate_node_plan()));
  const Evaluation duplicate_result = evaluate(duplicate, 2, 3);
  close(duplicate_result.value, 6, "duplicate operation value");
  close(duplicate_result.gradient[0], 3, "duplicate operation theta gradient");
  close(duplicate_result.gradient[1], 2, "duplicate operation beta gradient");
  check(duplicate_site_backward_calls == 1,
        "reverse resolves the exact duplicate operation node");
}

static std::shared_ptr<StructuredLoop> scalar_index_loop_plan(bool direct) {
  auto plan = std::make_shared<StructuredLoop>();
  const int base = plan->body.add_slot(3, false);
  const int theta = plan->body.add_slot(1, false);
  const int one = scalar(*plan, 1);
  const int three = scalar(*plan, 3);
  const int iterator = plan->body.add_slot(1, false);
  const int selected = plan->body.add_slot(1, false);
  const int term = plan->body.add_slot(1, false);
  const int zero = scalar(*plan, 0);
  const int total = plan->body.add_slot(1, false);
  const int next = plan->body.add_slot(1, false);
  plan->imports = {{base, 0, 1, true}, {theta, 1, 0, true}};
  auto spec = single_spec(3);
  spec->input_count = direct ? 2 : 0;
  Node index = call(*plan, OP_INDEX_DYNAMIC, {base, iterator}, selected);
  attach(*plan, index.op, spec);
  plan->root =
      sequence({alias(total, zero),
                counted(one, three, iterator,
                        sequence({std::move(index),
                                  call(*plan, OP_MUL, {selected, theta}, term),
                                  call(*plan, OP_ADD, {total, term}, next),
                                  alias(total, next)}))});
  plan->outputs = {total};
  plan->prepare();
  return plan;
}

static void routed_add_backward(KernelCtx& context) {
  find_kernel(OP_ADD)->backward(context);
}

// Select the same element on every trip. The alias that refreshes the
// selector prevents loop-invariant hoisting, and the ADD consumer's backward
// routes adjoints without rereading the selected primal.
static std::shared_ptr<StructuredLoop> repeated_index_retention_plan(
    bool alias_selected = false, bool replace_consumer_after_prepare = false) {
  auto plan = std::make_shared<StructuredLoop>();
  const int base = plan->body.add_slot(1, false);
  const int theta = plan->body.add_slot(1, false);
  const int one = scalar(*plan, 1);
  const int eight = scalar(*plan, 8);
  const int iterator = plan->body.add_slot(1, false);
  const int selector = plan->body.add_slot(1, false);
  const int selected = plan->body.add_slot(1, false);
  const int held = plan->body.add_slot(1, false);
  const int term = plan->body.add_slot(1, false);
  plan->imports = {{base, 0, 0, true}, {theta, 1, 0, true}};
  Node index = call(*plan, OP_INDEX_DYNAMIC, {base, selector}, selected);
  const int index_op = index.op;
  attach(*plan, index.op, single_spec(1));
  Node consume =
      call(*plan, OP_ADD, {alias_selected ? held : selected, theta}, term);
  const int consume_op = consume.op;
  std::vector<Node> body{alias(selector, one), std::move(index)};
  if (alias_selected) body.push_back(alias(held, selected));
  body.push_back(std::move(consume));
  body.push_back(target(term));
  plan->root = counted(one, eight, iterator, sequence(std::move(body)));
  plan->has_target = true;
  prepare_kernels(*plan);
  Node* indexed = find_call(plan->root, index_op);
  check(indexed && indexed->reuse_primal_output == !alias_selected,
        alias_selected ? "alias keeps indexed primal retained"
                       : "repeated index reuses one primal workspace");
  if (replace_consumer_after_prepare)
    check(set_backward(plan->root, consume_op, routed_add_backward),
          "replace retention consumer backward");
  return plan;
}

static void primal_retention_tests() {
  test_setenv("STANLI_STRUCTURED_LOOP_DIAGNOSTICS", "1");
  Executor executor(outer(repeated_index_retention_plan(), {1, 1}));
  test_unsetenv("STANLI_STRUCTURED_LOOP_DIAGNOSTICS");
  executor.params_data()[0] = 2;
  executor.params_data()[1] = .5;
  double gradient[2] = {};
  stanli_test::StdoutCapture captured(stderr);
  close(executor.gradient(gradient), 20,
        "repeated index retained-primal result");
  executor.params_data()[0] = 3;
  close(executor.gradient(gradient), 28,
        "reused primals refresh on next evaluation");
  const std::string diagnostics = captured.finish();
  check(diagnostics.find("reused_primal_cells=8") != std::string::npos,
        "repeated index saves eight arena cells");
  close(gradient[0], 8, "repeated index scatter accumulation");
  close(gradient[1], 8, "value-free consumer adjoint accumulation");

  Executor aliased(outer(repeated_index_retention_plan(true), {1, 1}));
  aliased.params_data()[0] = 2;
  aliased.params_data()[1] = .5;
  std::fill(std::begin(gradient), std::end(gradient), 0.0);
  close(aliased.gradient(gradient), 20, "aliased indexed primal result");
  close(gradient[0], 8, "aliased indexed primal gradient");

  // The plan was proved with the registered ADD backward, then mutated to a
  // private callback. Runtime identity validation must disable workspace
  // reuse while preserving the answer.
  test_setenv("STANLI_STRUCTURED_LOOP_DIAGNOSTICS", "1");
  Executor replaced(outer(repeated_index_retention_plan(false, true), {1, 1}));
  test_unsetenv("STANLI_STRUCTURED_LOOP_DIAGNOSTICS");
  replaced.params_data()[0] = 2;
  replaced.params_data()[1] = .5;
  std::fill(std::begin(gradient), std::end(gradient), 0.0);
  stanli_test::StdoutCapture fallback_captured(stderr);
  close(replaced.gradient(gradient), 20,
        "replaced consumer falls back to retained primals");
  replaced.params_data()[0] = 3;
  close(replaced.gradient(gradient), 28,
        "fallback refreshes on next evaluation");
  const std::string fallback_diagnostics = fallback_captured.finish();
  check(fallback_diagnostics.find("reused_primal_cells=0") != std::string::npos,
        "replaced consumer saves no arena cells");
  close(gradient[0], 8, "replaced consumer fallback gradient");
}

static std::shared_ptr<StructuredLoop> zero_scalar_index_plan(bool invalid) {
  auto plan = std::make_shared<StructuredLoop>();
  const int base = plan->body.add_slot(4, false);
  const int selectors = plan->body.add_slot(3, false);
  plan->fills.push_back({selectors, {1, 0, invalid ? 3.0 : 2.0}});
  const int result = plan->body.add_slot(1, false);
  plan->imports = {{base, 0, 0, true}};
  auto spec = std::make_shared<DynamicIndexSpec>();
  spec->axes = {
      {DynamicIndexSpec::Axis::Range, 2, 2, 1, 0},
      {DynamicIndexSpec::Axis::Single, 2, 1, 1, 2},
  };
  spec->axes[0].count_input_offset = 1;
  spec->selected_size = 1;
  Node index = call(*plan, OP_INDEX_DYNAMIC, {base, selectors}, result);
  attach(*plan, index.op, spec);
  plan->root = std::move(index);
  plan->outputs = {result};
  plan->prepare();
  return plan;
}

static std::shared_ptr<StructuredLoop> selector_only_scalar_index_plan() {
  auto plan = std::make_shared<StructuredLoop>();
  const int selector = plan->body.add_slot(1, false);
  const int base = plan->body.add_slot(3, false);
  plan->fills.push_back({base, {10, 20, 30}});
  const int result = plan->body.add_slot(1, false);
  plan->imports = {{selector, 0, 0, true}};
  Node index = call(*plan, OP_INDEX_DYNAMIC, {base, selector}, result);
  attach(*plan, index.op, single_spec(3));
  plan->root = std::move(index);
  plan->outputs = {result};
  plan->prepare();
  return plan;
}

static std::shared_ptr<StructuredLoop> scalar_index_after_updates_plan() {
  auto plan = std::make_shared<StructuredLoop>();
  const int base = plan->body.add_slot(3, false);
  const int theta = plan->body.add_slot(1, false);
  const int one = scalar(*plan, 1);
  const int two = scalar(*plan, 2);
  const int iterator = plan->body.add_slot(1, false);
  const int current = plan->body.add_slot(3, false);
  const int rhs = plan->body.add_slot(1, false);
  const int updated = plan->body.add_slot(3, false);
  const int selected = plan->body.add_slot(1, false);
  plan->imports = {{base, 0, 0, true}, {theta, 1, 0, true}};
  Node update =
      call(*plan, OP_SET_INDEX_DYNAMIC, {current, iterator, rhs}, updated);
  attach(*plan, update.op, single_spec(3));
  Node index = call(*plan, OP_INDEX_DYNAMIC, {current, two}, selected);
  attach(*plan, index.op, single_spec(3));
  plan->root =
      sequence({alias(current, base),
                counted(one, two, iterator,
                        sequence({call(*plan, OP_MUL, {theta, iterator}, rhs),
                                  std::move(update), alias(current, updated)})),
                std::move(index)});
  plan->outputs = {selected};
  plan->prepare();
  return plan;
}

static std::shared_ptr<StructuredLoop> scalar_index_target_plan() {
  auto plan = std::make_shared<StructuredLoop>();
  const int base = plan->body.add_slot(3, false);
  const int one = scalar(*plan, 1);
  const int selected = plan->body.add_slot(1, false);
  plan->imports = {{base, 0, 0, true}};
  Node index = call(*plan, OP_INDEX_DYNAMIC, {base, one}, selected);
  attach(*plan, index.op, single_spec(3));
  plan->root = sequence({std::move(index), target(selected)});
  plan->has_target = true;
  plan->prepare();
  return plan;
}

static std::shared_ptr<StructuredLoop> scalar_index_update_rhs_plan() {
  auto plan = std::make_shared<StructuredLoop>();
  const int base = plan->body.add_slot(3, false);
  const int one = scalar(*plan, 1);
  const int two = scalar(*plan, 2);
  const int iterator = plan->body.add_slot(1, false);
  const int current = plan->body.add_slot(3, false);
  const int updated = plan->body.add_slot(3, false);
  const int selected = plan->body.add_slot(1, false);
  const int result = plan->body.add_slot(1, false);
  plan->imports = {{base, 0, 0, true}};
  Node update =
      call(*plan, OP_SET_INDEX_DYNAMIC, {current, iterator, selected}, updated);
  attach(*plan, update.op, single_spec(3));
  Node index = call(*plan, OP_INDEX_DYNAMIC, {base, one}, selected);
  attach(*plan, index.op, single_spec(3));
  plan->root =
      sequence({std::move(index), alias(current, base),
                counted(one, two, iterator,
                        sequence({std::move(update), alias(current, updated)})),
                call(*plan, OP_SUM_VEC, {current}, result)});
  plan->outputs = {result};
  plan->prepare();
  return plan;
}

static void scalar_index_tests() {
  const double point[] = {7, 1e16, -1e16, 1, 9, .25};
  const double expected_gradient[] = {0, .25, .25, .25, 0, 0};
  Evaluation previous;
  for (bool direct : {false, true}) {
    Executor executor(outer(scalar_index_loop_plan(direct), {5, 1}));
    std::copy(std::begin(point), std::end(point), executor.params_data());
    double gradient[6] = {};
    const double value = executor.gradient(gradient);
    close(value, .25, "scalar index loop value");
    for (size_t i = 0; i < std::size(expected_gradient); ++i)
      close(gradient[i], expected_gradient[i], "scalar index loop gradient");
    Evaluation current;
    current.value = value;
    current.gradient[0] = gradient[1];
    current.gradient[1] = gradient[2];
    if (direct)
      check(std::memcmp(&previous, &current, sizeof(Evaluation)) == 0,
            "direct scalar index matches packed bitwise");
    previous = current;
  }

  Executor zero(outer(zero_scalar_index_plan(false), {4}));
  const double zero_point[] = {1, 2, 3, 4};
  std::copy(std::begin(zero_point), std::end(zero_point), zero.params_data());
  double zero_gradient[4] = {};
  close(zero.gradient(zero_gradient), 0,
        "scalar index supports an empty reached selection");
  check(std::all_of(std::begin(zero_gradient), std::end(zero_gradient),
                    [](double value) { return value == 0; }),
        "empty scalar index does not scatter an adjoint");

  Executor invalid(outer(zero_scalar_index_plan(true), {4}));
  std::copy(std::begin(zero_point), std::end(zero_point),
            invalid.params_data());
  try {
    (void)invalid.gradient(zero_gradient);
    check(false, "empty scalar index validates every other selector");
  } catch (const std::out_of_range&) {
  }

  Executor selector_only(outer(selector_only_scalar_index_plan()));
  const Evaluation selector_result = evaluate(selector_only, 2, 0);
  close(selector_result.value, 20, "selector-only scalar index value");
  close(selector_result.gradient[0], 0,
        "scalar index does not differentiate its selector");

  Executor updated(outer(scalar_index_after_updates_plan(), {3, 1}));
  const double updated_point[] = {10, 20, 30, .25};
  std::copy(std::begin(updated_point), std::end(updated_point),
            updated.params_data());
  double updated_gradient[4] = {};
  close(updated.gradient(updated_gradient), .5,
        "scalar index reads an in-place updated value");
  const double expected_updated_gradient[] = {0, 0, 0, 2};
  for (size_t i = 0; i < std::size(expected_updated_gradient); ++i)
    close(updated_gradient[i], expected_updated_gradient[i],
          "scalar index routes through the update adjoint");

  Executor targeted(outer(scalar_index_target_plan(), {3}));
  const double target_point[] = {2, 3, 4};
  std::copy(std::begin(target_point), std::end(target_point),
            targeted.params_data());
  double target_gradient[3] = {};
  close(targeted.gradient(target_gradient), 2,
        "indexed value contributes a target leaf");
  close(target_gradient[0], 1, "indexed target leaf scatters its adjoint");
  close(target_gradient[1], 0, "indexed target leaves other inputs unchanged");
  close(target_gradient[2], 0, "indexed target leaves final input unchanged");

  Executor update_rhs(outer(scalar_index_update_rhs_plan(), {3}));
  std::copy(std::begin(target_point), std::end(target_point),
            update_rhs.params_data());
  double update_rhs_gradient[3] = {};
  close(update_rhs.gradient(update_rhs_gradient), 8,
        "indexed right-hand side update value");
  const double expected_rhs_gradient[] = {2, 0, 1};
  for (size_t i = 0; i < std::size(expected_rhs_gradient); ++i)
    close(update_rhs_gradient[i], expected_rhs_gradient[i],
          "indexed right-hand side update gradient");
}

static std::shared_ptr<StructuredLoop> integer_history_plan() {
  auto plan = std::make_shared<StructuredLoop>();
  const int theta = plan->body.add_slot(1, false);
  const int lower = scalar(*plan, -1);
  const int upper = scalar(*plan, 1);
  const int iterator = plan->body.add_slot(1, false);
  const int two = scalar(*plan, 2);
  const int integer_result = plan->body.add_slot(1, false);
  const int zero = scalar(*plan, 0);
  const int result = plan->body.add_slot(1, false);
  const int term = plan->body.add_slot(1, false);
  const int next = plan->body.add_slot(1, false);
  plan->imports = {{theta, 0, 0, true}};
  Node arithmetic = call(*plan, OP_INT_ARITH, {iterator, two}, integer_result);
  plan->body.ops[static_cast<size_t>(arithmetic.op)].variant = 2;
  Node multiply = call(*plan, OP_MUL, {theta, integer_result}, term);
  const int multiply_op = multiply.op;
  plan->root =
      sequence({alias(result, zero),
                counted(lower, upper, iterator,
                        sequence({std::move(arithmetic), std::move(multiply),
                                  call(*plan, OP_ADD, {result, term}, next),
                                  alias(result, next)}))});
  plan->outputs = {result};
  prepare_kernels(*plan);
  check(set_forward(plan->root, multiply_op, trace_iterator_forward) &&
            set_backward(plan->root, multiply_op, trace_iterator_backward),
        "find integer-result trace callbacks");
  return plan;
}

static std::shared_ptr<StructuredLoop> comparison_target_plan() {
  auto plan = std::make_shared<StructuredLoop>();
  const int lower = scalar(*plan, -1);
  const int upper = scalar(*plan, 1);
  const int iterator = plan->body.add_slot(1, false);
  const int zero = scalar(*plan, 0);
  const int compared = plan->body.add_slot(1, false);
  Node comparison = call(*plan, OP_COMPARE, {iterator, zero}, compared);
  plan->body.ops[static_cast<size_t>(comparison.op)].variant = 0;
  plan->root = counted(lower, upper, iterator,
                       sequence({std::move(comparison), target(compared)}));
  plan->has_target = true;
  plan->prepare();
  return plan;
}

static std::shared_ptr<StructuredLoop> computed_nested_bound_plan() {
  auto plan = std::make_shared<StructuredLoop>();
  const int one = scalar(*plan, 1);
  const int two = scalar(*plan, 2);
  const int outer_iterator = plan->body.add_slot(1, false);
  const int inner_iterator = plan->body.add_slot(1, false);
  const int inner_upper = plan->body.add_slot(1, false);
  Node arithmetic =
      call(*plan, OP_INT_ARITH, {outer_iterator, one}, inner_upper);
  plan->body.ops[static_cast<size_t>(arithmetic.op)].variant = 0;
  plan->root = counted(
      one, two, outer_iterator,
      sequence({std::move(arithmetic), counted(one, inner_upper, inner_iterator,
                                               target(inner_iterator))}));
  plan->has_target = true;
  plan->prepare();
  return plan;
}

static std::shared_ptr<StructuredLoop> integer_output_plan(double a, double b) {
  auto plan = std::make_shared<StructuredLoop>();
  const int left = scalar(*plan, a);
  const int right = scalar(*plan, b);
  const int integer_result = plan->body.add_slot(1, false);
  const int result = plan->body.add_slot(1, false);
  Node arithmetic = call(*plan, OP_INT_ARITH, {left, right}, integer_result);
  plan->body.ops[static_cast<size_t>(arithmetic.op)].variant = 0;
  plan->root = sequence({std::move(arithmetic), alias(result, integer_result)});
  plan->outputs = {result};
  plan->prepare();
  return plan;
}

static std::shared_ptr<StructuredLoop> imported_integer_output_plan(
    int variant = 0) {
  auto plan = std::make_shared<StructuredLoop>();
  const int left = plan->body.add_slot(1, false);
  const int right = plan->body.add_slot(1, false);
  const int result = plan->body.add_slot(1, false);
  plan->imports = {{left, 0, 0, true}, {right, 1, 0, true}};
  Node arithmetic = call(*plan, OP_INT_ARITH, {left, right}, result);
  plan->body.ops[static_cast<size_t>(arithmetic.op)].variant = variant;
  plan->root = std::move(arithmetic);
  plan->outputs = {result};
  plan->prepare();
  return plan;
}

static int custom_integer_calls = 0;
static void custom_integer_forward(KernelCtx& context) {
  ++custom_integer_calls;
  find_kernel(OP_INT_ARITH)->forward(context);
  context.out.data[0] += 0.5;
}

static void registered_custom_integer_forward(KernelCtx& context) {
  ++custom_integer_calls;
  context.out.data[0] = 5.5;
}

struct ScopedKernelOverride {
  uint16_t opcode;
  Kernel saved;

  ScopedKernelOverride(uint16_t opcode, Kernel replacement)
      : opcode(opcode), saved(*find_kernel(opcode)) {
    register_kernel(opcode, replacement);
  }
  ~ScopedKernelOverride() { register_kernel(opcode, saved); }
};

static int inactive_workspace_calls = 0;
static double* inactive_workspace_address = nullptr;
static bool inactive_workspace_stable = true;
static bool inactive_workspace_disjoint = true;
static bool inactive_workspace_throw_once = false;
static bool workspace_producer_throw_once = false;

static int64_t inactive_workspace_size(const Op&, const Slot*) { return 3; }

static void inactive_workspace_forward(KernelCtx& context) {
  ++inactive_workspace_calls;
  if (!inactive_workspace_address) {
    inactive_workspace_address = context.scratch;
  } else {
    inactive_workspace_stable &= inactive_workspace_address == context.scratch;
  }
  inactive_workspace_disjoint &=
      context.scratch && context.scratch != context.in[0].data &&
      context.scratch != context.in[1].data &&
      context.scratch != context.out.data &&
      (!context.out2.data || context.scratch != context.out2.data) &&
      (context.out.len == 0 || !context.out2.data || context.out2.len == 0 ||
       context.out.data != context.out2.data);
  context.scratch[0] = context.in[0].data[0];
  context.scratch[1] = context.in[1].data[0];
  context.scratch[2] = context.scratch[0] + context.scratch[1];
  if (inactive_workspace_throw_once && context.scratch[0] == 2) {
    inactive_workspace_throw_once = false;
    throw std::runtime_error("injected inactive workspace failure");
  }
  if (context.out.len > 0) context.out.data[0] = context.scratch[2];
  if (context.out2.data)
    context.out2.data[0] = context.scratch[0] * context.scratch[1];
}

static std::shared_ptr<StructuredLoop> inactive_workspace_plan() {
  auto plan = std::make_shared<StructuredLoop>();
  const int lower = scalar(*plan, 1);
  const int upper = scalar(*plan, 3);
  const int iterator = plan->body.add_slot(1, false);
  const int two = scalar(*plan, 2);
  const int first = plan->body.add_slot(1, false);
  const int second = plan->body.add_slot(1, false);
  const int combined = plan->body.add_slot(1, false);
  const int zero = scalar(*plan, 0);
  const int result = plan->body.add_slot(1, false);
  Node workspace = call(*plan, OP_INT_ARITH, {iterator, two}, first);
  plan->body.ops[static_cast<size_t>(workspace.op)].out2 = second;
  const int workspace_op = workspace.op;
  plan->root = sequence(
      {alias(result, zero),
       counted(lower, upper, iterator,
               sequence({std::move(workspace),
                         call(*plan, OP_ADD, {first, second}, combined),
                         alias(result, combined)}))});
  plan->outputs = {result};
  plan->prepare();
  const Node* node = find_call(plan->root, workspace_op);
  check(node && node->storage == Node::Transient,
        "inactive two-output kernel read by inactive work is transient");
  return plan;
}

static std::shared_ptr<StructuredLoop> inactive_zero_output_plan() {
  auto plan = std::make_shared<StructuredLoop>();
  const int lower = scalar(*plan, 1);
  const int upper = scalar(*plan, 3);
  const int iterator = plan->body.add_slot(1, false);
  const int two = scalar(*plan, 2);
  const int empty = plan->body.add_slot(0, false);
  const int second_empty = plan->body.add_slot(0, false);
  const int second = plan->body.add_slot(1, false);
  const int zero = scalar(*plan, 0);
  const int result = plan->body.add_slot(1, false);
  Node empty_call = call(*plan, OP_INT_ARITH, {iterator, two}, empty);
  Node second_call = call(*plan, OP_INT_ARITH, {iterator, two}, second_empty);
  plan->body.ops[static_cast<size_t>(second_call.op)].out2 = second;
  plan->root =
      sequence({alias(result, zero),
                counted(lower, upper, iterator,
                        sequence({std::move(empty_call), std::move(second_call),
                                  alias(result, second)}))});
  plan->outputs = {result};
  plan->prepare();
  return plan;
}

static std::shared_ptr<StructuredLoop> inactive_target_plan() {
  auto plan = std::make_shared<StructuredLoop>();
  const int two = scalar(*plan, 2);
  const int three = scalar(*plan, 3);
  const int result = plan->body.add_slot(1, false);
  plan->root =
      sequence({call(*plan, OP_ADD, {two, three}, result), target(result)});
  plan->has_target = true;
  plan->prepare();
  return plan;
}

static std::shared_ptr<StructuredLoop> computed_bound_plan() {
  auto plan = std::make_shared<StructuredLoop>();
  const int one = scalar(*plan, 1);
  const int two = scalar(*plan, 2);
  const int upper = plan->body.add_slot(1, false);
  const int iterator = plan->body.add_slot(1, false);
  const int result = plan->body.add_slot(1, false);
  Node bound = call(*plan, OP_ADD, {one, two}, upper);
  const int bound_op = bound.op;
  plan->root = sequence({std::move(bound), counted(one, upper, iterator,
                                                   alias(result, iterator))});
  plan->outputs = {result};
  plan->prepare();
  const Node* node = find_call(plan->root, bound_op);
  check(node && node->storage == Node::Transient,
        "loop bound read only by control is transient");
  return plan;
}

static std::shared_ptr<StructuredLoop> repeated_output_plan(
    int upper_value = 4) {
  auto plan = std::make_shared<StructuredLoop>();
  const int lower = scalar(*plan, 1);
  const int upper = scalar(*plan, upper_value);
  const int iterator = plan->body.add_slot(1, false);
  const int two = scalar(*plan, 2);
  const int result = plan->body.add_slot(1, false);
  plan->fills.push_back({result, {99}});
  plan->root = counted(lower, upper, iterator,
                       call(*plan, OP_ADD, {iterator, two}, result));
  plan->outputs = {result};
  plan->prepare();
  return plan;
}

static void throwing_producer(KernelCtx& context) {
  if (workspace_producer_throw_once && context.in[0].data[0] == 2) {
    workspace_producer_throw_once = false;
    throw std::runtime_error("injected producer failure");
  }
  context.out.data[0] = context.in[0].data[0] + context.in[1].data[0];
}

static std::shared_ptr<StructuredLoop> inactive_input_reverse_plan() {
  auto plan = std::make_shared<StructuredLoop>();
  const int theta = plan->body.add_slot(1, false);
  const int two = scalar(*plan, 2);
  const int three = scalar(*plan, 3);
  const int inactive = plan->body.add_slot(1, false);
  const int result = plan->body.add_slot(1, false);
  plan->imports = {{theta, 0, 0, true}};
  Node sum = call(*plan, OP_ADD, {two, three}, inactive);
  const int sum_op = sum.op;
  plan->root = sequence(
      {std::move(sum), call(*plan, OP_MUL, {theta, inactive}, result)});
  plan->outputs = {result};
  plan->prepare();
  const Node* node = find_call(plan->root, sum_op);
  check(node && node->storage == Node::Retained && !node->active,
        "inactive value read by active work is retained");
  return plan;
}

static std::shared_ptr<StructuredLoop> inactive_update_plan() {
  auto plan = std::make_shared<StructuredLoop>();
  const int lower = scalar(*plan, 1);
  const int upper = scalar(*plan, 3);
  const int iterator = plan->body.add_slot(1, false);
  const int base = plan->body.add_slot(3, false);
  plan->fills.push_back({base, {10, 20, 30}});
  const int current = plan->body.add_slot(3, false);
  const int rhs = scalar(*plan, 7);
  const int updated = plan->body.add_slot(3, false);
  const int result = plan->body.add_slot(1, false);
  Node update =
      call(*plan, OP_SET_INDEX_DYNAMIC, {current, iterator, rhs}, updated);
  attach(*plan, update.op, single_spec(3));
  plan->root =
      sequence({alias(current, base),
                counted(lower, upper, iterator,
                        sequence({std::move(update), alias(current, updated)})),
                call(*plan, OP_SUM_VEC, {current}, result)});
  plan->outputs = {result};
  plan->prepare();
  return plan;
}

static int active_workspace_forward_calls = 0;
static int active_workspace_backward_calls = 0;
static double* active_workspace_address = nullptr;
static bool active_workspace_history_ok = true;

static void active_workspace_forward(KernelCtx& context) {
  ++active_workspace_forward_calls;
  active_workspace_address = context.scratch;
  context.scratch[0] = context.in[0].data[0];
  context.scratch[1] = context.in[1].data[0];
  context.scratch[2] = context.scratch[0] * context.scratch[1];
  context.out.data[0] = context.scratch[2];
  context.out2.data[0] = context.scratch[0] + context.scratch[1];
}

static void active_workspace_backward(KernelCtx& context) {
  ++active_workspace_backward_calls;
  active_workspace_history_ok &=
      context.scratch == active_workspace_address &&
      context.scratch[0] == context.in[0].data[0] &&
      context.scratch[1] == context.in[1].data[0] &&
      context.scratch[2] == context.in[0].data[0] * context.in[1].data[0];
  if (context.in_adj[0].data)
    context.in_adj[0].data[0] +=
        context.scratch[1] * context.out_adj + context.out2_adj;
  if (context.in_adj[1].data)
    context.in_adj[1].data[0] +=
        context.scratch[0] * context.out_adj + context.out2_adj;
}

static std::shared_ptr<StructuredLoop> active_workspace_plan() {
  auto plan = std::make_shared<StructuredLoop>();
  const int theta = plan->body.add_slot(1, false);
  const int two = scalar(*plan, 2);
  const int product = plan->body.add_slot(1, false);
  const int sum = plan->body.add_slot(1, false);
  const int result = plan->body.add_slot(1, false);
  plan->imports = {{theta, 0, 0, true}};
  Node workspace = call(*plan, OP_INT_ARITH, {theta, two}, product);
  plan->body.ops[static_cast<size_t>(workspace.op)].out2 = sum;
  plan->root = sequence(
      {std::move(workspace), call(*plan, OP_ADD, {product, sum}, result)});
  plan->outputs = {result};
  plan->prepare();
  return plan;
}

static void integer_result_tests() {
  iterator_forward_values.clear();
  iterator_reverse_values.clear();
  Executor history(outer(integer_history_plan()));
  const Evaluation traced = evaluate(history, .25, 0);
  close(traced.value, 0, "integer history value");
  close(traced.gradient[0], 0, "integer history theta gradient");
  check(iterator_forward_values == std::vector<double>({-2, 0, 2}),
        "integer forward values are exact");
  check(iterator_reverse_values == std::vector<double>({2, 0, -2}),
        "integer reverse values preserve history");

  Executor comparison_target(outer(comparison_target_plan()));
  const Evaluation compared = evaluate(comparison_target, 0, 0);
  close(compared.value, 1, "comparison target keeps reached values");
  close(compared.gradient[0], 0, "comparison target theta gradient");

  Executor nested_bound(outer(computed_nested_bound_plan()));
  const Evaluation nested = evaluate(nested_bound, 0, 0);
  close(nested.value, 9, "integer result supplies nested loop bound");

  for (const auto& point : std::vector<std::pair<double, double>>{
           {static_cast<double>(std::numeric_limits<int32_t>::min()), 0},
           {static_cast<double>(std::numeric_limits<int32_t>::max()) - 1, 1}}) {
    Executor boundary(outer(integer_output_plan(point.first, point.second)));
    const Evaluation result = evaluate(boundary, 0, 0);
    close(result.value, point.first + point.second,
          "integer boundary escapes through output alias");
  }

  Executor retry(outer(imported_integer_output_plan()));
  bool threw = false;
  try {
    (void)evaluate(retry,
                   static_cast<double>(std::numeric_limits<int32_t>::max()), 1);
  } catch (const std::domain_error& error) {
    threw = std::string(error.what()) ==
            "integer arithmetic exceeds Stan integer range";
  }
  check(threw, "integer arithmetic exception is preserved");
  const Evaluation retried = evaluate(retry, 2, 3);
  close(retried.value, 5, "integer retry value");
  close(retried.gradient[0], 0, "integer retry theta gradient");

  Executor division_retry(outer(imported_integer_output_plan(3)));
  threw = false;
  try {
    (void)evaluate(division_retry, 4, 0);
  } catch (const std::domain_error& error) {
    threw = std::string(error.what()) == "integer division by zero";
  }
  check(threw, "integer division exception is preserved");
  close(evaluate(division_retry, 4, 2).value, 2,
        "integer division retry value");

  custom_integer_calls = 0;
  {
    auto plan = integer_output_plan(2, 3);
    check(set_forward(plan->root, 0, custom_integer_forward),
          "find custom integer callback");
    Executor custom(outer(plan));
    close(evaluate(custom, 0, 0).value, 5.5,
          "node forward override is honoured");
  }
  check(custom_integer_calls == 1, "node forward override runs once");

  custom_integer_calls = 0;
  {
    Kernel replacement = *find_kernel(OP_INT_ARITH);
    replacement.forward = registered_custom_integer_forward;
    ScopedKernelOverride overridden(OP_INT_ARITH, replacement);
    Executor registered_custom(outer(integer_output_plan(2, 3)));
    close(evaluate(registered_custom, 0, 0).value, 5.5,
          "registered kernel override is honoured");
  }
  check(custom_integer_calls == 1, "registered kernel override runs once");

  inactive_workspace_calls = 0;
  inactive_workspace_address = nullptr;
  inactive_workspace_stable = true;
  inactive_workspace_disjoint = true;
  inactive_workspace_throw_once = true;
  {
    Kernel replacement = *find_kernel(OP_INT_ARITH);
    replacement.forward = inactive_workspace_forward;
    replacement.scratch_size = inactive_workspace_size;
    ScopedKernelOverride overridden(OP_INT_ARITH, replacement);
    Executor workspace(outer(inactive_workspace_plan()));
    bool workspace_threw = false;
    try {
      (void)evaluate(workspace, 0, 0);
    } catch (const std::runtime_error& error) {
      workspace_threw =
          std::string(error.what()) == "injected inactive workspace failure";
    }
    check(workspace_threw, "transient kernel exception is preserved");
    const Evaluation workspace_result = evaluate(workspace, 0, 0);
    close(workspace_result.value, 11,
          "transient kernel preserves second output and retry");
  }
  check(inactive_workspace_calls == 5,
        "transient kernel executes reached calls only");
  check(inactive_workspace_stable,
        "transient scratch is reused across calls and retry");
  check(inactive_workspace_disjoint,
        "transient workspace descriptors remain disjoint");

  inactive_workspace_calls = 0;
  inactive_workspace_address = nullptr;
  inactive_workspace_stable = true;
  inactive_workspace_disjoint = true;
  inactive_workspace_throw_once = false;
  {
    Kernel replacement = *find_kernel(OP_INT_ARITH);
    replacement.forward = inactive_workspace_forward;
    replacement.scratch_size = inactive_workspace_size;
    ScopedKernelOverride overridden(OP_INT_ARITH, replacement);
    Executor zero_output(outer(inactive_zero_output_plan()));
    close(evaluate(zero_output, 0, 0).value, 6,
          "zero-length primary output keeps the second output");
  }
  check(inactive_workspace_calls == 6, "zero-length outputs execute");

  Executor inactive_target(outer(inactive_target_plan()));
  close(evaluate(inactive_target, 0, 0).value, 5,
        "inactive value reaches the target reduction");

  Executor computed_bound(outer(computed_bound_plan()));
  close(evaluate(computed_bound, 0, 0).value, 3,
        "computed value supplies a counted-loop bound");

  Executor repeated_output(outer(repeated_output_plan()));
  close(evaluate(repeated_output, 0, 0).value, 6,
        "forward-only loop output keeps its final value");
  Executor zero_trip(outer(repeated_output_plan(0)));
  close(evaluate(zero_trip, 0, 0).value, 99,
        "zero-trip loop preserves its initial output");

  {
    Kernel replacement = *find_kernel(OP_ADD);
    replacement.forward = throwing_producer;
    ScopedKernelOverride overridden(OP_ADD, replacement);
    Executor retry_producer(outer(repeated_output_plan(3)));
    workspace_producer_throw_once = true;
    bool producer_threw = false;
    try {
      (void)evaluate(retry_producer, 0, 0);
    } catch (const std::runtime_error& error) {
      producer_threw = std::string(error.what()) == "injected producer failure";
    }
    check(producer_threw, "producer preserves its forward exception");
    close(evaluate(retry_producer, 0, 0).value, 5,
          "producer retry rebuilds its output");
  }

  Executor inactive_reverse(outer(inactive_input_reverse_plan()));
  const Evaluation reversed = evaluate(inactive_reverse, 4, 0);
  close(reversed.value, 20, "active operation reads an inactive primal");
  close(reversed.gradient[0], 5, "reverse reads the inactive primal");
  close(reversed.gradient[1], 0, "inactive input has no adjoint");

  Executor inactive_update(outer(inactive_update_plan()));
  close(evaluate(inactive_update, 0, 0).value, 21,
        "inactive in-place update value");

  active_workspace_forward_calls = 0;
  active_workspace_backward_calls = 0;
  active_workspace_address = nullptr;
  active_workspace_history_ok = true;
  {
    Kernel replacement = *find_kernel(OP_INT_ARITH);
    replacement.forward = active_workspace_forward;
    replacement.backward = active_workspace_backward;
    replacement.scratch_size = inactive_workspace_size;
    ScopedKernelOverride overridden(OP_INT_ARITH, replacement);
    Executor active_workspace(outer(active_workspace_plan()));
    const Evaluation active = evaluate(active_workspace, 3, 0);
    close(active.value, 11, "active scratchful callback value");
    close(active.gradient[0], 3, "active scratchful callback gradient");
  }
  check(active_workspace_forward_calls == 1 &&
            active_workspace_backward_calls == 1,
        "active scratchful callback retains one reverse record");
  check(active_workspace_history_ok,
        "active scratchful callback retains forward scratch for reverse");
}

static std::atomic<int> invariant_first_calls{0};
static std::atomic<int> invariant_second_calls{0};
static std::atomic<int> invariant_active_calls{0};
static std::atomic<int> invariant_variant_calls{0};
static std::atomic<int> dependent_compare_calls{0};

static void count_invariant_first(KernelCtx& context) {
  ++invariant_first_calls;
  find_kernel(OP_ADD)->forward(context);
}
static void count_invariant_second(KernelCtx& context) {
  ++invariant_second_calls;
  find_kernel(OP_MUL)->forward(context);
}
static void count_invariant_second_add(KernelCtx& context) {
  ++invariant_second_calls;
  find_kernel(OP_ADD)->forward(context);
}
static void count_invariant_active(KernelCtx& context) {
  ++invariant_active_calls;
  find_kernel(OP_ADD)->forward(context);
}
static void count_invariant_variant(KernelCtx& context) {
  ++invariant_variant_calls;
  find_kernel(OP_ADD)->forward(context);
}
static void count_dependent_compare(KernelCtx& context) {
  ++dependent_compare_calls;
  find_kernel(OP_COMPARE)->forward(context);
}

static std::shared_ptr<StructuredLoop> invariant_plan(int trips) {
  auto plan = std::make_shared<StructuredLoop>();
  const int theta = plan->body.add_slot(1, false);
  const int lower = scalar(*plan, 1);
  const int upper = scalar(*plan, trips);
  const int iterator = plan->body.add_slot(1, false);
  const int two = scalar(*plan, 2);
  const int three = scalar(*plan, 3);
  const int invariant_first = plan->body.add_slot(1, false);
  const int invariant_second = plan->body.add_slot(1, false);
  const int active = plan->body.add_slot(1, false);
  const int variant = plan->body.add_slot(1, false);
  const int result = plan->body.add_slot(1, false);
  plan->imports = {{theta, 0, 0, true}};
  Node first = call(*plan, OP_ADD, {two, three}, invariant_first);
  const int first_op = first.op;
  Node second = call(*plan, OP_MUL, {invariant_first, two}, invariant_second);
  const int second_op = second.op;
  Node active_call = call(*plan, OP_ADD, {result, invariant_second}, active);
  const int active_op = active_call.op;
  Node variant_call = call(*plan, OP_ADD, {iterator, two}, variant);
  const int variant_op = variant_call.op;
  plan->root =
      sequence({alias(result, theta),
                counted(lower, upper, iterator,
                        sequence({std::move(first), std::move(second),
                                  std::move(active_call), alias(result, active),
                                  std::move(variant_call)}))});
  plan->outputs = {result};
  prepare_kernels(*plan);
  check(find_call(plan->root, first_op)->invariant_loop == 0 &&
            find_call(plan->root, second_op)->invariant_loop == -1 &&
            find_call(plan->root, active_op)->invariant_loop == -1 &&
            find_call(plan->root, variant_op)->invariant_loop == -1,
        "invariance follows the written set of the enclosing loop");
  check(set_forward(plan->root, first_op, count_invariant_first) &&
            set_forward(plan->root, second_op, count_invariant_second) &&
            set_forward(plan->root, active_op, count_invariant_active) &&
            set_forward(plan->root, variant_op, count_invariant_variant),
        "find invariant plan callbacks");
  return plan;
}

static std::shared_ptr<StructuredLoop> dependent_guard_plan() {
  auto plan = std::make_shared<StructuredLoop>();
  const int theta = plan->body.add_slot(1, false);
  const int one = scalar(*plan, 1);
  const int three = scalar(*plan, 3);
  const int threshold = scalar(*plan, .3);
  const int zero = scalar(*plan, 0);
  const int iterator = plan->body.add_slot(1, false);
  const int base = plan->body.add_slot(1, false);
  plan->fills.push_back({base, {0}});
  const int current = plan->body.add_slot(1, false);
  const int rhs = plan->body.add_slot(1, false);
  const int updated = plan->body.add_slot(1, false);
  const int compared = plan->body.add_slot(1, false);
  const int term = plan->body.add_slot(1, false);
  const int result = plan->body.add_slot(1, false);
  const int next = plan->body.add_slot(1, false);
  plan->imports = {{theta, 0, 0, true}};
  Node update = call(*plan, OP_SET_INDEX_DYNAMIC, {current, one, rhs}, updated);
  attach(*plan, update.op, single_spec(1));
  Node comparison = call(*plan, OP_COMPARE, {current, threshold}, compared);
  const int comparison_op = comparison.op;
  plan->body.ops[static_cast<size_t>(comparison_op)].variant = 2;
  plan->root =
      sequence({alias(current, base), alias(result, zero),
                counted(one, three, iterator,
                        sequence({call(*plan, OP_MUL, {theta, iterator}, rhs),
                                  std::move(update), alias(current, updated),
                                  std::move(comparison),
                                  call(*plan, OP_MUL, {theta, compared}, term),
                                  call(*plan, OP_ADD, {result, term}, next),
                                  alias(result, next)}))});
  plan->outputs = {result};
  prepare_kernels(*plan);
  check(find_call(plan->root, comparison_op)->invariant_loop == -1,
        "a read of an in-place updated container is not invariant");
  check(set_forward(plan->root, comparison_op, count_dependent_compare),
        "find dependent comparison callback");
  return plan;
}

static Evaluation evaluate_invariant(Executor& executor, double theta) {
  return evaluate(executor, theta, 0);
}

static void loop_invariant_reuse_tests() {
  auto plan = invariant_plan(3);
  Executor enabled(outer(plan));
  invariant_first_calls = invariant_second_calls = 0;
  invariant_active_calls = invariant_variant_calls = 0;
  const Evaluation first = evaluate_invariant(enabled, .25);
  close(first.value, 30.25, "invariant plan value");
  close(first.gradient[0], 1, "invariant plan gradient");
  check(invariant_first_calls == 1,
        "inactive invariant executes once per loop entry");
  check(invariant_second_calls == 3,
        "a kernel reading a loop-written slot recomputes every iteration");
  check(invariant_active_calls == 3, "active loop work runs every iteration");
  check(invariant_variant_calls == 3, "iterator-dependent work is not reused");
  const Evaluation second = evaluate_invariant(enabled, .25);
  check(invariant_first_calls == 1 && invariant_second_calls == 3 &&
            invariant_active_calls == 6 && invariant_variant_calls == 3,
        "data-only work replays from the memo tape on a new evaluation");
  check(std::memcmp(&first, &second, sizeof(Evaluation)) == 0,
        "repeated invariant evaluation is bitwise stable");

  dependent_compare_calls = 0;
  Executor dependent(outer(dependent_guard_plan()));
  const Evaluation dependent_result = evaluate_invariant(dependent, .25);
  check(dependent_compare_calls == 3,
        "work reading an in-place container recomputes every iteration");
  close(dependent_result.value, .5, "dependent guard value");
  close(dependent_result.gradient[0], 2, "dependent guard gradient");

  auto zero = invariant_plan(0);
  Executor zero_executor(outer(zero));
  invariant_first_calls = 0;
  const Evaluation zero_result = evaluate_invariant(zero_executor, .25);
  check(invariant_first_calls == 0,
        "zero-trip loop does not pre-execute invariants");
  close(zero_result.value, .25, "zero-trip invariant value");

  auto late = std::make_shared<StructuredLoop>();
  const int late_theta = late->body.add_slot(1, false);
  const int late_lower = scalar(*late, 1);
  const int late_upper = scalar(*late, 3);
  const int late_iterator = late->body.add_slot(1, false);
  const int late_one = scalar(*late, 1);
  const int late_condition = late->body.add_slot(1, false);
  const int late_source = late->body.add_slot(1, false);
  const int late_result = late->body.add_slot(1, false);
  late->imports = {{late_theta, 0, 0, true}};
  Node comparison =
      call(*late, OP_COMPARE, {late_iterator, late_one}, late_condition);
  late->body.ops[static_cast<size_t>(comparison.op)].variant = 2;
  Node late_definition = call(*late, OP_ADD, {late_one, late_one}, late_source);
  const int late_definition_op = late_definition.op;
  Node late_use = call(*late, OP_ADD, {late_source, late_one}, late_result);
  const int late_use_op = late_use.op;
  late->root = counted(
      late_lower, late_upper, late_iterator,
      sequence(
          {std::move(comparison),
           branch(late_condition, std::move(late_definition), sequence({})),
           std::move(late_use)}));
  late->outputs = {late_result};
  late->prepare();
  check(set_forward(late->root, late_definition_op, count_invariant_first) &&
            set_forward(late->root, late_use_op, count_invariant_second_add),
        "find late invariant callbacks");
  invariant_first_calls = invariant_second_calls = 0;
  Executor late_executor(outer(late));
  close(evaluate_invariant(late_executor, 0).value, 3,
        "late conditional definition value");
  check(invariant_first_calls == 1 && invariant_second_calls == 3,
        "late definition is cached and its loop-written consumer is not");

  auto while_plan = std::make_shared<StructuredLoop>();
  const int while_theta = while_plan->body.add_slot(1, false);
  const int while_counter = scalar(*while_plan, 0);
  const int while_one = scalar(*while_plan, 1);
  const int while_three = scalar(*while_plan, 3);
  const int while_condition = while_plan->body.add_slot(1, false);
  const int while_next = while_plan->body.add_slot(1, false);
  const int while_invariant = while_plan->body.add_slot(1, false);
  const int while_result = while_plan->body.add_slot(1, false);
  while_plan->imports = {{while_theta, 0, 0, true}};
  Node while_compare = call(*while_plan, OP_COMPARE,
                            {while_three, while_counter}, while_condition);
  while_plan->body.ops[static_cast<size_t>(while_compare.op)].variant = 2;
  Node while_constant =
      call(*while_plan, OP_ADD, {while_one, while_one}, while_invariant);
  const int while_constant_op = while_constant.op;
  while_plan->root = while_loop(
      while_condition, std::move(while_compare),
      sequence(
          {std::move(while_constant),
           call(*while_plan, OP_ADD, {while_counter, while_one}, while_next),
           alias(while_counter, while_next),
           alias(while_result, while_invariant)}));
  while_plan->outputs = {while_result};
  while_plan->prepare();
  check(set_forward(while_plan->root, while_constant_op, count_invariant_first),
        "find while invariant callback");
  invariant_first_calls = 0;
  Executor while_executor(outer(while_plan));
  close(evaluate_invariant(while_executor, 0).value, 2,
        "while invariant result");
  check(invariant_first_calls == 1,
        "while body reuses its inactive invariant result");
}

static std::vector<double*> control_first_outputs;
static std::vector<double*> control_second_outputs;
static std::atomic<int> control_backward_calls{0};
static std::atomic<bool> control_throw_once{false};

static void record_control_add(KernelCtx& context) {
  control_first_outputs.push_back(context.out.data);
  find_kernel(OP_ADD)->forward(context);
}
static void record_control_compare(KernelCtx& context) {
  control_second_outputs.push_back(context.out.data);
  find_kernel(OP_COMPARE)->forward(context);
}
static void record_control_add_backward(KernelCtx& context) {
  ++control_backward_calls;
  find_kernel(OP_ADD)->backward(context);
}
static void throw_then_record_control_add(KernelCtx& context) {
  if (control_throw_once.exchange(false))
    throw std::runtime_error("injected control failure");
  record_control_add(context);
}

static bool one_address(const std::vector<double*>& addresses) {
  return !addresses.empty() &&
         std::all_of(addresses.begin(), addresses.end(), [&](double* address) {
           return address == addresses.front();
         });
}

static bool distinct_addresses(const std::vector<double*>& addresses) {
  for (size_t i = 0; i < addresses.size(); ++i)
    for (size_t j = 0; j < i; ++j)
      if (addresses[i] == addresses[j]) return false;
  return true;
}

static std::shared_ptr<StructuredLoop> control_cone_plan(bool active_control) {
  auto plan = std::make_shared<StructuredLoop>();
  const int theta = plan->body.add_slot(1, false);
  const int lower = scalar(*plan, 1);
  const int upper = scalar(*plan, 3);
  const int iterator = plan->body.add_slot(1, false);
  const int zero = scalar(*plan, 0);
  const int limit = scalar(*plan, 3);
  const int first = plan->body.add_slot(1, false);
  const int active_middle = plan->body.add_slot(1, false);
  const int condition = plan->body.add_slot(1, false);
  const int result = plan->body.add_slot(1, false);
  const int updated = plan->body.add_slot(1, false);
  plan->imports = {{theta, 0, 0, true}};
  Node first_call = call(*plan, OP_ADD, {iterator, zero}, first);
  const int first_op = first_call.op;
  std::vector<Node> control;
  control.push_back(std::move(first_call));
  int compare_input = first;
  int active_middle_op = -1;
  if (active_control) {
    Node middle = call(*plan, OP_ADD, {first, theta}, active_middle);
    active_middle_op = middle.op;
    compare_input = active_middle;
    control.push_back(std::move(middle));
  }
  Node compare = call(*plan, OP_COMPARE, {compare_input, limit}, condition);
  plan->body.ops[static_cast<size_t>(compare.op)].variant = 0;
  const int compare_op = compare.op;
  control.push_back(std::move(compare));
  Node update = call(*plan, OP_ADD, {result, theta}, updated);
  control.push_back(
      branch(condition, sequence({std::move(update), alias(result, updated)}),
             sequence({})));
  plan->root =
      sequence({alias(result, zero),
                counted(lower, upper, iterator, sequence(std::move(control)))});
  plan->outputs = {result};
  prepare_kernels(*plan);
  check(find_call(plan->root, first_op)->storage ==
            (active_control ? Node::Retained : Node::Transient),
        "control-cone head storage follows its readers' activity");
  check(find_call(plan->root, compare_op)->storage == Node::Transient,
        "control-cone comparison is transient");
  check(set_forward(plan->root, first_op, record_control_add) &&
            set_forward(plan->root, compare_op, record_control_compare),
        "find control-cone callbacks");
  if (active_control)
    check(
        set_backward(plan->root, active_middle_op, record_control_add_backward),
        "find active control-cone backward callback");
  return plan;
}

static void control_tests() {
  auto plan = control_cone_plan(false);
  Executor inactive(outer(plan));
  control_first_outputs.clear();
  control_second_outputs.clear();
  const Evaluation direct = evaluate_invariant(inactive, .25);
  close(direct.value, .5, "inactive-control branch result");
  close(direct.gradient[0], 2, "inactive-control branch gradient");
  check(control_first_outputs.size() == 3 &&
            control_second_outputs.size() == 3 &&
            one_address(control_first_outputs) &&
            one_address(control_second_outputs),
        "transient control cone reuses one workspace cell per kernel");

  auto active_plan = control_cone_plan(true);
  Executor active_executor(outer(active_plan));
  control_first_outputs.clear();
  control_second_outputs.clear();
  control_backward_calls = 0;
  const Evaluation active = evaluate_invariant(active_executor, .25);
  close(active.value, .5, "active control-cone result");
  close(active.gradient[0], 2, "active control-cone gradient");
  check(control_first_outputs.size() == 3 &&
            distinct_addresses(control_first_outputs),
        "a value read by active work keeps every version");
  check(control_backward_calls == 3,
        "active control member runs its backward every iteration");

  auto while_plan = std::make_shared<StructuredLoop>();
  const int while_theta = while_plan->body.add_slot(1, false);
  const int counter = scalar(*while_plan, 0);
  const int zero = scalar(*while_plan, 0);
  const int one = scalar(*while_plan, 1);
  const int three = scalar(*while_plan, 3);
  const int first = while_plan->body.add_slot(1, false);
  const int condition = while_plan->body.add_slot(1, false);
  const int next = while_plan->body.add_slot(1, false);
  while_plan->imports = {{while_theta, 0, 0, true}};
  Node first_call = call(*while_plan, OP_ADD, {counter, zero}, first);
  const int first_op = first_call.op;
  Node compare = call(*while_plan, OP_COMPARE, {three, first}, condition);
  while_plan->body.ops[static_cast<size_t>(compare.op)].variant = 2;
  const int compare_op = compare.op;
  while_plan->root = while_loop(
      condition, sequence({std::move(first_call), std::move(compare)}),
      sequence({call(*while_plan, OP_ADD, {counter, one}, next),
                alias(counter, next)}));
  while_plan->outputs = {counter};
  while_plan->prepare();
  check(set_forward(while_plan->root, first_op, record_control_add) &&
            set_forward(while_plan->root, compare_op, record_control_compare),
        "find while control-cone callbacks");
  control_first_outputs.clear();
  control_second_outputs.clear();
  Executor while_executor(outer(while_plan));
  close(evaluate_invariant(while_executor, 0).value, 3,
        "while without capacity proof runs to its guard");
  check(control_first_outputs.size() == 4 &&
            control_second_outputs.size() == 4 &&
            one_address(control_first_outputs),
        "while guard evaluates once more than its body");

  auto escaped_plan = std::make_shared<StructuredLoop>();
  const int escaped_theta = escaped_plan->body.add_slot(1, false);
  const int escaped_lower = scalar(*escaped_plan, 1);
  const int escaped_upper = scalar(*escaped_plan, 3);
  const int escaped_iterator = escaped_plan->body.add_slot(1, false);
  const int escaped_zero = scalar(*escaped_plan, 0);
  const int escaped_limit = scalar(*escaped_plan, 3);
  const int escaped_first = escaped_plan->body.add_slot(1, false);
  const int escaped_condition = escaped_plan->body.add_slot(1, false);
  const int escaped_output = escaped_plan->body.add_slot(1, false);
  escaped_plan->imports = {{escaped_theta, 0, 0, true}};
  Node escaped_add = call(*escaped_plan, OP_ADD,
                          {escaped_iterator, escaped_zero}, escaped_first);
  const int escaped_add_op = escaped_add.op;
  Node escaped_compare =
      call(*escaped_plan, OP_COMPARE, {escaped_first, escaped_limit},
           escaped_condition);
  escaped_plan->body.ops[static_cast<size_t>(escaped_compare.op)].variant = 0;
  escaped_plan->root =
      counted(escaped_lower, escaped_upper, escaped_iterator,
              sequence({std::move(escaped_add), std::move(escaped_compare),
                        branch(escaped_condition, sequence({}), sequence({})),
                        alias(escaped_output, escaped_first)}));
  escaped_plan->outputs = {escaped_output};
  escaped_plan->prepare();
  check(set_forward(escaped_plan->root, escaped_add_op, record_control_add),
        "find escaping control value callback");
  control_first_outputs.clear();
  Executor escaped_executor(outer(escaped_plan));
  close(evaluate_invariant(escaped_executor, 0).value, 3,
        "escaping control value result");
  check(control_first_outputs.size() == 3 &&
            distinct_addresses(control_first_outputs),
        "an aliased control value is retained");

  auto retry_plan = control_cone_plan(false);
  int retry_op = -1;
  std::function<void(const Node&)> find_first = [&](const Node& node) {
    if (retry_op >= 0) return;
    if (node.kind == Node::KernelCall &&
        retry_plan->body.ops[static_cast<size_t>(node.op)].opcode == OP_ADD) {
      const Op& op = retry_plan->body.ops[static_cast<size_t>(node.op)];
      if (op.in[0] == 3) retry_op = node.op;
    }
    for (const auto& child : node.children) find_first(child);
  };
  find_first(retry_plan->root);
  check(retry_op >= 0 && set_forward(retry_plan->root, retry_op,
                                     throw_then_record_control_add),
        "find retry control-cone callback");
  Executor retry_executor(outer(retry_plan));
  control_throw_once = true;
  bool threw = false;
  try {
    (void)evaluate_invariant(retry_executor, .25);
  } catch (const std::runtime_error& error) {
    threw = std::string(error.what()) == "injected control failure";
  }
  check(threw, "transient callback preserves its forward exception");
  const Evaluation retry = evaluate_invariant(retry_executor, .25);
  check(std::memcmp(&direct, &retry, sizeof(Evaluation)) == 0,
        "retry after a transient failure rebuilds clean state");
}

static std::vector<double*> iterator_inputs;
static void record_iterator_add(KernelCtx& context) {
  iterator_inputs.push_back(context.in[0].data);
  find_kernel(OP_ADD)->forward(context);
}
static void record_iterator_mul(KernelCtx& context) {
  iterator_inputs.push_back(context.in[1].data);
  find_kernel(OP_MUL)->forward(context);
}

static void iterator_cell_tests() {
  {
    auto plan = std::make_shared<StructuredLoop>();
    const int theta = plan->body.add_slot(1, false);
    const int lower = scalar(*plan, 1);
    const int upper = scalar(*plan, 3);
    const int iterator = plan->body.add_slot(1, false);
    const int zero = scalar(*plan, 0);
    const int first = plan->body.add_slot(1, false);
    const int result = plan->body.add_slot(1, false);
    plan->imports = {{theta, 0, 0, true}};
    Node read = call(*plan, OP_ADD, {iterator, zero}, first);
    const int read_op = read.op;
    plan->root = counted(lower, upper, iterator,
                         sequence({std::move(read), alias(result, first)}));
    plan->outputs = {result};
    plan->prepare();
    check(plan->root.storage == Node::Transient,
          "iterator read only in place keeps one cell");
    check(set_forward(plan->root, read_op, record_iterator_add),
          "find iterator cell callback");
    iterator_inputs.clear();
    Executor executor(outer(plan));
    close(evaluate(executor, 0, 0).value, 3, "iterator cell final value");
    check(iterator_inputs.size() == 3 && one_address(iterator_inputs),
          "iterator cell is reused across iterations");
  }
  {
    auto plan = std::make_shared<StructuredLoop>();
    const int theta = plan->body.add_slot(1, false);
    const int lower = scalar(*plan, 1);
    const int upper = scalar(*plan, 3);
    const int iterator = plan->body.add_slot(1, false);
    const int zero = scalar(*plan, 0);
    const int term = plan->body.add_slot(1, false);
    const int result = plan->body.add_slot(1, false);
    const int next = plan->body.add_slot(1, false);
    plan->imports = {{theta, 0, 0, true}};
    Node scale = call(*plan, OP_MUL, {theta, iterator}, term);
    const int scale_op = scale.op;
    plan->root =
        sequence({alias(result, zero),
                  counted(lower, upper, iterator,
                          sequence({std::move(scale),
                                    call(*plan, OP_ADD, {result, term}, next),
                                    alias(result, next)}))});
    plan->outputs = {result};
    prepare_kernels(*plan);
    check(plan->root.children[1].storage == Node::Retained,
          "iterator read by active work keeps every version");
    check(set_forward(plan->root, scale_op, record_iterator_mul),
          "find retained iterator callback");
    iterator_inputs.clear();
    Executor executor(outer(plan));
    const Evaluation traced = evaluate(executor, .25, 0);
    close(traced.value, 1.5, "retained iterator value");
    close(traced.gradient[0], 6, "retained iterator gradient");
    check(iterator_inputs.size() == 3 && distinct_addresses(iterator_inputs),
          "retained iterator has one version per iteration");
  }
}

static void concurrency_tests() {
  auto reference_plan = recurrence(64);
  Executor reference(outer(reference_plan));
  const Evaluation expected_a = evaluate(reference, .1, .7);
  const Evaluation expected_b = evaluate(reference, -.2, .3);

  const Graph graph = outer(recurrence(64));
  Executor first(graph), second(graph);
  std::atomic<bool> start{false};
  std::atomic<bool> correct{true};
  const auto run = [&](Executor& executor, double theta, double beta,
                       const Evaluation& expected) {
    while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
    for (int repetition = 0; repetition < 16; ++repetition) {
      const Evaluation actual = evaluate(executor, theta, beta);
      if (!near(actual.value, expected.value) ||
          !near(actual.gradient[0], expected.gradient[0]) ||
          !near(actual.gradient[1], expected.gradient[1]))
        correct.store(false, std::memory_order_relaxed);
    }
  };
  std::thread a(run, std::ref(first), .1, .7, std::cref(expected_a));
  std::thread b(run, std::ref(second), -.2, .3, std::cref(expected_b));
  start.store(true, std::memory_order_release);
  a.join();
  b.join();
  check(correct.load(std::memory_order_relaxed),
        "executor state is isolated across concurrent executors");
}

static std::atomic<bool> fail_forward{false};
static std::atomic<bool> fail_backward{false};

static void failure_test_forward(KernelCtx& context) {
  if (fail_forward.exchange(false))
    throw std::runtime_error("injected structured forward failure");
  find_kernel(OP_TANHV)->forward(context);
}

static void failure_test_backward(KernelCtx& context) {
  if (fail_backward.exchange(false))
    throw std::runtime_error("injected structured backward failure");
  find_kernel(OP_TANHV)->backward(context);
}

static bool install_failure_callbacks(StructuredLoop& plan, Node& node) {
  if (node.kind == Node::KernelCall &&
      plan.body.ops[static_cast<size_t>(node.op)].opcode == OP_TANHV) {
    node.forward = failure_test_forward;
    node.backward = failure_test_backward;
    return true;
  }
  for (auto& child : node.children)
    if (install_failure_callbacks(plan, child)) return true;
  return false;
}

static void failure_tests() {
  auto plan = unsegmented([] { return recurrence(8); });
  check(install_failure_callbacks(*plan, plan->root),
        "failure test finds a native callback");
  Executor executor(outer(plan));
  executor.params_data()[0] = .1;
  executor.params_data()[1] = .7;
  double gradient[2];

  fail_forward.store(true);
  bool forward_threw = false;
  try {
    (void)executor.gradient(gradient);
  } catch (const std::runtime_error& error) {
    forward_threw =
        std::string(error.what()) == "injected structured forward failure";
  }
  check(forward_threw, "forward exception is preserved");

  fail_backward.store(true);
  bool backward_threw = false;
  try {
    (void)executor.gradient(gradient);
  } catch (const std::runtime_error& error) {
    backward_threw =
        std::string(error.what()) == "injected structured backward failure";
  }
  check(backward_threw, "backward exception is preserved");

  bool stale_reverse = false;
  try {
    KernelCtx stale{};
    structured_loop_backward(stale);
  } catch (const std::logic_error&) {
    stale_reverse = true;
  }
  check(stale_reverse, "reverse without a forward is refused");

  auto reference_plan = recurrence(8);
  Executor reference(outer(reference_plan));
  const Evaluation expected = evaluate(reference, .1, .7);
  const Evaluation actual = evaluate(executor, .1, .7);
  close(actual.value, expected.value, "retry after failures value");
  close(actual.gradient[0], expected.gradient[0],
        "retry after failures theta gradient");
  close(actual.gradient[1], expected.gradient[1],
        "retry after failures beta gradient");
}

static void refusal_tests() {
  {
    auto plan = std::make_shared<StructuredLoop>();
    const int theta = plan->body.add_slot(1, false);
    plan->imports = {{theta, 0, 0, true}};
    Node exit;
    exit.kind = Node::Break;
    plan->root = sequence({std::move(exit)});
    plan->outputs = {theta};
    bool rejected = false;
    try {
      plan->prepare();
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    check(rejected, "break outside a loop is rejected");
  }
  {
    auto plan = std::make_shared<StructuredLoop>();
    const int theta = plan->body.add_slot(1, false);
    const int wide = plan->body.add_slot(2, false);
    plan->imports = {{theta, 0, 0, true}};
    plan->root = alias(wide, theta);
    plan->outputs = {wide};
    bool rejected = false;
    try {
      plan->prepare();
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    check(rejected, "alias between different lengths is rejected");
  }
  {
    auto plan = std::make_shared<StructuredLoop>();
    const int theta = plan->body.add_slot(1, false);
    const int result = plan->body.add_slot(1, false);
    plan->imports = {{theta, 0, 0, true}};
    plan->root = call(*plan, OP_LOOP, {theta}, result);
    plan->outputs = {result};
    bool rejected = false;
    try {
      plan->prepare();
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    check(rejected, "nested OP_LOOP is rejected");
  }
}

static void automatic_policy_tests() {
  const auto small_auto = compile_fixture("structured_counted", 4, Mode::Auto);
  const auto small_off = compile_fixture("structured_counted", 4, Mode::Off);
  check(retained(small_auto) == nullptr,
        "small scalar loop stays on the legacy path by default");
  check(same_graph_shape(small_auto.graph, small_off.graph),
        "small automatic graph matches the legacy graph");
  compare_gradients(small_auto, small_off, {{.1, .7}},
                    "small auto target parity", "small auto gradient parity");

  const auto hazard_auto = compile_fixture("structured_nested", 32, Mode::Auto);
  const auto hazard_off = compile_fixture("structured_nested", 32, Mode::Off);
  const StructuredLoop* hazard_plan = retained(hazard_auto);
  check(hazard_plan != nullptr,
        "outer repeated nested-control hazard selects OP_LOOP");
  if (hazard_plan) check_native_only(*hazard_plan, "auto body is native-only");
  compare_gradients(hazard_auto, hazard_off, {{.1, .7}, {-.2, .3}},
                    "auto hazard target parity", "auto hazard gradient parity");

  std::string renamed = fixture_mir("structured_nested");
  size_t position = 0;
  while ((position = renamed.find("state", position)) != std::string::npos) {
    renamed.replace(position, 5, "accumulation");
    position += 12;
  }
  test_unsetenv("STANLI_STRUCTURED_LOOPS");
  DataMap renamed_data;
  renamed_data.set_int("N", 32);
  const auto renamed_auto = compile_model(renamed, renamed_data);
  check(retained(renamed_auto) != nullptr,
        "alpha-renaming preserves automatic structural selection");

  bool force_refused = false;
  try {
    (void)compile_fixture("structured_auto_refusal", 32, Mode::Force);
  } catch (const CompileError&) {
    force_refused = true;
  }
  check(force_refused, "unsupported native candidate is identified");
  const auto refused_auto =
      compile_fixture("structured_auto_refusal", 32, Mode::Auto);
  const auto refused_off =
      compile_fixture("structured_auto_refusal", 32, Mode::Off);
  check(retained(refused_auto) == nullptr,
        "unsupported automatic candidate falls back to legacy");
  check(same_graph_shape(refused_auto.graph, refused_off.graph),
        "refused candidate leaves no partial graph state");
  compare_gradients(refused_auto, refused_off, {{.25}, {-1.5}},
                    "refusal target parity", "refusal gradient parity");

  const auto recurrence_auto =
      compile_fixture("structured_counted", 64, Mode::Auto);
  const auto recurrence_off =
      compile_fixture("structured_counted", 64, Mode::Off);
  check(retained(recurrence_auto) == nullptr,
        "control-free recurrence stays on the legacy path");
  check(same_graph_shape(recurrence_auto.graph, recurrence_off.graph),
        "control-free automatic graph matches the legacy graph");

  const auto param_if_auto =
      compile_fixture("structured_param_if", observed_data(64), Mode::Auto);
  const auto param_if_off =
      compile_fixture("structured_param_if", observed_data(64), Mode::Off);
  const StructuredLoop* param_if_plan = retained(param_if_auto);
  check(param_if_plan != nullptr,
        "parameter-dependent branch in a data loop selects OP_LOOP");
  if (param_if_plan) {
    check(has_kind(param_if_plan->root, Node::If), "retained parameter branch");
    check_native_only(*param_if_plan, "parameter branch body is native-only");
  }
  compare_gradients(
      param_if_auto, param_if_off, {{.4, .2}, {-.3, -.1}, {1.5, .7}},
      "parameter branch target parity", "parameter branch gradient parity");

  const auto data_if_auto =
      compile_fixture("structured_data_if", observed_data(64), Mode::Auto);
  const auto data_if_off =
      compile_fixture("structured_data_if", observed_data(64), Mode::Off);
  check(retained(data_if_auto) == nullptr,
        "data-only branch in a data loop stays on the legacy path");
  check(same_graph_shape(data_if_auto.graph, data_if_off.graph),
        "data-only branch automatic graph matches the legacy graph");

  const auto while_auto =
      compile_fixture("structured_while_top", 40, Mode::Auto);
  const auto while_off = compile_fixture("structured_while_top", 40, Mode::Off);
  const StructuredLoop* while_plan = retained(while_auto);
  check(while_plan != nullptr, "top-level while selects OP_LOOP");
  if (while_plan) {
    check(has_kind(while_plan->root, Node::While), "retained top-level while");
    check_native_only(*while_plan, "top-level while body is native-only");
  }
  compare_gradients(while_auto, while_off, {{.3}, {-.8}},
                    "top-level while target parity",
                    "top-level while gradient parity");
}

static void prefer_parent_tests() {
  DataMap data;
  data.set_int("N", 1);
  std::optional<CompiledModel> prefer;
  try {
    prefer = compile_fixture("structured_prefer_shape", data, Mode::Prefer);
  } catch (const CompileError& error) {
    std::printf("  %s\n", error.what());
  }
  check(prefer.has_value(),
        "prefer compiles a shape query on an indexed value");
  if (!prefer) return;
  const auto legacy =
      compile_fixture("structured_prefer_shape", data, Mode::Off);
  check(retained(*prefer) == nullptr, "prefer finds no loop to retain");
  check(same_graph_shape(prefer->graph, legacy.graph),
        "prefer leaves loop-free lowering on the legacy path");
  compare_gradients(*prefer, legacy, {{.5, -.2, .1}, {-1.0, .4, .3}},
                    "prefer target parity", "prefer gradient parity");
}

static void direct_index_lowering_tests() {
  test_unsetenv("STANLI_NO_STRUCTURED_DIRECT_INDEX_INPUTS");
  const auto direct =
      compile_fixture("structured_direct_index", 4, Mode::Force);
  test_setenv("STANLI_NO_STRUCTURED_DIRECT_INDEX_INPUTS", "1");
  const auto packed =
      compile_fixture("structured_direct_index", 4, Mode::Force);
  test_unsetenv("STANLI_NO_STRUCTURED_DIRECT_INDEX_INPUTS");
  const StructuredLoop* direct_plan = retained(direct);
  const StructuredLoop* packed_plan = retained(packed);
  check(direct_plan && packed_plan,
        "direct-index ablation keeps structured execution");
  size_t direct_reads = 0, direct_updates = 0, direct_concats = 0,
         packed_reads = 0, packed_updates = 0, packed_concats = 0;
  bool direct_multi_selector = false;
  size_t in_place = 0;
  std::function<void(const Node&)> count_in_place = [&](const Node& node) {
    in_place += node.kind == Node::KernelCall && node.storage == Node::InPlace;
    for (const auto& child : node.children) count_in_place(child);
  };
  if (direct_plan) {
    count_in_place(direct_plan->root);
    for (const auto& op : direct_plan->body.ops) {
      direct_concats += op.opcode == OP_CONCAT2;
      if (op.opcode == OP_SET_INDEX_DYNAMIC) {
        const auto* spec = static_cast<const DynamicIndexSpec*>(op.udata);
        if (spec && spec->input_count > 0) {
          ++direct_updates;
          check(op.n_in == spec->input_count && op.n_in >= 4 && op.n_in <= 6 &&
                    spec->rhs_input == op.n_in - 1,
                "indexed updates bind direct selectors before the RHS");
        }
      } else if (op.opcode == OP_INDEX_DYNAMIC) {
        const auto* spec = static_cast<const DynamicIndexSpec*>(op.udata);
        if (spec && spec->input_count > 0) {
          ++direct_reads;
          check(op.n_in == spec->input_count && op.n_in == 3,
                "two-axis read binds direct selector inputs");
          direct_multi_selector |=
              direct_plan->body.slots[static_cast<size_t>(op.in[1])].len > 1 ||
              direct_plan->body.slots[static_cast<size_t>(op.in[2])].len > 1;
        }
      }
    }
  }
  check(in_place > 0, "lowered indexed assignment is fused in place");
  if (packed_plan)
    for (const auto& op : packed_plan->body.ops) {
      packed_concats += op.opcode == OP_CONCAT2;
      if (op.opcode == OP_INDEX_DYNAMIC) {
        ++packed_reads;
        const auto* spec = static_cast<const DynamicIndexSpec*>(op.udata);
        check(spec && spec->input_count == 0 && spec->rhs_input == -1 &&
                  op.n_in == 2,
              "direct-index ablation preserves packed read ABI");
      } else if (op.opcode == OP_SET_INDEX_DYNAMIC) {
        ++packed_updates;
        const auto* spec = static_cast<const DynamicIndexSpec*>(op.udata);
        check(spec && spec->input_count == 0 && spec->rhs_input == -1 &&
                  op.n_in == 3,
              "direct-index ablation preserves packed update ABI");
      }
    }
  check(direct_reads > 0 && packed_reads >= direct_reads,
        "structured model exposes direct-index read sites");
  check(direct_multi_selector,
        "fixed-capacity multi-index binds a direct data operand");
  check(direct_updates > 0 && packed_updates >= direct_updates,
        "structured model exposes direct-index update sites");
  check(direct_concats < packed_concats,
        "direct selectors remove packing operations");
  compare_gradients(direct, packed, {{.1, .7}, {-.2, .3}, {0, .5}},
                    "direct-index value parity",
                    "direct-index gradient parity");
  const auto legacy = compile_fixture("structured_direct_index", 4, Mode::Off);
  compare_gradients(direct, legacy, {{.1, .7}, {-.2, .3}},
                    "direct-index legacy value parity",
                    "direct-index legacy gradient parity");
}

static DataMap runtime_slice_data(int op) {
  DataMap data;
  data.set_int("N", 5);
  data.set_int_array("nobs", {3, 1, 2});
  data.set_int("op", op);
  return data;
}

// A slice whose upper bound is a loop-carried integer keeps the declared
// capacity as its storage, so every consumer of one has to be told where the
// live values stop. The flat model spells the same arithmetic over a
// compile-time trip count, where the extents fold.
static double written_s(const CompiledModel& model,
                        const std::vector<double>& point) {
  Executor ex(model.graph);
  model.bind(ex);
  std::copy(point.begin(), point.end(), ex.params_data());
  ex.run_forward_only();
  if (model.write_array && model.write_array->interp) {
    WaRng rng(1234);
    const std::vector<double> row =
        model.write_array->interp->eval(model.constrained_env(ex), rng);
    const std::vector<std::string> names =
        CompiledModel::csv_names(model.write_array->interp->columns());
    for (size_t i = 0; i < names.size() && i < row.size(); ++i)
      if (names[i] == "s") return row[i];
  } else if (model.write_array && model.write_array->truncated.empty()) {
    Executor wex(model.write_array->graph);
    model.write_array->bind(wex);
    std::copy(point.begin(), point.end(), wex.params_data());
    wex.run_forward_only();
    for (const auto& column : model.write_array->columns)
      if (column.name == "s") return wex.value_ptr(column.slot)[0];
  }
  check(false, "runtime slice write_array has s");
  return 0;
}

static void runtime_slice_tests() {
  static const char* const names[] = {"",
                                      "log_sum_exp",
                                      "extrema",
                                      "num_elements",
                                      "sum and prod",
                                      "normal_lpdf",
                                      "elementwise product",
                                      "dot product",
                                      "moments",
                                      "reduction of an elementwise result",
                                      "matrix_exp of a runtime submatrix"};
  std::vector<double> point(14);
  for (size_t i = 0; i < point.size(); ++i) point[i] = 0.2 * double(i) - 0.9;
  for (int segments = 0; segments < 2; ++segments) {
    if (segments)
      test_unsetenv("STANLI_NO_STRUCTURED_SEGMENTS");
    else
      test_setenv("STANLI_NO_STRUCTURED_SEGMENTS", "1");
    for (int op = 1; op <= 10; ++op) {
      const auto loop =
          compile_fixture("dynslice", runtime_slice_data(op), Mode::Auto);
      const auto flat =
          compile_fixture("dynsliceflat", runtime_slice_data(op), Mode::Auto);
      check(retained(loop) != nullptr, "runtime slice model runs the loop");
      check(retained(flat) == nullptr, "flat runtime slice model has no loop");
      Executor a(loop.graph), b(flat.graph);
      loop.bind(a);
      flat.bind(b);
      std::vector<double> ga(14), gb(14);
      for (int again = 0; again < 2; ++again) {
        std::vector<double> at = point;
        if (again)
          for (double& q : at) q = 0.3 - q;
        std::copy(at.begin(), at.end(), a.params_data());
        std::copy(at.begin(), at.end(), b.params_data());
        const double va = a.gradient(ga.data()), vb = b.gradient(gb.data());
        if (va != vb) {
          std::printf("  %s: %.17g != %.17g\n", names[op], va, vb);
          check(false, "runtime slice value");
        }
        for (size_t i = 0; i < ga.size(); ++i)
          if (!near(ga[i], gb[i])) {
            std::printf("  %s g%zu: %.17g != %.17g\n", names[op], i, ga[i],
                        gb[i]);
            check(false, "runtime slice gradient");
          }
        close(written_s(loop, at), written_s(flat, at),
              "runtime slice write_array");
      }
    }
  }
  test_unsetenv("STANLI_NO_STRUCTURED_SEGMENTS");

  // The series whose `while` advances k past two terms, against the value
  // CmdStan computes for it.
  DataMap theta;
  theta.set_real("theta", std::exp(0.1));
  const auto series =
      compile_fixture("dynseries", std::move(theta), Mode::Force);
  Executor ex(series.graph);
  series.bind(ex);
  ex.params_data()[0] = 0.5;
  std::vector<double> g(static_cast<size_t>(series.n_unconstrained));
  close(ex.gradient(g.data()), -0.81912447467137883, "runtime slice series");
}

// A runtime-length selector reaches the index kernel as a fixed-capacity
// buffer with its live count beside it. The packing route and the direct one
// have to agree about that.
static void runtime_index_tests() {
  static const char* const names[] = {"", "gather", "matrix slice shape",
                                      "array slice size"};
  std::vector<double> point(14);
  for (size_t i = 0; i < point.size(); ++i) point[i] = 0.15 * double(i) - 0.8;
  for (int packed = 0; packed < 2; ++packed) {
    if (packed)
      test_setenv("STANLI_NO_STRUCTURED_DIRECT_INDEX_INPUTS", "1");
    else
      test_unsetenv("STANLI_NO_STRUCTURED_DIRECT_INDEX_INPUTS");
    for (int op = 1; op <= 3; ++op) {
      const auto loop =
          compile_fixture("dynindex", runtime_slice_data(op), Mode::Auto);
      const auto flat =
          compile_fixture("dynindexflat", runtime_slice_data(op), Mode::Auto);
      check(retained(loop) != nullptr, "runtime index model runs the loop");
      check(retained(flat) == nullptr, "flat runtime index model has no loop");
      Executor a(loop.graph), b(flat.graph);
      loop.bind(a);
      flat.bind(b);
      std::vector<double> ga(14), gb(14);
      std::copy(point.begin(), point.end(), a.params_data());
      std::copy(point.begin(), point.end(), b.params_data());
      const double va = a.gradient(ga.data()), vb = b.gradient(gb.data());
      if (va != vb) {
        std::printf("  %s: %.17g != %.17g\n", names[op], va, vb);
        check(false, "runtime index value");
      }
      for (size_t i = 0; i < ga.size(); ++i)
        if (!near(ga[i], gb[i])) {
          std::printf("  %s g%zu: %.17g != %.17g\n", names[op], i, ga[i],
                      gb[i]);
          check(false, "runtime index gradient");
        }
      close(written_s(loop, point), written_s(flat, point),
            "runtime index write_array");
    }
  }
  test_unsetenv("STANLI_NO_STRUCTURED_DIRECT_INDEX_INPUTS");

  std::string leaf_width;
  try {
    (void)compile_fixture("dynindex", runtime_slice_data(7), Mode::Force);
  } catch (const CompileError& e) {
    leaf_width = e.what();
  }
  check(leaf_width.find("num_elements") != std::string::npos,
        "num_elements declines a rank-2 runtime view");

  for (int op = 4; op <= 6; ++op) {
    std::string mixed;
    try {
      (void)compile_fixture("dynindex", runtime_slice_data(op), Mode::Force);
    } catch (const CompileError& e) {
      mixed = e.what();
    }
    if (mixed.find("full-extent operand") == std::string::npos) {
      std::printf("  op %d: %s\n", op, mixed.c_str());
      check(false, "a full-width operand beside a runtime-length one refuses");
    }
  }
}

static std::atomic<int> memo_int_calls{0};
static std::atomic<int> memo_compare_calls{0};
static std::atomic<int> memo_index_calls{0};

static void count_memo_int(KernelCtx& context) {
  ++memo_int_calls;
  find_kernel(OP_INT_ARITH)->forward(context);
}
static void count_memo_compare(KernelCtx& context) {
  ++memo_compare_calls;
  find_kernel(OP_COMPARE)->forward(context);
}
static void count_memo_index(KernelCtx& context) {
  ++memo_index_calls;
  find_kernel(OP_INDEX_DYNAMIC)->forward(context);
}

static const Node* find_kind(const Node& node, Node::Kind kind) {
  if (node.kind == kind) return &node;
  for (const auto& child : node.children)
    if (const Node* found = find_kind(child, kind)) return found;
  return nullptr;
}

static size_t count_memo(const Node& node) {
  size_t total = node.memo ? 1 : 0;
  for (const auto& child : node.children) total += count_memo(child);
  return total;
}

// theta * sum(1..n) where the sum is an integer chain on the data import n.
static std::shared_ptr<StructuredLoop> memo_counter_plan(bool with_break) {
  auto plan = std::make_shared<StructuredLoop>();
  const int theta = plan->body.add_slot(1, false);
  const int n = plan->body.add_slot(1, false);
  const int one = scalar(*plan, 1);
  const int three = scalar(*plan, 3);
  const int iterator = plan->body.add_slot(1, false);
  const int acc = plan->body.add_slot(1, false);
  plan->fills.push_back({acc, {0}});
  const int next = plan->body.add_slot(1, false);
  const int stop = plan->body.add_slot(1, false);
  const int result = plan->body.add_slot(1, false);
  plan->imports = {{theta, 0, 0, true, false}, {n, 2, 0, false, true}};
  Node step = call(*plan, OP_INT_ARITH, {acc, iterator}, next);
  const int step_op = step.op;
  std::vector<Node> body;
  if (with_break) {
    Node compare = call(*plan, OP_COMPARE, {iterator, three}, stop);
    plan->body.ops[static_cast<size_t>(compare.op)].variant = 2;
    Node exit;
    exit.kind = Node::Break;
    body.push_back(std::move(compare));
    body.push_back(branch(stop, std::move(exit), sequence({})));
  }
  body.push_back(std::move(step));
  body.push_back(alias(acc, next));
  plan->root = sequence({counted(one, n, iterator, sequence(std::move(body))),
                         call(*plan, OP_MUL, {theta, acc}, result)});
  plan->outputs = {result};
  plan->prepare();
  check(set_forward(plan->root, step_op, count_memo_int),
        "find memo counter callback");
  return plan;
}

static Evaluation evaluate_memo(Executor& executor, double theta, double n) {
  executor.value_ptr(2)[0] = n;
  return evaluate(executor, theta, 0);
}

// index table[k] > 0 selects the active arm; `active_compare` swaps the
// comparison threshold for theta so the condition depends on a parameter.
static std::shared_ptr<StructuredLoop> memo_branch_plan(bool active_compare) {
  auto plan = std::make_shared<StructuredLoop>();
  const int theta = plan->body.add_slot(1, false);
  const int table = plan->body.add_slot(3, false);
  const int k = plan->body.add_slot(1, false);
  const int zero = scalar(*plan, 0);
  const int three = scalar(*plan, 3);
  const int picked = plan->body.add_slot(1, false);
  const int condition = plan->body.add_slot(1, false);
  const int scaled = plan->body.add_slot(1, false);
  const int result = plan->body.add_slot(1, false);
  plan->imports = {{theta, 0, 0, true, false},
                   {table, 2, 0, false, true},
                   {k, 3, 0, false, true}};
  Node index = call(*plan, OP_INDEX_DYNAMIC, {table, k}, picked);
  attach(*plan, index.op, single_spec(3));
  const int index_op = index.op;
  Node compare = call(*plan, OP_COMPARE,
                      {picked, active_compare ? theta : zero}, condition);
  plan->body.ops[static_cast<size_t>(compare.op)].variant = 2;
  const int compare_op = compare.op;
  plan->root =
      sequence({std::move(index), std::move(compare),
                branch(condition,
                       sequence({call(*plan, OP_MUL, {theta, three}, scaled),
                                 alias(result, scaled)}),
                       alias(result, zero))});
  plan->outputs = {result};
  plan->prepare();
  check(set_forward(plan->root, index_op, count_memo_index) &&
            set_forward(plan->root, compare_op, count_memo_compare),
        "find memo branch callbacks");
  return plan;
}

static Evaluation evaluate_branch(Executor& executor, double theta,
                                  double table_at_two) {
  executor.value_ptr(2)[0] = 0;
  executor.value_ptr(2)[1] = table_at_two;
  executor.value_ptr(2)[2] = 0;
  executor.value_ptr(3)[0] = 2;
  return evaluate(executor, theta, 0);
}

static void memo_tests() {
  {
    auto plan = memo_counter_plan(false);
    const Node* loop = find_kind(plan->root, Node::For);
    check(loop && loop->memo && plan->memo_count == 1 &&
              count_memo(plan->root) == 1,
          "data-only counted loop is the memo node");
    memo_int_calls = 0;
    Executor executor(outer(plan, {1, 1}, {1}));
    const Evaluation first = evaluate_memo(executor, .25, 4);
    close(first.value, 2.5, "memo loop first value");
    close(first.gradient[0], 10, "memo loop first gradient");
    check(memo_int_calls == 4, "memo loop records on the first evaluation");
    const Evaluation second = evaluate_memo(executor, .5, 4);
    close(second.value, 5, "memo loop replayed value");
    close(second.gradient[0], 10, "memo loop replayed gradient");
    check(memo_int_calls == 4, "memo loop replays without kernel calls");
    const Evaluation third = evaluate_memo(executor, .5, 4);
    check(std::memcmp(&second, &third, sizeof(Evaluation)) == 0,
          "memo replay is bitwise stable");
  }
  {
    auto plan = memo_branch_plan(false);
    check(plan->memo_count == 1 && plan->root.children.size() == 2 &&
              plan->root.children[0].memo &&
              plan->root.children[0].kind == Node::Sequence &&
              plan->root.children[0].children.size() == 2 &&
              !plan->root.children[1].memo,
          "data-only condition cone is grouped into one memo sequence");
    memo_index_calls = memo_compare_calls = 0;
    Executor executor(outer(plan, {1, 1}, {3, 1}));
    const Evaluation first = evaluate_branch(executor, .25, 5);
    close(first.value, .75, "memo branch first value");
    close(first.gradient[0], 3, "memo branch first gradient");
    const Evaluation second = evaluate_branch(executor, .5, 5);
    close(second.value, 1.5, "memo branch replayed value");
    close(second.gradient[0], 3, "memo branch replayed gradient");
    check(memo_index_calls == 1 && memo_compare_calls == 1,
          "memo condition cone is not recomputed");
    Executor other(outer(memo_branch_plan(false), {1, 1}, {3, 1}));
    const Evaluation no_arm = evaluate_branch(other, .25, 0);
    close(no_arm.value, 0, "memo branch takes the data-selected arm");
    close(no_arm.gradient[0], 0, "memo branch inactive arm gradient");
  }
  {
    auto plan = memo_branch_plan(true);
    const Node* branch_node = find_kind(plan->root, Node::If);
    check(branch_node && !branch_node->memo && plan->memo_count == 1 &&
              plan->root.children[0].memo &&
              plan->root.children[0].children.size() == 1,
          "parameter-dependent comparison leaves only the data read memoized");
    memo_index_calls = memo_compare_calls = 0;
    Executor executor(outer(plan, {1, 1}, {3, 1}));
    const Evaluation first = evaluate_branch(executor, .25, 5);
    close(first.value, .75, "dependent branch first value");
    close(first.gradient[0], 3, "dependent branch first gradient");
    const Evaluation second = evaluate_branch(executor, 10, 5);
    close(second.value, 0, "dependent branch switches arms");
    close(second.gradient[0], 0, "dependent branch switched gradient");
    check(memo_compare_calls == 2 && memo_index_calls == 1,
          "parameter-dependent comparison runs every evaluation");
  }
  {
    auto plan = std::make_shared<StructuredLoop>();
    const int theta = plan->body.add_slot(1, false);
    const int n = plan->body.add_slot(1, false);
    const int one = scalar(*plan, 1);
    const int two = scalar(*plan, 2);
    const int zero = scalar(*plan, 0);
    const int iterator = plan->body.add_slot(1, false);
    const int positive = plan->body.add_slot(1, false);
    const int doubled = plan->body.add_slot(1, false);
    const int acc = plan->body.add_slot(1, false);
    plan->fills.push_back({acc, {0}});
    const int result = plan->body.add_slot(1, false);
    plan->imports = {{theta, 0, 0, true, false}, {n, 2, 0, false, true}};
    Node compare = call(*plan, OP_COMPARE, {theta, zero}, positive);
    plan->body.ops[static_cast<size_t>(compare.op)].variant = 2;
    Node step = call(*plan, OP_INT_ARITH, {two, two}, doubled);
    plan->body.ops[static_cast<size_t>(step.op)].variant = 2;
    const int step_op = step.op;
    plan->root = sequence(
        {std::move(compare),
         branch(positive,
                counted(one, n, iterator,
                        sequence({std::move(step), alias(acc, doubled)})),
                sequence({})),
         call(*plan, OP_MUL, {theta, acc}, result)});
    plan->outputs = {result};
    plan->prepare();
    check(plan->memo_count == 0 && count_memo(plan->root) == 0,
          "loop under a parameter-dependent branch is not memoized");
    check(set_forward(plan->root, step_op, count_memo_int),
          "find guarded loop callback");
    memo_int_calls = 0;
    Executor executor(outer(plan, {1, 1}, {1}));
    const Evaluation skipped = evaluate_memo(executor, -.5, 3);
    close(skipped.value, 0, "guarded loop skipped value");
    close(skipped.gradient[0], 0, "guarded loop skipped gradient");
    check(memo_int_calls == 0, "guarded loop skipped executes nothing");
    const Evaluation first = evaluate_memo(executor, .25, 3);
    close(first.value, 1, "guarded loop taken value");
    close(first.gradient[0], 4, "guarded loop taken gradient");
    const Evaluation second = evaluate_memo(executor, .5, 3);
    close(second.value, 2, "guarded loop second value");
    close(second.gradient[0], 4, "guarded loop second gradient");
    check(memo_int_calls == 2, "guarded loop recomputes on every entry");
  }
  {
    auto plan = memo_counter_plan(true);
    const Node* loop = find_kind(plan->root, Node::For);
    check(loop && loop->memo && plan->memo_count == 1,
          "break targeting a loop inside the memo node is allowed");
    memo_int_calls = 0;
    Executor executor(outer(plan, {1, 1}, {1}));
    const Evaluation first = evaluate_memo(executor, .25, 10);
    close(first.value, 1.5, "memo break loop first value");
    close(first.gradient[0], 6, "memo break loop first gradient");
    const Evaluation second = evaluate_memo(executor, .5, 10);
    close(second.value, 3, "memo break loop replayed value");
    close(second.gradient[0], 6, "memo break loop replayed gradient");
    check(memo_int_calls == 3, "memo break loop replays without kernel calls");

    auto escaping = std::make_shared<StructuredLoop>();
    const int theta = escaping->body.add_slot(1, false);
    const int n = escaping->body.add_slot(1, false);
    const int one = scalar(*escaping, 1);
    const int three = scalar(*escaping, 3);
    const int iterator = escaping->body.add_slot(1, false);
    const int stop = escaping->body.add_slot(1, false);
    const int term = escaping->body.add_slot(1, false);
    const int acc = escaping->body.add_slot(1, false);
    escaping->fills.push_back({acc, {0}});
    const int next = escaping->body.add_slot(1, false);
    escaping->imports = {{theta, 0, 0, true, false}, {n, 2, 0, false, true}};
    Node compare = call(*escaping, OP_COMPARE, {iterator, three}, stop);
    escaping->body.ops[static_cast<size_t>(compare.op)].variant = 2;
    const int compare_op = compare.op;
    Node exit;
    exit.kind = Node::Break;
    escaping->root = counted(
        one, n, iterator,
        sequence(
            {std::move(compare), branch(stop, std::move(exit), sequence({})),
             call(*escaping, OP_MUL, {theta, iterator}, term),
             call(*escaping, OP_ADD, {acc, term}, next), alias(acc, next)}));
    escaping->outputs = {acc};
    prepare_kernels(*escaping);
    const Node* guard = find_kind(escaping->root, Node::If);
    const Node* body = &escaping->root.children[0];
    check(guard && !guard->memo && !escaping->root.memo &&
              escaping->memo_count == 1 && body->children.size() == 5 &&
              body->children[0].memo && body->children[0].children.size() == 1,
          "branch whose break escapes is not memoized, its condition is");
    check(set_forward(escaping->root, compare_op, count_memo_compare),
          "find escaping break callback");
    memo_compare_calls = 0;
    Executor escaping_executor(outer(escaping, {1, 1}, {1}));
    const Evaluation escaped = evaluate_memo(escaping_executor, .25, 10);
    close(escaped.value, 1.5, "escaping break value");
    close(escaped.gradient[0], 6, "escaping break gradient");
    check(memo_compare_calls == 4, "escaping break guard evaluated per trip");
    const Evaluation replayed = evaluate_memo(escaping_executor, .5, 10);
    close(replayed.value, 3, "escaping break replayed value");
    close(replayed.gradient[0], 6, "escaping break replayed gradient");
    check(memo_compare_calls == 4, "escaping break guard replays from tape");
  }
  {
    auto plan = std::make_shared<StructuredLoop>();
    const int theta = plan->body.add_slot(1, false);
    const int k = plan->body.add_slot(1, false);
    const int zero = scalar(*plan, 0);
    const int one = scalar(*plan, 1);
    const int two = scalar(*plan, 2);
    const int three = scalar(*plan, 3);
    const int x = plan->body.add_slot(1, false);
    const int scaled = plan->body.add_slot(1, false);
    const int condition = plan->body.add_slot(1, false);
    const int result = plan->body.add_slot(1, false);
    plan->imports = {{theta, 0, 0, true, false}, {k, 2, 0, false, true}};
    Node compare = call(*plan, OP_COMPARE, {k, two}, condition);
    plan->body.ops[static_cast<size_t>(compare.op)].variant = 4;
    plan->root = sequence({call(*plan, OP_MUL, {theta, three}, scaled),
                           alias(x, scaled), std::move(compare),
                           branch(condition, sequence({}), alias(x, zero)),
                           call(*plan, OP_MUL, {x, one}, result)});
    plan->outputs = {result};
    plan->prepare();
    const Node* guard = find_kind(plan->root, Node::If);
    check(guard && !guard->memo && plan->memo_count == 1,
          "conditional data-only write of a parameter slot is not memoized");
    Executor executor(outer(plan, {1, 1}, {1}));
    const Evaluation first = evaluate_memo(executor, .25, 2);
    close(first.value, .75, "untaken data-only overwrite first value");
    close(first.gradient[0], 3, "untaken data-only overwrite first gradient");
    const Evaluation second = evaluate_memo(executor, .5, 2);
    close(second.value, 1.5, "untaken data-only overwrite second value");
    close(second.gradient[0], 3, "untaken data-only overwrite second gradient");
  }
  {
    const Graph graph = outer(memo_counter_plan(false), {1, 1}, {1});
    Executor first(graph), second(graph);
    memo_int_calls = 0;
    const Evaluation a = evaluate_memo(first, .25, 4);
    check(memo_int_calls == 4, "first executor records its own tape");
    const Evaluation b = evaluate_memo(second, .25, 4);
    check(memo_int_calls == 8, "second executor does not share the tape");
    check(std::memcmp(&a, &b, sizeof(Evaluation)) == 0,
          "independent tapes agree");
    (void)evaluate_memo(first, .5, 4);
    (void)evaluate_memo(second, .5, 4);
    check(memo_int_calls == 8, "each executor replays its own tape");
  }
}

static std::vector<double> traced_arm_iterators;
static void record_arm_mul(KernelCtx& context) {
  traced_arm_iterators.push_back(context.in[1].data[0]);
  find_kernel(OP_MUL)->forward(context);
}

static const Node* find_memo(const Node& node) {
  if (node.memo) return &node;
  for (const auto& child : node.children)
    if (const Node* found = find_memo(child)) return found;
  return nullptr;
}

// theta * sum of the iterators whose table entry is positive; `reuse_guard`
// also adds theta * guard after the branch so the guard value stays live.
static std::shared_ptr<StructuredLoop> traced_branch_plan(bool reuse_guard) {
  auto plan = std::make_shared<StructuredLoop>();
  const int theta = plan->body.add_slot(1, false);
  const int table = plan->body.add_slot(3, false);
  const int one = scalar(*plan, 1);
  const int three = scalar(*plan, 3);
  const int zero = scalar(*plan, 0);
  const int iterator = plan->body.add_slot(1, false);
  const int picked = plan->body.add_slot(1, false);
  const int guard = plan->body.add_slot(1, false);
  const int term = plan->body.add_slot(1, false);
  const int acc = plan->body.add_slot(1, false);
  plan->fills.push_back({acc, {0}});
  const int next = plan->body.add_slot(1, false);
  plan->imports = {{theta, 0, 0, true, false}, {table, 2, 0, false, true}};
  Node index = call(*plan, OP_INDEX_DYNAMIC, {table, iterator}, picked);
  attach(*plan, index.op, single_spec(3));
  const int index_op = index.op;
  Node compare = call(*plan, OP_COMPARE, {picked, zero}, guard);
  plan->body.ops[static_cast<size_t>(compare.op)].variant = 2;
  const int compare_op = compare.op;
  Node scale = call(*plan, OP_MUL, {theta, iterator}, term);
  const int scale_op = scale.op;
  std::vector<Node> body;
  body.push_back(std::move(index));
  body.push_back(std::move(compare));
  body.push_back(
      branch(guard,
             sequence({std::move(scale), call(*plan, OP_ADD, {acc, term}, next),
                       alias(acc, next)}),
             sequence({})));
  if (reuse_guard) {
    const int bonus = plan->body.add_slot(1, false);
    const int bumped = plan->body.add_slot(1, false);
    body.push_back(call(*plan, OP_MUL, {theta, guard}, bonus));
    body.push_back(call(*plan, OP_ADD, {acc, bonus}, bumped));
    body.push_back(alias(acc, bumped));
  }
  plan->root = counted(one, three, iterator, sequence(std::move(body)));
  plan->outputs = {acc};
  prepare_kernels(*plan);
  check(set_forward(plan->root, index_op, count_memo_index) &&
            set_forward(plan->root, compare_op, count_memo_compare) &&
            set_forward(plan->root, scale_op, record_arm_mul),
        "find traced branch callbacks");
  return plan;
}

static Evaluation evaluate_traced(Executor& executor, double theta,
                                  std::array<double, 3> table) {
  std::copy(table.begin(), table.end(), executor.value_ptr(2));
  traced_arm_iterators.clear();
  return evaluate(executor, theta, 0);
}

// theta^(k+1): x starts at theta and is multiplied by theta while j < k.
// `param_break` leaves the loop early once x exceeds 10.
static std::shared_ptr<StructuredLoop> traced_while_plan(bool param_break) {
  auto plan = std::make_shared<StructuredLoop>();
  const int theta = plan->body.add_slot(1, false);
  const int k = plan->body.add_slot(1, false);
  const int one = scalar(*plan, 1);
  const int ten = scalar(*plan, 10);
  const int j = plan->body.add_slot(1, false);
  plan->fills.push_back({j, {0}});
  const int more = plan->body.add_slot(1, false);
  const int x = plan->body.add_slot(1, false);
  const int grown = plan->body.add_slot(1, false);
  const int bumped = plan->body.add_slot(1, false);
  const int large = plan->body.add_slot(1, false);
  plan->imports = {{theta, 0, 0, true, false}, {k, 2, 0, false, true}};
  Node compare = call(*plan, OP_COMPARE, {j, k}, more);
  const int compare_op = compare.op;
  Node grow = call(*plan, OP_MUL, {x, theta}, grown);
  const int grow_op = grow.op;
  std::vector<Node> body;
  body.push_back(std::move(grow));
  body.push_back(alias(x, grown));
  if (param_break) {
    Node limit = call(*plan, OP_COMPARE, {x, ten}, large);
    plan->body.ops[static_cast<size_t>(limit.op)].variant = 2;
    Node exit;
    exit.kind = Node::Break;
    body.push_back(std::move(limit));
    body.push_back(branch(large, std::move(exit), sequence({})));
  }
  body.push_back(call(*plan, OP_INT_ARITH, {j, one}, bumped));
  body.push_back(alias(j, bumped));
  plan->root = sequence(
      {alias(x, theta), while_loop(more, sequence({std::move(compare)}),
                                   sequence(std::move(body)))});
  plan->outputs = {x};
  prepare_kernels(*plan);
  check(set_forward(plan->root, compare_op, count_memo_compare) &&
            set_forward(plan->root, grow_op, count_mul_forward),
        "find traced while callbacks");
  return plan;
}

// theta * sum of the iterators with 0 < table entry < 3; the guard is the
// short-circuit chain `table[i] > 0 && table[i] < 3` lowered as a branch.
static std::shared_ptr<StructuredLoop> traced_chain_plan() {
  auto plan = std::make_shared<StructuredLoop>();
  const int theta = plan->body.add_slot(1, false);
  const int table = plan->body.add_slot(3, false);
  const int one = scalar(*plan, 1);
  const int three = scalar(*plan, 3);
  const int zero = scalar(*plan, 0);
  const int iterator = plan->body.add_slot(1, false);
  const int picked = plan->body.add_slot(1, false);
  const int positive = plan->body.add_slot(1, false);
  const int again = plan->body.add_slot(1, false);
  const int small = plan->body.add_slot(1, false);
  const int guard = plan->body.add_slot(1, false);
  const int term = plan->body.add_slot(1, false);
  const int acc = plan->body.add_slot(1, false);
  plan->fills.push_back({acc, {0}});
  const int next = plan->body.add_slot(1, false);
  plan->imports = {{theta, 0, 0, true, false}, {table, 2, 0, false, true}};
  Node index = call(*plan, OP_INDEX_DYNAMIC, {table, iterator}, picked);
  attach(*plan, index.op, single_spec(3));
  const int index_op = index.op;
  Node compare = call(*plan, OP_COMPARE, {picked, zero}, positive);
  plan->body.ops[static_cast<size_t>(compare.op)].variant = 2;
  const int compare_op = compare.op;
  Node index2 = call(*plan, OP_INDEX_DYNAMIC, {table, iterator}, again);
  attach(*plan, index2.op, single_spec(3));
  const int index2_op = index2.op;
  Node compare2 = call(*plan, OP_COMPARE, {again, three}, small);
  const int compare2_op = compare2.op;
  Node scale = call(*plan, OP_MUL, {theta, iterator}, term);
  const int scale_op = scale.op;
  plan->root =
      counted(one, three, iterator,
              sequence({std::move(index), std::move(compare),
                        branch(positive,
                               sequence({std::move(index2), std::move(compare2),
                                         alias(guard, small)}),
                               alias(guard, zero)),
                        branch(guard,
                               sequence({std::move(scale),
                                         call(*plan, OP_ADD, {acc, term}, next),
                                         alias(acc, next)}),
                               sequence({}))}));
  plan->outputs = {acc};
  prepare_kernels(*plan);
  check(set_forward(plan->root, index_op, count_memo_index) &&
            set_forward(plan->root, compare_op, count_memo_compare) &&
            set_forward(plan->root, index2_op, count_memo_index) &&
            set_forward(plan->root, compare2_op, count_memo_compare) &&
            set_forward(plan->root, scale_op, record_arm_mul),
        "find traced chain callbacks");
  return plan;
}

static void trace_tests() {
  {
    auto plan = traced_chain_plan();
    const Node& body = plan->root.children[0];
    check(body.children.size() == 2 && body.children[0].memo &&
              body.children[0].children.size() == 3 &&
              body.children[0].children[2].kind == Node::If &&
              body.children[0].memo_outs.empty() && body.children[1].trace &&
              plan->root.trace && plan->memo_count == 1 &&
              plan->trace_count == 2,
          "guard chain groups its data-only branch into one memo node");
    memo_index_calls = memo_compare_calls = 0;
    Executor executor(outer(plan, {1, 1}, {3}));
    const Evaluation first = evaluate_traced(executor, .25, {1, 0, 2});
    close(first.value, 1, "guard chain first value");
    close(first.gradient[0], 4, "guard chain first gradient");
    check(memo_index_calls == 5 && memo_compare_calls == 5,
          "guard chain records both comparisons");
    const Evaluation second = evaluate_traced(executor, .5, {1, 0, 2});
    close(second.value, 2, "guard chain replayed value");
    close(second.gradient[0], 4, "guard chain replayed gradient");
    check(traced_arm_iterators == std::vector<double>{1, 3},
          "guard chain replays the recorded arms");
    check(memo_index_calls == 5 && memo_compare_calls == 5,
          "guard chain replays without guard kernel calls");
  }
  {
    auto plan = traced_branch_plan(false);
    const Node* guard = find_memo(plan->root);
    const Node* branch_node = find_kind(plan->root, Node::If);
    check(guard && guard->children.size() == 2 && guard->memo_outs.empty(),
          "guard cone feeding only a traced branch has no live-outs");
    check(branch_node && branch_node->trace && plan->root.trace &&
              plan->trace_count == 2,
          "data-only branch with an active arm is traced under a traced for");
    memo_index_calls = memo_compare_calls = 0;
    Executor executor(outer(plan, {1, 1}, {3}));
    const Evaluation first = evaluate_traced(executor, .25, {1, 0, 2});
    close(first.value, 1, "traced branch first value");
    close(first.gradient[0], 4, "traced branch first gradient");
    check(traced_arm_iterators == std::vector<double>{1, 3},
          "traced branch records the data-selected arms");
    check(memo_index_calls == 3 && memo_compare_calls == 3,
          "traced branch evaluates its guard once per trip when recording");
    const Evaluation second = evaluate_traced(executor, .5, {1, 0, 2});
    close(second.value, 2, "traced branch replayed value");
    close(second.gradient[0], 4, "traced branch replayed gradient");
    check(traced_arm_iterators == std::vector<double>{1, 3},
          "traced branch replays the recorded arms");
    check(memo_index_calls == 3 && memo_compare_calls == 3,
          "traced branch replays without guard kernel calls");
  }
  {
    auto plan = traced_while_plan(false);
    const Node* loop = find_kind(plan->root, Node::While);
    check(loop && loop->trace && plan->trace_count == 1 &&
              loop->children[0].memo && loop->children[0].memo_outs.empty(),
          "data-only while condition with an active body is traced");
    memo_compare_calls = 0;
    counted_forward_calls = 0;
    Executor executor(outer(plan, {1, 1}, {1}));
    const Evaluation first = evaluate_memo(executor, .5, 3);
    close(first.value, .0625, "traced while first value");
    close(first.gradient[0], .5, "traced while first gradient");
    check(memo_compare_calls == 4 && counted_forward_calls == 3,
          "traced while records the condition per test");
    const Evaluation second = evaluate_memo(executor, .25, 3);
    close(second.value, .00390625, "traced while replayed value");
    close(second.gradient[0], .0625, "traced while replayed gradient");
    check(memo_compare_calls == 4 && counted_forward_calls == 6,
          "traced while replays the body count without condition calls");
  }
  {
    auto plan = memo_branch_plan(true);
    const Node* branch_node = find_kind(plan->root, Node::If);
    check(branch_node && !branch_node->trace && plan->trace_count == 0,
          "parameter-dependent branch is not traced");
    auto escaping = traced_while_plan(true);
    const Node* loop = find_kind(escaping->root, Node::While);
    const Node* exit = find_kind(escaping->root, Node::If);
    check(loop && !loop->trace && exit && !exit->trace &&
              escaping->trace_count == 0,
          "while with a parameter-dependent break is not traced");
    Executor executor(outer(escaping, {1, 1}, {1}));
    const Evaluation full = evaluate_memo(executor, .5, 3);
    close(full.value, .0625, "untraced while full value");
    close(full.gradient[0], .5, "untraced while full gradient");
    const Evaluation early = evaluate_memo(executor, 3, 3);
    close(early.value, 27, "untraced while breaks on the second evaluation");
    close(early.gradient[0], 27, "untraced while early gradient");
  }
  {
    auto plan = traced_branch_plan(true);
    const Node* guard = find_memo(plan->root);
    const Node* branch_node = find_kind(plan->root, Node::If);
    check(guard && guard->memo_outs.size() == 1 && branch_node &&
              branch_node->trace &&
              guard->memo_outs[0] == branch_node->condition,
          "guard read by an active kernel stays a live-out");
    memo_index_calls = memo_compare_calls = 0;
    Executor executor(outer(plan, {1, 1}, {3}));
    const Evaluation first = evaluate_traced(executor, .25, {1, 0, 2});
    close(first.value, 1.5, "live guard first value");
    close(first.gradient[0], 6, "live guard first gradient");
    const Evaluation second = evaluate_traced(executor, .5, {1, 0, 2});
    close(second.value, 3, "live guard replayed value");
    close(second.gradient[0], 6, "live guard replayed gradient");
    check(traced_arm_iterators == std::vector<double>{1, 3},
          "live guard replays the recorded arms");
    check(memo_index_calls == 3 && memo_compare_calls == 3,
          "live guard replays without guard kernel calls");
  }
  {
    const Graph graph = outer(traced_branch_plan(false), {1, 1}, {3});
    Executor first(graph), second(graph);
    memo_index_calls = memo_compare_calls = 0;
    const Evaluation a = evaluate_traced(first, .25, {1, 0, 2});
    close(a.value, 1, "first executor traced value");
    const Evaluation b = evaluate_traced(second, .25, {0, 1, 0});
    close(b.value, .5, "second executor traced value");
    close(b.gradient[0], 2, "second executor traced gradient");
    check(traced_arm_iterators == std::vector<double>{2},
          "second executor records its own arms");
    check(memo_index_calls == 6 && memo_compare_calls == 6,
          "each executor records its own trace");
    const Evaluation a2 = evaluate_traced(first, .5, {1, 0, 2});
    close(a2.value, 2, "first executor replayed value");
    check(traced_arm_iterators == std::vector<double>{1, 3},
          "first executor replays its own arms");
    const Evaluation b2 = evaluate_traced(second, .5, {0, 1, 0});
    close(b2.value, 1, "second executor replayed value");
    check(traced_arm_iterators == std::vector<double>{2},
          "second executor replays its own arms");
    check(memo_index_calls == 6 && memo_compare_calls == 6,
          "executors replay independently");
  }
}

// theta * sum of the iterators whose table entry is positive over `rows`
// rows; `param_guard` also adds theta per row while theta > 0.
static std::shared_ptr<StructuredLoop> row_scan_plan(int rows,
                                                     bool param_guard) {
  auto plan = std::make_shared<StructuredLoop>();
  const int theta = plan->body.add_slot(1, false);
  const int table = plan->body.add_slot(rows, false);
  const int one = scalar(*plan, 1);
  const int last = scalar(*plan, rows);
  const int zero = scalar(*plan, 0);
  const int iterator = plan->body.add_slot(1, false);
  const int picked = plan->body.add_slot(1, false);
  const int guard = plan->body.add_slot(1, false);
  const int term = plan->body.add_slot(1, false);
  const int acc = plan->body.add_slot(1, false);
  plan->fills.push_back({acc, {0}});
  const int next = plan->body.add_slot(1, false);
  plan->imports = {{theta, 0, 0, true, false}, {table, 2, 0, false, true}};
  Node index = call(*plan, OP_INDEX_DYNAMIC, {table, iterator}, picked);
  attach(*plan, index.op, single_spec(rows));
  const int index_op = index.op;
  Node compare = call(*plan, OP_COMPARE, {picked, zero}, guard);
  plan->body.ops[static_cast<size_t>(compare.op)].variant = 2;
  const int compare_op = compare.op;
  Node scale = call(*plan, OP_MUL, {theta, iterator}, term);
  const int scale_op = scale.op;
  std::vector<Node> body;
  body.push_back(std::move(index));
  body.push_back(std::move(compare));
  body.push_back(
      branch(guard,
             sequence({std::move(scale), call(*plan, OP_ADD, {acc, term}, next),
                       alias(acc, next)}),
             sequence({})));
  if (param_guard) {
    const int positive = plan->body.add_slot(1, false);
    const int bumped = plan->body.add_slot(1, false);
    Node sign = call(*plan, OP_COMPARE, {theta, zero}, positive);
    plan->body.ops[static_cast<size_t>(sign.op)].variant = 2;
    body.push_back(std::move(sign));
    body.push_back(branch(positive,
                          sequence({call(*plan, OP_ADD, {acc, theta}, bumped),
                                    alias(acc, bumped)}),
                          sequence({})));
  }
  plan->root = counted(one, last, iterator, sequence(std::move(body)));
  plan->outputs = {acc};
  prepare_kernels(*plan);
  check(set_forward(plan->root, index_op, count_memo_index) &&
            set_forward(plan->root, compare_op, count_memo_compare) &&
            set_forward(plan->root, scale_op, record_arm_mul),
        "find row scan callbacks");
  return plan;
}

// theta * sum of k over the cells (k, i) of a 3 x 100 table holding a
// positive entry, scanned by nested loops through a data-only flat index.
static std::shared_ptr<StructuredLoop> nested_scan_plan() {
  auto plan = std::make_shared<StructuredLoop>();
  const int theta = plan->body.add_slot(1, false);
  const int table = plan->body.add_slot(300, false);
  const int one = scalar(*plan, 1);
  const int three = scalar(*plan, 3);
  const int hundred = scalar(*plan, 100);
  const int zero = scalar(*plan, 0);
  const int k = plan->body.add_slot(1, false);
  const int i = plan->body.add_slot(1, false);
  const int previous = plan->body.add_slot(1, false);
  const int base = plan->body.add_slot(1, false);
  const int flat = plan->body.add_slot(1, false);
  const int picked = plan->body.add_slot(1, false);
  const int guard = plan->body.add_slot(1, false);
  const int term = plan->body.add_slot(1, false);
  const int acc = plan->body.add_slot(1, false);
  plan->fills.push_back({acc, {0}});
  const int next = plan->body.add_slot(1, false);
  plan->imports = {{theta, 0, 0, true, false}, {table, 2, 0, false, true}};
  Node decrement = call(*plan, OP_INT_ARITH, {k, one}, previous);
  plan->body.ops[static_cast<size_t>(decrement.op)].variant = 1;
  Node stride = call(*plan, OP_INT_ARITH, {previous, hundred}, base);
  plan->body.ops[static_cast<size_t>(stride.op)].variant = 2;
  Node offset = call(*plan, OP_INT_ARITH, {base, i}, flat);
  Node index = call(*plan, OP_INDEX_DYNAMIC, {table, flat}, picked);
  attach(*plan, index.op, single_spec(300));
  const int index_op = index.op;
  Node compare = call(*plan, OP_COMPARE, {picked, zero}, guard);
  plan->body.ops[static_cast<size_t>(compare.op)].variant = 2;
  Node scale = call(*plan, OP_MUL, {theta, k}, term);
  const int scale_op = scale.op;
  Node inner =
      counted(one, hundred, i,
              sequence({std::move(decrement), std::move(stride),
                        std::move(offset), std::move(index), std::move(compare),
                        branch(guard,
                               sequence({std::move(scale),
                                         call(*plan, OP_ADD, {acc, term}, next),
                                         alias(acc, next)}),
                               sequence({}))}));
  plan->root = counted(one, three, k, sequence({std::move(inner)}));
  plan->outputs = {acc};
  prepare_kernels(*plan);
  check(set_forward(plan->root, index_op, count_memo_index) &&
            set_forward(plan->root, scale_op, record_arm_mul),
        "find nested scan callbacks");
  return plan;
}

// theta * sum of the iterators before the first row whose entry is positive.
static std::shared_ptr<StructuredLoop> break_scan_plan(int rows) {
  auto plan = std::make_shared<StructuredLoop>();
  const int theta = plan->body.add_slot(1, false);
  const int table = plan->body.add_slot(rows, false);
  const int one = scalar(*plan, 1);
  const int last = scalar(*plan, rows);
  const int zero = scalar(*plan, 0);
  const int iterator = plan->body.add_slot(1, false);
  const int picked = plan->body.add_slot(1, false);
  const int guard = plan->body.add_slot(1, false);
  const int term = plan->body.add_slot(1, false);
  const int acc = plan->body.add_slot(1, false);
  plan->fills.push_back({acc, {0}});
  const int next = plan->body.add_slot(1, false);
  plan->imports = {{theta, 0, 0, true, false}, {table, 2, 0, false, true}};
  Node index = call(*plan, OP_INDEX_DYNAMIC, {table, iterator}, picked);
  attach(*plan, index.op, single_spec(rows));
  const int index_op = index.op;
  Node compare = call(*plan, OP_COMPARE, {picked, zero}, guard);
  plan->body.ops[static_cast<size_t>(compare.op)].variant = 2;
  Node scale = call(*plan, OP_MUL, {theta, iterator}, term);
  const int scale_op = scale.op;
  Node exit;
  exit.kind = Node::Break;
  plan->root = counted(
      one, last, iterator,
      sequence({std::move(index), std::move(compare),
                branch(guard, std::move(exit), sequence({})), std::move(scale),
                call(*plan, OP_ADD, {acc, term}, next), alias(acc, next)}));
  plan->outputs = {acc};
  prepare_kernels(*plan);
  check(set_forward(plan->root, index_op, count_memo_index) &&
            set_forward(plan->root, scale_op, record_arm_mul),
        "find break scan callbacks");
  return plan;
}

static Evaluation evaluate_rows(Executor& executor, double theta,
                                const std::vector<double>& table) {
  std::copy(table.begin(), table.end(), executor.value_ptr(2));
  traced_arm_iterators.clear();
  return evaluate(executor, theta, 0);
}

static std::vector<double> sparse_rows(int rows, std::vector<int> positive) {
  std::vector<double> table(static_cast<size_t>(rows), 0.0);
  for (int row : positive) table[static_cast<size_t>(row - 1)] = 1;
  return table;
}

static size_t reported_field(const std::string& diagnostics,
                             const std::string& name) {
  const size_t at = diagnostics.find(name);
  return at == std::string::npos
             ? std::numeric_limits<size_t>::max()
             : std::stoul(diagnostics.substr(at + name.size()));
}

static size_t reported_visits(const std::string& diagnostics) {
  return reported_field(diagnostics, "visits=");
}

// Replays under STANLI_STRUCTURED_LOOP_DIAGNOSTICS and returns the node
// visits the tape line reports for that evaluation.
static size_t replay_visits(const std::function<Evaluation()>& replay,
                            Evaluation& result) {
  stanli_test::StdoutCapture captured(stderr);
  result = replay();
  return reported_visits(captured.finish());
}

static Executor diagnosed_executor(Graph graph) {
  test_setenv("STANLI_STRUCTURED_LOOP_DIAGNOSTICS", "1");
  Executor executor(std::move(graph));
  test_unsetenv("STANLI_STRUCTURED_LOOP_DIAGNOSTICS");
  return executor;
}

// A data-only scan whose values leave nothing behind runs once, during the
// recording evaluation, and is skipped afterwards. What it allocated has no
// reader once it exits.
static void memo_release_tests() {
  auto plan = std::make_shared<StructuredLoop>();
  const int theta = plan->body.add_slot(1, false);
  const int one = scalar(*plan, 1);
  const int rows = scalar(*plan, 200);
  const int columns = scalar(*plan, 50);
  const int row = plan->body.add_slot(1, false);
  const int column = plan->body.add_slot(1, false);
  const int seen = plan->body.add_slot(1, false);
  const int acc = plan->body.add_slot(1, false);
  plan->fills.push_back({acc, {0}});
  const int next = plan->body.add_slot(1, false);
  plan->imports = {{theta, 0, 0, true, false}};
  const int half = scalar(*plan, 100);
  const int guard = plan->body.add_slot(1, false);
  Node scan = counted(one, columns, column, sequence({alias(seen, column)}));
  Node compare = call(*plan, OP_COMPARE, {row, half}, guard);
  plan->body.ops[static_cast<size_t>(compare.op)].variant = 2;
  plan->root = counted(
      one, rows, row,
      sequence({std::move(scan), std::move(compare),
                branch(guard,
                       sequence({call(*plan, OP_ADD, {acc, theta}, next),
                                 alias(acc, next)}),
                       sequence({}))}));
  plan->outputs = {acc};
  prepare_kernels(*plan);
  check(plan->memo_count == 1, "the data-only scan is one memo node");
  check(plan->root.children[0].children[0].memo_keep.size() == 1,
        "the guard the traced branch reads survives the scan");

  Executor executor = diagnosed_executor(outer(plan, {1, 1}));
  const Evaluation first = evaluate(executor, .25, 0);
  close(first.value, 25, "scan recording value");
  close(first.gradient[0], 100, "scan recording gradient");
  Evaluation second;
  std::string diagnostics;
  {
    stanli_test::StdoutCapture captured(stderr);
    second = evaluate(executor, .5, 0);
    diagnostics = captured.finish();
  }
  close(second.value, 50, "scan replayed value");
  close(second.gradient[0], 100, "scan replayed gradient");
  check(reported_field(diagnostics, "record_versions=") < 500,
        "the recording evaluation keeps one scan of versions");
  check(reported_field(diagnostics, "record_arena=") < 500,
        "the recording evaluation keeps one scan of storage");
}

static void for_trace_tests() {
  {
    auto plan = row_scan_plan(1000, false);
    check(plan->root.trace && plan->trace_count == 2,
          "data-only row scan is a traced for");
    const std::vector<double> table = sparse_rows(1000, {7, 500, 1000});
    memo_index_calls = memo_compare_calls = 0;
    Executor executor = diagnosed_executor(outer(plan, {1, 1}, {1000}));
    const Evaluation first = evaluate_rows(executor, .25, table);
    close(first.value, 1507 * .25, "row scan first value");
    close(first.gradient[0], 1507, "row scan first gradient");
    check(traced_arm_iterators == std::vector<double>{7, 500, 1000} &&
              memo_index_calls == 1000 && memo_compare_calls == 1000,
          "row scan records every row once");
    Evaluation second;
    const size_t visits = replay_visits(
        [&] { return evaluate_rows(executor, .5, table); }, second);
    close(second.value, 1507 * .5, "row scan replayed value");
    close(second.gradient[0], 1507, "row scan replayed gradient");
    check(traced_arm_iterators == std::vector<double>{7, 500, 1000},
          "row scan replays only the effective rows");
    check(memo_index_calls == 1000 && memo_compare_calls == 1000,
          "row scan replays without guard kernel calls");
    check(visits < 40, "row scan replay visits only the effective rows");
  }
  {
    auto plan = row_scan_plan(1000, true);
    check(!plan->root.trace && plan->trace_count == 1,
          "row scan with a parameter branch is not a traced for");
    const std::vector<double> table = sparse_rows(1000, {7, 500, 1000});
    Executor executor = diagnosed_executor(outer(plan, {1, 1}, {1000}));
    const Evaluation first = evaluate_rows(executor, -.5, table);
    close(first.value, 1507 * -.5, "parameter branch first value");
    close(first.gradient[0], 1507, "parameter branch first gradient");
    Evaluation second;
    const size_t visits = replay_visits(
        [&] { return evaluate_rows(executor, .25, table); }, second);
    close(second.value, 2507 * .25, "parameter branch switched value");
    close(second.gradient[0], 2507, "parameter branch switched gradient");
    check(traced_arm_iterators == std::vector<double>{7, 500, 1000},
          "data branch still replays under an untraced for");
    check(visits >= 1000, "untraced for visits every row on replay");
  }
  {
    auto plan = nested_scan_plan();
    const Node& inner = plan->root.children[0].children[0];
    check(plan->root.trace && inner.kind == Node::For && inner.trace &&
              plan->trace_count == 3,
          "nested data-only scans are both traced");
    const std::vector<double> table = sparse_rows(300, {5, 250});
    memo_index_calls = 0;
    Executor executor = diagnosed_executor(outer(plan, {1, 1}, {300}));
    const Evaluation first = evaluate_rows(executor, .25, table);
    close(first.value, 1, "nested scan first value");
    close(first.gradient[0], 4, "nested scan first gradient");
    check(traced_arm_iterators == std::vector<double>{1, 3} &&
              memo_index_calls == 300,
          "nested scan records every cell once");
    Evaluation second;
    const size_t visits = replay_visits(
        [&] { return evaluate_rows(executor, .5, table); }, second);
    close(second.value, 2, "nested scan replayed value");
    close(second.gradient[0], 4, "nested scan replayed gradient");
    check(traced_arm_iterators == std::vector<double>{1, 3} &&
              memo_index_calls == 300,
          "nested scan replays only the effective cells");
    check(visits < 40, "nested scan replay skips empty outer iterations");
  }
  {
    auto plan = break_scan_plan(100);
    check(plan->root.trace && plan->trace_count == 2,
          "scan with a data-only break is a traced for");
    const std::vector<double> table = sparse_rows(100, {5});
    memo_index_calls = 0;
    Executor executor = diagnosed_executor(outer(plan, {1, 1}, {100}));
    const Evaluation first = evaluate_rows(executor, .25, table);
    close(first.value, 2.5, "break scan first value");
    close(first.gradient[0], 10, "break scan first gradient");
    check(traced_arm_iterators == std::vector<double>{1, 2, 3, 4} &&
              memo_index_calls == 5,
          "break scan stops at the breaking row");
    Evaluation second;
    const size_t visits = replay_visits(
        [&] { return evaluate_rows(executor, .5, table); }, second);
    close(second.value, 5, "break scan replayed value");
    close(second.gradient[0], 10, "break scan replayed gradient");
    check(traced_arm_iterators == std::vector<double>{1, 2, 3, 4} &&
              memo_index_calls == 5,
          "break scan replays through the breaking row");
    check(visits < 60, "break scan replay visits rows up to the break");
  }
  {
    const Graph graph = outer(row_scan_plan(1000, false), {1, 1}, {1000});
    Executor first(graph), second(graph);
    const std::vector<double> a = sparse_rows(1000, {7, 500, 1000});
    const std::vector<double> b = sparse_rows(1000, {1, 2});
    memo_index_calls = 0;
    close(evaluate_rows(first, .25, a).value, 1507 * .25,
          "first executor row scan value");
    close(evaluate_rows(second, .25, b).value, 3 * .25,
          "second executor row scan value");
    check(memo_index_calls == 2000, "each executor records its own rows");
    close(evaluate_rows(first, .5, a).value, 1507 * .5,
          "first executor replayed row scan value");
    check(traced_arm_iterators == std::vector<double>{7, 500, 1000},
          "first executor replays its own rows");
    close(evaluate_rows(second, .5, b).value, 3 * .5,
          "second executor replayed row scan value");
    check(traced_arm_iterators == std::vector<double>{1, 2},
          "second executor replays its own rows");
    check(memo_index_calls == 2000, "executors replay rows independently");
  }
}

static size_t count_kind(const Node& node, Node::Kind kind) {
  size_t total = node.kind == kind ? 1 : 0;
  for (const auto& child : node.children) total += count_kind(child, kind);
  return total;
}

static bool binds_slot(const std::vector<SegmentBinding>& bindings, int slot) {
  return std::any_of(bindings.begin(), bindings.end(),
                     [&](const SegmentBinding& b) { return b.slot == slot; });
}

template <class Build>
static void compare_segmented(Build build, const char* name,
                              std::vector<int64_t> param_lens = {1, 1}) {
  auto segmented = build();
  auto plain = unsegmented(build);
  Executor a(outer(segmented, param_lens)), b(outer(plain, param_lens));
  int64_t n = 0;
  for (int64_t len : param_lens) n += len;
  std::vector<double> ga(static_cast<size_t>(n)), gb(ga);
  for (double seed : {.1, -.2}) {
    for (int64_t i = 0; i < n; ++i)
      a.params_data()[i] = b.params_data()[i] =
          seed + .3 * static_cast<double>(i);
    close(a.gradient(ga.data()), b.gradient(gb.data()), name);
    for (size_t i = 0; i < ga.size(); ++i) close(ga[i], gb[i], name);
  }
}

// acc = sum_i sum(set_index(x, 1, acc * theta)) through one CALL per trip.
static std::shared_ptr<StructuredLoop> call_segment_plan() {
  auto plan = std::make_shared<StructuredLoop>();
  const int theta = plan->body.add_slot(1, false);
  const int beta = plan->body.add_slot(1, false);
  const int lower = scalar(*plan, 1);
  const int upper = scalar(*plan, 4);
  const int iterator = plan->body.add_slot(1, false);
  const int x = plan->body.add_slot(3, false);
  plan->fills.push_back({x, {1, 2, 3}});
  const int acc = plan->body.add_slot(1, false);
  const int scaled = plan->body.add_slot(1, false);
  const int updated = plan->body.add_slot(3, false);
  const int total = plan->body.add_slot(1, false);
  plan->imports = {{theta, 0, 0, true}, {beta, 1, 0, true}};
  Node set_index;
  set_index.kind = Node::KernelCall;
  set_index.op = plan->body.add_op(OP_SET_INDEX, {x, scaled}, updated, {1});
  plan->root =
      sequence({alias(acc, beta),
                counted(lower, upper, iterator,
                        sequence({call(*plan, OP_MUL, {acc, theta}, scaled),
                                  std::move(set_index),
                                  call(*plan, OP_SUM_VEC, {updated}, total),
                                  alias(acc, total), alias(x, updated)}))});
  plan->outputs = {acc};
  plan->prepare();
  return plan;
}

// x = acc * beta is read after the loop; acc = x + theta carries.
static std::shared_ptr<StructuredLoop> alias_out_plan() {
  auto plan = std::make_shared<StructuredLoop>();
  const int theta = plan->body.add_slot(1, false);
  const int beta = plan->body.add_slot(1, false);
  const int lower = scalar(*plan, 1);
  const int upper = scalar(*plan, 3);
  const int iterator = plan->body.add_slot(1, false);
  const int acc = plan->body.add_slot(1, false);
  plan->fills.push_back({acc, {.5}});
  const int x = plan->body.add_slot(1, false);
  const int product = plan->body.add_slot(1, false);
  const int sum = plan->body.add_slot(1, false);
  plan->imports = {{theta, 0, 0, true}, {beta, 1, 0, true}};
  plan->root = counted(
      lower, upper, iterator,
      sequence({call(*plan, OP_MUL, {acc, beta}, product), alias(x, product),
                call(*plan, OP_ADD, {x, theta}, sum), alias(acc, sum)}));
  plan->outputs = {acc, x};
  plan->prepare();
  return plan;
}

// target += s * beta + theta with s carried only through its own cell.
static std::shared_ptr<StructuredLoop> carried_plan() {
  auto plan = std::make_shared<StructuredLoop>();
  const int theta = plan->body.add_slot(1, false);
  const int beta = plan->body.add_slot(1, false);
  const int lower = scalar(*plan, 1);
  const int upper = scalar(*plan, 3);
  const int iterator = plan->body.add_slot(1, false);
  const int s = plan->body.add_slot(1, false);
  plan->fills.push_back({s, {.25}});
  const int product = plan->body.add_slot(1, false);
  const int sum = plan->body.add_slot(1, false);
  plan->imports = {{theta, 0, 0, true}, {beta, 1, 0, true}};
  plan->has_target = true;
  plan->root = counted(lower, upper, iterator,
                       sequence({call(*plan, OP_MUL, {s, beta}, product),
                                 call(*plan, OP_ADD, {product, theta}, sum),
                                 alias(s, sum), target(sum)}));
  plan->prepare();
  return plan;
}

// base[i] = theta * i + 1 in place between two scalar runs.
static std::shared_ptr<StructuredLoop> split_update_plan() {
  auto plan = std::make_shared<StructuredLoop>();
  const int theta = plan->body.add_slot(1, false);
  const int beta = plan->body.add_slot(1, false);
  const int lower = scalar(*plan, 1);
  const int upper = scalar(*plan, 3);
  const int one = scalar(*plan, 1);
  const int iterator = plan->body.add_slot(1, false);
  const int base = plan->body.add_slot(3, false);
  plan->fills.push_back({base, {1, 2, 3}});
  const int acc = plan->body.add_slot(1, false);
  const int rhs = plan->body.add_slot(1, false);
  const int shifted = plan->body.add_slot(1, false);
  const int updated = plan->body.add_slot(3, false);
  const int sum = plan->body.add_slot(1, false);
  const int scaled = plan->body.add_slot(1, false);
  plan->imports = {{theta, 0, 0, true}, {beta, 1, 0, true}};
  Node update =
      call(*plan, OP_SET_INDEX_DYNAMIC, {base, iterator, shifted}, updated);
  attach(*plan, update.op, single_spec(3));
  plan->root =
      sequence({alias(acc, beta),
                counted(lower, upper, iterator,
                        sequence({call(*plan, OP_MUL, {theta, iterator}, rhs),
                                  call(*plan, OP_ADD, {rhs, one}, shifted),
                                  std::move(update), alias(base, updated),
                                  call(*plan, OP_SUM_VEC, {base}, sum),
                                  call(*plan, OP_MUL, {sum, acc}, scaled),
                                  alias(acc, scaled)}))});
  plan->outputs = {acc};
  plan->prepare();
  return plan;
}

static std::shared_ptr<StructuredLoop> lone_kernel_plan() {
  auto plan = std::make_shared<StructuredLoop>();
  const int theta = plan->body.add_slot(1, false);
  const int lower = scalar(*plan, 1);
  const int upper = scalar(*plan, 3);
  const int iterator = plan->body.add_slot(1, false);
  const int acc = plan->body.add_slot(1, false);
  const int product = plan->body.add_slot(1, false);
  plan->imports = {{theta, 0, 0, true}};
  plan->root = counted(lower, upper, iterator,
                       sequence({call(*plan, OP_MUL, {theta, acc}, product),
                                 alias(acc, product)}));
  plan->fills.push_back({acc, {1}});
  plan->outputs = {acc};
  plan->prepare();
  return plan;
}

// acc = 2 * check_lower(acc * beta + theta, -10): the check runs every trip.
static std::shared_ptr<StructuredLoop> effect_plan() {
  auto plan = std::make_shared<StructuredLoop>();
  const int theta = plan->body.add_slot(1, false);
  const int beta = plan->body.add_slot(1, false);
  const int lower = scalar(*plan, 1);
  const int upper = scalar(*plan, 3);
  const int bound = scalar(*plan, -10);
  const int two = scalar(*plan, 2);
  const int iterator = plan->body.add_slot(1, false);
  const int acc = plan->body.add_slot(1, false);
  plan->fills.push_back({acc, {1}});
  const int product = plan->body.add_slot(1, false);
  const int sum = plan->body.add_slot(1, false);
  const int checked = plan->body.add_slot(1, false);
  const int doubled = plan->body.add_slot(1, false);
  plan->imports = {{theta, 0, 0, true}, {beta, 1, 0, true}};
  Node check_node = call(*plan, OP_CHECK_LOWER, {sum, bound}, checked);
  auto spec = std::make_shared<BoundCheckSpec>();
  spec->name = "sum";
  spec->bound_is_scalar = true;
  spec->shapes_match = true;
  plan->body.ops[static_cast<size_t>(check_node.op)].udata = spec.get();
  plan->body.udata_pool.push_back(std::move(spec));
  plan->root = counted(
      lower, upper, iterator,
      sequence({call(*plan, OP_MUL, {acc, beta}, product),
                call(*plan, OP_ADD, {product, theta}, sum),
                std::move(check_node), call(*plan, OP_MUL, {sum, two}, doubled),
                alias(acc, doubled)}));
  plan->outputs = {acc};
  plan->prepare();
  return plan;
}

// A dynamic index (outside the register vocabulary) between two scalar ops.
static std::shared_ptr<StructuredLoop> foreign_op_plan() {
  auto plan = std::make_shared<StructuredLoop>();
  const int theta = plan->body.add_slot(1, false);
  const int beta = plan->body.add_slot(1, false);
  const int lower = scalar(*plan, 1);
  const int upper = scalar(*plan, 3);
  const int iterator = plan->body.add_slot(1, false);
  const int table = plan->body.add_slot(3, false);
  plan->fills.push_back({table, {2, 3, 5}});
  const int acc = plan->body.add_slot(1, false);
  plan->fills.push_back({acc, {1}});
  const int product = plan->body.add_slot(1, false);
  const int picked = plan->body.add_slot(1, false);
  const int sum = plan->body.add_slot(1, false);
  plan->imports = {{theta, 0, 0, true}, {beta, 1, 0, true}};
  Node index = call(*plan, OP_INDEX_DYNAMIC, {table, iterator}, picked);
  attach(*plan, index.op, single_spec(3));
  plan->root = counted(
      lower, upper, iterator,
      sequence({call(*plan, OP_MUL, {acc, beta}, product), std::move(index),
                call(*plan, OP_ADD, {product, picked}, sum), alias(acc, sum)}));
  plan->outputs = {acc};
  plan->prepare();
  return plan;
}

// A comparison with no backward feeds an active multiply.
static std::shared_ptr<StructuredLoop> no_backward_plan() {
  auto plan = std::make_shared<StructuredLoop>();
  const int theta = plan->body.add_slot(1, false);
  const int beta = plan->body.add_slot(1, false);
  const int lower = scalar(*plan, 1);
  const int upper = scalar(*plan, 3);
  const int iterator = plan->body.add_slot(1, false);
  const int acc = plan->body.add_slot(1, false);
  plan->fills.push_back({acc, {1}});
  const int product = plan->body.add_slot(1, false);
  const int positive = plan->body.add_slot(1, false);
  const int gated = plan->body.add_slot(1, false);
  const int sum = plan->body.add_slot(1, false);
  plan->imports = {{theta, 0, 0, true}, {beta, 1, 0, true}};
  Node compare = call(*plan, OP_COMPARE, {product, theta}, positive);
  plan->body.ops[static_cast<size_t>(compare.op)].variant = 2;
  plan->root = counted(
      lower, upper, iterator,
      sequence({call(*plan, OP_MUL, {acc, beta}, product), std::move(compare),
                call(*plan, OP_MUL, {positive, theta}, gated),
                call(*plan, OP_ADD, {product, gated}, sum), alias(acc, sum)}));
  plan->outputs = {acc};
  plan->prepare();
  return plan;
}

static void segment_tests() {
  {
    auto plan = recurrence(8);
    check(plan->segments.size() == 1, "scalar recurrence forms one segment");
    check(count_kind(plan->root, Node::Segment) == 1 &&
              count_kind(plan->root, Node::KernelCall) == 0 &&
              count_kind(plan->root, Node::Alias) == 1,
          "recurrence body is one segment node");
    const Node* loop = find_kind(plan->root, Node::For);
    check(loop && loop->children[0].kind == Node::Segment,
          "segment node replaces the loop body");
    if (!plan->segments.empty()) {
      const Segment& segment = plan->segments[0];
      check(segment.program.calls.empty(),
            "scalar recurrence uses register instructions only");
      check(segment.ins.size() == 3 && segment.outs.size() == 1 &&
                binds_slot(segment.outs, 2),
            "recurrence segment binds three live-ins and the carried cell");
      check(!segment.program.adj.empty() && segment.program.native_adj,
            "recurrence segment carries a generated adjoint");
    }
    compare_segmented([] { return recurrence(8); },
                      "segmented recurrence matches the kernel executor");
    compare_segmented([] { return recurrence(0); },
                      "segmented zero-trip recurrence matches");
  }
  {
    auto plan = call_segment_plan();
    check(plan->segments.size() == 1, "call run forms one segment");
    if (!plan->segments.empty()) {
      const Segment& segment = plan->segments[0];
      check(segment.program.calls.size() == 1,
            "vector kernel inside the run is a CALL");
      check(binds_slot(segment.outs, 5) && binds_slot(segment.outs, 6),
            "call segment publishes both aliased cells");
    }
    compare_segmented(call_segment_plan,
                      "segment with a CALL matches the kernel executor");
  }
  {
    auto plan = alias_out_plan();
    check(plan->segments.size() == 1, "aliased run forms one segment");
    if (!plan->segments.empty()) {
      const Segment& segment = plan->segments[0];
      check(binds_slot(segment.outs, 6),
            "alias read after the loop is a live-out");
      check(segment.outs.size() == 2, "alias run publishes two cells");
    }
    compare_segmented(alias_out_plan,
                      "aliased live-out matches the kernel executor");
  }
  {
    auto plan = carried_plan();
    check(plan->segments.size() == 1, "carried run forms one segment");
    if (!plan->segments.empty()) {
      const Segment& segment = plan->segments[0];
      check(binds_slot(segment.outs, 5), "loop-carried cell is a live-out");
      check(binds_slot(segment.ins, 5), "loop-carried cell is a live-in");
    }
    compare_segmented(carried_plan,
                      "loop-carried live-out matches the kernel executor");
  }
  {
    auto plan = split_update_plan();
    check(plan->segments.size() == 2, "in-place update splits the run");
    const Node* update = find_kind(plan->root, Node::KernelCall);
    check(update && update->storage == Node::InPlace &&
              count_kind(plan->root, Node::KernelCall) == 1,
          "in-place update stays a kernel call between segments");
    compare_segmented(split_update_plan,
                      "segments around an update match the kernel executor");
  }
  {
    auto plan = lone_kernel_plan();
    check(
        plan->segments.empty() && count_kind(plan->root, Node::KernelCall) == 1,
        "a lone kernel stays a kernel call");
  }
  {
    auto plan = effect_plan();
    check(plan->segments.size() == 1, "run before an effect is a segment");
    const Node* effect = find_kind(plan->root, Node::KernelCall);
    check(effect && plan->body.ops[effect->op].opcode == OP_CHECK_LOWER &&
              count_kind(plan->root, Node::KernelCall) == 2,
          "effect kernel stays a kernel call");
    compare_segmented(effect_plan, "effect run matches the kernel executor");
    Executor executor(outer(plan));
    bool threw = false;
    try {
      evaluate(executor, -50, .5);
    } catch (const std::domain_error&) {
      threw = true;
    }
    check(threw, "effect kernel still runs inside the loop");
  }
  {
    auto plan = foreign_op_plan();
    check(
        plan->segments.empty() && count_kind(plan->root, Node::KernelCall) == 3,
        "op outside the vocabulary leaves the run as kernel calls");
    compare_segmented(foreign_op_plan, "unsegmented run matches itself");
  }
  {
    auto plan = no_backward_plan();
    check(plan->segments.size() == 1,
          "kernel without a backward joins the segment");
    compare_segmented(no_backward_plan,
                      "no-backward kernel drops its adjoint identically");
    Executor executor(outer(plan));
    const Evaluation result = evaluate(executor, .5, 2);
    close(result.value, 11.5, "no-backward gated value");
    close(result.gradient[0], 7, "no-backward gated theta gradient");
    close(result.gradient[1], 14.5, "no-backward gated beta gradient");
  }
  {
    auto plan = recurrence(3);
    Executor executor(outer(plan));
    const Evaluation first = evaluate(executor, .1, .7);
    const Evaluation again = evaluate(executor, .1, .7);
    check(std::memcmp(&first, &again, sizeof(Evaluation)) == 0,
          "segmented evaluation is bitwise repeatable");
  }
}

int main() {
  test_unsetenv("STANLI_STRUCTURED_LOOP_DIAGNOSTICS");
  test_unsetenv("STANLI_NO_STRUCTURED_SEGMENTS");
  segment_tests();
  transient_classification_tests();
  invariant_active_reuse_tests();
  inplace_import_base_tests();
  inplace_promotion_tests();
  inplace_duplicate_position_tests();
  inplace_lifo_undo_tests();
  runtime_trip_tests();
  import_reference_tests();
  direct_index_kernel_tests();
  forced_control_tests();
  iterator_history_tests();
  primal_retention_tests();
  scalar_index_tests();
  integer_result_tests();
  loop_invariant_reuse_tests();
  control_tests();
  iterator_cell_tests();
  concurrency_tests();
  failure_tests();
  refusal_tests();
  automatic_policy_tests();
  prefer_parent_tests();
  direct_index_lowering_tests();
  runtime_slice_tests();
  runtime_index_tests();
  memo_tests();
  trace_tests();
  for_trace_tests();
  memo_release_tests();
  test_unsetenv("STANLI_STRUCTURED_LOOPS");
  test_unsetenv("STANLI_NO_STRUCTURED_DIRECT_INDEX_INPUTS");
  if (failures == 0) std::printf("test_structured_loop OK\n");
  return failures != 0;
}
