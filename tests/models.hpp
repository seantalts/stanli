// Hand-built model graphs shared by the parity and sampling tests: the
// structure lower.cpp emits from the MIR, fixed here so executor and
// sampler tests do not depend on the lowering.
#ifndef STANLI_TESTS_MODELS_HPP
#define STANLI_TESTS_MODELS_HPP

#include <stanli/graph.hpp>
#include <stanli/optable.hpp>

#include <vector>

namespace stanli {
namespace testmodels {

// Non-centered eight schools.
//   params (unconstrained): mu, log_tau, theta_tilde[8]
//   tau = exp(log_tau)
//   theta = mu + tau * theta_tilde
//   lp = normal(y | theta, sigma) + normal(theta_tilde | 0, 1)
//      + normal(mu | 0, 5) + cauchy(tau | 0, 5) + log_tau   (jacobian)
struct EightSchools {
  Graph graph;
  int mu, log_tau, theta_tilde;  // parameter slots
  int y, sigma;                  // data slots
  int zero, one, five;           // constant slots
  static constexpr int J = 8;
  static constexpr double kY[J] = {28, 8, -3, 7, -1, 1, 18, 12};
  static constexpr double kSigma[J] = {15, 10, 16, 11, 9, 11, 10, 18};
};

inline EightSchools eight_schools() {
  EightSchools m;
  Graph& g = m.graph;
  m.mu = g.add_slot(1, true);
  m.log_tau = g.add_slot(1, true);
  m.theta_tilde = g.add_slot(EightSchools::J, true);
  m.y = g.add_slot(EightSchools::J, false);
  m.sigma = g.add_slot(EightSchools::J, false);
  const int zero = m.zero = g.add_slot(1, false);
  const int one = m.one = g.add_slot(1, false);
  const int five = m.five = g.add_slot(1, false);
  const int tau = g.add_slot(1, false);
  const int theta = g.add_slot(EightSchools::J, false);
  const int lp1 = g.add_slot(1, false);
  const int lp2 = g.add_slot(1, false);
  const int lp3 = g.add_slot(1, false);
  const int lp4 = g.add_slot(1, false);
  const int lp = g.add_slot(1, false);

  g.add_op(OP_EXP, {m.log_tau}, tau);
  g.add_op(OP_BCAST_FMA, {m.mu, tau, m.theta_tilde}, theta);
  g.add_op(OP_NORMAL_LPDF, {m.y, theta, m.sigma}, lp1);
  g.add_op(OP_NORMAL_LPDF, {m.theta_tilde, zero, one}, lp2);
  g.add_op(OP_NORMAL_LPDF, {m.mu, zero, five}, lp3);
  g.add_op(OP_CAUCHY_LPDF, {tau, zero, five}, lp4);
  g.add_op(OP_ADD_N, {lp1, lp2, lp3, lp4, m.log_tau}, lp);
  g.result_slot = lp;
  return m;
}

// Fills data + constant slots. Call once after constructing the Executor.
inline void fill_eight_schools_data(const EightSchools& m, Executor& ex) {
  for (int i = 0; i < EightSchools::J; ++i) {
    ex.value_ptr(m.y)[i] = EightSchools::kY[i];
    ex.value_ptr(m.sigma)[i] = EightSchools::kSigma[i];
  }
  ex.value_ptr(m.zero)[0] = 0.0;
  ex.value_ptr(m.one)[0] = 1.0;
  ex.value_ptr(m.five)[0] = 5.0;
}

// Logistic GLM with fixed simulated data.
//   params: alpha, beta[3]
//   eta = X * beta + alpha
//   lp = bernoulli_logit(y | eta) + normal(beta | 0, 2.5) + normal(alpha|0,5)
struct LogisticGlm {
  Graph graph;
  int alpha, beta;
  int X;
  int zero, p25, five, one;  // constant slots
  static constexpr int N = 20, K = 3;
  // Deterministic fixed data, column-major (Stan/Eigen convention).
  static const double kX[N * K];
  static const int kYint[N];
};

inline const double LogisticGlm::kX[LogisticGlm::N * LogisticGlm::K] = {
    0.17,  -1.30, 0.55,  1.02,  -0.21, -0.88, 0.34,  0.91,  -1.44, 0.62,
    -0.53, 1.21,  -0.09, 0.75,  -0.66, 1.35,  -1.02, 0.28,  0.44,  -0.37,
    0.83,  -1.19, 0.06,  0.97,  -0.74, 0.51,  1.28,  -0.42, -0.15, 0.68,
    -0.95, 0.23,  1.07,  -0.58, 0.39,  -1.26, 0.71,  0.12,  -0.81, 1.14,
    -0.33, 0.86,  0.49,  -1.08, 0.25,  0.93,  -0.61, 0.18,  1.31,  -0.47,
    0.04,  0.78,  -1.15, 0.36,  0.59,  -0.92, 1.24,  -0.28, 0.65,  -0.11};
inline const int LogisticGlm::kYint[LogisticGlm::N] = {
    1, 0, 1, 1, 0, 1, 0, 1, 0, 1, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0};

inline LogisticGlm logistic_glm() {
  LogisticGlm m;
  Graph& g = m.graph;
  m.alpha = g.add_slot(1, true);
  m.beta = g.add_slot(LogisticGlm::K, true);
  m.X = g.add_slot(LogisticGlm::N * LogisticGlm::K, false);
  const int zero = m.zero = g.add_slot(1, false);
  const int p25 = m.p25 = g.add_slot(1, false);
  const int five = m.five = g.add_slot(1, false);
  const int one = m.one = g.add_slot(1, false);
  const int eta0 = g.add_slot(LogisticGlm::N, false);
  const int eta = g.add_slot(LogisticGlm::N, false);
  const int lp1 = g.add_slot(1, false);
  const int lp2 = g.add_slot(1, false);
  const int lp3 = g.add_slot(1, false);
  const int lp = g.add_slot(1, false);

  g.add_op(OP_MATVEC, {m.X, m.beta}, eta0, {LogisticGlm::N, LogisticGlm::K});
  g.add_op(OP_BCAST_FMA, {m.alpha, one, eta0}, eta);
  g.add_op(OP_BERNOULLI_LOGIT_LPMF, {eta}, lp1,
           std::vector<int>(LogisticGlm::kYint,
                            LogisticGlm::kYint + LogisticGlm::N));
  g.add_op(OP_NORMAL_LPDF, {m.beta, zero, p25}, lp2);
  g.add_op(OP_NORMAL_LPDF, {m.alpha, zero, five}, lp3);
  g.add_op(OP_ADD_N, {lp1, lp2, lp3}, lp);
  g.result_slot = lp;
  return m;
}

inline void fill_logistic_glm_data(const LogisticGlm& m, Executor& ex) {
  for (int i = 0; i < LogisticGlm::N * LogisticGlm::K; ++i)
    ex.value_ptr(m.X)[i] = LogisticGlm::kX[i];
  ex.value_ptr(m.zero)[0] = 0.0;
  ex.value_ptr(m.p25)[0] = 2.5;
  ex.value_ptr(m.five)[0] = 5.0;
  ex.value_ptr(m.one)[0] = 1.0;
}

}  // namespace testmodels
}  // namespace stanli

#endif
