// The Laplace whitening builder (runtime/src/laplace.cpp) on log densities
// written here with Stan Math, whose reference Hessians (autodiff) and known
// sparsity patterns are the expected values. The C API wrapper and real Stan
// models are in test_laplace_capi.cpp.
#include "expect.hpp"

#include <stanli/laplace.hpp>
#include <stanli/sparse_factor.hpp>

#include <stan/math.hpp>

#include <Eigen/Dense>
#include <Eigen/Sparse>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace {

using Eigen::MatrixXd;
using Eigen::VectorXd;
using stanli::laplace::Gradient;
using stanli::laplace::Sparse;

// A log density written once for doubles and autodiff types.
template <typename F>
struct Model {
  F f;
  int n;
  stanli::laplace::GradientFn gradient() const {
    return [f = f](const VectorXd& q, VectorXd& g) {
      double lp;
      stan::math::gradient(f, q, lp, g);
      if (!std::isfinite(lp)) throw stanli::laplace::EvalError("not finite");
      return lp;
    };
  }
  // Central differences of the gradient, one column at a time: the dense
  // reference that the coloured and probed pieces are checked against.
  stanli::laplace::HessianFn hessian() const {
    return [f = f](const VectorXd& q) {
      const int n = (int)q.size();
      MatrixXd h(n, n);
      for (int j = 0; j < n; ++j) {
        const double step = 1e-5 * std::max(1.0, std::fabs(q[j]));
        VectorXd up = q, down = q, gu, gd;
        up[j] += step;
        down[j] -= step;
        double lp;
        stan::math::gradient(f, up, lp, gu);
        stan::math::gradient(f, down, lp, gd);
        h.col(j) = (gu - gd) / (2 * step);
      }
      return MatrixXd((h + h.transpose()) / 2);
    };
  }
  MatrixXd reference_hessian(const VectorXd& q) const { return hessian()(q); }
};
template <typename F>
Model<F> model(F f, int n) {
  return {f, n};
}

// A centred hierarchical model: q = (mu, log sigma_a, a[1..J], log sigma_y).
struct Hierarchical {
  int J;
  std::vector<int> g;
  std::vector<double> y;
  template <typename T>
  T operator()(const Eigen::Matrix<T, -1, 1>& q) const {
    using stan::math::exp;
    using stan::math::normal_lpdf;
    const T mu = q[0];
    const T sa = exp(q[1]);
    const T sy = exp(q[J + 2]);
    T lp = normal_lpdf<false>(sa, 0, 1) + q[1] + normal_lpdf<false>(sy, 0, 1) +
           q[J + 2] + normal_lpdf<false>(mu, 0, 5);
    for (int j = 0; j < J; ++j) lp += normal_lpdf<false>(q[2 + j], mu, sa);
    for (size_t i = 0; i < y.size(); ++i)
      lp += normal_lpdf<false>(y[i], q[2 + g[i]], sy);
    return lp;
  }
};
Hierarchical hierarchical(int J, int per_group, unsigned seed) {
  std::mt19937 gen(seed);
  std::normal_distribution<double> z(0, 1);
  Hierarchical h{J, {}, {}};
  std::vector<double> a(J);
  for (auto& v : a) v = 1.3 + 0.4 * z(gen);
  for (int j = 0; j < J; ++j)
    for (int k = 0; k < per_group; ++k) {
      h.g.push_back(j);
      h.y.push_back(a[j] + 0.7 * z(gen));
    }
  return h;
}
std::vector<int> range(int from, int to) {
  std::vector<int> out;
  for (int i = from; i < to; ++i) out.push_back(i);
  return out;
}

// A random walk observed with noise: q = (log s, log s_y, x[1..K]); the
// Hessian over x is tridiagonal.
struct RandomWalk {
  std::vector<double> y;
  template <typename T>
  T operator()(const Eigen::Matrix<T, -1, 1>& q) const {
    using stan::math::exp;
    using stan::math::normal_lpdf;
    const int K = (int)y.size();
    const T s = exp(q[0]);
    const T sy = exp(q[1]);
    T lp = normal_lpdf<false>(s, 0, 1) + q[0] + normal_lpdf<false>(sy, 0, 1) +
           q[1] + normal_lpdf<false>(q[2], 0, 3);
    for (int k = 1; k < K; ++k) lp += normal_lpdf<false>(q[2 + k], q[1 + k], s);
    for (int k = 0; k < K; ++k) lp += normal_lpdf<false>(y[k], q[2 + k], sy);
    return lp;
  }
};
RandomWalk random_walk(int K, unsigned seed) {
  std::mt19937 gen(seed);
  std::normal_distribution<double> z(0, 0.3);
  RandomWalk r;
  double acc = 0;
  for (int k = 0; k < K; ++k) r.y.push_back(acc += z(gen));
  return r;
}

// Every latent entry coupled to every other: q = (mu, a[1..J]).
struct Hub {
  int J;
  template <typename T>
  T operator()(const Eigen::Matrix<T, -1, 1>& q) const {
    using stan::math::normal_lpdf;
    T lp = normal_lpdf<false>(q[0], 0, 3);
    T sum = 0;
    for (int j = 0; j < J; ++j) {
      lp += normal_lpdf<false>(q[1 + j], q[0], 1);
      sum += q[1 + j];
    }
    return lp - 0.5 * sum * sum / J;
  }
};

