// Dimension-gated retained-PartialPivLU path for Program mdivide_left.
// The public contract under test is deliberately stricter than gradient
// closeness: its forward must be the exact ordinary Stan prim solve.
#include <stanli/adjoint.hpp>
#include <stanli/graph.hpp>
#include <stanli/island.hpp>
#include <stanli/optable.hpp>

#include "env_helpers.hpp"

#include <stan/math.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

using stanli::Desc;
using stanli::Graph;
using stanli::IslandProg;
using stanli::KernelCtx;
using stanli::Op;
using stanli::Program;

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

bool same_fp(double a, double b) {
  return same_bits(a, b) || (std::isnan(a) && std::isnan(b));
}

bool close(double a, double b, double relative = 2e-10,
           double absolute = 2e-12) {
  if (std::isnan(a) || std::isnan(b)) return std::isnan(a) && std::isnan(b);
  if (a == b) return true;
  return std::abs(a - b) <=
         absolute + relative * std::max(std::abs(a), std::abs(b));
}

struct Result {
  std::vector<double> value;
  std::vector<double> a_adj;
  std::vector<double> b_adj;
  std::vector<double> scratch;
};

template <bool Vec>
std::vector<double> prim_value(const std::vector<double>& a_value,
                               const std::vector<double>& b_value, int n,
                               int k) {
  MatD a(n, n);
  for (int i = 0; i < n * n; ++i) a.data()[i] = a_value[i];
  std::conditional_t<Vec, VecD, MatD> b;
  if constexpr (Vec)
    b.resize(n);
  else
    b.resize(n, k);
  for (int i = 0; i < n * k; ++i) b.data()[i] = b_value[i];
  const auto out = stan::math::mdivide_left(a, b);
  return std::vector<double>(out.data(), out.data() + out.size());
}

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
    MatD seed_matrix(out.rows(), out.cols());
    for (Eigen::Index i = 0; i < out.size(); ++i)
      seed_matrix.data()[i] = seed[static_cast<size_t>(i)];
    stan::math::var objective =
        stan::math::sum(stan::math::elt_multiply(out, seed_matrix));
    stan::math::grad(objective.vi_);
    if (a)
      for (int i = 0; i < n * n; ++i)
        result.a_adj[static_cast<size_t>(i)] = a->data()[i].adj();
    if (b)
      for (int i = 0; i < n * k; ++i)
        result.b_adj[static_cast<size_t>(i)] = b->data()[i].adj();
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
      stanli::find_kernel(stanli::OP_MDIVIDE_LEFT_PREPARED_PRIM_LU);
  expect("prepared prim-LU kernel registered", kernel != nullptr);
  Result result;
  result.value.assign(static_cast<size_t>(n * k), 0.0);
  result.a_adj.assign(static_cast<size_t>(n * n), 0.0);
  result.b_adj.assign(static_cast<size_t>(n * k), 0.0);
  result.scratch.assign(static_cast<size_t>(n * n + n + 1), 0.0);
  int dims[2] = {n, k};
  KernelCtx ctx;
  ctx.n_in = 2;
  ctx.in[0] = Desc{const_cast<double*>(a_value.data()), n * n};
  ctx.in[1] = Desc{const_cast<double*>(b_value.data()), n * k};
  ctx.out = Desc{result.value.data(), n * k};
  ctx.variant = static_cast<uint8_t>(1u | (Vec ? 2u : 0u) | (Detail << 2u));
  ctx.scratch = result.scratch.data();
  ctx.idata = dims;
  ctx.n_idata = 2;
  ctx.in_adj[0] = Desc{Detail & 1u ? result.a_adj.data() : nullptr, n * n};
  ctx.in_adj[1] = Desc{Detail & 2u ? result.b_adj.data() : nullptr, n * k};
  ctx.out_adj_vec = Desc{const_cast<double*>(seed.data()), n * k};
  if (kernel) {
    kernel->forward(ctx);
    // A poor-system fallback replays the active QR kernel during backward,
    // which writes its private checkpoint output. Preserve the observable
    // Program-forward value before invoking it.
    const std::vector<double> forward = result.value;
    kernel->backward(ctx);
    result.value = forward;
  }
  return result;
}

