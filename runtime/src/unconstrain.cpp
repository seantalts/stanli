#include <stanli/unconstrain.hpp>

#include <stanli/compile.hpp>

// prim only: no direction here needs a derivative, and the per-header
// constraint includes do not pull in enough of the library to compile on
// their own.
#include <stan/math/prim.hpp>

#include <Eigen/Dense>

#include <string>

namespace stanli {
namespace {

using Vec = Eigen::VectorXd;
using MapVec = Eigen::Map<const Vec>;
using MapMat = Eigen::Map<const Eigen::MatrixXd>;

[[noreturn]] void bad_shape(const std::string& what) {
  throw CompileError(what);
}

int64_t product(const std::vector<int64_t>& dims) {
  int64_t n = 1;
  for (const int64_t d : dims) {
    if (d < 0) bad_shape("negative leaf dimension");
    n *= d;
  }
  return n;
}

bool elementwise(mir::Transform::Kind kind) {
  switch (kind) {
    case mir::Transform::Identity:
    case mir::Transform::Lower:
    case mir::Transform::Upper:
    case mir::Transform::LowerUpper:
    case mir::Transform::Offset:
    case mir::Transform::Multiplier:
    case mir::Transform::OffsetMultiplier:
      return true;
    default:
      return false;
  }
}

// The declared square side of a matrix leaf, checked square.
int64_t square_side(const std::vector<int64_t>& dims, const char* what) {
  if (dims.size() != 2) bad_shape(std::string(what) + " needs a matrix leaf");
  if (dims[0] != dims[1]) bad_shape(std::string(what) + " needs a square leaf");
  return dims[0];
}

int64_t vector_len(const std::vector<int64_t>& dims, const char* what) {
  if (dims.size() != 1) bad_shape(std::string(what) + " needs a vector leaf");
  return dims[0];
}

// One transform argument as a value for element `i` of the leaf. Length 1
// broadcasts, which is how a scalar bound over a container is spelled.
double arg_at(const TransformArg& arg, int64_t i, const char* what) {
  if (arg.len == 1) return arg.data[0];
  if (i >= arg.len)
    bad_shape(std::string(what) + " bound is shorter than the value");
  return arg.data[i];
}

void need_args(const std::vector<TransformArg>& args, size_t n,
               const char* what) {
  if (args.size() != n)
    bad_shape(std::string(what) + " expects " + std::to_string(n) +
              " transform arguments");
}

}  // namespace

int transform_leaf_rank(mir::Transform::Kind kind) {
  switch (kind) {
    case mir::Transform::Simplex:
    case mir::Transform::Ordered:
    case mir::Transform::PositiveOrdered:
    case mir::Transform::UnitVector:
      return 1;
    case mir::Transform::CholeskyCorr:
    case mir::Transform::Correlation:
    case mir::Transform::Covariance:
    case mir::Transform::CholeskyCov:
      return 2;
    default:
      // Elementwise transforms take the whole value, and SumToZero's rank
      // follows its declaration rather than its kind.
      return 0;
  }
}

int64_t free_leaf_size(mir::Transform::Kind kind,
                       const std::vector<int64_t>& leaf_dims) {
  // These match the forward lowering's table in lower.cpp exactly; the two
  // disagreeing would put every draw's parameters at the wrong offsets.
  if (elementwise(kind)) return product(leaf_dims);
  switch (kind) {
    case mir::Transform::Simplex:
      return vector_len(leaf_dims, "simplex") - 1;
    case mir::Transform::Ordered:
      return vector_len(leaf_dims, "ordered");
    case mir::Transform::PositiveOrdered:
      return vector_len(leaf_dims, "positive_ordered");
    case mir::Transform::UnitVector:
      return vector_len(leaf_dims, "unit_vector");
    case mir::Transform::SumToZero:
      if (leaf_dims.size() == 1) return leaf_dims[0] - 1;
      if (leaf_dims.size() == 2) return (leaf_dims[0] - 1) * (leaf_dims[1] - 1);
      bad_shape("sum_to_zero needs a vector or matrix leaf");
    case mir::Transform::CholeskyCorr:
      return square_side(leaf_dims, "cholesky_factor_corr") *
             (square_side(leaf_dims, "cholesky_factor_corr") - 1) / 2;
    case mir::Transform::Correlation:
      return square_side(leaf_dims, "corr_matrix") *
             (square_side(leaf_dims, "corr_matrix") - 1) / 2;
    case mir::Transform::Covariance:
      return square_side(leaf_dims, "cov_matrix") *
             (square_side(leaf_dims, "cov_matrix") + 1) / 2;
    case mir::Transform::CholeskyCov: {
      if (leaf_dims.size() != 2)
        bad_shape("cholesky_factor_cov needs a matrix leaf");
      const int64_t rows = leaf_dims[0], cols = leaf_dims[1];
      if (rows < cols)
        bad_shape("cholesky_factor_cov has fewer rows than columns");
      return cols * (cols + 1) / 2 + (rows - cols) * cols;
    }
    case mir::Transform::Unsupported:
    default:
      bad_shape("no inverse for this parameter transform");
  }
}

void unconstrain_leaf(mir::Transform::Kind kind,
                      const std::vector<int64_t>& leaf_dims,
                      const double* constrained,
                      const std::vector<TransformArg>& args, double* out) {
  const int64_t con_len = product(leaf_dims);
  const int64_t free_len = free_leaf_size(kind, leaf_dims);

  if (elementwise(kind)) {
    // Scalar overloads rather than hand-written inverses, and the same
    // broadcast rule the forward direction uses.
    for (int64_t i = 0; i < con_len; ++i) {
      const double x = constrained[i];
      switch (kind) {
        case mir::Transform::Identity:
          out[i] = x;
          break;
        case mir::Transform::Lower:
          need_args(args, 1, "lower bound");
          out[i] = stan::math::lb_free(x, arg_at(args[0], i, "lower"));
          break;
        case mir::Transform::Upper:
          need_args(args, 1, "upper bound");
          out[i] = stan::math::ub_free(x, arg_at(args[0], i, "upper"));
          break;
        case mir::Transform::LowerUpper:
          need_args(args, 2, "lower and upper bounds");
          out[i] = stan::math::lub_free(x, arg_at(args[0], i, "lower"),
                                        arg_at(args[1], i, "upper"));
          break;
        // stanc3 spells a lone offset or multiplier as the pair with the
        // other side at its identity, so one path serves all three.
        case mir::Transform::Offset:
          need_args(args, 1, "offset");
          out[i] = stan::math::offset_multiplier_free(
              x, arg_at(args[0], i, "offset"), 1.0);
          break;
        case mir::Transform::Multiplier:
          need_args(args, 1, "multiplier");
          out[i] = stan::math::offset_multiplier_free(
              x, 0.0, arg_at(args[0], i, "multiplier"));
          break;
        default:
          need_args(args, 2, "offset and multiplier");
          out[i] = stan::math::offset_multiplier_free(
              x, arg_at(args[0], i, "offset"),
              arg_at(args[1], i, "multiplier"));
          break;
      }
    }
    return;
  }

  need_args(args, 0, "a structured transform");
  const auto write = [&](const auto& free) {
    for (int64_t i = 0; i < free_len; ++i) out[i] = free.data()[i];
  };

  switch (kind) {
    case mir::Transform::Simplex:
      write(stan::math::simplex_free(MapVec(constrained, con_len)));
      return;
    case mir::Transform::Ordered:
      write(stan::math::ordered_free(MapVec(constrained, con_len)));
      return;
    case mir::Transform::PositiveOrdered:
      write(stan::math::positive_ordered_free(MapVec(constrained, con_len)));
      return;
    case mir::Transform::UnitVector:
      write(stan::math::unit_vector_free(MapVec(constrained, con_len)));
      return;
    case mir::Transform::SumToZero:
      // The vector and matrix forms are different overloads, and the matrix
      // one returns a matrix whose column-major storage is already the free
      // layout the forward kernel reads.
      if (leaf_dims.size() == 1)
        write(stan::math::sum_to_zero_free(MapVec(constrained, con_len)));
      else
        write(stan::math::sum_to_zero_free(
            MapMat(constrained, leaf_dims[0], leaf_dims[1])));
      return;
    case mir::Transform::CholeskyCorr:
      write(stan::math::cholesky_corr_free(
          MapMat(constrained, leaf_dims[0], leaf_dims[1])));
      return;
    case mir::Transform::Correlation:
      write(stan::math::corr_matrix_free(
          MapMat(constrained, leaf_dims[0], leaf_dims[1])));
      return;
    case mir::Transform::Covariance:
      write(stan::math::cov_matrix_free(
          MapMat(constrained, leaf_dims[0], leaf_dims[1])));
      return;
    case mir::Transform::CholeskyCov:
      write(stan::math::cholesky_factor_free(
          MapMat(constrained, leaf_dims[0], leaf_dims[1])));
      return;
    default:
      bad_shape("no inverse for this parameter transform");
  }
}

}  // namespace stanli
