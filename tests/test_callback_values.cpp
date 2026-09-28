// Value-only solver calls accept runtime real buffers without derivative
// storage. Run the actual selected kernels with null scratch and sentinel
// adjoints; a forward or reverse that still accesses the old Jacobian fails.
#include <stanli/compile.hpp>
#include <stanli/island.hpp>
#include <stanli/optable.hpp>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <limits>
#include <sstream>
#include <vector>

namespace {
int failures = 0;
void expect(const char* message, bool condition) {
  if (!condition) {
    ++failures;
    std::printf("FAIL %s\n", message);
  }
}
std::string slurp(const char* path) {
  std::ifstream f(path);
  std::ostringstream s;
  s << f.rdbuf();
  return s.str();
}
bool solver(uint16_t opcode) {
  using namespace stanli;
  return opcode == OP_ODE || opcode == OP_DAE || opcode == OP_ALGEBRA_SOLVER ||
         opcode == OP_QUADRATURE || opcode == OP_ODE_ADJOINT;
}
void check(const stanli::Op& op) {
  using namespace stanli;
  const std::vector<double> theta{0.1,  0.02, 0.01, 0.01,
                                  0.01, 0.01, 0.01, 0.01};
  std::vector<std::vector<double>> inputs;
  double want = 0.416;
  switch (op.opcode) {
    case OP_ODE:
      inputs = {{0.4}, theta};
      if (op.n_in == 4) {
        inputs.push_back({0.0});
        inputs.push_back({0.1});
      }
      expect("ODE explicitly selects double scalar types",
             op.variant == 0x4u || op.variant == 0x10u);
      break;
    case OP_DAE:
      inputs = {{0.4}, {0.16}, theta};
      expect("DAE explicitly selects double scalar types", op.variant == 0x8u);
      break;
    case OP_ALGEBRA_SOLVER:
      inputs = {{0.4}, theta};
      want = 0.16;
      expect("algebra selects double parameters", op.variant == 0);
      break;
    case OP_QUADRATURE:
      inputs = {{0.0}, {0.2}, theta};
      want = 0.0032;
      expect("quadrature selects double scalar types", op.variant == 0);
      break;
    case OP_ODE_ADJOINT:
      inputs = {{0.4}, {0.0}, {0.1}, theta};
      if (op.n_in == 5)
        inputs.push_back({1e-10, 1e-10, 1e-10, 1e-10, 1e-10, 1e-10,
                          100000, 10, 1, 1, 1});
      expect("adjoint ODE explicitly selects double scalar types",
             op.variant == 0x10u);
      break;
    default:
      return;
  }
  expect("solver test supplies every runtime input", inputs.size() == op.n_in);
  const Kernel* kernel = find_kernel(op.opcode);
  expect("solver kernel exists", kernel != nullptr);
  if (!kernel) return;
  expect("scratch callback is registered except for adjoint ODE",
         (kernel->scratch_size != nullptr) == (op.opcode != OP_ODE_ADJOINT));
  Op shape = op;
  std::vector<Slot> slots(inputs.size() + 1);
  for (size_t i = 0; i < inputs.size(); ++i) {
    shape.in[i] = static_cast<int>(i);
    slots[i].len = inputs[i].size();
  }
  shape.out = static_cast<int>(inputs.size());
  slots.back().len = 1;
  if (op.opcode == OP_ODE || op.opcode == OP_DAE) {
    Op legacy = shape;
    legacy.variant = 0;
    expect("absent activity marker preserves legacy active scratch",
           kernel->scratch_size(legacy, slots.data()) > 0);
  }
  expect(
      "inactive solver needs zero derivative scratch",
      !kernel->scratch_size || kernel->scratch_size(shape, slots.data()) == 0);
  // The sizing proof must remain independent of the number of runtime reals.
  for (size_t i = 0; i < inputs.size(); ++i) slots[i].len = 1000000;
  expect(
      "large inactive solver still needs zero derivative scratch",
      !kernel->scratch_size || kernel->scratch_size(shape, slots.data()) == 0);
  KernelCtx ctx;
  ctx.n_in = static_cast<int>(inputs.size());
  ctx.variant = op.variant;
  ctx.udata = op.udata;
  ctx.scratch = nullptr;
  std::vector<std::vector<double>> adjoints;
  adjoints.reserve(inputs.size());
  for (size_t i = 0; i < inputs.size(); ++i) {
    ctx.in[i] = Desc{inputs[i].data(), static_cast<int64_t>(inputs[i].size())};
    adjoints.emplace_back(inputs[i].size(), 9.0);
    ctx.in_adj[i] = Desc{adjoints.back().data(),
                         static_cast<int64_t>(adjoints.back().size())};
  }
  double result = 0.0, seed = -0.7;
  ctx.out = Desc{&result, 1};
  ctx.out_adj = seed;
  ctx.out_adj_vec = Desc{&seed, 1};
  kernel->forward(ctx);
  expect("inactive solver value is correct", std::abs(result - want) < 1e-12);
  for (double weight : {-0.7, std::numeric_limits<double>::infinity(),
                        std::numeric_limits<double>::quiet_NaN()}) {
    seed = weight;
    ctx.out_adj = weight;
    kernel->backward(ctx);
    for (const auto& values : adjoints)
      for (double value : values)
        expect("inactive backward preserves adjoints", value == 9.0);
  }
}
}  // namespace
int main() {
  using namespace stanli;
  auto model = compile_model(
      slurp("tests/fixtures/gq_callback_runtime_values.tmir.sexp"),
      DataMap::from_json("{}"));
  expect("runtime callback output graph compiles",
         model.write_array.has_value());
  if (!model.write_array) return 1;
  std::map<uint16_t, int> seen;
  std::map<int, int> ode_inputs;
  for (const auto& op : model.write_array->graph.ops) {
    if (solver(op.opcode)) {
      check(op);
      ++seen[op.opcode];
      if (op.opcode == OP_ODE) ++ode_inputs[op.n_in];
    }
    if (op.opcode == OP_ISLAND) {
      const auto& program = *static_cast<const IslandProg*>(op.udata);
      for (const auto& call : program.calls) {
        if (!solver(call.opcode)) continue;
        expect("value-only CALL has no scratch registers",
               call.scratch_len == 0);
        expect("value-only CALL has no differentiated inputs",
               call.input_adjoint_mask == 0);
        Op nested;
        nested.opcode = call.opcode;
        nested.variant = call.variant;
        nested.n_in = call.n_in;
        nested.udata = call.udata_owner.get();
        check(nested);
        ++seen[nested.opcode];
        if (nested.opcode == OP_ODE) ++ode_inputs[nested.n_in];
      }
    }
  }
  expect("all five solver families tested", seen.size() == 5);
  expect("ordinary and register ODE tested", seen[OP_ODE] == 2);
  expect("two-input and four-input ODE encodings tested",
         ode_inputs[2] == 1 && ode_inputs[4] == 1);
  expect("ordinary and register quadrature tested", seen[OP_QUADRATURE] == 2);
  if (!failures) std::puts("test_callback_values OK");
  return failures ? 1 : 0;
}
