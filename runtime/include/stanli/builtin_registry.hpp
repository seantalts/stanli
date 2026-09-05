// One semantic registry for ordinary pure numeric builtins.  As with
// density_registry.hpp, backends adapt their storage to these neutral shapes,
// ask one resolver to validate broadcasting and choose the result layout, and
// only then materialize the call.
#ifndef STANLI_BUILTIN_REGISTRY_HPP
#define STANLI_BUILTIN_REGISTRY_HPP

#include <stanli/function_shape.hpp>
#include <stanli/optable.hpp>
#include <stanli/rng_family.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace stanli {

using BuiltinArgumentKind = FunctionArgumentKind;
enum class BuiltinShapePolicy : uint8_t {
  Elementwise,
  WholeValue,
  Reduction,
  // Two equal-length containers (or one, paired with itself) folded to one
  // scalar through the dot kernel; `difference` subtracts first.
  PairedReduction,
  // Data-only scalar arguments producing a container constant through the
  // shared Stan Math evaluator; no kernel opcode.
  Constructor,
  // One container plus integer indexes selecting a subset of its cells; the
  // shared resolver computes the source-cell map and result geometry.
  SliceView,
  // Two equal-shape Eigen containers (or one, paired with itself) folded to
  // one scalar per column or row through the grouped dot kernel.
  GroupedReduction,
  // Dense linear algebra through one dedicated kernel per name: the shared
  // resolver validates operand geometry and reports the result shape, the
  // kernel idata, and the shape half of the variant bits.
  MatrixOp,
  // Integer questions about a value's logical geometry, answered from shape
  // metadata alone -- no kernel, no lanes, no gradient.
  ShapeQuery,
  // Scalar 0/1 answers computed on values read through value_of: the
  // comparison operators and their logical_* library spellings, logical
  // negation, and the IEEE classifications. No kernel and no adjoint edge.
  Predicate,
  // The scalar-argument RNG tranche: OP_RNG draws whose stream belongs to
  // the caller. The descriptor carries the ScalarRng family; every backend
  // routes the call to its stream-owning handler rather than a pure kernel.
  Rng,
  // Shaped multiplication (Times__/multiply): the shared resolver
  // classifies the scalar-scale/matvec/GEMM/outer/inner forms from operand
  // geometry alone, validates the inner dimension, and reports the result
  // shape with the m/k/n extents the product kernels take as idata. Each
  // backend chooses its own materialization.
  Product,
  // The linear solves (mdivide_* and their operator spellings): the
  // descriptor carries the divisor side, the factorization kind, and the
  // graph opcode; the shared resolver validates the square divisor and
  // conformable dividend and reports the result shape.
  Solve,
};

// Which factorization a Solve descriptor's kernel applies to the divisor.
enum class BuiltinSolveKind : uint8_t { None, Plain, Spd, TriLow };

// Which predicate a Predicate descriptor evaluates. The operator spellings
// (Equals__, PNot__, ...) and the logical_* library names register the same
// descriptors, so every backend answers both identically.
enum class BuiltinPredicate : uint8_t {
  None,
  Eq,
  Neq,
  Lt,
  Lte,
  Gt,
  Gte,
  And,
  Or,
  Negation,
  IsNan,
  IsInf,
};

// Which extent a ShapeQuery descriptor reads. FnLength, the compiler
// internal stanc3 emits for vectorized observation counts, stays outside
// the registry: the interpreter answers it as num_elements while the
// compile-time evaluators answer it as size, a standing divergence that
// predates the registry.
enum class BuiltinShapeQueryKind : uint8_t {
  None,
  Rows,
  Cols,
  Size,
  NumElements,
  Dims,
};

