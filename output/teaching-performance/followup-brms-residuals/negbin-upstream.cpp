// Diagnostic only: call upstream Stan Math without any Stanli
// headers/libraries.
#include <stan/math/prim.hpp>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <vector>
int main() {
  const int n = 40, y = 3;
  Eigen::MatrixXd x = Eigen::MatrixXd::Zero(n, 1);
  Eigen::VectorXd beta = Eigen::VectorXd::Zero(1);
  const double eta = std::log(3.0);
  const std::vector<int> ys(n, y);
  std::cout << std::setprecision(17)
            << "phi,upstream_glm,upstream_non_glm,integer_identity\n";
  for (double phi : {2.0, 1e4, 1e8, 1e12, 1e16, 1e22, 1e100}) {
    // For integer y, expand Gamma(phi+y)/Gamma(phi) as y factors.
    // Cancel powers of phi symbolically before floating-point evaluation.
    long double p = phi;
    long double lp = y * static_cast<long double>(eta) -
                     std::lgamma(static_cast<long double>(y + 1));
    for (int k = 0; k < y; ++k) lp += std::log1p(k / p);
    lp -= (p + y) * std::log1p(std::exp(static_cast<long double>(eta)) / p);
    std::cout << phi << ','
              << stan::math::neg_binomial_2_log_glm_lpmf(ys, x, eta, beta, phi)
              << ',' << stan::math::neg_binomial_2_log_lpmf(ys, eta, phi) << ','
              << n * lp << '\n';
  }
}
