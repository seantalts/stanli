#include <stanli/graph.hpp>
#include <stanli/scan.hpp>
#include <stanli/optable.hpp>

#include "env_helpers.hpp"

#include <stan/math.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace stanli;

static int failures = 0;

static void expect_close(const std::string& what, double got, double want) {
  if (std::abs(got - want) > 1e-12) {
    ++failures;
    std::printf("FAIL %s got %.17g want %.17g\n", what.c_str(), got, want);
  }
}

static void expect(const std::string& what, bool ok) {
  if (!ok) {
    ++failures;
    std::printf("FAIL %s\n", what.c_str());
  }
}

static void expect_exact(const std::string& what, double got, double want) {
  if (std::memcmp(&got, &want, sizeof(double)) != 0) {
    ++failures;
    std::printf("FAIL %s got %.17g want %.17g\n", what.c_str(), got, want);
  }
}

static void run_checkpoint_policy_case() {
  constexpr int64_t count = 100;
  constexpr int64_t carry = 7;
  constexpr int64_t exact_bytes = (count + 1) * carry * 8;
  expect("full-boundary policy accepts exact budget",
         choose_scan_checkpoint_block(count, carry, exact_bytes) == 1);
  expect("full-boundary policy retains sqrt above budget",
         choose_scan_checkpoint_block(count, carry, exact_bytes - 1) == 10);
  expect("full-boundary policy retains sqrt for disabled budget",
         choose_scan_checkpoint_block(count, carry, -1) == 10);
  expect("zero-carry scan needs no replay",
         choose_scan_checkpoint_block(count, 0, 0) == 1);
  expect("ctsem carry boundaries fit default budget",
         choose_scan_checkpoint_block(4000, 1335) == 1);
  expect("large carry retains bounded sqrt plan",
         choose_scan_checkpoint_block(4000, 3000) == 63);
}

static ScanSpec make_spec(int64_t count, int64_t block) {
  ScanSpec s;
  s.count = count;
  s.templates.resize(1);
  s.checkpoint_block = block;
  s.carry_cells = 1;
  s.output_cells = count + 2;  // final carry, one sink per row, target
  auto& t = s.templates[0];

  IslandProg p;
  p.n_regs = 5;
  p.ins = {{0, 1, 0, 0, true}, {1, 1, 1, 0, true}, {2, 1, 2, 0, false}};
  // carry' = scale * carry + row; target and sink are both carry'.
  p.code.push_back(Program::Instr{Program::MUL, 3, 0, 1, 0, 1});
  p.code.push_back(Program::Instr{Program::ADD, 4, 3, 2, 0, 1});
  p.out_regs = {4};
  if (!gen_adjoint(p)) {
    std::printf("FAIL gen_adjoint refused scan step\n");
    ++failures;
  }
  t.step = std::move(p);
  t.inputs.push_back({1, 0, 0, 1, 1, true});
  if (count > 0) t.inputs.push_back({2, 0, 1, 2, 1, false});
  t.carry.push_back({0, 0, 0, 4, 1, 0});
  if (count > 0) t.sinks.push_back({4, 1, 1, 1});
  t.target_reg = 4;
  return s;
}

static void run_case(int64_t count, int64_t block) {
  Graph g;
  const int init = g.add_slot(1, true);
  const int scale = g.add_slot(1, true);
  const int rows = g.add_slot(count, false);
  const int out = g.add_slot(count + 2, false);
  auto spec = std::make_shared<ScanSpec>(make_spec(count, block));
  g.udata_pool.push_back(spec);
  Op op;
  op.opcode = OP_SCAN;
  op.out = out;
  op.n_in = 3;
  op.in[0] = init;
  op.in[1] = scale;
  op.in[2] = rows;
  op.udata = g.udata_pool.back().get();
  g.ops.push_back(op);
  const int target_slot = g.add_slot(1, false);
  const int final = g.add_slot(1, false);
  const int result = g.add_slot(1, false);
  g.add_op(OP_INDEX, {out}, target_slot, {int(count + 1)});
  g.add_op(OP_INDEX, {out}, final, {0});
  if (count > 0) {
    const int sink = g.add_slot(count, false);
    const int sink_sum = g.add_slot(1, false);
    g.add_op(OP_SLICE, {out}, sink, {1});
    g.add_op(OP_SUM_VEC, {sink}, sink_sum);
    g.add_op(OP_ADD_N, {target_slot, final, sink_sum}, result);
  } else {
    g.add_op(OP_ADD_N, {target_slot, final}, result);
  }
  g.result_slot = result;

  Executor ex(std::move(g));
  *ex.param_ptr(init) = 2.0;
  *ex.param_ptr(scale) = 0.75;
  for (int64_t i = 0; i < count; ++i) ex.value_ptr(rows)[i] = double(i + 1);
  double grad[2] = {0.0, 0.0};
  const double value = ex.gradient(grad);
  double carry = 2.0;
  double d_init = 1.0;
  double d_scale = 0.0;
  double target = 0.0;
  double target_d_init = 0.0;
  double target_d_scale = 0.0;
  for (int64_t i = 0; i < count; ++i) {
    d_scale = carry + 0.75 * d_scale;
    d_init *= 0.75;
    carry = 0.75 * carry + double(i + 1);
    target += carry;
    target_d_init += d_init;
    target_d_scale += d_scale;
  }
  // The downstream result seeds target, final carry, and every sink. Sinks
  // repeat the target's per-row carry values, so this checks all three
  // reverse routes at once.
  expect_close("target result", value, 2.0 * target + carry);
  expect_close("initial carry gradient", grad[0], 2.0 * target_d_init + d_init);
  expect_close("active invariant gradient", grad[1],
               2.0 * target_d_scale + d_scale);

  // Repeated evaluation and copied executors exercise payload ownership and
  // fresh scratch/boundary arenas.
  double grad2[2] = {0.0, 0.0};
  expect_close("repeat value", ex.gradient(grad2), 2.0 * target + carry);
  expect_close("repeat initial gradient", grad2[0], grad[0]);
  expect_close("repeat invariant gradient", grad2[1], grad[1]);
  Executor copy(ex);
  double grad3[2] = {0.0, 0.0};
  expect_close("copy value", copy.gradient(grad3), 2.0 * target + carry);
  expect_close("copy initial gradient", grad3[0], grad[0]);
  expect_close("copy invariant gradient", grad3[1], grad[1]);
}

static void run_vector_schedule_case() {
  constexpr int64_t n = 3;
  Graph g;
  const int init = g.add_slot(2, true);
  const int rows = g.add_slot(n, false);
  const int out = g.add_slot(2 + 2 * n + 1, false);
  auto spec = std::make_shared<ScanSpec>();
  spec->count = n;
  spec->checkpoint_block = 2;  // includes a partial final block
  spec->carry_cells = 2;
  spec->output_cells = 2 + 2 * n + 1;
  spec->template_for_iteration = {0, 1, 0};
  for (int variant = 0; variant < 2; ++variant) {
    ScanSpec::Template t;
    IslandProg p;
    p.n_regs = 6;
    p.ins = {{0, 2, 0, 0, true}, {1, 1, 4, 0, false}};
    // c0' = c0 + (1 or 2) * row; c1' = c1 + (1 or 2).
    p.pool.push_back(variant == 0 ? 1.0 : 2.0);
    p.code.push_back(Program::Instr{Program::CONST, 5, 0, 0, 0, 1});
    if (variant == 0) {
      p.code.push_back(Program::Instr{Program::ADD, 2, 0, 4, 0, 1});
      p.code.push_back(Program::Instr{Program::ADD, 3, 1, 5, 0, 1});
    } else {
      p.code.push_back(Program::Instr{Program::MUL, 5, 4, 5, 0, 1});
      p.code.push_back(Program::Instr{Program::ADD, 2, 0, 5, 0, 1});
      p.code.push_back(Program::Instr{Program::CONST, 5, 0, 0, 0, 1});
      p.code.push_back(Program::Instr{Program::ADD, 3, 1, 5, 0, 1});
    }
    p.out_regs = {2, 3};
    if (!gen_adjoint(p)) ++failures;
    t.step = std::move(p);
    t.inputs.push_back({1, 0, 1, 4, 1, false});
    t.carry.push_back({0, 0, 0, 2, 2, 0});
    t.sinks.push_back({2, 2, 2, 1});
    t.sinks.push_back({3, 3, 2, 1});
    t.target_reg = 2;
    spec->templates.push_back(std::move(t));
  }
  g.udata_pool.push_back(spec);
  Op op;
  op.opcode = OP_SCAN;
  op.out = out;
  op.n_in = 2;
  op.in[0] = init;
  op.in[1] = rows;
  op.udata = g.udata_pool.back().get();
  g.ops.push_back(op);
  const int target = g.add_slot(1, false);
  const int final0 = g.add_slot(1, false);
  const int result = g.add_slot(1, false);
  g.add_op(OP_INDEX, {out}, target, {int(2 + 2 * n)});
  g.add_op(OP_INDEX, {out}, final0, {0});
  g.add_op(OP_ADD_N, {target, final0}, result);
  g.result_slot = result;
  Executor ex(std::move(g));
  ex.param_ptr(init)[0] = 1.0;
  ex.param_ptr(init)[1] = 10.0;
  for (int64_t i = 0; i < n; ++i) ex.value_ptr(rows)[i] = double(i + 1);
  double grad[2] = {0, 0};
  // c0: 1 -> 2 -> 6 -> 9; target=17, final0=9.
  expect_close("vector schedule value", ex.gradient(grad), 26.0);
  expect_close("vector schedule grad0", grad[0], 4.0);
  expect_close("vector schedule grad1", grad[1], 0.0);
}