// sum(c_j x_j^2 / 2 - x_j^4 / 4) + sum_k normal(y_k | slope x_(k mod J), 1)
// with c_j = 1 + spread j: q = (x[1..J], y[1..K]). At q = 0 the negative
// Hessian has one negative eigenvalue per x.
struct Saddles {
  int J;
  int K = J;
  double slope = 0.5;
  double spread = 0;
  template <typename T>
  T operator()(const Eigen::Matrix<T, -1, 1>& q) const {
    T lp = 0;
    for (int j = 0; j < J; ++j) {
      const T x = q[j];
      const double c = 1 + spread * j;
      lp += c * x * x / 2 - x * x * x * x / 4;
    }
    for (int k = 0; k < K; ++k) {
      const T r = q[J + k] - slope * q[k % J];
      lp -= r * r / 2;
    }
    return lp;
  }
};

// -q' A q / 2 + b' q: the Laplace approximation is exact, so the marginal
// mode is the joint mode A^-1 b and the precision is A.
struct Gaussian {
  MatrixXd a;
  VectorXd b;
  template <typename T>
  T operator()(const Eigen::Matrix<T, -1, 1>& q) const {
    T lp = 0;
    for (int i = 0; i < a.rows(); ++i) {
      lp += b[i] * q[i];
      for (int j = 0; j < a.cols(); ++j)
        if (a(i, j) != 0) lp -= 0.5 * a(i, j) * q[i] * q[j];
    }
    return lp;
  }
};

Sparse to_sparse(const MatrixXd& a) { return a.sparseView(); }

bool same_pattern(const Sparse& a, const Sparse& b) {
  if (a.rows() != b.rows() || a.cols() != b.cols()) return false;
  const MatrixXd x = MatrixXd(a), y = MatrixXd(b);
  return ((x.array() != 0) == (y.array() != 0)).all();
}

// The dense pattern stanli_laplace's thresholding should give, computed
// here directly from the reference Hessian.
MatrixXd expected_pattern(const MatrixXd& h, const std::vector<int>& iu,
                          double tol) {
  const int m = (int)iu.size();
  double fallback = h.cwiseAbs().maxCoeff();
  MatrixXd p = MatrixXd::Zero(m, m);
  for (int a = 0; a < m; ++a)
    for (int b = 0; b < m; ++b) {
      const int i = iu[a], j = iu[b];
      double scale = std::sqrt(std::fabs(h(i, i)) * std::fabs(h(j, j)));
      if (scale == 0) scale = fallback;
      if (std::fabs(h(i, j)) > tol * scale || a == b) {
        p(a, b) = 1;
        p(b, a) = 1;
      }
    }
  return p;
}

double max_abs(const MatrixXd& a) { return a.cwiseAbs().maxCoeff(); }

VectorXd random_point(int n, unsigned seed, double scale) {
  std::mt19937 gen(seed);
  std::normal_distribution<double> z(0, 1);
  VectorXd q(n);
  for (int i = 0; i < n; ++i) q[i] = scale * z(gen);
  return q;
}

// ---- colouring and the coloured Hessian --------------------------------

void test_colouring_is_valid_and_small_for_a_band() {
  std::mt19937 gen(8);
  std::uniform_real_distribution<double> u(0, 1);
  const int n = 60;
  MatrixXd s = MatrixXd::Identity(n, n);
  for (int i = 0; i < n; ++i)
    for (int j = 0; j < i; ++j)
      if (u(gen) < 0.04) s(i, j) = s(j, i) = 1;
  const auto colour = stanli::laplace::distance2_colouring(to_sparse(s));
  const int n_colours = *std::max_element(colour.begin(), colour.end()) + 1;
  for (int c = 0; c < n_colours; ++c)
    for (int i = 0; i < n; ++i) {
      int hits = 0;
      for (int j = 0; j < n; ++j) hits += colour[j] == c && s(i, j) != 0;
      expect_true(
          "colour " + std::to_string(c) + " shares row " + std::to_string(i),
          hits <= 1);
    }
  MatrixXd band = MatrixXd::Zero(n, n);
  for (int i = 0; i < n; ++i)
    for (int j = std::max(0, i - 1); j <= std::min(n - 1, i + 1); ++j)
      band(i, j) = 1;
  const auto bc = stanli::laplace::distance2_colouring(to_sparse(band));
  expect_true("a band takes three colours",
              *std::max_element(bc.begin(), bc.end()) == 2);
}

void test_sparse_hessian_matches_the_exact_one() {
  const auto h = hierarchical(14, 10, 5);
  const auto m = model(h, 14 + 3);
  const int n = m.n;
  const VectorXd q = random_point(n, 0, 0.3);
  const MatrixXd exact = m.reference_hessian(q);
  const Sparse pattern =
      stanli::laplace::threshold_pattern(to_sparse(exact), 1e-6);
  std::vector<int> theta = {0, 1, n - 1};
  Gradient grad(m.gradient());
  const auto plan = stanli::laplace::hessian_plan(pattern, theta);
  const Sparse got =
      stanli::laplace::sparse_hessian(grad, q, plan, range(0, n));
  const double err = max_abs(MatrixXd(got) - exact) / max_abs(exact);
  expect_true("coloured Hessian within 1e-6 of the reference one, got " +
                  std::to_string(err),
              err <= 1e-6);
  expect_true("coloured Hessian is cheaper than 2n gradients",
              grad.count < 2 * n);
  expect_true("coloured Hessian costs two per colour and dense column",
              grad.count == 2 * (plan.n_colours + (int)plan.dense.size()));

  const auto iu = range(2, 16);
  const Sparse block = stanli::laplace::submatrix(pattern, iu);
  Gradient grad_block(m.gradient());
  const auto block_plan = stanli::laplace::hessian_plan(block, {});
  const Sparse got_block =
      stanli::laplace::sparse_hessian(grad_block, q, block_plan, iu);
  MatrixXd want_block(iu.size(), iu.size());
  for (size_t a = 0; a < iu.size(); ++a)
    for (size_t b = 0; b < iu.size(); ++b)
      want_block(a, b) = exact(iu[a], iu[b]);
  expect_true(
      "block Hessian matches the reference block",
      max_abs(MatrixXd(got_block) - want_block) <= 1e-6 * max_abs(exact));
  expect_true("block Hessian costs two gradients per colour",
              grad_block.count == 2 * block_plan.n_colours);
}

