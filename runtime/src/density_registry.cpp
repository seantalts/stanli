#include <stanli/density_registry.hpp>
#include <stanli/function_registry.hpp>

#include <stan/math/prim/prob/discrete_range_lpmf.hpp>
#include <stan/math/prim/prob/hypergeometric_lpmf.hpp>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace stanli {
namespace {

template <typename F>
double with_integer_argument(const IntegerDensityArgument& argument, F&& f) {
  if (argument.scalar) {
    if (argument.values.size() != 1)
      throw std::logic_error("malformed scalar all-integer density argument");
    return std::forward<F>(f)(argument.values[0]);
  }
  return std::forward<F>(f)(argument.values);
}

double evaluate_all_integer(
    AllIntegerDensity density,
    const std::vector<IntegerDensityArgument>& arguments, bool propto) {
  switch (density) {
    case AllIntegerDensity::HypergeometricLpmf:
      return with_integer_argument(arguments[0], [&](const auto& n) {
        return with_integer_argument(arguments[1], [&](const auto& N) {
          return with_integer_argument(arguments[2], [&](const auto& a) {
            return with_integer_argument(arguments[3], [&](const auto& b) {
              return propto
                         ? stan::math::hypergeometric_lpmf<true>(n, N, a, b)
                         : stan::math::hypergeometric_lpmf<false>(n, N, a, b);
            });
          });
        });
      });
    case AllIntegerDensity::DiscreteRangeLpmf:
      return with_integer_argument(arguments[0], [&](const auto& y) {
        return with_integer_argument(arguments[1], [&](const auto& lower) {
          return with_integer_argument(arguments[2], [&](const auto& upper) {
            return propto
                       ? stan::math::discrete_range_lpmf<true>(y, lower, upper)
                       : stan::math::discrete_range_lpmf<false>(y, lower,
                                                                upper);
          });
        });
      });
    case AllIntegerDensity::None:
      break;
  }
  throw std::logic_error("unknown all-integer density evaluator");
}

}  // namespace

const DensitySpec* density_spec(const std::string& name) {
  const FunctionSpec* candidate = function_spec(name, FunctionFamily::Density);
  return candidate == nullptr ? nullptr : candidate->density();
}

const std::vector<const FunctionSpec*>& density_specs() {
  return function_specs(FunctionFamily::Density);
}

DensityCallArgument integer_density_argument(std::vector<int> values,
                                             bool scalar, bool data_only) {
  DensityCallArgument argument;
  argument.scalar = scalar;
  argument.data_only = data_only;
  argument.active = !data_only;
  argument.shape.value = FunctionArgumentKind::Integer;
  argument.shape.container =
      scalar ? FunctionContainerKind::Scalar : FunctionContainerKind::Array;
  argument.shape.array_leaf = FunctionContainerKind::Scalar;
  argument.shape.storage_size = static_cast<int64_t>(values.size());
  argument.shape.array_depth = scalar ? 0 : 1;
  if (!scalar) argument.shape.dimensions = {argument.shape.storage_size};
  argument.integers = std::move(values);
  return argument;
}

double evaluate_all_integer_density(
    const DensitySpec& spec,
    const std::vector<IntegerDensityArgument>& arguments, bool propto) {
  if (spec.evaluation != DensityEvaluationPolicy::AllInteger ||
      spec.all_integer == AllIntegerDensity::None)
    throw std::logic_error("density does not use all-integer evaluation");
  if ((int)arguments.size() != spec.arity)
    throw std::invalid_argument("all-integer density has wrong arity");
  return evaluate_all_integer(spec.all_integer, arguments, propto);
}

std::vector<int> pack_all_integer_density_arguments(
    const std::vector<IntegerDensityArgument>& arguments) {
  std::vector<int> packed;
  for (const IntegerDensityArgument& argument : arguments) {
    if (argument.scalar && argument.values.size() != 1)
      throw std::logic_error("malformed scalar all-integer density argument");
    if (argument.values.size() >
        static_cast<size_t>(std::numeric_limits<int>::max()))
      throw std::length_error("all-integer density argument is too large");
    packed.push_back(argument.scalar ? -1 : (int)argument.values.size());
    packed.insert(packed.end(), argument.values.begin(), argument.values.end());
  }
  return packed;
}