// Which matrix kernel a MatrixOp descriptor drives. The kernels themselves
// (matrix_fns.cpp) own the Stan Math calls, the overload-selection variant
// semantics, and the value-dependent checks (symmetry, positive
// definiteness); the resolver owns shapes alone.
enum class BuiltinMatrixOp : uint8_t {
  None,
  CholeskyDecompose,
  MatrixExp,
  Inverse,
  InverseSpd,
  LogDeterminant,
  EigenvaluesSym,
  EigenvectorsSym,
  Crossprod,
  MultiplyLowerTriSelfTranspose,
  DiagMatrix,
  AddDiag,
  QuadForm,
  QuadFormSym,
};

enum class BuiltinSlice : uint8_t {
  None,
  Head,
  Tail,
  Segment,
  Col,
  Row,
  SubCol,
  Block,
  Diagonal,
  Reverse,
  // Reshapes: identity or transpose permutations over the same machinery.
  Transpose,
  ToVector,
  ToRowVector,
  ToMatrix,
  ToArray1d,
  // Expansions: broadcasts of one value or container, and two-container
  // concatenations mapped over the inputs' concatenated cell space.
  RepVector,
  RepRowVector,
  RepMatrix,
  RepArray,
  AppendRow,
  AppendCol,
  AppendArray,
  // GroupedReduction axes: one dot product per column or per row.
  ColumnsDot,
  RowsDot,
};

// How the caller's flat storage arranges a container's logical cells.
// Matrices are column-major in every backend; only arrays differ.
enum class SliceStorageOrder : uint8_t {
  // Each outer array element is one contiguous chunk (graph slots).
  OuterMajor,
  // One flat first-index-fast layout over array extents followed by leaf
  // extents (DataMap, interpreter values, and register-machine ranges).
  FirstIndexFast,
};

enum class BuiltinConstructor : uint8_t {
  None,
  Zeros,
  Ones,
  LinSpaced,
  Identity,
  OneHot,
  UniformSimplex,
};
enum class BuiltinCompatibilityPolicy : uint8_t {
  LogicalShape,
  // Stan permits the integer container paired with a real container to have
  // a different surface type, provided their vectorized lane counts agree.
  LaneCount,
};

using BuiltinContainerKind = FunctionContainerKind;
using BuiltinArgumentShape = FunctionArgumentShape;

struct BuiltinLayout {
  int64_t lanes = 1;
  uint8_t result_argument = 0;
  // Nonzero for the sole storage-order mismatch: an integer array paired
  // lane-wise with a real matrix (or array of matrices).
  int64_t integer_matrix_rows = 0;
  int64_t integer_matrix_cols = 0;
};

struct BuiltinSpec {
  uint16_t opcode = OP_NONE_;
  uint8_t arity = 0;
  // Value-initialized entries are Real; SliceView's block needs five.
  std::array<BuiltinArgumentKind, 5> arguments{};
  FunctionArgumentKind result = FunctionArgumentKind::Real;
  BuiltinShapePolicy shape = BuiltinShapePolicy::Elementwise;
  BuiltinCompatibilityPolicy compatibility =
      BuiltinCompatibilityPolicy::LogicalShape;
  uint8_t activity_mask = 0;
  // Reductions which reject an empty container (sd, variance).
  bool nonempty_input = false;
  // PairedReduction: fold the elementwise difference (squared_distance).
  bool difference = false;
  // Constructor descriptors: what is built and which container holds it.
  BuiltinConstructor constructor = BuiltinConstructor::None;
  FunctionContainerKind constructor_container = FunctionContainerKind::Scalar;
  // SliceView: which selection this descriptor performs. GroupedReduction:
  // which grouping axis (ColumnsDot or RowsDot).
  BuiltinSlice slice = BuiltinSlice::None;
  // MatrixOp: which matrix kernel this descriptor drives.
  BuiltinMatrixOp matrix_op = BuiltinMatrixOp::None;
  // ShapeQuery: which extent this descriptor reads.
  BuiltinShapeQueryKind shape_query = BuiltinShapeQueryKind::None;
  // Predicate: which 0/1 answer this descriptor evaluates.
  BuiltinPredicate predicate = BuiltinPredicate::None;
  // Rng: which scalar family this descriptor draws (valid only under the
  // Rng policy; the field's default is meaningless elsewhere).
  ScalarRng rng = ScalarRng::PoissonLog;
  // Solve: which side the divisor sits on and which factorization applies;
  // the opcode field carries the solve kernel.
  bool solve_left = false;
  BuiltinSolveKind solve = BuiltinSolveKind::None;
};

