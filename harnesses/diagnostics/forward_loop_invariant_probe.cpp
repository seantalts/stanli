// Counterproposal probe: retained OP_LOOP versus its bounded no_reverse path
// in the same runtime/binary. This deliberately bypasses compiler folding to
// isolate the execution-policy cost of an admitted inactive loop invariant.
// It is not a Stan-source benchmark or a universal performance comparison.
//
// From the repository root, after building the existing shared test runtime:
// /usr/bin/c++ -std=c++17 -O3 -DNDEBUG -ffp-contract=off -Iruntime/include \
//   harnesses/diagnostics/forward_loop_invariant_probe.cpp tests/tbb_stub.cpp \
//   -Lbuild -lstanli_test_core -Wl,-rpath,"$PWD/build" \
//   -o build/forward_loop_invariant_probe
// build/forward_loop_invariant_probe > invariant-probe.jsonl
//
// The shared test runtime uses the exact runtime objects used by libstanli.
// It exposes the C++ Graph/Executor symbols needed here. No Stan Math headers,
// compiler, or new runtime build are required. Retain the linked build identity
// and this compile command with results. Run only in a controlled quiet window.
#include <stanli/graph.hpp>
#include <stanli/optable.hpp>
#include <stanli/structured_loop.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
using namespace stanli;
using Node = StructuredLoop::Node;
using Clock = std::chrono::steady_clock;
std::size_t cholesky_calls = 0;

double seconds(Clock::time_point begin) {
  return std::chrono::duration<double>(Clock::now() - begin).count();
}
void counted_cholesky(KernelCtx& context) {
  ++cholesky_calls;
  find_kernel(OP_CHOLESKY)->forward(context);
}
Node call(StructuredLoop& plan, uint16_t opcode,
          std::initializer_list<int> inputs, int output,
          std::initializer_list<int> idata = {}) {
  Node node;
  node.kind = Node::KernelCall;
  node.op = plan.body.add_op(opcode, inputs, output, idata);
  return node;
}
int fill(StructuredLoop& plan, std::vector<double> values) {
  const int slot =
      plan.body.add_slot(static_cast<int64_t>(values.size()), false);
  plan.fills.push_back({slot, std::move(values)});
  return slot;
}

Graph model(int n, int trips, bool overwrite) {
  auto plan = std::make_shared<StructuredLoop>();
  const int theta = plan->body.add_slot(1, false);
  std::vector<double> identity(static_cast<std::size_t>(n) * n, 0.0);
  for (int i = 0; i < n; ++i)
    identity[static_cast<std::size_t>(i) * n + i] = 4.0;
  const int matrix = fill(*plan, std::move(identity));
  const int zero =
      fill(*plan, std::vector<double>(static_cast<std::size_t>(n) * n));
  const int lower = fill(*plan, {1.0}), upper = fill(*plan, {double(trips)});
  const int iterator = plan->body.add_slot(1, false);
  const int factor = plan->body.add_slot(int64_t(n) * n, false);
  const int sum = plan->body.add_slot(1, false);
  const int scaled = plan->body.add_slot(1, false);
  plan->imports = {{theta, 0, 0, true}};
  Node cholesky = call(*plan, OP_CHOLESKY, {matrix}, factor, {n});
  cholesky.forward = counted_cholesky;
  Node target;
  target.kind = Node::Target;
  target.src = scaled;
  Node body;
  body.children = {std::move(cholesky), call(*plan, OP_SUM_VEC, {factor}, sum),
                   call(*plan, OP_MUL, {sum, theta}, scaled), target};
  if (overwrite) {
    Node reset;
    reset.kind = Node::Alias;
    reset.dst = factor;
    reset.src = zero;
    body.children.push_back(reset);
  }
  plan->root.kind = Node::For;
  plan->root.lower = lower;
  plan->root.upper = upper;
  plan->root.iterator = iterator;
  plan->root.children.push_back(std::move(body));
  plan->has_target = true;
  plan->prepare();
  const Node& prepared_cholesky = plan->root.children[0].children[0];
  if (prepared_cholesky.invariant_loop != plan->root.loop_index)
    throw std::logic_error(
        "probe Cholesky was not classified as loop-invariant");
  Graph graph;
  const int parameter = graph.add_slot(1, true);
  const int output = graph.add_slot(1, false);
  Op op;
  op.opcode = OP_LOOP;
  op.in[0] = parameter;
  op.n_in = 1;
  op.out = output;
  op.udata = plan.get();
  graph.ops.push_back(op);
  graph.udata_pool.push_back(std::move(plan));
  graph.result_slot = output;
  return graph;
}

