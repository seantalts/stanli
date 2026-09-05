#include <stanli/builtin_registry.hpp>
#include <stanli/function_registry.hpp>

#include <stan/math/prim/err/check_greater.hpp>
#include <stan/math/prim/err/check_less_or_equal.hpp>
#include <stan/math/prim/err/check_matching_dims.hpp>
#include <stan/math/prim/err/check_nonnegative.hpp>
#include <stan/math/prim/err/check_size_match.hpp>
#include <stan/math/prim/err/out_of_range.hpp>
#include <stan/math/prim/fun/choose.hpp>
#include <stan/math/prim/fun/identity_matrix.hpp>
#include <stan/math/prim/fun/linspaced_array.hpp>
#include <stan/math/prim/fun/linspaced_int_array.hpp>
#include <stan/math/prim/fun/linspaced_row_vector.hpp>
#include <stan/math/prim/fun/linspaced_vector.hpp>
#include <stan/math/prim/fun/one_hot_array.hpp>
#include <stan/math/prim/fun/one_hot_int_array.hpp>
#include <stan/math/prim/fun/ones_array.hpp>
#include <stan/math/prim/fun/zeros_array.hpp>
#include <stan/math/prim/fun/one_hot_row_vector.hpp>
#include <stan/math/prim/fun/one_hot_vector.hpp>
#include <stan/math/prim/fun/ones_int_array.hpp>
#include <stan/math/prim/fun/ones_row_vector.hpp>
#include <stan/math/prim/fun/ones_vector.hpp>
#include <stan/math/prim/fun/uniform_simplex.hpp>
#include <stan/math/prim/fun/zeros_int_array.hpp>
#include <stan/math/prim/fun/zeros_row_vector.hpp>
#include <stan/math/prim/fun/zeros_vector.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace stanli {
namespace {

bool same_builtin_shape(const BuiltinArgumentShape& a,
                        const BuiltinArgumentShape& b) {
  return a.container == b.container && a.array_leaf == b.array_leaf &&
         a.dimensions == b.dimensions && a.storage_size == b.storage_size;
}

const char* slice_function_name(BuiltinSlice slice) {
  switch (slice) {
    case BuiltinSlice::Head:
      return "head";
    case BuiltinSlice::Tail:
      return "tail";
    case BuiltinSlice::Segment:
      return "segment";
    case BuiltinSlice::Col:
      return "col";
    case BuiltinSlice::Row:
      return "row";
    case BuiltinSlice::SubCol:
      return "sub_col";
    case BuiltinSlice::Block:
      return "block";
    case BuiltinSlice::Diagonal:
      return "diagonal";
    case BuiltinSlice::Reverse:
      return "reverse";
    case BuiltinSlice::Transpose:
      return "transpose";
    case BuiltinSlice::ToVector:
      return "to_vector";
    case BuiltinSlice::ToRowVector:
      return "to_row_vector";
    case BuiltinSlice::ToMatrix:
      return "to_matrix";
    case BuiltinSlice::ToArray1d:
      return "to_array_1d";
    case BuiltinSlice::RepVector:
      return "rep_vector";
    case BuiltinSlice::RepRowVector:
      return "rep_row_vector";
    case BuiltinSlice::RepMatrix:
      return "rep_matrix";
    case BuiltinSlice::RepArray:
      return "rep_array";
    case BuiltinSlice::AppendRow:
      return "append_row";
    case BuiltinSlice::AppendCol:
      return "append_col";
    case BuiltinSlice::AppendArray:
      return "append_array";
    case BuiltinSlice::None:
      break;
  }
  throw std::logic_error("builtin is not a slice");
}

int64_t checked_slice_product(int64_t lhs, int64_t rhs) {
  if (lhs != 0 && rhs > std::numeric_limits<int64_t>::max() / lhs)
    throw std::invalid_argument("result is too large");
  return lhs * rhs;
}

// The logical matrix extents append_row/append_col reason over: a scalar is
// a one-by-one block, a vector one column, a row vector one row.
struct AppendBlock {
  int64_t rows = 0;
  int64_t columns = 0;
};

AppendBlock append_block(const BuiltinArgumentShape& shape) {
  switch (shape.container) {
    case FunctionContainerKind::Scalar:
      return {1, 1};
    case FunctionContainerKind::Vector:
      return {shape.storage_size, 1};
    case FunctionContainerKind::RowVector:
      return {1, shape.storage_size};
    case FunctionContainerKind::Matrix:
      return {shape.dimensions[0], shape.dimensions[1]};
    case FunctionContainerKind::Array:
      break;
  }
  throw std::invalid_argument("needs a scalar, vector, row vector, or matrix");
}

// The 1-based index checks Stan Math's check_vector_index / check_row_index /
// check_column_index family performs, with the same messages and the same
// std::out_of_range it throws.
void check_slice_index(const char* function, int64_t index, int64_t size,
                       const char* message) {
  if (index >= 1 && index <= size) return;
  stan::math::out_of_range(function, static_cast<int>(size),
                           static_cast<int>(index), message);
}

// Stan Math's segment checks: check_greater / check_less_or_equal, throwing
// std::domain_error. The Eigen overload names both checked values "n"; the
// std::vector overload names them "i" and "i+n-1".
void check_segment_bound(const char* name, int64_t value, int64_t size) {
  stan::math::check_greater("segment", name, value, 0);
  stan::math::check_less_or_equal("segment", name, value, size);
}

int64_t slice_suffix_width(const BuiltinArgumentShape& input) {
  int64_t width = 1;
  for (size_t d = 1; d < input.dimensions.size(); ++d)
    width *= input.dimensions[d];
  return width;
}

// head/tail/segment/reverse act on dimension zero alone. Cells sharing one
// suffix coordinate are `width` chunks of the outer extent under OuterMajor
// storage and `width` interleaved runs under FirstIndexFast storage.
BuiltinSliceMap slice_along_outer(const BuiltinArgumentShape& input,
                                  int64_t start, int64_t count, bool reversed,
                                  SliceStorageOrder order) {
  const int64_t outer = input.dimensions.empty() ? 0 : input.dimensions.front();
  const int64_t width = slice_suffix_width(input);
  BuiltinSliceMap map;
  map.count = count * width;
  std::vector<int64_t> dimensions = input.dimensions;
  dimensions.front() = count;
  map.result =
      make_function_shape(input.value, input.container, input.array_leaf,
                          std::move(dimensions), map.count);
  if (!reversed && (order == SliceStorageOrder::OuterMajor || width == 1)) {
    map.kind = BuiltinSliceMap::Kind::Contiguous;
    map.offset = start * width;
    return map;
  }
  map.kind = BuiltinSliceMap::Kind::Gather;
  map.gather.reserve(static_cast<size_t>(map.count));
  if (order == SliceStorageOrder::OuterMajor) {
    for (int64_t k = 0; k < count; ++k) {
      const int64_t element = start + (reversed ? count - 1 - k : k);
      for (int64_t cell = 0; cell < width; ++cell)
        map.gather.push_back(element * width + cell);
    }
    return map;
  }
  for (int64_t suffix = 0; suffix < width; ++suffix)
    for (int64_t k = 0; k < count; ++k)
      map.gather.push_back(suffix * outer + start +
                           (reversed ? count - 1 - k : k));
  return map;
}

BuiltinArgumentShape slice_vector_shape(FunctionArgumentKind value,
                                        FunctionContainerKind container,
                                        int64_t size) {
  return make_function_shape(value, container, FunctionContainerKind::Scalar,
                             {size}, size);
}

BuiltinSliceMap slice_identity_map(BuiltinArgumentShape result) {
  BuiltinSliceMap map;
  map.kind = BuiltinSliceMap::Kind::Contiguous;
  map.offset = 0;
  map.count = result.storage_size;
  map.result = std::move(result);
  return map;
}

BuiltinSliceMap slice_transpose_map(FunctionArgumentKind value, int64_t rows,
                                    int64_t columns) {
  BuiltinSliceMap map;
  map.kind = BuiltinSliceMap::Kind::Transpose;
  map.count = rows * columns;
  map.result = make_function_shape(value, FunctionContainerKind::Matrix,
                                   FunctionContainerKind::Scalar,
                                   {rows, columns}, map.count);
  return map;
}

bool slice_flat_source(const BuiltinArgumentShape& input) {
  return input.container == FunctionContainerKind::Vector ||
         input.container == FunctionContainerKind::RowVector ||
         input.container == FunctionContainerKind::Matrix ||
         (input.container == FunctionContainerKind::Array &&
          input.array_leaf == FunctionContainerKind::Scalar &&
          input.dimensions.size() == 1);
}

}  // namespace

