// Backend-neutral numeric value geometry shared by every registered Stan Math
// function. Backends translate their local value representation once, then
// registry-specific layout policies validate these descriptors.
#ifndef STANLI_FUNCTION_SHAPE_HPP
#define STANLI_FUNCTION_SHAPE_HPP

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace stanli {

enum class FunctionArgumentKind : uint8_t { Real, Integer };

enum class FunctionContainerKind : uint8_t {
  Scalar,
  Vector,
  RowVector,
  Matrix,
  Array,
};

struct FunctionArgumentShape {
  FunctionArgumentKind value = FunctionArgumentKind::Real;
  FunctionContainerKind container = FunctionContainerKind::Scalar;
  FunctionContainerKind array_leaf = FunctionContainerKind::Scalar;
  // Array extents precede leaf extents. Matrix leaves therefore contribute
  // two trailing dimensions and vector/row-vector leaves contribute one.
  std::vector<int64_t> dimensions;
  int64_t storage_size = 1;
  uint8_t array_depth = 0;
};

inline FunctionContainerKind function_leaf(const FunctionArgumentShape& shape) {
  return shape.container == FunctionContainerKind::Array ? shape.array_leaf
                                                         : shape.container;
}

inline size_t function_leaf_rank(FunctionContainerKind container) {
  switch (container) {
    case FunctionContainerKind::Scalar:
      return 0;
    case FunctionContainerKind::Vector:
    case FunctionContainerKind::RowVector:
      return 1;
    case FunctionContainerKind::Matrix:
      return 2;
    case FunctionContainerKind::Array:
      throw std::invalid_argument("function shape has an array leaf");
  }
  throw std::invalid_argument("function shape has an unknown leaf");
}

// Construct the backend-neutral shape and validate the complete logical
// geometry at the backend boundary. Array extents precede leaf extents.
inline FunctionArgumentShape make_function_shape(
    FunctionArgumentKind value, FunctionContainerKind container,
    FunctionContainerKind array_leaf, std::vector<int64_t> dimensions,
    int64_t storage_size) {
  if (storage_size < 0)
    throw std::invalid_argument("function shape has negative storage size");

  const FunctionContainerKind leaf =
      container == FunctionContainerKind::Array ? array_leaf : container;
  const size_t leaf_dimensions = function_leaf_rank(leaf);
  size_t array_depth = 0;
  if (container == FunctionContainerKind::Array) {
    if (dimensions.size() <= leaf_dimensions)
      throw std::invalid_argument("function array shape has no array extent");
    array_depth = dimensions.size() - leaf_dimensions;
    if (array_depth > std::numeric_limits<uint8_t>::max())
      throw std::length_error("function array rank exceeds 255");
  } else if (dimensions.size() != leaf_dimensions) {
    throw std::invalid_argument("function shape rank does not match container");
  }

  int64_t logical_size = 1;
  for (int64_t extent : dimensions) {
    if (extent < 0)
      throw std::invalid_argument("function shape has a negative extent");
    if (extent != 0 &&
        logical_size > std::numeric_limits<int64_t>::max() / extent)
      throw std::length_error("function shape storage size overflows");
    logical_size *= extent;
  }
  if (logical_size != storage_size)
    throw std::invalid_argument("function shape does not match storage");

  FunctionArgumentShape result;
  result.value = value;
  result.container = container;
  result.array_leaf = container == FunctionContainerKind::Array
                          ? array_leaf
                          : FunctionContainerKind::Scalar;
  result.dimensions = std::move(dimensions);
  result.storage_size = storage_size;
  result.array_depth = static_cast<uint8_t>(array_depth);
  return result;
}

}  // namespace stanli

#endif
