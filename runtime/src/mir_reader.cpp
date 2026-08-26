#include <stanli/mir.hpp>
#include <stanli/optable.hpp>

#include "mir_reader_internal.hpp"

#include <limits>
#include <map>
#include <stdexcept>

namespace stanli {
namespace mir {
namespace {

using sexp::Node;

[[noreturn]] void malformed(const std::string& what) {
  throw std::runtime_error("mir: malformed " + what);
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

  if (named(e, {"IndexAll"})) {
    require_arity(e, 0);
    return;
  }
  if (named(e, {"IndexSingle", "IndexMulti", "IndexUpfrom"})) {
    require_arity(e, 1);
    return;
  }
  if (named(e, {"IndexBetween"})) {
    require_arity(e, 2);
    return;
  }

  if (e.fn_lib == Expr::Lib::Internal &&
      (e.name == "FnReadData" || e.name == "FnLength")) {
    require_arity(e, 1);
    return;
  }

  if (named(e, {"pi", "e", "machine_precision", "negative_infinity",
                "positive_infinity"})) {
    require_arity(e, 0);
    return;
  }

// Keep these generated checks tied to the same registries that dispatch the
// native lowering and interpreter kernels.
#define STANLI_VALIDATE_UNARY(code, fn, value, delta, topology) \
  if (e.name == #fn) {                                          \
    require_arity(e, 1);                                        \
    return;                                                     \
  }
  STANLI_SCALAR_UNARY_LIST(STANLI_VALIDATE_UNARY)
#undef STANLI_VALIDATE_UNARY

#define STANLI_VALIDATE_BINARY(code, fn, impl) \
  if (e.name == #fn) {                         \
    require_arity(e, 2);                       \
    return;                                    \
  }
  STANLI_SCALAR_BINARY_LIST(STANLI_VALIDATE_BINARY)
  STANLI_SCALAR_BINARY_INT_FIRST_LIST(STANLI_VALIDATE_BINARY)
  STANLI_SCALAR_BINARY_INT_SECOND_LIST(STANLI_VALIDATE_BINARY)
#undef STANLI_VALIDATE_BINARY

#define STANLI_VALIDATE_DENSITY(code, fn, arity, tier) \
  if (e.name == #fn) {                                 \
    require_arity(e, arity);                           \
    return;                                            \
  }
  STANLI_SCALAR_DENSITY_LIST(STANLI_VALIDATE_DENSITY)
  STANLI_SCALAR_CDF_LIST(STANLI_VALIDATE_DENSITY)
  STANLI_TAIL_CDF_LIST(STANLI_VALIDATE_DENSITY)
#undef STANLI_VALIDATE_DENSITY

#define STANLI_VALIDATE_INT_DENSITY(code, fn, real_arity, tier) \
  if (e.name == #fn) {                                          \
    require_arity(e, (real_arity) + 1);                         \
    return;                                                     \
  }
  STANLI_INT_DENSITY_LIST(STANLI_VALIDATE_INT_DENSITY)
  STANLI_INT_CDF_LIST(STANLI_VALIDATE_INT_DENSITY)
  STANLI_TAIL_INT_CDF_LIST(STANLI_VALIDATE_INT_DENSITY)
  STANLI_ORDERED_DENSITY_LIST(STANLI_VALIDATE_INT_DENSITY)
#undef STANLI_VALIDATE_INT_DENSITY

#define STANLI_VALIDATE_TWO_INT_CDF(code, fn, real_arity, tier) \
  if (e.name == #fn) {                                          \
    require_arity(e, (real_arity) + 2);                         \
    return;                                                     \
  }
  STANLI_TWO_INT_CDF_LIST(STANLI_VALIDATE_TWO_INT_CDF)
#undef STANLI_VALIDATE_TWO_INT_CDF

  if (named(e, {"Plus__",
                "Minus__",
                "Times__",
                "LDivide__",
                "Divide__",
                "EltTimes__",
                "EltDivide__",
                "Pow__",
                "pow",
                "Modulo__",
                "IntDivide__",
                "Greater__",
                "Geq__",
                "Less__",
                "Leq__",
                "Equals__",
                "NEquals__",
                "add",
                "subtract",
                "multiply",
                "divide",
                "elt_multiply",
                "elt_divide",
                "multiply_log",
                "binomial_coefficient_log",
                "dot_product",
                "squared_distance",
                "diag_pre_multiply",
                "diag_post_multiply",
                "quad_form_diag",
                "append_row",
                "append_col",
                "rep_vector",
                "rep_row_vector",
                "col",
                "row",
                "head",
                "tail"})) {
    require_arity(e, 2);
    return;
  }
  if (named(e, {"PMinus__",
                "PPlus__",
                "PNot__",
                "minus",
                "plus",
                "exp",
                "log",
                "sqrt",
                "square",
                "inv",
                "fabs",
                "inv_logit",
                "logit",
                "log1m",
                "tanh",
                "cumulative_sum",
                "softmax",
                "log_softmax",
                "mean",
                "sd",
                "sum",
                "prod",
                "dot_self",
                "Transpose__",
                "transpose",
                "to_vector",
                "to_row_vector",
                "to_array_1d",
                "rows",
                "cols",
                "size",
                "num_elements",
                "dims",
                "multiply_lower_tri_self_transpose",
                "diag_matrix",
                "cholesky_decompose",
                "eigenvalues_sym",
                "eigenvectors_sym",
                "categorical_rng",
                "poisson_log_rng",
                "bernoulli_rng",
                "std_normal_qf",
                "trigamma",
                "is_nan",
                "tcrossprod"})) {
    require_arity(e, 1);
    return;
  }
  if (named(e, {"min", "max", "log_sum_exp"})) {
    require_arity(e, 1, 2);
    return;
  }
  if (e.name == "rep_matrix") {
    require_arity(e, 2, 3);
    return;
  }
  if (named(e, {"fma", "segment"})) {
    require_arity(e, 3);
    return;
  }
  if (e.name == "log_mix") {
    require_arity(e, 2, 3);
    return;
  }
  if (e.name == "gp_exp_quad_cov") {
    require_arity(e, 3, 4);
    return;
  }
  if (e.name == "to_matrix") {
    if (e.args.size() != 1 && e.args.size() != 3 && e.args.size() != 4)
      malformed("to_matrix call: expected 1, 3, or 4 argument(s), got " +
                std::to_string(e.args.size()));
    return;
  }
  if (e.name == "sub_col") {
    require_arity(e, 4);
    return;
  }
  if (e.name == "map_rect") {
    require_arity(e, 5);
    return;
  }
  if (e.name == "algebra_solver") {
    require_arity(e, 5, 8);
    return;
  }
  if (e.name == "log_diff_exp") {
    require_arity(e, 2);
    return;
  }
  if (named(e, {"multi_normal_rng", "uniform_rng", "normal_rng",
                "lognormal_rng", "binomial_rng"})) {
    require_arity(e, 2);
    return;
  }

  struct NamedArity {
    const char* name;
    size_t arity;
  };
  static constexpr NamedArity kManual[] = {
      {"bernoulli_lpmf", 2},
      {"bernoulli_logit_lpmf", 2},
      {"poisson_lpmf", 2},
      {"poisson_log_lpmf", 2},
      {"categorical_lpmf", 2},
      {"categorical_logit_lpmf", 2},
      {"dirichlet_lpdf", 2},
      {"lkj_corr_lpdf", 2},
      {"lkj_corr_cholesky_lpdf", 2},
      {"multinomial_lpmf", 2},
      {"multinomial_logit_lpmf", 2},
      {"dirichlet_multinomial_lpmf", 2},
      {"neg_binomial_2_lpmf", 3},
      {"binomial_lpmf", 3},
      {"binomial_logit_lpmf", 3},
      {"multi_normal_lpdf", 3},
      {"multi_normal_prec_lpdf", 3},
      {"multi_normal_cholesky_lpdf", 3},
      {"multi_gp_lpdf", 3},
      {"multi_gp_cholesky_lpdf", 3},
      {"ordered_probit_lpmf", 3},
      {"wishart_lpdf", 3},
      {"inv_wishart_lpdf", 3},
      {"wishart_cholesky_lpdf", 3},
      {"inv_wishart_cholesky_lpdf", 3},
      {"beta_binomial_lpmf", 4},
      {"poisson_log_glm_lpmf", 4},
      {"bernoulli_logit_glm_lpmf", 4},
      {"lkj_cov_lpdf", 4},
      {"multi_student_t_lpdf", 4},
      {"multi_student_t_cholesky_lpdf", 4},
      {"categorical_logit_glm_lpmf", 4},
      {"ordered_logistic_glm_lpmf", 4},
      {"hypergeometric_lpmf", 4},
      {"discrete_range_lpmf", 3},
      {"neg_binomial_2_log_glm_lpmf", 5},
      {"binomial_logit_glm_lpmf", 5},
      {"normal_id_glm_lpdf", 5},
      {"gaussian_dlm_obs_lpdf", 7},
  };
  for (const NamedArity& item : kManual)
    if (e.name == item.name) {
      require_arity(e, item.arity);
      return;
    }

  // The language has both the original five-parameter Wiener density and
  // its seven-parameter extension. The current execution kernels implement
  // only the five-argument form, but decoding must preserve either valid MIR
  // shape so the normal unsupported-function path can report that boundary.
  if (e.name == "wiener_lpdf") {
    require_arity(e, 5, 7);
    return;
  }

  const auto validate_transform_call = [&](const char* stem, size_t arity) {
    const std::string prefix(stem);
    if (e.name.compare(0, prefix.size(), prefix) != 0) return false;
    const std::string tail = e.name.substr(prefix.size());
    if (tail != "constrain" && tail != "jacobian" && tail != "unconstrain")
      return false;
    require_arity(e, arity);
    return true;
  };
  if (validate_transform_call("lower_bound_", 2) ||
      validate_transform_call("upper_bound_", 2) ||
      validate_transform_call("lower_upper_bound_", 3) ||
      validate_transform_call("offset_multiplier_", 3))
    return;

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
  } else if (!ix_n.is_atom() && ix_n.head_is("Upfrom")) {
    ix.kind = Expr::FunApp;
    ix.name = "IndexUpfrom";
    ix.args.push_back(read_expr(ix_n[1]));
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

void validate_expression_shape(const Expr& e);

std::string unsized_spelling(const UnsizedView& view) {
  std::string type;
  switch (view.leaf) {
    case UnsizedLeaf::Int:
      type = "UInt";
      break;
    case UnsizedLeaf::Real:
      type = "UReal";
      break;
    case UnsizedLeaf::Complex:
      type = "UComplex";
      break;
    case UnsizedLeaf::Vector:
      type = "UVector";
      break;
    case UnsizedLeaf::RowVector:
      type = "URowVector";
      break;
    case UnsizedLeaf::Matrix:
      type = "UMatrix";
      break;
    case UnsizedLeaf::Unknown:
      return {};
  }
  for (uint8_t depth = 0; depth < view.depth; ++depth)
    type = "(UArray " + type + ")";
  return type;
}

bool parse_unsized_spelling(std::string_view text, UnsizedView* out) {
  size_t depth = 0;
  constexpr std::string_view prefix = "(UArray ";
  while (text.size() > prefix.size() &&
         text.substr(0, prefix.size()) == prefix && text.back() == ')') {
    if (++depth > std::numeric_limits<uint8_t>::max()) return false;
    text.remove_prefix(prefix.size());
    text.remove_suffix(1);
  }
  UnsizedLeaf leaf = UnsizedLeaf::Unknown;
  if (text == "UInt")
    leaf = UnsizedLeaf::Int;
  else if (text == "UReal")
    leaf = UnsizedLeaf::Real;
  else if (text == "UComplex")
    leaf = UnsizedLeaf::Complex;
  else if (text == "UVector")
    leaf = UnsizedLeaf::Vector;
  else if (text == "URowVector")
    leaf = UnsizedLeaf::RowVector;
  else if (text == "UMatrix")
    leaf = UnsizedLeaf::Matrix;
  else
    return false;
  *out = UnsizedView{static_cast<uint8_t>(depth), leaf};
  return true;
}

void validate_expression_metadata(const Expr& e) {
  if (e.unsized.leaf == UnsizedLeaf::Unknown) {
    // Unknown upstream types use the raw/unsupported channel and clear the
    // compact type spelling. A recognized compact spelling paired with an
    // unknown structural view would let different consumers infer different
    // shapes.
    if (e.type_ == "UInt" || e.type_ == "UReal" || e.type_ == "UComplex" ||
        e.type_ == "UVector" || e.type_ == "URowVector" ||
        e.type_ == "UMatrix" || e.type_ == "UArray")
      malformed("expression type metadata");
    return;
  }
  const std::string expected = e.unsized.depth == 0
                                   ? unsized_spelling(e.unsized)
                                   : std::string("UArray");
  if (e.type_ != expected) malformed("expression type metadata");
}

void validate_index_shape(const Expr& index) {
  if (index.kind != Expr::FunApp || index.fn_lib != Expr::Lib::StanLib ||
      !named(index, {"IndexAll", "IndexSingle", "IndexBetween", "IndexMulti",
                     "IndexUpfrom"}))
    malformed("index descriptor");
  validate_expression_shape(index);
}

void validate_expression_shape(const Expr& e) {
  validate_expression_metadata(e);
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
      // Index consumers address each descriptor according to its synthetic
      // name when descriptors are present.
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
  if (statement.read_transform)
    validate_transform_shape(*statement.read_transform);
  if (statement.check_transform)
    validate_transform_shape(*statement.check_transform);

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
  for (const std::vector<Stmt>* body :
       {&program.prepare_data, &program.log_prob, &program.generate_quantities})
    for (const Stmt& statement : *body) validate_statement_shape(statement);
  for (const FunDef& function : program.fun_defs) {
    const size_t arity = function.arg_names.size();
    if (function.arg_types.size() != arity ||
        function.arg_views.size() != arity ||
        function.arg_data_only.size() != arity)
      malformed("function argument field lengths disagree");
    for (size_t i = 0; i < arity; ++i) {
      UnsizedView parsed;
      const bool recognized =
          parse_unsized_spelling(function.arg_types[i], &parsed);
      const UnsizedView& view = function.arg_views[i];
      if ((recognized &&
           (parsed.depth != view.depth || parsed.leaf != view.leaf)) ||
          (!recognized && view.leaf != UnsizedLeaf::Unknown))
        malformed("function argument type disagrees with its view");
    }
    for (const Stmt& statement : function.body)
      validate_statement_shape(statement);
  }
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
                       const Functions& functions,
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
      throw std::runtime_error(
          "mir: variable type disagrees with its binding for " + e.name);
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
  for (const Expr& arg : e.args)
    validate_expression(arg, bindings, functions, strict_variable_metadata);
}

void validate_bindings(const Stmt& s, Bindings& bindings,
                       const Functions& functions,
                       bool strict_variable_metadata) {
  for (const Expr* e :
       {&s.init, &s.rhs, &s.target, &s.lower, &s.upper, &s.cond})
    validate_expression(*e, bindings, functions, strict_variable_metadata);
  for (const auto* expressions : {&s.read_dims, &s.lhs_idx, &s.fn_args})
    for (const Expr& e : *expressions)
      validate_expression(e, bindings, functions, strict_variable_metadata);
  for (const Transform* transform :
       {s.read_transform ? &*s.read_transform : nullptr,
        s.check_transform ? &*s.check_transform : nullptr})
    if (transform)
      for (const Expr& e : transform->args)
        validate_expression(e, bindings, functions, strict_variable_metadata);
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
    validate_bindings(s.body, bindings, functions, strict_variable_metadata);
  } else if (s.kind == Stmt::IfElse) {
    for (const auto& child : s.body) {
      Bindings branch = bindings;
      validate_bindings(child, branch, functions, strict_variable_metadata);
    }
  } else if (!s.body.empty()) {
    Bindings nested = bindings;
    if (s.kind == Stmt::For)
      nested[s.loopvar] = Binding{UnsizedView{0, UnsizedLeaf::Int}, true};
    validate_bindings(s.body, nested, functions, strict_variable_metadata);
  }
}

void validate_bindings(const std::vector<Stmt>& body, Bindings& bindings,
                       const Functions& functions,
                       bool strict_variable_metadata) {
  for (const auto& s : body)
    validate_bindings(s, bindings, functions, strict_variable_metadata);
}

// stanc3 keeps every overload of a user function under one fdname, and calls
// carry only that name, so the by-name function maps downstream would
// collide (last definition wins). Give each overload a distinct internal
// name and rewrite every call site to the overload its argument types
// select. stanc3 already ran overload resolution and inserted promotions at
// typecheck time, so a call's (leaf, depth) views match exactly one
// signature; parenthesized signatures cannot collide with Stan identifiers.

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

void resolve_calls(Expr& e, const Overloads& overloads) {
  for (Expr& a : e.args) resolve_calls(a, overloads);
  if (e.kind != Expr::FunApp || e.fn_lib != Expr::Lib::UserDefined) return;
  const auto it = overloads.find(e.name);
  if (it == overloads.end()) return;
  const FunDef* match = nullptr;
  for (const FunDef* f : it->second) {
    if (!views_match(e.args, f->arg_views)) continue;
    if (match)
      throw std::runtime_error("mir: ambiguous overload for " + e.name);
    match = f;
  }
  if (!match)
    throw std::runtime_error("mir: no overload of " + it->first +
                             " matches its call");
  e.name = match->name;
}

void resolve_calls(Stmt& s, const Overloads& overloads) {
  for (Expr* e : {&s.init, &s.rhs, &s.target, &s.lower, &s.upper, &s.cond})
    resolve_calls(*e, overloads);
  for (auto* exprs : {&s.read_dims, &s.lhs_idx, &s.fn_args})
    for (Expr& e : *exprs) resolve_calls(e, overloads);
  for (Expr& e : s.decl_type.dims) resolve_calls(e, overloads);
  for (Transform* t : {s.read_transform ? &*s.read_transform : nullptr,
                       s.check_transform ? &*s.check_transform : nullptr})
    if (t)
      for (Expr& e : t->args) resolve_calls(e, overloads);
  for (Stmt& k : s.body) resolve_calls(k, overloads);
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
          throw std::runtime_error("mir: indistinguishable overloads of " +
                                   name);
    for (FunDef* f : defs) f->name += signature(f->arg_views);
    overloads[name] = std::move(defs);
  }
  if (overloads.empty()) return;
  for (auto& [name, type] : prog.input_vars)
    for (Expr& d : type.dims) resolve_calls(d, overloads);
  for (auto* body :
       {&prog.prepare_data, &prog.log_prob, &prog.generate_quantities})
    for (Stmt& s : *body) resolve_calls(s, overloads);
  for (FunDef& f : prog.fun_defs)
    for (Stmt& s : f.body) resolve_calls(s, overloads);
}

}  // namespace

void detail::validate_portable_program(const Program& prog) {
  validate_program_shape(prog);
}

void detail::finalize_program(Program& prog, bool strict_variable_metadata) {
  resolve_overloads(prog);
  Functions functions;
  for (const auto& f : prog.fun_defs) {
    const auto inserted = functions.emplace(f.name, &f);
    if (!inserted.second)
      throw std::runtime_error(
          "mir: duplicate function name after overload resolution: " + f.name);
  }
  Bindings inputs;
  for (const auto& [name, type] : prog.input_vars)
    inputs[name] = Binding{declared_view(type), true};
  Bindings prepare_bindings = inputs;
  validate_bindings(prog.prepare_data, prepare_bindings, functions,
                    strict_variable_metadata);
  Bindings log_prob_bindings = prepare_bindings;
  validate_bindings(prog.log_prob, log_prob_bindings, functions,
                    strict_variable_metadata);
  Bindings gq_bindings = prepare_bindings;
  validate_bindings(prog.generate_quantities, gq_bindings, functions,
                    strict_variable_metadata);
  for (const auto& f : prog.fun_defs) {
    Bindings args;
    for (size_t i = 0; i < f.arg_names.size(); ++i)
      args[f.arg_names[i]] = Binding{f.arg_views[i], f.arg_data_only[i]};
    validate_bindings(f.body, args, functions, strict_variable_metadata);
  }
}

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
  detail::finalize_program(prog);
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