// The scan reuses one compact adjoint file for all rows and templates.  The
// first row visited by the reverse sweep leaves a derivative on an inactive
// data live-in at compact cell zero; the next template deliberately uses its
// own cell zero for the active parameter.  A narrow live-in reset must clear
// the former without relying on a full-file fill between rows.
static void run_recycled_adjoint_case() {
  Graph g;
  const int theta = g.add_slot(1, true);
  const int datum = g.add_slot(1, false);
  const int packed = g.add_slot(1, false);
  auto spec = std::make_shared<ScanSpec>();
  spec->count = 2;
  spec->checkpoint_block = 1;
  spec->carry_cells = 0;
  spec->output_cells = 1;
  // Forward order is active-at-zero, inactive-at-zero. Reverse therefore
  // creates the inactive residue before cell zero becomes active.
  spec->template_for_iteration = {1, 0};
  for (int which = 0; which < 2; ++which) {
    ScanSpec::Template tm;
    tm.step.n_regs = 3;
    tm.step.ins = {{0, 1, 0, 0, which == 1}, {1, 1, 1, 0, which == 0}};
    tm.step.code.push_back({Program::MUL, 2, 0, 1});
    tm.step.out_regs = {2};
    expect("recycled adjoint generated", gen_adjoint(tm.step));
    if (which == 0) {
      tm.inputs.push_back({1, 0, 0, 0, 1, false});
      tm.inputs.push_back({0, 0, 0, 1, 1, true});
    } else {
      tm.inputs.push_back({0, 0, 0, 0, 1, true});
      tm.inputs.push_back({1, 0, 0, 1, 1, false});
    }
    tm.target_reg = 2;
    spec->templates.push_back(std::move(tm));
  }
  g.udata_pool.push_back(spec);
  Op scan;
  scan.opcode = OP_SCAN;
  scan.out = packed;
  scan.n_in = 2;
  scan.in[0] = theta;
  scan.in[1] = datum;
  scan.udata = spec.get();
  g.ops.push_back(scan);
  g.result_slot = packed;

  Executor ex(std::move(g));
  *ex.param_ptr(theta) = 2.0;
  *ex.value_ptr(datum) = 3.0;
  double gradient = 0.0;
  expect_exact("recycled adjoint value", ex.gradient(&gradient), 12.0);
  expect_exact("recycled adjoint gradient", gradient, 6.0);
  expect_exact("recycled adjoint repeated value", ex.gradient(&gradient), 12.0);
  expect_exact("recycled adjoint repeated gradient", gradient, 6.0);
}

static void run_reset_carry_case() {
  constexpr int64_t n = 3;
  Graph g;
  const int initial = g.add_slot(1, true);
  const int rows = g.add_slot(n, false);
  const int packed = g.add_slot(2, false);  // final carry and target

  auto spec = std::make_shared<ScanSpec>();
  spec->count = n;
  spec->checkpoint_block = 2;
  spec->carry_cells = 1;
  spec->output_cells = 2;
  ScanSpec::Template tm;
  tm.step.n_regs = 1;
  tm.step.ins = {{0, 1, 0, 0, false}};
  tm.step.out_regs = {0};
  expect("reset carry adjoint generated", gen_adjoint(tm.step));
  tm.inputs.push_back({1, 0, 1, 0, 1, false});
  tm.carry.push_back({0, 0, -1, 0, 1, 0});
  tm.target_reg = 0;
  spec->templates.push_back(std::move(tm));

  g.udata_pool.push_back(spec);
  Op scan;
  scan.opcode = OP_SCAN;
  scan.out = packed;
  scan.n_in = 2;
  scan.in[0] = initial;
  scan.in[1] = rows;
  scan.udata = spec.get();
  g.ops.push_back(scan);
  const int final = g.add_slot(1, false);
  const int target = g.add_slot(1, false);
  const int result = g.add_slot(1, false);
  g.add_op(OP_INDEX, {packed}, final, {0});
  g.add_op(OP_INDEX, {packed}, target, {1});
  g.add_op(OP_ADD, {final, target}, result);
  g.result_slot = result;

  Executor ex(std::move(g));
  *ex.param_ptr(initial) = 123.0;
  ex.value_ptr(rows)[0] = 2.0;
  ex.value_ptr(rows)[1] = 3.0;
  ex.value_ptr(rows)[2] = 5.0;
  double grad = -1.0;
  // Every row resets the carry: final=5, target=sum(rows)=10, and the
  // initial parameter has no path to either result.
  expect_close("reset carry value", ex.gradient(&grad), 15.0);
  expect_close("reset carry initial gradient", grad, 0.0);
}

static void run_identity_carry_case() {
  constexpr int64_t n = 3;
  Graph g;
  const int initial = g.add_slot(1, true);
  const int rows = g.add_slot(n, false);
  const int packed = g.add_slot(2, false);  // final carry and target

  auto spec = std::make_shared<ScanSpec>();
  spec->count = n;
  spec->checkpoint_block = 2;
  spec->carry_cells = 1;
  spec->output_cells = 2;
  ScanSpec::Template tm;
  tm.step.n_regs = 3;
  tm.step.ins = {{0, 1, 0, 0, true}, {1, 1, 1, 0, false}};
  tm.step.code.push_back(Program::Instr{Program::ADD, 2, 0, 1, 0, 1});
  tm.step.out_regs = {2};
  expect("identity carry adjoint generated", gen_adjoint(tm.step));
  tm.inputs.push_back({1, 0, 1, 1, 1, false});
  tm.carry.push_back({0, 0, 0, -1, 1, 0});
  tm.target_reg = 2;
  spec->templates.push_back(std::move(tm));

  g.udata_pool.push_back(spec);
  Op scan;
  scan.opcode = OP_SCAN;
  scan.out = packed;
  scan.n_in = 2;
  scan.in[0] = initial;
  scan.in[1] = rows;
  scan.udata = spec.get();
  g.ops.push_back(scan);
  const int final = g.add_slot(1, false);
  const int target = g.add_slot(1, false);
  const int result = g.add_slot(1, false);
  g.add_op(OP_INDEX, {packed}, final, {0});
  g.add_op(OP_INDEX, {packed}, target, {1});
  g.add_op(OP_ADD, {final, target}, result);
  g.result_slot = result;

  Executor ex(std::move(g));
  *ex.param_ptr(initial) = 7.0;
  ex.value_ptr(rows)[0] = 2.0;
  ex.value_ptr(rows)[1] = 3.0;
  ex.value_ptr(rows)[2] = 5.0;
  double grad = -1.0;
  // The carry is read but untouched by every row: final=7 and target is
  // (7+2) + (7+3) + (7+5) = 31.
  expect_close("identity carry value", ex.gradient(&grad), 38.0);
  expect_close("identity carry initial gradient", grad, 4.0);
}

