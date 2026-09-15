// The built-in per-op profiler: opt-in accounting of where gradient time
// goes, per opcode, without touching the fast path when off.
#include <stanli/graph.hpp>
#include <stanli/island.hpp>
#include <stanli/optable.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int failures = 0;
static void expect(const char* what, bool ok) {
  if (!ok) {
    ++failures;
    std::printf("FAIL %s\n", what);
  }
}

// The opcode's calls column, or -1 if the report has no row for it. Rows are
// "%-22s %10lld %12lld %12lld %5.1f%% %12lld", so calls is the second
// whitespace-separated field; searching the report for the number anywhere
// would also match the two unbounded nanosecond columns.
static long long calls_for(const std::string& rep, const char* opcode) {
  const std::string name(opcode);
  for (size_t pos = 0; pos < rep.size();) {
    const size_t eol = rep.find('\n', pos);
    const std::string line = rep.substr(pos, eol - pos);
    pos = eol == std::string::npos ? rep.size() : eol + 1;
    const size_t a = line.find_first_not_of(' ');
    if (a == std::string::npos) continue;
    const size_t b = line.find(' ', a);
    if (b == std::string::npos || line.compare(a, b - a, name) != 0) continue;
    return std::strtoll(line.c_str() + b, nullptr, 10);
  }
  return -1;
}

static void bound_add_forward(stanli::KernelCtx& ctx) {
  ctx.out.data[0] = ctx.in[0].data[0] + ctx.in[1].data[0] + 3.0;
}

static void bound_add_backward(stanli::KernelCtx& ctx) {
  ctx.in_adj[0].data[0] += 4.0 * ctx.out_adj;
  ctx.in_adj[1].data[0] += 5.0 * ctx.out_adj;
}

static void replacement_add_forward(stanli::KernelCtx& ctx) {
  ctx.out.data[0] = ctx.in[0].data[0] + ctx.in[1].data[0] + 30.0;
}

static void replacement_add_backward(stanli::KernelCtx& ctx) {
  ctx.in_adj[0].data[0] += 40.0 * ctx.out_adj;
  ctx.in_adj[1].data[0] += 50.0 * ctx.out_adj;
}

static std::shared_ptr<stanli::Softmax3IslandProg> make_specialized_softmax() {
  auto p = std::make_shared<stanli::Softmax3IslandProg>();
  p->n_regs = 6;
  p->ins.push_back(stanli::IslandProg::LiveIn{0, 3});
  p->out_regs = {3, 4, 5};
  p->code.push_back(
      stanli::Program::Instr{stanli::Program::SOFTMAX, 3, 0, 0, 0, 3});
  p->native_adj = true;
  p->optimized_double = stanli::specialize_softmax3(*p, 1);
  return p;
}

static void test_specialized_forward_profile_parity() {
  using namespace stanli;
  auto payload = make_specialized_softmax();
  expect("profile specialized plan",
         static_cast<bool>(payload->optimized_double));

  Graph g;
  const int input = g.add_slot(3, true);
  const int probs = g.add_slot(3, false);
  const int result = g.add_slot(1, false);
  g.add_op(OP_ISLAND, {input}, probs);
  g.ops.back().variant = kIslandSoftmax3Variant;
  g.ops.back().udata = payload.get();
  g.udata_pool.push_back(payload);
  g.add_op(OP_INDEX, {probs}, result, {1});
  g.result_slot = result;

  Executor ex(std::move(g));
  ex.param_ptr(input)[0] = 1.2;
  ex.param_ptr(input)[1] = -0.7;
  ex.param_ptr(input)[2] = 0.4;
  ex.run_forward_only();
  const double ordinary = ex.forward();
  double ordinary_probs[3];
  std::memcpy(ordinary_probs, ex.value_ptr(probs), sizeof ordinary_probs);

  ex.set_profile(true);
  const double profiled = ex.forward();
  expect("specialized profiled value", profiled == ordinary);
  expect("specialized profiled lanes",
         std::memcmp(ordinary_probs, ex.value_ptr(probs),
                     sizeof ordinary_probs) == 0);
}

