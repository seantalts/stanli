// Logistic GLM: graph log_prob + gradient vs an all-var reference in the
// same op order, at three fixed parameter vectors. Bitwise.
#include "models.hpp"

#include <stan/math.hpp>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

static int failures = 0;
static void expect_eq(const std::string& what, double got, double want) {
  if (got != want && !(std::isnan(got) && std::isnan(want))) {
    ++failures;
    std::printf("FAIL %-16s got %.17g want %.17g\n", what.c_str(), got, want);
  }
}

// The three direct GLM kernels do not pass through the generated scalar
// density wrappers. Empty outcomes return zero before Stan Math builds a
// propagator, so each must still preserve the returned value and disconnected
// topology through its own recorder call.
static void check_empty_glm(uint16_t opcode, const std::string& name) {
  using namespace stanli;
  using stan::math::var;
  constexpr int K = 2;

  Graph g;
  const int X_slot = g.add_slot(0, false);
  const int alpha_slot = g.add_slot(1, true);
  const int beta_slot = g.add_slot(K, true);
  const bool has_phi = opcode == OP_NEG_BINOMIAL_2_LOG_GLM_LPMF;
  const int phi_slot = has_phi ? g.add_slot(1, true) : -1;
  const int infinity_slot = g.add_slot(1, false);
  const int lp_slot = g.add_slot(1, false);
  const int scaled_slot = g.add_slot(1, false);
  if (has_phi) {
    g.add_op(opcode, {X_slot, alpha_slot, beta_slot, phi_slot}, lp_slot,
             {0, K});
  } else {
    g.add_op(opcode, {X_slot, alpha_slot, beta_slot}, lp_slot, {0, K});
  }
  g.add_op(OP_MUL, {lp_slot, infinity_slot}, scaled_slot);
  g.result_slot = scaled_slot;

  Executor ex(std::move(g));
  ex.value_ptr(infinity_slot)[0] = std::numeric_limits<double>::infinity();
  ex.params_data()[0] = 0.4;
  ex.params_data()[1] = -0.3;
  ex.params_data()[2] = 0.8;
  if (has_phi) ex.params_data()[3] = 1.7;
  std::vector<double> grad(has_phi ? 4 : 3, 0.0);
  const double value = ex.gradient(grad.data());

  const std::vector<int> y;
  const Eigen::Matrix<double, -1, -1> X(0, K);
  var alpha = 0.4;
  Eigen::Matrix<var, -1, 1> beta(K);
  beta << -0.3, 0.8;
  var phi = 1.7;
  var lp_ref;
  if (opcode == OP_BERNOULLI_LOGIT_GLM_LPMF) {
    lp_ref = stan::math::bernoulli_logit_glm_lpmf<false>(y, X, alpha, beta);
  } else if (opcode == OP_POISSON_LOG_GLM_LPMF) {
    lp_ref = stan::math::poisson_log_glm_lpmf<false>(y, X, alpha, beta);
  } else {
    lp_ref =
        stan::math::neg_binomial_2_log_glm_lpmf<false>(y, X, alpha, beta, phi);
  }
  var value_ref = lp_ref * std::numeric_limits<double>::infinity();
  value_ref.grad();
  expect_eq(name + " empty value", value, value_ref.val());
  expect_eq(name + " empty alpha", grad[0], alpha.adj());
  for (int k = 0; k < K; ++k)
    expect_eq(name + " empty beta" + std::to_string(k), grad[1 + k],
              beta(k).adj());
  if (has_phi) expect_eq(name + " empty phi", grad[3], phi.adj());
  stan::math::recover_memory();
}

