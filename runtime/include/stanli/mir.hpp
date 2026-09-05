// C++ representation of the slice of stanc3's transformed MIR that the graph
// compiler consumes. Anything outside the slice is preserved as raw sexp text
// in `raw` and surfaces as a clear compile error, never a miscompile.
#ifndef STANLI_MIR_HPP
#define STANLI_MIR_HPP

#include <stanli/expression_layout.hpp>
#include <stanli/optable.hpp>
#include <stanli/sexp.hpp>

#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace stanli {
namespace mir {

enum class UnsizedLeaf : uint8_t {
  Unknown,
  Int,
  Real,
  Complex,
  Vector,
  RowVector,
  Matrix
};

struct UnsizedView {
  uint8_t depth = 0;
  UnsizedLeaf leaf = UnsizedLeaf::Unknown;
};

struct Expr {
  enum Kind {
    Var,
    LitInt,
    LitReal,
    LitStr,
    FunApp,
    Promotion,
    Indexed,
    TernaryIf,
    EOr,
    EAnd,
    Unsupported
  } kind = Unsupported;
  std::string name;  // Var name or FunApp function name
  enum class Lib { StanLib, Internal, UserDefined } fn_lib = Lib::StanLib;
  bool fn_propto = false;  // (FnLpdf true) / (FnLpmf true)
  long lit_i = 0;
  double lit = 0;
  std::string lit_s;
  std::vector<Expr> args;  // FunApp args; Promotion inner; Indexed base+idx
  std::string type_;       // UInt UReal UVector URowVector UMatrix ...
  UnsizedView unsized;     // structural (UArray ...), without text parsing
  bool data_only = false;  // adlevel DataOnly
  bool promoted = false;   // explicit MIR Promotion to this adlevel/type
  std::string raw;         // Unsupported diagnostics
};

// stanc leaves these calls in MIR rather than folding them: four have
// non-finite/platform values, while pi/e are still part of the same language
// surface. Keep name recognition and values together so graph lowering, the
// register compiler, and MirInterp cannot grow different subsets. FnNegInf is
// the optimizer's internal spelling of the same negative-infinity constant.
enum class NullaryConstantKind : uint8_t {
  E,
  Pi,
  MachinePrecision,
  NegativeInfinity,
  PositiveInfinity,
  NotANumber,
};

inline std::optional<NullaryConstantKind> nullary_constant_kind(const Expr& e) {
  if (e.kind != Expr::FunApp || !e.args.empty()) return std::nullopt;
  if (e.fn_lib == Expr::Lib::Internal)
    return e.name == "FnNegInf" ? std::optional<NullaryConstantKind>(
                                      NullaryConstantKind::NegativeInfinity)
                                : std::nullopt;
  if (e.fn_lib != Expr::Lib::StanLib) return std::nullopt;
  if (e.name == "e") return NullaryConstantKind::E;
  if (e.name == "pi") return NullaryConstantKind::Pi;
  if (e.name == "machine_precision")
    return NullaryConstantKind::MachinePrecision;
  if (e.name == "negative_infinity")
    return NullaryConstantKind::NegativeInfinity;
  if (e.name == "positive_infinity")
    return NullaryConstantKind::PositiveInfinity;
  if (e.name == "not_a_number") return NullaryConstantKind::NotANumber;
  return std::nullopt;
}

inline double nullary_constant_value(NullaryConstantKind kind) {
  switch (kind) {
    case NullaryConstantKind::E:
      return 0x1.5bf0a8b145769p+1;
    case NullaryConstantKind::Pi:
      return 0x1.921fb54442d18p+1;
    case NullaryConstantKind::MachinePrecision:
      return std::numeric_limits<double>::epsilon();
    case NullaryConstantKind::NegativeInfinity:
      return -std::numeric_limits<double>::infinity();
    case NullaryConstantKind::PositiveInfinity:
      return std::numeric_limits<double>::infinity();
    case NullaryConstantKind::NotANumber:
      return std::numeric_limits<double>::quiet_NaN();
  }
  return std::numeric_limits<double>::quiet_NaN();
}

inline std::optional<double> nullary_constant(const Expr& e) {
  const auto kind = nullary_constant_kind(e);
  return kind ? std::optional<double>(nullary_constant_value(*kind))
              : std::nullopt;
}

// Calls whose value comes from the evaluation context rather than solely
// from their arguments. Keep their recognition beside the other shared MIR
// call metadata so every backend, effect analysis, and optimizer agrees on
// the spelling and arity. Backends still own the state itself.
enum class StatefulIntrinsicKind : uint8_t { Target };

inline std::optional<StatefulIntrinsicKind> stateful_intrinsic_kind(
    const Expr& e) {
  if (e.kind == Expr::FunApp && e.fn_lib == Expr::Lib::StanLib &&
      e.name == "target" && e.args.empty())
    return StatefulIntrinsicKind::Target;
  return std::nullopt;
}

// A matrix row is a non-contiguous Eigen block.  Transposing it changes the
// logical orientation but not the stride, so an outer elementwise expression
// containing it has no packet access and Stan Math's product reduces in
// ascending scalar order.  Both write_array engines consult this syntactic
// fact before materializing the expression, when the stride is still visible.
inline bool is_matrix_row_value(const Expr& value) {
  const Expr* indexed = &value;
  if (value.kind == Expr::FunApp && value.fn_lib == Expr::Lib::StanLib &&
      (value.name == "Transpose__" || value.name == "transpose") &&
      value.args.size() == 1)
    indexed = &value.args[0];
  if (indexed->kind != Expr::Indexed ||
      indexed->unsized.leaf != UnsizedLeaf::RowVector ||
      indexed->args.empty() || indexed->args[0].kind != Expr::Var ||
      indexed->args[0].unsized.depth != 0 ||
      indexed->args[0].unsized.leaf != UnsizedLeaf::Matrix)
    return false;
  const bool implicit_all =
      indexed->args.size() == 2 && indexed->args[1].name == "IndexSingle";
  const bool explicit_all = indexed->args.size() == 3 &&
                            indexed->args[1].name == "IndexSingle" &&
                            indexed->args[2].name == "IndexAll";
  return implicit_all || explicit_all;
}

inline bool reduction_container(const Expr& e) {
  if (e.unsized.depth == 0)
    return e.unsized.leaf == UnsizedLeaf::Vector ||
           e.unsized.leaf == UnsizedLeaf::RowVector ||
           e.unsized.leaf == UnsizedLeaf::Matrix;
  return e.unsized.depth == 1 && (e.unsized.leaf == UnsizedLeaf::Real ||
                                  e.unsized.leaf == UnsizedLeaf::Int);
}

inline bool language_scalar(const Expr& e) {
  return e.unsized.depth == 0 && (e.unsized.leaf == UnsizedLeaf::Int ||
                                  e.unsized.leaf == UnsizedLeaf::Real);
}

inline bool source_unary_elementwise(const std::string& name) {
  bool unary = false;
#define STANLI_SOURCE_UNARY(code, fn_name, value, delta, topology) \
  unary = unary || name == #fn_name;
  STANLI_SCALAR_UNARY_LIST(STANLI_SOURCE_UNARY)
#undef STANLI_SOURCE_UNARY
  return unary || name == "PMinus__" || name == "minus" || name == "exp" ||
         name == "log" || name == "inv_logit" || name == "sqrt" ||
         name == "square" || name == "log1m" || name == "tanh" ||
         name == "trigamma" || name == "std_normal_qf" || name == "logit";
}

inline bool source_binary_elementwise(const std::string& name) {
  bool binary = false;
#define STANLI_SOURCE_BINARY(code, fn_name, fn) \
  binary = binary || name == #fn_name;
  STANLI_SCALAR_BINARY_LIST(STANLI_SOURCE_BINARY)
  STANLI_SCALAR_BINARY_INT_FIRST_LIST(STANLI_SOURCE_BINARY)
  STANLI_SCALAR_BINARY_INT_SECOND_LIST(STANLI_SOURCE_BINARY)
  STANLI_SCALAR_BINARY_INTEGER_LIST(STANLI_SOURCE_BINARY)
#undef STANLI_SOURCE_BINARY
  return binary || name == "Plus__" || name == "Minus__" ||
         name == "EltTimes__" || name == "EltDivide__" || name == "EltPow__" ||
         name == "add" || name == "subtract" || name == "elt_multiply" ||
         name == "elt_divide" || name == "plus";
}

// Conservative source-level counterpart of Lowering::Val::layout for the MIR
// interpreter. Both product and extrema consume this one classification so a
// newly admitted expression cannot acquire two different grouping rules.
inline ExpressionLayout source_expression_layout(const Expr& e) {
  if (language_scalar(e)) return ExpressionLayout::scalar();
  if (e.kind == Expr::Promotion && e.args.size() == 1)
    return source_expression_layout(e.args[0]);
  if (e.kind == Expr::Var && reduction_container(e))
    return ExpressionLayout::direct();
  if (e.kind == Expr::FunApp && e.fn_lib == Expr::Lib::UserDefined &&
      reduction_container(e))
    return ExpressionLayout::direct();
  if (is_matrix_row_value(e)) return ExpressionLayout::scalar();
  if (e.kind == Expr::Indexed && reduction_container(e) && !e.args.empty()) {
    const Expr& base = e.args[0];
    if (base.kind != Expr::Var || e.args.size() < 2)
      return ExpressionLayout::unknown();
    const Expr& index = e.args[1];
    if (base.unsized.depth > 0) {
      // Single outer-array indices expose an independently owning Eigen leaf.
      // A following range then has a phase relative to that leaf, never the
      // flattened offset of the preceding array elements. Non-single outer
      // indices instead build a new std::vector result at offset zero.
      bool selected_one_leaf = base.unsized.leaf == UnsizedLeaf::Vector ||
                               base.unsized.leaf == UnsizedLeaf::RowVector;
      for (uint8_t i = 0; i < base.unsized.depth; ++i)
        selected_one_leaf = selected_one_leaf && i + 1 < e.args.size() &&
                            e.args[i + 1].kind == Expr::FunApp &&
                            e.args[i + 1].name == "IndexSingle";
      const size_t leaf_index = static_cast<size_t>(base.unsized.depth) + 1;
      if (selected_one_leaf && leaf_index < e.args.size()) {
        const Expr& inner = e.args[leaf_index];
        if (inner.kind != Expr::FunApp) return ExpressionLayout::unknown();
        if (inner.name == "IndexMulti") return ExpressionLayout::scalar();
        if ((inner.name == "IndexBetween" || inner.name == "IndexUpfrom") &&
            !inner.args.empty() && inner.args[0].kind == Expr::LitInt &&
            inner.args[0].lit_i >= 1)
          return ExpressionLayout::direct(
              static_cast<int64_t>(inner.args[0].lit_i - 1));
        if (inner.name != "IndexAll") return ExpressionLayout::unknown();
      }
      return ExpressionLayout::direct();
    }
    if (index.kind != Expr::FunApp) return ExpressionLayout::unknown();
    if (index.name == "IndexMulti") return ExpressionLayout::scalar();
    if ((index.name == "IndexBetween" || index.name == "IndexUpfrom") &&
        !index.args.empty() && index.args[0].kind == Expr::LitInt &&
        index.args[0].lit_i >= 1)
      return ExpressionLayout::direct(
          static_cast<int64_t>(index.args[0].lit_i - 1));
    if (index.name == "IndexAll") return source_expression_layout(base);
    return ExpressionLayout::unknown();
  }
  if (e.kind != Expr::FunApp || e.fn_lib != Expr::Lib::StanLib ||
      !reduction_container(e))
    return ExpressionLayout::unknown();
  if (e.name == "segment" && e.args.size() == 3 &&
      e.args[1].kind == Expr::LitInt && e.args[1].lit_i >= 1)
    return expression_layout::contiguous(
        source_expression_layout(e.args[0]),
        static_cast<int64_t>(e.args[1].lit_i - 1));
  if (e.args.size() == 1 &&
      (e.name == "Transpose__" || e.name == "transpose")) {
    if (e.unsized.leaf == UnsizedLeaf::Matrix)
      return ExpressionLayout::unknown();
    return source_expression_layout(e.args[0]);
  }
  if (e.args.size() == 1 && (e.name == "PPlus__" || e.name == "plus"))
    return source_expression_layout(e.args[0]);
  if (e.name == "softmax" || e.name == "log_softmax" ||
      e.name == "cumulative_sum" || e.name == "rep_vector" ||
      e.name == "rep_row_vector" || e.name == "to_vector" ||
      e.name == "to_row_vector")
    return ExpressionLayout::packet();
  if (e.args.size() == 1 && source_unary_elementwise(e.name)) {
    const ExpressionLayout input = source_expression_layout(e.args[0]);
    if (!input.known()) return ExpressionLayout::unknown();
    return input.packet_access() ? ExpressionLayout::packet()
                                 : ExpressionLayout::scalar();
  }
  if (e.args.size() == 2 && source_binary_elementwise(e.name)) {
    bool saw_container = false;
    bool all_known = true;
    bool all_packet_access = true;
    for (const Expr& operand : e.args) {
      if (language_scalar(operand)) continue;
      if (!reduction_container(operand)) return ExpressionLayout::unknown();
      const ExpressionLayout input = source_expression_layout(operand);
      saw_container = true;
      all_known = all_known && input.known();
      all_packet_access = all_packet_access && input.packet_access();
    }
    if (!saw_container) return ExpressionLayout::unknown();
    return expression_layout::elementwise(false, true, all_known,
                                          all_packet_access);
  }
  return ExpressionLayout::unknown();
}

// min/max is overloaded across scalars, arrays, matrices, and Eigen
// expressions. The overload surface is independent of expression provenance;
// the graph lowerer obtains the latter from Val::layout after lowering.
enum class ExtremaKind : uint8_t { Legacy, Min, Max };

enum class ExtremaSurface : uint8_t {
  Legacy,
  RealVector,
  RealMatrix,
  RealArray,
  IntArray,
  IntPair,
};

struct ExtremaCall {
  ExtremaKind kind = ExtremaKind::Legacy;
  ExtremaSurface surface = ExtremaSurface::Legacy;
};

inline bool extrema_typed(const Expr& e, const char* type, UnsizedLeaf leaf,
                          uint8_t depth) {
  return e.type_ == type && e.unsized.leaf == leaf && e.unsized.depth == depth;
}

// Determine the overload from language-level types only. A vector expression
// or a gathered array element is still the same overload as its named source;
// ExpressionLayout decides whether the native kernel can reproduce its
// evaluation grouping.
inline ExtremaSurface extrema_container(const Expr& arg) {
  if (extrema_typed(arg, "UVector", UnsizedLeaf::Vector, 0) ||
      extrema_typed(arg, "URowVector", UnsizedLeaf::RowVector, 0))
    return ExtremaSurface::RealVector;
  if (extrema_typed(arg, "UMatrix", UnsizedLeaf::Matrix, 0))
    return ExtremaSurface::RealMatrix;
  if (extrema_typed(arg, "UArray", UnsizedLeaf::Real, 1))
    return ExtremaSurface::RealArray;
  if (extrema_typed(arg, "UArray", UnsizedLeaf::Int, 1))
    return ExtremaSurface::IntArray;
  return ExtremaSurface::Legacy;
}

inline ExtremaCall extrema_call(const Expr& call) {
  if (call.kind != Expr::FunApp || call.fn_lib != Expr::Lib::StanLib) return {};
  ExtremaKind kind = ExtremaKind::Legacy;
  if (call.name == "min")
    kind = ExtremaKind::Min;
  else if (call.name == "max")
    kind = ExtremaKind::Max;
  else
    return {};

  const bool real_result = extrema_typed(call, "UReal", UnsizedLeaf::Real, 0);
  const bool int_result = extrema_typed(call, "UInt", UnsizedLeaf::Int, 0);
  if (call.args.size() == 2) {
    if (!int_result ||
        !extrema_typed(call.args[0], "UInt", UnsizedLeaf::Int, 0) ||
        !extrema_typed(call.args[1], "UInt", UnsizedLeaf::Int, 0))
      return {};
    return {kind, ExtremaSurface::IntPair};
  }
  if (call.args.size() != 1) return {};
  const ExtremaSurface surface = extrema_container(call.args[0]);
  if (surface == ExtremaSurface::Legacy) return {};
  const bool result_matches =
      surface == ExtremaSurface::IntArray ? int_result : real_result;
  return result_matches ? ExtremaCall{kind, surface} : ExtremaCall{};
}

struct Transform {
  // The names are stanc3's own MIR tags, so a new transform in the
  // compiler is greppable here.
  enum Kind {
    Identity,
    Lower,
    Upper,
    LowerUpper,
    Offset,
    Multiplier,
    OffsetMultiplier,
    Simplex,
    Ordered,
    PositiveOrdered,
    CholeskyCorr,
    UnitVector,
    SumToZero,
    Correlation,
    Covariance,
    CholeskyCov,
    Unsupported
  } kind = Identity;
  std::vector<Expr> args;
  std::string raw;
};

inline bool is_structured_check(Transform::Kind kind) {
  switch (kind) {
    case Transform::Simplex:
    case Transform::Ordered:
    case Transform::PositiveOrdered:
    case Transform::CholeskyCorr:
    case Transform::UnitVector:
    case Transform::SumToZero:
    case Transform::Correlation:
    case Transform::Covariance:
    case Transform::CholeskyCov:
      return true;
    default:
      return false;
  }
}

struct SizedType {
  std::string base;        // SInt SReal SVector SRowVector SMatrix SArray ...
  std::vector<Expr> dims;  // outer-to-inner for SArray chains
  std::string elem_base;   // for SArray: the innermost element base
  std::string raw;         // Unsupported diagnostics
  // stanc also uses unsized declarations for optimizer temporaries. Their
  // shape is supplied by the first whole-variable assignment.
  UnsizedView unsized;
};

struct Stmt {
  enum Kind {
    Decl,
    Assignment,
    TargetPE,
    Block,
    SList,
    For,
    IfElse,
    While,
    NRFunApp,
    Return,
    Break,
    Continue,
    Skip,
    Unsupported
  } kind = Unsupported;
  // Decl
  std::string decl_id;
  SizedType decl_type;
  bool decl_data_only = false;
  bool has_init = false;
  Expr init;
  std::optional<Transform> read_transform;  // set iff init is FnReadParam
  std::vector<Expr> read_dims;              // FnReadParam dims
  // Assignment
  std::string lhs;
  std::vector<Expr> lhs_idx;
  Expr rhs;
  // TargetPE
  Expr target;
  // NRFunApp
  std::string fn_name;
  std::vector<Expr> fn_args;
  // FnCheck: the relation lives in the CompilerInternal payload rather than
  // the ordinary argument list. The first fn_arg is the value and the rest
  // are its bounds.
  std::optional<Transform> check_transform;
  std::string check_var_name;
  // FnWriteParam in transform_inits: the transform to INVERT, so that a
  // constrained value supplied by the user becomes a free one. write_array's
  // own FnWriteParam leaves this empty -- its value is constrained already.
  // Both readers split this out of the one optional-transform slot the wire
  // and the S-expression share with FnCheck, so consumers never have to ask
  // which meaning a transform carries.
  std::optional<Transform> write_transform;
  // For
  std::string loopvar;
  Expr lower, upper;
  // IfElse
  Expr cond;
  // Block / SList / For body / IfElse (then at body[0], else at body[1] if
  // present, each wrapped as its own Stmt)
  std::vector<Stmt> body;
  std::string raw;
};

// stanc3 separates write_array's three CSV sections with early-return
// guards rather than nested blocks:
//   if (!(emit_transformed_parameters__ || emit_generated_quantities__))
//     return;                                 <- transformed parameters start
//   if (!emit_generated_quantities__) return; <- generated quantities start
// Both flags are pinned on, so the guards emit nothing; the column count
// as one is reached is the only record of where a section begins.
enum class EmitGuard { None, TransformedParams, GeneratedQuantities };

inline EmitGuard emit_guard(const Stmt& s) {
  if (s.kind != Stmt::IfElse) return EmitGuard::None;
  const Expr& c = s.cond;
  if (c.kind != Expr::FunApp || c.name != "PNot__" || c.args.size() != 1)
    return EmitGuard::None;
  const Expr& a = c.args[0];
  if (a.kind == Expr::Var && a.name == "emit_generated_quantities__")
    return EmitGuard::GeneratedQuantities;
  if (a.kind == Expr::EOr && a.args.size() == 2 &&
      a.args[0].kind == Expr::Var &&
      a.args[0].name == "emit_transformed_parameters__" &&
      a.args[1].kind == Expr::Var &&
      a.args[1].name == "emit_generated_quantities__")
    return EmitGuard::TransformedParams;
  return EmitGuard::None;
}

struct FunDef {
  std::string name;
  std::vector<std::string> arg_names;
  std::vector<std::string> arg_types;  // unsized: UReal UVector UMatrix ...
  std::vector<UnsizedView> arg_views;
  std::vector<bool> arg_data_only;
  std::vector<Stmt> body;
};

struct Program {
  std::vector<std::pair<std::string, SizedType>> input_vars;
  std::vector<Stmt> prepare_data;
  std::vector<Stmt> log_prob;
  // stanc3's `generate_quantities` is the whole write_array body: it re-reads
  // the unconstrained draw, recomputes the transformed parameters, runs the
  // generated quantities block, and marks each CSV column with an
  // FnWriteParam. Gated on emit_transformed_parameters__ /
  // emit_generated_quantities__, which the lowering pins to 1.
  std::vector<Stmt> generate_quantities;
  // stanc3's `transform_inits`: reads each parameter by name from a caller
  // supplied context (FnReadData names the PARAMETER here, not model data)
  // and emits one FnWriteParam per parameter carrying the transform to
  // invert. This is the inverse direction of the log_prob graph's
  // FnReadParam constrains, and the only place stanli can learn it.
  // Presence is separate from content: a current parameterless model carries
  // an explicitly empty section, while an older producer carries no section.
  bool has_transform_inits = false;
  std::vector<Stmt> transform_inits;
  std::vector<FunDef> fun_defs;
  // Output variable names (params, transformed params, generated
  // quantities) in FnWriteParam emission order, from the MIR's
  // output_vars section.
  std::vector<std::string> output_vars;
};

// Higher-order Stan functions carry their callback as a bare Var rather than
// a user-function call.  Keep family recognition here so graph lowering, the
// register compiler, the interpreter, and effect analysis cannot acquire
// different lists as support grows.

enum class HigherOrderFamily : uint8_t {
  ReduceSum,
  MapRect,
  Algebra,
  Integrate1D,
  Ode,
  Dae,
};

struct HigherOrderCall {
  HigherOrderFamily family;
};

enum class QuadratureMethod : uint8_t {
  Integrate1D,
  DoubleExponential,
  GaussKronrod,
};

struct QuadratureCall {
  QuadratureMethod method;
  bool legacy = false;
  bool with_tolerance = false;
  size_t callback_args_begin = 3;
};

enum class OdeMethod : uint8_t { Rk45, Bdf, Adams, Ckrk, Adjoint };

struct OdeCall {
  OdeMethod method;
  bool legacy = false;
  bool with_tolerance = false;
  // First callback argument after (f, y0, t0, ts) and optional controls.
  size_t callback_args_begin = 4;
};

struct DaeCall {
  bool with_tolerance = false;
  // First callback argument after (f, y0, yp0, t0, ts) and optional controls.
  size_t callback_args_begin = 5;
};

inline std::optional<DaeCall> dae_call(std::string_view name) {
  if (name == "dae") return DaeCall{};
  if (name == "dae_tol") return DaeCall{true, 8};
  return {};
}

enum class AlgebraMethod : uint8_t { Powell, Newton };

struct AlgebraCall {
  AlgebraMethod method;
  bool legacy = false;
  bool with_tolerance = false;
  size_t callback_args_begin = 2;
};

inline std::optional<AlgebraCall> algebra_call(std::string_view name) {
  if (name == "algebra_solver") return AlgebraCall{AlgebraMethod::Powell, true};
  if (name == "algebra_solver_newton")
    return AlgebraCall{AlgebraMethod::Newton, true};
  if (name == "solve_newton") return AlgebraCall{AlgebraMethod::Newton};
  if (name == "solve_powell") return AlgebraCall{AlgebraMethod::Powell};
  if (name == "solve_newton_tol")
    return AlgebraCall{AlgebraMethod::Newton, false, true, 5};
  if (name == "solve_powell_tol")
    return AlgebraCall{AlgebraMethod::Powell, false, true, 5};
  return {};
}

inline std::optional<OdeCall> ode_call(std::string_view name) {
  if (name == "integrate_ode") return OdeCall{OdeMethod::Rk45, true, false, 4};
  if (name == "integrate_ode_rk45")
    return OdeCall{OdeMethod::Rk45, true, false, 4};
  if (name == "integrate_ode_bdf")
    return OdeCall{OdeMethod::Bdf, true, false, 4};
  if (name == "integrate_ode_adams")
    return OdeCall{OdeMethod::Adams, true, false, 4};
  if (name == "ode_rk45") return OdeCall{OdeMethod::Rk45};
  if (name == "ode_bdf") return OdeCall{OdeMethod::Bdf};
  if (name == "ode_adams") return OdeCall{OdeMethod::Adams};
  if (name == "ode_ckrk") return OdeCall{OdeMethod::Ckrk};
  if (name == "ode_rk45_tol") return OdeCall{OdeMethod::Rk45, false, true, 7};
  if (name == "ode_bdf_tol") return OdeCall{OdeMethod::Bdf, false, true, 7};
  if (name == "ode_adams_tol") return OdeCall{OdeMethod::Adams, false, true, 7};
  if (name == "ode_ckrk_tol") return OdeCall{OdeMethod::Ckrk, false, true, 7};
  if (name == "ode_adjoint_tol_ctl")
    return OdeCall{OdeMethod::Adjoint, false, true, 15};
  return {};
}

inline std::optional<QuadratureCall> quadrature_call(std::string_view name) {
  if (name == "integrate_1d")
    return QuadratureCall{QuadratureMethod::Integrate1D, true, false, 3};
  if (name == "integrate_1d_double_exponential")
    return QuadratureCall{QuadratureMethod::DoubleExponential, false, false, 3};
  if (name == "integrate_1d_double_exponential_tol")
    return QuadratureCall{QuadratureMethod::DoubleExponential, false, true, 6};
  if (name == "integrate_1d_gauss_kronrod")
    return QuadratureCall{QuadratureMethod::GaussKronrod, false, false, 3};
  if (name == "integrate_1d_gauss_kronrod_tol")
    return QuadratureCall{QuadratureMethod::GaussKronrod, false, true, 6};
  return {};
}

inline std::optional<HigherOrderCall> higher_order_call(const Expr& e) {
  if (e.kind != Expr::FunApp || e.fn_lib != Expr::Lib::StanLib) return {};
  const std::string_view name = e.name;
  if (name == "reduce_sum" || name == "reduce_sum_static")
    return HigherOrderCall{HigherOrderFamily::ReduceSum};
  if (name == "map_rect") return HigherOrderCall{HigherOrderFamily::MapRect};
  if (algebra_call(name)) return HigherOrderCall{HigherOrderFamily::Algebra};
  if (quadrature_call(name))
    return HigherOrderCall{HigherOrderFamily::Integrate1D};
  if (ode_call(name)) return HigherOrderCall{HigherOrderFamily::Ode};
  if (dae_call(name)) return HigherOrderCall{HigherOrderFamily::Dae};
  return {};
}

inline bool is_reduce_sum(const Expr& e) {
  const auto call = higher_order_call(e);
  return call && call->family == HigherOrderFamily::ReduceSum;
}

// The `_lupdf` / `_lupmf` spelling at the functor reference is stanc3's
// propto marker; the definition is always the normalized `_lpdf` / `_lpmf`.
// The marker is the only surviving propto signal here: the functor's
// `(FnLpdf true)` type is not part of the portable encoding, and the
// reduce_sum node itself is FnPlain, so neither can be consulted instead.
inline std::string reduce_sum_partial_name(const std::string& functor,
                                           bool* propto) {
  const auto unnormalized = [&](const char* marker) {
    return functor.size() > 6 &&
           functor.compare(functor.size() - 6, 6, marker) == 0;
  };
  if (unnormalized("_lupdf")) {
    *propto = true;
    return functor.substr(0, functor.size() - 6) + "_lpdf";
  }
  if (unnormalized("_lupmf")) {
    *propto = true;
    return functor.substr(0, functor.size() - 6) + "_lpmf";
  }
  *propto = false;
  return functor;
}

// The formals reduce_sum calls its partial-sum function with, in order: the
// slice, its two bounds, then every shared argument unchanged.
inline std::vector<UnsizedView> reduce_sum_partial_views(const Expr& e) {
  std::vector<UnsizedView> views;
  if (e.args.size() < 3) return views;
  views.reserve(e.args.size());
  views.push_back(e.args[1].unsized);
  views.push_back({0, UnsizedLeaf::Int});
  views.push_back({0, UnsizedLeaf::Int});
  for (size_t i = 3; i < e.args.size(); ++i) views.push_back(e.args[i].unsized);
  return views;
}

// The reader mangles an overloaded definition's name and rewrites ordinary
// call sites, but a higher-order callback reference is a Var and is never
// rewritten. Take the unmangled name when it is the only one, and otherwise
// select the overload whose formals match the call the family will make.
// Returns null when the name resolves to nothing or, impossibly, to more than
// one.  Family-specific helpers only have to construct `views`.
inline const FunDef* resolve_callback(
    const std::map<std::string, const FunDef*>& funs, const std::string& base,
    const std::vector<UnsizedView>& views) {
  const auto exact = funs.find(base);
  if (exact != funs.end()) return exact->second;
  const FunDef* match = nullptr;
  for (const auto& [name, def] : funs) {
    if (name.size() <= base.size() || name.compare(0, base.size(), base) != 0 ||
        name[base.size()] != '(')
      continue;
    if (def->arg_views.size() != views.size()) continue;
    bool same = true;
    for (size_t i = 0; i < views.size() && same; ++i)
      same = def->arg_views[i].depth == views[i].depth &&
             def->arg_views[i].leaf == views[i].leaf;
    if (!same) continue;
    if (match) return nullptr;
    match = def;
  }
  return match;
}

Program read_program(const sexp::Node& root);

}  // namespace mir
}  // namespace stanli

#endif