struct Sample {
  double wall_seconds, value, expected;
  std::size_t calls;
  bool correct() const { return value == expected; }
};
Sample run(Executor& executor, bool no_reverse, int n, int trips,
           double theta) {
  executor.params_data()[0] = theta;
  EvalState state;
  state.no_reverse = no_reverse;
  cholesky_calls = 0;
  const auto begin = Clock::now();
  executor.run_forward_only(state);
  const double elapsed = seconds(begin);
  return {elapsed, executor.value_ptr(executor.graph().result_slot)[0],
          2.0 * n * trips * theta, cholesky_calls};
}
void emit(const char* engine, const char* phase, int repetition, double theta,
          const Sample& sample) {
  std::cout << "{\"engine\":\"" << engine << "\",\"phase\":\"" << phase
            << "\",\"repetition\":" << repetition << ",\"theta\":" << theta
            << ",\"wall_seconds\":" << sample.wall_seconds
            << ",\"cholesky_calls\":" << sample.calls
            << ",\"value\":" << sample.value
            << ",\"expected\":" << sample.expected
            << ",\"correct\":" << (sample.correct() ? "true" : "false") << '}';
}
double median(std::vector<double> values) {
  std::sort(values.begin(), values.end());
  return values[values.size() / 2];
}

bool probe(int n, int trips, bool overwrite) {
  const auto source_begin = Clock::now();
  const Graph graph = model(n, trips, overwrite);
  const double plan_seconds = seconds(source_begin);
  const auto retained_begin = Clock::now();
  Executor retained(graph);
  const double retained_bind_seconds = seconds(retained_begin);
  const auto bounded_begin = Clock::now();
  Executor bounded(graph);
  const double bounded_bind_seconds = seconds(bounded_begin);
  bool correct = true;
  const bool expected_bounded = n <= 1;
  std::cout << "{\"n\":" << n << ",\"trips\":" << trips
            << ",\"overwrite_factor\":" << (overwrite ? "true" : "false")
            << ",\"no_reverse_selected_path_expected\":\""
            << (expected_bounded ? "bounded" : "retained_fallback") << '"'
            << ",\"plan_seconds\":" << plan_seconds
            << ",\"retained_bind_seconds\":" << retained_bind_seconds
            << ",\"bounded_bind_seconds\":" << bounded_bind_seconds
            << ",\"samples\":[";
  bool comma = false;
  const auto record = [&](bool no_reverse, const char* phase, int repetition,
                          double theta) {
    const Sample sample =
        run(no_reverse ? bounded : retained, no_reverse, n, trips, theta);
    correct &= sample.correct();
    if (comma) std::cout << ',';
    comma = true;
    emit(no_reverse ? "no_reverse" : "retained", phase, repetition, theta,
         sample);
    return sample;
  };
  record(false, "first", 0, .25);
  record(true, "first", 0, .25);
  std::vector<double> retained_times, bounded_times;
  for (int repetition = 0; repetition < 9; ++repetition) {
    const double theta = repetition % 2 == 0 ? -.125 : .25;
    Sample retained_sample{}, bounded_sample{};
    if (repetition % 2 == 0) {
      bounded_sample = record(true, "warm", repetition, theta);
      retained_sample = record(false, "warm", repetition, theta);
    } else {
      retained_sample = record(false, "warm", repetition, theta);
      bounded_sample = record(true, "warm", repetition, theta);
    }
    correct &= expected_bounded ? bounded_sample.calls == 1
                                : bounded_sample.calls == retained_sample.calls;
    retained_times.push_back(retained_sample.wall_seconds);
    bounded_times.push_back(bounded_sample.wall_seconds);
  }
  std::cout << "],\"retained_warm_median_seconds\":" << median(retained_times)
            << ",\"bounded_warm_median_seconds\":" << median(bounded_times)
            << ",\"correct\":" << (correct ? "true" : "false") << "}\n";
  return correct;
}
}  // namespace

int main() {
  std::cout << std::setprecision(17);
  bool correct = true;
  for (int n : {17, 128})
    for (int trips : {3, 1000})
      for (bool overwrite : {false, true})
        correct &= probe(n, trips, overwrite);
  return correct ? 0 : 1;
}
