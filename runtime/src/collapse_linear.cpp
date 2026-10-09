// See collapse_linear.hpp.
#include "collapse_linear.hpp"

#include <stanli/collapse.hpp>

#include <Eigen/Dense>

#include <cmath>

namespace stanli {

void Compensated::add_product(double x, double y) {
  const double p = x * y;
  add(p);
  add(std::fma(x, y, -p));
}

std::vector<double> grouped_statistics(const std::vector<double>& y,
                                       const std::vector<int>& group_of,
                                       size_t groups) {
  std::vector<double> stats((size_t)kGroupedStatColumns * groups, 0.0);
  double* count = stats.data();
  double* centre = count + groups;
  double* offset = centre + groups;
  double* squares = offset + groups;
  std::vector<Compensated> sum(groups);
  for (size_t i = 0; i < y.size(); ++i) {
    count[(size_t)group_of[i]] += 1.0;
    sum[(size_t)group_of[i]].add(y[i]);
  }
  for (size_t k = 0; k < groups; ++k) centre[k] = sum[k].value() / count[k];
  std::vector<Compensated> first(groups), second(groups);
  for (size_t i = 0; i < y.size(); ++i) {
    const size_t k = (size_t)group_of[i];
    // y - centre = s + e exactly.
    const double s = y[i] - centre[k];
    const double v = s - y[i];
    const double e = (y[i] - (s - v)) - (centre[k] + v);
    first[k].add(s);
    first[k].add(e);
    // (s + e)^2 = s^2 + 2 s e, dropping e^2.
    second[k].add_product(s, s);
    second[k].add(2.0 * s * e);
  }
  for (size_t k = 0; k < groups; ++k) {
    offset[k] = first[k].value();
    squares[k] = second[k].value();
  }
  return stats;
}

std::vector<double> linear_gaussian_data(
    int64_t p, const std::vector<LinearRow>& rows,
    const std::vector<double>& statistics) {
  const int64_t groups = (int64_t)rows.size();
  if (groups < p || p < 1) return {};
  const double* count = statistics.data();
  const double* centre = count + groups;
  const double* offset = centre + groups;
  const double* squares = offset + groups;

  // Weighted least squares: each group is one row scaled by sqrt(count).
  Eigen::MatrixXd a = Eigen::MatrixXd::Zero(groups, p);
  Eigen::VectorXd rhs(groups);
  for (int64_t g = 0; g < groups; ++g) {
    const double w = std::sqrt(count[g]);
    for (const auto& term : rows[(size_t)g].terms)
      a(g, term.first) += w * term.second;
    rhs[g] = w * (centre[g] - rows[(size_t)g].constant);
  }
  // Any centre makes the expansion exact; the minimum-norm solution is a
  // good one whatever the rank.
  const Eigen::VectorXd t = a.completeOrthogonalDecomposition().solve(rhs);
  if (!t.allFinite()) return {};
  // |A u| = |R u| for the triangular factor of any QR, full rank or not.
  const Eigen::HouseholderQR<Eigen::MatrixXd> qr(a);
  const Eigen::MatrixXd& factored = qr.matrixQR();

  // What is left at the centre, from the rows themselves: the residuals
  // decide the accuracy near the mode, so they are summed with care.
  Compensated base;
  std::vector<Compensated> linear((size_t)p);
  double total = 0;
  for (int64_t g = 0; g < groups; ++g) {
    const LinearRow& row = rows[(size_t)g];
    Compensated residual;
    residual.add(centre[g]);
    residual.add(-row.constant);
    for (const auto& term : row.terms)
      residual.add_product(-term.second, t[term.first]);
    const double r = residual.value();
    total += count[g];
    base.add(squares[g]);
    base.add_product(count[g] * r, r);
    base.add_product(2.0 * offset[g], r);
    for (const auto& term : row.terms) {
      linear[(size_t)term.first].add_product(count[g] * term.second, r);
      linear[(size_t)term.first].add_product(offset[g], term.second);
    }
  }

  std::vector<double> data;
  data.reserve((size_t)linear_gaussian_data_len(p));
  data.push_back(base.value());
  data.push_back(total);
  for (int64_t j = 0; j < p; ++j) data.push_back(t[j]);
  for (int64_t j = 0; j < p; ++j) data.push_back(linear[(size_t)j].value());
  for (int64_t i = 0; i < p; ++i)
    for (int64_t j = i; j < p; ++j) data.push_back(factored(i, j));
  return data;
}

}  // namespace stanli