// The three GLMs that take the var tape rather than the recorder, each with
// a non-unit output adjoint: their kernels seed the tape with 1.0 in the
// forward and scale in the backward.
static void check_tail_glm(const std::string& tag, uint16_t opcode, bool propto,
                           const std::vector<int>& idata, int rows, int cols,
                           int alpha_len, int beta_len) {
  using namespace stanli;
  using stan::math::var;
  const double seed = -0.73;
  std::vector<double> X((size_t)rows * cols), a((size_t)alpha_len),
      b((size_t)beta_len);
  for (int j = 0; j < cols; ++j)
    for (int i = 0; i < rows; ++i)
      X[(size_t)j * rows + i] = std::sin(0.31 * i + 0.7 * j);
  for (int i = 0; i < alpha_len; ++i) a[(size_t)i] = 0.15 - 0.08 * i;
  for (int i = 0; i < beta_len; ++i) b[(size_t)i] = 0.2 + 0.11 * i;

  Graph g;
  const int Xs = g.add_slot(rows * cols, true);
  const int as = g.add_slot(alpha_len, true);
  const int bs = g.add_slot(beta_len, true);
  const int lp = g.add_slot(1, false);
  const int ss = g.add_slot(1, false);
  const int total = g.add_slot(1, false);
  const int op = g.add_op(opcode, {Xs, as, bs}, lp, idata);
  g.ops[(size_t)op].variant = propto ? 0x80u : 0x00u;
  g.add_op(OP_MUL, {lp, ss}, total);
  g.result_slot = total;

  Executor ex(std::move(g));
  std::copy(X.begin(), X.end(), ex.param_ptr(Xs));
  std::copy(a.begin(), a.end(), ex.param_ptr(as));
  std::copy(b.begin(), b.end(), ex.param_ptr(bs));
  ex.value_ptr(ss)[0] = seed;
  std::vector<double> grad(X.size() + a.size() + b.size(), 0.0);
  const double got = ex.gradient(grad.data());

  stan::math::nested_rev_autodiff nested;
  Eigen::Matrix<var, -1, -1> Xv(rows, cols);
  for (size_t i = 0; i < X.size(); ++i) Xv.data()[i] = X[i];
  Eigen::Matrix<var, -1, 1> av(alpha_len), bv(beta_len);
  for (int i = 0; i < alpha_len; ++i) av(i) = a[(size_t)i];
  for (int i = 0; i < beta_len; ++i) bv(i) = b[(size_t)i];
  var ref;
  if (opcode == OP_BINOMIAL_LOGIT_GLM_LPMF) {
    std::vector<int> nn(idata.begin(), idata.begin() + rows);
    std::vector<int> NN(idata.begin() + rows, idata.begin() + 2 * rows);
    ref =
        propto
            ? stan::math::binomial_logit_glm_lpmf<true>(nn, NN, Xv, av(0), bv)
            : stan::math::binomial_logit_glm_lpmf<false>(nn, NN, Xv, av(0), bv);
  } else if (opcode == OP_CATEGORICAL_LOGIT_GLM_LPMF) {
    std::vector<int> yy(idata.begin(), idata.begin() + rows);
    Eigen::Matrix<var, -1, -1> bm(cols, beta_len / cols);
    for (int i = 0; i < beta_len; ++i) bm.data()[i] = bv(i);
    ref = propto
              ? stan::math::categorical_logit_glm_lpmf<true>(yy, Xv, av, bm)
              : stan::math::categorical_logit_glm_lpmf<false>(yy, Xv, av, bm);
    var scaled_cat = ref * seed;
    stan::math::grad(scaled_cat.vi_);
    expect_eq(tag + " total", got, scaled_cat.val());
    size_t at = 0;
    for (size_t i = 0; i < X.size(); ++i)
      expect_eq(tag + " dX" + std::to_string(i), grad[at++],
                Xv.data()[i].adj());
    for (int i = 0; i < alpha_len; ++i)
      expect_eq(tag + " da" + std::to_string(i), grad[at++], av(i).adj());
    for (int i = 0; i < beta_len; ++i)
      expect_eq(tag + " db" + std::to_string(i), grad[at++],
                bm.data()[i].adj());
    return;
  } else {
    std::vector<int> yy(idata.begin(), idata.begin() + rows);
    ref = propto ? stan::math::ordered_logistic_glm_lpmf<true>(yy, Xv, av, bv)
                 : stan::math::ordered_logistic_glm_lpmf<false>(yy, Xv, av, bv);
  }
  var scaled = ref * seed;
  stan::math::grad(scaled.vi_);
  expect_eq(tag + " total", got, scaled.val());
  size_t at = 0;
  for (size_t i = 0; i < X.size(); ++i)
    expect_eq(tag + " dX" + std::to_string(i), grad[at++], Xv.data()[i].adj());
  for (int i = 0; i < alpha_len; ++i)
    expect_eq(tag + " da" + std::to_string(i), grad[at++], av(i).adj());
  for (int i = 0; i < beta_len; ++i)
    expect_eq(tag + " db" + std::to_string(i), grad[at++], bv(i).adj());
}

