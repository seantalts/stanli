// Resolvers for probability functions over the one function table in
// function_registry.cpp. Graph lowering, the
// runtime-control register machine, and MIR interpretation use these same
// descriptors; backends only differ in how they materialize an argument.
#ifndef STANLI_DENSITY_REGISTRY_HPP
#define STANLI_DENSITY_REGISTRY_HPP

#include <stanli/density_layout.hpp>
#include <stanli/function_shape.hpp>
#include <stanli/kernel_types.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace stanli {

// The statistic a packed wiener op computes.
enum class WienerKind : int { Lpdf = 0, Lcdf = 1, Lccdf = 2 };

enum class DensityShape : uint8_t {
  Plain,
  // The integer outcome remains a runtime input rather than being copied to
  // idata.  This preserves data-only expressions and the scalar-vs-array
  // overload distinction while the vector argument stays one atomic value.
  Categorical,
  PoissonBinomial,
  FirstMatrixRows,
  FirstMatrixDimensions,
  // Two or more vector/row_vector arguments, each of which may independently
  // be an array of vectors. The innermost vector width and array counts are
  // validated and encoded once for every backend.
  VectorizedVectors,
  LastMatrixRowsAndRepetitions,
};

// Most registered probability functions become graph-kernel calls.  A
// density whose complete signature is integer-valued has no differentiable
// edge to attach such a call to; evaluate it directly, but keep that choice
// in the same registry so every MIR backend gets the same support.
enum class DensityEvaluationPolicy : uint8_t {
  GraphKernel,
  AllInteger,
};

enum class AllIntegerDensity : uint8_t {
  None,
  HypergeometricLpmf,
  DiscreteRangeLpmf,
  DiscreteRangeCdf,
  DiscreteRangeLcdf,
  DiscreteRangeLccdf,
};

// OP_ALL_INTEGER_DENSITY's variant: which density, and propto.
struct AllIntegerMode {
  AllIntegerDensity density = AllIntegerDensity::None;
  bool propto = false;

  constexpr uint8_t byte() const {
    return (uint8_t)((uint8_t)density |
                     (propto ? DensityMode::kProptoBit : 0u));
  }
  static constexpr AllIntegerMode of(uint8_t byte) {
    return {(AllIntegerDensity)(byte & ~DensityMode::kProptoBit),
            (byte & DensityMode::kProptoBit) != 0};
  }
};

enum class PoissonBinomialKind : uint8_t { Lpmf, Cdf, Lcdf, Lccdf };

// OP_POISSON_BINOMIAL's variant: the probability function, and whether its
// one real argument is autodiff.
struct PoissonBinomialMode {
  PoissonBinomialKind kind = PoissonBinomialKind::Lpmf;
  bool active = false;

  constexpr uint8_t byte() const {
    return (uint8_t)(((uint8_t)kind << 1) | (active ? 1u : 0u));
  }
  static constexpr PoissonBinomialMode of(uint8_t byte) {
    return {(PoissonBinomialKind)((byte >> 1) & 3u), (byte & 1u) != 0};
  }
};

struct IntegerDensityArgument {
  std::vector<int> values;
  // A language scalar broadcasts.  A one-element array does not.
  bool scalar = false;
};

using DensityArgumentShape = FunctionArgumentShape;

// A size mismatch Stan Math reports when the density runs. The message is
// Stan's, without the leading "<function>: ".
struct DensitySizeError : std::invalid_argument {
  using std::invalid_argument::invalid_argument;
};

struct VectorizedDensityLayout {
  int64_t width = 0;
  // One entry for every set bit in DensitySpec::vectorized_vector_args, in
  // source-argument order: the number of vectors, 1 for a language
  // vector/row_vector.
  std::vector<int> vector_counts;
  // Bit j: entry j is an array of vectors.
  int array_mask = 0;
};

