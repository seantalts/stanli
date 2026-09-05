#include "lower_internal.hpp"

namespace stanli {
namespace lower_detail {

Lowering::BuiltinDispatch Lowering::resolve_builtin(const mir::Expr& e) {
  if (const auto higher_order = mir::higher_order_call(e)) {
    switch (higher_order->family) {
      case mir::HigherOrderFamily::ReduceSum:
        return {BuiltinFamily::ReduceSum};
      case mir::HigherOrderFamily::MapRect:
        return {BuiltinFamily::MapRect};
      case mir::HigherOrderFamily::Algebra:
        return {BuiltinFamily::Algebra};
      case mir::HigherOrderFamily::Ode:
        return {BuiltinFamily::Ode};
      case mir::HigherOrderFamily::Integrate1D:
        return {BuiltinFamily::Quadrature};
      case mir::HigherOrderFamily::Dae:
        return {BuiltinFamily::Dae};
    }
  }
  // Bespoke functions still own their semantic checks. This registry only
  // selects the handler, replacing the old sequence in which every family
  // was probed and declined in turn.
  static const std::unordered_map<std::string_view, BuiltinDispatch> kBuiltins =
      {
          {"multi_normal_rng", BuiltinFamily::MultiNormalRng},
          {"dirichlet_rng", BuiltinFamily::DirichletRng},
          {"categorical_rng", BuiltinFamily::CategoricalRng},
          {"categorical_logit_rng", BuiltinFamily::CategoricalRng},
          {"append_array", BuiltinFamily::AppendArray},
          {"tcrossprod", BuiltinFamily::Matrix},
          {"diag_pre_multiply", BuiltinFamily::Matrix},
          {"diag_post_multiply", BuiltinFamily::Matrix},
          {"rep_matrix", BuiltinFamily::Matrix},
          {"gp_exp_quad_cov", BuiltinFamily::Matrix},
          {"gp_matern32_cov", BuiltinFamily::Matrix},
          {"gp_matern52_cov", BuiltinFamily::Matrix},
          {"gp_exponential_cov", BuiltinFamily::Matrix},
          {"quad_form_diag", BuiltinFamily::Matrix},
          {"append_row", BuiltinFamily::Matrix},
          {"append_col", BuiltinFamily::Matrix},
          {"rows", BuiltinFamily::ShapeQuery},
          {"cols", BuiltinFamily::ShapeQuery},
          {"size", BuiltinFamily::ShapeQuery},
          {"num_elements", BuiltinFamily::ShapeQuery},

      };
  const auto builtin = kBuiltins.find(e.name);
  if (builtin != kBuiltins.end()) return builtin->second;
  if (const FunctionSpec* registered = function_spec(e)) {
    if (registered->builtin() != nullptr) {
      // A registered scalar RNG routes to the stream-owning family, not
      // the pure-kernel dispatch a builtin descriptor normally selects.
      if (registered->builtin()->shape == BuiltinShapePolicy::Rng)
        return rng_dispatch(registered->builtin()->rng);
      return {BuiltinFamily::Elementwise, registered->builtin()};
    }
    return {BuiltinFamily::Density, nullptr, registered->density()};
  }
  // A known name whose concrete overload does not match a registry spec
  // may still have an established specialized path below (notably integer
  // operator spellings). Preserve that dispatch without attaching the
  // incompatible descriptor.
  if (function_registered(e.name)) return {BuiltinFamily::Elementwise};
  // Keep the scalar-RNG vocabulary in the shared classifier used by the
  // graph, interpreter, and runtime-region compiler. The selected family is
  // still carried into the handler, so dispatch performs this lookup once.
  if (const ScalarRng* rng = scalar_rng_family(e.name))
    return rng_dispatch(*rng);
  if (ends_with(e.name, "_lpdf") || ends_with(e.name, "_lpmf") ||
      ends_with(e.name, "_cdf") || ends_with(e.name, "_ccdf") ||
      ends_with(e.name, "_lcdf") || ends_with(e.name, "_lccdf"))
    return {BuiltinFamily::Density};
  CallableTransformSpec transform;
  if (callable_transform(e.name, &transform))
    return {BuiltinFamily::CallableTransform};
  return {};
}
// Fallback for expressions with no native lowering: a data-only subtree
// is evaluated at compile time and materialized as a constant. Unsupported
// expressions and Stan validation failures decline; the latter must stay
// at model evaluation rather than move to construction. Propto densities
// never fold because their value is instantiation-dependent.
bool Lowering::expr_effectful(const mir::Expr& e) {
  if (mir::stateful_intrinsic_kind(e)) return true;
  if (e.kind == mir::Expr::FunApp && e.name.size() >= 4 &&
      e.name.compare(e.name.size() - 4, 4, "_rng") == 0)
    return true;
  if (e.kind == mir::Expr::FunApp && e.fn_lib == mir::Expr::Lib::UserDefined &&
      fun_effectful(e.name))
    return true;
  // reduce_sum reaches its partial-sum function through a Var, so the
  // UserDefined test above cannot see a print or reject in that body.
  if (mir::is_reduce_sum(e) && reduce_sum_effectful(e)) return true;
  for (const auto& a : e.args)
    if (expr_effectful(a)) return true;
  return false;
}
bool Lowering::fun_effectful(const std::string& name) {
  auto memo = effectful_cache.find(name);
  if (memo != effectful_cache.end()) return memo->second;

  // Recursion is not itself an observable effect.  Walk the complete
  // reachable call graph for this query, treating an edge back into the
  // active component as already being examined.  Do not memoize an
  // intermediate node: in an effectful recursive component its answer can
  // depend on statements that the outer frame has not visited yet.
  std::set<std::string> visiting;
  std::function<bool(const std::string&)> visit_fun;
  std::function<bool(const mir::Expr&)> visit_expr;
  std::function<bool(const mir::Stmt&)> visit_stmt;

  visit_fun = [&](const std::string& called) {
    auto known = effectful_cache.find(called);
    if (known != effectful_cache.end()) return known->second;
    if (!visiting.insert(called).second) return false;
    bool found = false;
    auto f = fun_defs.find(called);
    if (f != fun_defs.end())
      for (const auto& s : f->second->body)
        if (visit_stmt(s)) {
          found = true;
          break;
        }
    visiting.erase(called);
    return found;
  };

  visit_expr = [&](const mir::Expr& e) {
    if (e.kind == mir::Expr::FunApp && e.name.size() >= 4 &&
        e.name.compare(e.name.size() - 4, 4, "_rng") == 0)
      return true;
    if (e.kind == mir::Expr::FunApp &&
        e.fn_lib == mir::Expr::Lib::UserDefined && visit_fun(e.name))
      return true;
    if (mir::is_reduce_sum(e)) {
      if (e.args.empty() || e.args[0].kind != mir::Expr::Var) return true;
      bool propto = false;
      const mir::FunDef* partial = mir::resolve_callback(
          fun_defs, mir::reduce_sum_partial_name(e.args[0].name, &propto),
          mir::reduce_sum_partial_views(e));
      if (partial == nullptr || visit_fun(partial->name)) return true;
    }
    for (const auto& a : e.args)
      if (visit_expr(a)) return true;
    return false;
  };

  visit_stmt = [&](const mir::Stmt& s) {
    if (s.kind == mir::Stmt::NRFunApp && message_action(s.fn_name)) return true;
    for (const auto& e : s.fn_args)
      if (visit_expr(e)) return true;
    if (s.has_init && visit_expr(s.init)) return true;
    if (visit_expr(s.rhs) || visit_expr(s.target) || visit_expr(s.lower) ||
        visit_expr(s.upper) || visit_expr(s.cond))
      return true;
    for (const auto& e : s.lhs_idx)
      if (visit_expr(e)) return true;
    for (const auto& child : s.body)
      if (visit_stmt(child)) return true;
    return false;
  };

  const bool effect = visit_fun(name);
  effectful_cache[name] = effect;
  return effect;
}
// Integer argument of a density/pmf: values must be known at compile
// time (int data, loop variables, or compile-time expressions).
std::vector<int> Lowering::int_arg_values(LoweredArgument& actual) {
  const mir::Expr& oc = actual.expr();
  if (oc.kind == mir::Expr::Var) {
    DataMap::Entry* en = td.find(oc.name);
    if (en && en->is_int && !en->i.empty()) return en->i;
    if (int_env.count(oc.name)) return {static_cast<int>(int_env[oc.name])};
  }
  if (oc.kind == mir::Expr::LitInt) return {static_cast<int>(oc.lit_i)};
  if (oc.kind == mir::Expr::Indexed) {
    // May be a slice (y[i] on a 2-D array yields a whole row), so
    // evaluate through the data interpreter, not scalar eval_int.
    DataMap::Entry v = eval_pure(oc, "an integer density argument");
    if (v.is_int && !v.i.empty()) return v.i;
  }
  if (oc.kind == mir::Expr::FunApp) {
    // Compile-time int expression (e.g. sum(y[n]) under an unrolled loop).
    return {static_cast<int>(eval_int(oc))};
  }
  fail("int argument must be int data (kind=" + std::to_string((int)oc.kind) +
           " type=" + oc.type_ + ")",
       oc.raw);
}
// Stan's bound transforms, callable as ordinary functions rather than
// written on a declaration. `<t>_constrain(x, bounds...)` is the value
// half of the declaration transform, `<t>_jacobian(...)` is the same
// value and also adds the transform's log absolute jacobian determinant
// to the target, and `<t>_unconstrain(y, bounds...)` is the inverse.
//
// stanc3 marks the jacobian direction with an FnJacobian suffix and emits
// no separate target statement for it, so the increment has to come from
// here -- and only in log_prob, because the generated model instantiates
// write_array with `jacobian__ = false`, which drops it.
//
// Argument 0 always carries the result's shape: every signature in the
// library pairs it either with scalar bounds or with bounds of exactly
// its own type, and none of them widens a scalar first argument against a
// container bound.
std::optional<Lowering::Val> Lowering::lower_callable_transform(
    const mir::Expr& e, CallArguments& actuals) {
  CallableTransformSpec tr;
  if (!callable_transform(e.name, &tr)) return std::nullopt;
  actuals.require_arity(tr.arity);

  if (tr.structured) {
    // The inverse structured transforms are not needed by Jacobian calls
    // and do not share the constrain kernels' two-output protocol.
    if (tr.direction == TransformDirection::Unconstrain) return std::nullopt;
    Val raw = actuals.at(0).value();
    ViewKind leaf = raw.si.kind;
    std::vector<int64_t> dims;
    if (is_array(raw.si)) {
      const ArrayShape& a = array_shape(raw.si);
      dims = a.dims;
      leaf = a.leaf;
    } else if (is_matrix(raw.si)) {
      dims = {raw.si.rows, raw.si.cols};
    } else if (is_vector(raw.si) || is_row_vector(raw.si)) {
      dims = {g.slots[raw.slot].len};
    }
    const size_t rank = (size_t)leaf_rank(leaf);
    if (rank == 0 || dims.size() < rank)
      fail(e.name + ": first argument has an invalid container type", e.raw);
    const size_t outer_rank = dims.size() - rank;
    std::vector<int64_t> outer(dims.begin(), dims.begin() + outer_rank);
    const int64_t batch = checked_product(outer, e.name + " batch");
    int64_t raw_rows = leaf == ViewKind::Matrix ? dims[dims.size() - 2] : 0;
    int64_t raw_cols = leaf == ViewKind::Matrix ? dims.back() : 0;
    int64_t out_rows = 0, out_cols = 0;
    ViewKind out_leaf = leaf;
    uint16_t opcode = tr.opcode;

    switch (tr.kind) {
      case CallableTransformKind::Ordered:
      case CallableTransformKind::PositiveOrdered:
        if (leaf != ViewKind::Vector) fail(e.name + ": expected vector", e.raw);
        out_rows = dims.back();
        break;
      case CallableTransformKind::Simplex:
        if (leaf != ViewKind::Vector) fail(e.name + ": expected vector", e.raw);
        out_rows = dims.back() + 1;
        break;
      case CallableTransformKind::UnitVector:
        if (leaf != ViewKind::Vector) fail(e.name + ": expected vector", e.raw);
        out_rows = dims.back();
        break;
      case CallableTransformKind::SumToZero:
        if (leaf == ViewKind::Vector) {
          out_rows = dims.back() + 1;
        } else if (leaf == ViewKind::Matrix) {
          out_rows = raw_rows + 1;
          out_cols = raw_cols + 1;
          opcode = OP_CONSTRAIN_SUM_TO_ZERO_MAT;
        } else {
          fail(e.name + ": expected vector or matrix", e.raw);
        }
        break;
      case CallableTransformKind::StochasticColumn:
      case CallableTransformKind::StochasticRow:
        if (leaf != ViewKind::Matrix) fail(e.name + ": expected matrix", e.raw);
        out_rows =
            raw_rows + (tr.kind == CallableTransformKind::StochasticColumn);
        out_cols = raw_cols + (tr.kind == CallableTransformKind::StochasticRow);
        break;
      case CallableTransformKind::CholeskyFactorCorr:
      case CallableTransformKind::CorrMatrix:
      case CallableTransformKind::CovMatrix: {
        if (leaf != ViewKind::Vector) fail(e.name + ": expected vector", e.raw);
        const int64_t k =
            actuals.at(1).require_constant_int("matrix dimension");
        out_leaf = ViewKind::Matrix;
        out_rows = out_cols = k;
        break;
      }
      case CallableTransformKind::CholeskyFactorCov:
        if (leaf != ViewKind::Vector) fail(e.name + ": expected vector", e.raw);
        out_leaf = ViewKind::Matrix;
        out_rows = actuals.at(1).require_constant_int("matrix rows");
        out_cols = actuals.at(2).require_constant_int("matrix columns");
        break;
      default:
        fail(e.name + ": invalid structured transform", e.raw);
    }
    if (out_rows < 0 || out_cols < 0)
      fail(e.name + ": negative result dimension", e.raw);
    const int64_t inner_raw =
        leaf == ViewKind::Matrix
            ? checked_product({raw_rows, raw_cols}, e.name + " raw matrix")
            : dims.back();
    const int64_t inner_con =
        out_leaf == ViewKind::Matrix
            ? checked_product({out_rows, out_cols}, e.name)
            : out_rows;
    const int64_t out_len = checked_product({batch, inner_con}, e.name);
    SlotInfo si;
    if (outer_rank != 0) {
      outer.push_back(out_rows);
      if (out_leaf == ViewKind::Matrix) outer.push_back(out_cols);
      si = array_view(std::move(outer), out_leaf, raw.si.param_free);
    } else if (out_leaf == ViewKind::Matrix) {
      si = matrix_view(out_rows, out_cols, raw.si.param_free);
    } else {
      si = view_of(out_leaf == ViewKind::RowVector ? "URowVector" : "UVector");
      si.param_free = raw.si.param_free;
    }
    std::vector<int> idata = {
        checked_immediate(batch, e.name + " batch"),
        checked_immediate(inner_raw, e.name + " raw leaf"),
        checked_immediate(out_leaf == ViewKind::Matrix ? out_rows : inner_con,
                          e.name + " result rows")};
    if (out_leaf == ViewKind::Matrix)
      idata.push_back(checked_immediate(out_cols, e.name + " result columns"));
    const int jac = add_slot(1, false);
    Val v = emit_raw(opcode, {raw.slot}, out_len, si, std::move(idata), jac,
                     raw.autodiff);
    v.layout = owning_layout(si);
    if (tr.direction == TransformDirection::Jacobian && !in_write_array)
      target_terms.push_back(jac);
    return v;
  }

  std::vector<Val> a;
  a.reserve(actuals.size());
  for (size_t i = 0; i < actuals.size(); ++i)
    a.push_back(actuals.at(i).value());
  const int64_t n = g.slots[a[0].slot].len;
  SlotInfo si = a[0].si;
  std::vector<int> ins;
  bool autodiff = false;
  for (const Val& v : a) {
    const int64_t len = g.slots[v.slot].len;
    if (len != 1 && len != n)
      fail(e.name + ": bound is neither one value nor one per element", e.raw);
    si.param_free = si.param_free && v.si.param_free;
    autodiff = autodiff || v.autodiff;
    ins.push_back(v.slot);
  }

  if (tr.direction == TransformDirection::Unconstrain)
    return free_transform(tr.opcode, a, si, n);
  // The declaration kernels, unchanged: they carry the arithmetic that was
  // measured against stan-math's rev overloads, which composing exp,
  // inv_logit, and fma out of the elementwise ops would not reproduce.
  // They always write the jacobian, so `_constrain` allocates the output
  // and simply leaves it unrooted -- no term reaches the target, and its
  // adjoint stays zero, which is exactly the no-lp overload's gradient.
  const int jac = add_slot(1, /*is_param=*/false);
  Val v = emit_raw(tr.opcode, ins, n, si, {}, jac, autodiff);
  v.layout = owning_layout(si);
  if (tr.direction == TransformDirection::Jacobian && !in_write_array)
    target_terms.push_back(jac);
  return v;
}
// The inverse transforms. stan-math has no rev overloads for these: its
// `log(y - lb)` is ordinary var arithmetic, which is what these
// elementwise ops emit, so the composition is the reference rather than an
// approximation of it, and no new kernel is needed.
Lowering::Val Lowering::free_transform(uint16_t opcode,
                                       const std::vector<Val>& a, SlotInfo si,
                                       int64_t n) {
  // An intermediate keeps the argument's logical view only when it is as
  // wide as the argument; `ub - lb` on two scalars is one value.
  const auto elt = [&](uint16_t op, const Val& x, const Val& y) {
    const int64_t w = std::max(g.slots[x.slot].len, g.slots[y.slot].len);
    return with_layout(emit_value(op, {x, y}, w, w == n ? si : SlotInfo{}),
                       elementwise_layout({x, y}));
  };
  const auto un = [&](uint16_t op, const Val& x) {
    return with_layout(emit_value(op, {x}, g.slots[x.slot].len, x.si),
                       elementwise_layout({x}));
  };
  switch (opcode) {
    case OP_CONSTRAIN_LOWER:  // lb_free: log(y - lb)
      return un(OP_LOGV, elt(OP_SUB, a[0], a[1]));
    case OP_CONSTRAIN_UPPER:  // ub_free: log(ub - y)
      return un(OP_LOGV, elt(OP_SUB, a[1], a[0]));
    case OP_CONSTRAIN_LU:  // lub_free: logit((y - lb) / (ub - lb))
      return un(OP_LOGIT,
                elt(OP_DIV, elt(OP_SUB, a[0], a[1]), elt(OP_SUB, a[2], a[1])));
    default:  // offset_multiplier_free: (y - mu) / sigma
      return elt(OP_DIV, elt(OP_SUB, a[0], a[1]), a[2]);
  }
}
// Inline a user-defined function at its call site: arguments are lowered
// in the caller's scope, bound under the parameter names in a shadowed
// scope, and the body lowers like any other statements (loops unroll,
// data-only conditions resolve). Return throws the result value out.
Lowering::Val Lowering::lower_call_udf(
    const mir::Expr& e, const std::function<void()>& before_body) {
  auto it = fun_defs.find(e.name);
  if (it == fun_defs.end()) fail("unknown function " + e.name, e.raw);
  const mir::FunDef& f = *it->second;
  CallArguments actuals(*this, e);
  actuals.require_arity(f.arg_names.size());
  struct Binding {
    bool is_int = false;
    long iv = 0;
    Val v{-1, false, {}};
    std::optional<DataMap::Entry> data;
    bool formal_data_only = false;
  };
  std::vector<Binding> binds(actuals.size());
  for (size_t i = 0; i < actuals.size(); ++i) {
    LoweredArgument& actual = actuals.at(i);
    const mir::Expr& a = actual.expr();
    binds[i].formal_data_only =
        i < f.arg_data_only.size() && f.arg_data_only[i];
    if (!region_current && a.data_only && a.type_ == "UInt") {
      try {
        binds[i].iv = actual.require_constant_int("integer argument");
        binds[i].is_int = true;
      } catch (const CompileError&) {
        if (in_write_array || !needs_runtime_value(a)) throw;
      }
    }
    if (!binds[i].is_int) {
      binds[i].v = actual.value();
      if (const DataMap::Entry* en = actual.observation()) binds[i].data = *en;
    }
    if (!in_write_array && binds[i].formal_data_only && !binds[i].is_int &&
        (binds[i].v.autodiff || !binds[i].v.si.param_free))
      fail(e.name + ": data-only argument depends on a parameter", e.raw);
  }
  // Higher-order calls may validate after evaluating all actual arguments
  // but before entering the user body (reduce_sum's grainsize check).
  if (before_body) before_body();
  if (++udf_depth > 64) {
    --udf_depth;
    fail("UDF recursion too deep in " + e.name);
  }
  auto sc_saved = std::move(scope);
  auto region_cells_saved = region_cells;
  const int region_depth_saved = region_control_depth;
  if (region_current) {
    region_cells.clear();
    region_control_depth = 0;
  }
  auto formal_autodiff_saved = std::move(udf_formal_autodiff);
  auto ie_saved = std::move(int_env);
  auto decls_saved = std::move(decls);
  auto il_saved = std::move(int_locals);
  auto env_saved = std::move(td.env());
  scope.clear();
  udf_formal_autodiff.clear();
  int_env.clear();
  decls.clear();
  int_locals.clear();
  td.env().clear();
  Val ret{-1, false, {}};
  bool returned = false;
  const bool propto_saved = propto_ctx;
  const bool autodiff_saved = udf_autodiff_ctx;
  const bool known_static_saved = write_array_known_static;
  write_array_known_static = false;
  propto_ctx = propto_ctx && e.fn_propto;
  udf_autodiff_ctx = false;
  for (size_t i = 0; i < binds.size(); ++i)
    if (!binds[i].is_int && f.arg_views[i].leaf != mir::UnsizedLeaf::Int)
      udf_autodiff_ctx = udf_autodiff_ctx || binds[i].v.autodiff;
  auto restore = [&] {
    propto_ctx = propto_saved;
    udf_autodiff_ctx = autodiff_saved;
    write_array_known_static = known_static_saved;
    scope = std::move(sc_saved);
    region_cells = std::move(region_cells_saved);
    region_control_depth = region_depth_saved;
    udf_formal_autodiff = std::move(formal_autodiff_saved);
    int_env = std::move(ie_saved);
    decls = std::move(decls_saved);
    int_locals = std::move(il_saved);
    td.env() = std::move(env_saved);
    --udf_depth;
  };
  try {
    for (size_t i = 0; i < binds.size(); ++i) {
      const std::string& name = f.arg_names[i];
      // Bind whenever the argument's value is computable at compile time,
      // not just when the MIR flags it DataOnly: a function may take a data
      // array without the `data` qualifier, and its body still asks for
      // shapes and sizes. Parameter expressions simply fail to evaluate.
      if (binds[i].data) {
        DataMap::Entry en = *binds[i].data;
        td.env()[name] = std::move(en);
      }
      if (binds[i].is_int) {
        int_env[name] = binds[i].iv;
      } else {
        scope[name] = binds[i].v;
        udf_formal_autodiff[name] = binds[i].v.autodiff;
        decls[name] = DeclView{g.slots[binds[i].v.slot].len,
                               binds[i].v.autodiff, binds[i].v.si};
      }
    }
    // CmdStan passes the CALLER's propto__ value into a user density.
    for (const auto& st : f.body) lower_stmt(st);
  } catch (LpReturn& r) {
    ret = r.v;
    returned = true;
  } catch (...) {
    restore();
    throw;
  }
  ret.autodiff = e.unsized.leaf != mir::UnsizedLeaf::Int && udf_autodiff_ctx;
  restore();
  if (!returned) fail(e.name + ": no return value on the executed path");
  ret.layout = owning_layout(ret.si);
  return ret;
}
Lowering::Val Lowering::lower_multi_normal_rng(const mir::Expr& e,
                                               CallArguments& actuals) {
  if (!in_write_array)
    fail("multi_normal_rng is supported only in generated quantities", e.raw);
  if (e.args.size() != 2 || e.type_ != "UVector" ||
      e.unsized.leaf != mir::UnsizedLeaf::Vector || e.unsized.depth != 0)
    fail("multi_normal_rng: expected one vector result", e.raw);
  const mir::Expr& location_expr = actuals.at(0).expr();
  const mir::Expr& covariance_expr = actuals.at(1).expr();
  if (location_expr.type_ != "UVector" ||
      location_expr.unsized.leaf != mir::UnsizedLeaf::Vector ||
      location_expr.unsized.depth != 0)
    fail("multi_normal_rng: expected one vector location", e.raw);
  if (covariance_expr.type_ != "UMatrix" ||
      covariance_expr.unsized.leaf != mir::UnsizedLeaf::Matrix ||
      covariance_expr.unsized.depth != 0)
    fail("multi_normal_rng: expected one covariance matrix", e.raw);

  Val location = actuals.at(0).value();
  Val covariance = actuals.at(1).value();
  if (!is_vector(location.si))
    fail("multi_normal_rng: location is not a logical vector", e.raw);
  if (!is_matrix(covariance.si))
    fail("multi_normal_rng: covariance has no known matrix shape", e.raw);
  const int64_t k = g.slots[location.slot].len;
  if (k > std::numeric_limits<int>::max() || covariance.si.rows != k ||
      covariance.si.cols != k ||
      g.slots[covariance.slot].len != checked_product({k, k}, "covariance"))
    fail("multi_normal_rng: covariance shape must match the location", e.raw);

  Val draw = with_layout(emit_value(OP_RNG, {location, covariance}, k,
                                    view_of(e.type_), {static_cast<int>(k)}),
                         ExpressionLayout::direct());
  g.ops.back().variant = kMultiNormalRngVariant;
  draw.si.param_free = false;
  draw.autodiff = false;
  return draw;
}
Lowering::Val Lowering::lower_dirichlet_rng(const mir::Expr& e,
                                            CallArguments& actuals) {
  if (!in_write_array)
    fail("dirichlet_rng is supported only in generated quantities", e.raw);
  if (e.args.size() != 1 || e.type_ != "UVector" ||
      e.unsized.leaf != mir::UnsizedLeaf::Vector || e.unsized.depth != 0)
    fail("dirichlet_rng: expected one vector result", e.raw);
  const mir::Expr& alpha_expr = actuals.at(0).expr();
  if (alpha_expr.type_ != "UVector" ||
      alpha_expr.unsized.leaf != mir::UnsizedLeaf::Vector ||
      alpha_expr.unsized.depth != 0)
    fail("dirichlet_rng: expected one concentration vector", e.raw);

  Val alpha = actuals.at(0).value();
  if (!is_vector(alpha.si))
    fail("dirichlet_rng: argument is not a logical vector", e.raw);
  const int64_t k = g.slots[alpha.slot].len;
  if (k <= 0 || k > std::numeric_limits<int>::max())
    fail("dirichlet_rng: concentration vector must have a positive length",
         e.raw);

  Val draw = with_layout(emit_value(OP_RNG, {alpha}, k, view_of(e.type_)),
                         ExpressionLayout::direct());
  g.ops.back().variant = kDirichletRngVariant;
  draw.si.param_free = false;
  draw.autodiff = false;
  return draw;
}
Lowering::Val Lowering::lower_regular_unary(uint16_t opcode,
                                            const std::string& type_,
                                            const std::string& name,
                                            const std::string& raw, Val a) {
  SlotInfo si = a.si;
  // Shape-preserving unaries keep rows/cols (softmax/cumulative_sum
  // are vector-only, so they never carry one).
  if (opcode != OP_SOFTMAX && opcode != OP_CUMSUM) {
    if (type_ == "UMatrix" && !is_matrix(si))
      fail(name + ": matrix result has unknown logical extents", raw);
    stamp_kind(&si, type_);
  } else {
    si = view_of(type_);
  }
  si.param_free = a.si.param_free;
  const bool packet_supported =
      opcode != OP_SOFTMAX && opcode != OP_LOG_SOFTMAX && opcode != OP_CUMSUM;
  // These functions return freshly allocated Eigen containers. Their
  // result starts at lane zero independently of the input's provenance;
  // the other unary operations are elementwise evaluator expressions.
  const ExpressionLayout layout =
      packet_supported ? elementwise_layout({a}) : owning_layout(si);
  return with_layout(emit_value(opcode, {a}, g.slots[a.slot].len, si), layout);
}
Lowering::Val Lowering::lower_categorical_rng(const mir::Expr& e,
                                              CallArguments& actuals) {
  const bool is_logit = e.name == "categorical_logit_rng";
  if (!in_write_array)
    fail(e.name + " is supported only in generated quantities", e.raw);
  if (e.args.size() != 1 || e.type_ != "UInt" ||
      e.unsized.leaf != mir::UnsizedLeaf::Int || e.unsized.depth != 0)
    fail(e.name + ": expected one scalar int result", e.raw);
  const mir::Expr& probabilities = actuals.at(0).expr();
  if (probabilities.type_ != "UVector" || probabilities.unsized.depth != 0 ||
      probabilities.unsized.leaf != mir::UnsizedLeaf::Vector)
    fail(e.name + ": expected one " + (is_logit ? "logit" : "probability") +
             "-vector argument",
         e.raw);

  Val argument = actuals.at(0).value();
  if (!is_vector(argument.si))
    fail(e.name + ": argument is not a logical vector", e.raw);
  if (is_logit)
    argument =
        lower_regular_unary(OP_SOFTMAX, "UVector", "softmax", e.raw, argument);
  Val draw = with_layout(emit_value(OP_RNG, {argument}, 1, view_of(e.type_)),
                         ExpressionLayout::scalar());
  g.ops.back().variant = kCategoricalRngVariant;
  // A successful call returns a Stan int, but deliberately do not widen
  // this tranche into runtime-sum range reasoning. Survey only needs the
  // scalar value; dynamic integer control and indexing still fail closed.
  draw.si.param_free = false;
  draw.autodiff = false;
  set_int_initialized(draw);
  return draw;
}
Lowering::Val Lowering::lower_scalar_rng(const mir::Expr& e,
                                         CallArguments& actuals,
                                         ScalarRng family) {
  if (!in_write_array)
    fail(e.name + " is supported only in generated quantities", e.raw);
  const size_t arity = scalar_rng_arity(family);
  if (actuals.size() != arity || e.unsized.depth != 0)
    fail(e.name + ": expected scalar result and " + std::to_string(arity) +
             " scalar argument(s)",
         e.raw);
  const mir::UnsizedLeaf result_leaf = scalar_rng_is_int(family)
                                           ? mir::UnsizedLeaf::Int
                                           : mir::UnsizedLeaf::Real;
  if (e.unsized.leaf != result_leaf)
    fail(e.name + ": result type does not match RNG family", e.raw);
  // Unlike the other scalar families, binomial's (and beta_binomial's)
  // first argument is a population count. Valid stanc MIR always marks it
  // UInt; fail closed on malformed hand-authored MIR rather than silently
  // truncating a real in the runtime helper's graph-storage conversion.
  if ((family == ScalarRng::Binomial || family == ScalarRng::BetaBinomial) &&
      actuals.at(0).expr().unsized.leaf != mir::UnsizedLeaf::Int)
    fail(e.name + ": first argument must be int", e.raw);
  std::vector<Val> args;
  args.reserve(arity);
  for (size_t i = 0; i < actuals.size(); ++i) {
    const mir::Expr& arg = actuals.at(i).expr();
    if (arg.unsized.depth != 0)
      fail(e.name + ": container arguments stay on WaInterp", e.raw);
    args.push_back(actuals.at(i).value());
    if (!is_scalar(args.back()))
      fail(e.name + ": container arguments stay on WaInterp", e.raw);
  }
  Val draw = with_layout(
      arity == 1   ? emit_value(OP_RNG, {args[0]}, 1, view_of(e.type_))
      : arity == 2 ? emit_value(OP_RNG, {args[0], args[1]}, 1, view_of(e.type_))
                   : emit_value(OP_RNG, {args[0], args[1], args[2]}, 1,
                                view_of(e.type_)),
      ExpressionLayout::scalar());
  g.ops.back().variant = static_cast<uint8_t>(family);
  // An effect is never a graph constant, even when all distribution
  // parameters are. This also keeps downstream compile-time demands from
  // mistaking a draw for data.
  draw.si.param_free = false;
  draw.autodiff = false;
  if (scalar_rng_is_int(family)) set_int_initialized(draw);
  if (family == ScalarRng::Bernoulli) set_int_range(draw, 0, 1);
  return draw;
}
Lowering::Val Lowering::lower_append_array(const mir::Expr& e,
                                           CallArguments& actuals) {
  actuals.require_arity(2);
  Val a = actuals.at(0).value();
  Val b = actuals.at(1).value();
  if (!is_array(a.si) || !is_array(b.si))
    fail("append_array: arguments must be arrays", e.raw);
  const ArrayShape& ash = array_shape(a.si);
  const ArrayShape& bsh = array_shape(b.si);
  if (ash.dims.empty() || bsh.dims.empty() ||
      ash.dims.size() != bsh.dims.size() || ash.leaf != bsh.leaf)
    fail("append_array: element shapes must match", e.raw);
  const int64_t a_outer = ash.dims[0], b_outer = bsh.dims[0];
  // stan-math checks element geometry only when both sides contain an
  // element. An empty side contributes no value whose shape could
  // disagree, and the nonempty side supplies the result's suffix.
  if (a_outer != 0 && b_outer != 0 &&
      !std::equal(ash.dims.begin() + 1, ash.dims.end(), bsh.dims.begin() + 1,
                  bsh.dims.end()))
    fail("append_array: element shapes must match", e.raw);
  if (a_outer > std::numeric_limits<int64_t>::max() - b_outer)
    fail("append_array: outer extent overflows", e.raw);
  const int64_t alen = g.slots[a.slot].len;
  const int64_t blen = g.slots[b.slot].len;
  if (alen > std::numeric_limits<int64_t>::max() - blen)
    fail("append_array: storage length overflows", e.raw);
  std::vector<int64_t> dims =
      a_outer == 0 && b_outer != 0 ? bsh.dims : ash.dims;
  dims[0] = a_outer + b_outer;
  const int64_t suffix_count =
      checked_product(std::vector<int64_t>(dims.begin() + 1, dims.end()),
                      "append_array element shape");
  SlotInfo si = array_view(std::move(dims), ash.leaf);
  Val joined = with_layout(emit_value(OP_CONCAT2, {a, b}, alen + blen, si),
                           owning_layout(si));

  // Preserve exact data values for compile-time integer loops and index
  // expressions. Integer arrays are always data-only in Stan, but this
  // also keeps real data arrays available to the ordinary const folder.
  const DataMap::Entry* ao = observation(a);
  const DataMap::Entry* bo = observation(b);
  if (ao && bo && ao->is_int == bo->is_int) {
    DataMap::Entry en;
    en.is_int = ao->is_int;
    en.r.reserve((size_t)(alen + blen));
    // DataMap is first-index-fast, unlike the graph's outer-major array
    // storage. Concatenation along dimension zero therefore interleaves
    // the two outer-axis blocks once for every suffix coordinate.
    const int64_t observation_lanes = a_outer + b_outer == 0 ? 0 : suffix_count;
    for (int64_t lane = 0; lane < observation_lanes; ++lane) {
      const auto ab = ao->r.begin() + lane * a_outer;
      const auto bb = bo->r.begin() + lane * b_outer;
      en.r.insert(en.r.end(), ab, ab + a_outer);
      en.r.insert(en.r.end(), bb, bb + b_outer);
    }
    if (en.is_int) {
      en.i.reserve((size_t)(alen + blen));
      for (int64_t lane = 0; lane < observation_lanes; ++lane) {
        const auto ab = ao->i.begin() + lane * a_outer;
        const auto bb = bo->i.begin() + lane * b_outer;
        en.i.insert(en.i.end(), ab, ab + a_outer);
        en.i.insert(en.i.end(), bb, bb + b_outer);
      }
      set_int_initialized(joined);
      if (!en.i.empty()) {
        const auto bounds = std::minmax_element(en.i.begin(), en.i.end());
        set_int_range(joined, *bounds.first, *bounds.second);
      }
    }
    observe(joined, std::move(en));
  }
  return joined;
}
Lowering::Val Lowering::lower_funapp(const mir::Expr& e) {
  if (const auto intrinsic = mir::stateful_intrinsic_kind(e)) {
    switch (*intrinsic) {
      case mir::StatefulIntrinsicKind::Target: {
        if (in_write_array)
          fail("target() is unavailable in write_array", e.raw);
        SlotInfo si;
        si.param_free = target_terms.empty() && jac_slots.empty();
        return {current_target_slot(), scalar_autodiff(), si};
      }
    }
  }
  if (const auto value = mir::nullary_constant(e)) return constant(*value);
  if (e.fn_lib == mir::Expr::Lib::UserDefined) {
    if (!region_current)
      if (auto v = fold_const(e)) return *v;
    return lower_call_udf(e);
  }
  if (e.fn_lib == mir::Expr::Lib::Internal &&
      (e.name == "FnMakeArray" || e.name == "FnMakeRowVec")) {
    // Array/row-vector literals are structural values: the interpreter's
    // numeric result does not retain enough information to reconstruct an
    // array of containers, so lower the pieces and attach the view here.
    std::vector<Val> parts;
    for (const auto& a : e.args) parts.push_back(lower_expr(a));
    Val acc;
    if (parts.empty()) {
      SlotInfo empty;
      empty.param_free = true;
      acc = Val{add_slot(0, false), false, empty, owning_layout(empty)};
      out.fills.emplace_back(acc.slot, std::vector<double>{});
    } else {
      acc = parts[0];
      for (size_t i = 1; i < parts.size(); ++i) {
        const int64_t len = g.slots[acc.slot].len + g.slots[parts[i].slot].len;
        acc = emit_value(OP_CONCAT2, {acc, parts[i]}, len);
      }
    }
    if (e.name == "FnMakeRowVec") {
      if (e.type_ == "UMatrix") {
        const int64_t rows = (int64_t)parts.size();
        const int64_t cols = parts.empty() ? 0 : g.slots[parts[0].slot].len;
        for (const Val& p : parts)
          if (!is_row_vector(p.si) || g.slots[p.slot].len != cols)
            fail("matrix literal rows have different logical views", e.raw);
        std::vector<int> gather;
        gather.reserve((size_t)(rows * cols));
        for (int64_t j = 0; j < cols; ++j)
          for (int64_t i = 0; i < rows; ++i)
            gather.push_back((int)(i * cols + j));
        acc = emit_value(OP_GATHER, {acc}, rows * cols, matrix_view(rows, cols),
                         gather);
      } else {
        acc.si.kind = ViewKind::RowVector;
        acc.si.shape = 0;
      }
    } else {
      if (parts.empty() && e.unsized.depth != 1)
        fail("empty nested array literal has unknown inner shape", e.raw);
      ViewKind leaf = leaf_kind(e.unsized.leaf);
      std::vector<int64_t> dims{(int64_t)parts.size()};
      if (!parts.empty()) {
        const Val& first = parts.front();
        for (const Val& p : parts)
          if (!same_view(first.si, g.slots[first.slot].len, p.si,
                         g.slots[p.slot].len))
            fail("array literal elements have different logical views", e.raw);
        if (is_array(first.si)) {
          const ArrayShape& child = array_shape(first.si);
          dims.insert(dims.end(), child.dims.begin(), child.dims.end());
          leaf = child.leaf;
        } else if (is_matrix(first.si)) {
          dims.push_back(first.si.rows);
          dims.push_back(first.si.cols);
          leaf = ViewKind::Matrix;
        } else if (is_vector(first.si) || is_row_vector(first.si)) {
          dims.push_back(g.slots[first.slot].len);
          leaf = first.si.kind;
        } else {
          leaf = ViewKind::Flat;
        }
      }
      acc.si = array_view(std::move(dims), leaf, acc.si.param_free);
    }
    if (acc.si.param_free) {
      // MirInterp's scalar-vs-container probe reads child[0], which is not
      // defined for an explicit array of zero-width containers. The view
      // already proves the complete native shape, and an empty value has
      // no bytes to reorder, so record that observation without executing
      // the structurally lossy interpreter path.
      if (g.slots[acc.slot].len == 0) {
        DataMap::Entry en;
        en.is_int = e.unsized.leaf == mir::UnsizedLeaf::Int;
        observe(acc, std::move(en));
      } else if (auto evaluated = try_eval_pure(e)) {
        observe(acc, std::move(*evaluated));
      }
    }
    acc.layout = owning_layout(acc.si);
    return acc;
  }
  if (e.fn_lib != mir::Expr::Lib::StanLib) {
    if (auto v = fold_const(e)) return *v;
    fail("unsupported function kind for " + e.name, e.raw);
  }
  // Construct the lazy argument state exactly once. Resolver and handlers
  // inspect source metadata freely; values are still acquired only when the
  // selected handler asks for them. Nullary constants above need no call
  // state at all.
  CallArguments actuals(*this, e);
  if (e.name == "dims") return lower_dims(e, actuals);
  const BuiltinDispatch dispatch = resolve_builtin(e);

  // One family decision replaces the former chain of optional handlers.
  // A handler can still decline a malformed/unsupported overload so the
  // common constant fallback and diagnostic below remain unchanged.
  switch (dispatch.family) {
    case BuiltinFamily::MapRect:
      if (auto v = lower_empty_map_rect(e, actuals)) return *v;
      return lower_program_expression(e);
    case BuiltinFamily::ReduceSum:
      return lower_reduce_sum(e, actuals);
    case BuiltinFamily::MultiNormalRng:
      return lower_multi_normal_rng(e, actuals);
    case BuiltinFamily::DirichletRng:
      return lower_dirichlet_rng(e, actuals);
    case BuiltinFamily::CategoricalRng:
      return lower_categorical_rng(e, actuals);
    case BuiltinFamily::ScalarRng:
      assert(dispatch.scalar_rng.has_value());
      return lower_scalar_rng(e, actuals, *dispatch.scalar_rng);
    case BuiltinFamily::Density:
      if (auto v = lower_density_fn(e, actuals, dispatch.density)) return *v;
      break;
    case BuiltinFamily::CallableTransform:
      if (auto v = lower_callable_transform(e, actuals)) return *v;
      break;
    case BuiltinFamily::Elementwise:
      if (auto v = lower_eltwise_fn(e, actuals, dispatch.builtin)) return *v;
      break;
    case BuiltinFamily::Matrix:
      if (auto v = lower_matrix_fn(e, actuals)) return *v;
      break;
    case BuiltinFamily::Algebra:
      if (const auto call = mir::algebra_call(e.name); call && !call->legacy)
        return lower_program_expression(e);
      return lower_algebra_fn(e, actuals);
    case BuiltinFamily::Quadrature:
      return lower_quadrature_fn(e, actuals);
    case BuiltinFamily::Ode:
      if (const auto call = mir::ode_call(e.name);
          call && call->method == mir::OdeMethod::Adjoint)
        return lower_program_expression(e);
      if (auto v = lower_ode_fn(e, actuals)) return *v;
      break;
    case BuiltinFamily::Dae:
      return lower_program_expression(e);
    case BuiltinFamily::AppendArray:
      if (e.args.size() == 2) return lower_append_array(e, actuals);
      break;
    case BuiltinFamily::ShapeQuery:
      break;
  }
  // A shape query in a REAL-valued expression. eval_int already answers
  // rows/cols/size from the slot or the data map, but only where an
  // integer was expected; brms's mo() helper writes
  // `rows(scale) * sum(scale[1:i])`, where the same call sits in the
  // middle of arithmetic and reached the failure below instead.
  if (dispatch.family == BuiltinFamily::ShapeQuery && e.args.size() == 1) {
    try {
      return constant((double)eval_int(e));
    } catch (const CompileError&) {
    }
  }
  if (auto v = fold_const(e)) return *v;
  fail("unsupported function " + e.name);
}
// Density calls: the registry-planned kernels.
std::optional<Lowering::Val> Lowering::lower_density_fn(
    const mir::Expr& e, CallArguments& actuals, const DensitySpec* selected) {
  // Leading integer arguments become idata; the rest become real slots.
  // Layouts: one integer group = raw values; two groups =
  // [len, vals..., len, vals...]; glm = [y..., rows, cols].
  const stanli::DensitySpec* density = selected;
  if (density != nullptr) {
    const stanli::DensitySpec& spec = *density;
    if ((int)actuals.size() != spec.arity) {
      // The compact cases below used to decline a bad arity and let the
      // common unsupported-function diagnostic report it.
      if (spec.shape != stanli::DensityShape::Plain || spec.activity_mask >= 0)
        return std::nullopt;
      actuals.require_arity((size_t)spec.arity);
    }
    std::vector<int> ins;
    SlotInfo result_si{0, 0, true};
    bool result_autodiff = false;
    std::vector<stanli::DensityCallArgument> plan_arguments;
    plan_arguments.reserve(actuals.size());
    if (spec.shape == stanli::DensityShape::Categorical) {
      const Val& outcome = actuals.at(0).value();
      const Val& arg = actuals.at(1).value();
      const bool scalar = e.args[0].unsized.depth == 0;
      if (e.args[0].unsized.leaf != mir::UnsizedLeaf::Int ||
          e.args[0].unsized.depth > 1 ||
          e.args[1].unsized.leaf != mir::UnsizedLeaf::Vector ||
          e.args[1].unsized.depth != 0)
        fail(e.name + ": expected int or array[] int and vector", e.raw);
      const bool array_outcome =
          is_array(outcome.si) &&
          array_shape(outcome.si).leaf == ViewKind::Flat &&
          array_shape(outcome.si).dims.size() == 1;
      if ((scalar && !is_scalar(outcome)) || (!scalar && !array_outcome) ||
          !is_vector(arg.si))
        fail(e.name + ": MIR type does not match lowered values", e.raw);
      if (!e.args[0].data_only || !outcome.si.param_free ||
          (udf_depth == 0 &&
           arg.autodiff != (!in_write_array && !e.args[1].data_only)))
        fail(e.name + ": MIR adlevel contradicts lowered dependencies", e.raw);
    }

    try {
      for (size_t i = 0; i < actuals.size(); ++i) {
        LoweredArgument& actual = actuals.at(i);
        const mir::Expr& source = actual.expr();
        stanli::DensityCallArgument argument;
        if (spec.evaluation == stanli::DensityEvaluationPolicy::AllInteger ||
            i < static_cast<size_t>(spec.integer_args)) {
          argument = stanli::integer_density_argument(int_arg_values(actual),
                                                      source.unsized.depth == 0,
                                                      source.data_only);
        } else {
          argument.scalar = source.unsized.depth == 0;
          argument.data_only = source.data_only;
          const Val value = actual.value();
          ins.push_back(value.slot);
          result_si.param_free = result_si.param_free && value.si.param_free;
          result_autodiff = result_autodiff || value.autodiff;
          argument.active = spec.shape == stanli::DensityShape::Categorical
                                ? value.autodiff
                                : !source.data_only;
          argument.shape = builtin_argument_shape(source, value);
        }
        plan_arguments.push_back(std::move(argument));
      }
      const stanli::DensityCallPlan plan =
          stanli::density_call_plan(spec, plan_arguments, propto(e));
      if (plan.empty_result) return constant(0.0);
      Val dv = emit_raw(spec.opcode, ins, 1, result_si, plan.idata, -1,
                        result_autodiff);
      dv.layout = ExpressionLayout::scalar();
      g.ops.back().variant = plan.variant;
      return dv;
    } catch (const std::exception& error) {
      fail(e.name + ": " + error.what(), e.raw);
    }
  }

  // gaussian_dlm_obs takes seven arguments and Op::in holds six, so it
  // cannot be lowered as one op at all. Raising the limit would add
  // bytes to every Op and every KernelCtx in every model for the sake
  // of one dynamic-linear-model density, so this refuses instead and
  // names the reason. See docs/coverage.md.
  if (e.name == "gaussian_dlm_obs_lpdf")
    fail("gaussian_dlm_obs takes 7 arguments and an op holds 6", e.raw);

  return std::nullopt;
}
// Elementwise math, reductions, and dot products.
std::optional<Lowering::Val> Lowering::lower_eltwise_fn(
    const mir::Expr& e, CallArguments& actuals, const BuiltinSpec* builtin) {
  // Once a generated int RNG has become a runtime scalar slot, named
  // integer division is no longer foldable. OP_DIV is real division and
  // would return 3.5 for divide(7, 2), while Stan truncates to 3. Refuse it
  // so the whole write_array stays on WaInterp until there is a native int
  // division op. The operator spelling is IntDivide__ and already refuses.
  if ((e.name == "divide" || e.name == "elt_divide") && e.type_ == "UInt")
    fail(e.name + ": runtime integer division stays on WaInterp", e.raw);
  // `A \ B` and `B / A` with a matrix divisor are linear solves, not
  // elementwise division: stanc spells them with the ordinary division
  // operators and lowers them to mdivide_left/mdivide_right. The divisor's
  // type is the whole discriminator -- a scalar divisor is elementwise, and
  // `./` is never a solve -- which is the rule the MIR interpreter applies,
  // kept identical here so a solve does not mean one thing in the model
  // block and another in transformed data.
  //
  // The named spellings share this lowering: they arrive with the same
  // argument order the operators use, divisor first for a left solve and
  // second for a right one. The _spd and _tri_low families get their own
  // opcodes rather than a flag because stan-math answers them by different
  // factorisations -- an LLT of a symmetric positive definite matrix, and
  // a triangular solve that never reads the upper triangle -- so they are
  // different results, not faster routes to the same one.
  const BuiltinSpec* solve =
      shaped_builtin_spec(e.name, e.args.size(), BuiltinShapePolicy::Solve);
  if (solve == nullptr && e.name == "Divide__" && e.args.size() == 2 &&
      e.args.at(1).type_ == "UMatrix")
    solve = shaped_builtin_spec("mdivide_right", 2, BuiltinShapePolicy::Solve);
  if (solve != nullptr) {
    actuals.require_arity(2);
    Val a = actuals.at(0).value();
    Val b = actuals.at(1).value();
    const bool left = solve->solve_left;
    const Val& divisor = left ? a : b;
    const Val& dividend = left ? b : a;
    // rows <= 0 is a matrix view whose shape the lowering never resolved;
    // the kernel would map n x n over the slot and read past it.
    if (!is_matrix(divisor.si) || divisor.si.rows <= 0)
      fail(e.name + ": divisor is not a square matrix of known size", e.raw);
    BuiltinSolveMap map;
    try {
      map = builtin_solve_map(*solve, builtin_argument_shape(e.args[0], a),
                              builtin_argument_shape(e.args[1], b));
    } catch (const std::invalid_argument& error) {
      fail(e.name + ": " + error.what(), e.raw);
    }
    const bool dm = is_matrix(dividend.si);
    Val v = emit_value(solve->opcode, {a, b}, map.order * map.columns,
                       dividend.si, {(int)map.order, (int)map.columns});
    // The kernel solves through the operand types CmdStan's generated code
    // would have used, because stan-math answers differently for each: bit
    // 0 says the result is var, bit 1 says the dividend is a vector rather
    // than a one-column matrix, and bits 2/3 retain the divisor/dividend
    // scalar types so mixed vv/vd/dv overloads do not collapse to vv.
    g.ops.back().variant =
        (uint8_t)((v.autodiff ? 1u : 0u) | (dm ? 0u : 2u) |
                  (divisor.autodiff ? 4u : 0u) | (dividend.autodiff ? 8u : 0u));
    return with_layout(v, owning_layout(dividend.si));
  }
  // multiply is the named spelling of `*`, including its linear algebra.
  // The shared product resolver classifies the scalar/matvec/GEMM/outer/
  // inner forms and validates the inner dimension; this lowering keeps the
  // kernel choices -- a data matrix against a true vector view stays on the
  // fused matvec kernel, everything else shaped goes through GEMM -- and
  // the established layout rules.
  if (e.name == "Times__" || (e.name == "multiply" && e.args.size() == 2)) {
    actuals.require_arity(2);
    Val a = actuals.at(0).value();
    Val b = actuals.at(1).value();
    BuiltinProductMap map;
    try {
      map = builtin_product_map(builtin_argument_shape(e.args[0], a),
                                builtin_argument_shape(e.args[1], b));
    } catch (const std::invalid_argument& error) {
      // Two orientation-free flat views (inlined UDF locals) keep the
      // elementwise broadcast they always lowered to.
      if (a.si.kind == ViewKind::Flat && b.si.kind == ViewKind::Flat) {
        const int64_t len = std::max(g.slots[a.slot].len, g.slots[b.slot].len);
        return with_layout(emit_value(OP_MUL, {a, b}, len),
                           elementwise_layout({a, b}));
      }
      fail(e.name + ": " + error.what(), e.raw);
    }
    switch (map.kind) {
      case BuiltinProductMap::Kind::ScalarScale: {
        const Val& shaped = is_scalar(a) ? b : a;
        SlotInfo si = shaped.si;
        si.param_free = a.si.param_free && b.si.param_free;
        return with_layout(
            emit_value(OP_MUL, {a, b}, g.slots[shaped.slot].len, si),
            elementwise_layout({a, b}));
      }
      case BuiltinProductMap::Kind::MatVec:
        if (a.si.param_free && b.si.kind == ViewKind::Vector)
          return with_layout(
              emit_value(OP_MATVEC, {a, b}, map.m, view_of("UVector"),
                         {(int)map.m, (int)map.k}),
              owning_layout(view_of("UVector")));
        return with_layout(
            emit_value(OP_GEMM, {a, b}, map.m, view_of("UVector"),
                       {(int)map.m, (int)map.k, 1}),
            owning_layout(view_of("UVector")));
      case BuiltinProductMap::Kind::Gemm:
      case BuiltinProductMap::Kind::Outer: {
        SlotInfo si = map.result.container == FunctionContainerKind::RowVector
                          ? view_of("URowVector")
                          : matrix_view(map.m, map.n);
        return with_layout(emit_value(OP_GEMM, {a, b}, map.m * map.n, si,
                                      {(int)map.m, (int)map.k, (int)map.n}),
                           owning_layout(si));
      }
      case BuiltinProductMap::Kind::Inner:
        return with_layout(emit_value(OP_DOT, {a, b}, 1),
                           ExpressionLayout::scalar());
    }
  }
  // fma from --O1 partial evaluation (`c + a*b`) or written explicitly:
  // fused (std::fma), elementwise with scalar broadcast on any argument.
  if (e.name == "fma" && e.args.size() == 3) {
    Val a = actuals.at(0).value();
    Val b = actuals.at(1).value();
    Val c = actuals.at(2).value();
    const int64_t la = g.slots[a.slot].len, lb = g.slots[b.slot].len,
                  lc = g.slots[c.slot].len;
    const int64_t n = std::max(la, std::max(lb, lc));
    for (int64_t l : {la, lb, lc})
      if (l != n && l != 1) fail("fma: incompatible lengths", e.raw);
    // The shape of whichever operand carries one, like the binaries.
    SlotInfo si = shape_of(a, b);
    if (is_scalar(a) && is_scalar(b)) si = shape_of(a, c);
    si.param_free = a.si.param_free && b.si.param_free && c.si.param_free;
    return with_layout(emit_value(OP_FMA, {a, b, c}, n, si),
                       elementwise_layout({a, b, c}));
  }
  // The generic lanes-in/lanes-out blocks below serve only elementwise
  // descriptors; reductions, paired reductions, and constructors have
  // their own policy dispatch further down.
  const bool elementwise_builtin =
      builtin != nullptr && builtin->shape == BuiltinShapePolicy::Elementwise;
  if (elementwise_builtin && builtin->arity == 2 &&
      builtin->arguments[0] == BuiltinArgumentKind::Real &&
      builtin->arguments[1] == BuiltinArgumentKind::Real) {
    actuals.require_arity(2);
    Val a = actuals.at(0).value();
    Val b = actuals.at(1).value();
    const std::vector<Val> values{a, b};
    const BuiltinLayout layout = resolved_builtin_layout(e, *builtin, values);
    SlotInfo si = values[layout.result_argument].si;
    si.param_free = a.si.param_free && b.si.param_free;
    return with_layout(emit_value(builtin->opcode, {a, b}, layout.lanes, si),
                       elementwise_layout({a, b}));
  }

  if (elementwise_builtin && builtin->arity == 2 &&
      (builtin_argument_is_integer(*builtin, 0) !=
       builtin_argument_is_integer(*builtin, 1))) {
    return lower_binary_int(*builtin, actuals);
  }

  if (elementwise_builtin && builtin->arity == 2 &&
      builtin_argument_is_integer(*builtin, 0) &&
      builtin_argument_is_integer(*builtin, 1)) {
    actuals.require_arity(2);
    Val a = actuals.at(0).value();
    Val b = actuals.at(1).value();
    const std::vector<Val> values{a, b};
    const BuiltinLayout layout = resolved_builtin_layout(e, *builtin, values);
    SlotInfo si = values[layout.result_argument].si;
    si.param_free = true;
    return with_layout(emit_value(builtin->opcode, {a, b}, layout.lanes, si),
                       elementwise_layout({a, b}));
  }

  // Registered constructors: data-only scalar arguments folded through the
  // shared Stan Math evaluator into one constant container. No kernel or
  // graph edge -- the result is param-free by construction, and extents,
  // spacing rules, and domain errors are CmdStan's own.
  if (const BuiltinSpec* ctor = shaped_builtin_spec(
          e.name, e.args.size(), BuiltinShapePolicy::Constructor)) {
    std::vector<double> ctor_args;
    ctor_args.reserve(e.args.size());
    for (const mir::Expr& argument : e.args) {
      const auto value = try_eval_pure(argument);
      if (!value || value->r.size() != 1)
        fail(e.name + ": argument must be a known data scalar", e.raw);
      ctor_args.push_back(value->r[0]);
    }
    ConstructorValue built;
    try {
      built = evaluate_constructor_builtin(*ctor, ctor_args);
    } catch (const std::domain_error&) {
      // Stan Math's own validation, the rejection CmdStan throws.
      throw;
    } catch (const std::invalid_argument& error) {
      fail(e.name + ": " + error.what(), e.raw);
    }
    const int64_t len = (int64_t)built.values.size();
    const int slot = add_slot(len, false);
    out.fills.emplace_back(slot, built.values);
    SlotInfo si;
    switch (ctor->constructor_container) {
      case FunctionContainerKind::Vector:
        si = view_of("UVector");
        break;
      case FunctionContainerKind::RowVector:
        si = view_of("URowVector");
        break;
      case FunctionContainerKind::Matrix:
        si = matrix_view(built.dimensions[0], built.dimensions[1], true);
        break;
      default:
        si = array_view(built.dimensions, ViewKind::Flat, true);
        break;
    }
    si.param_free = true;
    return Val{slot, false, si, owning_layout(si)};
  }

  // Registered slice/view selections: the shared resolver maps result
  // cells to source cells over the graph's outer-major array storage and
  // performs Stan Math's own index checks. Contiguous maps keep the
  // aliasing slice layout; strided and gathered maps land on the same
  // slice/gather kernels the named branches used.
  if (const BuiltinSpec* slice = shaped_builtin_spec(
          e.name, e.args.size(), BuiltinShapePolicy::SliceView);
      slice != nullptr && !builtin_slice_is_expansion(slice->slice)) {
    // Expansion descriptors fall through to their specialized graph
    // lowerings below and in the matrix/append families: dynamic rep
    // extents, the dedicated broadcast kernels, and append_array's
    // data-value observations have no cell-map equivalent.
    actuals.require_arity(slice->arity);
    Val a = actuals.at(0).value();
    std::vector<int64_t> indexes;
    indexes.reserve(e.args.size() - 1);
    const std::string what = e.name + " index";
    for (size_t k = 1; k < e.args.size(); ++k)
      indexes.push_back(actuals.at(k).require_constant_int(what.c_str()));
    BuiltinSliceMap map;
    try {
      map = builtin_slice_map(*slice, builtin_argument_shape(e.args[0], a),
                              indexes, SliceStorageOrder::OuterMajor);
    } catch (const std::invalid_argument& error) {
      // out_of_range and domain_error pass through: Stan Math's own index
      // validation, the rejection CmdStan throws.
      fail(e.name + ": " + error.what(), e.raw);
    }
    SlotInfo si;
    switch (map.result.container) {
      case FunctionContainerKind::Vector:
        si = view_of("UVector");
        break;
      case FunctionContainerKind::RowVector:
        si = view_of("URowVector");
        break;
      case FunctionContainerKind::Matrix:
        si = matrix_view(map.result.dimensions[0], map.result.dimensions[1]);
        break;
      case FunctionContainerKind::Array:
        si = array_view(map.result.dimensions,
                        function_view_kind(map.result.array_leaf));
        break;
      default:
        break;
    }
    si.param_free = a.si.param_free;
    switch (map.kind) {
      case BuiltinSliceMap::Kind::Contiguous:
        // A reshape's identity map is a relabelling of the same slot:
        // no instruction, as the named branches always lowered these. A
        // whole-container transpose spelling and to_matrix on an
        // already-matching matrix keep the source's layout provenance;
        // the other reshapes historically restarted at lane zero.
        if (builtin_slice_is_reshape(slice->slice)) {
          const bool preserves_view = slice->slice == BuiltinSlice::Transpose ||
                                      (slice->slice == BuiltinSlice::ToMatrix &&
                                       slice->arity == 1 && is_matrix(a.si));
          return Val{a.slot, a.autodiff, si,
                     preserves_view ? a.layout : owning_layout(si)};
        }
        return with_layout(
            emit_value(OP_SLICE, {a}, map.count, si,
                       {checked_immediate(map.offset, e.name + " offset")}),
            contiguous_layout(a, map.offset, e.name));
      case BuiltinSliceMap::Kind::Strided:
        return with_layout(
            emit_value(OP_SLICE_STRIDED, {a}, map.count, si,
                       {checked_immediate(map.offset, e.name + " offset"),
                        checked_immediate(map.stride, e.name + " stride")}),
            ExpressionLayout::scalar());
      case BuiltinSliceMap::Kind::Transpose: {
        // The native transpose kernel; idata names the source's rows and
        // columns, which are the result's columns and rows.
        const int source_rows =
            checked_immediate(map.result.dimensions[1], e.name + " rows");
        const int source_cols =
            checked_immediate(map.result.dimensions[0], e.name + " cols");
        const ExpressionLayout layout = slice->slice == BuiltinSlice::Transpose
                                            ? ExpressionLayout::unknown()
                                            : owning_layout(si);
        return with_layout(emit_value(OP_TRANSPOSE, {a}, map.count, si,
                                      {source_rows, source_cols}),
                           layout);
      }
      case BuiltinSliceMap::Kind::Gather:
        break;
    }
    std::vector<int> gather;
    gather.reserve(map.gather.size());
    for (const int64_t source : map.gather)
      gather.push_back(checked_immediate(source, e.name + " gather offset"));
    return with_layout(emit_value(OP_GATHER, {a}, map.count, si, gather),
                       ExpressionLayout::scalar());
  }

  // Registered paired reductions: two equal-length containers (or one,
  // paired with itself) folded to one scalar through the dot kernel.
  // squared_distance subtracts first: two kernels that already carry
  // native adjoints, so no new opcode. Neither form goes through
  // shape_of -- the language pairs a vector with a row_vector here, and
  // the only thing the difference could change, element order, is the
  // same on both sides because a length is all either view carries.
  if (const BuiltinSpec* paired = shaped_builtin_spec(
          e.name, e.args.size(), BuiltinShapePolicy::PairedReduction)) {
    actuals.require_arity(paired->arity);
    Val a = actuals.at(0).value();
    Val b = paired->arity == 2 ? actuals.at(1).value() : a;
    (void)resolved_builtin_layout(
        e, *paired,
        paired->arity == 2 ? std::vector<Val>{a, b} : std::vector<Val>{a});
    if (paired->difference) {
      const int64_t la = g.slots[a.slot].len;
      SlotInfo si;
      si.param_free = a.si.param_free && b.si.param_free;
      if (la > 1) si.kind = ViewKind::Vector;
      Val d = with_layout(emit_value(OP_SUB, {a, b}, la, si),
                          elementwise_layout({a, b}));
      return with_layout(emit_value(OP_DOT, {d, d}, 1),
                         ExpressionLayout::scalar());
    }
    return with_layout(emit_value(OP_DOT, {a, b}, 1),
                       ExpressionLayout::scalar());
  }

  // Registered grouped reductions: one dot per column or row through the
  // shared kernel, whose in-order accumulation matches the AoS
  // reverse-mode overloads CmdStan's model block instantiates (the old
  // ones-vector GEMM lowering grouped by Eigen's product packets instead).
  if (const BuiltinSpec* grouped = shaped_builtin_spec(
          e.name, e.args.size(), BuiltinShapePolicy::GroupedReduction)) {
    actuals.require_arity(grouped->arity);
    Val a = actuals.at(0).value();
    Val b = grouped->arity == 2 ? actuals.at(1).value() : a;
    BuiltinGroupedDotMap map;
    try {
      map = builtin_grouped_dot_map(
          *grouped, builtin_argument_shape(e.args[0], a),
          builtin_argument_shape(e.args[grouped->arity == 2 ? 1 : 0], b));
    } catch (const std::invalid_argument& error) {
      fail(e.name + ": " + error.what(), e.raw);
    }
    SlotInfo si = view_of(
        map.result.container == FunctionContainerKind::RowVector ? "URowVector"
                                                                 : "UVector");
    si.param_free = a.si.param_free && b.si.param_free;
    return with_layout(
        emit_value(OP_GROUP_DOT, {a, b}, map.groups, si,
                   {checked_immediate(map.groups, "grouped dot groups"),
                    checked_immediate(map.width, "grouped dot width"),
                    checked_immediate(map.group_stride, "grouped dot stride"),
                    checked_immediate(map.cell_stride, "grouped dot stride")}),
        owning_layout(si));
  }

  // Registered matrix operations: one dedicated kernel per name, with the
  // shared resolver supplying the shape checks, the idata block, and the
  // shape half of the variant; the autodiff half comes from the emitted
  // value, the same bit the named branches always stamped.
  if (const BuiltinSpec* matrix = shaped_builtin_spec(
          e.name, e.args.size(), BuiltinShapePolicy::MatrixOp)) {
    actuals.require_arity(matrix->arity);
    Val a = actuals.at(0).value();
    // A runtime square extent keeps matrix_exp's dynamic kernel: the
    // resolver reasons over static shapes only.
    if (matrix->matrix_op == BuiltinMatrixOp::MatrixExp &&
        has_runtime_shape(a)) {
      if (a.runtime_dims.size() != 2 || a.runtime_dims[0] < 0 ||
          a.runtime_dims[0] != a.runtime_dims[1])
        fail("matrix_exp: needs one runtime square extent", e.raw);
      Val extent{a.runtime_dims[0], false, view_of("UInt"),
                 ExpressionLayout::scalar()};
      extent.si.param_free = true;
      Val result = emit_value(OP_MATRIX_EXP_DYNAMIC, {a, extent},
                              g.slots[a.slot].len, a.si);
      result.runtime_dims = a.runtime_dims;
      return with_layout(result, owning_layout(a.si));
    }
    std::vector<Val> args{a};
    if (matrix->arity == 2) args.push_back(actuals.at(1).value());
    BuiltinMatrixMap map;
    try {
      std::vector<BuiltinArgumentShape> shapes;
      shapes.reserve(args.size());
      for (size_t k = 0; k < args.size(); ++k)
        shapes.push_back(builtin_argument_shape(e.args[k], args[k]));
      map = builtin_matrix_map(*matrix, shapes);
    } catch (const std::invalid_argument& error) {
      fail(e.name + ": " + error.what(), e.raw);
    }
    SlotInfo si;
    switch (map.result.container) {
      case FunctionContainerKind::Matrix:
        si = matrix_view(map.result.dimensions[0], map.result.dimensions[1]);
        break;
      case FunctionContainerKind::Vector:
        si = view_of("UVector");
        break;
      default:
        break;
    }
    si.param_free = std::all_of(args.begin(), args.end(),
                                [](const Val& v) { return v.si.param_free; });
    std::vector<int> idata;
    idata.reserve(map.idata.size());
    for (const int64_t value : map.idata)
      idata.push_back(checked_immediate(value, e.name + " extent"));
    Val v = matrix->arity == 2 ? emit_value(matrix->opcode, {args[0], args[1]},
                                            map.result.storage_size, si, idata)
                               : emit_value(matrix->opcode, {args[0]},
                                            map.result.storage_size, si, idata);
    g.ops.back().variant =
        (uint8_t)(map.variant | (v.autodiff ? map.active_variant : 0u));
    return with_layout(v, map.result.container == FunctionContainerKind::Scalar
                              ? ExpressionLayout::scalar()
                              : owning_layout(si));
  }

  // Registered reductions: one container argument, one scalar result,
  // through the registry-declared kernel. Resolution by name and arity
  // tolerates hand-built MIR without numeric metadata. sum keeps its
  // integer-surface and runtime-extent specializations here; prod, min,
  // and max stay below on their provenance-aware phased lowerings.
  if (const BuiltinSpec* reduction =
          stanli::reduction_builtin_spec(e.name, e.args.size());
      reduction != nullptr) {
    if (e.name == "sum") {
      const bool int_surface =
          e.type_ == "UInt" || e.unsized.leaf == mir::UnsizedLeaf::Int ||
          (!e.args.empty() && e.args[0].unsized.leaf == mir::UnsizedLeaf::Int);
      if (int_surface && in_write_array) {
        if (runtime_int_sum_candidate(e))
          return lower_runtime_int_sum(e, actuals);
        if (!is_int_sum_surface(e))
          fail(
              "runtime integer sum needs one one-dimensional int-array "
              "argument and a scalar int result",
              e.raw);
        if (e.args[0].kind != mir::Expr::Var || expr_effectful(e))
          fail("direct runtime integer sum stays on WaInterp", e.raw);
        // A param-free named array retains the legacy OP_SUM_VEC/fold path.
      }
    }
    actuals.require_arity(1);
    Val a = actuals.at(0).value();
    if (e.name == "sum" && has_runtime_shape(a)) {
      const int extent_slot = one_runtime_extent(a, "sum");
      Val extent{extent_slot, false, view_of("UInt"),
                 ExpressionLayout::scalar()};
      extent.si.param_free = true;
      return with_layout(emit_value(OP_SUM_VEC_DYNAMIC, {a, extent}, 1),
                         ExpressionLayout::scalar());
    }
    (void)resolved_builtin_layout(e, *reduction, std::vector<Val>{a});
    return with_layout(emit_value(reduction->opcode, {a}, 1),
                       ExpressionLayout::scalar());
  }

  if (builtin != nullptr && builtin->arity == 1 &&
      (builtin->shape == BuiltinShapePolicy::Elementwise ||
       builtin->shape == BuiltinShapePolicy::WholeValue)) {
    actuals.require_arity(1);
    Val a = actuals.at(0).value();
    const BuiltinLayout resolved =
        resolved_builtin_layout(e, *builtin, std::vector<Val>{a});
    SlotInfo si = a.si;
    // Every registered unary preserves its complete logical result view.
    // Whole-value kernels such as softmax and cumulative_sum can themselves
    // be vectorized over arrays, including one-lane leaves, so rebuilding a
    // view from the shallow MIR result spelling would discard outer axes.
    if (e.type_ == "UMatrix" && !is_matrix(si))
      fail(e.name + ": matrix result has unknown logical extents", e.raw);
    stamp_kind(&si, e.type_);
    si.param_free = a.si.param_free;
    // These functions return freshly allocated Eigen containers. Their
    // result starts at lane zero independently of the input's provenance;
    // the other unary operations are elementwise evaluator expressions.
    const ExpressionLayout layout =
        builtin->shape != BuiltinShapePolicy::WholeValue
            ? elementwise_layout({a})
            : owning_layout(si);
    return with_layout(
        emit_value(builtin->opcode, {a}, g.slots[a.slot].len, si), layout);
  }
  // plus, and its operator spelling, are the identity on every shape.
  if (e.name == "PPlus__" || (e.name == "plus" && e.args.size() == 1)) {
    actuals.require_arity(1);
    return actuals.at(0).value();
  }
  if (e.name == "min" || e.name == "max") {
    // Preserve the construction-time path for well-formed data-only
    // extrema, including the scalar two-argument overload.  Dynamic
    // lowering now uses the lowered argument's layout rather than
    // re-deriving its provenance from MIR syntax.
    if (e.args.size() == 1 || e.args.size() == 2)
      if (auto v = fold_const(e)) return *v;
    const mir::ExtremaCall call = mir::extrema_call(e);
    if (call.kind == mir::ExtremaKind::Legacy)
      fail("min/max expression surface stays on WaInterp", e.raw);
    return call.surface == mir::ExtremaSurface::IntPair
               ? lower_extrema_pair(e, actuals, call.kind)
               : lower_extrema_reduction(e, actuals, call);
  }
  if (e.name == "prod") {
    // Preserve the pre-existing construction-time behavior for data-only
    // products. Dynamic products use OP_PROD_VEC in either graph.
    if (auto v = fold_const(e)) return *v;
    if (e.args.size() != 1 || e.type_ != "UReal" ||
        e.unsized.leaf != mir::UnsizedLeaf::Real || e.unsized.depth != 0)
      fail("prod needs exactly one scalar-real result", e.raw);
    actuals.require_arity(1);
    Val a = actuals.at(0).value();
    if ((!is_vector(a.si) && !is_row_vector(a.si)) || g.slots[a.slot].len <= 0)
      fail("prod needs a nonempty vector or row-vector argument", e.raw);
    const bool active = a.autodiff && !in_write_array;
    const ReductionGrouping grouping = reduction_grouping(a, active);
    if (grouping == ReductionGrouping::Unknown)
      fail("prod expression grouping is not native", e.raw);
    Val result =
        with_layout(emit_value(OP_PROD_VEC, {a}, 1, {},
                               reduction_phase_idata(a, grouping, "prod")),
                    ExpressionLayout::scalar());
    // The active bit already selects scalar Matrix<var> traversal. Keep
    // the explicit scalar bit for inactive strided/gathered values so the
    // established active-vector variant remains 2.
    const bool scalar = grouping == ReductionGrouping::Scalar && !active;
    const bool phased = grouping == ReductionGrouping::Phased;
    g.ops.back().variant = static_cast<uint8_t>(
        (scalar ? 1u : 0u) | (active ? 2u : 0u) | (phased ? 4u : 0u));
    return result;
  }
  if (e.name == "rep_vector" || e.name == "rep_row_vector") {
    actuals.require_arity(2);
    Val a = actuals.at(0).value();
    if (region_current && needs_runtime_value(actuals.at(1).expr())) {
      const auto range = region_range(actuals.at(1).expr());
      if (!range || range->hi < 0)
        fail(e.name + ": runtime extent needs a finite capacity", e.raw);
      Val extent = actuals.at(1).value();
      if (!is_scalar(extent) || extent.autodiff)
        fail(e.name + ": runtime extent must be a data integer", e.raw);
      Val result = emit_value(OP_REP_VEC_DYNAMIC, {a, extent}, range->hi,
                              view_of(e.type_));
      result.runtime_dims = {extent.slot};
      return with_layout(result, owning_layout(view_of(e.type_)));
    }
    const long n = actuals.at(1).require_constant_int("rep_vector extent");
    return with_layout(emit_value(OP_REP_VEC, {a}, n, view_of(e.type_)),
                       owning_layout(view_of(e.type_)));
  }
  if (e.name == "rep_array" && e.args.size() >= 2 && e.args.size() <= 4) {
    // The element keeps its shape; rep_array prepends up to three outer
    // dimensions and tiles the element buffer once per outer cell. That
    // is a gather that walks 0..w-1 repeatedly.
    actuals.require_arity(2, 4);
    Val a = actuals.at(0).value();
    const int64_t w = g.slots[a.slot].len;
    std::vector<int64_t> dims;
    for (size_t k = 1; k < e.args.size(); ++k)
      dims.push_back(actuals.at(k).require_constant_int("rep_array extent"));
    const int64_t copies = checked_container_size(dims, e.name);
    ViewKind leaf = ViewKind::Flat;
    if (is_matrix(a.si)) {
      dims.push_back(a.si.rows);
      dims.push_back(a.si.cols);
      leaf = ViewKind::Matrix;
    } else if (is_vector(a.si)) {
      dims.push_back(w);
      leaf = ViewKind::Vector;
    } else if (is_row_vector(a.si)) {
      dims.push_back(w);
      leaf = ViewKind::RowVector;
    } else if (is_array(a.si)) {
      const ArrayShape& sh = array_shape(a.si);
      dims.insert(dims.end(), sh.dims.begin(), sh.dims.end());
      leaf = sh.leaf;
    }
    const int64_t size = checked_container_size({copies, w}, e.name);
    std::vector<int> gather;
    gather.reserve((size_t)size);
    for (int64_t k = 0; k < size; ++k)
      gather.push_back(checked_immediate(k % w, "rep_array gather offset"));
    const SlotInfo result_si = array_view(dims, leaf, a.si.param_free);
    return with_layout(emit_value(OP_GATHER, {a}, size, result_si, gather),
                       owning_layout(result_si));
  }
  if (e.name == "log_mix" && e.args.size() == 3) {
    Val a = actuals.at(0).value();
    Val b = actuals.at(1).value();
    Val c = actuals.at(2).value();
    return with_layout(emit_value(OP_LOG_MIX, {a, b, c}, 1),
                       ExpressionLayout::scalar());
  }
  if (e.name == "csr_matrix_times_vector" && e.args.size() == 6) {
    actuals.require_arity(6);
    const int64_t rows = actuals.at(0).require_constant_int("csr rows");
    const int64_t cols = actuals.at(1).require_constant_int("csr columns");
    if (rows <= 0 || cols <= 0)
      fail(e.name + ": row and column counts must be positive", e.raw);
    Val weights = actuals.at(2).value();
    Val vector = actuals.at(5).value();
    if (!is_vector(weights.si) || !is_vector(vector.si))
      fail(e.name + ": w and b must be vectors", e.raw);
    if (g.slots[vector.slot].len != cols)
      fail(e.name + ": column count does not match vector size", e.raw);
    const std::vector<int> columns =
        actuals.at(3).require_constant_ints("csr columns");
    const std::vector<int> starts =
        actuals.at(4).require_constant_ints("csr row starts");
    const int64_t nnz = g.slots[weights.slot].len;
    if ((int64_t)columns.size() != nnz)
      fail(e.name + ": w and v sizes differ", e.raw);
    if ((int64_t)starts.size() != rows + 1 || starts.front() != 1 ||
        starts.back() != nnz + 1)
      fail(e.name + ": u does not describe the requested rows", e.raw);
    for (int column : columns)
      if (column < 1 || column > cols)
        fail(e.name + ": v contains an out-of-range column", e.raw);

    Val result{-1, false, {}};
    for (int64_t row = 0; row < rows; ++row) {
      const int64_t begin = starts[(size_t)row] - 1;
      const int64_t end = starts[(size_t)row + 1] - 1;
      if (begin < 0 || end < begin || end > nnz)
        fail(e.name + ": u is not monotone or is out of range", e.raw);
      Val row_sum;
      if (begin == end) {
        row_sum = constant(0.0);
      } else {
        const int64_t len = end - begin;
        Val row_weights = with_layout(
            emit_value(OP_SLICE, {weights}, len, view_of("UVector"),
                       {checked_immediate(begin, "csr weight offset")}),
            contiguous_layout(weights, begin, "csr weights"));
        std::vector<int> gather;
        gather.reserve((size_t)len);
        for (int64_t k = begin; k < end; ++k)
          gather.push_back(columns[(size_t)k] - 1);
        Val row_vector =
            emit_value(OP_GATHER, {vector}, len, view_of("UVector"), gather);
        Val products =
            with_layout(emit_value(OP_MUL, {row_weights, row_vector}, len,
                                   view_of("UVector")),
                        elementwise_layout({row_weights, row_vector}));
        row_sum = with_layout(emit_value(OP_SUM_VEC, {products}, 1),
                              ExpressionLayout::scalar());
      }
      result = row == 0 ? row_sum
                        : with_layout(emit_value(OP_CONCAT2, {result, row_sum},
                                                 row + 1, view_of("UVector")),
                                      owning_layout(view_of("UVector")));
    }
    result.si = view_of("UVector");
    result.layout = owning_layout(result.si);
    return result;
  }

  return std::nullopt;
}

// Matrix shape and algebra: transposes, reshapes, factorizations,
// slices, and concatenations.
std::optional<Lowering::Val> Lowering::lower_matrix_fn(const mir::Expr& e,
                                                       CallArguments& actuals) {
  if (e.name == "tcrossprod" && e.args.size() == 1) {
    Val a = actuals.at(0).value();
    if (!is_matrix(a.si)) fail("tcrossprod: needs a matrix", e.raw);
    SlotInfo transpose_si = matrix_view(a.si.cols, a.si.rows, a.si.param_free);
    Val transpose =
        with_layout(emit_value(OP_TRANSPOSE, {a}, g.slots[a.slot].len,
                               transpose_si, {(int)a.si.rows, (int)a.si.cols}),
                    a.layout);
    SlotInfo si = matrix_view(a.si.rows, a.si.rows, a.si.param_free);
    return with_layout(
        emit_value(OP_GEMM, {a, transpose}, a.si.rows * a.si.rows, si,
                   {(int)a.si.rows, (int)a.si.cols, (int)a.si.rows}),
        owning_layout(si));
  }
  if ((e.name == "diag_pre_multiply" || e.name == "diag_post_multiply") &&
      e.args.size() == 2) {
    // diag_pre_multiply(v, M) = diag_matrix(v) * M (and the mirror);
    // the explicit zeros contribute exactly nothing to each sum.
    const bool pre = e.name.find("_pre_") != std::string::npos;
    Val v = actuals.at(pre ? 0 : 1).value();
    Val m = actuals.at(pre ? 1 : 0).value();
    const int64_t n = g.slots[v.slot].len;
    SlotInfo dsi = matrix_view(n, n, v.si.param_free);
    Val d = with_layout(emit_value(OP_DIAG_MATRIX, {v}, n * n, dsi),
                        owning_layout(dsi));
    Val a = pre ? d : m, b = pre ? m : d;
    SlotInfo si = matrix_view(a.si.rows, b.si.cols);
    return with_layout(
        emit_value(OP_GEMM, {a, b}, si.rows * si.cols, si,
                   {(int)a.si.rows, (int)a.si.cols, (int)b.si.cols}),
        owning_layout(si));
  }
  if (e.name == "rep_matrix") {
    SlotInfo si;
    if (e.args.size() == 3) {
      Val x = actuals.at(0).value();  // scalar fill
      const long R = actuals.at(1).require_constant_int("rep_matrix rows");
      const long C = actuals.at(2).require_constant_int("rep_matrix cols");
      si = matrix_view(R, C);
      return with_layout(
          emit_value(OP_REP_MAT, {x}, R * C, si, {(int)R, (int)C, 0}),
          owning_layout(si));
    }
    if (e.args.size() == 2) {
      Val v = actuals.at(0).value();
      const long n = actuals.at(1).require_constant_int("rep_matrix extent");
      const bool rowvec = actuals.at(0).expr().type_ == "URowVector";
      const long R = rowvec ? n : g.slots[v.slot].len;
      const long C = rowvec ? g.slots[v.slot].len : n;
      si = matrix_view(R, C);
      return with_layout(emit_value(OP_REP_MAT, {v}, R * C, si,
                                    {(int)R, (int)C, rowvec ? 2 : 1}),
                         owning_layout(si));
    }
    fail("rep_matrix arity", e.raw);
  }
  if (const std::optional<GpCov> gp = gp_cov_family(e.name);
      gp && e.args.size() == 3) {
    Val x = actuals.at(0).value();
    Val alpha = actuals.at(1).value();
    Val rho = actuals.at(2).value();
    // x may be data or a parameter: gp_cov_bwd rebuilds the points from
    // the promoted input, so a parameter x gets its adjoints too.
    // x is array[N] real (D == 1) or array[N] vector[D], stored
    // array-major, so D falls out of the declared dims.
    int64_t D = 1;
    if (is_array(x.si) && array_shape(x.si).dims.size() == 2)
      D = array_shape(x.si).dims[1];
    const int64_t N = g.slots[x.slot].len / D;
    SlotInfo si = matrix_view(N, N);
    Val v = emit_value(OP_GP_COV, {x, alpha, rho}, N * N, si, {(int)N, (int)D});
    g.ops.back().variant = static_cast<uint8_t>(*gp);
    return with_layout(v, owning_layout(si));
  }
  if (e.name == "quad_form_diag" && e.args.size() == 2) {
    // quad_form_diag(M, v) = diag(v) * M * diag(v).
    Val m = actuals.at(0).value();
    Val v = actuals.at(1).value();
    if (!is_matrix(m.si)) fail("quad_form_diag: needs a matrix", e.raw);
    const int64_t n = g.slots[v.slot].len;
    SlotInfo dsi = matrix_view(n, n, v.si.param_free);
    Val d = with_layout(emit_value(OP_DIAG_MATRIX, {v}, n * n, dsi),
                        owning_layout(dsi));
    SlotInfo si = matrix_view(n, n);
    Val left = with_layout(
        emit_value(OP_GEMM, {d, m}, n * n, si, {(int)n, (int)n, (int)n}),
        owning_layout(si));
    return with_layout(
        emit_value(OP_GEMM, {left, d}, n * n, si, {(int)n, (int)n, (int)n}),
        owning_layout(si));
  }

  if ((e.name == "append_row" || e.name == "append_col") &&
      e.args.size() == 2) {
    Val a = actuals.at(0).value();
    Val b = actuals.at(1).value();
    const int64_t la = g.slots[a.slot].len, lb = g.slots[b.slot].len;
    const LogicalDims da = logical_dims(a.si, la, e.name);
    const LogicalDims db = logical_dims(b.si, lb, e.name);
    if (e.name == "append_col") {
      if (da.rows != db.rows) fail("append_col row mismatch", e.raw);
      const LogicalDims out_dims{da.rows, da.cols + db.cols};
      const SlotInfo si = view_for_dims(e.type_, out_dims);
      // Every supported value is column-major under this logical view;
      // adding columns is therefore always a contiguous concatenation.
      return with_layout(emit_value(OP_CONCAT2, {a, b}, la + lb, si),
                         owning_layout(si));
    }
    if (da.cols != db.cols) fail("append_row column mismatch", e.raw);
    const LogicalDims out_dims{da.rows + db.rows, da.cols};
    const SlotInfo si = view_for_dims(e.type_, out_dims);
    if (out_dims.cols == 1)
      return with_layout(emit_value(OP_CONCAT2, {a, b}, la + lb, si),
                         owning_layout(si));

    // Adding rows interleaves the two column-major operands one column at
    // a time. The same gather handles row-vectors and mixed matrix+row.
    Val cat = emit_value(OP_CONCAT2, {a, b}, la + lb, {});
    std::vector<int> idx;
    idx.reserve((size_t)(la + lb));
    for (int64_t j = 0; j < out_dims.cols; ++j) {
      for (int64_t i = 0; i < da.rows; ++i)
        idx.push_back((int)(j * da.rows + i));
      for (int64_t i = 0; i < db.rows; ++i)
        idx.push_back((int)(la + j * db.rows + i));
    }
    return with_layout(emit_value(OP_GATHER, {cat}, la + lb, si, idx),
                       owning_layout(si));
  }
  return std::nullopt;
}

}  // namespace lower_detail
}  // namespace stanli