// ---- the probed pattern ------------------------------------------------

template <typename F>
int64_t check_probe_equals_exact(const std::string& what, const Model<F>& m,
                                 const std::vector<int>& iu, unsigned seed) {
  const VectorXd q = random_point(m.n, seed, 0.3);
  const MatrixXd want = expected_pattern(m.reference_hessian(q), iu, 1e-6);
  Gradient grad(m.gradient());
  const auto got = stanli::laplace::probe_pattern(grad, q, iu, 1e-6, 1);
  expect_true(what + ": the probe verified its pattern", got.has_value());
  if (got)
    expect_true(what + ": probe pattern equals the reference Hessian's",
                same_pattern(*got, to_sparse(want)));
  const MatrixXd dense = m.hessian()(q);
  expect_true(what + ": dense_pattern equals the reference Hessian's",
              same_pattern(stanli::laplace::dense_pattern(dense, iu, 1e-6),
                           to_sparse(want)));
  return grad.count;
}

void test_probe_finds_the_exact_pattern() {
  const auto h = hierarchical(14, 10, 5);
  check_probe_equals_exact("hierarchical", model(h, 17), range(2, 16), 3);
  for (int K : {40, 600}) {
    const auto rw = random_walk(K, 2);
    const int64_t grads =
        check_probe_equals_exact("random walk K=" + std::to_string(K),
                                 model(rw, K + 2), range(2, K + 2), 4);
    expect_true("random walk probe is cheaper than the dense Hessian",
                grads < 1 + 2 * (K + 2));
  }
  check_probe_equals_exact("dense hub", model(Hub{40}, 41), range(1, 41), 3);
  check_probe_equals_exact("one latent coordinate", model(Saddles{1}, 2), {1},
                           3);
}

void test_probe_gradients_do_not_grow_with_groups() {
  std::vector<int64_t> counts;
  for (int groups : {300, 1200}) {
    const auto h = hierarchical(groups, 10, 5);
    const auto m = model(h, groups + 3);
    const VectorXd q = random_point(m.n, 2, 0.3);
    Gradient grad(m.gradient());
    const auto got =
        stanli::laplace::probe_pattern(grad, q, range(2, groups + 2), 1e-6, 1);
    expect_true("probe verified with " + std::to_string(groups) + " groups",
                got.has_value());
    counts.push_back(grad.count);
  }
  expect_true("probe gradients grow by at most 20 from 300 to 1200 groups",
              counts[1] <= counts[0] + 20);
  expect_true("probe uses an eighth of the dense Hessian's gradients",
              counts[1] < (1 + 2 * 1203) / 8);
}

void test_verification_fails_on_lossy_candidates() {
  const auto m = model(Hub{40}, 41);
  const VectorXd q = random_point(m.n, 3, 0.3);
  const auto iu = range(1, 41);
  const VectorXd step = VectorXd::Constant(40, 1e-4).cwiseMax(
      1e-4 * VectorXd(q.segment(1, 40).cwiseAbs()));
  Sparse identity(40, 40);
  identity.setIdentity();
  Gradient grad(m.gradient());
  stanli::laplace::Rng rng = stan::services::util::create_rng(1, 1);
  expect_true("identity candidates fail the verification",
              !stanli::laplace::verified_pattern(grad, q, iu, step, identity,
                                                 1e-6, rng));
}

void test_verification_catches_one_missed_entry() {
  const auto rw = random_walk(60, 7);
  const auto m = model(rw, 62);
  const VectorXd q = random_point(m.n, 3, 0.3);
  const auto iu = range(2, 62);
  VectorXd step(60);
  for (int k = 0; k < 60; ++k)
    step[k] = 1e-4 * std::max(1.0, std::fabs(q[iu[k]]));
  Gradient grad0(m.gradient());
  const VectorXd g0 = grad0.at(q)(Eigen::seqN(2, 60));
  stanli::laplace::Rng rng0 = stan::services::util::create_rng(1, 1);
  const Sparse full =
      stanli::laplace::probe_candidates(grad0, q, iu, g0, step, rng0);
  int caught = 0, tried = 0;
  for (int i = 3; i < 59; i += 9) {
    MatrixXd c = MatrixXd(full);
    expect_true("the candidates hold the random walk's entry",
                c(i, i + 1) != 0);
    c(i, i + 1) = c(i + 1, i) = 0;
    Gradient grad(m.gradient());
    stanli::laplace::Rng rng = stan::services::util::create_rng(1, 1);
    caught += !stanli::laplace::verified_pattern(grad, q, iu, step,
                                                 to_sparse(c), 1e-6, rng);
    ++tried;
  }
  expect_true("every missed entry is caught: " + std::to_string(caught) +
                  " of " + std::to_string(tried),
              caught == tried);
}

void test_probe_agrees_with_exact_and_not_with_diagonal() {
  const auto rw = random_walk(40, 2);
  const auto m = model(rw, 42);
  const VectorXd q = random_point(m.n, 5, 0.3);
  const auto iu = range(2, 42);
  VectorXd step(40);
  for (int k = 0; k < 40; ++k)
    step[k] = 1e-4 * std::max(1.0, std::fabs(q[iu[k]]));
  const MatrixXd exact = m.reference_hessian(q).block(2, 2, 40, 40);
  Gradient grad(m.gradient());
  stanli::laplace::Rng rng = stan::services::util::create_rng(1, 1);
  expect_true("the reference block agrees",
              stanli::laplace::probe_agrees(grad, q, iu, to_sparse(exact), step,
                                            rng, 1e-6));
  const MatrixXd diagonal = exact.diagonal().asDiagonal();
  expect_true("the diagonal alone disagrees",
              !stanli::laplace::probe_agrees(grad, q, iu, to_sparse(diagonal),
                                             step, rng, 1e-6));
}

