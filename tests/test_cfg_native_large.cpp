// Durable unrelated large-structured gate for forward-CFG native adjoints.
//
// This complements test_cfg_native_canary's two endpoints: pr236 proves a
// small scalar CFG wins and paramcond_diag_multiply proves a small structured
// CFG loses. cfg_native_large_structured is deliberately large enough to
// exercise the proposed conservative tier, while remaining independent of
// ctsem and passing through normal MIR lowering and OP_ISLAND execution.
#include <stanli/compile.hpp>
#include <stanli/graph.hpp>
#include <stanli/island.hpp>
#include <stanli/optable.hpp>

#include "env_helpers.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

using namespace stanli;

namespace {

constexpr size_t kProposedLargeReverseThreshold = 20000;
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

Canary compile_canary() {
  const std::string stem = "tests/fixtures/cfg_native_large_structured";
  const DataMap data = DataMap::from_json(slurp(stem + ".json"));
  Canary canary{compile_model(slurp(stem + ".tmir.sexp"), data), nullptr};
  size_t largest_trace = 0;
  for (const Op& op : canary.model.graph.ops) {
    if (op.opcode != OP_ISLAND || op.udata == nullptr) continue;
    auto* candidate =
        const_cast<IslandProg*>(static_cast<const IslandProg*>(op.udata));
    if (candidate->adj.trace_bits <= 0 ||
        static_cast<size_t>(candidate->adj.trace_bits) <= largest_trace)
      continue;
    largest_trace = static_cast<size_t>(candidate->adj.trace_bits);
    canary.program = candidate;
  }
  return canary;
}

size_t count_forward(const IslandProg& program, Program::Code code) {
  return static_cast<size_t>(
      std::count_if(program.code.begin(), program.code.end(),
                    [&](const Program::Instr& instruction) {
                      return instruction.code == code;
                    }));
}

size_t count_reverse(const IslandProg& program, Program::Code code) {
  return static_cast<size_t>(std::count_if(
      program.adj.code.begin(), program.adj.code.end(),
      [&](const AdjInstr& instruction) { return instruction.code == code; }));
}

size_t count_pairs(const IslandProg& program) {
  return static_cast<size_t>(
      std::count_if(program.adj.code.begin(), program.adj.code.end(),
                    [](const AdjInstr& instruction) {
                      return instruction.pair != AdjPair::None;
                    }));
}

size_t count_call_opcode(const IslandProg& program, uint16_t opcode) {
  return static_cast<size_t>(std::count_if(
      program.calls.begin(), program.calls.end(),
      [&](const Program::Call& call) { return call.opcode == opcode; }));
}

void expect_close(double native, double replay, const std::string& message,
                  double* max_abs, double* max_rel) {
  if (std::isnan(native) && std::isnan(replay)) return;
  const double absolute = std::abs(native - replay);
  const double scale = std::max({std::abs(native), std::abs(replay), 1e-300});
  const double relative = absolute / scale;
  *max_abs = std::max(*max_abs, absolute);
  *max_rel = std::max(*max_rel, relative);
  if (std::isfinite(native) && std::isfinite(replay) &&
      absolute <= 1e-11 + 1e-11 * scale)
    return;
  ++failures;
  std::printf("FAIL %s native=%.17g replay=%.17g abs=%.3g rel=%.3g\n",
              message.c_str(), native, replay, absolute, relative);
}

void parity_at(Executor* executor, IslandProg* program, double condition,
               const char* label) {
  for (int64_t k = 0; k < executor->n_params(); ++k)
    executor->params_data()[k] = 0.1 + 0.05 * static_cast<double>(k % 7) -
                                 0.15 * static_cast<double>(k % 3);
  // theta is deliberately the first declared parameter.
  executor->params_data()[0] = condition;

  std::vector<double> native(static_cast<size_t>(executor->n_params()));
  std::vector<double> replay(static_cast<size_t>(executor->n_params()));
  const bool production_selection = program->native_adj;
  program->native_adj = true;
  const double native_lp = executor->gradient(native.data());
  program->native_adj = false;
  const double replay_lp = executor->gradient(replay.data());
  program->native_adj = production_selection;

  double max_abs = 0.0, max_rel = 0.0;
  expect_close(native_lp, replay_lp, std::string(label) + " LP", &max_abs,
               &max_rel);
  for (size_t k = 0; k < native.size(); ++k)
    expect_close(native[k], replay[k],
                 std::string(label) + " gradient " + std::to_string(k),
                 &max_abs, &max_rel);
  std::printf("cfg large parity %s max_abs=%.3g max_rel=%.3g\n", label, max_abs,
              max_rel);
}

void test_large_structured_cfg() {
  // Exercise production defaults, never ambient research or escape flags.
  for (const char* flag : {
           "STANLI_CFG_STRUCTURED_NATIVE",
           "STANLI_NO_NATIVE_ADJ",
           "STANLI_CFG_PREPARED_MDIVIDE_LEFT",
           "STANLI_CFG_PREPARED_MDIVIDE_LEFT_MIN_N",
           "STANLI_CFG_PREPARED_MDIVIDE_LEFT_PRIM_LU",
           "STANLI_CFG_PREPARED_MDIVIDE_LEFT_PRIM_LU_MIN_N",
           "STANLI_NO_CFG_PREPARED_MDIVIDE_LEFT_PRIM_LU",
           "STANLI_CFG_MATRIX_EXP_BLOCK_FRECHET",
           "STANLI_CFG_MATRIX_EXP_BLOCK_FRECHET_MIN_N",
           "STANLI_NO_CFG_MATRIX_EXP_BLOCK_FRECHET",
           "STANLI_CFG_ADJ_TRACE_BLOCKS",
           "STANLI_NO_CFG_ADJ_TRACE_BLOCKS",
           "STANLI_CFG_ADJ_SUPERINSTRUCTIONS",
           "STANLI_NO_CFG_ADJ_SUPERINSTRUCTIONS",
       })
    test_unsetenv(flag);
  test_setenv("STANLI_CFG_ADJ_SUPERINSTRUCTIONS", "1", 1);
  Canary canary = compile_canary();
  expect(canary.program != nullptr,
         "large fixture has a generated traced CFG island");
  if (canary.program == nullptr) return;
  IslandProg& program = *canary.program;

  expect(program.var_replay != nullptr,
         "large CFG retains its canonical replay oracle");
  expect(program.native_adj,
         "measured large structured CFG is selected in production");
  expect(program.adj.code.size() >= kProposedLargeReverseThreshold,
         "large CFG reaches the proposed reverse-size tier");
  expect(program.adj.trace_bits >=
             static_cast<int>(kProposedLargeReverseThreshold),
         "large CFG carries a correspondingly large path trace");
  expect(!program.adj.trace_blocks.empty() &&
             program.adj.trace_blocks.back().end ==
                 static_cast<int32_t>(program.adj.code.size()),
         "large CFG builds an exact reverse trace-block partition");
  expect(program.adj.has_pairs && count_pairs(program) > 1000,
         "large CFG tags a material scalar pair population");
  expect(count_forward(program, Program::MDIVIDE_LEFT) == 0,
         "large CFG replaces both solve producers with prepared CALLs");
  expect(count_forward(program, Program::MATRIX_EXP) == 1,
         "large CFG retains one direct matrix-exp forward");
  expect(count_forward(program, Program::CALL) == 2,
         "large CFG runs both prepared solve producers");
  expect(count_call_opcode(program, OP_MDIVIDE_LEFT_PREPARED_PRIM_LU) == 2,
         "large CFG defaults both n55 solves to retained prim-LU");
  expect(count_call_opcode(program, OP_MATRIX_EXP_BLOCK_FRECHET) == 1,
         "large CFG defaults its n6 exponential to block-Frechet");
  expect(count_call_opcode(program, OP_MDIVIDE_LEFT) == 0 &&
             count_call_opcode(program, OP_MATRIX_EXP) == 0,
         "large CFG has no ordinary structured backward payloads");
  expect(std::count_if(program.calls.begin(), program.calls.end(),
                       [](const Program::Call& call) {
                         return call.opcode ==
                                    OP_MDIVIDE_LEFT_PREPARED_PRIM_LU &&
                                call.scratch_len == 55 * 55 + 55 + 1;
                       }) == 2,
         "large CFG retains exact n55 prim-LU scratch twice");
  expect(std::count_if(program.calls.begin(), program.calls.end(),
                       [](const Program::Call& call) {
                         return call.opcode == OP_MATRIX_EXP_BLOCK_FRECHET &&
                                call.scratch_len == 0;
                       }) == 1,
         "large CFG block-Frechet remains backward-only metadata");
  expect(count_reverse(program, Program::CALL) == 3,
         "large CFG reverses all three structured operations");
  int32_t block_begin = 0;
  for (const AdjTraceBlock& block : program.adj.trace_blocks) {
    for (int32_t pc = block_begin; pc < block.end; ++pc)
      if (program.adj.code[static_cast<size_t>(pc)].code == Program::CALL)
        expect(
            block.end == block_begin + 1 &&
                program.adj.code[static_cast<size_t>(pc)].pair == AdjPair::None,
            "large CFG keeps every reverse CALL singleton and unpaired");
    block_begin = block.end;
  }
  for (size_t pc = 0; pc < program.code.size(); ++pc) {
    const Program::Instr& instruction = program.code[pc];
    if (instruction.code != Program::JZ && instruction.code != Program::JMP)
      continue;
    expect(instruction.dst > static_cast<int>(pc) &&
               instruction.dst <= static_cast<int>(program.code.size()),
           "large CFG contains only forward in-range edges");
  }

  // Size the bound arena for native parity, then restore the production
  // decision while alternating both backwards below.
  const bool production_selection = program.native_adj;
  program.native_adj = true;
  Executor executor(std::move(canary.model.graph));
  program.native_adj = production_selection;
  canary.model.bind(executor);
  parity_at(&executor, &program, 0.35, "structured arm");
  parity_at(&executor, &program, -0.35, "fallback arm");
  std::printf("cfg large trace blocks=%zu reverse=%zu pairs=%zu\n",
              program.adj.trace_blocks.size(), program.adj.code.size(),
              count_pairs(program));
  test_unsetenv("STANLI_CFG_ADJ_SUPERINSTRUCTIONS");
}

}  // namespace

int main() {
  test_large_structured_cfg();
  if (failures == 0) std::printf("OK cfg native large structured\n");
  return failures == 0 ? 0 : 1;
}
