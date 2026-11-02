#include <stanli/program.hpp>

#include <stan/math.hpp>

#include <algorithm>

namespace stanli {
namespace {

// An Eigen first input with shared scalar bounds is checked as one object by
// Stan Math, which names the bad element ("Lower bounded variable[1, 1]");
// per-element bounds or non-Eigen inputs reach the scalar overloads, which
// name nothing.
template <typename T, typename Check>
void check_eigen_input(const Program::Transform& tr, const T* reg,
                       Check&& check) {
  if (tr.eigen == 0 || tr.out_len == 0 || tr.in_len[1] != 1 ||
      (tr.n_in > 2 && tr.in_len[2] != 1))
    return;
  std::vector<double> y(static_cast<size_t>(tr.out_len));
  for (int i = 0; i < tr.out_len; ++i)
    y[static_cast<size_t>(i)] = stan::math::value_of(reg[tr.in[0] + i]);
  const auto bound = [&](int k) { return stan::math::value_of(reg[tr.in[k]]); };
  if (tr.eigen == 1)
    check(Eigen::Map<const Eigen::VectorXd>(y.data(), tr.out_len), bound);
  else if (tr.eigen == 2)
    check(Eigen::Map<const Eigen::RowVectorXd>(y.data(), tr.out_len), bound);
  else
    check(Eigen::Map<const Eigen::MatrixXd>(y.data(), tr.rows,
                                            tr.out_len / tr.rows),
          bound);
}

template <typename T>
void run_transform(const Program::Transform& tr, T* reg) {
  T lp = 0.0;
  const bool unconstrain = tr.direction == TransformDirection::Unconstrain;
  const auto bound = [&](int k, int i) -> const T& {
    return reg[tr.in[k] + (tr.in_len[k] == 1 ? 0 : i)];
  };
  switch (tr.kind) {
    case CallableTransformKind::Lower:
      if (unconstrain)
        check_eigen_input(tr, reg, [](const auto& y, const auto& bound) {
          if (bound(1) != stan::math::NEGATIVE_INFTY)
            stan::math::check_greater_or_equal(
                "lb_free", "Lower bounded variable", y, bound(1));
        });
      for (int i = 0; i < tr.out_len; ++i)
        reg[tr.out + i] =
            tr.direction == TransformDirection::Unconstrain
                ? stan::math::lb_free(reg[tr.in[0] + i], bound(1, i))
                : stan::math::lb_constrain(reg[tr.in[0] + i], bound(1, i), lp);
      break;
    case CallableTransformKind::Upper:
      if (unconstrain)
        check_eigen_input(tr, reg, [](const auto& y, const auto& bound) {
          if (bound(1) != stan::math::INFTY)
            stan::math::check_less_or_equal("ub_free", "Upper bounded variable",
                                            y, bound(1));
        });
      for (int i = 0; i < tr.out_len; ++i)
        reg[tr.out + i] =
            tr.direction == TransformDirection::Unconstrain
                ? stan::math::ub_free(reg[tr.in[0] + i], bound(1, i))
                : stan::math::ub_constrain(reg[tr.in[0] + i], bound(1, i), lp);
      break;
    case CallableTransformKind::LowerUpper:
      for (int i = 0; i < tr.out_len; ++i)
        reg[tr.out + i] =
            tr.direction == TransformDirection::Unconstrain
                ? stan::math::lub_free(reg[tr.in[0] + i], bound(1, i),
                                       bound(2, i))
                : stan::math::lub_constrain(reg[tr.in[0] + i], bound(1, i),
                                            bound(2, i), lp);
      break;
    case CallableTransformKind::OffsetMultiplier:
      for (int i = 0; i < tr.out_len; ++i)
        reg[tr.out + i] =
            tr.direction == TransformDirection::Unconstrain
                ? stan::math::offset_multiplier_free(reg[tr.in[0] + i],
                                                     bound(1, i), bound(2, i))
                : stan::math::offset_multiplier_constrain(
                      reg[tr.in[0] + i], bound(1, i), bound(2, i), lp);
      break;
    default: {
      using Vec = Eigen::Matrix<T, Eigen::Dynamic, 1>;
      using Mat = Eigen::Matrix<T, Eigen::Dynamic, Eigen::Dynamic>;
      const int con = tr.out_len / std::max(tr.batch, 1);
      for (int b = 0; b < tr.batch; ++b) {
        const T* input = reg + tr.in[0] + b * tr.inner_raw;
        T* output = reg + tr.out + b * con;
        if (tr.kind == CallableTransformKind::StochasticColumn ||
            tr.kind == CallableTransformKind::StochasticRow ||
            (tr.kind == CallableTransformKind::SumToZero && tr.out_cols)) {
          const int rr = tr.out_rows -
                         (tr.kind == CallableTransformKind::StochasticColumn ||
                          tr.kind == CallableTransformKind::SumToZero);
          const int rc =
              tr.out_cols - (tr.kind == CallableTransformKind::StochasticRow ||
                             tr.kind == CallableTransformKind::SumToZero);
          Eigen::Map<const Mat> y(input, rr, rc);
          Mat x;
          if (tr.kind == CallableTransformKind::StochasticColumn)
            x = stan::math::stochastic_column_constrain(y, lp);
          else if (tr.kind == CallableTransformKind::StochasticRow)
            x = stan::math::stochastic_row_constrain(y, lp);
          else
            x = stan::math::sum_to_zero_constrain(y);
          for (int i = 0; i < con; ++i) output[i] = x.data()[i];
          continue;
        }
        Eigen::Map<const Vec> y(input, tr.inner_raw);
        if (tr.out_cols) {
          Mat x;
          if (tr.kind == CallableTransformKind::CholeskyFactorCorr)
            x = stan::math::cholesky_corr_constrain(y, tr.out_rows, lp);
          else if (tr.kind == CallableTransformKind::CorrMatrix)
            x = stan::math::corr_matrix_constrain(y, tr.out_rows, lp);
          else if (tr.kind == CallableTransformKind::CovMatrix)
            x = stan::math::cov_matrix_constrain(y, tr.out_rows, lp);
          else
            x = stan::math::cholesky_factor_constrain(y, tr.out_rows,
                                                      tr.out_cols, lp);
          for (int i = 0; i < con; ++i) output[i] = x.data()[i];
        } else {
          Vec x;
          if (tr.kind == CallableTransformKind::Ordered)
            x = stan::math::ordered_constrain(y, lp);
          else if (tr.kind == CallableTransformKind::PositiveOrdered)
            x = stan::math::positive_ordered_constrain(y, lp);
          else if (tr.kind == CallableTransformKind::Simplex)
            x = stan::math::simplex_constrain(y, lp);
          else if (tr.kind == CallableTransformKind::UnitVector)
            x = stan::math::unit_vector_constrain(y, lp);
          else
            x = stan::math::sum_to_zero_constrain(y);
          for (int i = 0; i < con; ++i) output[i] = x[i];
        }
      }
      break;
    }
  }
  reg[tr.jac] = lp;
}

}  // namespace

void run_program_transform(const Program::Transform& tr, double* reg) {
  run_transform(tr, reg);
}

void run_program_transform(const Program::Transform& tr, stan::math::var* reg) {
  run_transform(tr, reg);
}

}  // namespace stanli
