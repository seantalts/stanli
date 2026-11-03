// The inverse parameter transforms, checked the only way that keeps them
// honest: against the forward direction they must undo. Every case starts
// from free values, constrains them with stan-math's own *_constrain, and
// requires unconstrain_leaf to recover the free values it started from.
//
// A hand-written expected vector would only restate the implementation. A
// round trip cannot: it fails if either direction drifts, if the free length
// is wrong, or if a matrix leaf is read in the wrong storage order.
#include <stanli/unconstrain.hpp>

#include <stanli/compile.hpp>

#include <stan/math.hpp>

#include <Eigen/Dense>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
  if (!ok) {
    ++failures;
    std::printf("FAIL %s\n", what.c_str());
  }
}

void expect_near(const std::string& what, const std::vector<double>& got,
                 const std::vector<double>& want, double tol = 1e-12) {
  if (got.size() != want.size()) {
    ++failures;
    std::printf("FAIL %s: %zu values, want %zu\n", what.c_str(), got.size(),
                want.size());
    return;
  }
  for (size_t i = 0; i < got.size(); ++i) {
    if (!(std::abs(got[i] - want[i]) <= tol)) {
      ++failures;
      std::printf("FAIL %s[%zu]: got %.17g want %.17g\n", what.c_str(), i,
                  got[i], want[i]);
      return;
    }
  }
}

// A deterministic, uninteresting-but-not-degenerate free vector.
std::vector<double> free_values(int64_t n) {
  std::vector<double> y((size_t)n);
  for (int64_t i = 0; i < n; ++i)
    y[(size_t)i] =
        0.35 * std::sin(0.7 * (double)i + 0.4) - 0.15 * (double)(i % 3);
  return y;
}

Eigen::VectorXd as_eigen(const std::vector<double>& v) {
  return Eigen::Map<const Eigen::VectorXd>(v.data(), (Eigen::Index)v.size());
}

std::vector<double> as_vector(const Eigen::MatrixXd& m) {
  return std::vector<double>(m.data(), m.data() + m.size());
}

// Constrain `free` with stan-math, hand the result to unconstrain_leaf, and
// require the original back.
template <typename Constrain>
void round_trip(const std::string& what, stanli::mir::Transform::Kind kind,
                const std::vector<int64_t>& leaf_dims, int64_t n_free,
                Constrain constrain,
                const std::vector<double>* expected = nullptr) {
  const std::vector<double> y = free_values(n_free);
  const std::vector<double> constrained = constrain(as_eigen(y));

  int64_t expected_free = 0;
  try {
    expected_free = stanli::free_leaf_size(kind, leaf_dims);
  } catch (const std::exception& e) {
    ++failures;
    std::printf("FAIL %s: free_leaf_size threw: %s\n", what.c_str(), e.what());
    return;
  }
  check(expected_free == n_free, what + ": free length " +
                                     std::to_string(expected_free) + ", want " +
                                     std::to_string(n_free));

  std::vector<double> got((size_t)n_free, 0.0);
  try {
    stanli::unconstrain_leaf(kind, leaf_dims, constrained.data(), {},
                             got.data());
  } catch (const std::exception& e) {
    ++failures;
    std::printf("FAIL %s: unconstrain threw: %s\n", what.c_str(), e.what());
    return;
  }
  expect_near(what, got, expected ? *expected : y);
}

