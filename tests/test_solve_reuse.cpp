// The same solve site executes with a different divisor on every iteration.
// Compare retained factors with recomputation across replay, frames and clones.
#include "env_helpers.hpp"
#include "stdout_capture.hpp"
#include <stanli/graph.hpp>
#include <stanli/island.hpp>
#include <stanli/optable.hpp>
#include <stanli/structured_loop.hpp>
#include <Eigen/Dense>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

using namespace stanli;
using Node = StructuredLoop::Node;
static Kernel original;
static int failures = 0;
static void check(bool ok, const char* message) {
  if (!ok) {
    ++failures;
    std::printf("FAIL %s\n", message);
  }
}
static void recompute_forward(KernelCtx& ctx) {
  KernelCtx copy = ctx;
  copy.scratch = nullptr;
  original.forward(copy);
}
static void recompute_backward(KernelCtx& ctx) {
  KernelCtx copy = ctx;
  copy.scratch = nullptr;
  original.backward(copy);
}

static Graph graph(int n, int k, int rows, bool vec) {
  auto plan = std::make_shared<StructuredLoop>();
  auto slot = [&](int len) { return plan->body.add_slot(len, false); };
  const int a = slot(n * n), b = slot(n * k), next = slot(n * n);
  const int delta = slot(n * n), solved = slot(n * k), total = slot(1);
  const int lo = slot(1), hi = slot(1), iterator = slot(1);
  const int first = slot(1), zero = slot(1), condition = slot(1);
  const int alternate = slot(n * n);
  std::vector<double> d(n * n, 0.0);
  for (int i = 0; i < n; ++i) d[i * (n + 1)] = 0.125;
  plan->fills = {{delta, d}, {lo, {1}}, {hi, {double(rows)}}, {zero, {0}}};
  plan->imports = {{a, 0, 0, true, false}, {b, 1, 0, true, false}};
  const auto call = [&](uint16_t opcode, std::initializer_list<int> ins,
                        int out, std::initializer_list<int> dims = {}) {
    Node node;
    node.kind = Node::KernelCall;
    node.op = plan->body.add_op(opcode, ins, out, dims);
    return node;
  };
  Node add = call(OP_ADD, {a, delta}, next);
  Node update;
  update.kind = Node::Alias;
  update.dst = a;
  update.src = next;
  Node read = call(OP_INDEX, {b}, first, {0});
  Node compare = call(OP_COMPARE, {first, zero}, condition);  // variant 0: <
  Node extra = call(OP_ADD, {a, delta}, alternate);
  Node extra_update = update;
  extra_update.src = alternate;
  Node yes;
  yes.children = {extra, extra_update};
  Node branch;
  branch.kind = Node::If;
  branch.condition = condition;
  branch.children = {yes, Node{}};
  Node solve = call(OP_MDIVIDE_LEFT, {a, b}, solved, {n, k});
  plan->body.ops[solve.op].variant = 13u | (vec ? 2u : 0u);
  Node sum = call(OP_SUM_VEC, {solved}, total);
  Node target;
  target.kind = Node::Target;
  target.src = total;
  Node body;
  body.children = {add, update, read, compare, branch, solve, sum, target};
  plan->root.kind = Node::For;
  plan->root.lower = lo;
  plan->root.upper = hi;
  plan->root.iterator = iterator;
  plan->root.children = {body};
  plan->has_target = true;
  plan->prepare();
  Graph outer;
  const int pa = outer.add_slot(n * n, true), pb = outer.add_slot(n * k, true);
  outer.result_slot = outer.add_slot(1, false);
  outer.add_op(OP_LOOP, {pa, pb}, outer.result_slot);
  outer.ops.back().udata = plan.get();
  outer.udata_pool.push_back(plan);
  return outer;
}

static std::vector<double> evaluate(Executor& ex, int n, int k, int point) {
  for (int j = 0; j < n; ++j)
    for (int i = 0; i < n; ++i)
      ex.params_data()[j * n + i] =
          i == j ? n + 1.0 + 0.1 * point : 0.01 * (i - 2 * j + point);
  for (int i = 0; i < n * k; ++i)
    ex.params_data()[n * n + i] = 0.02 * (i % 7 - point);
  std::vector<double> result(1 + ex.n_params());
  result[0] = ex.gradient(result.data() + 1);
  return result;
}
static bool identical(const std::vector<double>& a,
                      const std::vector<double>& b) {
  return a.size() == b.size() &&
         std::memcmp(a.data(), b.data(), a.size() * sizeof(double)) == 0;
}

