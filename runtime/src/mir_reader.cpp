#include <stanli/mir.hpp>

#include <limits>
#include <map>
#include <stdexcept>

namespace stanli {
namespace mir {
namespace {

using sexp::Node;

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

UnsizedView read_unsized(const Node& n) {
  UnsizedView out;
  const Node* leaf = &n;
  while (!leaf->is_atom() && leaf->head_is("UArray") && leaf->size() == 2) {
    if (out.depth == std::numeric_limits<uint8_t>::max())
      throw std::runtime_error("mir: unsized array nesting is too deep");
    ++out.depth;
    leaf = &(*leaf)[1];
  }
  if (leaf->is_atom()) out.leaf = unsized_leaf(leaf->atom);
  return out;
}

Expr read_expr(const Node& n);

Expr read_index(const Node& ix_n) {
  Expr ix;
  if (ix_n.is_atom() && ix_n.atom == "All") {
    ix.kind = Expr::FunApp;
    ix.name = "IndexAll";
  } else if (!ix_n.is_atom() && ix_n.head_is("Single")) {
    ix.kind = Expr::FunApp;
    ix.name = "IndexSingle";
    ix.args.push_back(read_expr(ix_n[1]));
  } else if (!ix_n.is_atom() && ix_n.head_is("Between")) {
    ix.kind = Expr::FunApp;
    ix.name = "IndexBetween";
    ix.args.push_back(read_expr(ix_n[1]));
    ix.args.push_back(read_expr(ix_n[2]));
  } else if (!ix_n.is_atom() && ix_n.head_is("MultiIndex")) {
    ix.kind = Expr::FunApp;
    ix.name = "IndexMulti";
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
      const Node& suffix = kind[2];
      if (!suffix.is_atom() &&
          (suffix.head_is("FnLpdf") || suffix.head_is("FnLpmf")))
        e.fn_propto = suffix[1].atom == "true";
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
        if (e.unsized.leaf != UnsizedLeaf::Unknown) {
          e.type_ = e.unsized.depth ? "UArray" : type.atom;
        } else {
          e.type_.clear();
          e.raw = dump(type);
        }
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
      s.decl_type = t.head_is("Sized") ? read_sized(t[1]) : SizedType{};
      if (!t.head_is("Sized")) {
        s.decl_type.raw = dump(t);
        // stanc3 declares scalar temporaries unsized. A vectorized `T[,]`
        // whose location is a container loops over the elements and hoists
        // the scale into one of these. A scalar carries no size
        // expression, so give it the sized spelling. Unsized containers
        // still reach the failure in sized_len, which is where their
        // missing size belongs.
        if (t.size() > 1 && t[1].is_atom()) {
          if (t[1].atom == "UReal")
            s.decl_type.base = "SReal";
          else if (t[1].atom == "UInt")
            s.decl_type.base = "SInt";
        }
      }
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
  } else if (head == "TargetPE") {
    s.kind = Stmt::TargetPE;
    s.target = read_expr(p[1]);
  } else if (head == "Block") {
    s.kind = Stmt::Block;
    read_stmt_list(p[1], s.body);
  } else if (head == "SList") {
    s.kind = Stmt::SList;
    read_stmt_list(p[1], s.body);
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
        // FnWriteParam names its column in the payload, not in the (empty)
        // argument list; FnCheck likewise carries its checked value here.
        if (const Node* v = field(internal, "var"))
          s.fn_args.push_back(read_expr((*v)[1]));
      }
    } else if (kind.head_is("StanLib")) {
      s.fn_name = kind[1].atom;
    } else {
      s.fn_name = dump(kind, 60);
    }
    for (size_t i = 2; i < p.size(); ++i)
      for (size_t a = 0; a < p[i].size(); ++a)
        s.fn_args.push_back(read_expr(p[i][a]));
  } else {
    s.raw = dump(p);
  }
  return s;
}

struct Binding {
  UnsizedView view;
  bool declared_data_only = false;
};

using Bindings = std::map<std::string, Binding>;
using Functions = std::map<std::string, const FunDef*>;

UnsizedView declared_view(const SizedType& type) {
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
    throw std::runtime_error("mir: malformed sized declaration type");
  view.depth = static_cast<uint8_t>(type.dims.size() - leaf_rank);
  return view;
}

void validate_bindings(const std::vector<Stmt>& body, Bindings& bindings,
                       const Functions& functions);

void validate_expression(const Expr& e, const Bindings& bindings,
                         const Functions& functions) {
  if (e.kind == Expr::FunApp && e.fn_lib == Expr::Lib::StanLib &&
      (e.name == "categorical_lpmf" || e.name == "categorical_logit_lpmf")) {
    if (e.args.size() != 2 || e.args[0].unsized.leaf != UnsizedLeaf::Int ||
        e.args[0].unsized.depth > 1 ||
        e.args[1].unsized.leaf != UnsizedLeaf::Vector ||
        e.args[1].unsized.depth != 0)
      throw std::runtime_error("mir: malformed categorical signature");
    for (const Expr& arg : e.args) {
      if (arg.kind != Expr::Var) continue;
      const auto binding = bindings.find(arg.name);
      if (binding == bindings.end())
        throw std::runtime_error("mir: categorical argument has no binding");
      if (binding->second.view.depth != arg.unsized.depth ||
          binding->second.view.leaf != arg.unsized.leaf)
        throw std::runtime_error(
            "mir: categorical argument type disagrees with its binding");
      // GQ is statically double even for a constrained-parameter read. In
      // log_prob, though, DataOnly metadata on an AutoDiffable declaration is
      // contradictory and must not be allowed to change propto semantics.
      if (arg.data_only && !binding->second.declared_data_only)
        throw std::runtime_error(
            "mir: categorical argument adlevel disagrees with its binding");
    }
  }
  if (e.kind == Expr::FunApp && e.fn_lib == Expr::Lib::UserDefined) {
    const auto found = functions.find(e.name);
    if (found == functions.end())
      throw std::runtime_error("mir: call to unknown user function " + e.name);
    const FunDef& function = *found->second;
    if (e.args.size() != function.arg_names.size())
      throw std::runtime_error("mir: user function arity mismatch for " +
                               e.name);
    for (size_t i = 0; i < e.args.size(); ++i) {
      if (e.args[i].unsized.depth != function.arg_views[i].depth ||
          e.args[i].unsized.leaf != function.arg_views[i].leaf)
        throw std::runtime_error(
            "mir: user function argument type mismatch for " + e.name);
      if (e.args[i].kind == Expr::Var && !e.args[i].promoted) {
        const auto binding = bindings.find(e.args[i].name);
        if (binding == bindings.end() ||
            binding->second.view.depth != e.args[i].unsized.depth ||
            binding->second.view.leaf != e.args[i].unsized.leaf)
          throw std::runtime_error(
              "mir: user function argument type disagrees with its binding "
              "for " +
              e.name);
      }
      if (function.arg_data_only[i] && !e.args[i].data_only)
        throw std::runtime_error(
            "mir: data-only function argument depends on a parameter");
    }
  }
  for (const Expr& arg : e.args) validate_expression(arg, bindings, functions);
}

void validate_bindings(const Stmt& s, Bindings& bindings,
                       const Functions& functions) {
  for (const Expr* e :
       {&s.init, &s.rhs, &s.target, &s.lower, &s.upper, &s.cond})
    validate_expression(*e, bindings, functions);
  for (const auto* expressions : {&s.read_dims, &s.lhs_idx, &s.fn_args})
    for (const Expr& e : *expressions)
      validate_expression(e, bindings, functions);
  for (const Transform* transform :
       {s.read_transform ? &*s.read_transform : nullptr,
        s.check_transform ? &*s.check_transform : nullptr})
    if (transform)
      for (const Expr& e : transform->args)
        validate_expression(e, bindings, functions);
  if (s.kind == Stmt::NRFunApp && s.fn_name == "FnCheck") {
    if (!s.check_transform)
      throw std::runtime_error("mir: FnCheck has no transform");
    const Transform::Kind kind = s.check_transform->kind;
    if (is_structured_check(kind)) {
      if (!s.check_transform->args.empty() || s.fn_args.size() != 1)
        throw std::runtime_error("mir: malformed structured FnCheck");
      const UnsizedLeaf leaf = s.fn_args[0].unsized.leaf;
      const bool matrix = leaf == UnsizedLeaf::Matrix;
      const bool vector = leaf == UnsizedLeaf::Vector;
      const bool matrix_only =
          kind == Transform::CholeskyCorr || kind == Transform::Correlation ||
          kind == Transform::Covariance || kind == Transform::CholeskyCov;
      const bool vector_only = kind != Transform::SumToZero && !matrix_only;
      if ((!matrix && !vector) || (matrix_only && !matrix) ||
          (vector_only && !vector))
        throw std::runtime_error(
            "mir: structured FnCheck transform and value type disagree");
    } else if ((kind == Transform::Lower || kind == Transform::Upper) &&
               s.check_transform->args.size() == 1 && s.fn_args.size() == 2) {
      // Exact scalar/container compatibility is value- and shape-dependent;
      // the lowering/interpreter check it at the original statement site.
    } else {
      throw std::runtime_error("mir: unsupported or malformed FnCheck");
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
        throw std::runtime_error(
            "mir: FnCheck value type disagrees with its declaration");
    }
  }
  if (s.kind == Stmt::Decl) {
    bindings[s.decl_id] = Binding{declared_view(s.decl_type), s.decl_data_only};
  }
  if (s.kind == Stmt::SList) {
    validate_bindings(s.body, bindings, functions);
  } else if (s.kind == Stmt::IfElse) {
    for (const auto& child : s.body) {
      Bindings branch = bindings;
      validate_bindings(child, branch, functions);
    }
  } else if (!s.body.empty()) {
    Bindings nested = bindings;
    if (s.kind == Stmt::For)
      nested[s.loopvar] = Binding{UnsizedView{0, UnsizedLeaf::Int}, true};
    validate_bindings(s.body, nested, functions);
  }
}

void validate_bindings(const std::vector<Stmt>& body, Bindings& bindings,
                       const Functions& functions) {
  for (const auto& s : body) validate_bindings(s, bindings, functions);
}

}  // namespace

