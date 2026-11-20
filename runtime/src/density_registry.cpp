#include <stanli/density_registry.hpp>
#include <stanli/function_registry.hpp>

#include <stan/math/prim/prob/discrete_range_cdf.hpp>
#include <stan/math/prim/prob/discrete_range_lccdf.hpp>
#include <stan/math/prim/prob/discrete_range_lcdf.hpp>
#include <stan/math/prim/err/check_consistent_size.hpp>
#include <stan/math/prim/err/check_consistent_sizes.hpp>
#include <stan/math/prim/err/check_size_match.hpp>
#include <stan/math/prim/prob/discrete_range_lpmf.hpp>
#include <stan/math/prim/prob/hypergeometric_lpmf.hpp>
#include <stan/math/prim/fun/lgamma.hpp>

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
    case AllIntegerDensity::DiscreteRangeCdf:
    case AllIntegerDensity::DiscreteRangeLcdf:
    case AllIntegerDensity::DiscreteRangeLccdf:
      return with_integer_argument(arguments[0], [&](const auto& y) {
        return with_integer_argument(arguments[1], [&](const auto& lower) {
          return with_integer_argument(arguments[2], [&](const auto& upper) {
            switch (density) {
              case AllIntegerDensity::DiscreteRangeCdf:
                return stan::math::discrete_range_cdf(y, lower, upper);
              case AllIntegerDensity::DiscreteRangeLcdf:
                return stan::math::discrete_range_lcdf(y, lower, upper);
              default:
                return stan::math::discrete_range_lccdf(y, lower, upper);
            }
          });
        });
      });
    case AllIntegerDensity::None:
      break;
  }
  throw std::logic_error("unknown all-integer density evaluator");
}

}  // namespace

void density_layout_error(const char* what) { throw std::logic_error(what); }