// ---- eigenpairs and the sparse factor ----------------------------------

MatrixXd random_orthogonal(int n, unsigned seed) {
  std::mt19937 gen(seed);
  std::normal_distribution<double> z(0, 1);
  MatrixXd a(n, n);
  for (int i = 0; i < n; ++i)
    for (int j = 0; j < n; ++j) a(i, j) = z(gen);
  return Eigen::HouseholderQR<MatrixXd>(a).householderQ();
}

void test_bad_eigenpairs_with_a_wide_spectrum() {
  const int n = 144;
  VectorXd w(n);
  const double low[] = {-1.1, -0.5, 0.2, 0.3, 0.35, 0.43, 0.57, 0.64, 0.67};
  for (int i = 0; i < 9; ++i) w[i] = low[i];
  for (int i = 9; i < n; ++i)
    w[i] = 1.9 * std::pow(5e5 / 1.9, double(i - 9) / (n - 10));
  const MatrixXd v = random_orthogonal(n, 3);
  const MatrixXd q = v * w.asDiagonal() * v.transpose();
  const auto r = stanli::laplace::bad_eigenpairs(to_sparse(q), 1e-8, 2);
  expect_true("two bad eigenvalues", r.values.size() == 2);
  if (r.values.size() == 2) {
    expect_true("they are -1.1 and -0.5",
                std::fabs(r.values[0] + 1.1) < 1e-8 &&
                    std::fabs(r.values[1] + 0.5) < 1e-8);
    expect_true(
        "their vectors are eigenvectors",
        max_abs(q * r.vectors - r.vectors * r.values.asDiagonal()) < 1e-6);
  }
  expect_true("smallest eigenvalue", std::fabs(r.min + 1.1) < 1e-8);
  expect_true("largest eigenvalue", std::fabs(r.max - 5e5) < 1e-3);
}

void test_bad_eigenpairs_finds_one_negative_among_many() {
  const int n = 300;
  VectorXd d(n);
  for (int i = 0; i < n; ++i) d[i] = 1 + 399.0 * i / (n - 1);
  d[149] = -80;
  Sparse q(n, n);
  for (int i = 0; i < n; ++i) q.insert(i, i) = d[i];
  const auto r = stanli::laplace::bad_eigenpairs(q, 1e-8, 1);
  expect_true("one bad eigenvalue", r.values.size() == 1);
  if (r.values.size() == 1) {
    expect_true("it is -80", std::fabs(r.values[0] + 80) < 1e-8);
    VectorXd e = VectorXd::Zero(n);
    e[149] = 1;
    expect_true("its vector is e_150",
                (r.vectors.col(0).cwiseAbs() - e).cwiseAbs().maxCoeff() < 1e-8);
  }
}

struct Csc {
  std::vector<int64_t> col_ptr, row_idx;
  std::vector<double> values;
};
Csc lower_csc(const MatrixXd& a) {
  Csc c;
  c.col_ptr.push_back(0);
  for (int j = 0; j < a.cols(); ++j) {
    for (int i = j; i < a.rows(); ++i)
      if (a(i, j) != 0) {
        c.row_idx.push_back(i);
        c.values.push_back(a(i, j));
      }
    c.col_ptr.push_back((int64_t)c.row_idx.size());
  }
  return c;
}
MatrixXd factor_l(const stanli::SparseFactor& f) {
  MatrixXd l = MatrixXd::Zero(f.n, f.n);
  for (int64_t j = 0; j < f.n; ++j)
    for (int64_t p = f.col_ptr[j]; p < f.col_ptr[j + 1]; ++p)
      l(f.row_idx[p], j) = f.values[p];
  return l;
}
MatrixXd permuted(const MatrixXd& a, const std::vector<int64_t>& perm) {
  MatrixXd out(a.rows(), a.cols());
  for (int i = 0; i < a.rows(); ++i)
    for (int j = 0; j < a.cols(); ++j) out(i, j) = a(perm[i], perm[j]);
  return out;
}
MatrixXd banded(int n, unsigned seed, const std::vector<int>& negative) {
  std::mt19937 gen(seed);
  std::uniform_real_distribution<double> d(2, 3), off(0.2, 0.6), neg(0.5, 1);
  MatrixXd a = MatrixXd::Zero(n, n);
  for (int i = 0; i < n; ++i) a(i, i) = d(gen);
  for (int i : negative) a(i, i) = -neg(gen);
  for (int i = 0; i + 1 < n; ++i) a(i, i + 1) = a(i + 1, i) = -off(gen);
  return a;
}

