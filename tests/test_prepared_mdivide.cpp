// Force-only retained-QR pullback for active Program mdivide_left.
#include <stanli/adjoint.hpp>
#include <stanli/graph.hpp>
#include <stanli/island.hpp>
#include <stanli/optable.hpp>

#include "env_helpers.hpp"

#include <stan/math.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using stanli::Desc;
using stanli::Graph;
using stanli::IslandProg;
using stanli::KernelCtx;
using stanli::Op;
using stanli::Program;
using stanli::Slot;

namespace {

using MatD = Eigen::MatrixXd;
using VecD = Eigen::VectorXd;
using VarM = Eigen::Matrix<stan::math::var, -1, -1>;
using VarV = Eigen::Matrix<stan::math::var, -1, 1>;

int failures = 0;

void expect(const std::string& what, bool ok) {
  if (!ok) {
    ++failures;
    std::printf("FAIL %s\n", what.c_str());
  }
}

bool same_bits(double a, double b) {
  uint64_t aa = 0, bb = 0;
  std::memcpy(&aa, &a, sizeof aa);
  std::memcpy(&bb, &b, sizeof bb);
  return aa == bb;
}

struct Result {
  std::vector<double> value;
  std::vector<double> a_adj;
  std::vector<double> b_adj;
};

template <bool Vec, unsigned Detail>
Result stan_reference(const std::vector<double>& a_value,
                      const std::vector<double>& b_value,
                      const std::vector<double>& seed, int n, int k) {
  stan::math::nested_rev_autodiff nested;
  MatD a_double(n, n);
  for (int i = 0; i < n * n; ++i) a_double.data()[i] = a_value[i];
  std::conditional_t<Vec, VecD, MatD> b_double;
  if constexpr (Vec)
    b_double.resize(n);
  else
    b_double.resize(n, k);
  for (int i = 0; i < n * k; ++i) b_double.data()[i] = b_value[i];

  Result result;
  result.a_adj.assign(a_value.size(), 0.0);
  result.b_adj.assign(b_value.size(), 0.0);
  auto finish = [&](auto& out, auto* a, auto* b) {
    result.value.resize(out.size());
    MatD seed_matrix(out.rows(), out.cols());
    for (Eigen::Index i = 0; i < out.size(); ++i) {
      result.value[i] = out.data()[i].val();
      seed_matrix.data()[i] = seed[i];
    }
    stan::math::var objective =
        stan::math::sum(stan::math::elt_multiply(out, seed_matrix));
    stan::math::grad(objective.vi_);
    if (a)
      for (int i = 0; i < n * n; ++i) result.a_adj[i] = a->data()[i].adj();
    if (b)
      for (int i = 0; i < n * k; ++i) result.b_adj[i] = b->data()[i].adj();
  };

  if constexpr (Detail == 3u) {
    VarM a(n, n);
    std::conditional_t<Vec, VarV, VarM> b;
    if constexpr (Vec)
      b.resize(n);
    else
      b.resize(n, k);
    for (int i = 0; i < n * n; ++i) a.data()[i] = a_value[i];
    for (int i = 0; i < n * k; ++i) b.data()[i] = b_value[i];
    auto out = stan::math::mdivide_left(a, b);
    finish(out, &a, &b);
  } else if constexpr (Detail == 1u) {
    VarM a(n, n);
    for (int i = 0; i < n * n; ++i) a.data()[i] = a_value[i];
    auto out = stan::math::mdivide_left(a, b_double);
    finish(out, &a, static_cast<decltype(&a)>(nullptr));
  } else {
    std::conditional_t<Vec, VarV, VarM> b;
    if constexpr (Vec)
      b.resize(n);
    else
      b.resize(n, k);
    for (int i = 0; i < n * k; ++i) b.data()[i] = b_value[i];
    auto out = stan::math::mdivide_left(a_double, b);
    finish(out, static_cast<decltype(&b)>(nullptr), &b);
  }
  return result;
}

template <bool Vec, unsigned Detail>
Result prepared_kernel(const std::vector<double>& a_value,
                       const std::vector<double>& b_value,
                       const std::vector<double>& seed, int n, int k) {
  const stanli::Kernel* kernel =
      stanli::find_kernel(stanli::OP_MDIVIDE_LEFT_PREPARED);
  expect("prepared kernel registered", kernel != nullptr);
  Result result;
  result.value.assign(n * k, 0.0);
  result.a_adj.assign(n * n, 0.0);
  result.b_adj.assign(n * k, 0.0);
  std::vector<double> scratch(n * n + n, 0.0);
  int dims[2] = {n, k};
  KernelCtx ctx;
  ctx.n_in = 2;
  ctx.in[0] = Desc{const_cast<double*>(a_value.data()), n * n};
  ctx.in[1] = Desc{const_cast<double*>(b_value.data()), n * k};
  ctx.out = Desc{result.value.data(), n * k};
  ctx.variant = static_cast<uint8_t>(1u | (Vec ? 2u : 0u) | (Detail << 2u));
  ctx.scratch = scratch.data();
  ctx.idata = dims;
  ctx.n_idata = 2;
  ctx.in_adj[0] = Desc{Detail & 1u ? result.a_adj.data() : nullptr, n * n};
  ctx.in_adj[1] = Desc{Detail & 2u ? result.b_adj.data() : nullptr, n * k};
  ctx.out_adj_vec = Desc{const_cast<double*>(seed.data()), n * k};
  if (kernel) {
    kernel->forward(ctx);
    kernel->backward(ctx);
  }
  return result;
}

template <bool Vec, unsigned Detail>
void check_kernel_case(const char* name) {
  constexpr int n = 3;
  const int k = Vec ? 1 : 2;
  const std::vector<double> a{1.3, -0.2, 0.4, 0.1, 1.7, -0.3, 0.2, 0.5, 1.1};
  const std::vector<double> b =
      Vec ? std::vector<double>{0.7, -1.2, 0.3}
          : std::vector<double>{0.7, -1.2, 0.3, 0.2, 0.9, -0.4};
  const std::vector<double> seed =
      Vec ? std::vector<double>{0.4, -0.8, 1.1}
          : std::vector<double>{0.4, -0.8, 1.1, -0.2, 0.6, 0.3};
  const Result want = stan_reference<Vec, Detail>(a, b, seed, n, k);
  const Result got = prepared_kernel<Vec, Detail>(a, b, seed, n, k);
  for (size_t i = 0; i < want.value.size(); ++i)
    expect(std::string(name) + " value " + std::to_string(i),
           same_bits(want.value[i], got.value[i]));
  for (size_t i = 0; i < want.a_adj.size(); ++i)
    expect(std::string(name) + " A adj " + std::to_string(i),
           same_bits(want.a_adj[i], got.a_adj[i]));
  for (size_t i = 0; i < want.b_adj.size(); ++i)
    expect(std::string(name) + " B adj " + std::to_string(i),
           same_bits(want.b_adj[i], got.b_adj[i]));
}

IslandProg cfg_solve() {
  IslandProg p;
  p.n_regs = 9;
  p.ins = {{0, 1, -1, 0, false}, {1, 4, -1, 0, true}, {5, 2, -1, 0, true}};
  p.out_regs = {7, 8};
  p.code = {{Program::MOVR, 7, 5, 0, 0, 2},
            {Program::JZ, 3, 0},
            {Program::MDIVIDE_LEFT, 7, 1, 5, -2, 2}};
  return p;
}

IslandProg cfg_two_solves() {
  IslandProg p;
  p.n_regs = 24;
  p.ins = {{0, 1, -1, 0, false},
           {1, 4, -1, 0, true},
           {5, 2, -1, 0, true},
           {7, 9, -1, 0, true},
           {16, 3, -1, 0, true}};
  p.out_regs = {19, 20, 21, 22, 23};
  p.code = {{Program::JZ, 3, 0},
            {Program::MDIVIDE_LEFT, 19, 1, 5, -2, 2},
            {Program::MDIVIDE_LEFT, 21, 7, 16, -3, 3}};
  return p;
}

void expect_two_solve_structure(const char* name, const IslandProg& p,
                                uint16_t small_opcode, uint16_t large_opcode,
                                int small_scratch, int large_scratch,
                                Program::Code small_code,
                                Program::Code large_code, int n_regs) {
  expect(std::string(name) + " generated", !p.adj.code.empty());
  expect(std::string(name) + " exact calls", p.calls.size() == 2);
  if (p.calls.size() != 2) return;
  const auto& small = p.calls[0];
  const auto& large = p.calls[1];
  expect(std::string(name) + " small opcode", small.opcode == small_opcode);
  expect(std::string(name) + " large opcode", large.opcode == large_opcode);
  expect(std::string(name) + " small scratch",
         small.scratch_len == small_scratch);
  expect(std::string(name) + " large scratch",
         large.scratch_len == large_scratch);
  expect(std::string(name) + " exact small call",
         small.variant == 15 && small.n_in == 2 && small.in[0] == 1 &&
             small.in_len[0] == 4 && small.in[1] == 5 && small.in_len[1] == 2 &&
             small.out == 19 && small.out_len == 2 && small.idata.size() == 2 &&
             small.idata[0] == 2 && small.idata[1] == 1);
  expect(std::string(name) + " exact large call",
         large.variant == 15 && large.n_in == 2 && large.in[0] == 7 &&
             large.in_len[0] == 9 && large.in[1] == 16 &&
             large.in_len[1] == 3 && large.out == 21 && large.out_len == 3 &&
             large.idata.size() == 2 && large.idata[0] == 3 &&
             large.idata[1] == 1);
  expect(std::string(name) + " instruction spellings",
         p.code.size() == 3 && p.code[1].code == small_code &&
             p.code[2].code == large_code);
  const int forward_calls = static_cast<int>(std::count_if(
      p.code.begin(), p.code.end(), [](const Program::Instr& instruction) {
        return instruction.code == Program::CALL;
      }));
  const int backward_calls =
      static_cast<int>(std::count_if(p.adj.code.begin(), p.adj.code.end(),
                                     [](const stanli::AdjInstr& instruction) {
                                       return instruction.code == Program::CALL;
                                     }));
  expect(std::string(name) + " forward call count",
         forward_calls == (small_scratch != 0) + (large_scratch != 0));
  expect(std::string(name) + " backward call count", backward_calls == 2);
  expect(std::string(name) + " register count", p.n_regs == n_regs);
  if (small_scratch != 0)
    expect(std::string(name) + " small private scratch",
           small.scratch == 24 &&
               small.scratch + small.scratch_len <=
                   static_cast<int>(p.adj.adj_reg.size()) &&
               std::all_of(
                   p.adj.adj_reg.begin() + small.scratch,
                   p.adj.adj_reg.begin() + small.scratch + small.scratch_len,
                   [](int32_t reg) { return reg == 0; }));
  if (large_scratch != 0)
    expect(std::string(name) + " large private scratch",
           large.scratch == 24 + small_scratch &&
               large.scratch + large.scratch_len <=
                   static_cast<int>(p.adj.adj_reg.size()) &&
               std::all_of(
                   p.adj.adj_reg.begin() + large.scratch,
                   p.adj.adj_reg.begin() + large.scratch + large.scratch_len,
                   [](int32_t reg) { return reg == 0; }));
}

void check_force_seam() {
  test_unsetenv("STANLI_CFG_PREPARED_MDIVIDE_LEFT");
  test_unsetenv("STANLI_CFG_PREPARED_MDIVIDE_LEFT_MIN_N");
  IslandProg ordinary = cfg_solve();
  expect("ordinary cfg generated", stanli::gen_cfg_adjoint(ordinary));
  expect("prepared solve production-off",
         ordinary.calls.size() == 1 &&
             ordinary.calls[0].opcode == stanli::OP_MDIVIDE_LEFT &&
             ordinary.calls[0].scratch_len == 0 &&
             ordinary.code[2].code == Program::MDIVIDE_LEFT);

  test_setenv("STANLI_CFG_PREPARED_MDIVIDE_LEFT", "1");
  IslandProg prepared = cfg_solve();
  expect("prepared cfg generated", stanli::gen_cfg_adjoint(prepared));
  expect("prepared exact producer CALL",
         prepared.calls.size() == 1 &&
             prepared.calls[0].opcode == stanli::OP_MDIVIDE_LEFT_PREPARED &&
             prepared.calls[0].scratch_len == 6 &&
             prepared.code[2].code == Program::CALL);
  if (!prepared.calls.empty()) {
    const auto& call = prepared.calls[0];
    bool value_only =
        call.scratch >= 0 && call.scratch + call.scratch_len <= prepared.n_regs;
    for (int i = 0; i < call.scratch_len && value_only; ++i)
      value_only =
          prepared.adj.adj_reg[call.scratch + i] == prepared.adj.adj_reg[0];
    expect("prepared scratch private value-only", value_only);
  }

  IslandProg refused = cfg_solve();
  refused.code.push_back({Program::JMP, 0});
  const int before_regs = refused.n_regs;
  expect("prepared refusal", !stanli::gen_cfg_adjoint(refused));
  expect("prepared refusal transactional",
         refused.n_regs == before_regs && refused.calls.empty() &&
             refused.code.back().code == Program::JMP);

  IslandProg all = cfg_two_solves();
  expect("all-solves cfg generated", stanli::gen_cfg_adjoint(all));
  expect_two_solve_structure("all-solves", all,
                             stanli::OP_MDIVIDE_LEFT_PREPARED,
                             stanli::OP_MDIVIDE_LEFT_PREPARED, 6, 12,
                             Program::CALL, Program::CALL, 42);

  test_setenv("STANLI_CFG_PREPARED_MDIVIDE_LEFT_MIN_N", "3");
  IslandProg threshold = cfg_two_solves();
  expect("threshold cfg generated", stanli::gen_cfg_adjoint(threshold));
  expect_two_solve_structure("threshold", threshold, stanli::OP_MDIVIDE_LEFT,
                             stanli::OP_MDIVIDE_LEFT_PREPARED, 0, 12,
                             Program::MDIVIDE_LEFT, Program::CALL, 36);

  test_setenv("STANLI_CFG_PREPARED_MDIVIDE_LEFT_MIN_N", "2");
  IslandProg inclusive = cfg_two_solves();
  expect("inclusive threshold cfg generated",
         stanli::gen_cfg_adjoint(inclusive));
  expect_two_solve_structure("inclusive threshold", inclusive,
                             stanli::OP_MDIVIDE_LEFT_PREPARED,
                             stanli::OP_MDIVIDE_LEFT_PREPARED, 6, 12,
                             Program::CALL, Program::CALL, 42);

  test_setenv("STANLI_CFG_PREPARED_MDIVIDE_LEFT_MIN_N", "invalid");
  IslandProg invalid = cfg_two_solves();
  expect("invalid threshold cfg generated", stanli::gen_cfg_adjoint(invalid));
  expect_two_solve_structure("invalid threshold", invalid,
                             stanli::OP_MDIVIDE_LEFT, stanli::OP_MDIVIDE_LEFT,
                             0, 0, Program::MDIVIDE_LEFT, Program::MDIVIDE_LEFT,
                             24);

  test_unsetenv("STANLI_CFG_PREPARED_MDIVIDE_LEFT");
  test_setenv("STANLI_CFG_PREPARED_MDIVIDE_LEFT_MIN_N", "2");
  IslandProg threshold_only = cfg_two_solves();
  expect("threshold-only cfg generated",
         stanli::gen_cfg_adjoint(threshold_only));
  expect_two_solve_structure("threshold-only production-off", threshold_only,
                             stanli::OP_MDIVIDE_LEFT, stanli::OP_MDIVIDE_LEFT,
                             0, 0, Program::MDIVIDE_LEFT, Program::MDIVIDE_LEFT,
                             24);
  test_unsetenv("STANLI_CFG_PREPARED_MDIVIDE_LEFT_MIN_N");
}

}  // namespace

int main() {
  Graph graph;
  graph.result_slot = graph.add_slot(1, false);
  stanli::Executor register_kernels(std::move(graph));

  check_kernel_case<true, 3u>("vector vv");
  check_kernel_case<true, 1u>("vector vd");
  check_kernel_case<true, 2u>("vector dv");
  check_kernel_case<false, 3u>("matrix vv");
  check_kernel_case<false, 1u>("matrix vd");
  check_kernel_case<false, 2u>("matrix dv");
  check_force_seam();

  if (failures) return 1;
  std::puts("prepared mdivide_left tests passed");
  return 0;
}