struct DensityCallArgument {
  FunctionArgumentShape shape;
  std::vector<int> integers;
  bool scalar = false;
  bool data_only = true;
  bool active = false;
};

// The shared descriptor for a leading integer argument: a language scalar or
// a rank-one integer array holding the materialized values.
DensityCallArgument integer_density_argument(std::vector<int> values,
                                             bool scalar, bool data_only);

struct DensityCallPlan {
  std::vector<int> idata;
  uint8_t variant = 0;
  uint8_t activity_mask = 0;
  bool empty_result = false;
  // A term of the integer arguments alone, computed once here. Backends
  // pass it to the kernel as one more real input.
  std::optional<double> data_term;
};

struct DensitySpec {
  uint16_t opcode;
  int arity;
  int integer_args;
  bool glm_layout = false;
  DensityShape shape = DensityShape::Plain;
  int activity_mask = -1;  // negative: derive from MIR arguments
  // The single integer group is one outcome per vectorized lane. A scalar
  // outcome may therefore be expanded to the real arguments' lane count.
  bool lane_outcome = false;
  // Index among real arguments of a GLM's design matrix.
  uint8_t glm_matrix_arg = 0;
  DensityEvaluationPolicy evaluation = DensityEvaluationPolicy::GraphKernel;
  AllIntegerDensity all_integer = AllIntegerDensity::None;
  // Real-input mask (after integer arguments) for vector/row_vector arguments
  // which also accept an array of vectors.
  uint8_t vectorized_vector_args = 0;
  bool categorical_logit = false;
  PoissonBinomialKind poisson_binomial = PoissonBinomialKind::Lpmf;
};

// OP_CATEGORICAL's ordinary variant byte replaces its former opaque per-op
// payload.  Bit zero deliberately denotes the only differentiable argument,
// regardless of its input position; the remaining bits describe the Stan
// overload selected by the source call.
inline constexpr uint8_t kCategoricalArgAutodiff = 1u << 0;
inline constexpr uint8_t kCategoricalLogit = 1u << 1;
inline constexpr uint8_t kCategoricalScalarOutcome = 1u << 2;
// Set only by lane partitioning, with kCategoricalScalarOutcome: input 0
// holds one outcome per row and input 1 the rows end to end, each row the
// scalar-outcome call. The output is the rows' lps summed in order.
inline constexpr uint8_t kCategoricalRows = 1u << 3;
inline constexpr uint8_t kCategoricalPropto = DensityMode::kProptoBit;
std::vector<int> pack_all_integer_density_arguments(
    const std::vector<IntegerDensityArgument>& arguments);
// Dispatches each source argument as either an int or vector<int>, preserving
// Stan Math's vectorization, validation order, and propto support checks.
double evaluate_packed_all_integer_density(AllIntegerDensity density,
                                           const int* packed, int64_t size,
                                           bool propto);

// Validate and encode the common (vector-or-array-of-vectors, ..., square
// matrix) signature. This is shared by graph lowering, runtime-control
// lowering, and MIR interpretation; adding another density with this input
// contract requires registry metadata rather than backend-specific handling.
VectorizedDensityLayout vectorized_density_layout(
    const DensitySpec& spec,
    const std::vector<DensityArgumentShape>& arguments);

// Resolve every backend-independent part of a density invocation. Backends
// only adapt their local values into DensityCallArgument and materialize the
// resulting graph call (or its constant all-integer result).
DensityCallPlan density_call_plan(
    const DensitySpec& spec, const std::vector<DensityCallArgument>& arguments,
    bool propto);

// multinomial_lpmf(y | softmax(x)), called with the logits x in place of
// softmax(x). Backends select it with mir::multinomial_softmax_logits.
const DensitySpec& multinomial_softmax_density();

// log(N! / (n_1! ... n_K!)), summed in the order Stan Math uses for the
// opcode's density.
double multinomial_coefficient(uint16_t opcode, const int* counts,
                               int64_t size);

}  // namespace stanli

#endif
