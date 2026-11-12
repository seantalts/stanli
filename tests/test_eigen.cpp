// Symmetric eigendecomposition kernels retain the decomposition their
// pullbacks need.  Pin the forward values and native pullbacks against the
// exact stan-math calls, including reuse of one Executor: stale eigenvectors
// or eigenvalues in scratch produce a plausible but wrong second gradient.
#include "expect.hpp"

#include <stanli/graph.hpp>
#include <stanli/optable.hpp>

#include <stan/math.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

using MatD = Eigen::MatrixXd;
using VecD = Eigen::VectorXd;
using VarM = Eigen::Matrix<stan::math::var, -1, -1>;

void check(bool ok, const std::string& what) {
  if (!ok) {
    ++failures;
    std::printf("FAIL %s\n", what.c_str());
  }
}

MatD first_matrix() {
  MatD a(3, 3);
  a << 4.0, 0.7, -0.2, 0.7, 2.5, 0.4, -0.2, 0.4, 1.2;
  return a;
}

MatD second_matrix() {
  MatD a(3, 3);
  a << 1.8, -0.3, 0.6, -0.3, 3.7, -0.5, 0.6, -0.5, 2.4;
  return a;
}

std::vector<double> weights(int n) {
  std::vector<double> w((size_t)n);
  for (int i = 0; i < n; ++i) w[(size_t)i] = 0.17 * (i + 1) - 0.23 * (i % 3);
  return w;
}

struct Result {
  std::vector<double> value;
  std::vector<double> grad;
};

stanli::Graph one_eigen_graph(uint16_t opcode, int n) {
  using namespace stanli;
  const int out_len = opcode == OP_EIGENVALUES_SYM ? n : n * n;
  Graph g;
  const int a = g.add_slot(n * n, true);
  const int w = g.add_slot(out_len, false);
  const int out = g.add_slot(out_len, false);
  const int lp = g.add_slot(1, false);
  g.add_op(opcode, {a}, out, {n});
  g.add_op(OP_DOT, {out, w}, lp);
  g.result_slot = lp;
  return g;
}

// The slot numbering in one_eigen_graph is deliberate and fixed here: input,
// weights, eigendecomposition output, scalar result.
class Runner {
 public:
  Runner(uint16_t opcode, int n)
      : opcode_(opcode), n_(n), ex_(one_eigen_graph(opcode, n)) {}

  Result run(const MatD& a, const std::vector<double>& w) {
    fill(a, w);
    Result r;
    r.grad.resize((size_t)n_ * n_);
    (void)ex_.gradient(r.grad.data());
    read_value(r);
    return r;
  }

  Result run_value_only(const MatD& a, const std::vector<double>& w) {
    fill(a, w);
    Result r;
    (void)ex_.forward_value_only();
    read_value(r);
    return r;
  }

 private:
  void fill(const MatD& a, const std::vector<double>& w) {
    for (int i = 0; i < n_ * n_; ++i) ex_.params_data()[i] = a.data()[i];
    for (size_t i = 0; i < w.size(); ++i) ex_.value_ptr(1)[i] = w[i];
  }

  void read_value(Result& r) {
    const int out_len = opcode_ == stanli::OP_EIGENVALUES_SYM ? n_ : n_ * n_;
    r.value.assign(ex_.value_ptr(2), ex_.value_ptr(2) + out_len);
  }

  uint16_t opcode_;
  int n_;
  stanli::Executor ex_;
};

std::vector<double> reference_grad(uint16_t opcode, const MatD& a,
                                   const std::vector<double>& w) {
  stan::math::nested_rev_autodiff nested;
  const int n = (int)a.rows();
  VarM av(n, n);
  for (int i = 0; i < n * n; ++i) av.data()[i] = a.data()[i];
  auto pullback = [&](const auto& out) {
    MatD seed(out.rows(), out.cols());
    for (int i = 0; i < seed.size(); ++i) seed.data()[i] = w[(size_t)i];
    stan::math::var lp = stan::math::sum(stan::math::elt_multiply(out, seed));
    stan::math::grad(lp.vi_);
    std::vector<double> grad((size_t)n * n);
    for (int i = 0; i < n * n; ++i) grad[(size_t)i] = av.data()[i].adj();
    return grad;
  };
  if (opcode == stanli::OP_EIGENVALUES_SYM)
    return pullback(stan::math::eigenvalues_sym(av));
  return pullback(stan::math::eigenvectors_sym(av));
}

std::vector<double> reference_value(uint16_t opcode, const MatD& a) {
  if (opcode == stanli::OP_EIGENVALUES_SYM) {
    VecD out = stan::math::eigenvalues_sym(a);
    return std::vector<double>(out.data(), out.data() + out.size());
  }
  MatD out = stan::math::eigenvectors_sym(a);
  return std::vector<double>(out.data(), out.data() + out.size());
}

void compare(uint16_t opcode, const std::string& name, Runner& runner,
             const MatD& a) {
  const int out_len =
      opcode == stanli::OP_EIGENVALUES_SYM ? (int)a.rows() : (int)a.size();
  const std::vector<double> w = weights(out_len);
  const Result got = runner.run(a, w);
  const std::vector<double> want_value = reference_value(opcode, a);
  const std::vector<double> want_grad = reference_grad(opcode, a, w);
  for (size_t i = 0; i < got.value.size(); ++i)
    expect_eq(name + " value[" + std::to_string(i) + "]", got.value[i],
              want_value[i]);
  for (size_t i = 0; i < got.grad.size(); ++i)
    expect_eq(name + " grad[" + std::to_string(i) + "]", got.grad[i],
              want_grad[i]);
}