static void run_mixed_carry_activity_case(int64_t checkpoint_block,
                                          std::vector<uint32_t> schedule,
                                          double expected_value,
                                          double expected_gradient) {
  const int64_t n = static_cast<int64_t>(schedule.size());
  Graph g;
  const int initial = g.add_slot(1, false);
  const int theta = g.add_slot(1, true);
  const int rows = g.add_slot(n, false);
  const int packed = g.add_slot(2, false);  // final carry and target

  auto spec = std::make_shared<ScanSpec>();
  spec->count = n;
  spec->checkpoint_block = checkpoint_block;
  spec->carry_cells = 1;
  spec->output_cells = 2;
  spec->template_for_iteration = std::move(schedule);

  // Active update: carry' = carry + theta + row.
  {
    ScanSpec::Template tm;
    tm.step.n_regs = 5;
    tm.step.ins = {{0, 1, 0, 0, true}, {1, 1, 1, 0, true}, {2, 1, 2, 0, false}};
    tm.step.code.push_back({Program::ADD, 3, 0, 1, 0, 1});
    tm.step.code.push_back({Program::ADD, 4, 3, 2, 0, 1});
    tm.step.out_regs = {4};
    expect("mixed active carry adjoint generated", gen_adjoint(tm.step));
    tm.inputs.push_back({1, 0, 0, 1, 1, true});
    tm.inputs.push_back({2, 0, 1, 2, 1, false});
    tm.carry.push_back({0, 0, 0, 4, 1, 0, true});
    tm.target_reg = 4;
    spec->templates.push_back(std::move(tm));
  }

  // Data-only reset: carry' = row + row.
  {
    ScanSpec::Template tm;
    tm.step.n_regs = 2;
    tm.step.ins = {{0, 1, 0, 0, false}};
    tm.step.code.push_back({Program::ADD, 1, 0, 0, 0, 1});
    tm.step.out_regs = {1};
    expect("mixed reset carry adjoint generated", gen_adjoint(tm.step));
    tm.inputs.push_back({2, 0, 1, 0, 1, false});
    tm.carry.push_back({0, 0, -1, 1, 1, 0, false});
    tm.target_reg = 1;
    spec->templates.push_back(std::move(tm));
  }

  // Identity: preserve the carry and publish it as this row's target.
  {
    ScanSpec::Template tm;
    tm.step.n_regs = 3;
    tm.step.ins = {{0, 1, 0, 0, true}};
    tm.step.pool = {0.0};
    tm.step.code.push_back({Program::CONST, 1, 0});
    tm.step.code.push_back({Program::ADD, 2, 0, 1, 0, 1});
    tm.step.out_regs = {2};
    expect("mixed identity carry adjoint generated", gen_adjoint(tm.step));
    tm.carry.push_back({0, 0, 0, -1, 1, 0, true});
    tm.target_reg = 2;
    spec->templates.push_back(std::move(tm));
  }

  g.udata_pool.push_back(spec);
  Op scan;
  scan.opcode = OP_SCAN;
  scan.out = packed;
  scan.n_in = 3;
  scan.in[0] = initial;
  scan.in[1] = theta;
  scan.in[2] = rows;
  scan.udata = spec.get();
  g.ops.push_back(scan);
  const int final = g.add_slot(1, false);
  const int target = g.add_slot(1, false);
  const int result = g.add_slot(1, false);
  g.add_op(OP_INDEX, {packed}, final, {0});
  g.add_op(OP_INDEX, {packed}, target, {1});
  g.add_op(OP_ADD, {final, target}, result);
  g.result_slot = result;

  Executor ex(std::move(g));
  *ex.value_ptr(initial) = 5.0;
  *ex.param_ptr(theta) = 2.0;
  for (int64_t k = 0; k < n; ++k) ex.value_ptr(rows)[k] = double(k + 1);
  double gradient = -1.0;
  const std::string tag =
      "mixed carry block=" + std::to_string(checkpoint_block) +
      " count=" + std::to_string(n);
  expect_exact(tag + " value", ex.gradient(&gradient), expected_value);
  expect_exact(tag + " gradient", gradient, expected_gradient);
  expect_exact(tag + " repeated value", ex.gradient(&gradient), expected_value);
  expect_exact(tag + " repeated gradient", gradient, expected_gradient);
  Executor copied(ex);
  expect_exact(tag + " copied value", copied.gradient(&gradient),
               expected_value);
  expect_exact(tag + " copied gradient", gradient, expected_gradient);
}

// A parameter-dependent branch remains an OP_ISLAND CALL inside the acyclic
// step. This is the Phase 1 reason Program::Call carries udata: the scan's
// generated adjoint delegates to the island's replay backward, including its
// payload and saved input values.
static void run_branch_call_case() {
  auto branch = std::make_shared<IslandProg>();
  branch->n_regs = 4;
  branch->ins = {{0, 1, 0, 0, true}};
  branch->pool = {0.0};
  branch->code = {
      {Program::CONST, 1, 0}, {Program::GT, 2, 0, 1}, {Program::JZ, 5, 2},
      {Program::EXP, 3, 0},   {Program::JMP, 6},      {Program::SQUARE, 3, 0},
  };
  branch->out_regs = {3};

  IslandProg step;
  step.n_regs = 3;
  step.ins = {{0, 1, 0, 0, true}};
  Program::Call call;
  call.opcode = OP_ISLAND;
  call.udata = branch.get();
  call.n_in = 1;
  call.in[0] = 0;
  call.in_len[0] = 1;
  call.out = 1;
  call.out_len = 1;
  call.scratch = 2;
  call.scratch_len = 1;
  call.bwd_value_in[0] = 0;
  call.bwd_value_out = 1;
  expect("branch CALL binds kernel", bind_call(call));
  step.calls.push_back(call);
  step.code.push_back({Program::CALL, 0, 0});
  step.out_regs = {1};
  if (!gen_adjoint(step)) {
    std::printf("FAIL gen_adjoint refused CALL-bearing scan step\n");
    ++failures;
    return;
  }

  auto spec = std::make_shared<ScanSpec>();
  spec->count = 3;
  spec->checkpoint_block = 2;
  spec->carry_cells = 0;
  spec->output_cells = 1;
  spec->templates.resize(1);
  auto& tm = spec->templates[0];
  tm.step = std::move(step);
  tm.udata_pool.push_back(branch);
  tm.inputs.push_back({0, 0, 0, 0, 1, true});
  tm.target_reg = 1;

  Graph g;
  const int x = g.add_slot(1, true);
  const int lp = g.add_slot(1, false);
  g.udata_pool.push_back(spec);
  Op op;
  op.opcode = OP_SCAN;
  op.out = lp;
  op.n_in = 1;
  op.in[0] = x;
  op.udata = g.udata_pool.back().get();
  g.ops.push_back(op);
  g.result_slot = lp;

  Executor ex(std::move(g));
  double grad = 0.0;
  *ex.param_ptr(x) = 0.4;
  expect_close("branch true value", ex.gradient(&grad), 3.0 * std::exp(0.4));
  expect_close("branch true gradient", grad, 3.0 * std::exp(0.4));
  *ex.param_ptr(x) = -0.4;
  expect_close("branch false value", ex.gradient(&grad), 3.0 * 0.16);
  expect_close("branch false gradient", grad, -2.4);
}

// An acceptance-sized scheduled scan: alternating rows stand for subject
// starts and continuations.  Its carry is a packed 2x2 matrix plus a length-2
// vector, while each row reads a disjoint affine slice of the data.  Both
// templates contain the same genuinely parameter-controlled island call.
struct ScheduledCase {
  Graph graph;
  std::shared_ptr<ScanSpec> spec;
  std::weak_ptr<IslandProg> branch_payload;
  int matrix_slot = -1;
  int vector_slot = -1;
  int coefficient_slot = -1;
  int row_slot = -1;
};

static std::shared_ptr<IslandProg> make_parameter_branch() {
  auto branch = std::make_shared<IslandProg>();
  branch->n_regs = 4;
  branch->ins = {{0, 1, 0, 0, true}};
  branch->pool = {0.0};
  // theta > 0 ? exp(theta) : square(theta)
  branch->code = {
      {Program::CONST, 1, 0}, {Program::GT, 2, 0, 1}, {Program::JZ, 5, 2},
      {Program::EXP, 3, 0},   {Program::JMP, 6},      {Program::SQUARE, 3, 0},
  };
  branch->out_regs = {3};
  return branch;
}