void test_ldlt_reproduces_the_matrix() {
  std::mt19937 gen(1);
  std::uniform_real_distribution<double> u(0, 1), diag(2.5, 3.5);
  const int n = 40;
  MatrixXd a = MatrixXd::Zero(n, n);
  for (int i = 0; i < n; ++i)
    for (int j = 0; j < i; ++j)
      if (u(gen) < 0.08) a(i, j) = a(j, i) = 0.8 * u(gen);
  for (int i = 0; i < n; ++i) a(i, i) = diag(gen);
  std::vector<int64_t> shuffled(n), identity(n);
  for (int i = 0; i < n; ++i) shuffled[i] = identity[i] = i;
  std::shuffle(shuffled.begin(), shuffled.end(), gen);
  const Csc c = lower_csc(a);
  for (const std::vector<int64_t>* perm :
       {(std::vector<int64_t>*)nullptr, &shuffled, &identity}) {
    const auto f =
        stanli::sparse_ldlt(n, c.col_ptr.data(), c.row_idx.data(),
                            c.values.data(), perm ? perm->data() : nullptr);
    const MatrixXd l = factor_l(f);
    const VectorXd d = Eigen::Map<const VectorXd>(f.d.data(), n);
    expect_true("L D L' reproduces P A P'",
                max_abs(l * d.asDiagonal() * l.transpose() -
                        permuted(a, f.perm)) <= 4e-12);
    expect_true("unit diagonal", (l.diagonal().array() == 1).all());
    expect_true("no negative pivots", f.n_negative == 0);
    expect_true("log |det|",
                std::fabs(f.log_abs_det - std::log(a.determinant())) <= 1e-10);
    if (perm) expect_true("given order kept", f.perm == *perm);
  }
  const MatrixXd b = banded(30, 3, {4, 17});
  const Csc cb = lower_csc(b);
  const auto fb = stanli::sparse_ldlt(30, cb.col_ptr.data(), cb.row_idx.data(),
                                      cb.values.data(), nullptr);
  Eigen::SelfAdjointEigenSolver<MatrixXd> eig(b);
  expect_true("negative pivots count negative eigenvalues",
              fb.n_negative == (eig.eigenvalues().array() < 0).count());
  expect_true(
      "indefinite log |det|",
      std::fabs(fb.log_abs_det -
                eig.eigenvalues().cwiseAbs().array().log().sum()) <= 1e-9);
}

void test_ldlt_refusals() {
  MatrixXd z(2, 2);
  z << 0, 1, 1, 0;
  const Csc c = lower_csc(z);
  const int64_t order[] = {0, 1};
  bool pivot = false;
  try {
    stanli::sparse_ldlt(2, c.col_ptr.data(), c.row_idx.data(), c.values.data(),
                        order);
  } catch (const std::runtime_error& e) {
    pivot = std::string(e.what()).find("pivot") != std::string::npos;
  }
  expect_true("a zero pivot is reported", pivot);
  const Csc cb = lower_csc(banded(5, 0, {}));
  const int64_t bad[] = {0, 1, 2, 2, 4};
  bool refused = false;
  try {
    stanli::sparse_ldlt(5, cb.col_ptr.data(), cb.row_idx.data(),
                        cb.values.data(), bad);
  } catch (const std::invalid_argument& e) {
    refused = std::string(e.what()).find("permutation") != std::string::npos;
  }
  expect_true("a bad permutation is refused", refused);
}

void check_lowrank(const MatrixXd& q, const MatrixXd& u, const int64_t* perm) {
  const int n = (int)q.rows();
  const int k = (int)u.cols();
  const Csc c = lower_csc(q);
  const auto f = stanli::sparse_ldlt(n, c.col_ptr.data(), c.row_idx.data(),
                                     c.values.data(), perm);
  const auto r = stanli::sparse_lowrank(f, k, u.data());
  MatrixXd cc = MatrixXd::Zero(n, n);
  for (int i = 0; i < n; ++i) {
    cc(i, i) = r.c[i];
    for (int j = 0; j < i; ++j)
      for (int a = 0; a < k; ++a) cc(i, j) += r.w[i * k + a] * r.b[j * k + a];
  }
  MatrixXd w(n, k);
  for (int i = 0; i < n; ++i)
    for (int a = 0; a < k; ++a) w(i, a) = r.w[i * k + a];
  const VectorXd d = Eigen::Map<const VectorXd>(f.d.data(), n);
  const MatrixXd t = MatrixXd(d.asDiagonal()) + w * w.transpose();
  expect_true("C C' = D + W W'",
              max_abs(cc * cc.transpose() - t) <= 1e-10 * max_abs(t));
  const MatrixXd l = factor_l(f);
  const MatrixXd implied = l * t * l.transpose();
  expect_true("L (D + W W') L' = P (Q + U U') P'",
              max_abs(implied - permuted(q + u * u.transpose(), f.perm)) <=
                  1e-10 * max_abs(q));
}

void test_lowrank_implies_the_corrected_precision() {
  const MatrixXd q = banded(24, 4, {5, 13});
  Eigen::SelfAdjointEigenSolver<MatrixXd> eig(q);
  const VectorXd w = eig.eigenvalues();
  std::vector<int> bad;
  for (int i = 0; i < 24; ++i)
    if (w[i] < 1e-8 * w.maxCoeff()) bad.push_back(i);
  MatrixXd u(24, (int)bad.size());
  for (size_t a = 0; a < bad.size(); ++a) {
    const double delta =
        std::max(std::fabs(w[bad[a]]), 1e-8 * w.cwiseAbs().maxCoeff()) -
        w[bad[a]];
    u.col(a) = eig.eigenvectors().col(bad[a]) * std::sqrt(delta);
  }
  expect_true("two corrections", bad.size() == 2);
  std::vector<int64_t> reversed(24);
  for (int i = 0; i < 24; ++i) reversed[i] = 23 - i;
  check_lowrank(q, u, nullptr);
  check_lowrank(q, u, reversed.data());
  check_lowrank(banded(16, 5, {}), 0.7 * random_point(16, 5, 1), nullptr);

  const MatrixXd q3 = banded(12, 6, {3});
  const Csc c = lower_csc(q3);
  const auto f = stanli::sparse_ldlt(12, c.col_ptr.data(), c.row_idx.data(),
                                     c.values.data(), nullptr);
  const VectorXd small = VectorXd::Constant(12, 0.01);
  bool refused = false;
  try {
    stanli::sparse_lowrank(f, 1, small.data());
  } catch (const std::runtime_error& e) {
    refused =
        std::string(e.what()).find("positive definite") != std::string::npos;
  }
  expect_true("a correction that leaves it indefinite is refused", refused);
}