inline bool builtin_argument_is_integer(const BuiltinSpec& spec, size_t i) {
  return i < spec.arity && spec.arguments[i] == BuiltinArgumentKind::Integer;
}

// Validate one selected overload and resolve its lane broadcasts and result
// geometry once for every backend. A one-element container is deliberately
// not a scalar and therefore never broadcasts.
BuiltinLayout builtin_layout(
    const BuiltinSpec& spec,
    const std::vector<BuiltinArgumentShape>& arguments);

struct FunctionSpec;
// Family views point into function_specs(); no family owns descriptor storage.
const std::vector<const FunctionSpec*>& builtin_specs();
const BuiltinSpec* builtin_spec(std::string_view name, size_t arity);
// Policy-checked name-and-arity lookups. Each name carrying one of these
// policies has exactly one registered overload -- or, for SliceView names
// with an integer-preserving result, one Real and one Integer overload that
// share every policy field -- so resolution needs no numeric argument
// metadata and legacy hand-built MIR dispatches identically.
const BuiltinSpec* shaped_builtin_spec(std::string_view name, size_t arity,
                                       BuiltinShapePolicy shape);
const BuiltinSpec* reduction_builtin_spec(std::string_view name, size_t arity);

// Evaluate one Constructor descriptor over its scalar data arguments through
// the original Stan Math builders, so extents, spacing rules, and domain
// errors are CmdStan's own. `integers` is filled when the result is Integer;
// `values` always carries the double mirror. Matrix values are column-major.
struct ConstructorValue {
  std::vector<double> values;
  std::vector<int> integers;
  std::vector<int64_t> dimensions;
};
ConstructorValue evaluate_constructor_builtin(
    const BuiltinSpec& spec, const std::vector<double>& arguments);

// One SliceView selection resolved to flat source cells: every output cell k
// reads exactly one input cell -- offset + k (Contiguous), offset + k*stride
// (Strided), or gather[k] (Gather) -- in the caller's declared storage order.
// `count` is the output storage size and `result` its logical geometry.
// Transpose names the one common permutation so a backend with a native
// transpose kernel can keep it: with `result` an m-by-n matrix (column-major
// storage), output cell k = i + m*j reads source cell j + n*i, i.e. the
// source is a column-major n-by-m view transposed.
struct BuiltinSliceMap {
  enum class Kind : uint8_t { Contiguous, Strided, Gather, Transpose };
  Kind kind = Kind::Contiguous;
  int64_t offset = 0;
  int64_t stride = 1;
  int64_t count = 0;
  std::vector<int64_t> gather;
  BuiltinArgumentShape result;
};

// Reshape descriptors relabel or permute the whole container, so an identity
// (Contiguous, offset zero, full length) map from one may alias its input
// outright -- the choice the pre-registry backends made -- where the
// selection kinds always materialize a copy.
inline bool builtin_slice_is_reshape(BuiltinSlice slice) {
  return slice == BuiltinSlice::Transpose || slice == BuiltinSlice::ToVector ||
         slice == BuiltinSlice::ToRowVector ||
         slice == BuiltinSlice::ToMatrix || slice == BuiltinSlice::ToArray1d;
}

// Appends take two containers and map result cells over their concatenated
// storage: a source cell below the left operand's storage size reads the
// left operand, and one at or above it reads the right operand shifted down.
inline bool builtin_slice_is_append(BuiltinSlice slice) {
  return slice == BuiltinSlice::AppendRow || slice == BuiltinSlice::AppendCol ||
         slice == BuiltinSlice::AppendArray;
}