void compare_value_only(uint16_t opcode, const std::string& name,
                        Runner& runner, const MatD& a) {
  const int out_len =
      opcode == stanli::OP_EIGENVALUES_SYM ? (int)a.rows() : (int)a.size();
  const Result got = runner.run_value_only(a, weights(out_len));
  const std::vector<double> want = reference_value(opcode, a);
  for (size_t i = 0; i < got.value.size(); ++i)
    expect_eq(name + " value-only[" + std::to_string(i) + "]", got.value[i],
              want[i]);
}

void check_full_solver_value_parity(const MatD& a, const std::string& name) {
  Eigen::SelfAdjointEigenSolver<MatD> solver(a);
  const VecD values_only = stan::math::eigenvalues_sym(a);
  const MatD vectors_prim = stan::math::eigenvectors_sym(a);
  for (int i = 0; i < a.rows(); ++i)
    expect_eq(name + " full/value-only eigenvalue " + std::to_string(i),
              solver.eigenvalues()(i), values_only(i));
  for (int i = 0; i < a.size(); ++i)
    expect_eq(name + " full/prim eigenvector " + std::to_string(i),
              solver.eigenvectors().data()[i], vectors_prim.data()[i]);
}

void check_scratch_sizes() {
  using namespace stanli;
  int n_data[] = {3};
  Slot slots[] = {{0, 9, true}, {0, 3, false}, {0, 9, false}};
  Op vals;
  vals.opcode = OP_EIGENVALUES_SYM;
  vals.in[0] = 0;
  vals.n_in = 1;
  vals.out = 1;
  vals.idata = n_data;
  vals.n_idata = 1;
  Op vecs = vals;
  vecs.opcode = OP_EIGENVECTORS_SYM;
  vecs.out = 2;
  const Kernel* kv = find_kernel(OP_EIGENVALUES_SYM);
  const Kernel* kV = find_kernel(OP_EIGENVECTORS_SYM);
  check(kv && kv->scratch_size, "eigenvalues scratch callback");
  check(kV && kV->scratch_size, "eigenvectors scratch callback");
  if (kv && kv->scratch_size)
    check(kv->scratch_size(vals, slots) == 9,
          "eigenvalues retain n*n eigenvectors");
  if (kV && kV->scratch_size)
    check(kV->scratch_size(vecs, slots) == 3,
          "eigenvectors retain n eigenvalues");
  n_data[0] = 0;
  if (kv && kv->scratch_size)
    check(kv->scratch_size(vals, slots) == 0, "zero eigenvalues scratch");
  if (kV && kV->scratch_size)
    check(kV->scratch_size(vecs, slots) == 0, "zero eigenvectors scratch");
}

}  // namespace

int main() {
  using namespace stanli;
  const MatD a = first_matrix();
  const MatD b = second_matrix();

  // Eigen's full and EigenvaluesOnly paths must agree bitwise before the
  // eigenvalues kernel may retain vectors from the full decomposition.
  check_full_solver_value_parity(a, "3x3 a");
  check_full_solver_value_parity(b, "3x3 b");
  MatD r(30, 30);
  for (int j = 0; j < r.cols(); ++j)
    for (int i = 0; i < r.rows(); ++i)
      r(i, j) = std::sin(0.13 * (i + 1) * (j + 2)) + 0.01 * (i - j);
  MatD big = r * r.transpose();
  big.diagonal().array() += 0.5;
  check_full_solver_value_parity(big, "30x30");

  Runner vals(OP_EIGENVALUES_SYM, 3);
  Runner vecs(OP_EIGENVECTORS_SYM, 3);
  compare_value_only(OP_EIGENVALUES_SYM, "eigenvalues", vals, a);
  compare_value_only(OP_EIGENVECTORS_SYM, "eigenvectors", vecs, a);
  compare(OP_EIGENVALUES_SYM, "eigenvalues first", vals, a);
  compare(OP_EIGENVECTORS_SYM, "eigenvectors first", vecs, a);
  // Reuse the same Executors in both directions.  This catches scratch that
  // is only initialized once as well as a partial overwrite on a later run.
  compare(OP_EIGENVALUES_SYM, "eigenvalues second", vals, b);
  compare(OP_EIGENVECTORS_SYM, "eigenvectors second", vecs, b);
  compare(OP_EIGENVALUES_SYM, "eigenvalues repeat", vals, a);
  compare(OP_EIGENVECTORS_SYM, "eigenvectors repeat", vecs, a);

  MatD scalar(1, 1);
  scalar(0, 0) = 2.75;
  Runner scalar_vals(OP_EIGENVALUES_SYM, 1);
  Runner scalar_vecs(OP_EIGENVECTORS_SYM, 1);
  compare(OP_EIGENVALUES_SYM, "eigenvalues 1x1", scalar_vals, scalar);
  compare(OP_EIGENVECTORS_SYM, "eigenvectors 1x1", scalar_vecs, scalar);

  // Real kronecker_gp shape: this exercises Eigen's blocked products over
  // CMap operands in both native pullbacks, not just scalar-size kernels.
  Runner big_vals(OP_EIGENVALUES_SYM, 30);
  Runner big_vecs(OP_EIGENVECTORS_SYM, 30);
  compare(OP_EIGENVALUES_SYM, "eigenvalues 30x30", big_vals, big);
  compare(OP_EIGENVECTORS_SYM, "eigenvectors 30x30", big_vecs, big);
  check_scratch_sizes();

  return finish("test_eigen");
}