// Cover Executor scratch directly, generated island adjoints, and repeated
// var CALLs whose same register range holds a new divisor on each iteration.
static Graph call_graph(int n, int k, int path) {
  Graph g;
  const int a = g.add_slot(n * n, true), b = g.add_slot(n * k, true);
  g.result_slot = g.add_slot(1, false);
  const uint8_t variant = 13u | (k == 1 ? 2u : 0u);
  if (path == 0) {
    const int solved = g.add_slot(n * k, false);
    g.add_op(OP_MDIVIDE_LEFT, {a, b}, solved, {n, k});
    g.ops.back().variant = variant;
    g.add_op(OP_SUM_VEC, {solved}, g.result_slot);
    return g;
  }
  auto p = std::make_shared<IslandProg>();
  p->ins = {{0, n * n}, {n * n, n * k}};
  const int solved = n * n + n * k, sum = solved + n * k;
  const int total = sum + 1, count = total + 1, one = count + 1;
  const int limit = one + 1, step = limit + 1, condition = step + 1;
  const int scratch = condition + 1;
  Op shape;
  const int dims[] = {n, k};
  shape.variant = variant;
  shape.idata = dims;
  shape.n_idata = 2;
  const Kernel solve = *find_kernel(OP_MDIVIDE_LEFT);
  const int saved = solve.scratch_size ? solve.scratch_size(shape, nullptr) : 0;
  p->n_regs = scratch + saved;
  Program::Call divide;
  divide.opcode = OP_MDIVIDE_LEFT;
  divide.variant = variant;
  divide.n_in = 2;
  divide.in[0] = 0;
  divide.in_len[0] = n * n;
  divide.in[1] = n * n;
  divide.in_len[1] = n * k;
  divide.out = solved;
  divide.out_len = n * k;
  divide.scratch = scratch;
  divide.scratch_len = saved;
  divide.idata = {n, k};
  divide.forward = solve.forward;
  divide.backward = solve.backward;
  Program::Call reduce;
  reduce.opcode = OP_SUM_VEC;
  reduce.n_in = 1;
  reduce.in[0] = solved;
  reduce.in_len[0] = n * k;
  reduce.out = sum;
  reduce.out_len = 1;
  reduce.forward = find_kernel(OP_SUM_VEC)->forward;
  reduce.backward = find_kernel(OP_SUM_VEC)->backward;
  p->calls = {divide, reduce};
  if (path == 1) {
    p->code = {{Program::CALL, 0, 0}, {Program::CALL, 0, 1}};
    p->out_regs = {sum};
    check(gen_adjoint(*p), "solve CALL supports a generated island adjoint");
    p->native_adj = true;
  } else {
    p->pool = {0, 1, 3, .125};
    p->code = {{Program::CONST, total, 0}, {Program::CONST, count, 0},
               {Program::CONST, one, 1},   {Program::CONST, limit, 2},
               {Program::CONST, step, 3},  {Program::CALL, 0, 0},
               {Program::CALL, 0, 1},      {Program::ADD, total, total, sum}};
    for (int i = 0; i < n; ++i)
      p->code.push_back({Program::ADD, i * (n + 1), i * (n + 1), step});
    p->code.push_back({Program::ADD, count, count, one});
    p->code.push_back({Program::LT, condition, count, limit});
    const int exit = static_cast<int>(p->code.size()) + 2;
    p->code.push_back({Program::JZ, exit, condition});
    p->code.push_back({Program::JMP, 5});
    p->out_regs = {total};
    check(!gen_adjoint(*p), "looping solve CALLs use private var replay tapes");
  }
  g.add_op(OP_ISLAND, {a, b}, g.result_slot);
  g.ops.back().udata = p.get();
  g.udata_pool.push_back(p);
  return g;
}

