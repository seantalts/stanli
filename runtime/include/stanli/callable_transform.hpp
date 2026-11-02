#ifndef STANLI_CALLABLE_TRANSFORM_HPP
#define STANLI_CALLABLE_TRANSFORM_HPP

#include <stanli/optable.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace stanli {

// The Stan functions which expose parameter transforms as ordinary calls.
// Keep name recognition and graph opcode selection here so graph lowering and
// runtime-control lowering cannot acquire different transform vocabularies.
enum class CallableTransformKind : unsigned char {
  Lower,
  Upper,
  LowerUpper,
  OffsetMultiplier,
  Ordered,
  PositiveOrdered,
  Simplex,
  StochasticColumn,
  StochasticRow,
  SumToZero,
  UnitVector,
  CholeskyFactorCorr,
  CorrMatrix,
  CovMatrix,
  CholeskyFactorCov,
};

enum class TransformDirection : unsigned char {
  Constrain,
  Jacobian,
  Unconstrain,
};

struct CallableTransformSpec {
  CallableTransformKind kind;
  TransformDirection direction;
  uint16_t opcode;
  size_t arity;
  bool structured;
};

inline bool transform_suffix(std::string_view name, std::string_view ending) {
  return name.size() >= ending.size() &&
         name.substr(name.size() - ending.size()) == ending;
}

inline bool callable_transform(std::string_view name,
                               CallableTransformSpec* out) {
  TransformDirection direction;
  std::string_view stem;
  if (transform_suffix(name, "_jacobian")) {
    direction = TransformDirection::Jacobian;
    stem = name.substr(0, name.size() - 9);
  } else if (transform_suffix(name, "_constrain")) {
    direction = TransformDirection::Constrain;
    stem = name.substr(0, name.size() - 10);
  } else if (transform_suffix(name, "_unconstrain")) {
    direction = TransformDirection::Unconstrain;
    stem = name.substr(0, name.size() - 12);
  } else {
    return false;
  }

  CallableTransformSpec s{};
  s.direction = direction;
  s.structured = true;
  if (stem == "lower_bound")
    s = {CallableTransformKind::Lower, direction, OP_CONSTRAIN_LOWER, 2, false};
  else if (stem == "upper_bound")
    s = {CallableTransformKind::Upper, direction, OP_CONSTRAIN_UPPER, 2, false};
  else if (stem == "lower_upper_bound")
    s = {CallableTransformKind::LowerUpper, direction, OP_CONSTRAIN_LU, 3,
         false};
  else if (stem == "offset_multiplier")
    s = {CallableTransformKind::OffsetMultiplier, direction,
         OP_CONSTRAIN_OFFSET_MULT, 3, false};
  else if (stem == "ordered")
    s = {CallableTransformKind::Ordered, direction, OP_CONSTRAIN_ORDERED, 1,
         true};
  else if (stem == "positive_ordered")
    s = {CallableTransformKind::PositiveOrdered, direction,
         OP_CONSTRAIN_POS_ORDERED, 1, true};
  else if (stem == "simplex")
    s = {CallableTransformKind::Simplex, direction, OP_CONSTRAIN_SIMPLEX, 1,
         true};
  else if (stem == "stochastic_column")
    s = {CallableTransformKind::StochasticColumn, direction,
         OP_CONSTRAIN_STOCHASTIC_COLUMN, 1, true};
  else if (stem == "stochastic_row")
    s = {CallableTransformKind::StochasticRow, direction,
         OP_CONSTRAIN_STOCHASTIC_ROW, 1, true};
  else if (stem == "sum_to_zero")
    s = {CallableTransformKind::SumToZero, direction, OP_CONSTRAIN_SUM_TO_ZERO,
         1, true};
  else if (stem == "unit_vector")
    s = {CallableTransformKind::UnitVector, direction, OP_CONSTRAIN_UNIT_VECTOR,
         1, true};
  else if (stem == "cholesky_factor_corr")
    s = {CallableTransformKind::CholeskyFactorCorr, direction,
         OP_CONSTRAIN_CHOL_CORR, 2, true};
  else if (stem == "corr_matrix")
    s = {CallableTransformKind::CorrMatrix, direction, OP_CONSTRAIN_CORR_MATRIX,
         2, true};
  else if (stem == "cov_matrix")
    s = {CallableTransformKind::CovMatrix, direction, OP_CONSTRAIN_COV_MATRIX,
         2, true};
  else if (stem == "cholesky_factor_cov")
    s = {CallableTransformKind::CholeskyFactorCov, direction,
         OP_CONSTRAIN_CHOL_COV, 3, true};
  else
    return false;
  if (s.structured && direction == TransformDirection::Unconstrain) s.arity = 1;
  *out = s;
  return true;
}

// The unconstrained leaf a structured inverse produces from one constrained
// leaf of `rows` by `cols` (cols is 0 for a vector). Returns false when the
// transform does not take that kind of leaf.
inline bool free_leaf_shape(CallableTransformKind kind, int64_t rows,
                            int64_t cols, int64_t* out_rows, int64_t* out_cols,
                            bool* out_matrix) {
  const bool matrix = cols != 0 ||
                      kind == CallableTransformKind::StochasticColumn ||
                      kind == CallableTransformKind::StochasticRow ||
                      kind == CallableTransformKind::CholeskyFactorCorr ||
                      kind == CallableTransformKind::CorrMatrix ||
                      kind == CallableTransformKind::CovMatrix ||
                      kind == CallableTransformKind::CholeskyFactorCov;
  *out_cols = 0;
  *out_matrix = false;
  switch (kind) {
    case CallableTransformKind::Ordered:
    case CallableTransformKind::PositiveOrdered:
    case CallableTransformKind::UnitVector:
      *out_rows = rows;
      return !matrix;
    case CallableTransformKind::Simplex:
      *out_rows = rows - 1;
      return !matrix;
    case CallableTransformKind::SumToZero:
      *out_rows = rows - 1;
      if (matrix) {
        *out_cols = cols - 1;
        *out_matrix = true;
      }
      return true;
    case CallableTransformKind::StochasticColumn:
      *out_rows = rows - 1;
      *out_cols = cols;
      *out_matrix = true;
      return matrix;
    case CallableTransformKind::StochasticRow:
      *out_rows = rows;
      *out_cols = cols - 1;
      *out_matrix = true;
      return matrix;
    case CallableTransformKind::CholeskyFactorCorr:
    case CallableTransformKind::CorrMatrix:
      *out_rows = rows * (rows - 1) / 2;
      return matrix;
    case CallableTransformKind::CovMatrix:
      *out_rows = rows * (rows + 1) / 2;
      return matrix;
    case CallableTransformKind::CholeskyFactorCov:
      *out_rows = cols * (cols + 1) / 2 + (rows - cols) * cols;
      return matrix;
    default:
      return false;
  }
}

}  // namespace stanli

#endif