BuiltinLayout builtin_layout(
    const BuiltinSpec& spec,
    const std::vector<BuiltinArgumentShape>& arguments) {
  if (arguments.size() != spec.arity)
    throw std::invalid_argument("wrong number of arguments");
  for (size_t k = 0; k < arguments.size(); ++k) {
    const BuiltinArgumentShape& argument = arguments[k];
    if (argument.storage_size < 0 ||
        (argument.container == BuiltinContainerKind::Scalar &&
         argument.storage_size != 1))
      throw std::invalid_argument("invalid argument shape");
    if (builtin_argument_is_integer(spec, k) &&
        argument.value != BuiltinArgumentKind::Integer)
      throw std::invalid_argument("expected integer argument " +
                                  std::to_string(k + 1));
  }

  BuiltinLayout layout;
  layout.lanes = arguments[0].storage_size;
  if (spec.shape == BuiltinShapePolicy::Reduction) {
    if (spec.arity != 1)
      throw std::invalid_argument("unsupported reduction arity");
    if (spec.nonempty_input && arguments[0].storage_size == 0)
      throw std::invalid_argument("input must have a positive size");
    layout.lanes = 1;
    return layout;
  }
  if (spec.shape == BuiltinShapePolicy::PairedReduction) {
    if (spec.arity != 1 && spec.arity != 2)
      throw std::invalid_argument("unsupported paired-reduction arity");
    if (spec.arity == 2 &&
        arguments[0].storage_size != arguments[1].storage_size)
      throw std::invalid_argument("arguments must match in size");
    layout.lanes = 1;
    return layout;
  }
  if (spec.shape == BuiltinShapePolicy::Constructor)
    throw std::logic_error("constructor builtins have no lane layout");
  if (spec.shape == BuiltinShapePolicy::Product ||
      spec.shape == BuiltinShapePolicy::Solve)
    throw std::logic_error("product and solve builtins have their own maps");
  if (spec.arity == 1) return layout;
  if (spec.arity != 2) throw std::invalid_argument("unsupported builtin arity");

  const bool scalar0 = arguments[0].container == BuiltinContainerKind::Scalar;
  const bool scalar1 = arguments[1].container == BuiltinContainerKind::Scalar;
  if (!scalar0 && !scalar1) {
    const bool compatible =
        spec.compatibility == BuiltinCompatibilityPolicy::LaneCount
            ? arguments[0].storage_size == arguments[1].storage_size
            : same_builtin_shape(arguments[0], arguments[1]);
    if (!compatible)
      throw std::invalid_argument(spec.compatibility ==
                                          BuiltinCompatibilityPolicy::LaneCount
                                      ? "arguments must match in size"
                                      : "incompatible logical views");
  }

  if (builtin_argument_is_integer(spec, 0) !=
      builtin_argument_is_integer(spec, 1)) {
    const uint8_t real = builtin_argument_is_integer(spec, 0) ? 1 : 0;
    const uint8_t integer = uint8_t{1} - real;
    layout.result_argument =
        arguments[real].container == BuiltinContainerKind::Scalar ? integer
                                                                  : real;
    const BuiltinArgumentShape& real_shape = arguments[real];
    if (arguments[integer].container != BuiltinContainerKind::Scalar) {
      if (real_shape.container == BuiltinContainerKind::Matrix &&
          real_shape.dimensions.size() == 2) {
        layout.integer_matrix_rows = real_shape.dimensions[0];
        layout.integer_matrix_cols = real_shape.dimensions[1];
      } else if (real_shape.container == BuiltinContainerKind::Array &&
                 real_shape.array_leaf == BuiltinContainerKind::Matrix &&
                 real_shape.dimensions.size() >= 2) {
        layout.integer_matrix_rows =
            real_shape.dimensions[real_shape.dimensions.size() - 2];
        layout.integer_matrix_cols = real_shape.dimensions.back();
      }
    }
  } else {
    layout.result_argument = scalar0 && !scalar1 ? 1 : 0;
  }
  layout.lanes = arguments[layout.result_argument].storage_size;
  return layout;
}

const std::vector<const FunctionSpec*>& builtin_specs() {
  return function_specs(FunctionFamily::Builtin);
}

const BuiltinSpec* builtin_spec(std::string_view name, size_t arity) {
  const FunctionSpec* candidate =
      function_spec(name, arity, FunctionFamily::Builtin);
  return candidate == nullptr ? nullptr : candidate->builtin();
}

const BuiltinSpec* shaped_builtin_spec(std::string_view name, size_t arity,
                                       BuiltinShapePolicy shape) {
  const BuiltinSpec* candidate = builtin_spec(name, arity);
  return candidate != nullptr && candidate->shape == shape ? candidate
                                                           : nullptr;
}

const BuiltinSpec* reduction_builtin_spec(std::string_view name, size_t arity) {
  return shaped_builtin_spec(name, arity, BuiltinShapePolicy::Reduction);
}

ConstructorValue evaluate_constructor_builtin(
    const BuiltinSpec& spec, const std::vector<double>& arguments) {
  if (spec.shape != BuiltinShapePolicy::Constructor ||
      spec.constructor == BuiltinConstructor::None)
    throw std::logic_error("builtin is not a constructor");
  if (arguments.size() != spec.arity)
    throw std::invalid_argument("wrong number of arguments");
  const auto as_int = [&](size_t index) {
    return static_cast<int>(std::llround(arguments[index]));
  };
  const int n = as_int(0);

  ConstructorValue result;
  // The Stan Math builders return lazy Eigen expressions; materialize by
  // coefficient so every overload lands in one flat buffer.
  const auto from_vector = [&](const auto& built) {
    const int64_t size = built.size();
    result.values.reserve(static_cast<size_t>(size));
    for (int64_t k = 0; k < size; ++k) result.values.push_back(built(k));
    result.dimensions = {size};
  };
  const auto from_array = [&](const std::vector<double>& built) {
    result.values = built;
    result.dimensions = {static_cast<int64_t>(built.size())};
  };
  const auto from_int_array = [&](const std::vector<int>& built) {
    result.integers = built;
    result.values.reserve(built.size());
    for (const int value : built) result.values.push_back(value);
    result.dimensions = {static_cast<int64_t>(built.size())};
  };
  const bool row =
      spec.constructor_container == FunctionContainerKind::RowVector;
  const bool integer_result = spec.result == FunctionArgumentKind::Integer;

  switch (spec.constructor) {
    case BuiltinConstructor::Zeros:
      if (integer_result)
        from_int_array(stan::math::zeros_int_array(n));
      else if (spec.constructor_container == FunctionContainerKind::Array)
        from_array(stan::math::zeros_array(n));
      else if (row)
        from_vector(stan::math::zeros_row_vector(n));
      else
        from_vector(stan::math::zeros_vector(n));
      return result;
    case BuiltinConstructor::Ones:
      if (integer_result)
        from_int_array(stan::math::ones_int_array(n));
      else if (spec.constructor_container == FunctionContainerKind::Array)
        from_array(stan::math::ones_array(n));
      else if (row)
        from_vector(stan::math::ones_row_vector(n));
      else
        from_vector(stan::math::ones_vector(n));
      return result;
    case BuiltinConstructor::LinSpaced:
      if (integer_result)
        from_int_array(
            stan::math::linspaced_int_array(n, as_int(1), as_int(2)));
      else if (spec.constructor_container == FunctionContainerKind::Array)
        from_array(stan::math::linspaced_array(n, arguments[1], arguments[2]));
      else if (row)
        from_vector(
            stan::math::linspaced_row_vector(n, arguments[1], arguments[2]));
      else
        from_vector(
            stan::math::linspaced_vector(n, arguments[1], arguments[2]));
      return result;
    case BuiltinConstructor::Identity: {
      const Eigen::MatrixXd built = stan::math::identity_matrix(n);
      result.values.assign(built.data(), built.data() + built.size());
      result.dimensions = {built.rows(), built.cols()};
      return result;
    }
    case BuiltinConstructor::OneHot:
      if (integer_result)
        from_int_array(stan::math::one_hot_int_array(n, as_int(1)));
      else if (spec.constructor_container == FunctionContainerKind::Array)
        from_array(stan::math::one_hot_array(n, as_int(1)));
      else if (row)
        from_vector(stan::math::one_hot_row_vector(n, as_int(1)));
      else
        from_vector(stan::math::one_hot_vector(n, as_int(1)));
      return result;
    case BuiltinConstructor::UniformSimplex:
      from_vector(stan::math::uniform_simplex(n));
      return result;
    case BuiltinConstructor::None:
      break;
  }
  throw std::logic_error("unknown constructor builtin");
}