static ScanSpec::Template make_subject_template(
    bool subject_start, const std::shared_ptr<IslandProg>& branch) {
  ScanSpec::Template tm;
  IslandProg step;
  // 0:3 matrix carry, 4:5 vector carry, 6:7 active coefficients,
  // 8:9 one inactive data row.  20:25 is the canonical exit carry;
  // 26 and 27 are the row sink and target.
  step.n_regs = 29;
  step.ins = {
      {0, 6, 0, 0, true},
      {6, 2, 1, 0, true},
      {8, 2, 2, 0, false},
  };

  Program::Call call;
  call.opcode = OP_ISLAND;
  call.udata = branch.get();
  call.n_in = 1;
  call.in[0] = 6;
  call.in_len[0] = 1;
  call.out = 10;
  call.out_len = 1;
  call.scratch = 11;
  call.scratch_len = 1;
  expect("subject CALL binds kernel", bind_call(call));
  step.calls.push_back(call);
  step.code.push_back({Program::CALL, 0, 0});

  if (subject_start) {
    // M' = rho * M + (x, y, theta, branch(theta)).
    step.code.push_back({Program::MUL, 12, 7, 0});
    step.code.push_back({Program::ADD, 20, 12, 8});
    step.code.push_back({Program::MUL, 13, 7, 1});
    step.code.push_back({Program::ADD, 21, 13, 9});
    step.code.push_back({Program::MUL, 14, 7, 2});
    step.code.push_back({Program::ADD, 22, 14, 6});
    step.code.push_back({Program::MUL, 15, 7, 3});
    step.code.push_back({Program::ADD, 23, 15, 10});
    // v' uses the old vector and the just-computed diagonal of M'.
    step.code.push_back({Program::MUL, 16, 8, 6});
    step.code.push_back({Program::ADD, 17, 20, 16});
    step.code.push_back({Program::ADD, 24, 4, 17});
    step.code.push_back({Program::MUL, 18, 9, 7});
    step.code.push_back({Program::ADD, 19, 23, 18});
    step.code.push_back({Program::ADD, 25, 5, 19});
  } else {
    // A continuation row couples each matrix row to the preceding vector.
    step.code.push_back({Program::MUL, 12, 7, 4});
    step.code.push_back({Program::ADD, 13, 0, 12});
    step.code.push_back({Program::ADD, 20, 13, 8});
    step.code.push_back({Program::MUL, 14, 7, 5});
    step.code.push_back({Program::ADD, 15, 1, 14});
    step.code.push_back({Program::ADD, 21, 15, 9});
    step.code.push_back({Program::MUL, 16, 6, 8});
    step.code.push_back({Program::ADD, 22, 2, 16});
    step.code.push_back({Program::MUL, 17, 10, 9});
    step.code.push_back({Program::ADD, 23, 3, 17});
    step.code.push_back({Program::ADD, 18, 12, 20});
    step.code.push_back({Program::ADD, 24, 18, 10});
    step.code.push_back({Program::ADD, 19, 14, 23});
    step.code.push_back({Program::SUB, 25, 19, 6});
  }
  step.code.push_back({Program::SUB, 26, 24, 25});
  step.code.push_back({Program::MUL, 28, 24, 25});
  step.code.push_back({Program::ADD, 27, 28, 10});
  step.out_regs = {20, 21, 22, 23, 24, 25, 26, 27};
  if (!gen_adjoint(step)) {
    ++failures;
    std::printf("FAIL gen_adjoint refused scheduled subject template\n");
  }

  tm.step = std::move(step);
  tm.udata_pool.push_back(branch);
  tm.inputs.push_back({2, 0, 0, 6, 2, true});
  tm.inputs.push_back({3, 0, 2, 8, 2, false});
  tm.carry.push_back({0, 0, 0, 20, 4, 0});
  tm.carry.push_back({1, 0, 4, 24, 2, 4});
  tm.sinks.push_back({26, 6, 1, 1});
  tm.target_reg = 27;
  return tm;
}

static ScheduledCase make_scheduled_case(int64_t count, int64_t block) {
  ScheduledCase out;
  out.matrix_slot = out.graph.add_slot(4, true);
  out.vector_slot = out.graph.add_slot(2, true);
  out.coefficient_slot = out.graph.add_slot(2, true);
  out.row_slot = out.graph.add_slot(2 * count, false);
  const int packed = out.graph.add_slot(7 + count, false);

  auto branch = make_parameter_branch();
  out.branch_payload = branch;
  out.spec = std::make_shared<ScanSpec>();
  out.spec->count = count;
  out.spec->checkpoint_block = block;
  out.spec->carry_cells = 6;
  out.spec->output_cells = 7 + count;
  out.spec->templates.push_back(make_subject_template(true, branch));
  out.spec->templates.push_back(make_subject_template(false, branch));
  for (int64_t i = 0; i < count; ++i)
    out.spec->template_for_iteration.push_back(static_cast<uint32_t>(i % 2));

  out.graph.udata_pool.push_back(out.spec);
  Op scan;
  scan.opcode = OP_SCAN;
  scan.out = packed;
  scan.n_in = 4;
  scan.in[0] = out.matrix_slot;
  scan.in[1] = out.vector_slot;
  scan.in[2] = out.coefficient_slot;
  scan.in[3] = out.row_slot;
  scan.udata = out.spec.get();
  out.graph.ops.push_back(scan);

  const int final_carry = out.graph.add_slot(6, false);
  const int carry_sum = out.graph.add_slot(1, false);
  const int sinks = out.graph.add_slot(count, false);
  const int sink_sum = out.graph.add_slot(1, false);
  const int target = out.graph.add_slot(1, false);
  const int result = out.graph.add_slot(1, false);
  out.graph.add_op(OP_SLICE, {packed}, final_carry, {0});
  out.graph.add_op(OP_SUM_VEC, {final_carry}, carry_sum);
  out.graph.add_op(OP_SLICE, {packed}, sinks, {6});
  out.graph.add_op(OP_SUM_VEC, {sinks}, sink_sum);
  out.graph.add_op(OP_INDEX, {packed}, target, {int(6 + count)});
  out.graph.add_op(OP_ADD_N, {carry_sum, sink_sum, target}, result);
  out.graph.result_slot = result;
  return out;
}

struct OracleResult {
  double value = 0.0;
  std::array<double, 8> gradient{};
};

static OracleResult scheduled_oracle(const std::array<double, 8>& parameter,
                                     const std::vector<double>& rows) {
  stan::math::nested_rev_autodiff nested;
  using stan::math::var;
  std::array<var, 8> q;
  for (size_t i = 0; i < q.size(); ++i) q[i] = parameter[i];
  std::array<var, 4> matrix = {q[0], q[1], q[2], q[3]};
  std::array<var, 2> vector = {q[4], q[5]};
  const var theta = q[6];
  const var rho = q[7];
  var target = 0.0;
  var sink_sum = 0.0;
  for (size_t i = 0; i < rows.size() / 2; ++i) {
    const double x = rows[2 * i];
    const double y = rows[2 * i + 1];
    const var branch =
        theta.val() > 0.0 ? stan::math::exp(theta) : stan::math::square(theta);
    std::array<var, 4> next_matrix;
    std::array<var, 2> next_vector;
    if (i % 2 == 0) {
      next_matrix = {rho * matrix[0] + x, rho * matrix[1] + y,
                     rho * matrix[2] + theta, rho * matrix[3] + branch};
      next_vector = {vector[0] + (next_matrix[0] + x * theta),
                     vector[1] + (next_matrix[3] + y * rho)};
    } else {
      next_matrix = {matrix[0] + rho * vector[0] + x,
                     matrix[1] + rho * vector[1] + y, matrix[2] + theta * x,
                     matrix[3] + branch * y};
      next_vector = {rho * vector[0] + next_matrix[0] + branch,
                     rho * vector[1] + next_matrix[3] - theta};
    }
    sink_sum += next_vector[0] - next_vector[1];
    target += next_vector[0] * next_vector[1] + branch;
    matrix = std::move(next_matrix);
    vector = std::move(next_vector);
  }
  var result = target + sink_sum;
  for (const var& x : matrix) result += x;
  for (const var& x : vector) result += x;
  stan::math::grad(result.vi_);

  OracleResult out;
  out.value = result.val();
  for (size_t i = 0; i < q.size(); ++i) out.gradient[i] = q[i].adj();
  return out;
}

static void bind_scheduled_case(Executor& ex, const ScheduledCase& c,
                                const std::array<double, 8>& parameter,
                                const std::vector<double>& rows) {
  std::copy_n(parameter.data(), 4, ex.param_ptr(c.matrix_slot));
  std::copy_n(parameter.data() + 4, 2, ex.param_ptr(c.vector_slot));
  std::copy_n(parameter.data() + 6, 2, ex.param_ptr(c.coefficient_slot));
  std::copy(rows.begin(), rows.end(), ex.value_ptr(c.row_slot));
}

static void check_scheduled_result(const std::string& tag, Executor& ex,
                                   const OracleResult& want) {
  std::array<double, 8> gradient{};
  expect_close(tag + " value", ex.gradient(gradient.data()), want.value);
  for (size_t i = 0; i < gradient.size(); ++i)
    expect_close(tag + " gradient " + std::to_string(i), gradient[i],
                 want.gradient[i]);
}

