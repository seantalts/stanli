#include <stanli/callable_transform.hpp>
#include <stanli/function_registry.hpp>
#include <stanli/mir.hpp>

#include "mir_reader_internal.hpp"
#include "mir_tuples.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>

namespace stanli {
namespace mir {
namespace {

[[noreturn]] void malformed(const std::string& what) {
  throw mir::MirError("mir: malformed " + what);
}

void require_arity(const Expr& e, size_t expected) {
  if (e.args.size() != expected)
    malformed(e.name + " call: expected " + std::to_string(expected) +
              " argument(s), got " + std::to_string(e.args.size()));
}

void require_arity(const Expr& e, size_t first, size_t second) {
  if (e.args.size() != first && e.args.size() != second)
    malformed(e.name + " call: expected " + std::to_string(first) + " or " +
              std::to_string(second) + " argument(s), got " +
              std::to_string(e.args.size()));
}

bool named(const Expr& e, std::initializer_list<const char*> names) {
  for (const char* name : names)
    if (e.name == name) return true;
  return false;
}

// Validate the arities that the lowering and interpreters use as structural
// dispatch. Unknown functions are intentionally left to their normal
// unsupported-function diagnostic, but a known function must never reach an
// unchecked args[k] with a malformed argument vector.
void validate_funapp_arity(const Expr& e) {
  if (e.fn_lib == Expr::Lib::UserDefined) return;

  // log2 and log10 also have unary registry entries. Their nullary forms
  // are language constants shared by all execution paths.
  if (nullary_constant_kind(e)) return;

  // A call matching a registered overload's arity is structurally sound.
  // A registered name at another arity is not rejected here: some names have
  // legitimate arities outside the registry (one-argument log_sum_exp's
  // reduction, seven-argument wiener_lpdf's extension), so those fall
  // through to the per-name rules below; a registered name matching no rule
  // at all is rejected at the end of this function.
  if (function_arity_registered(e.name, e.args.size())) return;

  if (e.fn_lib == Expr::Lib::Internal &&
      (e.name == "FnReadData" || e.name == "FnLength")) {
    require_arity(e, 1);
    return;
  }

  if (named(e, {"pi", "e", "sqrt2", "machine_precision", "negative_infinity",
                "positive_infinity", "not_a_number"})) {
    require_arity(e, 0);
    return;
  }

  if (named(e, {"Modulo__", "diag_pre_multiply", "diag_post_multiply",
                "quad_form_diag"})) {
    require_arity(e, 2);
    return;
  }
  if (named(e, {"PPlus__", "plus", "prod", "categorical_rng", "tcrossprod"})) {
    require_arity(e, 1);
    return;
  }
  if (named(e, {"min", "max"})) {
    require_arity(e, 1, 2);
    return;
  }
  if (e.name == "fma") {
    require_arity(e, 3);
    return;
  }
  if (e.name == "log_mix") {
    require_arity(e, 2, 3);
    return;
  }
  if (named(e, {"gp_exp_quad_cov", "gp_matern32_cov", "gp_matern52_cov",
                "gp_exponential_cov"})) {
    require_arity(e, 3, 4);
    return;
  }
  if (e.name == "gp_periodic_cov") {
    require_arity(e, 4, 5);
    return;
  }
  if (e.name == "map_rect") {
    require_arity(e, 5);
    return;
  }
  if (e.name == "reduce_sum" || e.name == "reduce_sum_static") {
    // A partial-sum function, the sliced argument, and a grainsize, then any
    // number of shared arguments.
    if (e.args.size() < 3)
      malformed("reduce_sum call: expected at least 3 arguments, got " +
                std::to_string(e.args.size()));
    return;
  }
  if (e.name == "algebra_solver") {
    require_arity(e, 5, 8);
    return;
  }
  if (named(e, {"multi_normal_rng"})) {
    require_arity(e, 2);
    return;
  }

  // Registered probability functions are validated through the registry
  // above; gaussian_dlm_obs is the one density with a fixed arity that stays
  // outside it (an op holds six inputs and it takes seven).
  if (e.name == "gaussian_dlm_obs_lpdf") {
    require_arity(e, 7);
    return;
  }

  // The original four-parameter Wiener density, and the five- and
  // seven-parameter forms, each with an optional precision argument.
  if (e.name == "wiener_lpdf") {
    if (e.args.size() < 5 || e.args.size() > 9)
      malformed(e.name + " call: expected 5 to 9 argument(s), got " +
                std::to_string(e.args.size()));
    return;
  }
  if (e.name == "wiener_lcdf_unnorm" || e.name == "wiener_lccdf_unnorm") {
    if (!wiener_form(e.name, e.args.size()))
      malformed(e.name + " call: expected 5, 6, 8 or 9 argument(s), got " +
                std::to_string(e.args.size()));
    return;
  }

  CallableTransformSpec transform;
  if (callable_transform(e.name, &transform)) {
    require_arity(e, transform.arity);
    return;
  }

  std::string ode_name = e.name;
  bool ode_tolerance = false;
  if (ode_name.size() > 4 &&
      ode_name.compare(ode_name.size() - 4, 4, "_tol") == 0) {
    ode_tolerance = true;
    ode_name.erase(ode_name.size() - 4);
  }
  const bool modern_ode = ode_name == "ode_bdf" || ode_name == "ode_adams" ||
                          ode_name == "ode_rk45" || ode_name == "ode_ckrk";
  if (modern_ode) {
    const size_t minimum = ode_tolerance ? 7 : 4;
    if (e.args.size() < minimum)
      malformed(e.name + " call: expected at least " + std::to_string(minimum) +
                " argument(s), got " + std::to_string(e.args.size()));
    return;
  }
  if (e.name.rfind("integrate_ode_", 0) == 0) {
    if (e.args.size() != 7 && e.args.size() != 10)
      malformed(e.name + " call: expected 7 or 10 argument(s), got " +
                std::to_string(e.args.size()));
  }

  // A registered name that matched neither a registry arity nor any per-name
  // rule above carries a malformed argument vector; reject it here rather
  // than letting it reach an unchecked args[k] in dispatch.
  if (function_registered(e.name))
    malformed(e.name + " call: no registered overload takes " +
              std::to_string(e.args.size()) + " argument(s)");
}

void validate_expression_shape(const Expr& e);

void validate_index_shape(const Expr& index) {
  if (!is_index(index)) malformed("index descriptor");
  validate_expression_shape(index);
}

void validate_expression_shape(const Expr& e) {
  switch (e.kind) {
    case Expr::Var:
    case Expr::LitInt:
    case Expr::LitReal:
    case Expr::LitStr:
    case Expr::Unsupported:
      if (!e.args.empty()) malformed("leaf expression argument list");
      break;
    case Expr::Promotion:  // the decoder rejects its tag
      malformed("Promotion expression");
    case Expr::Indexed:
      // args[0] is the base. stanc can retain a no-op Indexed wrapper with no
      // descriptors after transformations, so only the base is mandatory.
      // Index consumers address each descriptor according to its kind when
      // descriptors are present.
      if (e.args.empty()) malformed("Indexed expression arity");
      validate_expression_shape(e.args[0]);
      for (size_t i = 1; i < e.args.size(); ++i)
        validate_index_shape(e.args[i]);
      return;
    case Expr::TernaryIf:
      if (e.args.size() != 3) malformed("TernaryIf expression arity");
      break;
    case Expr::EOr:
      if (e.args.size() != 2) malformed("EOr expression arity");
      break;
    case Expr::EAnd:
      if (e.args.size() != 2) malformed("EAnd expression arity");
      break;
    case Expr::FunApp:
      validate_funapp_arity(e);
      break;
    case Expr::IndexAll:
      if (!e.args.empty()) malformed("IndexAll arity");
      break;
    case Expr::IndexSingle:
    case Expr::IndexUpfrom:
    case Expr::IndexMulti:
      if (e.args.size() != 1) malformed("index arity");
      break;
    case Expr::IndexBetween:
      if (e.args.size() != 2) malformed("IndexBetween arity");
      break;
  }
  for (const Expr& arg : e.args) validate_expression_shape(arg);
}

void validate_transform_shape(const Transform& transform) {
  size_t arity = 0;
  switch (transform.kind) {
    case Transform::Lower:
    case Transform::Upper:
    case Transform::Offset:
    case Transform::Multiplier:
      arity = 1;
      break;
    case Transform::LowerUpper:
    case Transform::OffsetMultiplier:
      arity = 2;
      break;
    case Transform::Identity:
    case Transform::Simplex:
    case Transform::Ordered:
    case Transform::PositiveOrdered:
    case Transform::CholeskyCorr:
    case Transform::UnitVector:
    case Transform::SumToZero:
    case Transform::Correlation:
    case Transform::Covariance:
    case Transform::CholeskyCov:
    case Transform::StochasticColumn:
    case Transform::StochasticRow:
      break;
    case Transform::Unsupported:
      for (const Expr& arg : transform.args) validate_expression_shape(arg);
      return;
  }
  if (transform.args.size() != arity)
    malformed("transform arity: expected " + std::to_string(arity) +
              " argument(s), got " + std::to_string(transform.args.size()));
  for (const Expr& arg : transform.args) validate_expression_shape(arg);
}

void validate_sized_type_shape(const SizedType& type) {
  size_t arity = 0;
  bool exact = true;
  if (type.base == "STuple") {
    arity = 0;
  } else if (type.base == "SVector" || type.base == "SRowVector") {
    arity = 1;
  } else if (type.base == "SMatrix") {
    arity = 2;
  } else if (type.base == "SArray") {
    size_t leaf_rank = 0;
    if (type.elem_base == "SVector" || type.elem_base == "SRowVector")
      leaf_rank = 1;
    else if (type.elem_base == "SMatrix")
      leaf_rank = 2;
    if (type.dims.size() <= leaf_rank)
      malformed("SArray sized type dimensions");
    exact = false;
  } else if (type.base != "SInt" && type.base != "SReal" &&
             type.base != "SComplex" && !type.base.empty()) {
    // Unknown sized types remain on the existing Unsupported path. Their
    // dimensions still contain expressions that must be structurally safe.
    exact = false;
  }
  if (exact && type.dims.size() != arity)
    malformed(type.base + " sized type dimensions: expected " +
              std::to_string(arity) + ", got " +
              std::to_string(type.dims.size()));
  for (const Expr& dim : type.dims) validate_expression_shape(dim);
}

size_t print_arguments(const PrintItem& item, bool top) {
  if (item.kind == PrintItem::Kind::Argument) {
    if (item.depth != 0 || !item.components.empty())
      malformed("print layout argument item");
    return 1;
  }
  if (item.components.empty() || (top && item.depth == 0))
    malformed("print layout tuple item");
  size_t count = 0;
  for (const PrintItem& component : item.components)
    count += print_arguments(component, false);
  return count;
}

void validate_statement_shape(const Stmt& statement) {
  validate_sized_type_shape(statement.decl_type);
  if (!statement.print_layout.empty()) {
    size_t count = 0;
    for (const PrintItem& item : statement.print_layout)
      count += print_arguments(item, true);
    if (count != statement.fn_args.size())
      malformed("print layout does not take every argument");
  }
  if (statement.kind == Stmt::MultiAssign) {
    if (statement.rhs.kind != Expr::FunApp || statement.fn_args.size() < 2)
      malformed("MultiAssign statement");
    for (const Expr& target : statement.fn_args)
      if (target.kind != Expr::Var) malformed("MultiAssign target");
  }
  if (statement.kind == Stmt::Return &&
      (statement.fn_args.size() == 1 ||
       (statement.has_init && !statement.fn_args.empty())))
    malformed("Return statement values");
  for (const Expr* expression :
       {&statement.init, &statement.rhs, &statement.target, &statement.lower,
        &statement.upper, &statement.cond})
    validate_expression_shape(*expression);
  for (const std::vector<Expr>* expressions :
       {&statement.read_dims, &statement.lhs_idx, &statement.fn_args})
    for (const Expr& expression : *expressions)
      validate_expression_shape(expression);
  for (const Expr& index : statement.lhs_idx) validate_index_shape(index);
  for (const std::optional<Transform>* transform :
       {&statement.read_transform, &statement.check_transform,
        &statement.write_transform})
    if (*transform) validate_transform_shape(**transform);

  switch (statement.kind) {
    case Stmt::For:
      if (statement.body.size() != 1) malformed("For statement body arity");
      break;
    case Stmt::While:
      if (statement.body.size() != 1) malformed("While statement body arity");
      break;
    case Stmt::IfElse:
      if (statement.body.empty() || statement.body.size() > 2)
        malformed("IfElse statement body arity");
      break;
    case Stmt::Block:
    case Stmt::SList:
      break;
    default:
      if (!statement.body.empty()) malformed("statement body");
      break;
  }
  for (const Stmt& child : statement.body) validate_statement_shape(child);
}

void validate_program_shape(const Program& program) {
  for (const auto& input : program.input_vars)
    validate_sized_type_shape(input.second);
  for (const OutputVar& output : program.output_vars)
    validate_sized_type_shape(output.type);
  for (const std::vector<Stmt>* body :
       {&program.prepare_data, &program.log_prob, &program.generate_quantities,
        &program.transform_inits})
    for (const Stmt& statement : *body) validate_statement_shape(statement);
  for (const FunDef& function : program.fun_defs) {
    const size_t arity = function.arg_names.size();
    if (function.arg_views.size() != arity ||
        function.arg_data_only.size() != arity)
      malformed("function argument field lengths disagree");
    for (const Stmt& statement : function.body)
      validate_statement_shape(statement);
  }
}

struct Binding {
  UnsizedView view;
  bool declared_data_only = false;
};

using Bindings = std::map<std::string, Binding>;

class BindingScope {
 public:
  explicit BindingScope(Bindings& bindings) : bindings_(bindings) {}
  BindingScope(const BindingScope&) = delete;
  BindingScope& operator=(const BindingScope&) = delete;
  ~BindingScope() {
    for (auto it = saved_.rbegin(); it != saved_.rend(); ++it) {
      if (it->second)
        bindings_[it->first] = *it->second;
      else
        bindings_.erase(it->first);
    }
  }
  void bind(const std::string& name, const Binding& binding) {
    const auto found = bindings_.find(name);
    saved_.emplace_back(name, found == bindings_.end()
                                  ? std::nullopt
                                  : std::optional<Binding>(found->second));
    bindings_[name] = binding;
  }