// Generated from the pinned deps/math prim/prob sources: each function's
// check_consistent_sizes arguments and names, case included.
// test_density_sizes checks every entry against Stan Math itself.
const DensitySizeCheck* density_size_check(std::string_view function) {
  static const std::vector<DensitySizeCheck> checks{
      {"bernoulli_cdf", {{0, "Random variable"}, {1, "Probability parameter"}}},
      {"bernoulli_lccdf",
       {{0, "Random variable"}, {1, "Probability parameter"}}},
      {"bernoulli_lcdf",
       {{0, "Random variable"}, {1, "Probability parameter"}}},
      {"bernoulli_logit_lpmf",
       {{0, "Random variable"}, {1, "Probability parameter"}}},
      {"bernoulli_lpmf",
       {{0, "Random variable"}, {1, "Probability parameter"}}},
      {"beta_binomial_cdf",
       {{0, "Successes variable"},
        {1, "Population size parameter"},
        {2, "First prior sample size parameter"},
        {3, "Second prior sample size parameter"}}},
      {"beta_binomial_lccdf",
       {{0, "Successes variable"},
        {1, "Population size parameter"},
        {2, "First prior sample size parameter"},
        {3, "Second prior sample size parameter"}}},
      {"beta_binomial_lcdf",
       {{0, "Successes variable"},
        {1, "Population size parameter"},
        {2, "First prior sample size parameter"},
        {3, "Second prior sample size parameter"}}},
      {"beta_binomial_lpmf",
       {{0, "Successes variable"},
        {1, "Population size parameter"},
        {2, "First prior sample size parameter"},
        {3, "Second prior sample size parameter"}}},
      {"beta_cdf",
       {{0, "Random variable"},
        {1, "First shape parameter"},
        {2, "Second shape parameter"}}},
      {"beta_lccdf",
       {{0, "Random variable"},
        {1, "First shape parameter"},
        {2, "Second shape parameter"}}},
      {"beta_lcdf",
       {{0, "Random variable"},
        {1, "First shape parameter"},
        {2, "Second shape parameter"}}},
      {"beta_lpdf",
       {{0, "Random variable"},
        {1, "First shape parameter"},
        {2, "Second shape parameter"}}},
      {"beta_neg_binomial_cdf",
       {{0, "Failures variable"},
        {1, "Number of successes parameter"},
        {2, "Prior success parameter"},
        {3, "Prior failure parameter"}}},
      {"beta_neg_binomial_lccdf",
       {{0, "Failures variable"},
        {1, "Number of successes parameter"},
        {2, "Prior success parameter"},
        {3, "Prior failure parameter"}}},
      {"beta_neg_binomial_lcdf",
       {{0, "Failures variable"},
        {1, "Number of successes parameter"},
        {2, "Prior success parameter"},
        {3, "Prior failure parameter"}}},
      {"beta_neg_binomial_lpmf",
       {{0, "Failures variable"},
        {1, "Number of successes parameter"},
        {2, "Prior success parameter"},
        {3, "Prior failure parameter"}}},
      {"beta_proportion_lccdf",
       {{0, "Random variable"},
        {1, "Location parameter"},
        {2, "Precision parameter"}}},
      {"beta_proportion_lcdf",
       {{0, "Random variable"},
        {1, "Location parameter"},
        {2, "Precision parameter"}}},
      {"beta_proportion_lpdf",
       {{0, "Random variable"},
        {1, "Location parameter"},
        {2, "Precision parameter"}}},
      {"binomial_cdf",
       {{0, "Successes variable"},
        {1, "Population size parameter"},
        {2, "Probability parameter"}}},
      {"binomial_lccdf",
       {{0, "Successes variable"},
        {1, "Population size parameter"},
        {2, "Probability parameter"}}},
      {"binomial_lcdf",
       {{0, "Successes variable"},
        {1, "Population size parameter"},
        {2, "Probability parameter"}}},
      {"binomial_logit_lpmf",
       {{0, "Successes variable"},
        {1, "Population size parameter"},
        {2, "Probability parameter"}}},
      {"binomial_lpmf",
       {{0, "Successes variable"},
        {1, "Population size parameter"},
        {2, "Probability parameter"}}},
      {"cauchy_cdf",
       {{0, "Random variable"},
        {1, "Location parameter"},
        {2, "Scale Parameter"}}},
      {"cauchy_lccdf",
       {{0, "Random variable"},
        {1, "Location parameter"},
        {2, "Scale Parameter"}}},
      {"cauchy_lcdf",
       {{0, "Random variable"},
        {1, "Location parameter"},
        {2, "Scale Parameter"}}},
      {"cauchy_lpdf",
       {{0, "Random variable"},
        {1, "Location parameter"},
        {2, "Scale parameter"}}},
      {"chi_square_cdf",
       {{0, "Random variable"}, {1, "Degrees of freedom parameter"}}},
      {"chi_square_lccdf",
       {{0, "Random variable"}, {1, "Degrees of freedom parameter"}}},
      {"chi_square_lcdf",
       {{0, "Random variable"}, {1, "Degrees of freedom parameter"}}},
      {"chi_square_lpdf",
       {{0, "Random variable"}, {1, "Degrees of freedom parameter"}}},
      {"double_exponential_lccdf",
       {{0, "Random variable"},
        {1, "Location parameter"},
        {2, "Scale Parameter"}}},
      {"double_exponential_lcdf",
       {{0, "Random variable"},
        {1, "Location parameter"},
        {2, "Scale Parameter"}}},
      {"double_exponential_lpdf",
       {{0, "Random variable"},
        {1, "Location parameter"},
        {2, "Shape parameter"}}},
      {"exp_mod_normal_cdf",
       {{0, "Random variable"},
        {1, "Location parameter"},
        {2, "Scale parameter"},
        {3, "Inv_scale parameter"}}},
      {"exp_mod_normal_lccdf",
       {{0, "Random variable"},
        {1, "Location parameter"},
        {2, "Scale parameter"},
        {3, "Inv_scale parameter"}}},
      {"exp_mod_normal_lcdf",
       {{0, "Random variable"},
        {1, "Location parameter"},
        {2, "Scale parameter"},
        {3, "Inv_scale parameter"}}},
      {"exp_mod_normal_lpdf",
       {{0, "Random variable"},
        {1, "Location parameter"},
        {2, "Scale parameter"},
        {3, "Inv_scale parameter"}}},
      {"exponential_lpdf",
       {{0, "Random variable"}, {1, "Inverse scale parameter"}}},
      {"frechet_lpdf",
       {{0, "Random variable"},
        {1, "Shape parameter"},
        {2, "Scale parameter"}}},
      {"gamma_cdf",
       {{0, "Random variable"},
        {1, "Shape parameter"},
        {2, "Inverse scale parameter"}}},
      {"gamma_lccdf",
       {{0, "Random variable"},
        {1, "Shape parameter"},
        {2, "Inverse scale parameter"}}},
      {"gamma_lcdf",
       {{0, "Random variable"},
        {1, "Shape parameter"},
        {2, "Inverse scale parameter"}}},
      {"gamma_lpdf",
       {{0, "Random variable"},
        {1, "Shape parameter"},
        {2, "Inverse scale parameter"}}},
      {"gumbel_cdf",
       {{0, "Random variable"},
        {1, "Location parameter"},
        {2, "Scale parameter"}}},
      {"gumbel_lcdf",
       {{0, "Random variable"},
        {1, "Location parameter"},
        {2, "Scale parameter"}}},
      {"gumbel_lpdf",
       {{0, "Random variable"},
        {1, "Location parameter"},
        {2, "Scale parameter"}}},
      {"inv_chi_square_cdf",
       {{0, "Random variable"}, {1, "Degrees of freedom parameter"}}},
      {"inv_chi_square_lccdf",
       {{0, "Random variable"}, {1, "Degrees of freedom parameter"}}},
      {"inv_chi_square_lcdf",
       {{0, "Random variable"}, {1, "Degrees of freedom parameter"}}},
      {"inv_chi_square_lpdf",
       {{0, "Random variable"}, {1, "Degrees of freedom parameter"}}},
      {"inv_gamma_cdf",
       {{0, "Random variable"},
        {1, "Shape parameter"},
        {2, "Scale Parameter"}}},
      {"inv_gamma_lccdf",
       {{0, "Random variable"},
        {1, "Shape parameter"},
        {2, "Scale Parameter"}}},
      {"inv_gamma_lcdf",
       {{0, "Random variable"},
        {1, "Shape parameter"},
        {2, "Scale Parameter"}}},
      {"inv_gamma_lpdf",
       {{0, "Random variable"},
        {1, "Shape parameter"},
        {2, "Scale parameter"}}},
      {"logistic_cdf",
       {{0, "Random variable"},
        {1, "Location parameter"},
        {2, "Scale parameter"}}},
      {"logistic_lccdf",
       {{0, "Random variable"},
        {1, "Location parameter"},
        {2, "Scale parameter"}}},
      {"logistic_lcdf",
       {{0, "Random variable"},
        {1, "Location parameter"},
        {2, "Scale parameter"}}},
      {"logistic_lpdf",
       {{0, "Random variable"},
        {1, "Location parameter"},
        {2, "Scale parameter"}}},
      {"loglogistic_cdf",
       {{0, "Random variable"},
        {1, "Scale parameter"},
        {2, "Shape parameter"}}},
      {"loglogistic_lpdf",
       {{0, "Random variable"},
        {1, "Scale parameter"},
        {2, "Shape parameter"}}},
      {"lognormal_lpdf",
       {{0, "Random variable"},
        {1, "Location parameter"},
        {2, "Scale parameter"}}},
      {"neg_binomial_2_cdf",
       {{0, "Random variable"},
        {1, "Location parameter"},
        {2, "Precision Parameter"}}},
      {"neg_binomial_2_lccdf",
       {{0, "Random variable"},
        {1, "Location parameter"},
        {2, "Precision Parameter"}}},
      {"neg_binomial_2_lcdf",
       {{0, "Random variable"},
        {1, "Location parameter"},
        {2, "Precision Parameter"}}},
      {"neg_binomial_2_log_lpmf",
       {{0, "Failures variable"},
        {1, "Log location parameter"},
        {2, "Precision parameter"}}},
      {"neg_binomial_2_lpmf",
       {{0, "Failures variable"},
        {1, "Location parameter"},
        {2, "Precision parameter"}}},
      {"neg_binomial_cdf",
       {{0, "Failures variable"},
        {1, "Shape parameter"},
        {2, "Inverse scale parameter"}}},
      {"neg_binomial_lccdf",
       {{0, "Failures variable"},
        {1, "Shape parameter"},
        {2, "Inverse scale parameter"}}},
      {"neg_binomial_lcdf",
       {{0, "Failures variable"},
        {1, "Shape parameter"},
        {2, "Inverse scale parameter"}}},
      {"neg_binomial_lpmf",
       {{0, "Failures variable"},
        {1, "Shape parameter"},
        {2, "Inverse scale parameter"}}},
      {"normal_cdf",
       {{0, "Random variable"},
        {1, "Location parameter"},
        {2, "Scale parameter"}}},
      {"normal_lcdf",
       {{0, "Random variable"},
        {1, "Location parameter"},
        {2, "Scale parameter"}}},
      {"normal_lpdf",
       {{0, "Random variable"},
        {1, "Location parameter"},
        {2, "Scale parameter"}}},
      {"ordered_logistic_lpmf", {{0, "Integers"}, {1, "Locations"}}},
      {"ordered_probit_lpmf", {{0, "Integers"}, {1, "Locations"}}},
      {"pareto_cdf",
       {{0, "Random variable"},
        {1, "Scale parameter"},
        {2, "Shape parameter"}}},
      {"pareto_lccdf",
       {{0, "Random variable"},
        {1, "Scale parameter"},
        {2, "Shape parameter"}}},
      {"pareto_lcdf",
       {{0, "Random variable"},
        {1, "Scale parameter"},
        {2, "Shape parameter"}}},
      {"pareto_lpdf",
       {{0, "Random variable"},
        {1, "Scale parameter"},
        {2, "Shape parameter"}}},
      {"pareto_type_2_cdf",
       {{0, "Random variable"},
        {1, "Location parameter"},
        {2, "Scale parameter"},
        {3, "Shape parameter"}}},
      {"pareto_type_2_lccdf",
       {{0, "Random variable"},
        {1, "Location parameter"},
        {2, "Scale parameter"},
        {3, "Shape parameter"}}},
      {"pareto_type_2_lcdf",
       {{0, "Random variable"},
        {1, "Location parameter"},
        {2, "Scale parameter"},
        {3, "Shape parameter"}}},
      {"pareto_type_2_lpdf",
       {{0, "Random variable"},
        {1, "Location parameter"},
        {2, "Scale parameter"},
        {3, "Shape parameter"}}},
      {"poisson_cdf", {{0, "Random variable"}, {1, "Rate parameter"}}},
      {"poisson_lccdf", {{0, "Random variable"}, {1, "Rate parameter"}}},
      {"poisson_lcdf", {{0, "Random variable"}, {1, "Rate parameter"}}},
      {"poisson_log_lpmf", {{0, "Random variable"}, {1, "Log rate parameter"}}},
      {"poisson_lpmf", {{0, "Random variable"}, {1, "Rate parameter"}}},
      {"rayleigh_cdf", {{0, "Random variable"}, {1, "Scale parameter"}}},
      {"rayleigh_lccdf", {{0, "Random variable"}, {1, "Scale parameter"}}},
      {"rayleigh_lcdf", {{0, "Random variable"}, {1, "Scale parameter"}}},
      {"rayleigh_lpdf", {{0, "Random variable"}, {1, "Scale parameter"}}},
      {"scaled_inv_chi_square_cdf",
       {{0, "Random variable"},
        {1, "Degrees of freedom parameter"},
        {2, "Scale parameter"}}},
      {"scaled_inv_chi_square_lccdf",
       {{0, "Random variable"},
        {1, "Degrees of freedom parameter"},
        {2, "Scale parameter"}}},
      {"scaled_inv_chi_square_lcdf",
       {{0, "Random variable"},
        {1, "Degrees of freedom parameter"},
        {2, "Scale parameter"}}},
      {"scaled_inv_chi_square_lpdf",
       {{0, "Random variable"},
        {1, "Degrees of freedom parameter"},
        {2, "Scale parameter"}}},
      {"skew_double_exponential_cdf",
       {{0, "Random variable"},
        {1, "Location parameter"},
        {2, "Shape parameter"},
        {3, "Skewness parameter"}}},
      {"skew_double_exponential_lccdf",
       {{0, "Random variable"},
        {1, "Location parameter"},
        {2, "Shape parameter"},
        {3, "Skewness parameter"}}},
      {"skew_double_exponential_lcdf",
       {{0, "Random variable"},
        {1, "Location parameter"},
        {2, "Shape parameter"},
        {3, "Skewness parameter"}}},
      {"skew_double_exponential_lpdf",
       {{0, "Random variable"},
        {1, "Location parameter"},
        {2, "Shape parameter"},
        {3, "Skewness parameter"}}},
      {"skew_normal_cdf",
       {{0, "Random variable"},
        {1, "Location parameter"},
        {2, "Scale parameter"},
        {3, "Shape parameter"}}},
      {"skew_normal_lccdf",
       {{0, "Random variable"},
        {1, "Location parameter"},
        {2, "Scale parameter"},
        {3, "Shape parameter"}}},
      {"skew_normal_lcdf",
       {{0, "Random variable"},
        {1, "Location parameter"},
        {2, "Scale parameter"},
        {3, "Shape parameter"}}},
      {"skew_normal_lpdf",
       {{0, "Random variable"},
        {1, "Location parameter"},
        {2, "Scale parameter"},
        {3, "Shape parameter"}}},
      {"student_t_lpdf",
       {{0, "Random variable"},
        {1, "Degrees of freedom parameter"},
        {2, "Location parameter"},
        {3, "Scale parameter"}}},
      {"uniform_cdf",
       {{0, "Random variable"},
        {1, "Lower bound parameter"},
        {2, "Upper bound parameter"}}},
      {"uniform_lccdf",
       {{0, "Random variable"},
        {1, "Lower bound parameter"},
        {2, "Upper bound parameter"}}},
      {"uniform_lcdf",
       {{0, "Random variable"},
        {1, "Lower bound parameter"},
        {2, "Upper bound parameter"}}},
      {"uniform_lpdf",
       {{0, "Random variable"},
        {1, "Lower bound parameter"},
        {2, "Upper bound parameter"}}},
      {"von_mises_cdf",
       {{0, "Random variable"},
        {1, "Location parameter"},
        {2, "Scale parameter"}}},
      {"von_mises_lccdf",
       {{0, "Random variable"},
        {1, "Location parameter"},
        {2, "Scale parameter"}}},
      {"von_mises_lcdf",
       {{0, "Random variable"},
        {1, "Location parameter"},
        {2, "Scale parameter"}}},
      {"von_mises_lpdf",
       {{0, "Random variable"},
        {1, "Location parameter"},
        {2, "Scale parameter"}}},
      {"weibull_lpdf",
       {{0, "Random variable"},
        {1, "Shape parameter"},
        {2, "Scale parameter"}}},
      {"wiener_lpdf",
       {{0, "Random variable"},
        {1, "Boundary separation"},
        {3, "A-priori bias"},
        {2, "Nondecision time"},
        {4, "Drift rate"}}},
      {"yule_simon_cdf", {{0, "Outcome variable"}, {1, "Shape parameter"}}},
      {"yule_simon_lccdf", {{0, "Outcome variable"}, {1, "Shape parameter"}}},
      {"yule_simon_lcdf", {{0, "Outcome variable"}, {1, "Shape parameter"}}},
      {"yule_simon_lpmf", {{0, "Failures variable"}, {1, "Shape parameter"}}},
  };
  for (const DensitySizeCheck& check : checks)
    if (function == check.function) return &check;
  return nullptr;
}