static void run_scheduled_acceptance_case(int64_t count, int64_t block) {
  std::array<double, 8> parameter = {
      0.2, -0.3, 0.4, 0.1, 0.5, -0.2, count == 2 ? -0.35 : 0.25, 0.6,
  };
  std::vector<double> rows(static_cast<size_t>(2 * count));
  for (int64_t i = 0; i < count; ++i) {
    rows[static_cast<size_t>(2 * i)] = 0.1 * double(i + 1);
    rows[static_cast<size_t>(2 * i + 1)] = -0.15 + 0.07 * double(i);
  }
  const OracleResult want = scheduled_oracle(parameter, rows);
  ScheduledCase c = make_scheduled_case(count, block);
  const std::string tag = "scheduled N=" + std::to_string(count) +
                          " block=" + std::to_string(block);
  expect(tag + " nested branch payload owned before bind",
         !c.branch_payload.expired());
  for (const auto& tm : c.spec->templates) {
    expect(tag + " template owns nested branch payload",
           tm.udata_pool.size() == 1 && !tm.step.calls.empty() &&
               tm.step.calls[0].udata == tm.udata_pool[0].get());
    expect(tag + " uses affine two-cell row binding",
           tm.inputs.size() == 2 && tm.inputs[1].iteration_stride == 2 &&
               tm.inputs[1].len == 2);
  }

  std::unique_ptr<Executor> copied;
  {
    Executor ex(std::move(c.graph));
    c.spec.reset();  // leave the executor as the sole top-level payload owner
    bind_scheduled_case(ex, c, parameter, rows);
    expect(tag + " nested branch payload owned by executor",
           !c.branch_payload.expired());
    check_scheduled_result(tag, ex, want);
    check_scheduled_result(tag + " repeated", ex, want);
    copied = std::make_unique<Executor>(ex);
  }
  expect(tag + " nested branch payload owned by copied executor",
         !c.branch_payload.expired());
  check_scheduled_result(tag + " copied", *copied, want);
}

template <typename Mutate>
static void expect_schedule_rejected(const std::string& tag, Mutate mutate) {
  ScheduledCase c = make_scheduled_case(2, 1);
  mutate(*c.spec);
  try {
    Executor ex(std::move(c.graph));
    (void)ex;
    expect(tag, false);
  } catch (const std::invalid_argument&) {
  }
}

static void reject_invalid_schedules() {
  expect_schedule_rejected(
      "multi-template scan needs explicit schedule",
      [](ScanSpec& s) { s.template_for_iteration.clear(); });
  expect_schedule_rejected("scan rejects short schedule", [](ScanSpec& s) {
    s.template_for_iteration.pop_back();
  });
  expect_schedule_rejected(
      "scan rejects missing scheduled template",
      [](ScanSpec& s) { s.template_for_iteration[1] = 2; });
  expect_schedule_rejected(
      "scan rejects mismatched carry schemas",
      [](ScanSpec& s) { s.templates[1].carry[1].output_offset = 3; });
  expect_schedule_rejected(
      "scan rejects an inactive direct parameter binding",
      [](ScanSpec& s) { s.templates[0].inputs[0].active = false; });
  expect_schedule_rejected(
      "scan rejects an unowned CALL payload",
      [](ScanSpec& s) { s.templates[0].udata_pool.clear(); });
  expect_schedule_rejected(
      "scan rejects overlapping step input destinations",
      [](ScanSpec& s) { s.templates[0].inputs[0].step_reg = 0; });
}

static void reject_overlapping_output_bindings() {
  Graph g;
  const int init = g.add_slot(1, true);
  const int scale = g.add_slot(1, true);
  const int rows = g.add_slot(1, false);
  const int out = g.add_slot(3, false);
  auto spec = std::make_shared<ScanSpec>(make_spec(1, 1));
  spec->templates[0].sinks[0].output_offset = 0;  // final-carry range
  g.udata_pool.push_back(spec);
  Op op;
  op.opcode = OP_SCAN;
  op.out = out;
  op.n_in = 3;
  op.in[0] = init;
  op.in[1] = scale;
  op.in[2] = rows;
  op.udata = g.udata_pool.back().get();
  g.ops.push_back(op);
  try {
    Executor ex(std::move(g));
    (void)ex;
    ++failures;
    std::printf("FAIL overlapping scan outputs were accepted\n");
  } catch (const std::invalid_argument&) {
  }
}

static ScanSpec::Template make_invariant_call_template(
    bool square_invariant, bool active_invariant = false,
    bool clobber_invariant_output = false) {
  ScanSpec::Template tm;
  IslandProg step;
  // invariant, row, carry, theta; three CALL results; two sums, target, carry.
  step.n_regs = 11;
  step.ins = {{0, 1, 0, 0, active_invariant},
              {1, 1, 1, 0, false},
              {2, 1, 2, 0, false},
              {3, 1, 3, 0, true}};
  const auto add_call = [&](uint16_t opcode, int input, int output) {
    Program::Call call;
    call.opcode = opcode;
    call.n_in = 1;
    call.in[0] = input;
    call.in_len[0] = 1;
    call.out = output;
    call.out_len = 1;
    expect("invariant CALL binds kernel", bind_call(call));
    step.calls.push_back(std::move(call));
    step.code.push_back(
        {Program::CALL, 0, static_cast<int32_t>(step.calls.size() - 1)});
  };
  add_call(square_invariant ? OP_SQUARE : OP_EXP, 0, 4);
  add_call(OP_EXP, 1, 5);  // row-varying, although parameter-inactive
  add_call(OP_EXP, 2, 6);  // carry-varying, although parameter-inactive
  if (clobber_invariant_output) {
    step.pool.push_back(1.25);
    step.code.push_back({Program::CONST, 4, 0});
  }
  step.code.push_back({Program::ADD, 7, 4, 5});
  step.code.push_back({Program::ADD, 8, 7, 6});
  step.code.push_back({Program::MUL, 9, 3, 8});
  step.code.push_back({Program::ADD, 10, 2, 1});
  step.out_regs = {10, 9};
  expect("invariant CALL step adjoint generated", gen_adjoint(step));

  tm.step = std::move(step);
  tm.inputs.push_back({1, 0, 0, 0, 1, active_invariant});
  tm.inputs.push_back({2, 0, 1, 1, 1, false});
  tm.inputs.push_back({3, 0, 0, 3, 1, true});
  tm.carry.push_back({0, 0, 2, 10, 1, 0});
  tm.sinks.push_back({9, 1, 1, 1});
  tm.target_reg = 9;
  const size_t prepared = prepare_scan_invariants(&tm);
  if (active_invariant) {
    expect("active invariant CALL remains in the backward", prepared == 0);
  } else if (clobber_invariant_output) {
    expect("clobbered invariant CALL output is not cached", prepared == 0);
  } else {
    expect("one invariant CALL prepared", prepared == 1);
  }
  int repeated_calls = 0;
  for (const Program::Instr& I : tm.repeated_code)
    repeated_calls += I.code == Program::CALL ? 1 : 0;
  if (!active_invariant && !clobber_invariant_output)
    expect("row and carry inactive CALLs remain per-row", repeated_calls == 2);
  return tm;
}

struct InvariantCallCase {
  Graph graph;
  std::shared_ptr<ScanSpec> spec;
  int initial = -1;
  int invariant = -1;
  int rows = -1;
  int theta = -1;
};

static InvariantCallCase make_invariant_call_scan(int64_t count,
                                                  int64_t block) {
  InvariantCallCase out;
  out.initial = out.graph.add_slot(1, false);
  out.invariant = out.graph.add_slot(1, false);
  out.rows = out.graph.add_slot(count, false);
  out.theta = out.graph.add_slot(1, true);
  const int packed = out.graph.add_slot(count + 2, false);
  out.spec = std::make_shared<ScanSpec>();
  out.spec->count = count;
  out.spec->checkpoint_block = block;
  out.spec->carry_cells = 1;
  out.spec->output_cells = count + 2;
  out.spec->templates.push_back(make_invariant_call_template(false));
  out.spec->templates.push_back(make_invariant_call_template(true));
  for (int64_t i = 0; i < count; ++i)
    out.spec->template_for_iteration.push_back(static_cast<uint32_t>(i % 2));
  out.graph.udata_pool.push_back(out.spec);
  Op scan;
  scan.opcode = OP_SCAN;
  scan.out = packed;
  scan.n_in = 4;
  scan.in[0] = out.initial;
  scan.in[1] = out.invariant;
  scan.in[2] = out.rows;
  scan.in[3] = out.theta;
  scan.udata = out.spec.get();
  out.graph.ops.push_back(scan);
  const int final = out.graph.add_slot(1, false);
  const int sinks = out.graph.add_slot(count, false);
  const int sink_sum = out.graph.add_slot(1, false);
  const int target = out.graph.add_slot(1, false);
  const int result = out.graph.add_slot(1, false);
  out.graph.add_op(OP_INDEX, {packed}, final, {0});
  out.graph.add_op(OP_SLICE, {packed}, sinks, {1});
  out.graph.add_op(OP_SUM_VEC, {sinks}, sink_sum);
  out.graph.add_op(OP_INDEX, {packed}, target, {static_cast<int>(count + 1)});
  out.graph.add_op(OP_ADD_N, {final, sink_sum, target}, result);
  out.graph.result_slot = result;
  return out;
}

