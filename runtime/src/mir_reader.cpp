#include <stanli/mir.hpp>

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
    if (kind.head_is("StanLib")) {
      e.fn_lib = Expr::Lib::StanLib;
      e.name = kind[1].atom;
      const Node& suffix = kind[2];
      if (!suffix.is_atom() &&
          (suffix.head_is("FnLpdf") || suffix.head_is("FnLpmf")))
        e.fn_propto = suffix[1].atom == "true";
    } else if (kind.head_is("CompilerInternal")) {
      e.fn_lib = Expr::Lib::Internal;
      const Node& internal = kind[1];
      e.name = internal.is_atom() ? internal.atom : internal[0].atom;
      if (!internal.is_atom()) e.raw = dump(internal);
    } else if (kind.head_is("UserDefined")) {
      e.fn_lib = Expr::Lib::UserDefined;
      e.name = kind[1].atom;
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
      if (const Node* t = field(m, "type_")) e.type_ = (*t)[1].atom;
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
    st.raw = inner.base == "SArray" ? inner.raw : inner.base;
  } else {
    st.raw = dump(n);
  }
  return st;
}

Transform read_transform(const Node& n) {
  Transform t;
  if (n.is_atom()) {
    if (n.atom == "Identity") t.kind = Transform::Identity;
    else if (n.atom == "Simplex") t.kind = Transform::Simplex;
    else if (n.atom == "Ordered") t.kind = Transform::Ordered;
    else if (n.atom == "PositiveOrdered") t.kind = Transform::PositiveOrdered;
    else if (n.atom == "CholeskyCorr") t.kind = Transform::CholeskyCorr;
    else if (n.atom == "UnitVector") t.kind = Transform::UnitVector;
    else if (n.atom == "SumToZero") t.kind = Transform::SumToZero;
    else if (n.atom == "Correlation") t.kind = Transform::Correlation;
    else if (n.atom == "Covariance") t.kind = Transform::Covariance;
    else if (n.atom == "CholeskyCov") t.kind = Transform::CholeskyCov;
    else t.kind = Transform::Unsupported;
    t.raw = n.atom;
    return t;
  }
  const std::string& k = n[0].atom;
  if (k == "Lower") t.kind = Transform::Lower;
  else if (k == "Upper") t.kind = Transform::Upper;
  else if (k == "LowerUpper") t.kind = Transform::LowerUpper;
  else if (k == "Offset") t.kind = Transform::Offset;
  else if (k == "Multiplier") t.kind = Transform::Multiplier;
  else if (k == "OffsetMultiplier") t.kind = Transform::OffsetMultiplier;
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
    if (p.atom == "Skip") s.kind = Stmt::Skip;
    else s.raw = p.atom;
    return s;
  }
  const std::string& head = p[0].atom;
  if (head == "Decl") {
    s.kind = Stmt::Decl;
    if (const Node* id = field(p, "decl_id")) s.decl_id = (*id)[1].atom;
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
          if (t[1].atom == "UReal") s.decl_type.base = "SReal";
          else if (t[1].atom == "UInt") s.decl_type.base = "SInt";
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
      // FnWriteParam names its column in the payload, not in the (empty)
      // argument list: (FnWriteParam (unconstrain_opt ()) (var <expr>)).
      if (!internal.is_atom())
        if (const Node* v = field(internal, "var"))
          s.fn_args.push_back(read_expr((*v)[1]));
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
  return prog;
}

}  // namespace mir
}  // namespace stanli