const DensitySpec& multinomial_softmax_density() {
  static const DensitySpec spec{OP_MULTINOMIAL_SOFTMAX_LPMF, 2, 1, false,
                                DensityShape::Plain};
  return spec;
}

double multinomial_coefficient(uint16_t opcode, const int* counts,
                               int64_t size) {
  if (opcode == OP_MULTINOMIAL_LOGIT_LPMF) {
    const Eigen::Map<const Eigen::ArrayXi> n(counts, size);
    return stan::math::lgamma(1 + n.sum()) - stan::math::lgamma(1 + n).sum();
  }
  double total = 1.0;
  double value = 0.0;
  for (int64_t i = 0; i < size; ++i) {
    total += counts[i];
    value -= stan::math::lgamma(counts[i] + 1.0);
  }
  return value + stan::math::lgamma(total);
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

std::vector<int> pack_all_integer_density_arguments(
    const std::vector<IntegerDensityArgument>& arguments) {
  if (arguments.size() > static_cast<size_t>(kMaxIntGroups))
    throw std::logic_error("too many all-integer density arguments");
  std::vector<int> packed;
  int scalar_mask = 0;
  for (size_t k = 0; k < arguments.size(); ++k) {
    const IntegerDensityArgument& argument = arguments[k];
    if (argument.scalar && argument.values.size() != 1)
      throw std::logic_error("malformed scalar all-integer density argument");
    append_int_group(packed, argument.values.data(),
                     static_cast<int64_t>(argument.values.size()));
    if (argument.scalar) scalar_mask |= 1 << k;
  }
  packed.push_back(scalar_mask);
  return packed;
}

double evaluate_packed_all_integer_density(AllIntegerDensity density,
                                           const int* packed, int64_t size,
                                           bool propto) {
  const int arity = density == AllIntegerDensity::HypergeometricLpmf ? 4
                    : density == AllIntegerDensity::None             ? 0
                                                                     : 3;
  if (arity == 0) throw std::logic_error("unknown all-integer density");
  const IntGroups groups = int_groups(packed, size, arity);
  std::vector<IntegerDensityArgument> arguments;
  arguments.reserve((size_t)arity);
  for (int k = 0; k < arity; ++k) {
    const IntGroup& g = groups.group[k];
    arguments.push_back({{g.values, g.values + g.count}, g.scalar});
  }
  return evaluate_all_integer(density, arguments, propto);
}

// Stan Math's check_consistent_sizes over the arguments that the density's
// function names: the first container sets the size, and the first container
// of another size is the error. The kernels bind a one-element argument as a
// scalar, so this check is the only place where it is still a container.
void check_consistent_density_sizes(
    const DensitySpec& spec,
    const std::vector<DensityCallArgument>& arguments) {
  const DensitySizeCheck& check = *spec.size_check;
  const DensitySizeName* first = nullptr;
  size_t first_size = 0;
  for (const DensitySizeName& named : check.arguments) {
    const DensityCallArgument& argument = arguments.at(named.argument);
    const bool integer = named.argument < spec.integer_args;
    if (integer ? argument.scalar
                : argument.shape.container == FunctionContainerKind::Scalar)
      continue;
    const size_t size = integer
                            ? argument.integers.size()
                            : static_cast<size_t>(argument.shape.storage_size);
    if (first == nullptr) {
      first = &named;
      first_size = size;
      continue;
    }
    if (size == first_size) continue;
    try {
      stan::math::check_consistent_sizes(check.function, first->name,
                                         std::vector<double>(first_size),
                                         named.name, std::vector<double>(size));
    } catch (const std::invalid_argument& error) {
      const std::string prefix = std::string(check.function) + ": ";
      std::string message = error.what();
      if (message.compare(0, prefix.size(), prefix) == 0)
        message.erase(0, prefix.size());
      throw DensitySizeError(message);
    }
  }
}

// Stan Math's check_consistent_size for a GLM argument that may be a scalar
// or one value per instance.
void check_glm_argument_size(const char* function, const char* name,
                             const DensityArgumentShape& shape,
                             int64_t instances) {
  if (shape.container == FunctionContainerKind::Scalar ||
      shape.storage_size == instances)
    return;
  try {
    stan::math::check_consistent_size(function, name,
                                      Eigen::VectorXd(shape.storage_size),
                                      static_cast<size_t>(instances));
  } catch (const std::invalid_argument& error) {
    const std::string prefix = std::string(function) + ": ";
    std::string message = error.what();
    if (message.compare(0, prefix.size(), prefix) == 0)
      message.erase(0, prefix.size());
    throw DensitySizeError(message);
  }
}

// multi_normal, multi_normal_cholesky and multi_normal_prec compare the
// random variable with the location and then with the matrix, in this order.
void check_multi_normal_sizes(
    const std::vector<DensityArgumentShape>& arguments, int64_t matrix_rows) {
  int64_t sizes[2];
  for (size_t k = 0; k < 2; ++k) {
    const DensityArgumentShape& arg = arguments[k];
    const FunctionContainerKind leaf = function_leaf(arg);
    if ((leaf != FunctionContainerKind::Vector &&
         leaf != FunctionContainerKind::RowVector) ||
        arg.dimensions.size() != (size_t)arg.array_depth + 1)
      return;
    for (size_t d = 0; d < arg.array_depth; ++d)
      if (arg.dimensions[d] == 0) return;
    sizes[k] = arg.dimensions.back();
  }
  const auto mismatch = [](int64_t y, const char* what, int64_t other) {
    return DensitySizeError("Size of random variable (" + std::to_string(y) +
                            ") and " + what + " (" + std::to_string(other) +
                            ") must match in size");
  };
  if (sizes[0] != sizes[1])
    throw mismatch(sizes[0], "size of location parameter", sizes[1]);
  if (sizes[0] != matrix_rows)
    throw mismatch(sizes[0], "rows of covariance parameter", matrix_rows);
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

  if (spec.shape == DensityShape::LastMatrixRowsAndRepetitions &&
      spec.arity == 3)
    check_multi_normal_sizes(arguments, width);

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
      result.vector_counts.push_back(1);
    } else {
      result.array_mask |= 1 << result.vector_counts.size();
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
    plan.variant = AllIntegerMode{spec.all_integer, propto}.byte();
    return plan;
  }

  if (spec.size_check != nullptr)
    check_consistent_density_sizes(spec, arguments);

  std::vector<std::vector<int>> integer_groups;
  int glm_flags = 0;
  bool scalar_outcome = false;
  if (spec.integer_args == 1) {
    plan.idata = arguments[0].integers;
    scalar_outcome = arguments[0].scalar && plan.idata.size() == 1;
    if (scalar_outcome) glm_flags |= kGlmScalarFirstOutcome;
  } else if (spec.integer_args == 2) {
    integer_groups.reserve(2);
    int scalar_mask = 0;
    for (size_t k = 0; k < 2; ++k) {
      const DensityCallArgument& argument = arguments[k];
      if (spec.glm_layout) {
        if (argument.scalar)
          glm_flags |=
              k == 0 ? kGlmScalarFirstOutcome : kGlmScalarSecondOutcome;
        integer_groups.push_back(argument.integers);
      } else {
        if (argument.scalar && argument.integers.size() != 1)
          throw std::logic_error("malformed scalar integer density argument");
        append_int_group(plan.idata, argument.integers.data(),
                         static_cast<int64_t>(argument.integers.size()));
        if (argument.scalar) scalar_mask |= 1 << k;
      }
    }
    if (!spec.glm_layout) plan.idata.push_back(scalar_mask);
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

  // A nonnegative spec mask is the kernel's promise of which real arguments
  // it can differentiate. Parameter-dependent input outside that promise
  // must refuse here -- the one plan choke point every backend shares --
  // because binding it anyway drops the argument's partial silently: the
  // lkj eta mask did exactly that and surfaced only as a 0.5% CmdStan
  // gradient divergence in the signature-reference gate.
  if (spec.activity_mask >= 0 &&
      (plan.activity_mask & ~static_cast<uint8_t>(spec.activity_mask)) != 0)
    throw std::invalid_argument(
        "density argument outside the kernel's differentiable set");

  const auto matrix_dimensions = [&](size_t real_index,
                                     bool allow_row_vector = false) {
    if (real_index >= real_shapes.size())
      throw std::invalid_argument("density is missing a matrix argument");
    const FunctionArgumentShape& shape = real_shapes[real_index];
    if (allow_row_vector &&
        shape.container == FunctionContainerKind::RowVector &&
        shape.array_depth == 0 && shape.dimensions.size() == 1)
      return std::pair<int64_t, int64_t>{1, shape.dimensions[0]};
    if (spec.opcode == OP_NORMAL_ID_GLM_LPDF && allow_row_vector &&
        shape.container == FunctionContainerKind::Vector &&
        shape.array_depth == 0)
      return std::pair<int64_t, int64_t>{shape.dimensions[0], 1};
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
    else {
      // Stan Math returns 0 when every container argument is empty.
      int64_t widest = -1;
      for (const DensityArgumentShape& shape : real_shapes) {
        lanes = std::max(lanes, shape.storage_size);
        if (shape.container != FunctionContainerKind::Scalar)
          widest = std::max(widest, shape.storage_size);
      }
      if (widest == 0) plan.idata.clear();
    }
    if (lanes > 1) {
      const int outcome = plan.idata[0];
      plan.idata.assign(static_cast<size_t>(lanes), outcome);
    }
  }

  if (spec.glm_layout) {
    const auto [rows, cols] = matrix_dimensions(spec.glm_matrix_arg, true);
    int64_t instances = rows;
    const bool row_design = (spec.opcode == OP_POISSON_LOG_GLM_LPMF ||
                             spec.opcode == OP_BERNOULLI_LOGIT_GLM_LPMF ||
                             spec.opcode == OP_NEG_BINOMIAL_2_LOG_GLM_LPMF ||
                             spec.opcode == OP_BINOMIAL_LOGIT_GLM_LPMF) &&
                            real_shapes[spec.glm_matrix_arg].container ==
                                FunctionContainerKind::RowVector;
    if (row_design) {
      glm_flags |= kGlmRowVectorDesign;
      instances =
          spec.integer_args == 1 ? static_cast<int64_t>(plan.idata.size()) : 1;
      for (const std::vector<int>& group : integer_groups)
        instances = std::max(instances, static_cast<int64_t>(group.size()));
    }
    if (spec.opcode == OP_CATEGORICAL_LOGIT_GLM_LPMF) {
      const char* function = "categorical_logit_glm_lpmf";
      const int64_t beta_rows = matrix_dimensions(2).first;
      const int64_t beta_cols = matrix_dimensions(2).second;
      try {
        stan::math::check_consistent_size(
            function, "Intercept vector",
            Eigen::VectorXd(real_shapes[1].storage_size),
            static_cast<size_t>(beta_cols));
        stan::math::check_size_match(function, "x.cols()", cols, "beta.rows()",
                                     beta_rows);
      } catch (const std::invalid_argument& error) {
        throw StanMathError(error.what());
      }
    }
    if (spec.integer_args == 2) {
      plan.idata.clear();
      for (size_t k = 0; k < integer_groups.size(); ++k) {
        std::vector<int>& group = integer_groups[k];
        if ((glm_flags &
             (k == 0 ? kGlmScalarFirstOutcome : kGlmScalarSecondOutcome)) &&
            instances > 1) {
          const int value = group[0];
          group.assign(static_cast<size_t>(instances), value);
        }
        if (static_cast<int64_t>(group.size()) != instances)
          throw std::invalid_argument(
              "integer density argument does not match matrix rows");
        plan.idata.insert(plan.idata.end(), group.begin(), group.end());
      }
    } else if (spec.integer_args == 1 &&
               static_cast<int64_t>(plan.idata.size()) != instances) {
      throw std::invalid_argument("density outcome does not match matrix rows");
    }
    // The intercept, and normal_id_glm's scale, is a scalar or one value per
    // instance. The kernels read a one-element vector as a shared scalar, so
    // the check Stan Math makes on it happens here.
    const char* glm_function =
        spec.opcode == OP_POISSON_LOG_GLM_LPMF ? "poisson_log_glm_lpmf"
        : spec.opcode == OP_BERNOULLI_LOGIT_GLM_LPMF
            ? "bernoulli_logit_glm_lpmf"
        : spec.opcode == OP_NEG_BINOMIAL_2_LOG_GLM_LPMF
            ? "neg_binomial_2_log_glm_lpmf"
        : spec.opcode == OP_BINOMIAL_LOGIT_GLM_LPMF ? "binomial_logit_glm_lpmf"
        : spec.opcode == OP_NORMAL_ID_GLM_LPDF      ? "normal_id_glm_lpdf"
                                                    : nullptr;
    if (glm_function != nullptr) {
      const size_t alpha = spec.glm_matrix_arg + 1u;
      int64_t expected = instances;
      if (spec.opcode == OP_NORMAL_ID_GLM_LPDF &&
          (glm_flags & kGlmRowVectorDesign) != 0)
        expected = real_shapes[0].container == FunctionContainerKind::Scalar
                       ? 1
                       : real_shapes[0].storage_size;
      // binomial_logit_glm takes the instance count from all of n, N, x
      // and alpha.
      if (spec.opcode == OP_BINOMIAL_LOGIT_GLM_LPMF) {
        expected = rows;
        for (size_t k = 0; k < 2; ++k)
          expected = std::max(
              expected, static_cast<int64_t>(arguments[k].integers.size()));
        if (real_shapes[alpha].container != FunctionContainerKind::Scalar)
          expected = std::max(expected, real_shapes[alpha].storage_size);
      }
      if (spec.opcode == OP_NORMAL_ID_GLM_LPDF)
        check_glm_argument_size(glm_function, "Vector of scale parameters",
                                real_shapes[alpha + 2], expected);
      check_glm_argument_size(glm_function, "Vector of intercepts",
                              real_shapes[alpha], expected);
    }
    // normal_id_glm has no integer outcomes; its row_vector design is the
    // same flag.
    if (spec.opcode == OP_NORMAL_ID_GLM_LPDF &&
        real_shapes[spec.glm_matrix_arg].container ==
            FunctionContainerKind::RowVector)
      glm_flags |= kGlmRowVectorDesign;
    append_glm_layout(plan.idata, as_int_extent(rows), as_int_extent(cols),
                      glm_flags);
  }

  if (spec.shape == DensityShape::PoissonBinomial) {
    const auto& theta = arguments[1].shape;
    const auto leaf = function_leaf(theta);
    const bool real_array = theta.container == FunctionContainerKind::Array &&
                            leaf == FunctionContainerKind::Scalar &&
                            theta.array_depth == 1;
    if (!real_array && ((leaf != FunctionContainerKind::Vector &&
                         leaf != FunctionContainerKind::RowVector) ||
                        theta.array_depth != 0))
      throw std::invalid_argument(
          "poisson_binomial expects vector, row_vector or array[] real");
    if (arguments[0].shape.array_depth > 1 || !arguments[0].data_only)
      throw std::invalid_argument(
          "poisson_binomial expects data int or array[] int");
    const int width = as_int_extent(theta.storage_size);
    // Math indexes y[0] even for an empty outcome array. Reject before its
    // unchecked scalar_seq_view access; empty probability vectors are valid.
    if (arguments[0].integers.empty())
      throw std::invalid_argument(
          "poisson_binomial requires nonempty outcomes");
    plan.idata = {width, arguments[0].scalar ? 1 : 0};
    plan.idata.insert(plan.idata.end(), arguments[0].integers.begin(),
                      arguments[0].integers.end());
    plan.variant =
        PoissonBinomialMode{spec.poisson_binomial, plan.activity_mask != 0}
            .byte();
    return plan;
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
    plan.variant =
        (uint8_t)((spec.categorical_logit ? kCategoricalLogit : 0u) |
                  (arguments[1].active ? kCategoricalArgAutodiff : 0u) |
                  (scalar ? kCategoricalScalarOutcome : 0u) |
                  (propto ? kCategoricalPropto : 0u));
  } else {
    // A nonnegative spec mask binds every differentiable argument as
    // autodiff whenever one is; an all-data GLM binds none.
    const bool all_data_glm =
        spec.glm_layout && spec.activity_mask >= 0 && plan.activity_mask == 0;
    const uint8_t active = spec.activity_mask < 0 ? plan.activity_mask
                           : all_data_glm
                               ? uint8_t{0}
                               : static_cast<uint8_t>(spec.activity_mask);
    plan.variant = DensityMode{active, false, propto}.byte();
  }

  // The coefficient depends on the counts alone, so compute it once here
  // instead of in every evaluation. With data probabilities the whole call
  // is data, and constant folding evaluates it once anyway.
  if ((spec.opcode == OP_MULTINOMIAL_LPMF ||
       spec.opcode == OP_MULTINOMIAL_LOGIT_LPMF ||
       spec.opcode == OP_MULTINOMIAL_SOFTMAX_LPMF) &&
      !propto && plan.activity_mask != 0) {
    plan.data_term =
        multinomial_coefficient(spec.opcode, plan.idata.data(),
                                static_cast<int64_t>(plan.idata.size()));
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
    if (!spec.integer_args) plan.idata.clear();
    append_vectorized_layout(plan.idata, as_int_extent(layout.width),
                             layout.vector_counts, layout.array_mask);
  }
  return plan;
}

}  // namespace stanli