static InvariantCallCase make_invariant_call_unrolled(int64_t count) {
  InvariantCallCase out;
  out.initial = out.graph.add_slot(1, false);
  out.invariant = out.graph.add_slot(1, false);
  out.rows = out.graph.add_slot(count, false);
  out.theta = out.graph.add_slot(1, true);
  int carry = out.initial;
  int result = -1;
  for (int64_t i = 0; i < count; ++i) {
    const int row = out.graph.add_slot(1, false);
    const int invariant_value = out.graph.add_slot(1, false);
    const int row_value = out.graph.add_slot(1, false);
    const int carry_value = out.graph.add_slot(1, false);
    const int partial = out.graph.add_slot(1, false);
    const int total = out.graph.add_slot(1, false);
    const int term = out.graph.add_slot(1, false);
    const int doubled = out.graph.add_slot(1, false);
    const int next_result = i == 0 ? doubled : out.graph.add_slot(1, false);
    const int next_carry = out.graph.add_slot(1, false);
    out.graph.add_op(OP_INDEX, {out.rows}, row, {static_cast<int>(i)});
    out.graph.add_op(i % 2 == 0 ? OP_EXP : OP_SQUARE, {out.invariant},
                     invariant_value);
    out.graph.add_op(OP_EXP, {row}, row_value);
    out.graph.add_op(OP_EXP, {carry}, carry_value);
    out.graph.add_op(OP_ADD, {invariant_value, row_value}, partial);
    out.graph.add_op(OP_ADD, {partial, carry_value}, total);
    out.graph.add_op(OP_MUL, {out.theta, total}, term);
    out.graph.add_op(OP_ADD, {term, term}, doubled);
    if (i > 0) out.graph.add_op(OP_ADD, {result, doubled}, next_result);
    out.graph.add_op(OP_ADD, {carry, row}, next_carry);
    result = next_result;
    carry = next_carry;
  }
  const int final_result = out.graph.add_slot(1, false);
  out.graph.add_op(OP_ADD, {result, carry}, final_result);
  out.graph.result_slot = final_result;
  return out;
}

static void bind_invariant_call_case(Executor& ex, const InvariantCallCase& c,
                                     double initial, double invariant,
                                     const std::vector<double>& rows,
                                     double theta) {
  *ex.value_ptr(c.initial) = initial;
  *ex.value_ptr(c.invariant) = invariant;
  for (size_t i = 0; i < rows.size(); ++i) ex.value_ptr(c.rows)[i] = rows[i];
  *ex.param_ptr(c.theta) = theta;
}

static void run_invariant_call_case() {
  constexpr int64_t count = 7;
  InvariantCallCase scan_case =
      make_invariant_call_scan(count, choose_scan_checkpoint_block(count, 1));
  InvariantCallCase blocked_case = make_invariant_call_scan(
      count, choose_scan_checkpoint_block(count, 1, 0));
  InvariantCallCase unrolled_case = make_invariant_call_unrolled(count);
  Executor scan(std::move(scan_case.graph));
  Executor blocked(std::move(blocked_case.graph));
  Executor unrolled(std::move(unrolled_case.graph));
  const auto compare = [&](const std::string& tag, double initial,
                           double invariant, std::vector<double> rows,
                           double theta) {
    bind_invariant_call_case(scan, scan_case, initial, invariant, rows, theta);
    bind_invariant_call_case(blocked, blocked_case, initial, invariant, rows,
                             theta);
    bind_invariant_call_case(unrolled, unrolled_case, initial, invariant, rows,
                             theta);
    double scan_grad = 0.0;
    double blocked_grad = 0.0;
    double unrolled_grad = 0.0;
    const double scan_value = scan.gradient(&scan_grad);
    const double blocked_value = blocked.gradient(&blocked_grad);
    const double unrolled_value = unrolled.gradient(&unrolled_grad);
    expect_exact(tag + " checkpoint value", scan_value, blocked_value);
    expect_exact(tag + " checkpoint gradient", scan_grad, blocked_grad);
    expect_close(tag + " value", scan_value, unrolled_value);
    expect_close(tag + " gradient", scan_grad, unrolled_grad);
  };
  compare("invariant CALL first", 0.4, 0.2,
          {0.1, -0.2, 0.3, 0.05, -0.1, 0.2, 0.4}, 0.7);
  // A fresh forward must invalidate both template caches before inputs change.
  compare("invariant CALL repeated", -0.1, 0.6,
          {-0.3, 0.15, 0.05, 0.2, -0.25, 0.1, 0.35}, -0.4);
  Executor copied(scan);
  double copied_grad = 0.0;
  double scan_grad = 0.0;
  expect_close("invariant CALL copied value", copied.gradient(&copied_grad),
               scan.gradient(&scan_grad));
  expect_close("invariant CALL copied gradient", copied_grad, scan_grad);

  // One row has no target-reduction regrouping, so the scan, cached Program,
  // and ordinary unrolled graph must agree exactly in both sweeps.
  InvariantCallCase exact_scan_case = make_invariant_call_scan(1, 1);
  InvariantCallCase exact_unrolled_case = make_invariant_call_unrolled(1);
  Executor exact_scan(std::move(exact_scan_case.graph));
  Executor exact_unrolled(std::move(exact_unrolled_case.graph));
  bind_invariant_call_case(exact_scan, exact_scan_case, 0.4, 0.2, {0.1}, 0.7);
  bind_invariant_call_case(exact_unrolled, exact_unrolled_case, 0.4, 0.2, {0.1},
                           0.7);
  double exact_scan_grad = 0.0;
  double exact_unrolled_grad = 0.0;
  expect_exact("scan/unrolled exact value",
               exact_scan.gradient(&exact_scan_grad),
               exact_unrolled.gradient(&exact_unrolled_grad));
  expect_exact("scan/unrolled exact gradient", exact_scan_grad,
               exact_unrolled_grad);
}

static void reject_unsound_invariant_call_plans() {
  (void)make_invariant_call_template(false, true, false);
  (void)make_invariant_call_template(false, false, true);

  ScanSpec::Template branched = make_invariant_call_template(false);
  branched.invariant_calls.clear();
  branched.repeated_code.clear();
  branched.invariant_cache_cells = 0;
  branched.step.adj = {};
  branched.step.pool.push_back(1.0);
  ++branched.step.n_regs;
  std::vector<Program::Instr> branch_code;
  branch_code.push_back({Program::CONST, branched.step.n_regs - 1,
                         static_cast<int32_t>(branched.step.pool.size() - 1)});
  branch_code.push_back({Program::JZ, 0, branched.step.n_regs - 1});
  branch_code.insert(branch_code.end(), branched.step.code.begin(),
                     branched.step.code.end());
  branch_code[1].dst = static_cast<int32_t>(branch_code.size());
  branched.step.code = std::move(branch_code);
  expect("branched invariant CALL step adjoint generated",
         gen_adjoint(branched.step));
  expect("branched template is not rewritten",
         prepare_scan_invariants(&branched) == 0);

  InvariantCallCase scan_case = make_invariant_call_scan(1, 1);
  scan_case.spec->templates[0].repeated_code.pop_back();
  try {
    Executor ex(std::move(scan_case.graph));
    (void)ex;
    ++failures;
    std::printf("FAIL stale invariant CALL plan was accepted\n");
  } catch (const std::invalid_argument&) {
  }
}

// A small, hand-built version of the production nesting shape:
//
//   OP_SCAN step -> OP_ISLAND -> traced native CFG -> prepared prim-LU CALL
//
// The solve result is copied to the island output and then overwritten. This
// deliberately forces bwd_value_out != out and pins the immediate generated
// checkpoint that retained replay must repopulate after restoring call.out.
struct PreparedRetentionCase {
  Graph graph;
  std::shared_ptr<ScanSpec> spec;
  std::shared_ptr<IslandProg> inner;
  int matrix = -1;
  int rhs = -1;
  int gates = -1;
};