// The expansion descriptors keep their specialized graph lowerings (dynamic
// extents, dedicated broadcast kernels, append_array's data observations);
// the shared resolver serves the interpreter and the register machine.
inline bool builtin_slice_is_expansion(BuiltinSlice slice) {
  return slice == BuiltinSlice::RepVector ||
         slice == BuiltinSlice::RepRowVector ||
         slice == BuiltinSlice::RepMatrix || slice == BuiltinSlice::RepArray ||
         builtin_slice_is_append(slice);
}

// Resolve one SliceView descriptor over the input geometry and its integer
// index arguments (in source order, without the container). Index validation
// reproduces the Stan Math checks the original functions perform, so
// std::out_of_range and std::domain_error are CmdStan's own rejections;
// std::invalid_argument marks an argument no overload accepts.
BuiltinSliceMap builtin_slice_map(const BuiltinSpec& spec,
                                  const BuiltinArgumentShape& input,
                                  const std::vector<int64_t>& indexes,
                                  SliceStorageOrder order);

// One GroupedReduction descriptor resolved to per-group storage geometry:
// result cell g folds `width` source cells starting at g*group_stride,
// spaced cell_stride apart. Matrices are column-major in every backend and
// vectors flat, so the geometry needs no storage-order parameter: columns
// are contiguous runs and rows are rows-strided lanes everywhere.
struct BuiltinGroupedDotMap {
  int64_t groups = 0;
  int64_t width = 0;
  int64_t group_stride = 0;
  int64_t cell_stride = 1;
  BuiltinArgumentShape result;
};

// Resolve one grouped dot over its operand geometry (self forms pass the one
// operand twice). Validation reproduces Stan Math's check_matching_dims
// (std::invalid_argument); a non-Eigen operand is an argument no overload
// accepts.
BuiltinGroupedDotMap builtin_grouped_dot_map(const BuiltinSpec& spec,
                                             const BuiltinArgumentShape& lhs,
                                             const BuiltinArgumentShape& rhs);

// One MatrixOp descriptor resolved over its operand geometry. `idata` is the
// kernel's dimension block, `variant` its shape-derived bits (a vector
// second operand, a scalar diagonal), and `active_variant` the bit a backend
// ORs in when CmdStan would have typed the expression `var` -- zero for the
// kernels with one association. Shape mismatches throw
// std::invalid_argument; value-dependent validation (symmetry, positive
// definiteness) stays in the kernels, which make Stan Math's own calls.
struct BuiltinMatrixMap {
  std::vector<int64_t> idata;
  uint8_t variant = 0;
  uint8_t active_variant = 0;
  BuiltinArgumentShape result;
};
BuiltinMatrixMap builtin_matrix_map(
    const BuiltinSpec& spec, const std::vector<BuiltinArgumentShape>& shapes);

// One shaped multiplication classified from operand geometry alone,
// covering exactly Stan's multiply signatures: a scalar on either side
// scales the other operand elementwise; otherwise matrix*vector (MatVec),
// matrix*matrix and row_vector*matrix (Gemm), vector*row_vector (Outer),
// and row_vector*vector (Inner). m/k/n are the GEMM extents -- the result
// is m-by-n from m-by-k times k-by-n, with a vector operand one column and
// a row_vector one row -- and stay zero for ScalarScale. Inner-dimension
// mismatches and unsupported operand pairs throw std::invalid_argument.
struct BuiltinProductMap {
  enum class Kind : uint8_t { ScalarScale, MatVec, Gemm, Outer, Inner };
  Kind kind = Kind::ScalarScale;
  int64_t m = 0;
  int64_t k = 0;
  int64_t n = 0;
  BuiltinArgumentShape result;
};
BuiltinProductMap builtin_product_map(const BuiltinArgumentShape& a,
                                      const BuiltinArgumentShape& b);