BuiltinSliceMap builtin_slice_map(const BuiltinSpec& spec,
                                  const BuiltinArgumentShape& input,
                                  const std::vector<int64_t>& indexes,
                                  SliceStorageOrder order) {
  if (spec.shape != BuiltinShapePolicy::SliceView ||
      spec.slice == BuiltinSlice::None)
    throw std::logic_error("builtin is not a slice");
  const char* function = slice_function_name(spec.slice);
  if (indexes.size() + 1 != spec.arity)
    throw std::invalid_argument("wrong number of arguments");

  const bool along_outer = spec.slice == BuiltinSlice::Head ||
                           spec.slice == BuiltinSlice::Tail ||
                           spec.slice == BuiltinSlice::Segment ||
                           spec.slice == BuiltinSlice::Reverse;
  if (along_outer) {
    if (input.container != FunctionContainerKind::Vector &&
        input.container != FunctionContainerKind::RowVector &&
        input.container != FunctionContainerKind::Array)
      throw std::invalid_argument(
          "argument is not a vector, row vector, or array");
    const bool array = input.container == FunctionContainerKind::Array;
    const int64_t outer = input.dimensions.front();
    if (spec.slice == BuiltinSlice::Reverse)
      return slice_along_outer(input, 0, outer, true, order);
    if (spec.slice == BuiltinSlice::Segment) {
      // The Eigen overload's checks name both bounds "n"; the std::vector
      // overload names them "i" and "i+n-1". Both throw std::domain_error.
      const int64_t from = indexes[0], count = indexes[1];
      check_segment_bound(array ? "i" : "n", from, outer);
      if (count != 0)
        check_segment_bound(array ? "i+n-1" : "n", from + count - 1, outer);
      return slice_along_outer(input, from - 1, count, false, order);
    }
    const int64_t count = indexes[0];
    if (count != 0)
      check_slice_index(function, count, outer,
                        array ? " for n" : " for size of n");
    const int64_t start = spec.slice == BuiltinSlice::Head ? 0 : outer - count;
    return slice_along_outer(input, start, count, false, order);
  }

  if (spec.slice == BuiltinSlice::Transpose) {
    if (input.container == FunctionContainerKind::Vector ||
        input.container == FunctionContainerKind::RowVector)
      return slice_identity_map(
          slice_vector_shape(input.value,
                             input.container == FunctionContainerKind::Vector
                                 ? FunctionContainerKind::RowVector
                                 : FunctionContainerKind::Vector,
                             input.storage_size));
    if (input.container != FunctionContainerKind::Matrix)
      throw std::invalid_argument("needs a vector, row vector, or matrix");
    return slice_transpose_map(input.value, input.dimensions[1],
                               input.dimensions[0]);
  }
  if (spec.slice == BuiltinSlice::ToVector ||
      spec.slice == BuiltinSlice::ToRowVector) {
    // Column-major flattening is the identity on every backend's storage.
    if (!slice_flat_source(input))
      throw std::invalid_argument(
          "argument is not a vector, row vector, matrix, or "
          "one-dimensional array");
    return slice_identity_map(slice_vector_shape(
        FunctionArgumentKind::Real,
        spec.slice == BuiltinSlice::ToVector ? FunctionContainerKind::Vector
                                             : FunctionContainerKind::RowVector,
        input.storage_size));
  }
  if (spec.slice == BuiltinSlice::ToMatrix) {
    if (spec.arity == 1) {
      if (input.container == FunctionContainerKind::Matrix)
        return slice_identity_map(input);
      if (input.container == FunctionContainerKind::Vector)
        return slice_identity_map(make_function_shape(
            FunctionArgumentKind::Real, FunctionContainerKind::Matrix,
            FunctionContainerKind::Scalar, {input.storage_size, 1},
            input.storage_size));
      if (input.container == FunctionContainerKind::RowVector)
        return slice_identity_map(make_function_shape(
            FunctionArgumentKind::Real, FunctionContainerKind::Matrix,
            FunctionContainerKind::Scalar, {1, input.storage_size},
            input.storage_size));
      // Two logical axes over scalar cells: array[,] and array[] row_vector
      // per the language, plus array[] vector for legacy hand-built MIR
      // (both prior backends accepted it with these same semantics).
      const bool matrix_shaped_array =
          input.container == FunctionContainerKind::Array &&
          input.dimensions.size() == 2 &&
          input.array_leaf != FunctionContainerKind::Matrix;
      if (!matrix_shaped_array)
        throw std::invalid_argument("cannot convert this shape to a matrix");
      // Stan Math builds an empty matrix from an empty outer array,
      // regardless of the declared suffix extent.
      const int64_t rows = input.dimensions[0];
      const int64_t columns = rows == 0 ? 0 : input.dimensions[1];
      // The array's semantic cell (i, j) already sits at the column-major
      // matrix position under first-index-fast storage; outer-major storage
      // is the row-major spelling, i.e. the transpose of that.
      if (order == SliceStorageOrder::FirstIndexFast)
        return slice_identity_map(make_function_shape(
            FunctionArgumentKind::Real, FunctionContainerKind::Matrix,
            FunctionContainerKind::Scalar, {rows, columns},
            input.storage_size));
      return slice_transpose_map(FunctionArgumentKind::Real, rows, columns);
    }
    if (!slice_flat_source(input))
      throw std::invalid_argument(
          "argument is not a vector, row vector, matrix, or "
          "one-dimensional array");
    const bool array = input.container == FunctionContainerKind::Array;
    const int64_t rows = indexes[0], columns = indexes[1];
    const bool column_major = spec.arity == 3 || indexes[2] != 0;
    // Stan Math leaves negative extents whose product still matches to
    // Eigen's undefined behavior; reject them as a size domain error.
    stan::math::check_nonnegative(function, "rows", rows);
    stan::math::check_nonnegative(function, "columns", columns);
    if (!column_major && array)
      stan::math::check_size_match("to_matrix", "rows * columns",
                                   rows * columns, "matrix size",
                                   input.storage_size);
    else
      stan::math::check_size_match(
          array ? "to_matrix(array)" : "to_matrix(matrix)", "rows * columns",
          rows * columns, "vector size", input.storage_size);
    if (column_major)
      return slice_identity_map(make_function_shape(
          FunctionArgumentKind::Real, FunctionContainerKind::Matrix,
          FunctionContainerKind::Scalar, {rows, columns}, input.storage_size));
    return slice_transpose_map(FunctionArgumentKind::Real, rows, columns);
  }
  if (spec.slice == BuiltinSlice::ToArray1d) {
    const auto result_shape = [&](FunctionArgumentKind value) {
      return make_function_shape(value, FunctionContainerKind::Array,
                                 FunctionContainerKind::Scalar,
                                 {input.storage_size}, input.storage_size);
    };
    if (input.container == FunctionContainerKind::Vector ||
        input.container == FunctionContainerKind::RowVector ||
        input.container == FunctionContainerKind::Matrix)
      return slice_identity_map(result_shape(FunctionArgumentKind::Real));
    if (input.container != FunctionContainerKind::Array ||
        input.array_leaf != FunctionContainerKind::Scalar)
      throw std::invalid_argument(
          "argument is not a vector, row vector, matrix, or scalar array");
    // Stan flattens arrays row-major (leftmost index slowest). Outer-major
    // storage already is that order; first-index-fast storage needs the
    // mixed-radix reversal, which is what makes the historical identity
    // shortcut on this storage wrong for rank two and deeper.
    if (order == SliceStorageOrder::OuterMajor || input.dimensions.size() == 1)
      return slice_identity_map(result_shape(input.value));
    BuiltinSliceMap map;
    map.kind = BuiltinSliceMap::Kind::Gather;
    map.count = input.storage_size;
    map.result = result_shape(input.value);
    map.gather.reserve(static_cast<size_t>(map.count));
    const std::vector<int64_t>& extents = input.dimensions;
    std::vector<int64_t> stride(extents.size(), 1);
    for (size_t d = 1; d < extents.size(); ++d)
      stride[d] = stride[d - 1] * extents[d - 1];
    std::vector<int64_t> coordinate(extents.size(), 0);
    for (int64_t cell = 0; cell < map.count; ++cell) {
      int64_t source = 0;
      for (size_t d = 0; d < extents.size(); ++d)
        source += coordinate[d] * stride[d];
      map.gather.push_back(source);
      for (size_t d = extents.size(); d-- > 0;) {
        if (++coordinate[d] < extents[d]) break;
        coordinate[d] = 0;
      }
    }
    return map;
  }

  if (spec.slice == BuiltinSlice::RepVector ||
      spec.slice == BuiltinSlice::RepRowVector) {
    if (input.container != FunctionContainerKind::Scalar)
      throw std::invalid_argument("needs a scalar fill value");
    const int64_t count = indexes[0];
    // Stan Math's rep_row_vector reuses rep_vector's own check name.
    stan::math::check_nonnegative("rep_vector", "n", count);
    BuiltinSliceMap map;
    map.kind = BuiltinSliceMap::Kind::Strided;
    map.offset = 0;
    map.stride = 0;
    map.count = count;
    map.result = slice_vector_shape(FunctionArgumentKind::Real,
                                    spec.slice == BuiltinSlice::RepVector
                                        ? FunctionContainerKind::Vector
                                        : FunctionContainerKind::RowVector,
                                    count);
    return map;
  }
  if (spec.slice == BuiltinSlice::RepMatrix) {
    BuiltinSliceMap map;
    if (spec.arity == 3) {
      if (input.container != FunctionContainerKind::Scalar)
        throw std::invalid_argument("needs a scalar fill value");
      const int64_t rows = indexes[0], columns = indexes[1];
      stan::math::check_nonnegative(function, "rows", rows);
      stan::math::check_nonnegative(function, "cols", columns);
      map.kind = BuiltinSliceMap::Kind::Strided;
      map.offset = 0;
      map.stride = 0;
      map.count = checked_slice_product(rows, columns);
      map.result = make_function_shape(
          FunctionArgumentKind::Real, FunctionContainerKind::Matrix,
          FunctionContainerKind::Scalar, {rows, columns}, map.count);
      return map;
    }
    const int64_t count = indexes[0];
    const bool by_rows = input.container == FunctionContainerKind::RowVector;
    if (!by_rows && input.container != FunctionContainerKind::Vector)
      throw std::invalid_argument("needs a vector or row vector");
    stan::math::check_nonnegative(function, by_rows ? "rows" : "cols", count);
    const int64_t length = input.storage_size;
    const int64_t rows = by_rows ? count : length;
    const int64_t columns = by_rows ? length : count;
    map.kind = BuiltinSliceMap::Kind::Gather;
    map.count = checked_slice_product(rows, columns);
    map.result = make_function_shape(
        FunctionArgumentKind::Real, FunctionContainerKind::Matrix,
        FunctionContainerKind::Scalar, {rows, columns}, map.count);
    map.gather.reserve(static_cast<size_t>(map.count));
    // Column-major result: a column vector repeats across columns (source
    // cell k % rows), a row vector repeats down each column (cell k / rows).
    for (int64_t cell = 0; cell < map.count; ++cell)
      map.gather.push_back(by_rows ? cell / rows : cell % rows);
    return map;
  }
  if (spec.slice == BuiltinSlice::RepArray) {
    static constexpr const char* kExtentNames[3][3] = {
        {"n", nullptr, nullptr},
        {"rows", "cols", nullptr},
        {"shelves", "rows", "cols"}};
    const size_t extents = indexes.size();
    if (extents < 1 || extents > 3)
      throw std::invalid_argument("unsupported rep_array arity");
    int64_t copies = 1;
    std::vector<int64_t> dimensions;
    dimensions.reserve(extents + input.dimensions.size());
    for (size_t k = 0; k < extents; ++k) {
      stan::math::check_nonnegative(function, kExtentNames[extents - 1][k],
                                    indexes[k]);
      copies = checked_slice_product(copies, indexes[k]);
      dimensions.push_back(indexes[k]);
    }
    if (input.container != FunctionContainerKind::Scalar)
      dimensions.insert(dimensions.end(), input.dimensions.begin(),
                        input.dimensions.end());
    const FunctionContainerKind leaf =
        input.container == FunctionContainerKind::Array ? input.array_leaf
                                                        : input.container;
    const int64_t width = input.storage_size;
    BuiltinSliceMap map;
    map.count = checked_slice_product(copies, width);
    map.result = make_function_shape(input.value, FunctionContainerKind::Array,
                                     leaf, std::move(dimensions), map.count);
    map.kind = BuiltinSliceMap::Kind::Gather;
    map.gather.reserve(static_cast<size_t>(map.count));
    // The element tiles as whole chunks under OuterMajor storage; under
    // FirstIndexFast the prepended axes vary fastest, so each element cell
    // spreads `copies` apart.
    for (int64_t cell = 0; cell < map.count; ++cell)
      map.gather.push_back(order == SliceStorageOrder::OuterMajor
                               ? cell % width
                               : cell / copies);
    return map;
  }

  if (input.container != FunctionContainerKind::Matrix)
    throw std::invalid_argument("needs a matrix");
  const int64_t rows = input.dimensions[0], cols = input.dimensions[1];
  BuiltinSliceMap map;
  switch (spec.slice) {
    case BuiltinSlice::Col: {
      const int64_t j = indexes[0];
      check_slice_index(function, j, cols, " for columns of j");
      map.kind = BuiltinSliceMap::Kind::Contiguous;
      map.offset = (j - 1) * rows;
      map.count = rows;
      map.result =
          slice_vector_shape(input.value, FunctionContainerKind::Vector, rows);
      return map;
    }
    case BuiltinSlice::Row: {
      const int64_t i = indexes[0];
      check_slice_index(function, i, rows, " for rows of i");
      map.kind = BuiltinSliceMap::Kind::Strided;
      map.offset = i - 1;
      map.stride = rows;
      map.count = cols;
      map.result = slice_vector_shape(input.value,
                                      FunctionContainerKind::RowVector, cols);
      return map;
    }
    case BuiltinSlice::SubCol: {
      const int64_t i = indexes[0], j = indexes[1], count = indexes[2];
      check_slice_index(function, i, rows, " for rows of i");
      // Stan Math leaves a negative count to Eigen's undefined behavior;
      // reject it through the same domain_error a size check raises.
      stan::math::check_nonnegative(function, "nrows", count);
      if (count > 0)
        check_slice_index(function, i + count - 1, rows,
                          " for rows of i+nrows-1");
      check_slice_index(function, j, cols, " for columns of j");
      map.kind = BuiltinSliceMap::Kind::Contiguous;
      map.offset = (j - 1) * rows + i - 1;
      map.count = count;
      map.result =
          slice_vector_shape(input.value, FunctionContainerKind::Vector, count);
      return map;
    }
    case BuiltinSlice::Block: {
      const int64_t i = indexes[0], j = indexes[1];
      const int64_t height = indexes[2], columns = indexes[3];
      check_slice_index(function, i, rows, " for rows of i");
      check_slice_index(function, i + height - 1, rows,
                        " for rows of i+nrows-1");
      check_slice_index(function, j, cols, " for columns of j");
      check_slice_index(function, j + columns - 1, cols,
                        " for columns of j+ncols-1");
      map.kind = BuiltinSliceMap::Kind::Gather;
      map.count = height * columns;
      map.gather.reserve(static_cast<size_t>(map.count));
      for (int64_t c = 0; c < columns; ++c)
        for (int64_t k = 0; k < height; ++k)
          map.gather.push_back((j - 1 + c) * rows + i - 1 + k);
      map.result = make_function_shape(
          input.value, FunctionContainerKind::Matrix,
          FunctionContainerKind::Scalar, {height, columns}, map.count);
      return map;
    }
    case BuiltinSlice::Diagonal: {
      map.kind = BuiltinSliceMap::Kind::Strided;
      map.offset = 0;
      map.stride = rows + 1;
      map.count = std::min(rows, cols);
      map.result = slice_vector_shape(input.value,
                                      FunctionContainerKind::Vector, map.count);
      return map;
    }
    default:
      break;
  }
  throw std::logic_error("unknown slice builtin");
}