void structured_round_trips() {
  using K = stanli::mir::Transform;
  using stan::math::value_of;

  round_trip("simplex[5]", K::Simplex, {5}, 4, [](const Eigen::VectorXd& y) {
    return as_vector(stan::math::simplex_constrain(y));
  });
  round_trip("ordered[4]", K::Ordered, {4}, 4, [](const Eigen::VectorXd& y) {
    return as_vector(stan::math::ordered_constrain(y));
  });
  round_trip("positive_ordered[4]", K::PositiveOrdered, {4}, 4,
             [](const Eigen::VectorXd& y) {
               return as_vector(stan::math::positive_ordered_constrain(y));
             });
  // unit_vector is the one transform here that is not injective: constrain
  // normalizes, so the free value that comes back is the direction, not the
  // magnitude. Round-tripping to y itself would be wrong to expect.
  {
    const Eigen::VectorXd y = as_eigen(free_values(3));
    const Eigen::VectorXd direction = y / y.norm();
    const std::vector<double> want(direction.data(),
                                   direction.data() + direction.size());
    round_trip(
        "unit_vector[3]", K::UnitVector, {3}, 3,
        [](const Eigen::VectorXd& v) {
          return as_vector(stan::math::unit_vector_constrain(v));
        },
        &want);
  }
  round_trip("sum_to_zero_vector[5]", K::SumToZero, {5}, 4,
             [](const Eigen::VectorXd& y) {
               return as_vector(stan::math::sum_to_zero_constrain(y));
             });
  // The matrix form is where a column-major slip would show: its free values
  // are an (R-1) x (C-1) block, not a flat run.
  round_trip("sum_to_zero_matrix[4,3]", K::SumToZero, {4, 3}, 6,
             [](const Eigen::VectorXd& y) {
               Eigen::MatrixXd free_block =
                   Eigen::Map<const Eigen::MatrixXd>(y.data(), 3, 2);
               return as_vector(Eigen::MatrixXd(
                   stan::math::sum_to_zero_constrain(free_block)));
             });
  // Rectangular so that swapping the two orientations shows.
  round_trip("column_stochastic_matrix[4,3]", K::StochasticColumn, {4, 3}, 9,
             [](const Eigen::VectorXd& y) {
               Eigen::MatrixXd free_block =
                   Eigen::Map<const Eigen::MatrixXd>(y.data(), 3, 3);
               return as_vector(Eigen::MatrixXd(
                   stan::math::stochastic_column_constrain(free_block)));
             });
  round_trip("row_stochastic_matrix[3,4]", K::StochasticRow, {3, 4}, 9,
             [](const Eigen::VectorXd& y) {
               Eigen::MatrixXd free_block =
                   Eigen::Map<const Eigen::MatrixXd>(y.data(), 3, 3);
               return as_vector(Eigen::MatrixXd(
                   stan::math::stochastic_row_constrain(free_block)));
             });
  round_trip("cholesky_factor_corr[4]", K::CholeskyCorr, {4, 4}, 6,
             [](const Eigen::VectorXd& y) {
               double lp = 0;
               return as_vector(Eigen::MatrixXd(
                   stan::math::cholesky_corr_constrain(y, 4, lp)));
             });
  round_trip("corr_matrix[4]", K::Correlation, {4, 4}, 6,
             [](const Eigen::VectorXd& y) {
               double lp = 0;
               return as_vector(Eigen::MatrixXd(
                   stan::math::corr_matrix_constrain(y, 4, lp)));
             });
  round_trip("cov_matrix[3]", K::Covariance, {3, 3}, 6,
             [](const Eigen::VectorXd& y) {
               double lp = 0;
               return as_vector(
                   Eigen::MatrixXd(stan::math::cov_matrix_constrain(y, 3, lp)));
             });
  // Rectangular on purpose: M > N is the case whose free length is not a
  // triangular number.
  round_trip("cholesky_factor_cov[4,3]", K::CholeskyCov, {4, 3}, 9,
             [](const Eigen::VectorXd& y) {
               double lp = 0;
               return as_vector(Eigen::MatrixXd(
                   stan::math::cholesky_factor_constrain(y, 4, 3, lp)));
             });
}

void elementwise_round_trips() {
  using K = stanli::mir::Transform;
  const std::vector<double> y = free_values(4);

  struct Case {
    std::string what;
    K::Kind kind;
    std::vector<std::vector<double>> args;
  };
  const std::vector<Case> cases = {
      {"identity", K::Identity, {}},
      {"lower scalar", K::Lower, {{-1.5}}},
      {"upper scalar", K::Upper, {{2.5}}},
      {"lower_upper scalar", K::LowerUpper, {{-1.0}, {3.0}}},
      {"offset scalar", K::Offset, {{2.0}}},
      {"multiplier scalar", K::Multiplier, {{3.0}}},
      {"offset_multiplier scalar", K::OffsetMultiplier, {{2.0}, {0.5}}},
      // Container bounds, one per element, which is what a data-valued bound
      // like vector<lower=lo>[N] produces.
      {"lower per element", K::Lower, {{-1.0, -2.0, 0.0, 0.5}}},
      {"lower_upper per element",
       K::LowerUpper,
       {{-1.0, -2.0, 0.0, 0.5}, {1.0, 2.0, 3.0, 4.0}}},
  };

  for (const Case& c : cases) {
    std::vector<stanli::TransformArg> args;
    for (const auto& a : c.args)
      args.push_back(stanli::TransformArg{a.data(), (int64_t)a.size()});

    // Forward, elementwise, through stan-math.
    std::vector<double> constrained(y.size());
    for (size_t i = 0; i < y.size(); ++i) {
      const auto at = [&](size_t k) {
        return c.args[k].size() == 1 ? c.args[k][0] : c.args[k][i];
      };
      switch (c.kind) {
        case K::Identity:
          constrained[i] = y[i];
          break;
        case K::Lower:
          constrained[i] = stan::math::lb_constrain(y[i], at(0));
          break;
        case K::Upper:
          constrained[i] = stan::math::ub_constrain(y[i], at(0));
          break;
        case K::LowerUpper:
          constrained[i] = stan::math::lub_constrain(y[i], at(0), at(1));
          break;
        case K::Offset:
          constrained[i] =
              stan::math::offset_multiplier_constrain(y[i], at(0), 1.0);
          break;
        case K::Multiplier:
          constrained[i] =
              stan::math::offset_multiplier_constrain(y[i], 0.0, at(0));
          break;
        default:
          constrained[i] =
              stan::math::offset_multiplier_constrain(y[i], at(0), at(1));
          break;
      }
    }

    check(stanli::free_leaf_size(c.kind, {(int64_t)y.size()}) ==
              (int64_t)y.size(),
          c.what + ": free length equals constrained length");
    std::vector<double> got(y.size(), 0.0);
    stanli::unconstrain_leaf(c.kind, {(int64_t)y.size()}, constrained.data(),
                             args, got.data());
    expect_near(c.what, got, y, 1e-11);
  }
}