static void clear_prepared_retention_environment() {
  test_unsetenv("STANLI_SCAN_PREPARED_RETENTION");
  test_unsetenv("STANLI_NO_SCAN_PREPARED_RETENTION");
  test_unsetenv("STANLI_CFG_PREPARED_MDIVIDE_LEFT_PRIM_LU");
  test_unsetenv("STANLI_CFG_PREPARED_MDIVIDE_LEFT_PRIM_LU_MIN_N");
  test_unsetenv("STANLI_NO_CFG_PREPARED_MDIVIDE_LEFT_PRIM_LU");
}

static std::shared_ptr<IslandProg> make_prepared_retention_inner() {
  test_setenv("STANLI_CFG_PREPARED_MDIVIDE_LEFT_PRIM_LU", "1", 1);
  auto inner = std::make_shared<IslandProg>();
  inner->n_regs = 13;
  inner->ins = {{0, 4, 0, 0, true}, {4, 2, 1, 0, true}, {6, 1, 2, 0, false}};
  inner->pool = {0.0};
  inner->code = {
      {Program::MOVR, 11, 4, 0, 0, 2},
      {Program::CONST, 7, 0},
      {Program::GT, 8, 6, 7},
      {Program::JZ, 6, 8},
      {Program::MDIVIDE_LEFT, 9, 0, 4, -2, 2},
      {Program::MOVR, 11, 9, 0, 0, 2},
      {Program::MOVR, 9, 4, 0, 0, 2},
  };
  inner->out_regs = {11, 12};
  expect("retention inner CFG adjoint generated", gen_cfg_adjoint(*inner));
  test_unsetenv("STANLI_CFG_PREPARED_MDIVIDE_LEFT_PRIM_LU");
  inner->native_adj = !inner->adj.empty() && inner->adj.trace_bits > 0;
  expect("retention inner is traced native", inner->native_adj);

  int prepared_pc = -1;
  int prepared_call = -1;
  for (size_t pc = 0; pc < inner->code.size(); ++pc) {
    const Program::Instr& instruction = inner->code[pc];
    if (instruction.code != Program::CALL || instruction.a < 0 ||
        static_cast<size_t>(instruction.a) >= inner->calls.size())
      continue;
    if (inner->calls[static_cast<size_t>(instruction.a)].opcode ==
        OP_MDIVIDE_LEFT_PREPARED_PRIM_LU) {
      prepared_pc = static_cast<int>(pc);
      prepared_call = instruction.a;
    }
  }
  expect("retention inner has one prepared prim-LU CALL",
         prepared_pc >= 0 && prepared_call >= 0);
  if (prepared_pc >= 0 && prepared_call >= 0) {
    const Program::Call& call =
        inner->calls[static_cast<size_t>(prepared_call)];
    expect("retention solve has exact scratch", call.scratch_len == 7);
    expect("retention solve output is checkpointed",
           call.bwd_value_out != call.out);
    expect(
        "retention solve checkpoint follows CALL",
        static_cast<size_t>(prepared_pc + 1) < inner->code.size() &&
            inner->code[static_cast<size_t>(prepared_pc + 1)].code ==
                Program::MOVR &&
            inner->code[static_cast<size_t>(prepared_pc + 1)].dst ==
                call.bwd_value_out &&
            inner->code[static_cast<size_t>(prepared_pc + 1)].a == call.out &&
            inner->trace_pc[static_cast<size_t>(prepared_pc + 1)] == -1);
  }
  return inner;
}

static std::shared_ptr<ScanSpec> make_prepared_retention_spec(
    int64_t count, int64_t block, std::shared_ptr<IslandProg>* inner_out) {
  auto inner = make_prepared_retention_inner();
  ScanSpec::Template tm;
  IslandProg step;
  const int32_t island_scratch =
      inner->n_regs + (inner->adj.trace_bits + 63) / 64;
  const int32_t target_reg = 9 + island_scratch;
  step.n_regs = target_reg + 1;
  step.ins = {{0, 4, 0, 0, true}, {4, 2, 1, 0, true}, {6, 1, 2, 0, false}};
  Program::Call outer;
  outer.opcode = OP_ISLAND;
  outer.n_in = 3;
  outer.in[0] = 0;
  outer.in_len[0] = 4;
  outer.in[1] = 4;
  outer.in_len[1] = 2;
  outer.in[2] = 6;
  outer.in_len[2] = 1;
  outer.out = 7;
  outer.out_len = 2;
  outer.scratch = 9;
  outer.scratch_len = island_scratch;
  outer.udata = inner.get();
  expect("retention outer CALL binds kernel", bind_call(outer));
  step.calls.push_back(outer);
  step.code.push_back({Program::CALL, 0, 0});
  step.code.push_back({Program::ADD, target_reg, 7, 8});
  step.out_regs = {target_reg};
  expect("retention outer step adjoint generated", gen_adjoint(step));

  tm.step = std::move(step);
  tm.udata_pool.push_back(inner);
  tm.inputs.push_back({0, 0, 0, 0, 4, true});
  tm.inputs.push_back({1, 0, 0, 4, 2, true});
  tm.inputs.push_back({2, 0, 1, 6, 1, false});
  tm.target_reg = target_reg;

  auto spec = std::make_shared<ScanSpec>();
  spec->count = count;
  spec->checkpoint_block = block;
  spec->carry_cells = 0;
  spec->output_cells = 1;
  spec->templates.push_back(std::move(tm));
  if (inner_out != nullptr) *inner_out = std::move(inner);
  return spec;
}

static PreparedRetentionCase make_prepared_retention_case(int64_t block,
                                                          bool retain) {
  constexpr int64_t count = 3;
  PreparedRetentionCase out;
  out.spec = make_prepared_retention_spec(count, block, &out.inner);
  if (retain) test_setenv("STANLI_SCAN_PREPARED_RETENTION", "1", 1);
  const size_t prepared = prepare_scan_prepared_retention(out.spec.get());
  test_unsetenv("STANLI_SCAN_PREPARED_RETENTION");
  expect(retain ? "prepared retention plan selected"
                : "prepared retention stays default-off",
         prepared == (retain ? 1u : 0u));
  expect(retain ? "prepared retention exact packed cells"
                : "default-off retention has no cells",
         out.spec->prepared_retention_cells == (retain ? 30 : 0));

  out.matrix = out.graph.add_slot(4, true);
  out.rhs = out.graph.add_slot(2, true);
  out.gates = out.graph.add_slot(count, false);
  const int target = out.graph.add_slot(1, false);
  out.graph.udata_pool.push_back(out.spec);
  Op scan;
  scan.opcode = OP_SCAN;
  scan.out = target;
  scan.n_in = 3;
  scan.in[0] = out.matrix;
  scan.in[1] = out.rhs;
  scan.in[2] = out.gates;
  scan.udata = out.spec.get();
  out.graph.ops.push_back(scan);
  out.graph.result_slot = target;
  return out;
}

struct PreparedRetentionResult {
  double value = 0.0;
  std::array<double, 6> gradient{};
};

static void bind_prepared_retention_case(Executor& ex,
                                         const PreparedRetentionCase& c) {
  const std::array<double, 4> matrix = {2.0, 0.2, 0.1, 1.5};
  const std::array<double, 2> rhs = {1.0, -0.5};
  const std::array<double, 3> gates = {1.0, -1.0, 1.0};
  std::copy(matrix.begin(), matrix.end(), ex.param_ptr(c.matrix));
  std::copy(rhs.begin(), rhs.end(), ex.param_ptr(c.rhs));
  std::copy(gates.begin(), gates.end(), ex.value_ptr(c.gates));
}

static PreparedRetentionResult evaluate_prepared_retention(
    Executor& ex, const PreparedRetentionCase& c) {
  bind_prepared_retention_case(ex, c);
  PreparedRetentionResult result;
  result.value = ex.gradient(result.gradient.data());
  return result;
}

static void expect_retention_exact(const std::string& tag,
                                   const PreparedRetentionResult& got,
                                   const PreparedRetentionResult& want) {
  expect_exact(tag + " value", got.value, want.value);
  for (size_t i = 0; i < got.gradient.size(); ++i)
    expect_exact(tag + " gradient " + std::to_string(i), got.gradient[i],
                 want.gradient[i]);
}

static PreparedRetentionResult prepared_retention_oracle() {
  stan::math::nested_rev_autodiff nested;
  Eigen::Matrix<stan::math::var, 2, 2> matrix;
  Eigen::Matrix<stan::math::var, 2, 1> rhs;
  const std::array<double, 4> matrix_value = {2.0, 0.2, 0.1, 1.5};
  const std::array<double, 2> rhs_value = {1.0, -0.5};
  for (int i = 0; i < 4; ++i) matrix.data()[i] = matrix_value[i];
  for (int i = 0; i < 2; ++i) rhs.data()[i] = rhs_value[i];
  const auto solved = stan::math::mdivide_left(matrix, rhs);
  stan::math::var target = 2.0 * (solved(0) + solved(1)) + rhs(0) + rhs(1);
  stan::math::grad(target.vi_);
  PreparedRetentionResult out;
  out.value = target.val();
  for (int i = 0; i < 4; ++i)
    out.gradient[static_cast<size_t>(i)] = matrix.data()[i].adj();
  for (int i = 0; i < 2; ++i)
    out.gradient[static_cast<size_t>(4 + i)] = rhs.data()[i].adj();
  return out;
}

