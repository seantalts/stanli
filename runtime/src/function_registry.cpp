#include <stanli/function_registry.hpp>
#include <stanli/mir.hpp>

#include <stdexcept>
#include <unordered_map>

namespace stanli {
namespace {

struct FunctionRegistryIndex {
  std::unordered_multimap<std::string_view, const FunctionSpec*> by_name;
  std::array<std::vector<const FunctionSpec*>, 2> by_family;

  FunctionRegistryIndex() {
    const auto& entries = function_specs();
    by_name.reserve(entries.size());
    for (const FunctionSpec& spec : entries) {
      by_name.emplace(spec.name, &spec);
      by_family[spec.family() == FunctionFamily::Builtin ? 0 : 1].push_back(
          &spec);
    }
  }
};

const FunctionRegistryIndex& function_registry_index() {
  static const FunctionRegistryIndex index;
  return index;
}

size_t family_index(FunctionFamily family) {
  return family == FunctionFamily::Builtin ? 0 : 1;
}

constexpr BuiltinSpec real_binary_builtin(uint16_t opcode) {
  return {opcode,
          2,
          {BuiltinArgumentKind::Real, BuiltinArgumentKind::Real},
          FunctionArgumentKind::Real,
          BuiltinShapePolicy::Elementwise,
          BuiltinCompatibilityPolicy::LogicalShape,
          0x3};
}

constexpr BuiltinSpec mixed_binary_builtin(uint16_t opcode, bool int_first) {
  return {
      opcode,
      2,
      {int_first ? BuiltinArgumentKind::Integer : BuiltinArgumentKind::Real,
       int_first ? BuiltinArgumentKind::Real : BuiltinArgumentKind::Integer},
      FunctionArgumentKind::Real,
      BuiltinShapePolicy::Elementwise,
      BuiltinCompatibilityPolicy::LaneCount,
      static_cast<uint8_t>(int_first ? 0x2 : 0x1)};
}

constexpr BuiltinSpec integer_binary_builtin(uint16_t opcode) {
  return {opcode,
          2,
          {BuiltinArgumentKind::Integer, BuiltinArgumentKind::Integer},
          FunctionArgumentKind::Integer,
          BuiltinShapePolicy::Elementwise,
          BuiltinCompatibilityPolicy::LogicalShape,
          0};
}

constexpr BuiltinSpec integer_unary_builtin(uint16_t opcode) {
  return {opcode,
          1,
          {BuiltinArgumentKind::Integer, BuiltinArgumentKind::Integer},
          FunctionArgumentKind::Integer,
          BuiltinShapePolicy::Elementwise,
          BuiltinCompatibilityPolicy::LogicalShape,
          0};
}

constexpr BuiltinSpec paired_reduction_builtin(uint8_t arity,
                                               bool difference = false) {
  BuiltinSpec spec{OP_DOT,
                   arity,
                   {BuiltinArgumentKind::Real, BuiltinArgumentKind::Real},
                   FunctionArgumentKind::Real,
                   BuiltinShapePolicy::PairedReduction,
                   BuiltinCompatibilityPolicy::LaneCount,
                   static_cast<uint8_t>(arity == 1 ? 0x1 : 0x3)};
  spec.difference = difference;
  return spec;
}

constexpr BuiltinSpec grouped_dot_builtin(BuiltinSlice axis, uint8_t arity) {
  BuiltinSpec spec{OP_GROUP_DOT,
                   arity,
                   {BuiltinArgumentKind::Real, BuiltinArgumentKind::Real},
                   FunctionArgumentKind::Real,
                   BuiltinShapePolicy::GroupedReduction,
                   BuiltinCompatibilityPolicy::LogicalShape,
                   static_cast<uint8_t>(arity == 1 ? 0x1 : 0x3)};
  spec.slice = axis;
  return spec;
}

constexpr BuiltinSpec product_builtin() {
  return BuiltinSpec{OP_NONE_,
                     2,
                     {BuiltinArgumentKind::Real, BuiltinArgumentKind::Real},
                     FunctionArgumentKind::Real,
                     BuiltinShapePolicy::Product,
                     BuiltinCompatibilityPolicy::LogicalShape,
                     0x3};
}

constexpr BuiltinSpec solve_builtin(uint16_t opcode, bool left,
                                    BuiltinSolveKind kind) {
  BuiltinSpec spec{opcode,
                   2,
                   {BuiltinArgumentKind::Real, BuiltinArgumentKind::Real},
                   FunctionArgumentKind::Real,
                   BuiltinShapePolicy::Solve,
                   BuiltinCompatibilityPolicy::LogicalShape,
                   0x3};
  spec.solve_left = left;
  spec.solve = kind;
  return spec;
}

constexpr BuiltinSpec matrix_builtin(BuiltinMatrixOp op, uint16_t opcode,
                                     uint8_t arity) {
  BuiltinSpec spec{opcode,
                   arity,
                   {BuiltinArgumentKind::Real, BuiltinArgumentKind::Real},
                   FunctionArgumentKind::Real,
                   BuiltinShapePolicy::MatrixOp,
                   BuiltinCompatibilityPolicy::LogicalShape,
                   static_cast<uint8_t>(arity == 1 ? 0x1 : 0x3)};
  spec.matrix_op = op;
  return spec;
}

// Names taking any numeric input register a Real and an Integer variant so
// signature-driven tooling matches both; every lookup dispatches identically.
constexpr BuiltinSpec shape_query_builtin(
    BuiltinShapeQueryKind kind,
    FunctionArgumentKind element = FunctionArgumentKind::Real) {
  BuiltinSpec spec{OP_NONE_,
                   1,
                   {element},
                   FunctionArgumentKind::Integer,
                   BuiltinShapePolicy::ShapeQuery,
                   BuiltinCompatibilityPolicy::LogicalShape,
                   0};
  spec.shape_query = kind;
  return spec;
}

constexpr BuiltinSpec predicate_builtin(BuiltinPredicate kind, uint8_t arity) {
  BuiltinSpec spec{OP_NONE_,
                   arity,
                   {BuiltinArgumentKind::Real, BuiltinArgumentKind::Real},
                   FunctionArgumentKind::Integer,
                   BuiltinShapePolicy::Predicate,
                   BuiltinCompatibilityPolicy::LogicalShape,
                   0};
  spec.predicate = kind;
  return spec;
}

// Not constexpr: arity and result kind come from the enum-keyed helpers the
// OP_RNG kernel and the draw functions already use, so the family's
// properties are stated exactly once.
inline BuiltinSpec rng_builtin(ScalarRng family) {
  BuiltinSpec spec{OP_RNG,
                   static_cast<uint8_t>(scalar_rng_arity(family)),
                   {BuiltinArgumentKind::Real, BuiltinArgumentKind::Real,
                    BuiltinArgumentKind::Real},
                   scalar_rng_is_int(family) ? FunctionArgumentKind::Integer
                                             : FunctionArgumentKind::Real,
                   BuiltinShapePolicy::Rng,
                   BuiltinCompatibilityPolicy::LogicalShape,
                   0};
  spec.rng = family;
  return spec;
}

constexpr BuiltinSpec constructor_builtin(
    BuiltinConstructor kind, uint8_t arity,
    std::array<BuiltinArgumentKind, 5> arguments,
    FunctionContainerKind container,
    FunctionArgumentKind result = FunctionArgumentKind::Real) {
  BuiltinSpec spec{OP_NONE_,
                   arity,
                   arguments,
                   result,
                   BuiltinShapePolicy::Constructor,
                   BuiltinCompatibilityPolicy::LogicalShape,
                   0};
  spec.constructor = kind;
  spec.constructor_container = container;
  return spec;
}

// One container argument (whose element kind is the result kind) followed by
// integer indexes. Names whose result preserves an integer input register a
// Real and an Integer variant; the two share every policy field, so any
// name-and-arity lookup dispatches identically.
constexpr BuiltinSpec slice_builtin(
    BuiltinSlice kind, uint8_t arity,
    FunctionArgumentKind element = FunctionArgumentKind::Real) {
  BuiltinSpec spec{
      OP_NONE_,
      arity,
      {element, BuiltinArgumentKind::Integer, BuiltinArgumentKind::Integer,
       BuiltinArgumentKind::Integer, BuiltinArgumentKind::Integer},
      element,
      BuiltinShapePolicy::SliceView,
      BuiltinCompatibilityPolicy::LogicalShape,
      0x1};
  spec.slice = kind;
  return spec;
}

// Two containers of one element kind, mapped over their concatenated cells.
constexpr BuiltinSpec append_builtin(
    BuiltinSlice kind,
    FunctionArgumentKind element = FunctionArgumentKind::Real) {
  BuiltinSpec spec{OP_NONE_,
                   2,
                   {element, element},
                   element,
                   BuiltinShapePolicy::SliceView,
                   BuiltinCompatibilityPolicy::LogicalShape,
                   0x3};
  spec.slice = kind;
  return spec;
}

constexpr BuiltinSpec reduction_builtin(uint16_t opcode,
                                        bool nonempty_input = false) {
  BuiltinSpec spec{opcode,
                   1,
                   {BuiltinArgumentKind::Real, BuiltinArgumentKind::Real},
                   FunctionArgumentKind::Real,
                   BuiltinShapePolicy::Reduction,
                   BuiltinCompatibilityPolicy::LogicalShape,
                   0x1};
  spec.nonempty_input = nonempty_input;
  return spec;
}

constexpr BuiltinSpec unary_builtin(uint16_t opcode) {
  const bool whole_value =
      opcode == OP_SOFTMAX || opcode == OP_LOG_SOFTMAX || opcode == OP_CUMSUM;
  return {opcode,
          1,
          {BuiltinArgumentKind::Real, BuiltinArgumentKind::Real},
          FunctionArgumentKind::Real,
          whole_value ? BuiltinShapePolicy::WholeValue
                      : BuiltinShapePolicy::Elementwise,
          BuiltinCompatibilityPolicy::LogicalShape,
          0x1};
}

DensitySpec all_integer_spec(int arity, AllIntegerDensity density) {
  DensitySpec spec{OP_ALL_INTEGER_DENSITY, arity, arity};
  spec.fixed_variant = static_cast<uint8_t>(density);
  spec.evaluation = DensityEvaluationPolicy::AllInteger;
  spec.all_integer = density;
  return spec;
}

DensitySpec vectorized_mvt_spec(uint16_t opcode, int arity, uint8_t vector_args,
                                int activity_mask = -1) {
  DensitySpec spec{opcode, arity, 0, false,
                   DensityShape::LastMatrixRowsAndRepetitions};
  spec.activity_mask = activity_mask;
  spec.vectorized_vector_args = vector_args;
  return spec;
}

}  // namespace

FunctionFamily FunctionSpec::family() const {
  return std::holds_alternative<BuiltinSpec>(payload) ? FunctionFamily::Builtin
                                                      : FunctionFamily::Density;
}

const BuiltinSpec* FunctionSpec::builtin() const {
  return std::get_if<BuiltinSpec>(&payload);
}

const DensitySpec* FunctionSpec::density() const {
  return std::get_if<DensitySpec>(&payload);
}

uint16_t FunctionSpec::opcode() const {
  if (const BuiltinSpec* spec = builtin()) return spec->opcode;
  return density()->opcode;
}

size_t FunctionSpec::arity() const {
  if (const BuiltinSpec* spec = builtin()) return spec->arity;
  return static_cast<size_t>(density()->arity);
}

FunctionArgumentKind FunctionSpec::result() const {
  if (const BuiltinSpec* spec = builtin()) return spec->result;
  return FunctionArgumentKind::Real;
}

int FunctionSpec::activity_mask() const {
  if (const BuiltinSpec* spec = builtin()) return spec->activity_mask;
  return density()->activity_mask;
}

FunctionArgumentKind FunctionSpec::argument_kind(size_t index) const {
  if (index >= arity()) throw std::out_of_range("function argument index");
  if (const BuiltinSpec* spec = builtin()) return spec->arguments[index];
  // Density kernels keep their integer payload first, followed by real graph
  // inputs. This is an explicit part of DensitySpec's existing contract.
  const bool categorical_outcome =
      density()->shape == DensityShape::Categorical && index == 0;
  return categorical_outcome ||
                 index < static_cast<size_t>(density()->integer_args)
             ? FunctionArgumentKind::Integer
             : FunctionArgumentKind::Real;
}

bool FunctionSpec::accepts(size_t call_arity,
                           uint64_t integer_arguments) const {
  if (call_arity != arity() || call_arity > 64) return false;
  const uint64_t valid =
      call_arity == 64 ? ~uint64_t{0} : (uint64_t{1} << call_arity) - 1;
  if ((integer_arguments & ~valid) != 0) return false;
  for (size_t index = 0; index < call_arity; ++index) {
    const bool actual_integer =
        (integer_arguments & (uint64_t{1} << index)) != 0;
    if (argument_kind(index) == FunctionArgumentKind::Integer &&
        !actual_integer)
      return false;
  }
  return true;
}

const std::vector<FunctionSpec>& function_specs() {
  static const std::vector<FunctionSpec> entries = [] {
    std::vector<FunctionSpec> result;
    result.reserve(384);
    const auto builtin = [&](std::string_view name, BuiltinSpec spec) {
      result.emplace_back(name, std::move(spec));
    };
    builtin("log_sum_exp", real_binary_builtin(OP_LSE2));
    builtin("log_diff_exp", real_binary_builtin(OP_LOG_DIFF_EXP));
    builtin("Plus__", real_binary_builtin(OP_ADD));
    builtin("Minus__", real_binary_builtin(OP_SUB));
    builtin("Divide__", real_binary_builtin(OP_DIV));
    builtin("EltTimes__", real_binary_builtin(OP_MUL));
    builtin("EltDivide__", real_binary_builtin(OP_DIV));
    builtin("Pow__", real_binary_builtin(OP_POW));
    builtin("EltPow__", real_binary_builtin(OP_POW));
    builtin("pow", real_binary_builtin(OP_POW));
    builtin("add", real_binary_builtin(OP_ADD));
    builtin("subtract", real_binary_builtin(OP_SUB));
    builtin("divide", real_binary_builtin(OP_DIV));
    builtin("elt_multiply", real_binary_builtin(OP_MUL));
    builtin("elt_divide", real_binary_builtin(OP_DIV));
    builtin("Plus__", integer_binary_builtin(OP_ADD));
    builtin("Minus__", integer_binary_builtin(OP_SUB));
    builtin("Times__", integer_binary_builtin(OP_MUL));
    builtin("Divide__", integer_binary_builtin(OP_DIV));
    builtin("IntDivide__", integer_binary_builtin(OP_DIV));
    builtin("EltTimes__", integer_binary_builtin(OP_MUL));
    builtin("EltDivide__", integer_binary_builtin(OP_DIV));
    builtin("add", integer_binary_builtin(OP_ADD));
    builtin("subtract", integer_binary_builtin(OP_SUB));
    builtin("divide", integer_binary_builtin(OP_DIV));
    builtin("multiply", integer_binary_builtin(OP_MUL));
    builtin("elt_multiply", integer_binary_builtin(OP_MUL));
    builtin("elt_divide", integer_binary_builtin(OP_DIV));
#define STANLI_BUILTIN_REAL_BINARY(code, fn_name, fn) \
  builtin(#fn_name, real_binary_builtin(code));
    STANLI_SCALAR_BINARY_LIST(STANLI_BUILTIN_REAL_BINARY)
#undef STANLI_BUILTIN_REAL_BINARY
    builtin("multiply_log", real_binary_builtin(OP_LMULTIPLY));
    // Stan Math spellings of names the scalar lists hold as lchoose and abs.
    builtin("binomial_coefficient_log", real_binary_builtin(OP_LCHOOSE));
#define STANLI_BUILTIN_INT_FIRST(code, fn_name, fn) \
  builtin(#fn_name, mixed_binary_builtin(code, true));
    STANLI_SCALAR_BINARY_INT_FIRST_LIST(STANLI_BUILTIN_INT_FIRST)
#undef STANLI_BUILTIN_INT_FIRST
#define STANLI_BUILTIN_INT_SECOND(code, fn_name, fn) \
  builtin(#fn_name, mixed_binary_builtin(code, false));
    STANLI_SCALAR_BINARY_INT_SECOND_LIST(STANLI_BUILTIN_INT_SECOND)
#undef STANLI_BUILTIN_INT_SECOND
#define STANLI_BUILTIN_INTEGER_BINARY(code, fn_name, fn) \
  builtin(#fn_name, integer_binary_builtin(code));
    STANLI_SCALAR_BINARY_INTEGER_LIST(STANLI_BUILTIN_INTEGER_BINARY)
#undef STANLI_BUILTIN_INTEGER_BINARY
#define STANLI_BUILTIN_UNARY(code, fn_name, value, delta, topology) \
  builtin(#fn_name, unary_builtin(code));
    STANLI_SCALAR_UNARY_LIST(STANLI_BUILTIN_UNARY)
#undef STANLI_BUILTIN_UNARY
    builtin("PMinus__", unary_builtin(OP_NEG));
    builtin("minus", unary_builtin(OP_NEG));
    builtin("PMinus__", integer_unary_builtin(OP_NEG));
    builtin("minus", integer_unary_builtin(OP_NEG));
    builtin("abs", integer_unary_builtin(OP_ABS));
    builtin("fabs", unary_builtin(OP_ABS));
    builtin("std_normal_qf", unary_builtin(OP_INV_PHI));
    builtin("trigamma", unary_builtin(OP_TRIGAMMA));
    builtin("exp", unary_builtin(OP_EXPV));
    builtin("log", unary_builtin(OP_LOGV));
    builtin("inv_logit", unary_builtin(OP_INV_LOGIT));
    builtin("logit", unary_builtin(OP_LOGIT));
    builtin("sqrt", unary_builtin(OP_SQRT));
    builtin("square", unary_builtin(OP_SQUARE));
    builtin("log1m", unary_builtin(OP_LOG1M));
    builtin("softmax", unary_builtin(OP_SOFTMAX));
    builtin("tanh", unary_builtin(OP_TANHV));
    builtin("cumulative_sum", unary_builtin(OP_CUMSUM));
    builtin("log_softmax", unary_builtin(OP_LOG_SOFTMAX));
    // Whole-container-to-scalar reductions. sum's integer-container overload
    // and prod/min/max's provenance-aware phased lowerings stay outside the
    // registry deliberately.
    builtin("sum", reduction_builtin(OP_SUM_VEC));
    builtin("mean", reduction_builtin(OP_MEAN));
    builtin("sd", reduction_builtin(OP_SD, true));
    builtin("variance", reduction_builtin(OP_VARIANCE, true));
    builtin("log_sum_exp", reduction_builtin(OP_LOG_SUM_EXP));
    builtin("dot_product", paired_reduction_builtin(2));
    builtin("dot_self", paired_reduction_builtin(1));
    builtin("squared_distance", paired_reduction_builtin(2, true));
    // Grouped dots: one dot per column or row through the shared grouped
    // kernel, whose in-order accumulation matches the AoS reverse-mode
    // overloads CmdStan's model block instantiates.
    builtin("columns_dot_product",
            grouped_dot_builtin(BuiltinSlice::ColumnsDot, 2));
    builtin("rows_dot_product", grouped_dot_builtin(BuiltinSlice::RowsDot, 2));
    builtin("columns_dot_self",
            grouped_dot_builtin(BuiltinSlice::ColumnsDot, 1));
    builtin("rows_dot_self", grouped_dot_builtin(BuiltinSlice::RowsDot, 1));
    // Dense linear algebra with a dedicated kernel per name. The composite
    // lowerings (tcrossprod, diag_pre/post_multiply, quad_form_diag), the
    // solves the division operators share, and gp_exp_quad_cov's
    // array-argument inference stay on their specialized paths.
    // Shaped multiplication and the linear solves. Times__ also carries the
    // integer scalar overload registered above; the operator spellings and
    // the library names share one descriptor each, like the predicates.
    builtin("Times__", product_builtin());
    builtin("multiply", product_builtin());
    builtin("mdivide_left",
            solve_builtin(OP_MDIVIDE_LEFT, true, BuiltinSolveKind::Plain));
    builtin("mdivide_right",
            solve_builtin(OP_MDIVIDE_RIGHT, false, BuiltinSolveKind::Plain));
    builtin("mdivide_left_spd",
            solve_builtin(OP_MDIVIDE_LEFT_SPD, true, BuiltinSolveKind::Spd));
    builtin("mdivide_right_spd",
            solve_builtin(OP_MDIVIDE_RIGHT_SPD, false, BuiltinSolveKind::Spd));
    builtin("mdivide_left_tri_low", solve_builtin(OP_MDIVIDE_LEFT_TRI_LOW, true,
                                                  BuiltinSolveKind::TriLow));
    builtin("mdivide_right_tri_low",
            solve_builtin(OP_MDIVIDE_RIGHT_TRI_LOW, false,
                          BuiltinSolveKind::TriLow));
    builtin("LDivide__",
            solve_builtin(OP_MDIVIDE_LEFT, true, BuiltinSolveKind::Plain));
    builtin("cholesky_decompose",
            matrix_builtin(BuiltinMatrixOp::CholeskyDecompose, OP_CHOLESKY, 1));
    builtin("matrix_exp",
            matrix_builtin(BuiltinMatrixOp::MatrixExp, OP_MATRIX_EXP, 1));
    builtin("inverse", matrix_builtin(BuiltinMatrixOp::Inverse, OP_INVERSE, 1));
    builtin("inverse_spd",
            matrix_builtin(BuiltinMatrixOp::InverseSpd, OP_INVERSE_SPD, 1));
    builtin("log_determinant", matrix_builtin(BuiltinMatrixOp::LogDeterminant,
                                              OP_LOG_DETERMINANT, 1));
    builtin("eigenvalues_sym", matrix_builtin(BuiltinMatrixOp::EigenvaluesSym,
                                              OP_EIGENVALUES_SYM, 1));
    builtin("eigenvectors_sym", matrix_builtin(BuiltinMatrixOp::EigenvectorsSym,
                                               OP_EIGENVECTORS_SYM, 1));
    builtin("crossprod",
            matrix_builtin(BuiltinMatrixOp::Crossprod, OP_CROSSPROD, 1));
    builtin("multiply_lower_tri_self_transpose",
            matrix_builtin(BuiltinMatrixOp::MultiplyLowerTriSelfTranspose,
                           OP_MULT_LOWER_TRI_SELF_TRANSPOSE, 1));
    builtin("diag_matrix",
            matrix_builtin(BuiltinMatrixOp::DiagMatrix, OP_DIAG_MATRIX, 1));
    builtin("add_diag",
            matrix_builtin(BuiltinMatrixOp::AddDiag, OP_ADD_DIAG, 2));
    builtin("quad_form",
            matrix_builtin(BuiltinMatrixOp::QuadForm, OP_QUAD_FORM, 2));
    builtin("quad_form_sym",
            matrix_builtin(BuiltinMatrixOp::QuadFormSym, OP_QUAD_FORM_SYM, 2));
    // Shape queries: integer answers read from logical geometry alone.
    builtin("rows", shape_query_builtin(BuiltinShapeQueryKind::Rows));
    builtin("cols", shape_query_builtin(BuiltinShapeQueryKind::Cols));
    builtin("size", shape_query_builtin(BuiltinShapeQueryKind::Size));
    builtin("size", shape_query_builtin(BuiltinShapeQueryKind::Size,
                                        BuiltinArgumentKind::Integer));
    builtin("num_elements",
            shape_query_builtin(BuiltinShapeQueryKind::NumElements));
    builtin("num_elements",
            shape_query_builtin(BuiltinShapeQueryKind::NumElements,
                                BuiltinArgumentKind::Integer));
    builtin("dims", shape_query_builtin(BuiltinShapeQueryKind::Dims));
    builtin("dims", shape_query_builtin(BuiltinShapeQueryKind::Dims,
                                        BuiltinArgumentKind::Integer));
    // Predicates: the operator spellings and their logical_* library names
    // share one descriptor each, so both dispatch identically everywhere.
    builtin("Equals__", predicate_builtin(BuiltinPredicate::Eq, 2));
    builtin("logical_eq", predicate_builtin(BuiltinPredicate::Eq, 2));
    builtin("NEquals__", predicate_builtin(BuiltinPredicate::Neq, 2));
    builtin("logical_neq", predicate_builtin(BuiltinPredicate::Neq, 2));
    builtin("Less__", predicate_builtin(BuiltinPredicate::Lt, 2));
    builtin("logical_lt", predicate_builtin(BuiltinPredicate::Lt, 2));
    builtin("Leq__", predicate_builtin(BuiltinPredicate::Lte, 2));
    builtin("logical_lte", predicate_builtin(BuiltinPredicate::Lte, 2));
    builtin("Greater__", predicate_builtin(BuiltinPredicate::Gt, 2));
    builtin("logical_gt", predicate_builtin(BuiltinPredicate::Gt, 2));
    builtin("Geq__", predicate_builtin(BuiltinPredicate::Gte, 2));
    builtin("logical_gte", predicate_builtin(BuiltinPredicate::Gte, 2));
    builtin("logical_and", predicate_builtin(BuiltinPredicate::And, 2));
    builtin("logical_or", predicate_builtin(BuiltinPredicate::Or, 2));
    builtin("PNot__", predicate_builtin(BuiltinPredicate::Negation, 1));
    builtin("logical_negation",
            predicate_builtin(BuiltinPredicate::Negation, 1));
    builtin("is_nan", predicate_builtin(BuiltinPredicate::IsNan, 1));
    builtin("is_inf", predicate_builtin(BuiltinPredicate::IsInf, 1));
    // The scalar RNG tranche. The container draws (multi_normal_rng,
    // dirichlet_rng, categorical_rng) and the remaining stan-math draws the
    // interpreted write_array reaches by suffix stay on their bespoke paths.
    builtin("poisson_log_rng", rng_builtin(ScalarRng::PoissonLog));
    builtin("uniform_rng", rng_builtin(ScalarRng::Uniform));
    builtin("bernoulli_rng", rng_builtin(ScalarRng::Bernoulli));
    builtin("normal_rng", rng_builtin(ScalarRng::Normal));
    builtin("lognormal_rng", rng_builtin(ScalarRng::Lognormal));
    builtin("binomial_rng", rng_builtin(ScalarRng::Binomial));
    builtin("gumbel_rng", rng_builtin(ScalarRng::Gumbel));
    builtin("beta_binomial_rng", rng_builtin(ScalarRng::BetaBinomial));
    builtin("exponential_rng", rng_builtin(ScalarRng::Exponential));

    constexpr auto kInt = BuiltinArgumentKind::Integer;
    constexpr auto kReal = BuiltinArgumentKind::Real;
    constexpr auto kVec = FunctionContainerKind::Vector;
    constexpr auto kRow = FunctionContainerKind::RowVector;
    constexpr auto kArr = FunctionContainerKind::Array;
    builtin("zeros_vector",
            constructor_builtin(BuiltinConstructor::Zeros, 1, {kInt}, kVec));
    builtin("zeros_row_vector",
            constructor_builtin(BuiltinConstructor::Zeros, 1, {kInt}, kRow));
    builtin("zeros_int_array",
            constructor_builtin(BuiltinConstructor::Zeros, 1, {kInt}, kArr,
                                FunctionArgumentKind::Integer));
    builtin("zeros_array",
            constructor_builtin(BuiltinConstructor::Zeros, 1, {kInt}, kArr));
    builtin("ones_array",
            constructor_builtin(BuiltinConstructor::Ones, 1, {kInt}, kArr));
    builtin("ones_vector",
            constructor_builtin(BuiltinConstructor::Ones, 1, {kInt}, kVec));
    builtin("ones_row_vector",
            constructor_builtin(BuiltinConstructor::Ones, 1, {kInt}, kRow));
    builtin("ones_int_array",
            constructor_builtin(BuiltinConstructor::Ones, 1, {kInt}, kArr,
                                FunctionArgumentKind::Integer));
    builtin("linspaced_vector",
            constructor_builtin(BuiltinConstructor::LinSpaced, 3,
                                {kInt, kReal, kReal}, kVec));
    builtin("linspaced_row_vector",
            constructor_builtin(BuiltinConstructor::LinSpaced, 3,
                                {kInt, kReal, kReal}, kRow));
    builtin("linspaced_array",
            constructor_builtin(BuiltinConstructor::LinSpaced, 3,
                                {kInt, kReal, kReal}, kArr));
    builtin("linspaced_int_array",
            constructor_builtin(BuiltinConstructor::LinSpaced, 3,
                                {kInt, kInt, kInt}, kArr,
                                FunctionArgumentKind::Integer));
    builtin("identity_matrix",
            constructor_builtin(BuiltinConstructor::Identity, 1, {kInt},
                                FunctionContainerKind::Matrix));
    builtin("one_hot_vector", constructor_builtin(BuiltinConstructor::OneHot, 2,
                                                  {kInt, kInt}, kVec));
    builtin(
        "one_hot_row_vector",
        constructor_builtin(BuiltinConstructor::OneHot, 2, {kInt, kInt}, kRow));
    builtin("one_hot_array", constructor_builtin(BuiltinConstructor::OneHot, 2,
                                                 {kInt, kInt}, kArr));
    builtin("one_hot_int_array",
            constructor_builtin(BuiltinConstructor::OneHot, 2, {kInt, kInt},
                                kArr, FunctionArgumentKind::Integer));
    builtin("uniform_simplex",
            constructor_builtin(BuiltinConstructor::UniformSimplex, 1, {kInt},
                                kVec));

    // Slice/view selections resolved through builtin_slice_map. head, tail,
    // segment, and reverse keep an integer container's element kind, so each
    // registers both element variants.
    builtin("head", slice_builtin(BuiltinSlice::Head, 2));
    builtin("head", slice_builtin(BuiltinSlice::Head, 2, kInt));
    builtin("tail", slice_builtin(BuiltinSlice::Tail, 2));
    builtin("tail", slice_builtin(BuiltinSlice::Tail, 2, kInt));
    builtin("segment", slice_builtin(BuiltinSlice::Segment, 3));
    builtin("segment", slice_builtin(BuiltinSlice::Segment, 3, kInt));
    builtin("reverse", slice_builtin(BuiltinSlice::Reverse, 1));
    builtin("reverse", slice_builtin(BuiltinSlice::Reverse, 1, kInt));
    builtin("col", slice_builtin(BuiltinSlice::Col, 2));
    builtin("row", slice_builtin(BuiltinSlice::Row, 2));
    builtin("sub_col", slice_builtin(BuiltinSlice::SubCol, 4));
    builtin("block", slice_builtin(BuiltinSlice::Block, 5));
    builtin("diagonal", slice_builtin(BuiltinSlice::Diagonal, 1));
    // Reshapes on the same policy: identity or transpose cell maps. Integer
    // containers convert to real everywhere except to_array_1d, which keeps
    // the element kind.
    builtin("transpose", slice_builtin(BuiltinSlice::Transpose, 1));
    builtin("Transpose__", slice_builtin(BuiltinSlice::Transpose, 1));
    builtin("to_vector", slice_builtin(BuiltinSlice::ToVector, 1));
    builtin("to_row_vector", slice_builtin(BuiltinSlice::ToRowVector, 1));
    builtin("to_matrix", slice_builtin(BuiltinSlice::ToMatrix, 1));
    builtin("to_matrix", slice_builtin(BuiltinSlice::ToMatrix, 3));
    builtin("to_matrix", slice_builtin(BuiltinSlice::ToMatrix, 4));
    builtin("to_array_1d", slice_builtin(BuiltinSlice::ToArray1d, 1));
    builtin("to_array_1d", slice_builtin(BuiltinSlice::ToArray1d, 1, kInt));
    // Expansions: broadcasts and concatenations. The graph keeps its
    // specialized lowerings (dynamic rep extents, the dedicated broadcast
    // kernels, append_array's data-value observations); the interpreter and
    // register machine execute the shared resolver's maps.
    builtin("rep_vector", slice_builtin(BuiltinSlice::RepVector, 2));
    builtin("rep_row_vector", slice_builtin(BuiltinSlice::RepRowVector, 2));
    builtin("rep_matrix", slice_builtin(BuiltinSlice::RepMatrix, 2));
    builtin("rep_matrix", slice_builtin(BuiltinSlice::RepMatrix, 3));
    builtin("rep_array", slice_builtin(BuiltinSlice::RepArray, 2));
    builtin("rep_array", slice_builtin(BuiltinSlice::RepArray, 2, kInt));
    builtin("rep_array", slice_builtin(BuiltinSlice::RepArray, 3));
    builtin("rep_array", slice_builtin(BuiltinSlice::RepArray, 3, kInt));
    builtin("rep_array", slice_builtin(BuiltinSlice::RepArray, 4));
    builtin("rep_array", slice_builtin(BuiltinSlice::RepArray, 4, kInt));
    builtin("append_row", append_builtin(BuiltinSlice::AppendRow));
    builtin("append_col", append_builtin(BuiltinSlice::AppendCol));
    builtin("append_array", append_builtin(BuiltinSlice::AppendArray));
    builtin("append_array", append_builtin(BuiltinSlice::AppendArray, kInt));

    const auto density = [&](std::string_view name, DensitySpec spec) {
      result.emplace_back(name, std::move(spec));
    };
    density("poisson_log_lpmf",
            {OP_POISSON_LOG_LPMF, 2, 1, false, DensityShape::Plain, -1, true});
    density("bernoulli_logit_lpmf", {OP_BERNOULLI_LOGIT_LPMF, 2, 1, false,
                                     DensityShape::Plain, -1, true});
    density("hypergeometric_lpmf",
            all_integer_spec(4, AllIntegerDensity::HypergeometricLpmf));
    density("discrete_range_lpmf",
            all_integer_spec(3, AllIntegerDensity::DiscreteRangeLpmf));

#define STANLI_DENSITY_ENTRY(code, fn, n, tier) density(#fn, {code, n, 0});
    STANLI_SCALAR_DENSITY_LIST(STANLI_DENSITY_ENTRY)
    STANLI_SCALAR_CDF_LIST(STANLI_DENSITY_ENTRY)
#undef STANLI_DENSITY_ENTRY

#define STANLI_INT_DENSITY_ENTRY(code, fn, nreal, tier) \
  density(#fn, {code, nreal + 1, 1, false, DensityShape::Plain, -1, true});
    STANLI_INT_DENSITY_LIST(STANLI_INT_DENSITY_ENTRY)
    STANLI_INT_CDF_LIST(STANLI_INT_DENSITY_ENTRY)
#undef STANLI_INT_DENSITY_ENTRY

#define STANLI_TWO_INT_CDF_ENTRY(code, fn, nreal, tier) \
  density(#fn, {code, nreal + 2, 2});
    STANLI_TWO_INT_CDF_LIST(STANLI_TWO_INT_CDF_ENTRY)
#undef STANLI_TWO_INT_CDF_ENTRY

#define STANLI_TAIL_CDF_ENTRY(code, fn, n, tier) \
  density(#fn, {code, n, 0, false, DensityShape::Plain, (1 << n) - 1});
    STANLI_TAIL_CDF_LIST(STANLI_TAIL_CDF_ENTRY)
#undef STANLI_TAIL_CDF_ENTRY

#define STANLI_TAIL_INT_CDF_ENTRY(code, fn, nreal, tier)        \
  density(#fn, {code, nreal + 1, 1, false, DensityShape::Plain, \
                (1 << nreal) - 1, true});
    STANLI_TAIL_INT_CDF_LIST(STANLI_TAIL_INT_CDF_ENTRY)
#undef STANLI_TAIL_INT_CDF_ENTRY

    density(
        "ordered_logistic_lpmf",
        {OP_ORDERED_LOGISTIC_LPMF, 3, 1, false, DensityShape::VectorizedVectors,
         -1, false, 0, 0, DensityEvaluationPolicy::GraphKernel,
         AllIntegerDensity::None, 0x2});
    density("bernoulli_lpmf",
            {OP_BERNOULLI_LPMF, 2, 1, false, DensityShape::Plain, -1, true});
    density("categorical_lpmf",
            {OP_CATEGORICAL, 2, 0, false, DensityShape::Categorical});
    density("categorical_logit_lpmf",
            {OP_CATEGORICAL, 2, 0, false, DensityShape::Categorical, -1, false,
             0, kCategoricalLogit});
    density("poisson_lpmf",
            {OP_POISSON_LPMF, 2, 1, false, DensityShape::Plain, -1, true});
    density("neg_binomial_2_lpmf", {OP_NEG_BINOMIAL_2_LPMF, 3, 1, false,
                                    DensityShape::Plain, -1, true});
    density("binomial_lpmf", {OP_BINOMIAL_LPMF, 3, 2});
    density("binomial_logit_lpmf", {OP_BINOMIAL_LOGIT_LPMF, 3, 2});
    density("poisson_log_glm_lpmf", {OP_POISSON_LOG_GLM_LPMF, 4, 1, true,
                                     DensityShape::Plain, -1, true});
    density("neg_binomial_2_log_glm_lpmf",
            {OP_NEG_BINOMIAL_2_LOG_GLM_LPMF, 5, 1, true, DensityShape::Plain,
             -1, true});
    density("beta_binomial_lpmf", {OP_BETA_BINOMIAL_LPMF, 4, 2});
    density("bernoulli_logit_glm_lpmf", {OP_BERNOULLI_LOGIT_GLM_LPMF, 4, 1,
                                         true, DensityShape::Plain, -1, true});
    density("binomial_logit_glm_lpmf", {OP_BINOMIAL_LOGIT_GLM_LPMF, 5, 2, true,
                                        DensityShape::Plain, 0x7, true});
    density("categorical_logit_glm_lpmf",
            {OP_CATEGORICAL_LOGIT_GLM_LPMF, 4, 1, true, DensityShape::Plain,
             0x7, true});
    density("ordered_logistic_glm_lpmf",
            {OP_ORDERED_LOGISTIC_GLM_LPMF, 4, 1, true, DensityShape::Plain, 0x7,
             true});
    density("normal_id_glm_lpdf", {OP_NORMAL_ID_GLM_LPDF, 5, 0, true,
                                   DensityShape::Plain, -1, false, 1});
    density("dirichlet_lpdf",
            {OP_DIRICHLET_LPDF, 2, 0, false, DensityShape::VectorizedVectors,
             -1, false, 0, 0, DensityEvaluationPolicy::GraphKernel,
             AllIntegerDensity::None, 0x3});
    density("multi_normal_cholesky_lpdf",
            vectorized_mvt_spec(OP_MULTI_NORMAL_CHOL_LPDF, 3, 0x3));
    density("multi_normal_lpdf",
            vectorized_mvt_spec(OP_MULTI_NORMAL_LPDF, 3, 0x3));
    density("multi_normal_prec_lpdf",
            vectorized_mvt_spec(OP_MULTI_NORMAL_PREC_LPDF, 3, 0x3));
    density("lkj_corr_cholesky_lpdf", {OP_LKJ_CORR_CHOL_LPDF, 2, 0, false,
                                       DensityShape::FirstMatrixRows, 0x1});
    density("lkj_corr_lpdf", {OP_LKJ_CORR_LPDF, 2, 0, false,
                              DensityShape::FirstMatrixRows, 0x1});
    density("lkj_cov_lpdf",
            {OP_LKJ_COV_LPDF, 4, 0, false, DensityShape::FirstMatrixRows, 0xf});
    density("multi_gp_lpdf", {OP_MULTI_GP_LPDF, 3, 0, false,
                              DensityShape::FirstMatrixDimensions, 0x7});
    density("multi_gp_cholesky_lpdf",
            {OP_MULTI_GP_CHOL_LPDF, 3, 0, false,
             DensityShape::FirstMatrixDimensions, 0x7});
    density("multi_student_t_lpdf",
            vectorized_mvt_spec(OP_MULTI_STUDENT_T_LPDF, 4, 0x5, 0xf));
    density("multi_student_t_cholesky_lpdf",
            vectorized_mvt_spec(OP_MULTI_STUDENT_T_CHOL_LPDF, 4, 0x5, 0xf));
    density("multinomial_lpmf",
            {OP_MULTINOMIAL_LPMF, 2, 1, false, DensityShape::Plain, 0x1});
    density("multinomial_logit_lpmf",
            {OP_MULTINOMIAL_LOGIT_LPMF, 2, 1, false, DensityShape::Plain, 0x1});
    density("dirichlet_multinomial_lpmf", {OP_DIRICHLET_MULTINOMIAL_LPMF, 2, 1,
                                           false, DensityShape::Plain, 0x1});
    density("ordered_probit_lpmf", {OP_ORDERED_PROBIT_LPMF, 3, 1, false,
                                    DensityShape::VectorizedVectors, 0x3, false,
                                    0, 0, DensityEvaluationPolicy::GraphKernel,
                                    AllIntegerDensity::None, 0x2});
    density("wiener_lpdf",
            {OP_WIENER_LPDF, 5, 0, false, DensityShape::Plain, 0x1f});
    density("wishart_lpdf",
            {OP_WISHART_LPDF, 3, 0, false, DensityShape::FirstMatrixRows, 0x7});
    density("inv_wishart_lpdf", {OP_INV_WISHART_LPDF, 3, 0, false,
                                 DensityShape::FirstMatrixRows, 0x7});
    density("wishart_cholesky_lpdf", {OP_WISHART_CHOL_LPDF, 3, 0, false,
                                      DensityShape::FirstMatrixRows, 0x7});
    density("inv_wishart_cholesky_lpdf", {OP_INV_WISHART_CHOL_LPDF, 3, 0, false,
                                          DensityShape::FirstMatrixRows, 0x7});
    return result;
  }();
  return entries;
}

bool function_registered(std::string_view name) {
  return function_registry_index().by_name.find(name) !=
         function_registry_index().by_name.end();
}

bool function_arity_registered(std::string_view name, size_t arity) {
  const auto matches = function_registry_index().by_name.equal_range(name);
  for (auto found = matches.first; found != matches.second; ++found)
    if (found->second->arity() == arity) return true;
  return false;
}

const FunctionSpec* function_spec(std::string_view name, size_t arity,
                                  FunctionFamily family) {
  const auto matches = function_registry_index().by_name.equal_range(name);
  for (auto found = matches.first; found != matches.second; ++found) {
    const FunctionSpec* candidate = found->second;
    if (candidate->family() == family && candidate->arity() == arity)
      return candidate;
  }
  return nullptr;
}

const FunctionSpec* function_spec(std::string_view name,
                                  FunctionFamily family) {
  const auto matches = function_registry_index().by_name.equal_range(name);
  for (auto found = matches.first; found != matches.second; ++found)
    if (found->second->family() == family) return found->second;
  return nullptr;
}

const std::vector<const FunctionSpec*>& function_specs(FunctionFamily family) {
  return function_registry_index().by_family[family_index(family)];
}

const FunctionSpec* function_spec(std::string_view name, size_t arity,
                                  uint64_t integer_arguments,
                                  FunctionArgumentKind result_kind) {
  const auto matches = function_registry_index().by_name.equal_range(name);
  const FunctionSpec* best = nullptr;
  size_t best_promotions = 0;
  bool best_result_matches = false;
  for (auto found = matches.first; found != matches.second; ++found) {
    const FunctionSpec* candidate = found->second;
    if (!candidate->accepts(arity, integer_arguments)) continue;
    size_t promotions = 0;
    for (size_t index = 0; index < arity; ++index)
      if ((integer_arguments & (uint64_t{1} << index)) != 0 &&
          candidate->argument_kind(index) == FunctionArgumentKind::Real)
        ++promotions;
    const bool result_matches = candidate->result() == result_kind;
    if (best == nullptr || promotions < best_promotions ||
        (promotions == best_promotions && result_matches &&
         !best_result_matches)) {
      best = candidate;
      best_promotions = promotions;
      best_result_matches = result_matches;
    } else if (promotions == best_promotions &&
               result_matches == best_result_matches) {
      throw std::logic_error("ambiguous function registry overload: " +
                             std::string(name));
    }
  }
  return best;
}

const FunctionSpec* function_spec(const mir::Expr& call) {
  const auto numeric_kind = [](const mir::Expr& expression,
                               FunctionArgumentKind* kind) {
    if (expression.unsized.leaf == mir::UnsizedLeaf::Int ||
        (expression.unsized.leaf == mir::UnsizedLeaf::Unknown &&
         expression.type_ == "UInt")) {
      *kind = FunctionArgumentKind::Integer;
      return true;
    }
    if (expression.unsized.leaf == mir::UnsizedLeaf::Real ||
        expression.unsized.leaf == mir::UnsizedLeaf::Vector ||
        expression.unsized.leaf == mir::UnsizedLeaf::RowVector ||
        expression.unsized.leaf == mir::UnsizedLeaf::Matrix ||
        (expression.unsized.leaf == mir::UnsizedLeaf::Unknown &&
         (expression.type_ == "UReal" || expression.type_ == "UVector" ||
          expression.type_ == "URowVector" || expression.type_ == "UMatrix"))) {
      *kind = FunctionArgumentKind::Real;
      return true;
    }
    return false;
  };
  if (call.args.size() > 64) return nullptr;
  uint64_t integer_arguments = 0;
  for (size_t index = 0; index < call.args.size(); ++index) {
    FunctionArgumentKind kind;
    if (!numeric_kind(call.args[index], &kind)) return nullptr;
    if (kind == FunctionArgumentKind::Integer)
      integer_arguments |= uint64_t{1} << index;
  }
  FunctionArgumentKind result;
  if (!numeric_kind(call, &result)) {
    // Older hand-built MIR may omit result metadata. Infer it only when the
    // argument contract admits exactly one registry result kind.
    if (call.unsized.leaf != mir::UnsizedLeaf::Unknown || !call.type_.empty())
      return nullptr;
    const FunctionSpec* integer =
        function_spec(call.name, call.args.size(), integer_arguments,
                      FunctionArgumentKind::Integer);
    const FunctionSpec* real =
        function_spec(call.name, call.args.size(), integer_arguments,
                      FunctionArgumentKind::Real);
    return integer == real      ? integer
           : integer == nullptr ? real
           : real == nullptr    ? integer
                                : nullptr;
  }
  return function_spec(call.name, call.args.size(), integer_arguments, result);
}

}  // namespace stanli
