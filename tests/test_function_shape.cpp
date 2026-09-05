#include <stanli/function_shape.hpp>

#include <cstdio>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

int failures = 0;

template <typename F>
void rejects(const char* name, F&& operation) {
  try {
    operation();
    std::printf("FAIL accepted %s\n", name);
    ++failures;
  } catch (const std::invalid_argument&) {
  } catch (const std::length_error&) {
  }
}

}  // namespace

int main() {
  using namespace stanli;

  if (function_leaf_rank(FunctionContainerKind::Scalar) != 0 ||
      function_leaf_rank(FunctionContainerKind::Vector) != 1 ||
      function_leaf_rank(FunctionContainerKind::RowVector) != 1 ||
      function_leaf_rank(FunctionContainerKind::Matrix) != 2) {
    std::printf("FAIL leaf ranks\n");
    ++failures;
  }

  const FunctionArgumentShape scalar = make_function_shape(
      FunctionArgumentKind::Real, FunctionContainerKind::Scalar,
      FunctionContainerKind::Scalar, {}, 1);
  const FunctionArgumentShape vector = make_function_shape(
      FunctionArgumentKind::Real, FunctionContainerKind::Vector,
      FunctionContainerKind::Scalar, {3}, 3);
  const FunctionArgumentShape scalar_array = make_function_shape(
      FunctionArgumentKind::Integer, FunctionContainerKind::Array,
      FunctionContainerKind::Scalar, {2, 3}, 6);
  const FunctionArgumentShape vector_array = make_function_shape(
      FunctionArgumentKind::Real, FunctionContainerKind::Array,
      FunctionContainerKind::Vector, {2, 3}, 6);
  const FunctionArgumentShape matrix_array = make_function_shape(
      FunctionArgumentKind::Real, FunctionContainerKind::Array,
      FunctionContainerKind::Matrix, {2, 3, 4}, 24);
  const FunctionArgumentShape empty_array = make_function_shape(
      FunctionArgumentKind::Real, FunctionContainerKind::Array,
      FunctionContainerKind::Scalar, {0}, 0);
  if (scalar.array_depth != 0 || scalar.storage_size != 1 ||
      vector.array_depth != 0 || vector.dimensions != std::vector<int64_t>{3} ||
      scalar_array.array_depth != 2 ||
      function_leaf(scalar_array) != FunctionContainerKind::Scalar ||
      vector_array.array_depth != 1 ||
      function_leaf(vector_array) != FunctionContainerKind::Vector ||
      matrix_array.array_depth != 1 ||
      function_leaf(matrix_array) != FunctionContainerKind::Matrix ||
      empty_array.array_depth != 1 || empty_array.storage_size != 0) {
    std::printf("FAIL constructed shapes\n");
    ++failures;
  }

  rejects("array leaf",
          [] { (void)function_leaf_rank(FunctionContainerKind::Array); });
  rejects("scalar rank", [] {
    (void)make_function_shape(FunctionArgumentKind::Real,
                              FunctionContainerKind::Scalar,
                              FunctionContainerKind::Scalar, {1}, 1);
  });
  rejects("missing scalar-array extent", [] {
    (void)make_function_shape(FunctionArgumentKind::Real,
                              FunctionContainerKind::Array,
                              FunctionContainerKind::Scalar, {}, 1);
  });
  rejects("missing matrix-array extent", [] {
    (void)make_function_shape(FunctionArgumentKind::Real,
                              FunctionContainerKind::Array,
                              FunctionContainerKind::Matrix, {2, 3}, 6);
  });
  rejects("nested array leaf", [] {
    (void)make_function_shape(FunctionArgumentKind::Real,
                              FunctionContainerKind::Array,
                              FunctionContainerKind::Array, {1}, 1);
  });
  rejects("negative extent", [] {
    (void)make_function_shape(FunctionArgumentKind::Real,
                              FunctionContainerKind::Vector,
                              FunctionContainerKind::Scalar, {-1}, 0);
  });
  rejects("negative storage", [] {
    (void)make_function_shape(FunctionArgumentKind::Real,
                              FunctionContainerKind::Scalar,
                              FunctionContainerKind::Scalar, {}, -1);
  });
  rejects("storage mismatch", [] {
    (void)make_function_shape(FunctionArgumentKind::Real,
                              FunctionContainerKind::Matrix,
                              FunctionContainerKind::Scalar, {2, 3}, 5);
  });
  rejects("extent overflow", [] {
    (void)make_function_shape(FunctionArgumentKind::Real,
                              FunctionContainerKind::Array,
                              FunctionContainerKind::Scalar,
                              {std::numeric_limits<int64_t>::max(), 2}, 0);
  });
  rejects("array depth overflow", [] {
    (void)make_function_shape(
        FunctionArgumentKind::Real, FunctionContainerKind::Array,
        FunctionContainerKind::Scalar, std::vector<int64_t>(256, 1), 1);
  });

  if (failures == 0) std::printf("OK\n");
  return failures == 0 ? 0 : 1;
}