template <bool Vec, unsigned Detail>
void check_case(const std::string& name, const std::vector<double>& a,
                const std::vector<double>& b, const std::vector<double>& seed,
                int n, int k, bool accepted, bool exact_gradient) {
  const std::vector<double> want_value = prim_value<Vec>(a, b, n, k);
  const Result want = stan_reference<Vec, Detail>(a, b, seed, n, k);
  const Result got = prepared_kernel<Vec, Detail>(a, b, seed, n, k);
  expect(name + " acceptance marker",
         got.scratch.size() == static_cast<size_t>(n * n + n + 1) &&
             (got.scratch.back() >= 0.0) == accepted);
  for (size_t i = 0; i < want_value.size(); ++i)
    expect(name + " exact prim value " + std::to_string(i),
           same_fp(want_value[i], got.value[i]));
  for (size_t i = 0; i < want.a_adj.size(); ++i)
    expect(name + " A adj " + std::to_string(i),
           exact_gradient ? same_fp(want.a_adj[i], got.a_adj[i])
                          : close(want.a_adj[i], got.a_adj[i]));
  for (size_t i = 0; i < want.b_adj.size(); ++i)
    expect(name + " B adj " + std::to_string(i),
           exact_gradient ? same_fp(want.b_adj[i], got.b_adj[i])
                          : close(want.b_adj[i], got.b_adj[i]));
}

template <bool Vec, unsigned Detail>
void check_well_conditioned(const char* name) {
  constexpr int n = 3;
  const int k = Vec ? 1 : 2;
  const std::vector<double> a{1.3, -0.2, 0.4, 0.1, 1.7, -0.3, 0.2, 0.5, 1.1};
  const std::vector<double> b =
      Vec ? std::vector<double>{0.7, -1.2, 0.3}
          : std::vector<double>{0.7, -1.2, 0.3, 0.2, 0.9, -0.4};
  const std::vector<double> seed =
      Vec ? std::vector<double>{0.4, -0.8, 1.1}
          : std::vector<double>{0.4, -0.8, 1.1, -0.2, 0.6, 0.3};
  check_case<Vec, Detail>(name, a, b, seed, n, k, true, false);
}

