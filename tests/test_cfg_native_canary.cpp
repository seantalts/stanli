// Durable non-ctsem canaries for the forward-CFG generated adjoint.
//
// pr236_island is a prior-issue matrix/indexing regression with a
// parameter-dependent if/else.  It is deliberately the timed canary used by
// tools/bench_cfg_native_canary.py: unlike a synthetic register program, it
// has passed through transformed MIR, necessity-region lowering, compaction,
// graph execution, and the island kernel.
//
// paramcond_diag_multiply separately keeps structured DIAG_PRE/POST rules in
// that same CFG path.  It is a structural/numeric gate, not the timed case:
// at 4x4 the trace setup is larger than the work it replaces.
#include <stanli/compile.hpp>
#include <stanli/graph.hpp>
#include <stanli/island.hpp>
#include <stanli/optable.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace stanli;

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
  if (condition) return;
  ++failures;
  std::printf("FAIL %s\n", message.c_str());
}

std::string slurp(const std::string& path) {
  std::ifstream input(path);
  std::ostringstream text;
  text << input.rdbuf();
  return text.str();
}

struct Canary {
  CompiledModel model;
  IslandProg* program = nullptr;
};

Canary compile_canary(const char* fixture) {
  const std::string stem = std::string("tests/fixtures/") + fixture;
  DataMap data = DataMap::from_json(slurp("tests/fixtures/paramcond.json"));
  Canary canary{compile_model(slurp(stem + ".tmir.sexp"), data), nullptr};
  for (const Op& op : canary.model.graph.ops) {
    if (op.opcode != OP_ISLAND || op.udata == nullptr) continue;
    auto* candidate =
        const_cast<IslandProg*>(static_cast<const IslandProg*>(op.udata));
    if (candidate->adj.trace_bits > 0) {
      canary.program = candidate;
      break;
    }
  }
  return canary;
}

void expect_exact(double got, double want, const std::string& message) {
  if (std::memcmp(&got, &want, sizeof(double)) == 0) return;
  ++failures;
  std::printf("FAIL %s got=%.17g want=%.17g\n", message.c_str(), got, want);
}

void parity_at(Executor* executor, IslandProg* program, int condition_param,
               double condition, const char* label) {
  for (int64_t k = 0; k < executor->n_params(); ++k)
    executor->params_data()[k] =
        0.1 + 0.05 * static_cast<double>(k % 7) -
        0.15 * static_cast<double>(k % 3);
  executor->params_data()[condition_param] = condition;

  std::vector<double> native(static_cast<size_t>(executor->n_params()));
  std::vector<double> replay(static_cast<size_t>(executor->n_params()));
  const bool production_selection = program->native_adj;
  program->native_adj = true;
  const double native_lp = executor->gradient(native.data());
  program->native_adj = false;
  const double replay_lp = executor->gradient(replay.data());
  program->native_adj = production_selection;

  expect_exact(native_lp, replay_lp, std::string(label) + " LP");
  expect(native.size() == replay.size(), std::string(label) + " gradient size");
  for (size_t k = 0; k < native.size(); ++k)
    expect_exact(native[k], replay[k],
                 std::string(label) + " gradient " + std::to_string(k));
}

void test_prior_issue_timed_canary() {
  Canary canary = compile_canary("pr236_island");
  expect(canary.program != nullptr, "pr236 has a native traced CFG island");
  if (canary.program == nullptr) return;
  const IslandProg& program = *canary.program;
  expect(program.native_adj, "pr236 scalar CFG is selected in production");
  expect(program.adj.trace_bits > 0, "pr236 carries a path trace");
  expect(!program.adj.code.empty(), "pr236 carries generated reverse code");
  expect(std::any_of(program.code.begin(), program.code.end(),
                     [](const Program::Instr& instruction) {
                       return instruction.code == Program::JZ;
                     }),
         "pr236 forward retains a conditional edge");
  for (size_t pc = 0; pc < program.code.size(); ++pc) {
    const Program::Instr& instruction = program.code[pc];
    if (instruction.code != Program::JZ && instruction.code != Program::JMP)
      continue;
    expect(instruction.dst > static_cast<int>(pc) &&
               instruction.dst <= static_cast<int>(program.code.size()),
           "pr236 has only forward in-range CFG edges");
  }

  Executor executor(std::move(canary.model.graph));
  canary.model.bind(executor);
  // m[1,1] is the first unconstrained value and selects both arms.
  parity_at(&executor, canary.program, 0, 0.35, "pr236 true arm");
  parity_at(&executor, canary.program, 0, -0.35, "pr236 false arm");
}

void test_structured_diag_cfg_canary() {
  Canary canary = compile_canary("paramcond_diag_multiply");
  expect(canary.program != nullptr,
         "diag multiply retains a generated traced CFG adjoint");
  if (canary.program == nullptr) return;
  const IslandProg& program = *canary.program;
  expect(!program.native_adj,
         "tiny structured diag CFG is rejected by the profitability gate");
  const auto count_forward = [&](Program::Code code) {
    return std::count_if(program.code.begin(), program.code.end(),
                         [&](const Program::Instr& instruction) {
                           return instruction.code == code;
                         });
  };
  const auto count_reverse = [&](Program::Code code) {
    return std::count_if(program.adj.code.begin(), program.adj.code.end(),
                         [&](const AdjInstr& instruction) {
                           return instruction.code == code;
                         });
  };
  expect(count_forward(Program::DIAG_PRE_MULTIPLY) == 1,
         "diag CFG retains one DIAG_PRE_MULTIPLY");
  expect(count_forward(Program::DIAG_POST_MULTIPLY) == 1,
         "diag CFG retains one DIAG_POST_MULTIPLY");
  expect(count_reverse(Program::DIAG_PRE_MULTIPLY) == 1,
         "diag CFG generates one DIAG_PRE_MULTIPLY pullback");
  expect(count_reverse(Program::DIAG_POST_MULTIPLY) == 1,
         "diag CFG generates one DIAG_POST_MULTIPLY pullback");

  // Production deliberately rejects this tiny structured region, so size
  // the bound scratch for the forced native parity run before restoring the
  // production selection. The same oversized scratch is safe for replay.
  canary.program->native_adj = true;
  Executor executor(std::move(canary.model.graph));
  canary.program->native_adj = false;
  canary.model.bind(executor);
  // Parameter order is m[16], left[4], right[4], theta.
  parity_at(&executor, canary.program, 24, 0.35, "diag true arm");
  parity_at(&executor, canary.program, 24, -0.35, "diag false arm");
}

}  // namespace

int main() {
  test_prior_issue_timed_canary();
  test_structured_diag_cfg_canary();
  if (failures == 0) std::printf("OK cfg native canary\n");
  return failures == 0 ? 0 : 1;
}
