#include <stanli/program.hpp>

#include <stan/math.hpp>

#include <algorithm>

namespace stanli {
namespace {

template <typename T>
void run_transform(const Program::Transform& tr, T* reg) {
  T lp = 0.0;
  const auto bound = [&](int k, int i) -> const T& {
    return reg[tr.in[k] + (tr.in_len[k] == 1 ? 0 : i)];
  };
  switch (tr.kind) {
    case CallableTransformKind::Lower:
      for (int i = 0; i < tr.out_len; ++i)
        reg[tr.out + i] =
            tr.direction == TransformDirection::Unconstrain
                ? stan::math::lb_free(reg[tr.in[0] + i], bound(1, i))
                : stan::math::lb_constrain(reg[tr.in[0] + i], bound(1, i), lp);
      break;
    case CallableTransformKind::Upper:
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
      if (tr.direction == TransformDirection::Unconstrain) {
        const int free_len = tr.out_len / std::max(tr.batch, 1);
        for (int b = 0; b < tr.batch; ++b) {
          const T* input = reg + tr.in[0] + b * tr.inner_raw;
          T* output = reg + tr.out + b * free_len;
          const auto store = [&](const auto& x) {
            for (int i = 0; i < free_len; ++i) output[i] = x.data()[i];
          };
          Eigen::Map<const Mat> m(input, tr.in_rows, std::max(tr.in_cols, 1));
          Eigen::Map<const Vec> v(input, tr.inner_raw);
          switch (tr.kind) {
            case CallableTransformKind::Ordered:
              store(Vec(stan::math::ordered_free(v)));
              break;
            case CallableTransformKind::PositiveOrdered:
              store(Vec(stan::math::positive_ordered_free(v)));
              break;
            case CallableTransformKind::Simplex:
              store(Vec(stan::math::simplex_free(v)));
              break;
            case CallableTransformKind::UnitVector:
              store(Vec(stan::math::unit_vector_free(v)));
              break;
            case CallableTransformKind::SumToZero:
              if (tr.in_cols)
                store(Mat(stan::math::sum_to_zero_free(Mat(m))));
              else
                store(Vec(stan::math::sum_to_zero_free(Vec(v))));
              break;
            case CallableTransformKind::StochasticColumn:
              store(Mat(stan::math::stochastic_column_free(Mat(m))));
              break;
            case CallableTransformKind::StochasticRow:
              store(Mat(stan::math::stochastic_row_free(Mat(m))));
              break;
            case CallableTransformKind::CholeskyFactorCorr:
              store(Vec(stan::math::cholesky_corr_free(Mat(m))));
              break;
            case CallableTransformKind::CorrMatrix:
              store(Vec(stan::math::corr_matrix_free(Mat(m))));
              break;
            case CallableTransformKind::CovMatrix:
              store(Vec(stan::math::cov_matrix_free(Mat(m))));
              break;
            default:
              store(Vec(stan::math::cholesky_factor_free(Mat(m))));
              break;
          }
        }
        break;
      }
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