double evaluate_packed_all_integer_density(AllIntegerDensity density,
                                           const int* packed, int64_t size,
                                           bool propto) {
  const int arity = density == AllIntegerDensity::HypergeometricLpmf  ? 4
                    : density == AllIntegerDensity::DiscreteRangeLpmf ? 3
                                                                      : 0;
  if (arity == 0) throw std::logic_error("unknown all-integer density");
  std::vector<IntegerDensityArgument> arguments;
  arguments.reserve((size_t)arity);
  int64_t at = 0;
  for (int k = 0; k < arity; ++k) {
    if (at >= size)
      throw std::logic_error("truncated all-integer density payload");
    const int encoded = packed[at++];
    const bool scalar = encoded == -1;
    const int64_t count = scalar ? 1 : encoded;
    if (count < 0 || count > size - at)
      throw std::logic_error("malformed all-integer density payload");
    arguments.push_back({{packed + at, packed + at + count}, scalar});
    at += count;
  }
  if (at != size)
    throw std::logic_error("trailing all-integer density payload");
  return evaluate_all_integer(density, arguments, propto);
}

VectorizedDensityLayout vectorized_density_layout(
    const DensitySpec& spec,
    const std::vector<DensityArgumentShape>& arguments) {
  if ((spec.shape != DensityShape::LastMatrixRowsAndRepetitions &&
       spec.shape != DensityShape::VectorizedVectors) ||
      spec.vectorized_vector_args == 0)
    throw std::logic_error("density does not use vectorized-vector layout");
  if ((int)arguments.size() != spec.arity - spec.integer_args)
    throw std::invalid_argument("vectorized density has wrong arity");

  int64_t width = -1;
  if (spec.shape == DensityShape::LastMatrixRowsAndRepetitions) {
    const DensityArgumentShape& matrix = arguments.back();
    if (function_leaf(matrix) != FunctionContainerKind::Matrix ||
        matrix.array_depth != 0 || matrix.dimensions.size() != 2 ||
        matrix.dimensions[0] < 0 ||
        matrix.dimensions[0] != matrix.dimensions[1])
      throw std::invalid_argument("last argument must be a square matrix");
    width = matrix.dimensions[0];
    if (width != 0 && width > std::numeric_limits<int64_t>::max() / width)
      throw std::length_error("matrix extent overflows");
    if (matrix.storage_size != width * width)
      throw std::invalid_argument("matrix shape does not match storage");
  }

  VectorizedDensityLayout result;
  result.width = width;
  int array_count = -1;
  for (size_t k = 0; k < arguments.size(); ++k) {
    if ((spec.vectorized_vector_args & (uint8_t)(1u << k)) == 0) continue;
    const DensityArgumentShape& arg = arguments[k];
    const FunctionContainerKind leaf = function_leaf(arg);
    if ((leaf != FunctionContainerKind::Vector &&
         leaf != FunctionContainerKind::RowVector) ||
        arg.dimensions.size() != (size_t)arg.array_depth + 1 ||
        arg.dimensions.empty() ||
        (width >= 0 && arg.dimensions.back() != width))
      throw std::invalid_argument(
          "vectorized argument must be a matching vector or array of vectors");
    if (width < 0) width = arg.dimensions.back();
    int64_t count = 1;
    for (size_t d = 0; d < arg.array_depth; ++d) {
      const int64_t extent = arg.dimensions[d];
      if (extent < 0 ||
          (extent != 0 && count > std::numeric_limits<int64_t>::max() / extent))
        throw std::length_error("vectorized argument extent overflows");
      count *= extent;
    }
    if (width != 0 && count > std::numeric_limits<int64_t>::max() / width)
      throw std::length_error("vectorized argument storage overflows");
    if (arg.storage_size != count * width)
      throw std::invalid_argument(
          "vectorized argument shape does not match storage");
    if (arg.array_depth == 0) {
      result.vector_counts.push_back(-1);
    } else {
      if (count > std::numeric_limits<int>::max())
        throw std::length_error("vectorized argument has too many elements");
      if (array_count >= 0 && count != array_count)
        throw std::invalid_argument("vectorized argument sizes differ");
      array_count = (int)count;
      result.vector_counts.push_back((int)count);
    }
  }
  if (width < 0)
    throw std::logic_error("vectorized density has no vector argument");
  result.width = width;
  return result;
}