BuiltinSliceMap builtin_append_map(const BuiltinSpec& spec,
                                   const BuiltinArgumentShape& lhs,
                                   const BuiltinArgumentShape& rhs,
                                   SliceStorageOrder order) {
  if (spec.shape != BuiltinShapePolicy::SliceView ||
      !builtin_slice_is_append(spec.slice))
    throw std::logic_error("builtin is not an append");
  const char* function = slice_function_name(spec.slice);
  const int64_t left = lhs.storage_size;
  const int64_t total = left + rhs.storage_size;

  if (spec.slice == BuiltinSlice::AppendArray) {
    if (lhs.container != FunctionContainerKind::Array ||
        rhs.container != FunctionContainerKind::Array ||
        lhs.dimensions.size() != rhs.dimensions.size() ||
        lhs.array_leaf != rhs.array_leaf)
      throw std::invalid_argument("arguments must be arrays of the same rank");
    const int64_t left_outer = lhs.dimensions[0];
    const int64_t right_outer = rhs.dimensions[0];
    // Stan Math compares element geometry only when both sides contain an
    // element; an empty side contributes no value whose shape could
    // disagree, and the nonempty side supplies the result's suffix.
    if (left_outer != 0 && right_outer != 0)
      for (size_t d = 1; d < lhs.dimensions.size(); ++d)
        stan::math::check_size_match(function, "shape of x", lhs.dimensions[d],
                                     "shape of y", rhs.dimensions[d]);
    std::vector<int64_t> dimensions =
        left_outer == 0 && right_outer != 0 ? rhs.dimensions : lhs.dimensions;
    dimensions[0] = left_outer + right_outer;
    const FunctionArgumentKind value =
        lhs.value == FunctionArgumentKind::Integer &&
                rhs.value == FunctionArgumentKind::Integer
            ? FunctionArgumentKind::Integer
            : FunctionArgumentKind::Real;
    BuiltinSliceMap map;
    map.count = total;
    map.result =
        make_function_shape(value, FunctionContainerKind::Array, lhs.array_leaf,
                            std::move(dimensions), total);
    int64_t width = 1;
    for (size_t d = 1; d < map.result.dimensions.size(); ++d)
      width *= map.result.dimensions[d];
    if (order == SliceStorageOrder::OuterMajor || width <= 1) {
      map.kind = BuiltinSliceMap::Kind::Contiguous;
      map.offset = 0;
      return map;
    }
    map.kind = BuiltinSliceMap::Kind::Gather;
    map.gather.reserve(static_cast<size_t>(total));
    for (int64_t suffix = 0; suffix < width; ++suffix) {
      for (int64_t k = 0; k < left_outer; ++k)
        map.gather.push_back(suffix * left_outer + k);
      for (int64_t k = 0; k < right_outer; ++k)
        map.gather.push_back(left + suffix * right_outer + k);
    }
    return map;
  }

  if (lhs.container == FunctionContainerKind::Scalar &&
      rhs.container == FunctionContainerKind::Scalar)
    throw std::invalid_argument(
        "needs a vector, row vector, or matrix argument");
  const AppendBlock a = append_block(lhs);
  const AppendBlock b = append_block(rhs);
  BuiltinSliceMap map;
  map.count = total;
  if (spec.slice == BuiltinSlice::AppendCol) {
    stan::math::check_size_match(function, "rows of A", a.rows, "rows of B",
                                 b.rows);
    // Column-major storage makes adding columns a plain concatenation.
    map.kind = BuiltinSliceMap::Kind::Contiguous;
    map.offset = 0;
    const bool row_result =
        (lhs.container == FunctionContainerKind::Scalar ||
         lhs.container == FunctionContainerKind::RowVector) &&
        (rhs.container == FunctionContainerKind::Scalar ||
         rhs.container == FunctionContainerKind::RowVector);
    map.result =
        row_result
            ? slice_vector_shape(FunctionArgumentKind::Real,
                                 FunctionContainerKind::RowVector, total)
            : make_function_shape(FunctionArgumentKind::Real,
                                  FunctionContainerKind::Matrix,
                                  FunctionContainerKind::Scalar,
                                  {a.rows, a.columns + b.columns}, total);
    return map;
  }
  stan::math::check_size_match(function, "columns of A", a.columns,
                               "columns of B", b.columns);
  const bool vector_result = (lhs.container == FunctionContainerKind::Scalar ||
                              lhs.container == FunctionContainerKind::Vector) &&
                             (rhs.container == FunctionContainerKind::Scalar ||
                              rhs.container == FunctionContainerKind::Vector);
  map.result = vector_result
                   ? slice_vector_shape(FunctionArgumentKind::Real,
                                        FunctionContainerKind::Vector, total)
                   : make_function_shape(FunctionArgumentKind::Real,
                                         FunctionContainerKind::Matrix,
                                         FunctionContainerKind::Scalar,
                                         {a.rows + b.rows, a.columns}, total);
  if (a.columns <= 1) {
    // One column: stacking rows is itself a plain concatenation.
    map.kind = BuiltinSliceMap::Kind::Contiguous;
    map.offset = 0;
    return map;
  }
  // Column-major operands interleave one column at a time.
  map.kind = BuiltinSliceMap::Kind::Gather;
  map.gather.reserve(static_cast<size_t>(total));
  for (int64_t column = 0; column < a.columns; ++column) {
    for (int64_t k = 0; k < a.rows; ++k)
      map.gather.push_back(column * a.rows + k);
    for (int64_t k = 0; k < b.rows; ++k)
      map.gather.push_back(left + column * b.rows + k);
  }
  return map;
}

