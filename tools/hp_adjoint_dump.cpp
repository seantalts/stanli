// Developer-only bridge for tools/matrix_pullback_hp_check.py: for each
// case in a text file, compute the pre-native nested-tape adjoint (stan-math
// promoted to var, replayed, grad()) and the native kernel's adjoint, and
// print both so the Python side can compare each against an independent
// arbitrary-precision reference.
//
// matrix_exp cases: "matrix_exp <n>" then n*n A values then n*n G values
// (row-major). Prints n*n old values then n*n new values (row-major).
//
// solve cases: "solve <left01> <kind012> <n> <k>" then n*n A values then
// (br*bc) B values (row-major, br=left?n:k, bc=left?k:n). Both operands are
// active. Prints old adjA (n*n), old adjB (br*bc), new adjA (n*n), new
// adjB (br*bc), all row-major.
//
// One case per non-blank input line's worth of tokens; the file is just a
// flat token stream, read case by case.
#include <stanli/graph.hpp>
#include <stanli/optable.hpp>

#include <stan/math.hpp>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace stanli;
using stan::math::var;
using MatD = Eigen::MatrixXd;
using VarM = Eigen::Matrix<var, -1, -1>;

namespace {

// Row-major in, column-major Eigen out (and back), so the file format and
// KernelCtx's column-major slots don't have to agree.
MatD read_rm(std::istream& in, int64_t rows, int64_t cols) {
  MatD m(rows, cols);
  for (int64_t i = 0; i < rows; ++i)
    for (int64_t j = 0; j < cols; ++j) in >> m(i, j);
  return m;
}
void print_rm(const MatD& m) {
  for (int64_t i = 0; i < m.rows(); ++i)
    for (int64_t j = 0; j < m.cols(); ++j) std::printf("%.17g\n", m(i, j));
}

void run_matrix_exp(std::istream& in) {
  int64_t n;
  in >> n;
  const MatD a = read_rm(in, n, n);
  const MatD g = read_rm(in, n, n);

  MatD old_adj = MatD::Zero(n, n);
  {
    stan::math::nested_rev_autodiff nested;
    VarM av(n, n);
    for (int64_t i = 0; i < n; ++i)
      for (int64_t j = 0; j < n; ++j) av(i, j) = a(i, j);
    VarM out = stan::math::matrix_exp(av);
    var obj = stan::math::sum(stan::math::elt_multiply(out, MatD(g)));
    stan::math::grad(obj.vi_);
    for (int64_t i = 0; i < n; ++i)
      for (int64_t j = 0; j < n; ++j) old_adj(i, j) = av(i, j).adj();
  }

  MatD new_adj = MatD::Zero(n, n);
  {
    std::vector<double> a_cm(n * n), g_cm(n * n), out_v(n * n),
        adj_cm(n * n, 0.0);
    for (int64_t i = 0; i < n; ++i)
      for (int64_t j = 0; j < n; ++j) {
        a_cm[j * n + i] = a(i, j);
        g_cm[j * n + i] = g(i, j);
      }
    KernelCtx ctx;
    int dims[] = {(int)n};
    ctx.n_in = 1;
    ctx.idata = dims;
    ctx.n_idata = 1;
    ctx.in[0] = {a_cm.data(), n * n};
    ctx.in_adj[0] = {adj_cm.data(), n * n};
    ctx.out = {out_v.data(), n * n};
    ctx.out_adj_vec = {g_cm.data(), n * n};
    const Kernel& k = *find_kernel(OP_MATRIX_EXP);
    k.forward(ctx);
    k.backward(ctx);
    for (int64_t i = 0; i < n; ++i)
      for (int64_t j = 0; j < n; ++j) new_adj(i, j) = adj_cm[j * n + i];
  }

  print_rm(old_adj);
  print_rm(new_adj);
}

uint16_t solve_opcode(bool left, int kind) {
  if (left) {
    if (kind == 0) return OP_MDIVIDE_LEFT;
    if (kind == 1) return OP_MDIVIDE_LEFT_SPD;
    return OP_MDIVIDE_LEFT_TRI_LOW;
  }
  if (kind == 0) return OP_MDIVIDE_RIGHT;
  if (kind == 1) return OP_MDIVIDE_RIGHT_SPD;
  return OP_MDIVIDE_RIGHT_TRI_LOW;
}

VarM solve_at_kind(bool left, int kind, const VarM& a, const VarM& b) {
  if (kind == 1) {
    return left ? VarM(stan::math::mdivide_left_spd(a, b))
                : VarM(stan::math::mdivide_right_spd(b, a));
  }
  if (kind == 2) {
    return left ? VarM(stan::math::mdivide_left_tri_low(a, b))
                : VarM(stan::math::mdivide_right_tri_low(b, a));
  }
  return left ? VarM(stan::math::mdivide_left(a, b))
              : VarM(stan::math::mdivide_right(b, a));
}

void run_solve(std::istream& in) {
  int left01, kind, n, k;
  in >> left01 >> kind >> n >> k;
  const bool left = left01 != 0;
  const int64_t br = left ? n : k, bc = left ? k : n;
  const MatD a = read_rm(in, n, n);
  const MatD b = read_rm(in, br, bc);

  MatD old_adj_a = MatD::Zero(n, n), old_adj_b = MatD::Zero(br, bc);
  {
    stan::math::nested_rev_autodiff nested;
    VarM av(n, n), bv(br, bc);
    for (int64_t i = 0; i < n; ++i)
      for (int64_t j = 0; j < n; ++j) av(i, j) = a(i, j);
    for (int64_t i = 0; i < br; ++i)
      for (int64_t j = 0; j < bc; ++j) bv(i, j) = b(i, j);
    VarM out = solve_at_kind(left, kind, av, bv);
    MatD seed = MatD::Ones(out.rows(), out.cols());
    var obj = stan::math::sum(stan::math::elt_multiply(out, seed));
    stan::math::grad(obj.vi_);
    for (int64_t i = 0; i < n; ++i)
      for (int64_t j = 0; j < n; ++j) old_adj_a(i, j) = av(i, j).adj();
    for (int64_t i = 0; i < br; ++i)
      for (int64_t j = 0; j < bc; ++j) old_adj_b(i, j) = bv(i, j).adj();
  }

  MatD new_adj_a = MatD::Zero(n, n), new_adj_b = MatD::Zero(br, bc);
  {
    std::vector<double> a_cm(n * n), b_cm(br * bc), out_v(br * bc),
        seed_cm(br * bc, 1.0), a_adj_cm(n * n, 0.0), b_adj_cm(br * bc, 0.0);
    for (int64_t i = 0; i < n; ++i)
      for (int64_t j = 0; j < n; ++j) a_cm[j * n + i] = a(i, j);
    for (int64_t i = 0; i < br; ++i)
      for (int64_t j = 0; j < bc; ++j) b_cm[j * br + i] = b(i, j);
    KernelCtx ctx;
    int dims[] = {(int)n, (int)k};
    ctx.n_in = 2;
    ctx.idata = dims;
    ctx.n_idata = 2;
    const int ai = left ? 0 : 1, bi = left ? 1 : 0;
    ctx.in[ai] = {a_cm.data(), n * n};
    ctx.in[bi] = {b_cm.data(), br * bc};
    ctx.in_adj[ai] = {a_adj_cm.data(), n * n};
    ctx.in_adj[bi] = {b_adj_cm.data(), br * bc};
    ctx.out = {out_v.data(), br * bc};
    ctx.out_adj_vec = {seed_cm.data(), br * bc};
    ctx.variant = 1u | (3u << 2);  // active, both operands var
    const Kernel& kern = *find_kernel(solve_opcode(left, kind));
    kern.forward(ctx);
    kern.backward(ctx);
    for (int64_t i = 0; i < n; ++i)
      for (int64_t j = 0; j < n; ++j) new_adj_a(i, j) = a_adj_cm[j * n + i];
    for (int64_t i = 0; i < br; ++i)
      for (int64_t j = 0; j < bc; ++j) new_adj_b(i, j) = b_adj_cm[j * br + i];
  }

  print_rm(old_adj_a);
  print_rm(old_adj_b);
  print_rm(new_adj_a);
  print_rm(new_adj_b);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s cases.txt\n", argv[0]);
    return 2;
  }
  std::ifstream f(argv[1]);
  if (!f) {
    std::fprintf(stderr, "cannot open %s\n", argv[1]);
    return 2;
  }
  int64_t ncases;
  f >> ncases;
  for (int64_t c = 0; c < ncases; ++c) {
    std::string mode;
    f >> mode;
    if (mode == "matrix_exp") {
      run_matrix_exp(f);
    } else if (mode == "solve") {
      run_solve(f);
    } else {
      std::fprintf(stderr, "unknown mode %s\n", mode.c_str());
      return 2;
    }
  }
  return 0;
}