static void direct_and_island_calls() {
  for (int n : {2, 10, 55})
    for (int k : {1, 3})
      for (int path : {0, 1, 2}) {
        Kernel without = original;
        without.forward = recompute_forward;
        without.backward = recompute_backward;
        without.scratch_size = nullptr;
        register_kernel(OP_MDIVIDE_LEFT, without);
        Executor expected(call_graph(n, k, path));
        register_kernel(OP_MDIVIDE_LEFT, original);
        Executor actual(call_graph(n, k, path));
        for (int point : {0, 1, 2, 0}) {
          check(identical(evaluate(actual, n, k, point),
                          evaluate(expected, n, k, point)),
                "top-level and island CALL factors match recomputation");
          const double v = actual.forward_value_only();
          const double w = expected.forward_value_only();
          check(std::memcmp(&v, &w, sizeof(double)) == 0,
                "CALL value-only interleave preserves prim arithmetic");
        }
        Executor copy(actual);
        check(identical(evaluate(copy, n, k, 3), evaluate(expected, n, k, 3)),
              "top-level and island copies own their factors");
        check(identical(evaluate(actual, n, k, 4), evaluate(expected, n, k, 4)),
              "CALL copy evaluation leaves source factors independent");
      }
}

int main() {
  original = *find_kernel(OP_MDIVIDE_LEFT);
  check(original.scratch_size != nullptr, "plain-left solve retains its QR");
  direct_and_island_calls();
  for (int n : {2, 10, 55})
    for (int k : {1, 3})
      for (int rows : {0, 1, 33})
        for (bool frames : {false, true}) {
          Kernel without = original;
          without.forward = recompute_forward;
          without.backward = recompute_backward;
          without.scratch_size = nullptr;
          register_kernel(OP_MDIVIDE_LEFT, without);
          const Graph reference = graph(n, k, rows, k == 1);
          register_kernel(OP_MDIVIDE_LEFT, original);
          const Graph candidate = graph(n, k, rows, k == 1);
          test_setenv("STANLI_STRUCTURED_FRAMES", frames ? "1" : "0");
          Executor expected(reference);
          test_setenv("STANLI_STRUCTURED_LOOP_DIAGNOSTICS", "1");
          Executor actual(candidate);
          test_unsetenv("STANLI_STRUCTURED_LOOP_DIAGNOSTICS");
          stanli_test::StdoutCapture captured(stderr);
          for (int point : {0, 1, 2, 0}) {
            check(identical(evaluate(actual, n, k, point),
                            evaluate(expected, n, k, point)),
                  "each backward reads its own forward factor");
            const double v = actual.forward_value_only();
            const double w = expected.forward_value_only();
            check(std::memcmp(&v, &w, sizeof(double)) == 0,
                  "value-only retains the prim solve arithmetic");
          }
          Executor copy(actual);
          check(identical(evaluate(copy, n, k, 3), evaluate(expected, n, k, 3)),
                "copied executor independently records factors");
          check(
              identical(evaluate(actual, n, k, 4), evaluate(expected, n, k, 4)),
              "copy leaves the original factor storage independent");
          const auto diagnostics = captured.finish();
          if (frames && rows == 33)
            check(diagnostics.find("stanli_structured frames:") !=
                      std::string::npos,
                  "factor lifetime check reaches numerical frames");
          if (rows > 0) {
            // Frames return before the stream's respecialized counter is
            // printed. Count their completed recordings instead: the first
            // recording plus the two parameter-controlled branch changes.
            size_t recordings = 0, position = 0;
            while ((position = diagnostics.find("stanli_structured frames:",
                                                position)) !=
                   std::string::npos) {
              ++recordings;
              ++position;
            }
            const bool respecialized =
                frames
                    ? recordings >= 3
                    : diagnostics.find("respecialized=2") != std::string::npos;
            if (!respecialized && failures < 2)
              std::printf("n=%d k=%d rows=%d frames=%d\n%s\n", n, k, rows,
                          frames, diagnostics.c_str());
            check(respecialized,
                  "changed branches re-record factors at least twice");
          }
          test_unsetenv("STANLI_STRUCTURED_FRAMES");
        }
  std::printf("test_solve_reuse: %d failures\n", failures);
  return failures ? 1 : 0;
}