BuiltinGroupedDotMap builtin_grouped_dot_map(const BuiltinSpec& spec,
                                             const BuiltinArgumentShape& lhs,
                                             const BuiltinArgumentShape& rhs) {
  if (spec.shape != BuiltinShapePolicy::GroupedReduction ||
      (spec.slice != BuiltinSlice::ColumnsDot &&
       spec.slice != BuiltinSlice::RowsDot))
    throw std::invalid_argument("descriptor is not a grouped reduction");
  const auto eigen_block = [](const BuiltinArgumentShape& shape) {
    if (shape.container == FunctionContainerKind::Scalar ||
        shape.container == FunctionContainerKind::Array)
      throw std::invalid_argument(
          "argument must be a matrix, vector, or row vector");
    return append_block(shape);
  };
  const AppendBlock a = eigen_block(lhs);
  const AppendBlock b = eigen_block(rhs);
  if (spec.arity == 2 && (a.rows != b.rows || a.columns != b.columns)) {
    // Stan Math's own size validation. The AoS reverse-mode overload the
    // model block instantiates passes "check_matching_dims" itself as
    // columns_dot_product's function name; rows_dot_product uses its own.
    const Eigen::MatrixXd shape_a(a.rows, a.columns);
    const Eigen::MatrixXd shape_b(b.rows, b.columns);
    stan::math::check_matching_dims(spec.slice == BuiltinSlice::ColumnsDot
                                        ? "check_matching_dims"
                                        : "rows_dot_product",
                                    "v1", shape_a, "v2", shape_b);
    throw std::invalid_argument("arguments must have matching dimensions");
  }
  BuiltinGroupedDotMap map;
  if (spec.slice == BuiltinSlice::ColumnsDot) {
    map.groups = a.columns;
    map.width = a.rows;
    map.group_stride = a.rows;
    map.cell_stride = 1;
    map.result =
        slice_vector_shape(FunctionArgumentKind::Real,
                           FunctionContainerKind::RowVector, map.groups);
  } else {
    map.groups = a.rows;
    map.width = a.columns;
    map.group_stride = 1;
    map.cell_stride = a.rows;
    map.result = slice_vector_shape(FunctionArgumentKind::Real,
                                    FunctionContainerKind::Vector, map.groups);
  }
  return map;
}

