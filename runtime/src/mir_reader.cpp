#include <stanli/callable_transform.hpp>
#include <stanli/function_registry.hpp>
#include <stanli/large_stack.hpp>
#include <stanli/mir.hpp>

#include "mir_reader_internal.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>

namespace stanli {
namespace mir {
namespace {

using sexp::Node;

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

std::string dump(const Node& n, size_t budget = 240) {
  std::string out;
  if (n.is_atom()) {
    out = n.atom;
  } else {
    out = "(";
    for (size_t i = 0; i < n.size() && out.size() < budget; ++i) {
      if (i) out += ' ';
      out += dump(n[i], budget - out.size());
    }
    out += ")";
  }
  if (out.size() > budget) out = out.substr(0, budget) + "...";
  return out;
}

// Find `(key ...)` inside a list of key-value lists; null if absent.
const Node* field(const Node& n, const char* key) {
  for (size_t i = 0; i < n.size(); ++i)
    if (n[i].head_is(key)) return &n[i];
  return nullptr;
}

UnsizedLeaf unsized_leaf(const std::string& atom) {
  if (atom == "UInt") return UnsizedLeaf::Int;
  if (atom == "UReal") return UnsizedLeaf::Real;
  if (atom == "UComplex") return UnsizedLeaf::Complex;
  if (atom == "UVector") return UnsizedLeaf::Vector;
  if (atom == "URowVector") return UnsizedLeaf::RowVector;
  if (atom == "UMatrix") return UnsizedLeaf::Matrix;
  return UnsizedLeaf::Unknown;
}

// stanc3's Fun_kind suffix: `FnPlain`, `(FnLpdf true)`, ...
FnSuffix read_suffix(const Node& n, bool* propto) {
  *propto = false;
  const std::string& head = n.is_atom() ? n.atom : n[0].atom;
  if (!n.is_atom() && n.size() == 2 && (head == "FnLpdf" || head == "FnLpmf")) {
    *propto = n[1].atom == "true";
    return head == "FnLpdf" ? FnSuffix::Lpdf : FnSuffix::Lpmf;
  }
  if (head == "FnRng") return FnSuffix::Rng;
  if (head == "FnTarget") return FnSuffix::Target;
  if (head == "FnJacobian") return FnSuffix::Jacobian;
  if (head == "FnPlain") return FnSuffix::Plain;
  throw mir::MirError("mir: unknown function suffix " + head);
}

UnsizedView read_unsized(const Node& n);

// (UFun ((arg types...) return-type suffix mem-pattern)): a variable that
// names a function.
std::optional<FunctionType> read_function_type(const Node& type) {
  if (type.is_atom() || !type.head_is("UFun")) return std::nullopt;
  if (type.size() != 2 || type[1].is_atom() || type[1].size() < 3 ||
      type[1][0].is_atom())
    throw mir::MirError("mir: malformed function type");
  FunctionType out;
  for (const Node& arg : type[1][0].kids) {
    if (arg.is_atom() || arg.size() != 2)
      throw mir::MirError("mir: malformed function type");
    out.args.push_back(read_unsized(arg[1]));
  }
  out.suffix = read_suffix(type[1][2], &out.propto);
  return out;
}

UnsizedView read_unsized(const Node& n) {
  UnsizedView out;
  const Node* leaf = &n;
  while (!leaf->is_atom() && leaf->head_is("UArray") && leaf->size() == 2) {
    if (out.depth == std::numeric_limits<uint8_t>::max())
      throw mir::MirError("mir: unsized array nesting is too deep");
    ++out.depth;
    leaf = &(*leaf)[1];
  }
  if (leaf->is_atom()) out.leaf = unsized_leaf(leaf->atom);
  return out;
}

struct NestingMeter {
  unsigned depth = 0;
  unsigned peak = 0;
};
thread_local NestingMeter nesting_meter;

class NestingScope {
 public:
  NestingScope() {
    if (nesting_meter.depth >= nesting_limit())
      throw NestingTooDeep("mir: nested too deeply (more than " +
                           std::to_string(nesting_limit()) + " levels)");
    nesting_meter.peak = std::max(nesting_meter.peak, ++nesting_meter.depth);
  }
  ~NestingScope() { --nesting_meter.depth; }
  NestingScope(const NestingScope&) = delete;
  NestingScope& operator=(const NestingScope&) = delete;
};

Expr read_expr(const Node& n);

Expr read_index(const Node& ix_n) {
  Expr ix;
  if (ix_n.is_atom() && ix_n.atom == "All") {
    ix.kind = Expr::IndexAll;
  } else if (!ix_n.is_atom() && ix_n.head_is("Single")) {
    ix.kind = Expr::IndexSingle;
    ix.args.push_back(read_expr(ix_n[1]));
  } else if (!ix_n.is_atom() && ix_n.head_is("Between")) {
    ix.kind = Expr::IndexBetween;
    ix.args.push_back(read_expr(ix_n[1]));
    ix.args.push_back(read_expr(ix_n[2]));
  } else if (!ix_n.is_atom() && ix_n.head_is("Upfrom")) {
    ix.kind = Expr::IndexUpfrom;
    ix.args.push_back(read_expr(ix_n[1]));
  } else if (!ix_n.is_atom() && ix_n.head_is("MultiIndex")) {
    ix.kind = Expr::IndexMulti;
    ix.args.push_back(read_expr(ix_n[1]));
  } else {
    ix.kind = Expr::Unsupported;
    ix.raw = dump(ix_n);
  }
  return ix;
}

// n is the `(pattern X)` payload X for an expression.
Expr read_expr_pattern(const Node& p) {
  Expr e;
  if (p.head_is("Var")) {
    e.kind = Expr::Var;
    e.name = p[1].atom;
  } else if (p.head_is("Lit")) {
    const std::string& k = p[1].atom;
    const std::string& v = p[2].atom;
    if (k == "Int") {
      e.kind = Expr::LitInt;
      e.lit_i = std::stol(v);
      e.lit = static_cast<double>(e.lit_i);
    } else if (k == "Real") {
      e.kind = Expr::LitReal;
      e.lit = std::stod(v);
    } else {  // Str
      e.kind = Expr::LitStr;
      e.lit_s = v;
    }
  } else if (p.head_is("FunApp")) {
    e.kind = Expr::FunApp;
    const Node& kind = p[1];
    // (StanLib normal_lpdf (FnLpdf true) AoS) and
    // (UserDefined f_lpdf (FnLpdf true)) spell the propto flag the same
    // way, and it means the same thing on both: `_lupdf` was written. On a
    // user function it is the value of CmdStan's propto__ template
    // argument, which the body inherits.
    auto read_propto = [&](const Node& kind) {
      e.fn_suffix = read_suffix(kind[2], &e.fn_propto);
    };
    if (kind.head_is("StanLib")) {
      e.fn_lib = Expr::Lib::StanLib;
      e.name = kind[1].atom;
      read_propto(kind);
    } else if (kind.head_is("CompilerInternal")) {
      e.fn_lib = Expr::Lib::Internal;
      const Node& internal = kind[1];
      e.name = internal.is_atom() ? internal.atom : internal[0].atom;
      if (!internal.is_atom()) e.raw = dump(internal);
    } else if (kind.head_is("UserDefined")) {
      e.fn_lib = Expr::Lib::UserDefined;
      e.name = kind[1].atom;
      read_propto(kind);
    } else {
      e.kind = Expr::Unsupported;
      e.raw = dump(p);
      return e;
    }
    for (size_t i = 2; i < p.size(); ++i) {
      // args are a single list node
      const Node& args = p[i];
      for (size_t a = 0; a < args.size(); ++a)
        e.args.push_back(read_expr(args[a]));
    }
  } else if (p.head_is("Promotion")) {
    e = read_expr(p[1]);  // transparent: keep inner, adopt promoted type later
    e.promoted = true;
    return e;
  } else if (p.head_is("TernaryIf")) {
    e.kind = Expr::TernaryIf;
    for (size_t i = 1; i < p.size(); ++i) e.args.push_back(read_expr(p[i]));
  } else if (p.head_is("EOr") || p.head_is("EAnd")) {
    e.kind = p.head_is("EOr") ? Expr::EOr : Expr::EAnd;
    for (size_t i = 1; i < p.size(); ++i) e.args.push_back(read_expr(p[i]));
  } else if (p.head_is("Indexed")) {
    e.kind = Expr::Indexed;
    e.args.push_back(read_expr(p[1]));
    for (size_t i = 2; i < p.size(); ++i) {
      const Node& idxs = p[i];
      for (size_t a = 0; a < idxs.size(); ++a)
        e.args.push_back(read_index(idxs[a]));
    }
  } else {
    e.kind = Expr::Unsupported;
    e.raw = dump(p);
  }
  return e;
}

// n is `((pattern X) (meta ((type_ T) (loc _) (adlevel A))))`.
Expr read_expr(const Node& n) {
  NestingScope nesting;
  const Node* pat = field(n, "pattern");
  if (!pat) {
    Expr e;
    e.kind = Expr::Unsupported;
    e.raw = dump(n);
    return e;
  }
  Expr e = read_expr_pattern((*pat)[1]);
  if (const Node* meta = field(n, "meta")) {
    const Node& m = (*meta)[1];
    if (!m.is_atom()) {
      if (const Node* t = field(m, "type_")) {
        const Node& type = (*t)[1];
        e.unsized = read_unsized(type);
        if (e.kind == Expr::Var) {
          e.fn_type = read_function_type(type);
          // A density passed as `foo_lupdf` names the definition foo_lpdf
          // and asks for its unnormalized form, as stanc3's C++ generation
          // reads it; the portable producer does the same.
          if (e.fn_type && e.fn_type->propto) {
            e.fn_propto = true;
            for (const char* unnormalized : {"_lupdf", "_lupmf"})
              if (e.name.size() > 6 &&
                  e.name.compare(e.name.size() - 6, 6, unnormalized) == 0)
                e.name.replace(e.name.size() - 6, 6,
                               unnormalized[4] == 'd' ? "_lpdf" : "_lpmf");
          }
        }
        if (e.unsized.leaf == UnsizedLeaf::Unknown) e.raw = dump(type);
      }
      if (const Node* a = field(m, "adlevel"))
        e.data_only = (*a)[1].is_atom() && (*a)[1].atom == "DataOnly";
    }
  }
  return e;
}

SizedType read_sized(const Node& n) {
  SizedType st;
  if (n.is_atom()) {
    st.base = n.atom;  // SInt / SReal / SComplex
    return st;
  }
  st.base = n[0].atom;
  if (st.base == "SVector" || st.base == "SRowVector") {
    st.dims.push_back(read_expr(n[2]));  // n[1] is AoS/SoA mem pattern
  } else if (st.base == "SMatrix") {
    st.dims.push_back(read_expr(n[2]));
    st.dims.push_back(read_expr(n[3]));
  } else if (st.base == "SArray") {
    SizedType inner = read_sized(n[1]);
    st.dims.push_back(read_expr(n[2]));
    for (auto& d : inner.dims) st.dims.push_back(std::move(d));
    // Innermost element base kept for the lowering (nested SArray chains
    // propagate it up, so array[T, N] int reports SInt).
    st.elem_base = inner.base == "SArray" ? inner.elem_base : inner.base;
  } else {
    st.raw = dump(n);
  }
  return st;
}

Transform read_transform(const Node& n) {
  Transform t;
  if (n.is_atom()) {
    if (n.atom == "Identity")
      t.kind = Transform::Identity;
    else if (n.atom == "Simplex")
      t.kind = Transform::Simplex;
    else if (n.atom == "Ordered")
      t.kind = Transform::Ordered;
    else if (n.atom == "PositiveOrdered")
      t.kind = Transform::PositiveOrdered;
    else if (n.atom == "CholeskyCorr")
      t.kind = Transform::CholeskyCorr;
    else if (n.atom == "UnitVector")
      t.kind = Transform::UnitVector;
    else if (n.atom == "SumToZero")
      t.kind = Transform::SumToZero;
    else if (n.atom == "Correlation")
      t.kind = Transform::Correlation;
    else if (n.atom == "Covariance")
      t.kind = Transform::Covariance;
    else if (n.atom == "CholeskyCov")
      t.kind = Transform::CholeskyCov;
    else if (n.atom == "StochasticColumn")
      t.kind = Transform::StochasticColumn;
    else if (n.atom == "StochasticRow")
      t.kind = Transform::StochasticRow;
    else
      t.kind = Transform::Unsupported;
    t.raw = n.atom;
    return t;
  }
  const std::string& k = n[0].atom;
  if (k == "Lower")
    t.kind = Transform::Lower;
  else if (k == "Upper")
    t.kind = Transform::Upper;
  else if (k == "LowerUpper")
    t.kind = Transform::LowerUpper;
  else if (k == "Offset")
    t.kind = Transform::Offset;
  else if (k == "Multiplier")
    t.kind = Transform::Multiplier;
  else if (k == "OffsetMultiplier")
    t.kind = Transform::OffsetMultiplier;
  else {
    t.kind = Transform::Unsupported;
    t.raw = dump(n);
  }
  for (size_t i = 1; i < n.size(); ++i) t.args.push_back(read_expr(n[i]));
  return t;
}

Stmt read_stmt(const Node& n);

void read_stmt_list(const Node& list, std::vector<Stmt>& out) {
  for (size_t i = 0; i < list.size(); ++i) out.push_back(read_stmt(list[i]));
}

Stmt read_stmt(const Node& n) {
  NestingScope nesting;
  Stmt s;
  const Node* pat = field(n, "pattern");
  if (!pat) {
    s.raw = dump(n);
    return s;
  }
  const Node& p = (*pat)[1];
  if (p.is_atom()) {
    if (p.atom == "Skip")
      s.kind = Stmt::Skip;
    else if (p.atom == "Break")
      s.kind = Stmt::Break;
    else if (p.atom == "Continue")
      s.kind = Stmt::Continue;
    else
      s.raw = p.atom;
    return s;
  }
  const std::string& head = p[0].atom;
  if (head == "Decl") {
    s.kind = Stmt::Decl;
    if (const Node* id = field(p, "decl_id")) s.decl_id = (*id)[1].atom;
    if (const Node* ad = field(p, "decl_adtype"))
      s.decl_data_only = (*ad)[1].is_atom() && (*ad)[1].atom == "DataOnly";
    if (const Node* dt = field(p, "decl_type")) {
      const Node& t = (*dt)[1];  // (Sized ST) or (Unsized T)
      if (t.head_is("Sized"))
        s.decl_type = read_sized(t[1]);
      else if (t.head_is("Unsized") && t.size() == 2) {
        const UnsizedView view = read_unsized(t[1]);
        s.decl_type = detail::unsized_declaration(
            view, view.leaf == UnsizedLeaf::Unknown ? dump(t[1]) : "");
      } else
        s.decl_type.raw = dump(t);
    }
    if (const Node* init = field(p, "initialize")) {
      const Node& iv = (*init)[1];
      if (iv.head_is("Assign")) {
        s.has_init = true;
        s.init = read_expr(iv[1]);
        // FnReadParam: pull transform + dims straight from the sexp.
        const Node& expr_pat = (*field(iv[1], "pattern"))[1];
        if (expr_pat.head_is("FunApp") &&
            expr_pat[1].head_is("CompilerInternal") &&
            !expr_pat[1][1].is_atom() &&
            expr_pat[1][1].head_is("FnReadParam")) {
          const Node& rp = expr_pat[1][1];
          if (const Node* c = field(rp, "constrain"))
            s.read_transform = read_transform((*c)[1]);
          if (const Node* d = field(rp, "dims")) {
            const Node& dims = (*d)[1];
            for (size_t i = 0; i < dims.size(); ++i)
              s.read_dims.push_back(read_expr(dims[i]));
          }
        }
      }
    }
  } else if (head == "Assignment") {
    s.kind = Stmt::Assignment;
    const Node& lhs = p[1];  // ((LVariable name) (idxs))
    if (lhs[0].head_is("LVariable")) {
      s.lhs = lhs[0][1].atom;
    } else {
      s.kind = Stmt::Unsupported;
      s.raw = dump(p);
      return s;
    }
    if (lhs.size() > 1) {
      for (size_t i = 0; i < lhs[1].size(); ++i)
        s.lhs_idx.push_back(read_index(lhs[1][i]));
    }
    s.rhs = read_expr(p[p.size() - 1]);
  } else if (head == "TargetPE" || head == "JacobianPE") {
    s.kind = Stmt::TargetPE;
    s.jacobian = head == "JacobianPE";
    s.target = read_expr(p[1]);
  } else if (head == "Block") {
    s.kind = Stmt::Block;
    read_stmt_list(p[1], s.body);
  } else if (head == "SList") {
    s.kind = Stmt::SList;
    read_stmt_list(p[1], s.body);
  } else if (head == "Profile") {
    // (Profile name (stmts)): the name is only for stanc's own profiling
    // output, which stanli does not produce, so unwrap to a plain block.
    s.kind = Stmt::Block;
    read_stmt_list(p[2], s.body);
  } else if (head == "For") {
    s.kind = Stmt::For;
    if (const Node* lv = field(p, "loopvar")) s.loopvar = (*lv)[1].atom;
    if (const Node* lo = field(p, "lower")) s.lower = read_expr((*lo)[1]);
    if (const Node* up = field(p, "upper")) s.upper = read_expr((*up)[1]);
    if (const Node* b = field(p, "body")) s.body.push_back(read_stmt((*b)[1]));
  } else if (head == "IfElse") {
    s.kind = Stmt::IfElse;
    s.cond = read_expr(p[1]);
    s.body.push_back(read_stmt(p[2]));
    if (p.size() > 3 && !p[3].is_atom() && p[3].size() > 0)
      s.body.push_back(read_stmt(p[3][0]));
  } else if (head == "While") {
    s.kind = Stmt::While;
    s.cond = read_expr(p[1]);
    s.body.push_back(read_stmt(p[2]));
  } else if (head == "Return") {
    // (Return ()) or (Return (expr)); the value, if any, lands in rhs.
    s.kind = Stmt::Return;
    s.has_init = !p[1].is_atom() && p[1].size() > 0;
    if (s.has_init) s.rhs = read_expr(p[1][0]);
  } else if (head == "NRFunApp") {
    s.kind = Stmt::NRFunApp;
    const Node& kind = p[1];
    if (kind.head_is("CompilerInternal")) {
      s.fn_lib = Expr::Lib::Internal;
      const Node& internal = kind[1];
      s.fn_name = internal.is_atom() ? internal.atom : internal[0].atom;
      if (!internal.is_atom()) {
        // FnCheck's payload distinguishes lower from upper and names the
        // value. Its ordinary operands contain the value and bound but not
        // that relation.
        if (s.fn_name == "FnCheck") {
          if (const Node* t = field(internal, "trans"))
            s.check_transform = read_transform((*t)[1]);
          if (const Node* name = field(internal, "var_name"))
            s.check_var_name = (*name)[1].atom;
        }
        // transform_inits' FnWriteParam names the transform to INVERT, so
        // that an already-constrained value becomes a free one. The payload
        // is an OCaml option one level deeper than FnReadParam's `constrain`:
        // `(unconstrain_opt (Simplex))` and `(unconstrain_opt ((Lower e)))`
        // both wrap the transform in a one-element list, and
        // `(unconstrain_opt ())` is None -- the shape every write_array
        // FnWriteParam has, its value being constrained already.
        if (s.fn_name == "FnWriteParam") {
          if (const Node* t = field(internal, "unconstrain_opt")) {
            const Node& option = (*t)[1];
            if (!option.is_atom() && option.size() == 1)
              s.write_transform = read_transform(option[0]);
          }
        }
        // FnWriteParam names its column in the payload, not in the (empty)
        // argument list; FnCheck likewise carries its checked value here.
        if (const Node* v = field(internal, "var"))
          s.fn_args.push_back(read_expr((*v)[1]));
      }
    } else if (kind.head_is("StanLib")) {
      s.fn_name = kind[1].atom;
      bool propto = false;
      if (kind.size() > 2) s.fn_suffix = read_suffix(kind[2], &propto);
    } else if (kind.head_is("UserDefined") && kind.size() == 3) {
      s.fn_lib = Expr::Lib::UserDefined;
      s.fn_name = kind[1].atom;
      bool propto = false;
      s.fn_suffix = read_suffix(kind[2], &propto);
    } else {
      s.kind = Stmt::Unsupported;
      s.raw = dump(p);
      return s;
    }
    for (size_t i = 2; i < p.size(); ++i)
      for (size_t a = 0; a < p[i].size(); ++a)
        s.fn_args.push_back(read_expr(p[i][a]));
  } else {
    s.raw = dump(p);
  }
  return s;
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
    case Expr::Promotion:
      if (e.args.size() != 1) malformed("Promotion expression arity");
      break;
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
  if (type.base == "SVector" || type.base == "SRowVector") {
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

void validate_statement_shape(const Stmt& statement) {
  validate_sized_type_shape(statement.decl_type);
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
                       BindingScope* scope, const Functions& functions,
                       bool strict_variable_metadata);

void validate_expression(const Expr& e, const Bindings& bindings,
                         const Functions& functions,
                         bool strict_variable_metadata) {
  if (strict_variable_metadata && e.kind == Expr::Var && !e.promoted) {
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
  for (const Expr& arg : e.args)
    validate_expression(arg, bindings, functions, strict_variable_metadata);
}

void validate_bindings(const Stmt& s, Bindings& bindings, BindingScope* scope,
                       const Functions& functions,
                       bool strict_variable_metadata) {
  for_each_expr(s, [&](const Expr& e) {
    validate_expression(e, bindings, functions, strict_variable_metadata);
  });
  if (const auto call = user_defined_statement_call(s))
    validate_expression(*call, bindings, functions, strict_variable_metadata);
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
  if (strict_variable_metadata && s.kind == Stmt::Assignment &&
      s.lhs_idx.empty()) {
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
    if (strict_variable_metadata &&
        s.decl_type.unsized.leaf != UnsizedLeaf::Unknown && s.has_init &&
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
    validate_bindings(s.body, bindings, scope, functions,
                      strict_variable_metadata);
  } else if (s.kind == Stmt::IfElse) {
    for (const auto& child : s.body) {
      BindingScope branch(bindings);
      validate_bindings(child, bindings, &branch, functions,
                        strict_variable_metadata);
    }
  } else if (!s.body.empty()) {
    BindingScope nested(bindings);
    if (s.kind == Stmt::For)
      nested.bind(s.loopvar, Binding{UnsizedView{0, UnsizedLeaf::Int}, true});
    validate_bindings(s.body, bindings, &nested, functions,
                      strict_variable_metadata);
  }
}

void validate_bindings(const std::vector<Stmt>& body, Bindings& bindings,
                       BindingScope* scope, const Functions& functions,
                       bool strict_variable_metadata) {
  for (const auto& s : body)
    validate_bindings(s, bindings, scope, functions, strict_variable_metadata);
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
  for (Expr& dim : type.dims) unmangle(dim);
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
          throw mir::MirError("mir: indistinguishable overloads of " +
                                   name);
    for (FunDef* f : defs) f->overload = signature(f->arg_views);
    overloads[name] = std::move(defs);
  }
  for (auto& [name, type] : prog.input_vars)
    for (Expr& d : type.dims) resolve_calls(d, overloads);
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
  if (view.depth == 0 && view.leaf == UnsizedLeaf::Int)
    type.base = "SInt";
  else if (view.depth == 0 && view.leaf == UnsizedLeaf::Real)
    type.base = "SReal";
  else
    type.unsized = view;
  return type;
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

void detail::finalize_program(Program& prog, bool strict_variable_metadata) {
  strip_eval(prog);
  unmangle(prog);
  resolve_overloads(prog);
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
  validate_bindings(prog.prepare_data, prepare_bindings, nullptr, functions,
                    strict_variable_metadata);
  Bindings log_prob_bindings = prepare_bindings;
  validate_bindings(prog.log_prob, log_prob_bindings, nullptr, functions,
                    strict_variable_metadata);
  Bindings gq_bindings = prepare_bindings;
  validate_bindings(prog.generate_quantities, gq_bindings, nullptr, functions,
                    strict_variable_metadata);
  // transform_inits sees data and transformed data and declares each
  // parameter locally, exactly like generate_quantities does.
  Bindings init_bindings = prepare_bindings;
  validate_bindings(prog.transform_inits, init_bindings, nullptr, functions,
                    strict_variable_metadata);
  for (const auto& f : prog.fun_defs) {
    Bindings args;
    for (size_t i = 0; i < f.arg_names.size(); ++i)
      args[f.arg_names[i]] = Binding{f.arg_views[i], f.arg_data_only[i]};
    validate_bindings(f.body, args, nullptr, functions,
                      strict_variable_metadata);
  }
}

// stanc3 separates write_array's three sections with top-level early-return
// guards on its emit flags:
//   if (!(emit_transformed_parameters__ || emit_generated_quantities__))
//     return;                                 <- transformed parameters start
//   if (!emit_generated_quantities__) return; <- generated quantities start
// stanli always emits every section, so drop the guards and keep where they
// were, as the portable producer does.
void read_write_array(const Node& list, Program& prog) {
  const auto mentions = [](const auto& self, const Expr& e,
                           const char* name) -> bool {
    if (e.kind == Expr::Var && e.name == name) return true;
    for (const Expr& arg : e.args)
      if (self(self, arg, name)) return true;
    return false;
  };
  std::optional<size_t> tp_begin, gq_begin;
  for (size_t i = 0; i < list.size(); ++i) {
    Stmt s = read_stmt(list[i]);
    const size_t here = prog.generate_quantities.size();
    if (s.kind == Stmt::IfElse && s.cond.kind == Expr::FunApp &&
        s.cond.args.size() == 1 && s.cond.args[0].kind == Expr::Var &&
        s.cond.args[0].name == "emit_generated_quantities__") {
      if (!gq_begin) {
        gq_begin = here;
        continue;
      }
    } else if (s.kind == Stmt::IfElse &&
               mentions(mentions, s.cond, "emit_transformed_parameters__")) {
      if (!tp_begin) {
        tp_begin = here;
        continue;
      }
    }
    prog.generate_quantities.push_back(std::move(s));
  }
  prog.generated_quantities_begin =
      gq_begin.value_or(prog.generate_quantities.size());
  prog.transformed_parameters_begin =
      tp_begin.value_or(prog.generated_quantities_begin);
}

Program read_program(const sexp::Node& root) {
  Program prog;
  nesting_meter = {};
  const Node* iv = field(root, "input_vars");
  if (!iv) throw mir::MirError("mir: no input_vars section");
  const Node& vars = (*iv)[1];
  for (size_t i = 0; i < vars.size(); ++i) {
    const Node& v = vars[i];  // (name <loc> sizedtype)
    prog.input_vars.emplace_back(v[0].atom, read_sized(v[2]));
  }
  if (const Node* pd = field(root, "prepare_data"))
    read_stmt_list((*pd)[1], prog.prepare_data);
  if (const Node* fb = field(root, "functions_block")) {
    const Node& defs = (*fb)[1];
    for (size_t i = 0; i < defs.size(); ++i) {
      const Node& fd = defs[i];
      FunDef f;
      if (const Node* nm = field(fd, "fdname")) f.name = (*nm)[1].atom;
      if (const Node* fa = field(fd, "fdargs")) {
        const Node& args = (*fa)[1];
        for (size_t a = 0; a < args.size(); ++a) {
          // (AutoDiffable name type) or (DataOnly name type)
          f.arg_names.push_back(args[a][1].atom);
          f.arg_views.push_back(read_unsized(args[a][2]));
          f.arg_data_only.push_back(args[a][0].is_atom() &&
                                    args[a][0].atom == "DataOnly");
        }
      }
      if (const Node* fb2 = field(fd, "fdbody"))
        read_stmt_list((*fb2)[1], f.body);
      prog.fun_defs.push_back(std::move(f));
    }
  }
  const Node* lp = field(root, "log_prob");
  if (!lp) throw mir::MirError("mir: no log_prob section");
  read_stmt_list((*lp)[1], prog.log_prob);
  if (const Node* gq = field(root, "generate_quantities"))
    read_write_array((*gq)[1], prog);
  // stanc3's inverse parameter transforms. Absent from a hand-written or
  // pre-backend-transform MIR, in which case constrained-scale inits stay
  // unavailable for this model rather than becoming an error here.
  if (const Node* ti = field(root, "transform_inits")) {
    prog.has_transform_inits = true;
    read_stmt_list((*ti)[1], prog.transform_inits);
  }
  if (const Node* ov = field(root, "output_vars")) {
    // ((name <opaque> ((out_unconstrained_st ST) (out_constrained_st ST)
    // (out_block B) (out_trans T))) ...)
    const Node& vars = (*ov)[1];
    for (size_t i = 0; i < vars.size(); ++i) {
      const Node& v = vars[i];
      const Node* type =
          v.size() == 3 ? field(v[2], "out_constrained_st") : nullptr;
      const Node* block = v.size() == 3 ? field(v[2], "out_block") : nullptr;
      if (!type || !block || !(*block)[1].is_atom())
        throw mir::MirError("mir: malformed output variable");
      OutputVar out;
      out.name = v[0].atom;
      out.type = read_sized((*type)[1]);
      const std::string& b = (*block)[1].atom;
      if (b == "Parameters")
        out.block = OutputVar::Block::Parameters;
      else if (b == "TransformedParameters")
        out.block = OutputVar::Block::TransformedParameters;
      else if (b == "GeneratedQuantities")
        out.block = OutputVar::Block::GeneratedQuantities;
      else
        throw mir::MirError("mir: unknown output block " + b);
      prog.output_vars.push_back(std::move(out));
    }
  }
  prog.nesting = nesting_meter.peak;
  detail::finalize_program(prog);
  return prog;
}

}  // namespace mir
}  // namespace stanli
