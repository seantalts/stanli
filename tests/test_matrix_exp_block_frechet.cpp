// Dimension-gated block-Frechet pullback for structured Program matrix_exp.
//
// The ordinary kernel is the exact replay oracle and stays the default. This
// test covers the experimental formula numerically and the environment seam
// structurally, including fail-closed thresholds and transactional refusal.
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
#include <random>
#include <string>
#include <vector>

using stanli::Desc;
using stanli::IslandProg;
using stanli::KernelCtx;
using stanli::Program;

namespace {

using Mat = Eigen::MatrixXd;
using VarM = Eigen::Matrix<stan::math::var, -1, -1>;

int failures = 0;

void expect(const std::string& what, bool ok) {
  if (ok) return;
  ++failures;
  std::printf("FAIL %s\n", what.c_str());
}

bool same_bits(double a, double b) {
  uint64_t aa = 0, bb = 0;
  std::memcpy(&aa, &a, sizeof aa);
  std::memcpy(&bb, &b, sizeof bb);
  return aa == bb;
}

struct Result {
  Mat value;
  Mat gradient;
};

Result stan_active_reference(const Mat& a, const Mat& seed) {
  stan::math::nested_rev_autodiff nested;
  const int n = static_cast<int>(a.rows());
  VarM active(n, n);
  for (int i = 0; i < n * n; ++i) active.data()[i] = a.data()[i];
  auto output = stan::math::matrix_exp(active);
  stan::math::var objective = stan::math::sum(
      stan::math::elt_multiply(output, stan::math::to_matrix(seed)));
  stan::math::grad(objective.vi_);
  Result result{Mat(n, n), Mat(n, n)};
  for (int i = 0; i < n * n; ++i) {
    result.value.data()[i] = output.data()[i].val();
    result.gradient.data()[i] = active.data()[i].adj();
  }
  return result;
}

Result block_kernel(const Mat& a, const Mat& seed, double initial_adj) {
  const int n = static_cast<int>(a.rows());
  const stanli::Kernel* kernel =
      stanli::find_kernel(stanli::OP_MATRIX_EXP_BLOCK_FRECHET);
  expect("block-Frechet kernel registered", kernel != nullptr);
  Result result{Mat::Zero(n, n), Mat::Constant(n, n, initial_adj)};
  int dims[1] = {n};
  KernelCtx ctx;
  ctx.n_in = 1;
  ctx.in[0] = Desc{const_cast<double*>(a.data()), n * n};
  ctx.in_adj[0] = Desc{result.gradient.data(), n * n};
  ctx.out = Desc{result.value.data(), n * n};
  ctx.out_adj_vec = Desc{const_cast<double*>(seed.data()), n * n};
  ctx.idata = dims;
  ctx.n_idata = 1;
  if (kernel) {
    kernel->forward(ctx);
    kernel->backward(ctx);
  }
  return result;
}

Mat explicit_block_formula(const Mat& a, const Mat& seed,
                           bool wrong_transposed_seed) {
  const int n = static_cast<int>(a.rows());
  Mat block = Mat::Zero(2 * n, 2 * n);
  block.topLeftCorner(n, n) = a.transpose();
  block.bottomRightCorner(n, n) = a.transpose();
  if (wrong_transposed_seed)
    block.topRightCorner(n, n) = seed.transpose();
  else
    block.topRightCorner(n, n) = seed;
  const Mat exponential = stan::math::matrix_exp(block);
  return wrong_transposed_seed
             ? Mat(exponential.topRightCorner(n, n).transpose())
             : Mat(exponential.topRightCorner(n, n));
}

double max_abs(const Mat& a, const Mat& b) {
  return (a - b).cwiseAbs().maxCoeff();
}

double objective(const Mat& a, const Mat& seed) {
  return (stan::math::matrix_exp(a).array() * seed.array()).sum();
}

Mat dense_seed(int n, int variant) {
  Mat seed(n, n);
  for (int c = 0; c < n; ++c)
    for (int r = 0; r < n; ++r)
      seed(r, c) = variant == 0 ? 0.4 * std::cos(0.19 * (1 + 2 * r + 5 * c))
                                : 0.3 * std::sin(0.41 * (2 + 7 * r + 3 * c)) +
                                      (r == c ? 0.2 : 0.0);
  return seed;
}

void check_numeric_case(const char* name, const Mat& a) {
  const int n = static_cast<int>(a.rows());
  for (int seed_variant = 0; seed_variant < 2; ++seed_variant) {
    const Mat seed = dense_seed(n, seed_variant);
    const Result reference = stan_active_reference(a, seed);
    constexpr double initial_adj = 0.125;
    const Result got = block_kernel(a, seed, initial_adj);
    const Mat block = explicit_block_formula(a, seed, false);

    const Mat double_value = stan::math::matrix_exp(a);
    for (int i = 0; i < n * n; ++i)
      expect(std::string(name) + " direct forward " + std::to_string(i),
             same_bits(got.value.data()[i], double_value.data()[i]));

    double largest_abs = 0.0, largest_rel = 0.0;
    for (int i = 0; i < n * n; ++i) {
      const double actual = got.gradient.data()[i] - initial_adj;
      const double wanted = reference.gradient.data()[i];
      const double absolute = std::abs(actual - wanted);
      const double scale =
          std::max({std::abs(actual), std::abs(wanted), 1e-300});
      largest_abs = std::max(largest_abs, absolute);
      largest_rel = std::max(largest_rel, absolute / scale);
      expect(std::string(name) + " Stan active gradient " +
                 std::to_string(seed_variant) + ":" + std::to_string(i),
             absolute <= 2e-12 + 2e-12 * scale);
      expect(std::string(name) + " explicit block orientation " +
                 std::to_string(seed_variant) + ":" + std::to_string(i),
             same_bits(got.gradient.data()[i], initial_adj + block.data()[i]));
    }

    Mat direction(n, n);
    for (int c = 0; c < n; ++c)
      for (int r = 0; r < n; ++r)
        direction(r, c) = std::sin(0.31 * (1 + r + 7 * c));
    constexpr double epsilon = 1e-6;
    const double finite = (objective(a + epsilon * direction, seed) -
                           objective(a - epsilon * direction, seed)) /
                          (2.0 * epsilon);
    const double analytic = (block.array() * direction.array()).sum();
    expect(std::string(name) + " finite difference " +
               std::to_string(seed_variant),
           std::abs(finite - analytic) <=
               2e-7 * std::max({1.0, std::abs(finite), std::abs(analytic)}));
    std::printf("%s seed=%d max_abs=%.3g max_rel=%.3g fd_abs=%.3g\n", name,
                seed_variant, largest_abs, largest_rel,
                std::abs(finite - analytic));
  }
}

void check_orientation(const Mat& a) {
  const Mat seed = dense_seed(static_cast<int>(a.rows()), 0);
  const Mat reference = stan_active_reference(a, seed).gradient;
  const Mat correct = explicit_block_formula(a, seed, false);
  const Mat wrong = explicit_block_formula(a, seed, true);
  expect("block formula has correct orientation",
         max_abs(reference, correct) < 1e-11);
  expect("transposed-seed orientation is rejected",
         max_abs(reference, wrong) > 1e-4);
}

Mat random_matrix(int n) {
  std::mt19937 generator(24801);
  std::uniform_real_distribution<double> distribution(-0.12, 0.12);
  Mat a(n, n);
  for (int i = 0; i < n * n; ++i) a.data()[i] = distribution(generator);
  for (int i = 0; i < n; ++i) a(i, i) -= 0.15 + 0.01 * i;
  return a;
}

Mat ctsem_like_matrix(int n) {
  Mat a(n, n);
  for (int c = 0; c < n; ++c)
    for (int r = 0; r < n; ++r) {
      a(r, c) = 0.025 * std::sin(0.73 * (1 + r + 3 * c));
      if (r == c) a(r, c) -= 0.18 + 0.01 * r;
    }
  return a;
}

Mat nonnormal_matrix(int n) {
  Mat a = Mat::Zero(n, n);
  for (int i = 0; i < n; ++i) {
    a(i, i) = -0.07 * i;
    if (i + 1 < n) a(i, i + 1) = 0.8 - 0.04 * i;
    if (i > 0) a(i, i - 1) = -0.03 * i;
  }
  return a;
}

IslandProg cfg_matrix_exp(int n) {
  const int width = n * n;
  const int input = 1;
  const int output = input + width;
  IslandProg p;
  p.n_regs = 1 + 2 * width;
  p.ins = {{0, 1, -1, 0, false}, {input, width, -1, 0, true}};
  p.out_regs = {output};
  p.code = {{Program::MOVR, output, input, 0, 0, width},
            {Program::JZ, 3, 0},
            {Program::MATRIX_EXP, output, input, n, n, width}};
  return p;
}

void expect_structure(const char* name, const IslandProg& p, uint16_t opcode) {
  expect(std::string(name) + " generated", !p.adj.code.empty());
  expect(std::string(name) + " one synthetic call", p.calls.size() == 1);
  if (p.calls.size() != 1) return;
  const Program::Call& call = p.calls[0];
  expect(std::string(name) + " opcode", call.opcode == opcode);
  expect(std::string(name) + " backward-only metadata",
         call.scratch_len == 0 && p.code.size() == 3 &&
             p.code[2].code == Program::MATRIX_EXP &&
             std::none_of(p.code.begin(), p.code.end(),
                          [](const Program::Instr& instruction) {
                            return instruction.code == Program::CALL;
                          }));
  expect(std::string(name) + " exact shape metadata",
         call.n_in == 1 && call.idata.size() == 1 &&
             call.in_len[0] == call.idata[0] * call.idata[0] &&
             call.out_len == call.in_len[0]);
}

void check_force_seam() {
  constexpr const char* flag = "STANLI_CFG_MATRIX_EXP_BLOCK_FRECHET";
  constexpr const char* minimum = "STANLI_CFG_MATRIX_EXP_BLOCK_FRECHET_MIN_N";
  constexpr const char* escape = "STANLI_NO_CFG_MATRIX_EXP_BLOCK_FRECHET";
  test_unsetenv(flag);
  test_unsetenv(minimum);
  test_unsetenv(escape);

  IslandProg default_enabled = cfg_matrix_exp(6);
  expect("default block cfg generated",
         stanli::gen_cfg_adjoint(default_enabled));
  expect_structure("production n6", default_enabled,
                   stanli::OP_MATRIX_EXP_BLOCK_FRECHET);

  IslandProg default_small = cfg_matrix_exp(5);
  expect("small default cfg generated", stanli::gen_cfg_adjoint(default_small));
  expect_structure("production rejects n5", default_small,
                   stanli::OP_MATRIX_EXP);

  test_setenv(flag, "1");
  IslandProg forced_small = cfg_matrix_exp(5);
  expect("forced small block cfg generated",
         stanli::gen_cfg_adjoint(forced_small));
  expect_structure("force overrides production minimum", forced_small,
                   stanli::OP_MATRIX_EXP_BLOCK_FRECHET);

  test_setenv(minimum, "10");
  IslandProg below = cfg_matrix_exp(6);
  expect("below threshold cfg generated", stanli::gen_cfg_adjoint(below));
  expect_structure("below threshold", below, stanli::OP_MATRIX_EXP);
  IslandProg inclusive = cfg_matrix_exp(10);
  expect("inclusive threshold cfg generated",
         stanli::gen_cfg_adjoint(inclusive));
  expect_structure("inclusive threshold", inclusive,
                   stanli::OP_MATRIX_EXP_BLOCK_FRECHET);

  test_setenv(minimum, "invalid");
  IslandProg malformed_threshold = cfg_matrix_exp(10);
  expect("malformed threshold cfg generated",
         stanli::gen_cfg_adjoint(malformed_threshold));
  expect_structure("malformed threshold fails closed", malformed_threshold,
                   stanli::OP_MATRIX_EXP);

  test_unsetenv(flag);
  test_setenv(minimum, "6");
  IslandProg threshold_only = cfg_matrix_exp(10);
  expect("threshold-only cfg generated",
         stanli::gen_cfg_adjoint(threshold_only));
  expect_structure("threshold alone preserves production default",
                   threshold_only, stanli::OP_MATRIX_EXP_BLOCK_FRECHET);

  test_setenv(escape, "1");
  IslandProg escaped = cfg_matrix_exp(10);
  expect("escaped default cfg generated", stanli::gen_cfg_adjoint(escaped));
  expect_structure("escape disables production default", escaped,
                   stanli::OP_MATRIX_EXP);

  test_setenv(flag, "1");
  IslandProg escaped_force = cfg_matrix_exp(10);
  expect("escaped forced cfg generated",
         stanli::gen_cfg_adjoint(escaped_force));
  expect_structure("escape has precedence over force", escaped_force,
                   stanli::OP_MATRIX_EXP);

  test_setenv(escape, "0");
  test_unsetenv(flag);
  IslandProg zero_escape = cfg_matrix_exp(6);
  expect("zero escape cfg generated", stanli::gen_cfg_adjoint(zero_escape));
  expect_structure("zero escape preserves default", zero_escape,
                   stanli::OP_MATRIX_EXP_BLOCK_FRECHET);

  test_setenv(flag, "1");
  test_unsetenv(escape);
  test_unsetenv(minimum);
  IslandProg refused = cfg_matrix_exp(6);
  refused.code[2].c = 7;  // non-square/mismatched structured metadata
  const IslandProg before = refused;
  expect("malformed forced matrix-exp refused",
         !stanli::gen_cfg_adjoint(refused));
  expect("malformed refusal transactional",
         refused.n_regs == before.n_regs && refused.calls.empty() &&
             refused.adj.empty() && refused.trace_pc.empty() &&
             refused.code.size() == before.code.size() &&
             refused.code[2].c == before.code[2].c && !refused.var_replay);

  test_unsetenv(flag);
  test_unsetenv(minimum);
  test_unsetenv(escape);
}

}  // namespace

int main() {
  stanli::Graph graph;
  graph.result_slot = graph.add_slot(1, false);
  stanli::Executor register_kernels(std::move(graph));

  const Mat random6 = random_matrix(6);
  const Mat ctsem10 = ctsem_like_matrix(10);
  const Mat nonnormal10 = nonnormal_matrix(10);
  check_numeric_case("random6", random6);
  check_numeric_case("ctsem10", ctsem10);
  check_numeric_case("nonnormal10", nonnormal10);
  check_orientation(nonnormal10);
  check_force_seam();

  if (failures) return 1;
  std::puts("matrix-exp block-Frechet tests passed");
  return 0;
}
