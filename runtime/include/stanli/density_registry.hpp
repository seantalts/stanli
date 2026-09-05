// One semantic registry for probability functions. Graph lowering, the
// runtime-control register machine, and MIR interpretation use these same
// descriptors; backends only differ in how they materialize an argument.
#ifndef STANLI_DENSITY_REGISTRY_HPP
#define STANLI_DENSITY_REGISTRY_HPP

#include <stanli/function_shape.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace stanli {

enum class DensityShape : uint8_t {
  Plain,
  // The integer outcome remains a runtime input rather than being copied to
  // idata.  This preserves data-only expressions and the scalar-vs-array
  // overload distinction while the vector argument stays one atomic value.
  Categorical,
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
};

struct IntegerDensityArgument {
  std::vector<int> values;
  // A language scalar broadcasts.  A one-element array does not.
  bool scalar = false;
};

using DensityArgumentShape = FunctionArgumentShape;

struct VectorizedDensityLayout {
  int64_t width = 0;
  // One entry for every set bit in DensitySpec::vectorized_vector_args, in
  // source-argument order. -1 denotes a language vector/row_vector; a
  // nonnegative value denotes the number of elements in an array of vectors.
  std::vector<int> vector_counts;
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
  // Function-family bits which are independent of activity and propto.
  uint8_t fixed_variant = 0;
  DensityEvaluationPolicy evaluation = DensityEvaluationPolicy::GraphKernel;
  AllIntegerDensity all_integer = AllIntegerDensity::None;
  // Real-input mask (after integer arguments) for vector/row_vector arguments
  // which also accept an array of vectors.
  uint8_t vectorized_vector_args = 0;
};

// OP_CATEGORICAL's ordinary variant byte replaces its former opaque per-op
// payload.  Bit zero deliberately denotes the only differentiable argument,
// regardless of its input position; the remaining bits describe the Stan
// overload selected by the source call.
inline constexpr uint8_t kCategoricalArgAutodiff = 1u << 0;
inline constexpr uint8_t kCategoricalLogit = 1u << 1;
inline constexpr uint8_t kCategoricalScalarOutcome = 1u << 2;
// Separates integer outcome values from an appended vectorized-layout payload.
inline constexpr int kVectorizedDensityLayoutMarker = -2147483647;
inline constexpr int kGlmScalarLayoutMarker = -2147483646;

// Null means the function needs a genuinely custom contract or is not a
// probability function.
struct FunctionSpec;
const DensitySpec* density_spec(const std::string& name);
// Family views point into function_specs(); no family owns descriptor storage.
const std::vector<const FunctionSpec*>& density_specs();

// Execute a descriptor carrying the AllInteger policy.  The implementation
// dispatches each source argument as either an int or vector<int>, preserving
// Stan Math's vectorization, validation order, and propto support checks.
double evaluate_all_integer_density(
    const DensitySpec& spec,
    const std::vector<IntegerDensityArgument>& arguments, bool propto);
std::vector<int> pack_all_integer_density_arguments(
    const std::vector<IntegerDensityArgument>& arguments);
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

}  // namespace stanli

#endif