BuiltinMatrixMap builtin_matrix_map(
    const BuiltinSpec& spec, const std::vector<BuiltinArgumentShape>& shapes) {
  if (spec.shape != BuiltinShapePolicy::MatrixOp ||
      spec.matrix_op == BuiltinMatrixOp::None || shapes.size() != spec.arity)
    throw std::invalid_argument("descriptor is not a matrix operation");
  const BuiltinArgumentShape& a = shapes[0];
  const auto matrix_shape = [](int64_t rows, int64_t columns) {
    return make_function_shape(FunctionArgumentKind::Real,
                               FunctionContainerKind::Matrix,
                               FunctionContainerKind::Scalar, {rows, columns},
                               checked_slice_product(rows, columns));
  };
  const auto require_matrix = [&](const BuiltinArgumentShape& shape) {
    if (shape.container != FunctionContainerKind::Matrix)
      throw std::invalid_argument("needs a matrix");
    return AppendBlock{shape.dimensions[0], shape.dimensions[1]};
  };
  const auto require_square = [&](const BuiltinArgumentShape& shape) {
    const AppendBlock block = require_matrix(shape);
    if (block.rows != block.columns)
      throw std::invalid_argument("needs a square matrix");
    return block.rows;
  };
  BuiltinMatrixMap map;
  switch (spec.matrix_op) {
    case BuiltinMatrixOp::CholeskyDecompose:
    case BuiltinMatrixOp::MatrixExp:
    case BuiltinMatrixOp::Inverse:
    case BuiltinMatrixOp::InverseSpd:
    case BuiltinMatrixOp::EigenvectorsSym: {
      const int64_t n = require_square(a);
      map.idata = {n};
      map.result = matrix_shape(n, n);
      if (spec.matrix_op == BuiltinMatrixOp::InverseSpd) map.active_variant = 1;
      return map;
    }
    case BuiltinMatrixOp::LogDeterminant: {
      map.idata = {require_square(a)};
      map.result = make_function_shape(FunctionArgumentKind::Real,
                                       FunctionContainerKind::Scalar,
                                       FunctionContainerKind::Scalar, {}, 1);
      return map;
    }
    case BuiltinMatrixOp::EigenvaluesSym: {
      const int64_t n = require_square(a);
      map.idata = {n};
      map.result = slice_vector_shape(FunctionArgumentKind::Real,
                                      FunctionContainerKind::Vector, n);
      return map;
    }
    case BuiltinMatrixOp::Crossprod:
    case BuiltinMatrixOp::MultiplyLowerTriSelfTranspose: {
      const AppendBlock block = require_matrix(a);
      map.idata = {block.rows, block.columns};
      const int64_t n = spec.matrix_op == BuiltinMatrixOp::Crossprod
                            ? block.columns
                            : block.rows;
      map.result = matrix_shape(n, n);
      map.active_variant = 1;
      return map;
    }
    case BuiltinMatrixOp::DiagMatrix: {
      if (a.container != FunctionContainerKind::Vector &&
          a.container != FunctionContainerKind::RowVector)
        throw std::invalid_argument("needs a vector");
      map.result = matrix_shape(a.storage_size, a.storage_size);
      return map;
    }
    case BuiltinMatrixOp::AddDiag: {
      const AppendBlock block = require_matrix(a);
      const BuiltinArgumentShape& d = shapes[1];
      const bool scalar = d.container == FunctionContainerKind::Scalar;
      if (!scalar && d.container != FunctionContainerKind::Vector &&
          d.container != FunctionContainerKind::RowVector)
        throw std::invalid_argument("diagonal must be a scalar or vector");
      if (!scalar && d.storage_size != std::min(block.rows, block.columns))
        throw std::invalid_argument("diagonal length mismatch");
      map.idata = {block.rows, block.columns};
      map.variant = scalar ? 1 : 0;
      map.result = matrix_shape(block.rows, block.columns);
      return map;
    }
    case BuiltinMatrixOp::QuadForm:
    case BuiltinMatrixOp::QuadFormSym: {
      const int64_t n = require_square(a);
      const BuiltinArgumentShape& b = shapes[1];
      const bool b_matrix = b.container == FunctionContainerKind::Matrix;
      if (!b_matrix && b.container != FunctionContainerKind::Vector)
        throw std::invalid_argument(
            "second argument is not a matrix or vector");
      const int64_t rb = b_matrix ? b.dimensions[0] : b.storage_size;
      const int64_t m = b_matrix ? b.dimensions[1] : 1;
      if (rb != n)
        throw std::invalid_argument(
            "inner dimension mismatch (" + std::to_string(n) + "x" +
            std::to_string(n) + " against " + std::to_string(rb) + ")");
      map.idata = {n, m};
      // Bit 0 is the operand shape; the backend's active bit picks
      // stan-math's other association of the same vector product.
      map.variant = b_matrix ? 0 : 1;
      map.active_variant = 2;
      map.result =
          b_matrix ? matrix_shape(m, m)
                   : make_function_shape(FunctionArgumentKind::Real,
                                         FunctionContainerKind::Scalar,
                                         FunctionContainerKind::Scalar, {}, 1);
      return map;
    }
    case BuiltinMatrixOp::None:
      break;
  }
  throw std::invalid_argument("descriptor is not a matrix operation");
}