// Each width makes a size product overflow int64_t: n * k for the first,
// only k * k for the second. Both must be refused before anything is
// allocated or read from u.
void test_lowrank_width_overflow_is_refused() {
  const auto refused = [](int n, int64_t k, const char* label) {
    const Csc c = lower_csc(banded(n, 2, {}));
    const auto f = stanli::sparse_ldlt(n, c.col_ptr.data(), c.row_idx.data(),
                                       c.values.data(), nullptr);
    const std::vector<double> u((size_t)n, 0.5);
    try {
      stanli::sparse_lowrank(f, k, u.data());
    } catch (const std::invalid_argument& e) {
      expect_true(std::string(label) + " names the width: " + e.what(),
                  std::string(e.what()).find("too large") != std::string::npos);
      return;
    }
    expect_true(std::string(label) + " is refused", false);
  };
  refused(4, int64_t{1} << 62, "a width whose n * k overflows");
  refused(1, int64_t{1} << 32, "a width whose k * k overflows");
}

// ---- the whole builder -------------------------------------------------

template <typename F>
stanli::laplace::Result build(const Model<F>& m, const VectorXd& start,
                              const std::vector<int>& latent,
                              stanli::laplace::Options o, int64_t* grads) {
  Gradient grad(m.gradient());
  o.keep_precision = true;
  auto r = stanli::laplace::whiten(grad, m.hessian(), start, latent, o);
  if (grads) *grads = grad.count;
  return r;
}

MatrixXd result_l(const stanli::laplace::Result& r) {
  MatrixXd l = MatrixXd::Zero(r.n, r.n);
  for (int64_t j = 0; j < r.n; ++j) {
    expect_true("each column starts at its diagonal",
                r.row_idx[r.col_ptr[j]] == j);
    for (int64_t p = r.col_ptr[j]; p < r.col_ptr[j + 1]; ++p)
      l(r.row_idx[p], j) = r.values[p];
  }
  return l;
}

// The precision the whitening implies: P' L (C C') L' P.
MatrixXd implied_precision(const stanli::laplace::Result& r) {
  MatrixXd l = result_l(r);
  MatrixXd inner = MatrixXd::Identity(r.n, r.n);
  if (r.rank > 0) {
    MatrixXd c = MatrixXd::Zero(r.n, r.n);
    for (int i = 0; i < r.n; ++i) {
      c(i, i) = r.c[i];
      for (int j = 0; j < i; ++j)
        for (int a = 0; a < r.rank; ++a)
          c(i, j) += r.w[i * r.rank + a] * r.b[j * r.rank + a];
    }
    inner = c * c.transpose();
  }
  const MatrixXd p_space = l * inner * l.transpose();
  MatrixXd out(r.n, r.n);
  for (int i = 0; i < r.n; ++i)
    for (int j = 0; j < r.n; ++j) out(r.perm[i], r.perm[j]) = p_space(i, j);
  return out;
}

void test_gaussian_marginal_mode_and_precision() {
  const int J = 20, n = J + 2;
  MatrixXd a = MatrixXd::Zero(n, n);
  a(0, 0) = 3;
  a(1, 1) = 4;
  for (int j = 0; j < J; ++j) {
    a(2 + j, 2 + j) = 2.5;
    a(0, 2 + j) = a(2 + j, 0) = -0.3;
    a(1, 2 + j) = a(2 + j, 1) = 0.2;
    if (j + 1 < J) a(2 + j, 3 + j) = a(3 + j, 2 + j) = -0.6;
  }
  VectorXd b(n);
  for (int i = 0; i < n; ++i) b[i] = std::sin(1.0 + i);
  const auto m = model(Gaussian{a, b}, n);
  const VectorXd mode = a.ldlt().solve(b);
  for (bool dense_hessian : {false, true}) {
    stanli::laplace::Options o;
    o.dense_hessian = dense_hessian;
    const auto r = build(m, VectorXd::Zero(n), range(2, n), o, nullptr);
    const std::string what = dense_hessian ? "dense: " : "coloured: ";
    std::printf("  %sq_hat gap %.2e, theta gap %.2e\n", what.c_str(),
                (r.q_hat - mode).cwiseAbs().maxCoeff(),
                (r.theta_hat - mode.head(2)).cwiseAbs().maxCoeff());
    // The outer gradient is a forward difference with step 1e-5, which
    // moves the optimum of a quadratic by about half the step.
    expect_true(what + "q_hat is the joint mode",
                (r.q_hat - mode).cwiseAbs().maxCoeff() <= 2e-5);
    expect_true(what + "theta_hat is the joint mode's theta",
                (r.theta_hat - mode.head(2)).cwiseAbs().maxCoeff() <= 2e-5);
    expect_true(what + "precision is A",
                max_abs(r.precision - a) <= 1e-6 * max_abs(a));
    expect_true(what + "the factor reproduces it",
                max_abs(implied_precision(r) - a) <= 1e-6 * max_abs(a));
    expect_true(what + "no repair", r.n_clipped == 0 && r.rank == 0 &&
                                        !r.has_spectrum && r.warnings.empty());
  }
}