static void check_tail_glms() {
  using namespace stanli;
  const int rows = 5, cols = 3;
  std::vector<int> bin;
  for (int i = 0; i < rows; ++i) bin.push_back(1 + (i % 4));
  for (int i = 0; i < rows; ++i) bin.push_back(5 + i);
  bin.push_back(rows);
  bin.push_back(cols);
  check_tail_glm("binom glm", OP_BINOMIAL_LOGIT_GLM_LPMF, false, bin, rows,
                 cols, 1, cols);
  check_tail_glm("binom glm propto", OP_BINOMIAL_LOGIT_GLM_LPMF, true, bin,
                 rows, cols, 1, cols);

  const int cats = 3;
  std::vector<int> cat;
  for (int i = 0; i < rows; ++i) cat.push_back(1 + (i % cats));
  cat.push_back(rows);
  cat.push_back(cols);
  check_tail_glm("cat glm", OP_CATEGORICAL_LOGIT_GLM_LPMF, false, cat, rows,
                 cols, cats, cols * cats);
  check_tail_glm("cat glm propto", OP_CATEGORICAL_LOGIT_GLM_LPMF, true, cat,
                 rows, cols, cats, cols * cats);

  // in = {X, beta, cutpoints}: the cutpoints ride the `alpha` slot here and
  // must stay ordered.
  check_tail_glm("ord glm", OP_ORDERED_LOGISTIC_GLM_LPMF, false, cat, rows,
                 cols, cols, cats - 1);
  check_tail_glm("ord glm propto", OP_ORDERED_LOGISTIC_GLM_LPMF, true, cat,
                 rows, cols, cols, cats - 1);
}

static void reference(const double* q, double* lp_out, double* grad_out) {
  using stan::math::var;
  using stanli::testmodels::LogisticGlm;
  const int N = LogisticGlm::N, K = LogisticGlm::K;

  var alpha = q[0];
  Eigen::Matrix<var, -1, 1> beta(K);
  for (int i = 0; i < K; ++i) beta(i) = q[1 + i];
  var zero = 0.0, p25 = 2.5, five = 5.0;

  // eta in the same order as OP_MATVEC then OP_BCAST_FMA (b = 1.0).
  // kX is column-major (Stan/Eigen convention).
  Eigen::Matrix<var, -1, 1> eta(N);
  for (int r = 0; r < N; ++r) {
    var acc = 0.0;
    for (int c = 0; c < K; ++c) acc += LogisticGlm::kX[c * N + r] * beta(c);
    eta(r) = alpha + 1.0 * acc;
  }
  std::vector<int> y(LogisticGlm::kYint, LogisticGlm::kYint + N);
  var lp1 = stan::math::bernoulli_logit_lpmf<false>(y, eta);
  var lp2 = stan::math::normal_lpdf<false>(beta, zero, p25);
  var lp3 = stan::math::normal_lpdf<false>(alpha, zero, five);
  var lp = lp1 + lp2 + lp3;
  lp.grad();

  *lp_out = lp.val();
  grad_out[0] = alpha.adj();
  for (int i = 0; i < K; ++i) grad_out[1 + i] = beta(i).adj();
  stan::math::recover_memory();
}

int main() {
  using namespace stanli;
  auto m = testmodels::logistic_glm();
  Executor ex(std::move(m.graph));
  testmodels::fill_logistic_glm_data(m, ex);
  const int NP = 4;

  const double qs[3][NP] = {
      {0.2, 0.5, -0.8, 1.1}, {-1.0, 0.0, 0.3, -0.2}, {2.2, -1.5, 0.9, 0.4}};

  for (int c = 0; c < 3; ++c) {
    ex.param_ptr(m.alpha)[0] = qs[c][0];
    for (int i = 0; i < 3; ++i) ex.param_ptr(m.beta)[i] = qs[c][1 + i];

    double grad[NP], lp_ref, grad_ref[NP];
    const double lp = ex.gradient(grad);
    reference(qs[c], &lp_ref, grad_ref);

    const std::string tag = "case" + std::to_string(c);
    expect_eq(tag + " lp", lp, lp_ref);
    for (int i = 0; i < NP; ++i)
      expect_eq(tag + " g" + std::to_string(i), grad[i], grad_ref[i]);
  }

  check_tail_glms();

  check_empty_glm(OP_BERNOULLI_LOGIT_GLM_LPMF, "bernoulli_logit_glm");
  check_empty_glm(OP_POISSON_LOG_GLM_LPMF, "poisson_log_glm");
  check_empty_glm(OP_NEG_BINOMIAL_2_LOG_GLM_LPMF, "neg_binomial_2_log_glm");

  if (failures == 0) std::printf("test_glm OK\n");
  return failures == 0 ? 0 : 1;
}