// A leaf of any rank is one elementwise transform: an array of bounded reals
// unconstrains element for element regardless of how it is shaped.
void elementwise_ignores_rank() {
  using K = stanli::mir::Transform;
  const std::vector<double> lo = {0.0};
  const std::vector<stanli::TransformArg> args = {
      stanli::TransformArg{lo.data(), 1}};
  const std::vector<double> constrained = {0.5, 1.5, 2.5, 3.5, 4.5, 5.5};
  std::vector<double> got(6, 0.0);
  stanli::unconstrain_leaf(K::Lower, {2, 3}, constrained.data(), args,
                           got.data());
  for (size_t i = 0; i < constrained.size(); ++i)
    check(std::abs(got[i] - std::log(constrained[i])) < 1e-12,
          "lower bound over a 2x3 leaf, element " + std::to_string(i));
}

void free_sizes() {
  using K = stanli::mir::Transform;
  check(stanli::free_leaf_size(K::Simplex, {1}) == 0,
        "simplex[1] is free of 0");
  check(stanli::free_leaf_size(K::Covariance, {1, 1}) == 1, "cov_matrix[1]");
  check(stanli::free_leaf_size(K::CholeskyCov, {3, 3}) == 6,
        "square cholesky_factor_cov");
  check(stanli::transform_leaf_rank(K::Simplex) == 1, "simplex leaf rank");
  check(stanli::transform_leaf_rank(K::Covariance) == 2, "cov leaf rank");
  check(stanli::transform_leaf_rank(K::StochasticColumn) == 2,
        "column_stochastic leaf rank");
  check(stanli::free_leaf_size(K::StochasticRow, {3, 4}) == 9,
        "row_stochastic_matrix[3,4] frees 3x3");
  check(stanli::transform_leaf_rank(K::SumToZero) == 0,
        "sum_to_zero rank follows the declaration");
  check(stanli::transform_leaf_rank(K::Lower) == 0, "bounds take any shape");
}

// Shape disagreements are malformed models, and a violated constraint is a
// user's value. Both must be refused rather than quietly produce numbers.
void refusals() {
  using K = stanli::mir::Transform;
  const auto throws = [](auto&& f) {
    try {
      f();
    } catch (const std::exception&) {
      return true;
    }
    return false;
  };

  check(throws([] { return stanli::free_leaf_size(K::Simplex, {3, 3}); }),
        "a simplex over a matrix leaf is refused");
  check(throws([] { return stanli::free_leaf_size(K::Correlation, {3, 4}); }),
        "a rectangular corr_matrix is refused");
  check(throws([] { return stanli::free_leaf_size(K::CholeskyCov, {2, 3}); }),
        "cholesky_factor_cov with fewer rows than columns is refused");
  check(throws([] { return stanli::free_leaf_size(K::Unsupported, {3}); }),
        "an unsupported transform has no inverse");

  const std::vector<double> not_a_simplex = {0.5, 0.2, 0.1};
  std::vector<double> out(2, 0.0);
  check(throws([&] {
          stanli::unconstrain_leaf(K::Simplex, {3}, not_a_simplex.data(), {},
                                   out.data());
        }),
        "a simplex that does not sum to one is refused");

  const std::vector<double> below = {-1.0};
  const std::vector<double> zero = {0.0};
  const std::vector<stanli::TransformArg> lower = {
      stanli::TransformArg{zero.data(), 1}};
  std::vector<double> one(1, 0.0);
  check(throws([&] {
          stanli::unconstrain_leaf(K::Lower, {1}, below.data(), lower,
                                   one.data());
        }),
        "a value below its lower bound is refused");

  const std::vector<double> value = {1.0};
  check(throws([&] {
          stanli::unconstrain_leaf(K::Lower, {1}, value.data(), {}, one.data());
        }),
        "a bounded transform with no bound is refused");
}

}  // namespace

int main() {
  structured_round_trips();
  elementwise_round_trips();
  elementwise_ignores_rank();
  free_sizes();
  refusals();
  if (failures == 0) std::printf("test_unconstrain OK\n");
  return failures == 0 ? 0 : 1;
}