 private:
  Bindings& bindings_;
  std::vector<std::pair<std::string, std::optional<Binding>>> saved_;
};
using Functions = std::map<std::string, const FunDef*>;

UnsizedView declared_view(const SizedType& type) {
  if (type.unsized.leaf != UnsizedLeaf::Unknown) return type.unsized;

  const std::string& base = type.base == "SArray" ? type.elem_base : type.base;
  UnsizedView view;
  if (base == "SInt")
    view.leaf = UnsizedLeaf::Int;
  else if (base == "SReal")
    view.leaf = UnsizedLeaf::Real;
  else if (base == "SComplex")
    view.leaf = UnsizedLeaf::Complex;
  else if (base == "SVector")
    view.leaf = UnsizedLeaf::Vector;
  else if (base == "SRowVector")
    view.leaf = UnsizedLeaf::RowVector;
  else if (base == "SMatrix")
    view.leaf = UnsizedLeaf::Matrix;
  if (type.base != "SArray") return view;
  // A complex container has no view, in an array as on its own; the
  // lowering reports that complex numbers are not supported yet.
  if (base.rfind("SComplex", 0) == 0 && base != "SComplex") return view;

  size_t leaf_rank = 0;
  if (view.leaf == UnsizedLeaf::Matrix)
    leaf_rank = 2;
  else if (view.leaf == UnsizedLeaf::Vector ||
           view.leaf == UnsizedLeaf::RowVector)
    leaf_rank = 1;
  if (view.leaf == UnsizedLeaf::Unknown || type.dims.size() < leaf_rank ||
      type.dims.size() - leaf_rank > std::numeric_limits<uint8_t>::max())
    throw mir::MirError("mir: malformed sized declaration type");
  view.depth = static_cast<uint8_t>(type.dims.size() - leaf_rank);
  return view;
}

void validate_bindings(const std::vector<Stmt>& body, Bindings& bindings,
                       BindingScope* scope, const Functions& functions);

void validate_expression(const Expr& e, const Bindings& bindings,
                         const Functions& functions,
                         bool several_results = false) {
  if (e.kind == Expr::Var && !e.promoted) {
    const auto binding = bindings.find(e.name);
    if (binding != bindings.end() &&
        binding->second.view.leaf != UnsizedLeaf::Unknown &&
        (binding->second.view.depth != e.unsized.depth ||
         binding->second.view.leaf != e.unsized.leaf))
      throw mir::MirError("mir: variable type disagrees with its binding for " +
                          e.name);
    // Do not equate data_only with the declaration here. In write_array,
    // stanc intentionally marks constrained-parameter reads DataOnly even
    // though the corresponding declaration was AutoDiffable. The narrower
    // checks below retain the phase-sensitive adlevel invariants needed for
    // categorical and data-only UDF arguments.
  }
  if (e.kind == Expr::FunApp && e.fn_lib == Expr::Lib::StanLib &&
      (e.name == "categorical_lpmf" || e.name == "categorical_logit_lpmf")) {
    if (e.args.size() != 2 || e.args[0].unsized.leaf != UnsizedLeaf::Int ||
        e.args[0].unsized.depth > 1 ||
        e.args[1].unsized.leaf != UnsizedLeaf::Vector ||
        e.args[1].unsized.depth != 0)
      throw mir::MirError("mir: malformed categorical signature");
    for (const Expr& arg : e.args) {
      if (arg.kind != Expr::Var) continue;
      const auto binding = bindings.find(arg.name);
      if (binding == bindings.end())
        throw mir::MirError("mir: categorical argument has no binding");
      if (binding->second.view.depth != arg.unsized.depth ||
          binding->second.view.leaf != arg.unsized.leaf)
        throw mir::MirError(
            "mir: categorical argument type disagrees with its binding");
      // Do not compare adlevels here. stanc specializes an inlined user
      // function to its caller: the declaration can retain the function's
      // AutoDiffable type while a transformed-data or generated-quantities
      // expression is correctly marked DataOnly. Execution derives activity
      // from the actual binding (or the interpreter scalar type), just as it
      // does for every other density.
    }
  }
  if (e.kind == Expr::FunApp && e.fn_lib == Expr::Lib::UserDefined) {
    const auto found = functions.find(function_key(e));
    if (found == functions.end())
      throw mir::MirError("mir: call to unknown user function " + e.name);
    const FunDef& function = *found->second;
    if (e.args.size() != function.arg_names.size())
      throw mir::MirError("mir: user function arity mismatch for " + e.name);
    if (function.results.size() > 1 && !several_results)
      throw mir::MirError("mir: " + e.name +
                          " returns several values outside a MultiAssign");
    for (size_t i = 0; i < e.args.size(); ++i) {
      if (e.args[i].unsized.depth != function.arg_views[i].depth ||
          e.args[i].unsized.leaf != function.arg_views[i].leaf)
        throw mir::MirError("mir: user function argument type mismatch for " +
                            e.name);
      if (e.args[i].kind == Expr::Var && !e.args[i].promoted) {
        const auto binding = bindings.find(e.args[i].name);
        if (binding == bindings.end() ||
            binding->second.view.depth != e.args[i].unsized.depth ||
            binding->second.view.leaf != e.args[i].unsized.leaf)
          throw mir::MirError(
              "mir: user function argument type disagrees with its binding "
              "for " +
              e.name);
      }
      if (function.arg_data_only[i] && !e.args[i].data_only)
        throw mir::MirError(
            "mir: data-only function argument depends on a parameter");
    }
  }
  for (const Expr& arg : e.args) validate_expression(arg, bindings, functions);
}

// The number of values a supported library function returning a tuple
// returns. Lowering refuses the others.
std::optional<size_t> library_results(const std::string& name) {
  static const std::map<std::string, size_t> results = {
      {"qr", 2},
      {"qr_thin", 2},
      {"eigendecompose_sym", 2},
      {"svd", 3},
      {"csr_extract", 3}};
  const auto found = results.find(name);
  return found == results.end() ? std::nullopt
                                : std::optional<size_t>(found->second);
}

void validate_multi_assign(const Stmt& s, const Bindings& bindings,
                           const Functions& functions) {
  const Expr& call = s.rhs;
  std::vector<UnsizedView> results;
  if (call.fn_lib == Expr::Lib::UserDefined) {
    const auto found = functions.find(function_key(call));
    if (found == functions.end())
      throw mir::MirError("mir: call to unknown user function " + call.name);
    results = found->second->results;
  } else if (call.fn_lib == Expr::Lib::StanLib) {
    const auto count = library_results(call.name);
    if (count && *count != s.fn_args.size())
      throw mir::MirError("mir: " + call.name + " returns " +
                          std::to_string(*count) + " values, not " +
                          std::to_string(s.fn_args.size()));
  } else {
    throw mir::MirError("mir: MultiAssign of a compiler-internal function");
  }
  if (call.fn_lib == Expr::Lib::UserDefined &&
      results.size() != s.fn_args.size())
    throw mir::MirError("mir: " + call.name + " returns " +
                        std::to_string(results.size()) + " values, not " +
                        std::to_string(s.fn_args.size()));
  std::set<std::string> targets;
  for (size_t k = 0; k < s.fn_args.size(); ++k) {
    const Expr& target = s.fn_args[k];
    if (!targets.insert(target.name).second)
      throw mir::MirError("mir: MultiAssign assigns " + target.name + " twice");
    const auto binding = bindings.find(target.name);
    if (binding == bindings.end())
      throw mir::MirError("mir: MultiAssign target " + target.name +
                          " is not declared");
    const UnsizedView declared = binding->second.view;
    if (k < results.size() && declared.leaf != UnsizedLeaf::Unknown &&
        (declared.depth != results[k].depth ||
         declared.leaf != results[k].leaf))
      throw mir::MirError("mir: MultiAssign target type disagrees for " +
                          target.name);
  }
  validate_expression(call, bindings, functions, true);
}

void validate_bindings(const Stmt& s, Bindings& bindings, BindingScope* scope,
                       const Functions& functions) {
  if (s.kind == Stmt::MultiAssign)
    return validate_multi_assign(s, bindings, functions);
  for_each_expr(
      s, [&](const Expr& e) { validate_expression(e, bindings, functions); });
  if (const auto call = user_defined_statement_call(s))
    validate_expression(*call, bindings, functions);
  if (internal_name(s) == "FnCheck") {
    if (!s.check_transform)
      throw mir::MirError("mir: FnCheck has no transform");
    const Transform::Kind kind = s.check_transform->kind;
    if (is_structured_check(kind)) {
      if (!s.check_transform->args.empty() || s.fn_args.size() != 1)
        throw mir::MirError("mir: malformed structured FnCheck");
      const UnsizedLeaf leaf = s.fn_args[0].unsized.leaf;
      const bool matrix = leaf == UnsizedLeaf::Matrix;
      const bool vector = leaf == UnsizedLeaf::Vector;
      const bool matrix_only = is_matrix_only_check(kind);
      const bool vector_only = kind != Transform::SumToZero && !matrix_only;
      if ((!matrix && !vector) || (matrix_only && !matrix) ||
          (vector_only && !vector))
        throw mir::MirError(
            "mir: structured FnCheck transform and value type disagree");
    } else if ((kind == Transform::Lower || kind == Transform::Upper) &&
               s.check_transform->args.size() == 1 && s.fn_args.size() == 2) {
      // Exact scalar/container compatibility is value- and shape-dependent;
      // the lowering/interpreter check it at the original statement site.
    } else {
      throw mir::MirError("mir: unsupported or malformed FnCheck");
    }
    const Expr& value = s.fn_args[0];
    // --O1 constant propagation may substitute the checked value itself
    // (`(var 2)` where the source said `(var K)`); a non-Var value carries
    // its own type and has no declaration to cross-check.
    if (value.kind == Expr::Var) {
      const auto binding = bindings.find(value.name);
      if (binding == bindings.end() ||
          binding->second.view.depth != value.unsized.depth ||
          binding->second.view.leaf != value.unsized.leaf)
        throw mir::MirError(
            "mir: FnCheck value type disagrees with its declaration");
    }
  }
  if (s.kind == Stmt::Assignment && s.lhs_idx.empty()) {
    const auto binding = bindings.find(s.lhs);
    if (binding != bindings.end() &&
        binding->second.view.leaf != UnsizedLeaf::Unknown &&
        s.rhs.unsized.leaf != UnsizedLeaf::Unknown &&
        (binding->second.view.depth != s.rhs.unsized.depth ||
         binding->second.view.leaf != s.rhs.unsized.leaf))
      throw mir::MirError(
          "mir: assignment type disagrees with its binding for " + s.lhs);
  }
  if (s.kind == Stmt::Decl) {
    const UnsizedView view = declared_view(s.decl_type);
    if (s.decl_type.unsized.leaf != UnsizedLeaf::Unknown && s.has_init &&
        s.init.unsized.leaf != UnsizedLeaf::Unknown &&
        (view.depth != s.init.unsized.depth ||
         view.leaf != s.init.unsized.leaf))
      throw mir::MirError(
          "mir: declaration initializer type disagrees with its binding for " +
          s.decl_id);
    const Binding declared{view, s.decl_data_only};
    if (scope)
      scope->bind(s.decl_id, declared);
    else
      bindings[s.decl_id] = declared;
  }
  if (s.kind == Stmt::SList) {
    validate_bindings(s.body, bindings, scope, functions);
  } else if (s.kind == Stmt::IfElse) {
    for (const auto& child : s.body) {
      BindingScope branch(bindings);
      validate_bindings(child, bindings, &branch, functions);
    }
  } else if (!s.body.empty()) {
    BindingScope nested(bindings);
    if (s.kind == Stmt::For)
      nested.bind(s.loopvar, Binding{UnsizedView{0, UnsizedLeaf::Int}, true});
    validate_bindings(s.body, bindings, &nested, functions);
  }
}

void validate_bindings(const std::vector<Stmt>& body, Bindings& bindings,
                       BindingScope* scope, const Functions& functions) {
  for (const auto& s : body) validate_bindings(s, bindings, scope, functions);
}

// stanc3 keeps every overload of a user function under one fdname, and calls
// carry only that name, so the by-name function maps downstream would
// collide (last definition wins). Give each overload its signature and
// point every call site at the overload its argument types select. stanc3
// already ran overload resolution and inserted promotions at typecheck
// time, so a call's (leaf, depth) views match exactly one signature.

using Overloads = std::map<std::string, std::vector<FunDef*>>;

std::string signature(const std::vector<UnsizedView>& views) {
  std::string out = "(";
  for (size_t i = 0; i < views.size(); ++i) {
    if (i) out += ',';
    switch (views[i].leaf) {
      case UnsizedLeaf::Int:
        out += "int";
        break;
      case UnsizedLeaf::Real:
        out += "real";
        break;
      case UnsizedLeaf::Complex:
        out += "complex";
        break;
      case UnsizedLeaf::Vector:
        out += "vector";
        break;
      case UnsizedLeaf::RowVector:
        out += "row_vector";
        break;
      case UnsizedLeaf::Matrix:
        out += "matrix";
        break;
      default:
        out += "?";
        break;
    }
    for (uint8_t d = 0; d < views[i].depth; ++d) out += "[]";
  }
  return out + ")";
}

bool views_match(const std::vector<UnsizedView>& a,
                 const std::vector<UnsizedView>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (a[i].depth != b[i].depth || a[i].leaf != b[i].leaf) return false;
  return true;
}

bool views_match(const std::vector<Expr>& args,
                 const std::vector<UnsizedView>& views) {
  if (args.size() != views.size()) return false;
  for (size_t i = 0; i < args.size(); ++i)
    if (args[i].unsized.depth != views[i].depth ||
        args[i].unsized.leaf != views[i].leaf)
      return false;
  return true;
}

template <typename Args>
std::string resolve_overload(const Overloads::value_type& candidates,
                             const Args& args) {
  const FunDef* match = nullptr;
  for (const FunDef* f : candidates.second) {
    if (!views_match(args, f->arg_views)) continue;
    if (match)
      throw mir::MirError("mir: ambiguous overload for " + candidates.first);
    match = f;
  }
  if (!match)
    throw mir::MirError("mir: no overload of " + candidates.first +
                        " matches its call");
  return match->overload;
}

void resolve_calls(Expr& e, const Overloads& overloads) {
  for (Expr& a : e.args) resolve_calls(a, overloads);
  const bool call =
      e.kind == Expr::FunApp && e.fn_lib == Expr::Lib::UserDefined;
  const bool callback = e.kind == Expr::Var && e.fn_type;
  if (!call && !callback) return;
  const auto it = overloads.find(e.name);
  if (it == overloads.end()) return;
  e.fn_overload = call ? resolve_overload(*it, e.args)
                       : resolve_overload(*it, e.fn_type->args);
}

void resolve_calls(Stmt& s, const Overloads& overloads) {
  if (s.kind == Stmt::NRFunApp && s.fn_lib == Expr::Lib::UserDefined) {
    const auto it = overloads.find(s.fn_name);
    if (it != overloads.end()) s.fn_overload = resolve_overload(*it, s.fn_args);
  }
  for_each_expr(s, [&](Expr& e) { resolve_calls(e, overloads); });
  for (Stmt& k : s.body) resolve_calls(k, overloads);
}

void unmangle(std::string& name) {
  constexpr std::string_view prefix = "_stan_";
  if (name.compare(0, prefix.size(), prefix) == 0) name.erase(0, prefix.size());
}

void unmangle(Expr& e) {
  unmangle(e.name);
  for (Expr& arg : e.args) unmangle(arg);
}

void unmangle(SizedType& type) {
  for_each_dim(type, [](Expr& dim) { unmangle(dim); });
}

void unmangle(Stmt& s) {
  for (std::string* name :
       {&s.decl_id, &s.lhs, &s.loopvar, &s.fn_name, &s.check_var_name})
    unmangle(*name);
  for_each_expr(s, [](Expr& e) { unmangle(e); });
  for (Stmt& child : s.body) unmangle(child);
}

void unmangle(Program& prog) {
  for (auto& [name, type] : prog.input_vars) {
    unmangle(name);
    unmangle(type);
  }
  for (auto* body : {&prog.prepare_data, &prog.log_prob,
                     &prog.generate_quantities, &prog.transform_inits})
    for (Stmt& s : *body) unmangle(s);
  for (FunDef& f : prog.fun_defs) {
    unmangle(f.name);
    for (std::string& arg : f.arg_names) unmangle(arg);
    for (Stmt& s : f.body) unmangle(s);
  }
  for (OutputVar& var : prog.output_vars) {
    unmangle(var.name);
    unmangle(var.type);
  }
}

void resolve_overloads(Program& prog) {
  std::map<std::string, std::vector<FunDef*>> by_name;
  for (FunDef& f : prog.fun_defs) by_name[f.name].push_back(&f);
  Overloads overloads;
  for (auto& [name, defs] : by_name) {
    if (defs.size() < 2) continue;
    for (size_t i = 0; i < defs.size(); ++i)
      for (size_t j = i + 1; j < defs.size(); ++j)
        if (views_match(defs[i]->arg_views, defs[j]->arg_views))
          throw mir::MirError("mir: indistinguishable overloads of " + name);
    for (FunDef* f : defs) f->overload = signature(f->arg_views);
    overloads[name] = std::move(defs);
  }
  for (auto& [name, type] : prog.input_vars)
    for_each_dim(type, [&](Expr& d) { resolve_calls(d, overloads); });
  for (auto* body : {&prog.prepare_data, &prog.log_prob,
                     &prog.generate_quantities, &prog.transform_inits})
    for (Stmt& s : *body) resolve_calls(s, overloads);
  for (FunDef& f : prog.fun_defs)
    for (Stmt& s : f.body) resolve_calls(s, overloads);
}

}  // namespace

SizedType detail::unsized_declaration(const UnsizedView& view,
                                      std::string raw) {
  SizedType type;
  type.raw = std::move(raw);
  if (view.depth == 0 && view.leaf == UnsizedLeaf::Int) {
    type.base = "SInt";
  } else if (view.depth == 0 && view.leaf == UnsizedLeaf::Real) {
    type.base = "SReal";
  } else {
    type.unsized = view;
  }
  return type;
}

std::string detail::unmangled(std::string name) {
  unmangle(name);
  return name;
}

UnsizedView detail::declaration_view(const SizedType& type) {
  return declared_view(type);
}

void detail::validate_portable_program(const Program& prog) {
  validate_program_shape(prog);
}

namespace {

// stanc3 wraps an argument in eval() to force an Eigen expression; the value
// is the argument's.
void strip_eval(Expr& e) {
  for (Expr& arg : e.args) strip_eval(arg);
  if (e.kind == Expr::FunApp && e.fn_lib == Expr::Lib::StanLib &&
      e.name == "eval" && e.args.size() == 1) {
    Expr inner = std::move(e.args[0]);
    e = std::move(inner);
  }
}

void strip_eval(Stmt& s) {
  for_each_expr(s, [](Expr& e) { strip_eval(e); });
  for (Stmt& child : s.body) strip_eval(child);
}

void strip_eval(Program& prog) {
  for (auto* body : {&prog.prepare_data, &prog.log_prob,
                     &prog.generate_quantities, &prog.transform_inits})
    for (Stmt& s : *body) strip_eval(s);
  for (FunDef& f : prog.fun_defs)
    for (Stmt& s : f.body) strip_eval(s);
}

}  // namespace

namespace {

// Each return gives as many values as the function has results.
void validate_returns(const Stmt& s, const FunDef& f) {
  if (s.kind == Stmt::Return) {
    const size_t values = s.has_init ? 1 : s.fn_args.size();
    if (values != f.results.size())
      throw mir::MirError("mir: a return in " + f.name + " gives " +
                          std::to_string(values) + " values, not " +
                          std::to_string(f.results.size()));
  }
  for (const Stmt& child : s.body) validate_returns(child, f);
}

}  // namespace

void detail::finalize_program(Program& prog) {
  strip_eval(prog);
  unmangle(prog);
  resolve_overloads(prog);
  finish_multi_assigns(prog);
  record_tuple_reads(prog);
  Functions functions;
  for (const auto& f : prog.fun_defs) {
    const auto inserted = functions.emplace(function_key(f), &f);
    if (!inserted.second)
      throw mir::MirError(
          "mir: duplicate function name after overload resolution: " +
          function_key(f));
  }
  Bindings inputs;
  for (const auto& [name, type] : prog.input_vars)
    inputs[name] = Binding{declared_view(type), true};
  Bindings prepare_bindings = inputs;
  validate_bindings(prog.prepare_data, prepare_bindings, nullptr, functions);
  Bindings log_prob_bindings = prepare_bindings;
  validate_bindings(prog.log_prob, log_prob_bindings, nullptr, functions);
  Bindings gq_bindings = prepare_bindings;
  validate_bindings(prog.generate_quantities, gq_bindings, nullptr, functions);
  // transform_inits sees data and transformed data and declares each
  // parameter locally, exactly like generate_quantities does.
  Bindings init_bindings = prepare_bindings;
  validate_bindings(prog.transform_inits, init_bindings, nullptr, functions);
  for (const auto& f : prog.fun_defs) {
    for (const Stmt& s : f.body) validate_returns(s, f);
    Bindings args;
    for (size_t i = 0; i < f.arg_names.size(); ++i)
      args[f.arg_names[i]] = Binding{f.arg_views[i], f.arg_data_only[i]};
    validate_bindings(f.body, args, nullptr, functions);
  }
}

}  // namespace mir
}  // namespace stanli