static void check_prepared_retention_record_validity() {
  PreparedRetentionCase c = make_prepared_retention_case(1, true);
  const Kernel* kernel = find_kernel(OP_SCAN);
  expect("retention scan kernel available", kernel != nullptr);
  if (kernel == nullptr) return;
  Op& scan = c.graph.ops.front();
  const int64_t scratch_cells =
      kernel->scratch_size(scan, c.graph.slots.data());
  std::vector<double> scratch(static_cast<size_t>(scratch_cells), 0.0);
  std::array<double, 4> matrix = {2.0, 0.2, 0.1, 1.5};
  std::array<double, 2> rhs = {1.0, -0.5};
  std::array<double, 3> gates = {1.0, -1.0, 1.0};
  double output = 0.0;
  KernelCtx ctx;
  ctx.n_in = 3;
  ctx.in[0] = Desc{matrix.data(), 4};
  ctx.in[1] = Desc{rhs.data(), 2};
  ctx.in[2] = Desc{gates.data(), 3};
  ctx.out = Desc{&output, 1};
  ctx.scratch = scratch.data();
  ctx.udata = c.spec.get();
  kernel->forward(ctx);
  const double* const retained =
      scratch.data() + scratch_cells - c.spec->prepared_retention_cells;
  for (size_t row = 0; row < gates.size(); ++row) {
    const int64_t offset = c.spec->prepared_retention_iteration_offsets[row];
    expect_exact("retention row " + std::to_string(row) + " validity",
                 retained[offset], gates[row] > 0.0 ? 1.0 : 0.0);
  }
}

static void check_prepared_retention_preparation() {
  clear_prepared_retention_environment();
  auto spec = make_prepared_retention_spec(3, 1, nullptr);
  expect("retention preparation is default-off",
         prepare_scan_prepared_retention(spec.get()) == 0 &&
             spec->prepared_retention_cells == 0);

  test_setenv("STANLI_SCAN_PREPARED_RETENTION", "1", 1);
  test_setenv("STANLI_NO_SCAN_PREPARED_RETENTION", "1", 1);
  expect("retention escape overrides force",
         prepare_scan_prepared_retention(spec.get()) == 0 &&
             spec->prepared_retention_cells == 0);
  test_unsetenv("STANLI_NO_SCAN_PREPARED_RETENTION");
  expect("retention force prepares exact call",
         prepare_scan_prepared_retention(spec.get()) == 1 &&
             spec->prepared_retention_cells == 30);
  test_unsetenv("STANLI_SCAN_PREPARED_RETENTION");
  expect("retention plan remains immutable after environment capture",
         spec->prepared_retention_cells == 30);
  expect("default re-preparation clears prior force plan",
         prepare_scan_prepared_retention(spec.get()) == 0 &&
             spec->prepared_retention_cells == 0);

  test_setenv("STANLI_SCAN_PREPARED_RETENTION", "1", 1);
  expect("retention force re-prepares",
         prepare_scan_prepared_retention(spec.get()) == 1);
  // An outer branch has no scan-level trace in this tranche. Re-preparation
  // must fail closed and clear the formerly valid plan transactionally.
  spec->templates[0].step.code.push_back(
      {Program::JMP,
       static_cast<int32_t>(spec->templates[0].step.code.size() + 1)});
  expect("retention outer branch fails closed",
         prepare_scan_prepared_retention(spec.get()) == 0 &&
             spec->prepared_retention_cells == 0 &&
             spec->templates[0].prepared_solve_retention.empty());
  test_unsetenv("STANLI_SCAN_PREPARED_RETENTION");

  auto malformed = make_prepared_retention_spec(3, 1, nullptr);
  auto& inner = *static_cast<IslandProg*>(
      const_cast<void*>(malformed->templates[0].step.calls[0].udata));
  for (Program::Call& call : inner.calls)
    if (call.opcode == OP_MDIVIDE_LEFT_PREPARED_PRIM_LU) --call.scratch_len;
  test_setenv("STANLI_SCAN_PREPARED_RETENTION", "1", 1);
  expect("malformed prepared scratch fails closed",
         prepare_scan_prepared_retention(malformed.get()) == 0 &&
             malformed->prepared_retention_cells == 0);
  test_unsetenv("STANLI_SCAN_PREPARED_RETENTION");

  PreparedRetentionCase stale = make_prepared_retention_case(1, true);
  stale.spec->templates[0].prepared_solve_retention[0].record_offset = 1;
  try {
    Executor ex(std::move(stale.graph));
    (void)ex;
    expect("stale prepared retention plan rejected", false);
  } catch (const std::invalid_argument&) {
  }
  clear_prepared_retention_environment();
}

static void run_prepared_retention_case() {
  clear_prepared_retention_environment();
  check_prepared_retention_preparation();
  check_prepared_retention_record_validity();

  PreparedRetentionCase off1 = make_prepared_retention_case(1, false);
  PreparedRetentionCase on1 = make_prepared_retention_case(1, true);
  PreparedRetentionCase off2 = make_prepared_retention_case(2, false);
  PreparedRetentionCase on2 = make_prepared_retention_case(2, true);
  Executor off1_executor(std::move(off1.graph));
  Executor on1_executor(std::move(on1.graph));
  Executor off2_executor(std::move(off2.graph));
  Executor on2_executor(std::move(on2.graph));
  const PreparedRetentionResult off1_result =
      evaluate_prepared_retention(off1_executor, off1);
  const PreparedRetentionResult on1_result =
      evaluate_prepared_retention(on1_executor, on1);
  const PreparedRetentionResult off2_result =
      evaluate_prepared_retention(off2_executor, off2);
  const PreparedRetentionResult on2_result =
      evaluate_prepared_retention(on2_executor, on2);
  expect_retention_exact("retention block1 on/off", on1_result, off1_result);
  expect_retention_exact("retention block2 on/off", on2_result, off2_result);
  expect_retention_exact("retention block1/block2", on2_result, on1_result);

  const PreparedRetentionResult oracle = prepared_retention_oracle();
  expect_close("retention oracle value", on1_result.value, oracle.value);
  for (size_t i = 0; i < oracle.gradient.size(); ++i)
    expect_close("retention oracle gradient " + std::to_string(i),
                 on1_result.gradient[i], oracle.gradient[i]);

  const PreparedRetentionResult repeated =
      evaluate_prepared_retention(on1_executor, on1);
  expect_retention_exact("retention repeated", repeated, on1_result);
  Executor copied(on1_executor);
  const PreparedRetentionResult copied_result =
      evaluate_prepared_retention(copied, on1);
  expect_retention_exact("retention copied executor", copied_result,
                         on1_result);
  clear_prepared_retention_environment();
}

int main() {
  if (find_kernel(OP_SCAN) == nullptr) {
    std::printf("FAIL OP_SCAN is not registered\n");
    return 1;
  }
  run_checkpoint_policy_case();
  run_case(1, 1);
  run_case(5, 2);
  run_case(0, 1);
  run_vector_schedule_case();
  run_recycled_adjoint_case();
  run_reset_carry_case();
  run_identity_carry_case();
  run_mixed_carry_activity_case(1, {0, 1, 2, 0, 1, 2}, 56.0, 2.0);
  run_mixed_carry_activity_case(4, {0, 1, 2, 0, 1, 2}, 56.0, 2.0);
  run_mixed_carry_activity_case(1, {1, 0, 2}, 20.0, 3.0);
  run_mixed_carry_activity_case(3, {1, 0, 2}, 20.0, 3.0);
  run_invariant_call_case();
  reject_unsound_invariant_call_plans();
  run_prepared_retention_case();
  run_branch_call_case();
  for (int64_t count : {1, 2, 7}) {
    run_scheduled_acceptance_case(count, 1);
    const int64_t sqrt_block = std::max<int64_t>(
        2, static_cast<int64_t>(std::ceil(std::sqrt(double(count)))));
    run_scheduled_acceptance_case(count, sqrt_block);
  }
  reject_invalid_schedules();
  reject_overlapping_output_bindings();
  return failures == 0 ? 0 : 1;
}