void check_numerical_cases() {
  check_well_conditioned<true, 3u>("vector vv");
  check_well_conditioned<true, 1u>("vector vd");
  check_well_conditioned<true, 2u>("vector dv");
  check_well_conditioned<false, 3u>("matrix vv");
  check_well_conditioned<false, 1u>("matrix vd");
  check_well_conditioned<false, 2u>("matrix dv");

  // A one-column matrix and a vector have identical flat lengths but select
  // different Stan language overloads and different Program variant bits.
  const std::vector<double> ordinary{1.3,  -0.2, 0.4, 0.1, 1.7,
                                     -0.3, 0.2,  0.5, 1.1};
  check_case<false, 3u>("one-column matrix vv", ordinary, {0.7, -1.2, 0.3},
                        {0.4, -0.8, 1.1}, 3, 1, true, false);

  // The first column forces PartialPivLU to swap rows. This catches storing
  // transpositions instead of the final permutation, and P versus P^-1.
  const std::vector<double> pivoted{0.01, 3.0, 0.4, 2.0, 0.1,
                                    0.2,  0.0, 0.2, 4.0};
  check_case<true, 3u>("pivoted vector", pivoted, {0.7, -1.2, 0.3},
                       {0.4, -0.8, 1.1}, 3, 1, true, false);
  check_case<false, 3u>("pivoted matrix", pivoted,
                        {0.7, -1.2, 0.3, 0.2, 0.9, -0.4},
                        {0.4, -0.8, 1.1, -0.2, 0.6, 0.3}, 3, 2, true, false);

  std::vector<double> hilbert(64);
  for (int j = 0; j < 8; ++j)
    for (int i = 0; i < 8; ++i)
      hilbert[static_cast<size_t>(i + 8 * j)] = 1.0 / (i + j + 1.0);
  const std::vector<double> h_b{0.2, -0.1, 0.3, -0.5, 0.7, 0.4, -0.2, 0.6};
  const std::vector<double> h_seed{-0.3, 0.8, 0.1, -0.4, 0.5, 0.2, -0.7, 0.9};
  check_case<true, 3u>("Hilbert fallback", hilbert, h_b, h_seed, 8, 1, false,
                       true);

  const std::vector<double> near_rank{1.0, 0.0, 0.0, 0.0,  1.0,
                                      0.0, 0.0, 0.0, 1e-12};
  check_case<false, 3u>("near-rank fallback", near_rank,
                        {0.4, -0.2, 0.8, 0.7, 0.1, -0.3},
                        {0.2, 0.6, -0.5, -0.1, 0.9, 0.3}, 3, 2, false, true);

  const std::vector<double> rank_deficient{1.0, 0.0, 0.0, 0.0, 1.0,
                                           0.0, 0.0, 0.0, 0.0};
  check_case<true, 3u>("rank-deficient fallback", rank_deficient,
                       {0.4, -0.2, 0.8}, {0.2, 0.6, -0.5}, 3, 1, false, true);
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

IslandProg cfg_solve(int n) {
  const int matrix = 1;
  const int rhs = matrix + n * n;
  const int output = rhs + n;
  IslandProg p;
  p.n_regs = output + n;
  p.ins = {{0, 1, -1, 0, false},
           {matrix, n * n, -1, 0, true},
           {rhs, n, -1, 0, true}};
  p.out_regs = {output};
  p.code = {{Program::MOVR, output, rhs, 0, 0, n},
            {Program::JZ, 3, 0},
            {Program::MDIVIDE_LEFT, output, matrix, rhs, -n, n}};
  return p;
}

void expect_single_structure(const std::string& name, const IslandProg& p,
                             uint16_t opcode, int scratch) {
  expect(name + " generated", !p.adj.code.empty());
  expect(name + " one call", p.calls.size() == 1);
  if (p.calls.size() != 1) return;
  const Program::Call& call = p.calls[0];
  expect(name + " opcode", call.opcode == opcode);
  expect(name + " scratch", call.scratch_len == scratch);
  expect(
      name + " forward spelling",
      p.code.size() == 3 &&
          p.code[2].code == (scratch ? Program::CALL : Program::MDIVIDE_LEFT));
  expect(name + " backward CALL",
         std::count_if(p.adj.code.begin(), p.adj.code.end(),
                       [](const stanli::AdjInstr& instruction) {
                         return instruction.code == Program::CALL;
                       }) == 1);
}

void expect_structure(const std::string& name, const IslandProg& p,
                      uint16_t small_opcode, uint16_t large_opcode,
                      int small_scratch, int large_scratch) {
  expect(name + " generated", !p.adj.code.empty());
  expect(name + " call count", p.calls.size() == 2);
  if (p.calls.size() != 2) return;
  const Program::Call& small = p.calls[0];
  const Program::Call& large = p.calls[1];
  expect(name + " opcodes",
         small.opcode == small_opcode && large.opcode == large_opcode);
  expect(name + " scratch lengths", small.scratch_len == small_scratch &&
                                        large.scratch_len == large_scratch);
  expect(name + " call shapes",
         small.variant == 15 && small.in[0] == 1 && small.in_len[0] == 4 &&
             small.in[1] == 5 && small.in_len[1] == 2 && small.out == 19 &&
             small.out_len == 2 && small.idata == std::vector<int>({2, 1}) &&
             large.variant == 15 && large.in[0] == 7 && large.in_len[0] == 9 &&
             large.in[1] == 16 && large.in_len[1] == 3 && large.out == 21 &&
             large.out_len == 3 && large.idata == std::vector<int>({3, 1}));
  expect(name + " forward spellings",
         p.code.size() == 3 &&
             p.code[1].code ==
                 (small_scratch ? Program::CALL : Program::MDIVIDE_LEFT) &&
             p.code[2].code ==
                 (large_scratch ? Program::CALL : Program::MDIVIDE_LEFT));
  const int forward_calls = static_cast<int>(std::count_if(
      p.code.begin(), p.code.end(), [](const Program::Instr& instruction) {
        return instruction.code == Program::CALL;
      }));
  const int backward_calls =
      static_cast<int>(std::count_if(p.adj.code.begin(), p.adj.code.end(),
                                     [](const stanli::AdjInstr& instruction) {
                                       return instruction.code == Program::CALL;
                                     }));
  expect(name + " exact CALL counts",
         forward_calls == (small_scratch != 0) + (large_scratch != 0) &&
             backward_calls == 2);
  const int total_scratch = small_scratch + large_scratch;
  expect(name + " register count", p.n_regs == 24 + total_scratch);
  if (small_scratch) {
    expect(name + " small scratch offset", small.scratch == 24);
    expect(name + " small private value-only",
           small.scratch + small.scratch_len <=
                   static_cast<int>(p.adj.adj_reg.size()) &&
               std::all_of(
                   p.adj.adj_reg.begin() + small.scratch,
                   p.adj.adj_reg.begin() + small.scratch + small.scratch_len,
                   [](int32_t reg) { return reg == 0; }));
  }
  if (large_scratch) {
    expect(name + " large scratch offset", large.scratch == 24 + small_scratch);
    expect(name + " large private value-only",
           large.scratch + large.scratch_len <=
                   static_cast<int>(p.adj.adj_reg.size()) &&
               std::all_of(
                   p.adj.adj_reg.begin() + large.scratch,
                   p.adj.adj_reg.begin() + large.scratch + large.scratch_len,
                   [](int32_t reg) { return reg == 0; }));
  }
}

void clear_force_seams() {
  test_unsetenv("STANLI_CFG_PREPARED_MDIVIDE_LEFT");
  test_unsetenv("STANLI_CFG_PREPARED_MDIVIDE_LEFT_MIN_N");
  test_unsetenv("STANLI_CFG_PREPARED_MDIVIDE_LEFT_PRIM_LU");
  test_unsetenv("STANLI_CFG_PREPARED_MDIVIDE_LEFT_PRIM_LU_MIN_N");
  test_unsetenv("STANLI_NO_CFG_PREPARED_MDIVIDE_LEFT_PRIM_LU");
}

void check_force_seam() {
  clear_force_seams();
  IslandProg ordinary = cfg_two_solves();
  expect("ordinary cfg generated", stanli::gen_cfg_adjoint(ordinary));
  expect_structure("production rejects small solves", ordinary,
                   stanli::OP_MDIVIDE_LEFT, stanli::OP_MDIVIDE_LEFT, 0, 0);

  IslandProg production = cfg_solve(32);
  expect("production n32 cfg generated", stanli::gen_cfg_adjoint(production));
  expect_single_structure("production n32", production,
                          stanli::OP_MDIVIDE_LEFT_PREPARED_PRIM_LU,
                          32 * 32 + 32 + 1);

  test_setenv("STANLI_CFG_PREPARED_MDIVIDE_LEFT_PRIM_LU", "1");
  IslandProg all = cfg_two_solves();
  expect("all prim-LU cfg generated", stanli::gen_cfg_adjoint(all));
  expect_structure("all prim-LU", all, stanli::OP_MDIVIDE_LEFT_PREPARED_PRIM_LU,
                   stanli::OP_MDIVIDE_LEFT_PREPARED_PRIM_LU, 7, 13);

  test_setenv("STANLI_CFG_PREPARED_MDIVIDE_LEFT_PRIM_LU_MIN_N", "3");
  IslandProg threshold = cfg_two_solves();
  expect("threshold prim-LU cfg generated", stanli::gen_cfg_adjoint(threshold));
  expect_structure("threshold prim-LU", threshold, stanli::OP_MDIVIDE_LEFT,
                   stanli::OP_MDIVIDE_LEFT_PREPARED_PRIM_LU, 0, 13);

  test_setenv("STANLI_CFG_PREPARED_MDIVIDE_LEFT_PRIM_LU_MIN_N", "invalid");
  IslandProg invalid = cfg_two_solves();
  expect("invalid prim-LU threshold generated",
         stanli::gen_cfg_adjoint(invalid));
  expect_structure("invalid threshold fails closed", invalid,
                   stanli::OP_MDIVIDE_LEFT, stanli::OP_MDIVIDE_LEFT, 0, 0);

  IslandProg invalid_production = cfg_solve(32);
  expect("invalid production threshold generated",
         stanli::gen_cfg_adjoint(invalid_production));
  expect_single_structure("invalid force does not fall back to production",
                          invalid_production, stanli::OP_MDIVIDE_LEFT, 0);

  clear_force_seams();
  test_setenv("STANLI_CFG_PREPARED_MDIVIDE_LEFT_PRIM_LU_MIN_N", "2");
  IslandProg threshold_only = cfg_two_solves();
  expect("threshold-only cfg generated",
         stanli::gen_cfg_adjoint(threshold_only));
  expect_structure("threshold alone keeps small solves ordinary",
                   threshold_only, stanli::OP_MDIVIDE_LEFT,
                   stanli::OP_MDIVIDE_LEFT, 0, 0);

  clear_force_seams();
  test_setenv("STANLI_CFG_PREPARED_MDIVIDE_LEFT", "1");
  test_setenv("STANLI_CFG_PREPARED_MDIVIDE_LEFT_PRIM_LU", "1");
  IslandProg precedence = cfg_two_solves();
  expect("prim-LU precedence cfg generated",
         stanli::gen_cfg_adjoint(precedence));
  expect_structure("prim-LU precedence", precedence,
                   stanli::OP_MDIVIDE_LEFT_PREPARED_PRIM_LU,
                   stanli::OP_MDIVIDE_LEFT_PREPARED_PRIM_LU, 7, 13);

  clear_force_seams();
  test_setenv("STANLI_NO_CFG_PREPARED_MDIVIDE_LEFT_PRIM_LU", "1");
  IslandProg escaped = cfg_solve(32);
  expect("escaped production cfg generated", stanli::gen_cfg_adjoint(escaped));
  expect_single_structure("escape disables production", escaped,
                          stanli::OP_MDIVIDE_LEFT, 0);

  test_setenv("STANLI_CFG_PREPARED_MDIVIDE_LEFT_PRIM_LU", "1");
  IslandProg escaped_force = cfg_solve(32);
  expect("escaped force cfg generated", stanli::gen_cfg_adjoint(escaped_force));
  expect_single_structure("escape has precedence over force", escaped_force,
                          stanli::OP_MDIVIDE_LEFT, 0);

  test_setenv("STANLI_NO_CFG_PREPARED_MDIVIDE_LEFT_PRIM_LU", "0");
  test_unsetenv("STANLI_CFG_PREPARED_MDIVIDE_LEFT_PRIM_LU");
  IslandProg zero_escape = cfg_solve(32);
  expect("zero escape cfg generated", stanli::gen_cfg_adjoint(zero_escape));
  expect_single_structure("zero escape preserves production", zero_escape,
                          stanli::OP_MDIVIDE_LEFT_PREPARED_PRIM_LU,
                          32 * 32 + 32 + 1);

  IslandProg refused = cfg_two_solves();
  refused.code.push_back({Program::JMP, 0});
  const IslandProg before = refused;
  expect("prim-LU refusal", !stanli::gen_cfg_adjoint(refused));
  expect("prim-LU refusal transactional",
         refused.n_regs == before.n_regs && refused.calls.empty() &&
             refused.code.size() == before.code.size() &&
             refused.code.back().code == Program::JMP &&
             refused.adj.code.empty());

  clear_force_seams();
}

void check_scratch_contract() {
  const stanli::Kernel* kernel =
      stanli::find_kernel(stanli::OP_MDIVIDE_LEFT_PREPARED_PRIM_LU);
  expect("scratch kernel registered", kernel && kernel->scratch_size);
  if (!kernel || !kernel->scratch_size) return;
  int dims[2] = {3, 2};
  Op active;
  active.opcode = stanli::OP_MDIVIDE_LEFT_PREPARED_PRIM_LU;
  active.variant = 1;
  active.idata = dims;
  active.n_idata = 2;
  expect("scratch exact n^2+n+1", kernel->scratch_size(active, nullptr) == 13);
  active.variant = 0;
  expect("inactive call owns no scratch",
         kernel->scratch_size(active, nullptr) == 0);
}

}  // namespace

int main() {
  Graph graph;
  graph.result_slot = graph.add_slot(1, false);
  stanli::Executor register_kernels(std::move(graph));

  check_numerical_cases();
  check_scratch_contract();
  check_force_seam();

  if (failures) return 1;
  std::puts("prepared prim-LU mdivide_left tests passed");
  return 0;
}