static void test_bound_kernel_survives_post_bind_override() {
  using namespace stanli;
  const Kernel saved = *find_kernel(OP_ADD_N);
  register_kernel(OP_ADD_N,
                  Kernel{bound_add_forward, bound_add_backward, nullptr});

  Graph g;
  const int a = g.add_slot(1, true);
  const int b = g.add_slot(1, true);
  const int out = g.add_slot(1, false);
  g.add_op(OP_ADD_N, {a, b}, out);
  g.result_slot = out;
  Executor ex(std::move(g));
  ex.param_ptr(a)[0] = 0.2;
  ex.param_ptr(b)[0] = -1.0;

  // The executor has already bound the original pair. A later registry write
  // must not make profiling run a different forward or reverse function.
  register_kernel(OP_ADD_N, Kernel{replacement_add_forward,
                                   replacement_add_backward, nullptr});
  double grad[2];
  const double unprofiled = ex.gradient(grad);
  expect("bound forward before profile", unprofiled == 2.2);
  expect("bound backward before profile", grad[0] == 4.0 && grad[1] == 5.0);

  ex.set_profile(true);
  // A single tiny call can round to zero at the profiler clock resolution.
  // The report omits a zero-time sample, so accumulate several executions.
  for (int i = 0; i < 100; ++i) {
    const double profiled = ex.gradient(grad);
    expect("bound forward under profile", profiled == 2.2);
    expect("bound backward under profile", grad[0] == 4.0 && grad[1] == 5.0);
  }
  expect("bound override is profiled",
         calls_for(ex.profile_report(), "OP_ADD_N") == 100);

  register_kernel(OP_ADD_N, saved);
}

int main() {
  using namespace stanli;

  Graph g;
  const int a = g.add_slot(1, /*is_param=*/true);
  const int b = g.add_slot(1, /*is_param=*/true);
  const int ea = g.add_slot(1, false);
  const int lp = g.add_slot(1, false);
  const int control = g.add_slot(1, false);
  g.add_op(OP_EXP, {a}, ea);
  g.add_op(OP_COMPARE, {a, b}, control);
  g.add_op(OP_ADD_N, {ea, b}, lp);
  g.result_slot = lp;

  Executor ex(std::move(g));
  *ex.param_ptr(a) = 0.3;
  *ex.param_ptr(b) = -1.1;
  double grad[2];

  // Off by default: no rows, and results are unaffected either way.
  expect("empty report when off", ex.profile_report().empty());
  const double v_off = ex.gradient(grad);
  const double da_off = grad[0];

  ex.set_profile(true);
  for (int i = 0; i < 10; ++i) ex.gradient(grad);
  expect("same value profiled", ex.gradient(grad) == v_off);
  expect("same grad profiled", grad[0] == da_off);

  const std::string rep = ex.profile_report();
  expect("report names EXP", rep.find("EXP") != std::string::npos);
  expect("report names ADD_N", rep.find("ADD_N") != std::string::npos);
  // 11 profiled gradient evaluations, one op instance of each opcode.
  expect("counts EXP calls", calls_for(rep, "OP_EXP") == 11);
  expect("counts ADD_N calls", calls_for(rep, "OP_ADD_N") == 11);
  expect("report has totals", rep.find("total") != std::string::npos);
  expect("forward-only gap is profiled", calls_for(rep, "OP_COMPARE") == 11);

  // A copy must attribute its own rebound contexts and preserve reverse order.
  Executor copied(ex);
  copied.set_profile(true);
  expect("copied executor profiled value", copied.gradient(grad) == v_off);
  expect("copied executor profiled gradient",
         grad[0] == da_off && grad[1] == 1);

  // Toggling off stops accumulation but keeps the collected numbers.
  ex.set_profile(false);
  const std::string before = ex.profile_report();
  ex.gradient(grad);
  expect("no growth when off", ex.profile_report() == before);

  test_specialized_forward_profile_parity();
  test_bound_kernel_survives_post_bind_override();

  if (failures == 0) std::printf("test_profile: all ok\n");
  return failures == 0 ? 0 : 1;
}