BuiltinProductMap builtin_product_map(const BuiltinArgumentShape& a,
                                      const BuiltinArgumentShape& b) {
  using Kind = FunctionContainerKind;
  const auto shaped = [](FunctionContainerKind container,
                         std::vector<int64_t> dimensions, int64_t size) {
    return make_function_shape(FunctionArgumentKind::Real, container,
                               FunctionContainerKind::Scalar,
                               std::move(dimensions), size);
  };
  const auto mismatch = [](int64_t ra, int64_t ca, int64_t rb, int64_t cb) {
    throw std::invalid_argument("inner dimension mismatch (" +
                                std::to_string(ra) + "x" + std::to_string(ca) +
                                " times " + std::to_string(rb) + "x" +
                                std::to_string(cb) + ")");
  };
  BuiltinProductMap map;
  if (a.container == Kind::Scalar || b.container == Kind::Scalar) {
    map.kind = BuiltinProductMap::Kind::ScalarScale;
    map.result = a.container == Kind::Scalar ? b : a;
    return map;
  }
  if (a.container == Kind::Matrix && b.container == Kind::Vector) {
    if (a.dimensions[1] != b.storage_size)
      mismatch(a.dimensions[0], a.dimensions[1], b.storage_size, 1);
    map.kind = BuiltinProductMap::Kind::MatVec;
    map.m = a.dimensions[0];
    map.k = a.dimensions[1];
    map.n = 1;
    map.result = shaped(Kind::Vector, {map.m}, map.m);
    return map;
  }
  if (a.container == Kind::Matrix && b.container == Kind::Matrix) {
    if (a.dimensions[1] != b.dimensions[0])
      mismatch(a.dimensions[0], a.dimensions[1], b.dimensions[0],
               b.dimensions[1]);
    map.kind = BuiltinProductMap::Kind::Gemm;
    map.m = a.dimensions[0];
    map.k = a.dimensions[1];
    map.n = b.dimensions[1];
    map.result = shaped(Kind::Matrix, {map.m, map.n},
                        checked_slice_product(map.m, map.n));
    return map;
  }
  if (a.container == Kind::RowVector && b.container == Kind::Matrix) {
    if (a.storage_size != b.dimensions[0])
      mismatch(1, a.storage_size, b.dimensions[0], b.dimensions[1]);
    map.kind = BuiltinProductMap::Kind::Gemm;
    map.m = 1;
    map.k = a.storage_size;
    map.n = b.dimensions[1];
    map.result = shaped(Kind::RowVector, {map.n}, map.n);
    return map;
  }
  if (a.container == Kind::Vector && b.container == Kind::RowVector) {
    map.kind = BuiltinProductMap::Kind::Outer;
    map.m = a.storage_size;
    map.k = 1;
    map.n = b.storage_size;
    map.result = shaped(Kind::Matrix, {map.m, map.n},
                        checked_slice_product(map.m, map.n));
    return map;
  }
  if (a.container == Kind::RowVector && b.container == Kind::Vector) {
    if (a.storage_size != b.storage_size)
      mismatch(1, a.storage_size, b.storage_size, 1);
    map.kind = BuiltinProductMap::Kind::Inner;
    map.m = 1;
    map.k = a.storage_size;
    map.n = 1;
    map.result = shaped(Kind::Scalar, {}, 1);
    return map;
  }
  throw std::invalid_argument(
      "needs scalar, vector, row_vector, or matrix operands");
}

BuiltinSolveMap builtin_solve_map(const BuiltinSpec& spec,
                                  const BuiltinArgumentShape& a,
                                  const BuiltinArgumentShape& b) {
  using Kind = FunctionContainerKind;
  if (spec.shape != BuiltinShapePolicy::Solve ||
      spec.solve == BuiltinSolveKind::None)
    throw std::invalid_argument("descriptor is not a solve");
  const BuiltinArgumentShape& divisor = spec.solve_left ? a : b;
  const BuiltinArgumentShape& dividend = spec.solve_left ? b : a;
  if (divisor.container != Kind::Matrix ||
      divisor.dimensions[0] != divisor.dimensions[1])
    throw std::invalid_argument("requires a square matrix divisor");
  BuiltinSolveMap map;
  map.order = divisor.dimensions[0];
  if (spec.solve_left) {
    // divisor \ dividend: the dividend is a vector or matrix with `order`
    // rows and keeps its shape.
    if (dividend.container == Kind::Vector) {
      if (dividend.storage_size != map.order)
        throw std::invalid_argument("right-hand side size mismatch");
      map.columns = 1;
    } else if (dividend.container == Kind::Matrix) {
      if (dividend.dimensions[0] != map.order)
        throw std::invalid_argument("right-hand side size mismatch");
      map.columns = dividend.dimensions[1];
    } else {
      throw std::invalid_argument("needs a vector or matrix dividend");
    }
  } else {
    // dividend / divisor: the dividend is a row_vector or matrix with
    // `order` columns and keeps its shape.
    if (dividend.container == Kind::RowVector) {
      if (dividend.storage_size != map.order)
        throw std::invalid_argument("left-hand side size mismatch");
      map.columns = 1;
    } else if (dividend.container == Kind::Matrix) {
      if (dividend.dimensions[1] != map.order)
        throw std::invalid_argument("left-hand side size mismatch");
      map.columns = dividend.dimensions[0];
    } else {
      throw std::invalid_argument("needs a row_vector or matrix dividend");
    }
  }
  map.result = dividend;
  return map;
}

std::vector<int64_t> builtin_shape_query(const BuiltinSpec& spec,
                                         const BuiltinArgumentShape& shape) {
  if (spec.shape != BuiltinShapePolicy::ShapeQuery ||
      spec.shape_query == BuiltinShapeQueryKind::None)
    throw std::invalid_argument("descriptor is not a shape query");
  switch (spec.shape_query) {
    case BuiltinShapeQueryKind::Dims: {
      // Stan Math's dims() pushes rows and columns for any Eigen value, so
      // a vector leaf contributes {n, 1} and a row vector {1, n} -- two
      // entries, not the one its storage rank suggests (CmdStan prints
      // dims(vector[5]) as [5,1]).
      const FunctionContainerKind leaf =
          shape.container == FunctionContainerKind::Array ? shape.array_leaf
                                                          : shape.container;
      const size_t leaf_rank = function_leaf_rank(leaf);
      std::vector<int64_t> extents(
          shape.dimensions.begin(),
          shape.dimensions.end() - (std::ptrdiff_t)leaf_rank);
      const auto* leaf_extents =
          shape.dimensions.data() + (shape.dimensions.size() - leaf_rank);
      switch (leaf) {
        case FunctionContainerKind::Vector:
          extents.push_back(leaf_extents[0]);
          extents.push_back(1);
          break;
        case FunctionContainerKind::RowVector:
          extents.push_back(1);
          extents.push_back(leaf_extents[0]);
          break;
        case FunctionContainerKind::Matrix:
          extents.push_back(leaf_extents[0]);
          extents.push_back(leaf_extents[1]);
          break;
        default:
          break;
      }
      return extents;
    }
    case BuiltinShapeQueryKind::NumElements:
      return {shape.storage_size};
    case BuiltinShapeQueryKind::Size:
      return {shape.container == FunctionContainerKind::Array
                  ? shape.dimensions.front()
                  : shape.storage_size};
    case BuiltinShapeQueryKind::Rows:
    case BuiltinShapeQueryKind::Cols: {
      const bool rows = spec.shape_query == BuiltinShapeQueryKind::Rows;
      switch (shape.container) {
        case FunctionContainerKind::Matrix:
          return {shape.dimensions[rows ? 0 : 1]};
        case FunctionContainerKind::Vector:
          return {rows ? shape.storage_size : 1};
        case FunctionContainerKind::RowVector:
          return {rows ? 1 : shape.storage_size};
        case FunctionContainerKind::Array:
          throw std::invalid_argument("is undefined for an array value");
        case FunctionContainerKind::Scalar:
          break;
      }
      throw std::invalid_argument("needs a matrix or vector");
    }
    case BuiltinShapeQueryKind::None:
      break;
  }
  throw std::invalid_argument("descriptor is not a shape query");
}