Program read_program(const sexp::Node& root) {
  Program prog;
  const Node* iv = field(root, "input_vars");
  if (!iv) throw std::runtime_error("mir: no input_vars section");
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
          f.arg_types.push_back(args[a][2].is_atom() ? args[a][2].atom
                                                     : dump(args[a][2], 40));
        }
      }
      if (const Node* fb2 = field(fd, "fdbody"))
        read_stmt_list((*fb2)[1], f.body);
      prog.fun_defs.push_back(std::move(f));
    }
  }
  const Node* lp = field(root, "log_prob");
  if (!lp) throw std::runtime_error("mir: no log_prob section");
  read_stmt_list((*lp)[1], prog.log_prob);
  if (const Node* gq = field(root, "generate_quantities"))
    read_stmt_list((*gq)[1], prog.generate_quantities);
  Functions functions;
  for (const auto& f : prog.fun_defs) functions[f.name] = &f;
  Bindings inputs;
  for (const auto& [name, type] : prog.input_vars)
    inputs[name] = Binding{declared_view(type), true};
  Bindings prepare_bindings = inputs;
  validate_bindings(prog.prepare_data, prepare_bindings, functions);
  Bindings log_prob_bindings = prepare_bindings;
  validate_bindings(prog.log_prob, log_prob_bindings, functions);
  Bindings gq_bindings = prepare_bindings;
  validate_bindings(prog.generate_quantities, gq_bindings, functions);
  for (const auto& f : prog.fun_defs) {
    Bindings args;
    for (size_t i = 0; i < f.arg_names.size(); ++i)
      args[f.arg_names[i]] = Binding{f.arg_views[i], f.arg_data_only[i]};
    validate_bindings(f.body, args, functions);
  }
  if (const Node* ov = field(root, "output_vars")) {
    // ((name <opaque> (...)) ...): parameters, transformed parameters and
    // generated quantities in declaration order -- the order FnWriteParam
    // statements emit them in. Only the names matter here; they are the
    // naming fallback for a write whose variable reference the optimizer
    // replaced with the value itself.
    const Node& vars = (*ov)[1];
    for (size_t i = 0; i < vars.size(); ++i)
      prog.output_vars.push_back(vars[i][0].atom);
  }
  return prog;
}

}  // namespace mir
}  // namespace stanli