void test_hierarchical_marginal_mode_is_a_maximum() {
  const auto h = hierarchical(12, 10, 5);
  const auto m = model(h, 15);
  const auto iu = range(2, 14);
  const std::vector<int> it = {0, 1, 14};
  stanli::laplace::Options o;
  const auto r = build(m, VectorXd::Zero(15), iu, o, nullptr);
  // The marginal at theta, by Newton steps on u with the reference Hessian.
  const auto marginal = [&](const VectorXd& theta) {
    VectorXd q = r.q_hat;
    for (int k = 0; k < 3; ++k) q[it[k]] = theta[k];
    MatrixXd hb;
    for (int step = 0; step < 50; ++step) {
      const MatrixXd hess = m.reference_hessian(q);
      VectorXd g;
      double lp;
      stan::math::gradient(h, q, lp, g);
      hb = MatrixXd(12, 12);
      VectorXd gb(12);
      for (int a = 0; a < 12; ++a) {
        gb[a] = g[iu[a]];
        for (int b2 = 0; b2 < 12; ++b2) hb(a, b2) = hess(iu[a], iu[b2]);
      }
      const VectorXd du = hb.ldlt().solve(-gb);
      for (int a = 0; a < 12; ++a) q[iu[a]] += du[a];
      if (du.cwiseAbs().maxCoeff() < 1e-13) break;
    }
    const MatrixXd hess = m.reference_hessian(q);
    for (int a = 0; a < 12; ++a)
      for (int b2 = 0; b2 < 12; ++b2) hb(a, b2) = -hess(iu[a], iu[b2]);
    return h(q) - 0.5 * std::log(hb.determinant());
  };
  // Newton's method on the reference marginal, with central differences.
  VectorXd ref = r.theta_hat;
  const double h_step = 1e-3;
  for (int iter = 0; iter < 30; ++iter) {
    VectorXd g(3);
    MatrixXd hm(3, 3);
    const double f0 = marginal(ref);
    for (int k = 0; k < 3; ++k) {
      VectorXd ek = VectorXd::Zero(3);
      ek[k] = h_step;
      const double up = marginal(ref + ek), down = marginal(ref - ek);
      g[k] = (up - down) / (2 * h_step);
      hm(k, k) = (up - 2 * f0 + down) / (h_step * h_step);
      for (int l = 0; l < k; ++l) {
        VectorXd el = VectorXd::Zero(3);
        el[l] = h_step;
        hm(k, l) = hm(l, k) =
            (marginal(ref + ek + el) - marginal(ref + ek - el) -
             marginal(ref - ek + el) + marginal(ref - ek - el)) /
            (4 * h_step * h_step);
      }
    }
    const VectorXd step = hm.ldlt().solve(-g);
    ref += step;
    if (step.cwiseAbs().maxCoeff() < 1e-9) break;
  }
  const double gap = (r.theta_hat - ref).cwiseAbs().maxCoeff();
  const double excess = marginal(ref) - marginal(r.theta_hat);
  std::printf("  theta_hat max gap %.2e, objective excess %.2e\n", gap, excess);
  expect_true("theta_hat within 2e-3 of the reference marginal mode",
              gap <= 2e-3);
  expect_true("objective within 1e-5 of the reference maximum", excess <= 1e-5);
  expect_true("precision is positive definite and reproduced",
              max_abs(implied_precision(r) - r.precision) <=
                  1e-9 * max_abs(r.precision));
}

void test_probe_and_dense_patterns_give_the_same_whitening() {
  const auto h = hierarchical(30, 10, 6);
  const auto m = model(h, 33);
  stanli::laplace::Options probe, dense;
  dense.dense_pattern = true;
  const auto a = build(m, VectorXd::Zero(33), range(2, 32), probe, nullptr);
  const auto b = build(m, VectorXd::Zero(33), range(2, 32), dense, nullptr);
  expect_true("same q_hat", (a.q_hat - b.q_hat).cwiseAbs().maxCoeff() <= 1e-8);
  expect_true("same factor size", a.values.size() == b.values.size());
  expect_true("the probe is cheaper",
              a.pattern_gradients < b.pattern_gradients);
  expect_true("the dense pattern costs 1 + 2n",
              b.pattern_gradients == 1 + 2 * 33);
  expect_true("probe pattern did not fall back", !a.pattern_fallback);

  stanli::laplace::Options both = dense;
  both.dense_hessian = true;
  int64_t used = 0;
  const auto c = build(m, VectorXd::Zero(33), range(2, 32), both, &used);
  expect_true("dense final Hessian costs 1 + 2n",
              c.final_gradients == 1 + 2 * 33 && c.n_colours == 33);
  expect_true("the coloured final Hessian is cheaper",
              a.final_gradients < 1 + 2 * 33);
  expect_true(
      "dense and coloured precisions agree",
      max_abs(a.precision - c.precision) <= 1e-6 * max_abs(c.precision));
  expect_true("marginal gradients counted", c.marginal_gradients > 0);
  expect_true("theta has the hyperparameters",
              c.theta_hat.size() == 3 && c.theta_start.size() == 3);
  expect_true("outer evaluations counted", c.outer_evaluations > 0);
}

// A gradient whose first call (the probe's gradient at the start) is not
// finite makes the probe give up; the builder then warns and takes the dense
// pattern, at 1 + 2n more gradients.
void test_failed_probe_falls_back_to_the_dense_pattern() {
  const auto h = hierarchical(14, 10, 5);
  const auto m = model(h, 17);
  const auto base = m.gradient();
  bool first = true;
  Gradient grad([&](const VectorXd& q, VectorXd& g) {
    const double lp = base(q, g);
    if (first) g[3] = std::numeric_limits<double>::quiet_NaN();
    first = false;
    return lp;
  });
  stanli::laplace::Options o;
  const auto r = stanli::laplace::whiten(grad, m.hessian(), VectorXd::Zero(17),
                                         range(2, 16), o);
  stanli::laplace::Options dense = o;
  dense.dense_pattern = true;
  const auto d = build(m, VectorXd::Zero(17), range(2, 16), dense, nullptr);
  expect_true("the failed probe is reported", r.pattern_fallback);
  bool warned = false;
  for (const auto& w : r.warnings)
    warned = warned || w.find("dense Hessian pattern") != std::string::npos;
  expect_true("and warned about", warned);
  expect_true("the probe's one gradient plus the dense Hessian's",
              r.pattern_gradients == 1 + (1 + 2 * 17));
  expect_true("the fallback gives the dense pattern's result",
              r.q_hat == d.q_hat && r.values == d.values);
}

