// Adapter from the graph/register-machine view vocabulary to the neutral
// function shape contract. Both lowering backends use this representation.
#ifndef STANLI_FUNCTION_VIEW_SHAPE_HPP
#define STANLI_FUNCTION_VIEW_SHAPE_HPP

#include <stanli/function_shape.hpp>

#include <cstdint>
#include <utility>
#include <vector>

namespace stanli {

enum class ViewKind : uint8_t { Flat, Vector, RowVector, Matrix, Array };

inline FunctionContainerKind function_container_kind(ViewKind kind,
                                                     int64_t storage_size) {
  switch (kind) {
    case ViewKind::Flat:
      return storage_size == 1 ? FunctionContainerKind::Scalar
                               : FunctionContainerKind::Vector;
    case ViewKind::Vector:
      return FunctionContainerKind::Vector;
    case ViewKind::RowVector:
      return FunctionContainerKind::RowVector;
    case ViewKind::Matrix:
      return FunctionContainerKind::Matrix;
    case ViewKind::Array:
      return FunctionContainerKind::Array;
  }
  throw std::invalid_argument("unknown function view kind");
}

inline ViewKind function_view_kind(FunctionContainerKind kind) {
  switch (kind) {
    case FunctionContainerKind::Scalar:
      return ViewKind::Flat;
    case FunctionContainerKind::Vector:
      return ViewKind::Vector;
    case FunctionContainerKind::RowVector:
      return ViewKind::RowVector;
    case FunctionContainerKind::Matrix:
      return ViewKind::Matrix;
    case FunctionContainerKind::Array:
      return ViewKind::Array;
  }
  throw std::invalid_argument("unknown function container kind");
}

inline FunctionArgumentShape make_view_function_shape(
    FunctionArgumentKind value, ViewKind kind, ViewKind leaf,
    std::vector<int64_t> array_dimensions, int64_t storage_size,
    int64_t rows = 0, int64_t cols = 0) {
  const FunctionContainerKind container =
      function_container_kind(kind, storage_size);
  FunctionContainerKind array_leaf = FunctionContainerKind::Scalar;
  std::vector<int64_t> dimensions;
  if (kind == ViewKind::Vector || kind == ViewKind::RowVector ||
      (kind == ViewKind::Flat && storage_size != 1)) {
    dimensions = {storage_size};
  } else if (kind == ViewKind::Matrix) {
    dimensions = {rows, cols};
  } else if (kind == ViewKind::Array) {
    dimensions = std::move(array_dimensions);
    array_leaf = function_container_kind(leaf, 1);
  }
  return make_function_shape(value, container, array_leaf,
                             std::move(dimensions), storage_size);
}

}  // namespace stanli

#endif
