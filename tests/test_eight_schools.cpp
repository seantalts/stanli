// Eight schools (non-centered): graph log_prob + full 10-dim gradient vs a
// var reference coded with the same formulas in the same op order, at three
// fixed parameter vectors. Bitwise.
#include "models.hpp"

#include <stan/math.hpp>
#include <cstdio>
#include <string>

static int failures = 0;
static void expect_eq(const std::string& what, double got, double want) {
  if (got != want) {
    ++failures;
    std::printf("FAIL %-20s got %.17g want %.17g\n", what.c_str(), got, want);
  }
}

// Reference: same math, all-var (matching kernel all-rvar activity),
// operations in graph op order.
static void reference(const double* q, double* lp_out, double* grad_out) {
  using stan::math::var;
  using stanli::testmodels::EightSchools;
  const int J = EightSchools::J;

  var mu = q[0];
  var log_tau = q[1];
  Eigen::Matrix<var, -1, 1> theta_tilde(J);
  for (int i = 0; i < J; ++i) theta_tilde(i) = q[2 + i];

  var zero = 0.0, one = 1.0, five = 5.0;
  var tau = stan::math::exp(log_tau);
  // Built in descending order so the reverse sweep reaches the elements in
  // ascending order, as the OP_ADD and OP_MUL scalar-broadcast adjoints do.
  Eigen::Matrix<var, -1, 1> tau_theta_tilde(J), theta(J);
  for (int i = J - 1; i >= 0; --i) tau_theta_tilde(i) = tau * theta_tilde(i);
  for (int i = J - 1; i >= 0; --i) theta(i) = mu + tau_theta_tilde(i);

  // Promote data to var: the all-rvar kernels instantiate every argument as
  // an autodiff type, and matching that activity is what makes bitwise
  // parity achievable (mixed instantiations differ by ULPs via to_ref_if).
  Eigen::Matrix<var, -1, 1> y(J), sigma(J);
  for (int i = 0; i < J; ++i) {
    y(i) = EightSchools::kY[i];
    sigma(i) = EightSchools::kSigma[i];
  }
  var lp1 = stan::math::normal_lpdf<false>(y, theta, sigma);
  var lp2 = stan::math::normal_lpdf<false>(theta_tilde, zero, one);
  var lp3 = stan::math::normal_lpdf<false>(mu, zero, five);
  var lp4 = stan::math::cauchy_lpdf<false>(tau, zero, five);
  var lp = lp1 + lp2 + lp3 + lp4 + log_tau;
  lp.grad();

  *lp_out = lp.val();
  grad_out[0] = mu.adj();
  grad_out[1] = log_tau.adj();
  for (int i = 0; i < J; ++i) grad_out[2 + i] = theta_tilde(i).adj();
  stan::math::recover_memory();
}

int main() {
  using namespace stanli;
  auto m = testmodels::eight_schools();
  Executor ex(std::move(m.graph));
  testmodels::fill_eight_schools_data(m, ex);
  const int NP = 10;

  const double qs[3][NP] = {
      {4.0, 1.0, 0.1, -0.2, 0.3, -0.4, 0.5, -0.6, 0.7, -0.8},
      {0.0, -1.5, 1.2, 0.8, -1.1, 0.05, -0.3, 0.9, -1.4, 0.2},
      {-2.5, 0.3, -0.7, 1.5, 0.6, -0.9, 1.1, 0.4, -0.2, -1.3}};

  for (int c = 0; c < 3; ++c) {
    ex.param_ptr(m.mu)[0] = qs[c][0];
    ex.param_ptr(m.log_tau)[0] = qs[c][1];
    for (int i = 0; i < 8; ++i) ex.param_ptr(m.theta_tilde)[i] = qs[c][2 + i];

    double grad[NP], lp_ref, grad_ref[NP];
    const double lp = ex.gradient(grad);
    reference(qs[c], &lp_ref, grad_ref);

    const std::string tag = "case" + std::to_string(c);
    expect_eq(tag + " lp", lp, lp_ref);
    for (int i = 0; i < NP; ++i)
      expect_eq(tag + " g" + std::to_string(i), grad[i], grad_ref[i]);
  }

  if (failures == 0) std::printf("test_eight_schools OK\n");
  return failures == 0 ? 0 : 1;
}