void test_saddle_repairs() {
  const auto m = model(Saddles{1}, 2);
  stanli::laplace::Options abs_o, dense_o, clip_o;
  dense_o.repair = stanli::laplace::Repair::abs_dense;
  clip_o.repair = stanli::laplace::Repair::clip;
  clip_o.clip = 1e-3;
  const auto a = build(m, VectorXd::Zero(2), {1}, abs_o, nullptr);
  const auto d = build(m, VectorXd::Zero(2), {1}, dense_o, nullptr);
  const auto c = build(m, VectorXd::Zero(2), {1}, clip_o, nullptr);
  expect_true("abs repairs one", a.n_clipped == 1 && d.n_clipped == 1);
  expect_true("abs and abs_dense agree",
              max_abs(a.precision - d.precision) <= 1e-8);
  Eigen::SelfAdjointEigenSolver<MatrixXd> ea(a.precision), ec(c.precision);
  expect_true("abs keeps the scale of the negative direction",
              std::fabs(ea.eigenvalues()[0] - std::fabs(a.min_eigenvalue)) <=
                  1e-9 * ea.eigenvalues().maxCoeff());
  expect_true(
      "clip raises to the floor",
      ec.eigenvalues()[0] >= 1e-3 * ec.eigenvalues().maxCoeff() * (1 - 1e-9));
  expect_true("the abs factor implies the repaired precision",
              max_abs(implied_precision(a) - a.precision) <= 1e-9);
  expect_true("the clip factor implies the repaired precision",
              max_abs(implied_precision(c) - c.precision) <= 1e-9);
  const auto mentions = [](const stanli::laplace::Result& r, const char* s) {
    for (const auto& w : r.warnings)
      if (w.find(s) != std::string::npos) return true;
    return false;
  };
  expect_true("abs warns", mentions(a, "replaced by absolute values"));
  expect_true("clip warns", mentions(c, "clipped"));
}

// Fourteen saddles take the dense eigensolver (n <= 64); three saddles among
// 83 coordinates take the Lanczos search.
void test_many_saddles_lanczos_repair_matches_dense() {
  for (const Saddles& sad : {Saddles{14}, Saddles{3, 80, 0.1, 0.2}}) {
    const int n = sad.J + sad.K;
    const auto m = model(sad, n);
    stanli::laplace::Options abs_o, dense_o;
    dense_o.repair = stanli::laplace::Repair::abs_dense;
    const auto a = build(m, VectorXd::Zero(n), range(sad.J, n), abs_o, nullptr);
    const auto d =
        build(m, VectorXd::Zero(n), range(sad.J, n), dense_o, nullptr);
    const std::string what = "n=" + std::to_string(n) + ": ";
    expect_true(what + "every saddle is repaired",
                a.n_clipped == sad.J && d.n_clipped == sad.J);
    expect_true(
        what + "the low-rank repair matches the dense one",
        max_abs(a.precision - d.precision) <= 1e-6 * max_abs(d.precision));
    expect_true(what + "same smallest eigenvalue",
                std::fabs(a.min_eigenvalue - d.min_eigenvalue) <= 1e-6);
    expect_true(what + "the factor implies the repaired precision",
                max_abs(implied_precision(a) - a.precision) <=
                    1e-8 * max_abs(a.precision));
    expect_true(what + "low rank carries the repair", a.rank == sad.J);
  }
}

void test_all_latent_has_no_hyperparameters() {
  const auto m = model(Saddles{1}, 2);
  stanli::laplace::Options o;
  const auto r = build(m, VectorXd::Zero(2), {0, 1}, o, nullptr);
  expect_true("no theta", r.theta_hat.size() == 0 && r.outer_iterations == 0);
  expect_true("n", r.n == 2);
}

void test_bad_arguments_are_refused() {
  const auto m = model(Saddles{1}, 2);
  stanli::laplace::Options o;
  const auto refused = [&](const std::vector<int>& latent, const char* match) {
    try {
      build(m, VectorXd::Zero(2), latent, o, nullptr);
    } catch (const std::invalid_argument& e) {
      expect_true(std::string("message mentions ") + match + ": " + e.what(),
                  std::string(e.what()).find(match) != std::string::npos);
      return;
    }
    expect_true(std::string("refused: ") + match, false);
  };
  refused({}, "latent");
  refused({2}, "latent");
  refused({1, 0}, "latent");
}

}  // namespace

int main() {
  test_colouring_is_valid_and_small_for_a_band();
  test_sparse_hessian_matches_the_exact_one();
  test_probe_finds_the_exact_pattern();
  test_probe_gradients_do_not_grow_with_groups();
  test_verification_fails_on_lossy_candidates();
  test_verification_catches_one_missed_entry();
  test_probe_agrees_with_exact_and_not_with_diagonal();
  test_bad_eigenpairs_with_a_wide_spectrum();
  test_bad_eigenpairs_finds_one_negative_among_many();
  test_ldlt_reproduces_the_matrix();
  test_ldlt_refusals();
  test_lowrank_implies_the_corrected_precision();
  test_lowrank_width_overflow_is_refused();
  test_gaussian_marginal_mode_and_precision();
  test_hierarchical_marginal_mode_is_a_maximum();
  test_probe_and_dense_patterns_give_the_same_whitening();
  test_failed_probe_falls_back_to_the_dense_pattern();
  test_saddle_repairs();
  test_many_saddles_lanczos_repair_matches_dense();
  test_all_latent_has_no_hyperparameters();
  test_bad_arguments_are_refused();
  return finish("test_laplace");
}