DensityCallPlan density_call_plan(
    const DensitySpec& spec, const std::vector<DensityCallArgument>& arguments,
    bool propto) {
  if (arguments.size() != static_cast<size_t>(spec.arity))
    throw std::invalid_argument("density has wrong arity");

  DensityCallPlan plan;
  if (spec.evaluation == DensityEvaluationPolicy::AllInteger) {
    std::vector<IntegerDensityArgument> integers;
    integers.reserve(arguments.size());
    for (const DensityCallArgument& argument : arguments) {
      if (argument.shape.value != FunctionArgumentKind::Integer)
        throw std::invalid_argument("all-integer density has a real argument");
      integers.push_back({argument.integers, argument.scalar});
    }
    plan.idata = pack_all_integer_density_arguments(integers);
    plan.variant = spec.fixed_variant | (propto ? uint8_t{0x80} : uint8_t{0});
    return plan;
  }

  std::vector<std::vector<int>> integer_groups;
  uint8_t glm_scalar_mask = 0;
  bool scalar_outcome = false;
  if (spec.integer_args == 1) {
    plan.idata = arguments[0].integers;
    scalar_outcome = arguments[0].scalar && plan.idata.size() == 1;
    if (scalar_outcome) glm_scalar_mask |= 1u;
  } else if (spec.integer_args == 2) {
    integer_groups.reserve(2);
    for (size_t k = 0; k < 2; ++k) {
      const DensityCallArgument& argument = arguments[k];
      if (spec.glm_layout) {
        if (argument.scalar) glm_scalar_mask |= uint8_t{1} << k;
        integer_groups.push_back(argument.integers);
      } else {
        if (argument.integers.size() >
            static_cast<size_t>(std::numeric_limits<int>::max()))
          throw std::length_error("integer density argument is too large");
        plan.idata.push_back(
            argument.scalar ? -1 : static_cast<int>(argument.integers.size()));
        plan.idata.insert(plan.idata.end(), argument.integers.begin(),
                          argument.integers.end());
      }
    }
  }

  std::vector<DensityArgumentShape> real_shapes;
  real_shapes.reserve(arguments.size() -
                      static_cast<size_t>(spec.integer_args));
  for (size_t k = static_cast<size_t>(spec.integer_args); k < arguments.size();
       ++k) {
    real_shapes.push_back(arguments[k].shape);
    if (arguments[k].active)
      plan.activity_mask |= uint8_t{1}
                            << (k - static_cast<size_t>(spec.integer_args));
  }

  const auto matrix_dimensions = [&](size_t real_index,
                                     bool allow_row_vector = false) {
    if (real_index >= real_shapes.size())
      throw std::invalid_argument("density is missing a matrix argument");
    const FunctionArgumentShape& shape = real_shapes[real_index];
    if (allow_row_vector &&
        shape.container == FunctionContainerKind::RowVector &&
        shape.array_depth == 0 && shape.dimensions.size() == 1)
      return std::pair<int64_t, int64_t>{1, shape.dimensions[0]};
    if (shape.container != FunctionContainerKind::Matrix ||
        shape.array_depth != 0 || shape.dimensions.size() != 2)
      throw std::invalid_argument("density argument is not a matrix");
    return std::pair<int64_t, int64_t>{shape.dimensions[0],
                                       shape.dimensions[1]};
  };
  const auto as_int_extent = [](int64_t value) {
    if (value < 0 || value > std::numeric_limits<int>::max())
      throw std::length_error("density extent exceeds integer payload");
    return static_cast<int>(value);
  };

  if (spec.lane_outcome && scalar_outcome) {
    int64_t lanes = 1;
    if (spec.glm_layout)
      lanes = matrix_dimensions(spec.glm_matrix_arg, true).first;
    else
      for (const DensityArgumentShape& shape : real_shapes)
        lanes = std::max(lanes, shape.storage_size);
    if (lanes > 1) plan.idata.assign(static_cast<size_t>(lanes), plan.idata[0]);
  }

  if (spec.glm_layout) {
    const auto [rows, cols] = matrix_dimensions(spec.glm_matrix_arg, true);
    if (spec.integer_args == 2) {
      plan.idata.clear();
      for (size_t k = 0; k < integer_groups.size(); ++k) {
        std::vector<int>& group = integer_groups[k];
        if ((glm_scalar_mask & (uint8_t{1} << k)) && rows > 1)
          group.assign(static_cast<size_t>(rows), group[0]);
        if (static_cast<int64_t>(group.size()) != rows)
          throw std::invalid_argument(
              "integer density argument does not match matrix rows");
        plan.idata.insert(plan.idata.end(), group.begin(), group.end());
      }
    } else if (spec.integer_args == 1 &&
               static_cast<int64_t>(plan.idata.size()) != rows) {
      throw std::invalid_argument("density outcome does not match matrix rows");
    }
    plan.idata.push_back(as_int_extent(rows));
    plan.idata.push_back(as_int_extent(cols));
    plan.idata.push_back(kGlmScalarLayoutMarker);
    plan.idata.push_back(static_cast<int>(glm_scalar_mask));
  }

  if (spec.shape == DensityShape::Categorical) {
    const FunctionArgumentShape& outcome = arguments[0].shape;
    const FunctionArgumentShape& probabilities = arguments[1].shape;
    const bool scalar = arguments[0].scalar;
    const bool valid_outcome =
        outcome.value == FunctionArgumentKind::Integer &&
        ((scalar && outcome.container == FunctionContainerKind::Scalar) ||
         (!scalar && outcome.container == FunctionContainerKind::Array &&
          function_leaf(outcome) == FunctionContainerKind::Scalar &&
          outcome.array_depth == 1));
    if (!valid_outcome || !arguments[0].data_only ||
        probabilities.container != FunctionContainerKind::Vector ||
        probabilities.array_depth != 0)
      throw std::invalid_argument(
          "categorical expects data-only int or array[] int and vector");
    plan.variant = spec.fixed_variant;
    if (arguments[1].active) plan.variant |= kCategoricalArgAutodiff;
    if (scalar) plan.variant |= kCategoricalScalarOutcome;
  } else {
    plan.variant =
        spec.fixed_variant |
        (spec.activity_mask < 0 ? plan.activity_mask
                                : static_cast<uint8_t>(spec.activity_mask));
  }

  if (spec.shape == DensityShape::FirstMatrixRows) {
    plan.idata = {as_int_extent(matrix_dimensions(0).first)};
  } else if (spec.shape == DensityShape::FirstMatrixDimensions) {
    const auto [rows, cols] = matrix_dimensions(0);
    (void)matrix_dimensions(1);
    plan.idata = {as_int_extent(rows), as_int_extent(cols)};
  } else if (spec.shape == DensityShape::LastMatrixRowsAndRepetitions ||
             spec.shape == DensityShape::VectorizedVectors) {
    const VectorizedDensityLayout layout =
        vectorized_density_layout(spec, real_shapes);
    plan.empty_result =
        std::any_of(layout.vector_counts.begin(), layout.vector_counts.end(),
                    [](int count) { return count == 0; });
    if (spec.integer_args) {
      plan.idata.push_back(kVectorizedDensityLayoutMarker);
      plan.idata.push_back(as_int_extent(layout.width));
    } else {
      plan.idata = {as_int_extent(layout.width)};
    }
    plan.idata.insert(plan.idata.end(), layout.vector_counts.begin(),
                      layout.vector_counts.end());
  }
  if (propto) plan.variant |= 0x80u;
  return plan;
}

}  // namespace stanli