BuiltinIndexMap builtin_index_map(
    const BuiltinArgumentShape& input,
    const std::vector<std::vector<int64_t>>& positions,
    const std::vector<bool>& collapse, SliceStorageOrder order) {
  const FunctionContainerKind leaf =
      input.container == FunctionContainerKind::Array ? input.array_leaf
                                                      : input.container;
  return builtin_index_map(input.dimensions, function_leaf_rank(leaf),
                           positions, collapse, order);
}

BuiltinIndexMap builtin_index_map(
    const std::vector<int64_t>& extents, size_t leaf_rank,
    const std::vector<std::vector<int64_t>>& positions,
    const std::vector<bool>& collapse, SliceStorageOrder order) {
  const size_t rank = extents.size();
  if (positions.size() != collapse.size() || positions.size() > rank ||
      leaf_rank > rank)
    throw std::invalid_argument("malformed index selection");
  const size_t array_rank = rank - leaf_rank;

  std::vector<std::vector<int64_t>> selected(positions);
  std::vector<bool> dropped(collapse);
  selected.reserve(rank);
  dropped.reserve(rank);
  for (size_t d = selected.size(); d < rank; ++d) {
    selected.emplace_back();
    selected.back().reserve(static_cast<size_t>(extents[d]));
    for (int64_t k = 0; k < extents[d]; ++k) selected.back().push_back(k);
    dropped.push_back(false);
  }

  // Source strides. FirstIndexFast is Fortran over every logical axis;
  // OuterMajor keeps each leaf contiguous (column-major) with the array
  // axes row-major above it.
  std::vector<int64_t> stride(rank, 1);
  {
    int64_t running = 1;
    if (order == SliceStorageOrder::FirstIndexFast) {
      for (size_t d = 0; d < rank; ++d) {
        stride[d] = running;
        running = checked_slice_product(running, extents[d]);
      }
    } else {
      for (size_t d = array_rank; d < rank; ++d) {
        stride[d] = running;
        running = checked_slice_product(running, extents[d]);
      }
      for (size_t d = array_rank; d-- > 0;) {
        stride[d] = running;
        running = checked_slice_product(running, extents[d]);
      }
    }
  }

  // Result cells are enumerated in the same convention, over the surviving
  // axes' selections (a collapsed axis contributes one fixed position).
  std::vector<size_t> axis_order;
  axis_order.reserve(rank);
  if (order == SliceStorageOrder::FirstIndexFast) {
    for (size_t d = 0; d < rank; ++d) axis_order.push_back(d);
  } else {
    for (size_t d = array_rank; d < rank; ++d) axis_order.push_back(d);
    for (size_t d = array_rank; d-- > 0;) axis_order.push_back(d);
  }

  BuiltinIndexMap map;
  int64_t total = 1;
  for (size_t d = 0; d < rank; ++d) {
    if (dropped[d] && selected[d].size() != 1)
      throw std::invalid_argument("collapsed axis needs one position");
    if (!dropped[d])
      map.dimensions.push_back(static_cast<int64_t>(selected[d].size()));
    total =
        checked_slice_product(total, static_cast<int64_t>(selected[d].size()));
  }
  map.count = total;
  map.gather.reserve(static_cast<size_t>(total));
  std::vector<size_t> at(rank, 0);
  for (int64_t cell = 0; cell < total; ++cell) {
    int64_t offset = 0;
    for (size_t d = 0; d < rank; ++d) offset += selected[d][at[d]] * stride[d];
    map.gather.push_back(offset);
    for (const size_t axis : axis_order) {
      if (++at[axis] < selected[axis].size()) break;
      at[axis] = 0;
    }
  }

  // A selection that walks storage at one non-negative step needs no gather.
  bool constant_step = true;
  const int64_t step = total > 1 ? map.gather[1] - map.gather[0] : 1;
  for (int64_t cell = 1; constant_step && cell < total; ++cell)
    constant_step = map.gather[cell] - map.gather[cell - 1] == step;
  if (total == 0) {
    map.kind = BuiltinSliceMap::Kind::Contiguous;
    map.offset = 0;
    map.gather.clear();
  } else if (constant_step && step == 1) {
    map.kind = BuiltinSliceMap::Kind::Contiguous;
    map.offset = map.gather.front();
    map.gather.clear();
  } else if (constant_step && step >= 0) {
    map.kind = BuiltinSliceMap::Kind::Strided;
    map.offset = map.gather.front();
    map.stride = step;
    map.gather.clear();
  } else {
    map.kind = BuiltinSliceMap::Kind::Gather;
  }
  return map;
}

int evaluate_predicate_builtin(const BuiltinSpec& spec, double lhs,
                               double rhs) {
  if (spec.shape != BuiltinShapePolicy::Predicate ||
      spec.predicate == BuiltinPredicate::None)
    throw std::invalid_argument("descriptor is not a predicate");
  switch (spec.predicate) {
    case BuiltinPredicate::Eq:
      return lhs == rhs;
    case BuiltinPredicate::Neq:
      return lhs != rhs;
    case BuiltinPredicate::Lt:
      return lhs < rhs;
    case BuiltinPredicate::Lte:
      return lhs <= rhs;
    case BuiltinPredicate::Gt:
      return lhs > rhs;
    case BuiltinPredicate::Gte:
      return lhs >= rhs;
    case BuiltinPredicate::And:
      return lhs != 0.0 && rhs != 0.0;
    case BuiltinPredicate::Or:
      return lhs != 0.0 || rhs != 0.0;
    case BuiltinPredicate::Negation:
      return lhs == 0.0;
    case BuiltinPredicate::IsNan:
      return std::isnan(lhs);
    case BuiltinPredicate::IsInf:
      return std::isinf(lhs);
    case BuiltinPredicate::None:
      break;
  }
  throw std::invalid_argument("descriptor is not a predicate");
}

int evaluate_integer_binary_builtin(const BuiltinSpec& spec, int lhs, int rhs) {
  if (spec.arity != 2 || spec.result != FunctionArgumentKind::Integer ||
      !builtin_argument_is_integer(spec, 0) ||
      !builtin_argument_is_integer(spec, 1))
    throw std::logic_error("builtin is not an integer binary function");
  return evaluate_integer_binary_builtin(spec.opcode, lhs, rhs);
}

int evaluate_integer_binary_builtin(uint16_t opcode, int lhs, int rhs) {
  switch (opcode) {
    case OP_ADD:
      return lhs + rhs;
    case OP_SUB:
      return lhs - rhs;
    case OP_MUL:
      return lhs * rhs;
    case OP_DIV:
      return lhs / rhs;
    case OP_CHOOSE:
      return stan::math::choose(lhs, rhs);
    default:
      throw std::logic_error("integer binary builtin has no evaluator");
  }
}

int evaluate_integer_unary_builtin(const BuiltinSpec& spec, int value) {
  if (spec.arity != 1 || spec.result != FunctionArgumentKind::Integer ||
      !builtin_argument_is_integer(spec, 0))
    throw std::logic_error("builtin is not an integer unary function");
  switch (spec.opcode) {
    case OP_NEG:
      return -value;
    case OP_ABS:
      return value < 0 ? -value : value;
    default:
      throw std::logic_error("integer unary builtin has no evaluator");
  }
}

}  // namespace stanli