// One Solve descriptor validated: the divisor (left operand for
// solve_left, right otherwise) must be a square matrix and the dividend
// conformable on the divisor's side; the result takes the dividend's
// shape. `order` is the divisor extent and `columns` the number of
// dividend columns the kernel sweeps (1 for a vector or row_vector).
struct BuiltinSolveMap {
  int64_t order = 0;
  int64_t columns = 0;
  BuiltinArgumentShape result;
};
BuiltinSolveMap builtin_solve_map(const BuiltinSpec& spec,
                                  const BuiltinArgumentShape& a,
                                  const BuiltinArgumentShape& b);

// Answer one ShapeQuery descriptor from a value's logical geometry: Dims
// returns every extent (array extents before leaf extents, empty for a
// scalar); the other kinds return one value. `size` is an array's first
// extent and any other value's storage length -- stan::math::size counts a
// matrix's cells -- and rows/cols read a matrix's declared extents or a
// vector's orientation; both are undefined for arrays
// (std::invalid_argument).
std::vector<int64_t> builtin_shape_query(const BuiltinSpec& spec,
                                         const BuiltinArgumentShape& shape);

// Evaluate one Predicate descriptor on scalar values (`rhs` is ignored for
// the unary kinds). Stan Math's semantics: IEEE comparisons on doubles,
// logical_and / logical_or on zero-ness with both sides evaluated, negation
// of zero-ness, and the IEEE classifications.
int evaluate_predicate_builtin(const BuiltinSpec& spec, double lhs,
                               double rhs = 0.0);

// One rvalue Cartesian index selection resolved to flat source cells: the
// single statement of Stan's indexing geometry over both storage orders,
// shared by every backend's generic Indexed path. `positions[k]` lists the
// 0-based selections along logical axis k (already validated -- each
// backend keeps its own bounds checks and messages); `collapse[k]` marks a
// Single index whose axis leaves the result; axes past positions.size()
// keep their full extent. `dimensions` lists the surviving extents in
// logical order, and output cells are enumerated in the same storage-order
// convention as the input, so a backend copies map cells straight into its
// own layout. Contiguous and constant-stride selections are detected so a
// backend with slice opcodes need not emit a gather.
struct BuiltinIndexMap {
  BuiltinSliceMap::Kind kind = BuiltinSliceMap::Kind::Contiguous;
  int64_t offset = 0;
  int64_t stride = 1;
  int64_t count = 0;
  std::vector<int64_t> gather;
  std::vector<int64_t> dimensions;
};
BuiltinIndexMap builtin_index_map(
    const BuiltinArgumentShape& input,
    const std::vector<std::vector<int64_t>>& positions,
    const std::vector<bool>& collapse, SliceStorageOrder order);
// Extents form for callers holding logical dimensions directly. `leaf_rank`
// counts the trailing Eigen axes (it shapes OuterMajor storage only;
// FirstIndexFast is Fortran over every axis regardless).
BuiltinIndexMap builtin_index_map(
    const std::vector<int64_t>& extents, size_t leaf_rank,
    const std::vector<std::vector<int64_t>>& positions,
    const std::vector<bool>& collapse, SliceStorageOrder order);

// Resolve one append descriptor over both operands' geometry. Source cells
// live in the operands' concatenated storage (left first); validation
// reproduces Stan Math's own size checks (std::invalid_argument).
BuiltinSliceMap builtin_append_map(const BuiltinSpec& spec,
                                   const BuiltinArgumentShape& lhs,
                                   const BuiltinArgumentShape& rhs,
                                   SliceStorageOrder order);

int evaluate_integer_binary_builtin(const BuiltinSpec& spec, int lhs, int rhs);
// Opcode-keyed form for callers that already hold the kernel opcode (the
// generated integer kernels); the descriptor form above delegates here.
int evaluate_integer_binary_builtin(uint16_t opcode, int lhs, int rhs);
int evaluate_integer_unary_builtin(const BuiltinSpec& spec, int value);

}  // namespace stanli

#endif
