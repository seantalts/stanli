#include "lower_internal.hpp"

#include "build_id.hpp"

namespace stanli {
namespace lower_detail {

StructuredMode read_structured_mode() {
  const char* flag = std::getenv("STANLI_STRUCTURED_LOOPS");
  if (!flag || !*flag || std::string_view(flag) == "auto")
    return StructuredMode::Auto;
  if (std::string_view(flag) == "0") return StructuredMode::Off;
  if (std::string_view(flag) == "1") return StructuredMode::Prefer;
  if (std::string_view(flag) == "force") return StructuredMode::Force;
  // An unrecognized policy must preserve the established representation.
  return StructuredMode::Off;
}
Lowering::Lowering(const DataMap& d, PrepTrace& p, PassDumper& dump_to,
                   const char* graph_name, WaRng stream,
                   std::shared_ptr<ShapeInterner> pool)
    : data(d),
      shape_pool(std::move(pool)),
      prep(p),
      dumper(dump_to),
      prep_graph(graph_name),
      td_rng(std::move(stream)) {}
Lowering::Lowering(const DataMap& d, PrepTrace& p, PassDumper& dump_to,
                   const char* graph_name,
                   const PreparedContext& prepared_context)
    : Lowering(d, p, dump_to, graph_name, prepared_context.rng,
               prepared_context.shapes) {
  const auto env_copy_time = prep.start();
  td.env() = prepared_context.environment;
  int_env = prepared_context.integers;
  decls = prepared_context.declarations;
  prep.plain(prep_graph, "env_copy", env_copy_time);
}
Lowering::Lowering(Lowering& parent, RegionTrialTag)
    : Lowering(parent.data, parent.prep, parent.dumper, parent.prep_graph,
               parent.td_rng,
               std::make_shared<ShapeInterner>(*parent.shape_pool)) {
  fun_defs = parent.fun_defs;
  decls = parent.decls;
  td.env() = parent.td.env();
  int_env = parent.int_env;
  int_locals = parent.int_locals;
  propto_ctx = parent.propto_ctx;
  udf_depth = parent.udf_depth;
  udf_autodiff_ctx = parent.udf_autodiff_ctx;
  udf_formal_autodiff = parent.udf_formal_autodiff;
}
PreparedContext Lowering::prepared_context() {
  if (!data_prepared)
    throw std::logic_error("lowering has not prepared model data");
  return {td_rng, shape_pool, td.env(), int_env_data, decls};
}
Lowering Lowering::fork_region_trial() {
  return Lowering(*this, RegionTrialTag{});
}
void Lowering::dump_named(const std::string& label, const std::string& name,
                          const std::vector<int>& roots, bool unfiltered) {
  GraphPrintInfo info;
  info.roots = roots;
  info.target_terms = target_terms;
  info.jac_slots = jac_slots;
  for (const CompiledModel::ParamView& v : out.views)
    info.views.emplace_back(v.name, v.slot);
  info.fills = &out.fills;
  std::string text;
  print_graph(text, g, info);
  dumper.write(label, name, text, unfiltered);
}
void Lowering::sync_data_local(const std::string& name, const mir::Expr& rhs,
                               const Val& v) {
  if (!v.si.param_free) {
    td.env().erase(name);
    return;
  }
  if (const DataMap::Entry* en = observation(v)) {
    td.env()[name] = *en;
    return;
  }
  // Evaluate before erasing the old binding: `x = x + data_step` reads the
  // previous x, and data-only while loops depend on retaining that value for
  // their next condition.
  auto evaluated = try_eval_pure(rhs);
  td.env().erase(name);
  if (evaluated) {
    DataMap::Entry en = std::move(*evaluated);
    td.env()[name] = en;
    observe(v, std::move(en));
  }
}
void Lowering::observe_indexed_rhs(const mir::Expr& rhs, const Val& v) {
  if (observation(v) || !v.si.param_free) return;
  if (auto evaluated = try_eval_pure(rhs)) {
    observe(v, std::move(*evaluated));
    return;
  }
  if (rhs.type_ != "UInt" || g.slots[v.slot].len != 1) return;
  try {
    const long value = eval_int(rhs);
    DataMap::Entry en;
    en.is_int = true;
    en.i = {static_cast<int>(value)};
    en.r = {static_cast<double>(value)};
    observe(v, std::move(en));
  } catch (const CompileError&) {
    // Observation is an optimization. Runtime integer expressions remain
    // graph values and deliberately do not acquire a compile-time binding.
  }
}
// CmdStan's var_context validates every declared dimension against the
// supplied values before it reads one, and throws std::invalid_argument
// naming the variable and both shapes. Without the same check the short
// side is read past its end, and a host that tells bad data from a
// broken model by the exception type sees the wrong answer. Only the
// element count is compared: JSON carries a nested shape but stanc has
// already flattened the read, and a declaration whose extents multiply
// out to the supplied count is the shape the reader would produce.
void Lowering::validate_data_dims(const std::string& name,
                                  const mir::SizedType& t) {
  if (!data.has(name)) return;
  const DataMap::Entry& en = data.at(name);
  // A declared-int variable must arrive integer-typed, as CmdStan's
  // var_context requires (JSON 1.0 is not an int there either). Without
  // this the entry binds as typeless reals and the failure surfaces at
  // whatever consumer touches it first, e.g. the gather index guard.
  if ((t.base == "SInt" || (t.base == "SArray" && t.elem_base == "SInt")) &&
      !en.is_int)
    throw std::invalid_argument(
        "int variable contained non-int values; processing stage=data "
        "initialization; variable name=" +
        name + "; base type=int");
  const int64_t found = (int64_t)std::max(en.r.size(), en.i.size());
  const std::vector<int64_t> declared = sized_dims(t);
  int64_t want = 1;
  for (int64_t d : declared) {
    if (d < 0) fail("negative extent for data " + name, t.raw);
    want *= d;
  }
  if (want == found) return;
  const auto tuple = [](const std::vector<int64_t>& dims) {
    std::string s = "(";
    for (size_t k = 0; k < dims.size(); ++k) {
      if (k) s += ',';
      s += std::to_string(dims[k]);
    }
    return s + ")";
  };
  throw std::invalid_argument(
      "mismatch in dimension declared and found in context; processing "
      "stage=data initialization; variable name=" +
      name + "; position=0; dims declared=" + tuple(declared) +
      "; dims found=" +
      tuple(en.dims.empty() ? std::vector<int64_t>{found} : en.dims));
}
void Lowering::scan_rebuild(const mir::Stmt& s, RebuildShape& shape) {
  switch (s.kind) {
    case mir::Stmt::Block:
    case mir::Stmt::SList:
      for (const auto& k : s.body) scan_rebuild(k, shape);
      return;
    case mir::Stmt::For: {
      std::set<std::string> bounds_reads;
      data_reads(s.lower, bounds_reads);
      data_reads(s.upper, bounds_reads);
      if (!bounds_reads.empty()) shape.supported = false;
      for (const auto& k : s.body) scan_rebuild(k, shape);
      return;
    }
    case mir::Stmt::Decl: {
      shape.decls.insert(s.decl_id);
      if (!s.has_init) return;
      std::set<std::string> reads;
      data_reads(s.init, reads);
      shape.reads.insert(reads.begin(), reads.end());
      if (!reads.empty()) {
        ++shape.loaders;
        shape.loader_lhs = s.decl_id;
      }
      return;
    }
    case mir::Stmt::Assignment: {
      shape.writes.insert(s.lhs);
      std::set<std::string> reads;
      data_reads(s.rhs, reads);
      for (const auto& ix : s.lhs_idx) data_reads(ix, reads);
      shape.reads.insert(reads.begin(), reads.end());
      if (!reads.empty()) {
        if (!s.lhs_idx.empty()) shape.supported = false;
        ++shape.loaders;
        shape.loader_lhs = s.lhs;
      }
      return;
    }
    default:
      // A generated input rebuild has no effects, conditionals, target
      // writes, validation calls, or returns. New statement kinds fall back
      // to interpretation rather than guessing which children are safe.
      shape.supported = false;
      return;
  }
}
bool Lowering::canonical_input_rebuild(const mir::Stmt& s,
                                       const std::set<std::string>& inputs) {
  if (s.kind != mir::Stmt::Block && s.kind != mir::Stmt::SList) return false;
  RebuildShape shape;
  scan_rebuild(s, shape);
  if (!shape.supported || shape.loaders != 1 || shape.reads.size() != 1)
    return false;
  const std::string& input = *shape.reads.begin();
  if (!inputs.count(input) || shape.loader_lhs.empty() ||
      shape.loader_lhs == input || !shape.decls.count(shape.loader_lhs) ||
      !shape.writes.count(input))
    return false;
  const auto allowed = [&](const std::string& name) {
    return name == input || name == shape.loader_lhs || name == "pos__";
  };
  for (const auto& name : shape.decls)
    if (!allowed(name)) return false;
  for (const auto& name : shape.writes)
    if (!allowed(name)) return false;
  return true;
}
void Lowering::bind_data(const mir::Program& p) {
  std::set<std::string> input_names;
  bool all_inputs_bound = true;
  bool use_prebound = std::getenv("STANLI_NO_DATA_PRELOAD") == nullptr;
  for (const auto& [name, type] : p.input_vars) {
    input_names.insert(name);
    if (!data.has(name)) {
      all_inputs_bound = false;
      continue;
    }
    // DataMap does not have the Stan schema, so JSON values spelled with
    // integer tokens carry an int mirror even when the declaration is
    // real. Reconstruct the typed value directly, without copying an
    // irrelevant mirror for a large real matrix.
    const DataMap::Entry& src = data.at(name);
    if (!use_prebound) {
      td.env()[name] = src;
      continue;
    }
    DataMap::Entry dst;
    const bool want_int = type.base == "SInt" ||
                          (type.base == "SArray" && type.elem_base == "SInt");
    dst.is_int = want_int;
    dst.r = src.r;
    dst.dims = src.dims;
    if (want_int) {
      if (!src.i.empty()) {
        dst.i = src.i;
      } else if (!src.r.empty()) {
        // Preserve the interpreter's existing error/coercion behavior for
        // malformed data instead of silently truncating real values here.
        use_prebound = false;
      }
    }
    td.env()[name] = std::move(dst);
  }
  use_prebound = use_prebound && all_inputs_bound;
  if (use_prebound) {
    // The generated reconstruction allocated the MIR-declared shape and
    // copied exactly that many flat elements. Normalize to the same shape;
    // a malformed length falls back to that checked interpreter path.
    for (const auto& [name, type] : p.input_vars) {
      DataMap::Entry& dst = td.env().at(name);
      if (static_cast<int64_t>(dst.r.size()) != sized_len(type)) {
        use_prebound = false;
        break;
      }
      dst.dims.clear();
      if (type.base != "SInt" && type.base != "SReal")
        for (const auto& d : type.dims) dst.dims.push_back(eval_int(d));
    }
  }
  if (use_prebound) {
    // Skipping the generated declarations also skips MirInterp's normal
    // declaration-geometry bookkeeping. Preserve it explicitly so checks
    // on an empty outer array still see its trailing vector/matrix extents,
    // which JSON [] cannot represent.
    for (const auto& input : p.input_vars) {
      const std::string& name = input.first;
      td.set_declared_dims(name, td.env().at(name).dims);
    }
  }
  auto record = [&](const std::string& name, const mir::SizedType& type) {
    if (type.base == "SInt") return;
    DeclView sh;
    sh.len = sized_len(type);
    sh.si = view_of(type, true);
    decls[name] = sh;
  };
  for (const auto& [name, type] : p.input_vars) {
    record(name, type);
    validate_data_dims(name, type);
  }
  for (const auto& st : p.prepare_data) {
    if (st.kind == mir::Stmt::Decl) record(st.decl_id, st.decl_type);
    // stanc's prepare_data first rebuilds every input from a flat
    // FnReadData buffer. DataMap has already parsed that buffer into the
    // same typed, column-major representation above. Replaying the
    // canonical matrix reconstruction means one interpreted assignment
    // per element (47 million for nn_rbm1bJ100) and used to dominate model
    // preparation. FnReadData is compiler-internal and cannot occur in
    // source transformed-data code, so a top-level statement containing it
    // is input hydration, not user computation.
    if (use_prebound &&
        ((st.kind == mir::Stmt::Decl && input_names.count(st.decl_id)) ||
         direct_input_load(st, input_names) ||
         canonical_input_rebuild(st, input_names)))
      continue;
    td.exec(st);
  }
  for (auto& [name, e] : td.env()) {
    if (e.is_int && e.i.size() == 1 && e.dims.empty()) int_env[name] = e.i[0];
  }
  int_env_data = int_env;
  data_prepared = true;
}
// ---- statements -----------------------------------------------------------
CompiledModel::ParamView Lowering::parameter_view(const mir::Stmt& s, int slot,
                                                  int64_t len) {
  using Naming = CompiledModel::ParamView::Naming;
  CompiledModel::ParamView view{s.decl_id, slot, len};
  if (s.decl_type.base == "SReal" || s.decl_type.base == "SInt" ||
      s.decl_type.base == "SComplex") {
    view.naming = Naming::Scalar;
    return view;
  }
  view.naming = Naming::Container;
  std::vector<int64_t> dims;
  for (const auto& dim : s.decl_type.dims) dims.push_back(eval_int(dim));
  int64_t declared_len = 1;
  for (int64_t dim : dims) declared_len *= dim;
  if (declared_len != len)
    fail("constrained shape does not match its flattened length", s.raw);
  const bool matrix_storage =
      s.decl_type.base == "SMatrix" ||
      (s.decl_type.base == "SArray" && s.decl_type.elem_base == "SMatrix");
  view.set_serial_layout(std::move(dims), matrix_storage);
  if (matrix_storage) {
    view.naming = Naming::Matrix;
    view.rows = view.dims.at(view.dims.size() - 2);
  }
  return view;
}
void Lowering::lower_read_param(const mir::Stmt& s) {
  const mir::Transform& tr = *s.read_transform;
  const std::vector<int64_t> declared_dims = sized_dims(s.decl_type);
  const ViewKind leaf = s.decl_type.base == "SArray"
                            ? leaf_kind(s.decl_type.elem_base)
                            : leaf_kind(s.decl_type.base);
  const size_t leaf_dims = static_cast<size_t>(leaf_rank(leaf));
  if (declared_dims.size() < leaf_dims)
    fail("parameter declaration has incomplete leaf dimensions", s.raw);
  const std::vector<int64_t> outer_dims(declared_dims.begin(),
                                        declared_dims.end() - leaf_dims);
  const int64_t n_batch = checked_product(outer_dims, "parameter batch");

  // Unstructured transforms use FnReadParam's declared raw shape. A
  // structured leaf replaces only its innermost dimensions below; the
  // outer array geometry remains declaration-owned and orthogonal.
  std::vector<int64_t> raw_dims;
  for (const auto& d : s.read_dims) raw_dims.push_back(eval_int(d));
  std::vector<int64_t> expected_read_dims = declared_dims;
  if (tr.kind == mir::Transform::CholeskyCorr ||
      tr.kind == mir::Transform::Correlation ||
      tr.kind == mir::Transform::Covariance) {
    if (leaf != ViewKind::Matrix || expected_read_dims.size() < 2)
      fail("matrix parameter transform has a non-matrix declaration", s.raw);
    expected_read_dims.pop_back();
  }
  if (raw_dims != expected_read_dims)
    fail("parameter read dimensions do not match its declaration", s.raw);
  int64_t con_len = checked_product(raw_dims, "parameter read shape");
  int64_t raw_len = con_len;
  int64_t inner_raw = 0, inner_con = 0;
  int64_t matrix_rows = 0, matrix_cols = 0;

  auto vector_leaf = [&](int64_t free_size) {
    if (leaf != ViewKind::Vector || declared_dims.empty())
      fail("vector parameter transform has a non-vector declaration", s.raw);
    inner_raw = free_size;
    inner_con = declared_dims.back();
    raw_dims = outer_dims;
    raw_dims.push_back(inner_raw);
    raw_len = checked_product({n_batch, inner_raw}, "parameter raw shape");
    con_len =
        checked_product({n_batch, inner_con}, "parameter constrained shape");
  };
  auto flat_matrix_leaf = [&](int64_t free_size) {
    if (leaf != ViewKind::Matrix || declared_dims.size() < 2)
      fail("matrix parameter transform has a non-matrix declaration", s.raw);
    matrix_rows = declared_dims[declared_dims.size() - 2];
    matrix_cols = declared_dims.back();
    inner_raw = free_size;
    inner_con =
        checked_product({matrix_rows, matrix_cols}, "parameter matrix leaf");
    raw_dims = outer_dims;
    raw_dims.push_back(inner_raw);
    raw_len = checked_product({n_batch, inner_raw}, "parameter raw shape");
    con_len =
        checked_product({n_batch, inner_con}, "parameter constrained shape");
  };

  if (tr.kind == mir::Transform::Simplex ||
      tr.kind == mir::Transform::SumToZero) {
    if (leaf == ViewKind::Vector) {
      vector_leaf(declared_dims.back() - 1);
    } else if (tr.kind == mir::Transform::SumToZero &&
               leaf == ViewKind::Matrix) {
      matrix_rows = declared_dims[declared_dims.size() - 2];
      matrix_cols = declared_dims.back();
      inner_raw = checked_product({matrix_rows - 1, matrix_cols - 1},
                                  "sum-to-zero matrix raw shape");
      inner_con = checked_product({matrix_rows, matrix_cols},
                                  "sum-to-zero matrix leaf");
      raw_dims = outer_dims;
      raw_dims.push_back(matrix_rows - 1);
      raw_dims.push_back(matrix_cols - 1);
      raw_len = checked_product({n_batch, inner_raw}, "parameter raw shape");
      con_len =
          checked_product({n_batch, inner_con}, "parameter constrained shape");
    } else {
      fail("sum-to-zero or simplex transform has an invalid declaration",
           s.raw);
    }
  } else if (tr.kind == mir::Transform::Ordered ||
             tr.kind == mir::Transform::PositiveOrdered ||
             tr.kind == mir::Transform::UnitVector) {
    if (leaf != ViewKind::Vector || declared_dims.empty())
      fail("vector parameter transform has a non-vector declaration", s.raw);
    vector_leaf(declared_dims.back());
  } else if (tr.kind == mir::Transform::CholeskyCorr ||
             tr.kind == mir::Transform::Correlation ||
             tr.kind == mir::Transform::Covariance) {
    if (leaf != ViewKind::Matrix || declared_dims.size() < 2)
      fail("matrix parameter transform has a non-matrix declaration", s.raw);
    const int64_t K = declared_dims.back();
    if (declared_dims[declared_dims.size() - 2] != K)
      fail("square matrix transform has a rectangular declaration", s.raw);
    const int64_t free_size = tr.kind == mir::Transform::Covariance
                                  ? K * (K + 1) / 2
                                  : K * (K - 1) / 2;
    flat_matrix_leaf(free_size);
  } else if (tr.kind == mir::Transform::CholeskyCov) {
    if (leaf != ViewKind::Matrix || declared_dims.size() < 2)
      fail("matrix parameter transform has a non-matrix declaration", s.raw);
    const int64_t M = declared_dims[declared_dims.size() - 2];
    const int64_t N = declared_dims.back();
    if (M < N) fail("cholesky-factor-cov rows are smaller than columns", s.raw);
    flat_matrix_leaf(N * (N + 1) / 2 + (M - N) * N);
  }

  SlotInfo psi = view_of(s.decl_type);
  const int64_t declared_len = sized_len(s.decl_type);
  if (declared_len != con_len)
    fail("parameter view length does not match constrained storage", s.raw);
  validate_view(psi, con_len, "parameter " + s.decl_id);
  decls[s.decl_id] = DeclView{con_len, scalar_autodiff(), psi};
  const int raw = add_slot(raw_len, /*is_param=*/true);
  out.param_names.push_back(s.decl_id);
  {
    // The unconstrained layout the caller needs to slice a draw: how
    // long this parameter's piece is, and what it means. raw_len and
    // con_len part company for every structured transform.
    CompiledModel::UncParam u;
    u.name = s.decl_id;
    u.len = raw_len;
    u.dims = raw_dims;
    u.transform = tr.kind;
    out.unc_params.push_back(std::move(u));
  }
  out.n_unconstrained += raw_len;

  if (tr.kind == mir::Transform::Identity) {
    Val value{raw, scalar_autodiff(), psi, owning_layout(psi)};
    CompiledModel::ParamView serial_view = parameter_view(s, raw, raw_len);
    scope[s.decl_id] = value;
    // In write_array mode the column order is dictated by the FnWriteParam
    // statements, which come later and cover transformed parameters and
    // generated quantities too; declaration order would be wrong.
    if (!in_write_array) {
      serial_view.slot = value.slot;
      out.views.push_back(std::move(serial_view));
    }
    return;
  }
  uint16_t opcode = 0;
  std::vector<int> ins{raw};
  switch (tr.kind) {
    case mir::Transform::Lower:
      opcode = OP_CONSTRAIN_LOWER;
      ins.push_back(lower_expr(tr.args[0]).slot);
      break;
    case mir::Transform::Upper:
      opcode = OP_CONSTRAIN_UPPER;
      ins.push_back(lower_expr(tr.args[0]).slot);
      break;
    case mir::Transform::LowerUpper:
      opcode = OP_CONSTRAIN_LU;
      ins.push_back(lower_expr(tr.args[0]).slot);
      ins.push_back(lower_expr(tr.args[1]).slot);
      break;
    case mir::Transform::CholeskyCorr:
      opcode = OP_CONSTRAIN_CHOL_CORR;
      break;
    case mir::Transform::Simplex:
      opcode = OP_CONSTRAIN_SIMPLEX;
      break;
    case mir::Transform::Ordered:
      opcode = OP_CONSTRAIN_ORDERED;
      break;
    case mir::Transform::PositiveOrdered:
      opcode = OP_CONSTRAIN_POS_ORDERED;
      break;
    // offset / multiplier: the affine transform, and the modern
    // non-centering idiom. stanc3 emits three tags depending on which
    // halves were written, so the missing half becomes its identity
    // (offset 0, multiplier 1) and one kernel serves all three.
    case mir::Transform::Offset:
    case mir::Transform::Multiplier:
    case mir::Transform::OffsetMultiplier: {
      opcode = OP_CONSTRAIN_OFFSET_MULT;
      const int zero = const_slot(0.0);
      const int one = const_slot(1.0);
      if (tr.kind == mir::Transform::Offset) {
        ins.push_back(lower_expr(tr.args[0]).slot);
        ins.push_back(one);
      } else if (tr.kind == mir::Transform::Multiplier) {
        ins.push_back(zero);
        ins.push_back(lower_expr(tr.args[0]).slot);
      } else {
        ins.push_back(lower_expr(tr.args[0]).slot);
        ins.push_back(lower_expr(tr.args[1]).slot);
      }
      break;
    }
    case mir::Transform::UnitVector:
      opcode = OP_CONSTRAIN_UNIT_VECTOR;
      break;
    case mir::Transform::SumToZero:
      opcode = leaf == ViewKind::Matrix ? OP_CONSTRAIN_SUM_TO_ZERO_MAT
                                        : OP_CONSTRAIN_SUM_TO_ZERO;
      break;
    case mir::Transform::Correlation:
      opcode = OP_CONSTRAIN_CORR_MATRIX;
      break;
    case mir::Transform::Covariance:
      opcode = OP_CONSTRAIN_COV_MATRIX;
      break;
    case mir::Transform::CholeskyCov:
      opcode = OP_CONSTRAIN_CHOL_COV;
      break;
    default:
      fail("unsupported parameter transform", tr.raw);
  }
  const int jac = add_slot(1, false);
  std::vector<int> tr_idata;
  if (opcode == OP_CONSTRAIN_SIMPLEX || opcode == OP_CONSTRAIN_ORDERED ||
      opcode == OP_CONSTRAIN_POS_ORDERED ||
      opcode == OP_CONSTRAIN_UNIT_VECTOR || opcode == OP_CONSTRAIN_SUM_TO_ZERO)
    tr_idata = {checked_immediate(n_batch, "structured parameter batch"),
                checked_immediate(inner_raw, "structured raw leaf"),
                checked_immediate(inner_con, "structured constrained leaf")};
  if (opcode == OP_CONSTRAIN_CHOL_CORR || opcode == OP_CONSTRAIN_CORR_MATRIX ||
      opcode == OP_CONSTRAIN_COV_MATRIX || opcode == OP_CONSTRAIN_CHOL_COV)
    tr_idata = {checked_immediate(n_batch, "structured parameter batch"),
                checked_immediate(inner_raw, "structured raw leaf"),
                checked_immediate(matrix_rows, "structured matrix rows"),
                checked_immediate(matrix_cols, "structured matrix columns")};
  if (opcode == OP_CONSTRAIN_SUM_TO_ZERO_MAT) {
    tr_idata = {checked_immediate(n_batch, "structured parameter batch"),
                checked_immediate(inner_raw, "structured raw leaf"),
                checked_immediate(matrix_rows, "structured matrix rows"),
                checked_immediate(matrix_cols, "structured matrix columns")};
  }
  Val con =
      emit_raw(opcode, ins, con_len, psi, tr_idata, jac, scalar_autodiff());
  con.layout = owning_layout(psi);
  jac_slots.push_back(jac);
  scope[s.decl_id] = con;
  if (!in_write_array)
    out.views.push_back(parameter_view(s, con.slot, con_len));
}
namespace {

// The grouping loop behind reduce_terms: chunks of up to 6 terms fold
// through one ADD_N each until one remains. emit_chunk does the actual op
// emission.
template <typename EmitChunk>
int reduce_terms_grouped(std::vector<int> terms, EmitChunk emit_chunk) {
  while (terms.size() > 1) {
    std::vector<int> next;
    for (size_t i = 0; i < terms.size(); i += 6) {
      const size_t n = std::min<size_t>(6, terms.size() - i);
      if (n == 1) {
        next.push_back(terms[i]);
        continue;
      }
      std::vector<int> chunk(terms.begin() + i, terms.begin() + i + n);
      next.push_back(emit_chunk(chunk));
    }
    terms = std::move(next);
  }
  return terms.empty() ? -1 : terms[0];
}

}  // namespace

// Scalar terms reduce through chained ADD_N ops (6-input limit per op).
int Lowering::reduce_terms(std::vector<int> terms) {
  // The target is a scalar, and every consumer of a term reads one value
  // from it. A container term is therefore not a shape to accommodate but
  // a lowering bug -- one whose symptom, before this check, was a model
  // that sampled a wrong posterior without saying anything.
  for (int t : terms)
    if (g.slots[t].len != 1) fail("target term is not a scalar");
  if (terms.empty()) return const_slot(0.0);
  return reduce_terms_grouped(std::move(terms),
                              [&](const std::vector<int>& chunk) {
                                return emit_raw(OP_ADD_N, chunk, 1, {}).slot;
                              });
}

// Shared tail of both lowerings: inplace/store-forward/reroll always run;
// the rest is gated by plan so write_array can skip the passes that assume
// a scalar log-density result. Ordering constraints between the stages
// that do run are noted where each stage starts.
void Lowering::run_passes(const std::vector<int>& roots, const PassPlan& plan) {
  const auto trace = [&](const char* stage, PrepTrace::Time from,
                         const std::vector<int>& stage_roots,
                         PrepTrace::Extra extra = PrepTrace::Extra::None,
                         int64_t a = 0, int64_t b = 0, bool deep = false,
                         int64_t params = 0, int64_t c = 0, int64_t d = 0,
                         const detail::RerollDispositionStats* disp = nullptr) {
    prep.graph(prep_graph, stage, from, g, out.fills, target_terms.size(),
               out.views.size(), extra, a, b, deep, params, c, d, disp);
    dump(stage, stage_roots);
  };

  // Target terms have no consuming op yet either: reduce_terms (log_prob)
  // or the arena reads (write_array) see them only after the passes run.
  std::vector<int> update_roots = roots;
  update_roots.insert(update_roots.end(), target_terms.begin(),
                      target_terms.end());
  const auto inplace_time = prep.start();
  const int inplace =
      make_inplace_updates(g, update_roots);  // off under STANLI_NO_INPLACE
  trace("inplace", inplace_time, update_roots, PrepTrace::Extra::Rewrites,
        inplace);
  // Deleting the write/read-back pairs first is what leaves a plain
  // arithmetic lane for reroll to vectorize.
  const auto forward_time = prep.start();
  const int forwarded = forward_stores_to_loads(g, update_roots);
  trace("store_forward", forward_time, update_roots, PrepTrace::Extra::Removed,
        forwarded);
  if (plan.constfold) {
    // After the update chains collapse, so a data-only chain is one slot
    // rather than N; before reroll, so the lanes it sees have data
    // operands.
    const auto constfold_time = prep.start();
    const ConstFoldStats constfolded = const_fold(g, out.fills, update_roots);
    trace("constfold", constfold_time, update_roots,
          PrepTrace::Extra::ConstFold, constfolded.ops_removed,
          constfolded.slots_folded);
  }
  const auto reroll_time = prep.start();
  RerollStats rerolled;
  detail::RerollDispositionStats reroll_dispositions;
  if (prep.enabled()) {
    detail::ProfiledRerollStats profiled =
        detail::reroll_profiled(g, out.fills, target_terms, roots);
    rerolled = profiled.work;
    reroll_dispositions = profiled.dispositions;
  } else {
    rerolled = reroll(g, out.fills, target_terms, roots);  // STANLI_NO_REROLL
  }
  trace("reroll", reroll_time, roots, PrepTrace::Extra::Reroll,
        rerolled.regions, rerolled.list_steps, false, 0,
        rerolled.candidate_steps, rerolled.row_steps, &reroll_dispositions);
  // Re-roll can replace many element writes with copying slice stores, and
  // may have replaced target terms with vector reductions: rebuild the
  // implicit-root set before giving those new ops the same last-use proof
  // as the scalar stores.
  std::vector<int> post_reroll_roots = roots;
  post_reroll_roots.insert(post_reroll_roots.end(), target_terms.begin(),
                           target_terms.end());
  const auto post_reroll_inplace_time = prep.start();
  const int post_reroll_inplace =
      rerolled.regions ? make_inplace_updates(g, post_reroll_roots) : 0;
  trace("post_reroll_inplace", post_reroll_inplace_time, post_reroll_roots,
        PrepTrace::Extra::Rewrites, post_reroll_inplace);
  std::vector<int> current_roots = post_reroll_roots;
  if (plan.partition) {
    // After re-roll, which keeps first crack at the contiguous shapes it
    // already handles, and before CSE, which would merge ops shared
    // between lanes and leave the lanes no longer whole.
    const auto partition_time = prep.start();
    const PartitionStats parted =
        partition_lanes(g, out.fills, target_terms, roots);
    trace("partition", partition_time, roots, PrepTrace::Extra::Partition,
          parted.groups, parted.lanes, false, 0, parted.declined,
          parted.list_steps);
    // Same proof the slice stores re-roll makes get: rebuilt from the
    // terms partition just replaced.
    std::vector<int> post_partition_roots = roots;
    post_partition_roots.insert(post_partition_roots.end(),
                                target_terms.begin(), target_terms.end());
    const auto post_partition_inplace_time = prep.start();
    const int post_partition_inplace =
        parted.groups ? make_inplace_updates(g, post_partition_roots) : 0;
    trace("post_partition_inplace", post_partition_inplace_time,
          post_partition_roots, PrepTrace::Extra::Rewrites,
          post_partition_inplace);
    current_roots = post_partition_roots;
  }
  if (plan.elide_stores) {
    // After every pass that emits a slice store, and before islands,
    // whose bodies name outer slots in a payload this rename cannot
    // reach.
    const auto elide_time = prep.start();
    const int elided = elide_full_extent_stores(g, current_roots);
    trace("elide_stores", elide_time, current_roots, PrepTrace::Extra::Removed,
          elided);
  }
  if (plan.cse) {
    // After reroll, whose lane matching needs the repeated ops it hoists
    // to still be there, and before islands, so they compile the smaller
    // residue.
    const auto cse_time = prep.start();
    const CseStats cse_st = cse(g, out.fills, target_terms, roots);
    trace("cse", cse_time, roots, PrepTrace::Extra::Removed,
          cse_st.ops_removed);
  }
  if (plan.island) {
    // LAST, after every other pass has had first crack: compile whatever
    // scalar residue survives (recurrences the re-roll can never widen)
    // into island ops. Off under STANLI_NO_ISLAND.
    const auto island_time = prep.start();
    const int islands = carve_islands(g, out.fills, target_terms, roots);
    trace("island", island_time, roots, PrepTrace::Extra::Regions, islands);
  }
}
CompiledModel::WriteArray Lowering::run_write_array(const mir::Program& p) {
  const auto total_time = prep.start();
  for (const auto& f : p.fun_defs) fun_defs[f.name] = &f;
  in_write_array = true;
  output_vars = &p.output_vars;
  // stanc3 guards the two emission groups on these flags; the sampler wants
  // both, so pin them and let the data-only IfElse fold them away.
  int_env["emit_transformed_parameters__"] = 1;
  int_env["emit_generated_quantities__"] = 1;
  CompiledModel::WriteArray wa;
  DumpOnThrow guard{*this};
  const auto lower_time = prep.start();
  try {
    for (const auto& s : p.generate_quantities) lower_stmt(s);
  } catch (const CompileError& e) {
    // Keep the valid prefix for diagnostics, but drivers select WaInterp
    // whenever this marker is set and it evaluates the whole section from
    // statement zero. There is no continuation frame for an arbitrary
    // nested failure or its lexical live-outs.
    wa.truncated = e.what();
  }
  std::vector<int> roots = jac_slots;
  for (const auto& v : out.views) roots.push_back(v.slot);
  roots.insert(roots.end(), extra_roots.begin(), extra_roots.end());
  prep.graph(prep_graph, "lower", lower_time, g, out.fills, target_terms.size(),
             out.views.size(), PrepTrace::Extra::Truncated,
             !wa.truncated.empty());
  dump("lower", roots);

  run_passes(roots, PassPlan{true, false, false, true, false});

  const auto finalize_time = prep.start();
  // Nothing reads a result here, but forward() asserts a scalar result
  // slot, so point it at one.
  g.result_slot = const_slot(0.0);
  wa.n_unconstrained = out.n_unconstrained;
  prep.graph(prep_graph, "finalize", finalize_time, g, out.fills,
             target_terms.size(), out.views.size());
  dump("finalize", roots);
  prep.graph(prep_graph, "total", total_time, g, out.fills, target_terms.size(),
             out.views.size(), PrepTrace::Extra::None, 0, 0, true,
             out.n_unconstrained);
  guard.done = true;
  wa.graph = std::move(g);
  // A section stanc did not emit a guard for (or one lowering stopped
  // short of) has no columns of its own: it starts where the CSV ends.
  // The transformed-parameter boundary falls back to the generated
  // quantities one rather than to the end, so a missing first guard
  // cannot order the two backwards and hand a reader a negative count.
  wa.n_gq_start = n_gq_start.value_or(out.views.size());
  wa.n_tp_start = n_tp_start.value_or(wa.n_gq_start);
  wa.columns = std::move(out.views);
  wa.fills = std::move(out.fills);
  return wa;
}
CompiledModel Lowering::run(const mir::Program& p) {
  DumpOnThrow guard{*this};
  const auto total_time = prep.start();
  for (const auto& f : p.fun_defs) fun_defs[f.name] = &f;
  const auto bind_time = prep.start();
  bind_data(p);
  prep.graph(prep_graph, "bind_data", bind_time, g, out.fills,
             target_terms.size(), out.views.size());
  dump("bind_data", {});
  const auto lower_time = prep.start();
  const char* symbolic_lanes = std::getenv("STANLI_SYMBOLIC_LANES");
  if (!symbolic_lanes || std::string_view(symbolic_lanes) == "1") {
    const auto terminal =
        [&](const auto& self,
            const std::vector<mir::Stmt>& body) -> const mir::Stmt* {
      for (auto it = body.rbegin(); it != body.rend(); ++it) {
        if (it->kind == mir::Stmt::Skip) continue;
        if (it->kind == mir::Stmt::Block || it->kind == mir::Stmt::SList) {
          if (const auto* tail = self(self, it->body)) return tail;
          continue;
        }
        return &*it;
      }
      return nullptr;
    };
    symbolic_lane_tail = terminal(terminal, p.log_prob);
  }
  for (const auto& s : p.log_prob) lower_stmt(s);
  prep.graph(prep_graph, "lower", lower_time, g, out.fills, target_terms.size(),
             out.views.size());
  dump("lower", {});
  // Jacobian terms and constrained-parameter views are read straight out
  // of the arena, so no op consumes them and the pass cannot infer them.
  std::vector<int> roots = jac_slots;
  for (const auto& v : out.views) roots.push_back(v.slot);
  roots.insert(roots.end(), extra_roots.begin(), extra_roots.end());

  run_passes(roots, PassPlan{true, true, true, true, true});

  const auto reduce_time = prep.start();
  std::vector<int> all = target_terms;
  all.insert(all.end(), jac_slots.begin(), jac_slots.end());
  g.result_slot = reduce_terms(all);
  prep.graph(prep_graph, "reduce", reduce_time, g, out.fills,
             target_terms.size(), out.views.size());
  dump("reduce", roots);
  prep.graph(prep_graph, "total", total_time, g, out.fills, target_terms.size(),
             out.views.size(), PrepTrace::Extra::None, 0, 0, true,
             out.n_unconstrained);
  guard.done = true;
  out.graph = std::move(g);
  return std::move(out);
}
}  // namespace lower_detail

using namespace lower_detail;

namespace {

void add_interpreter_fallback(CompiledModel& cm, const std::string& note) {
  auto& all = cm.interpreter_fallbacks;
  if (std::find(all.begin(), all.end(), note) == all.end()) all.push_back(note);
}

std::string fallback_items(const CompiledModel& cm) {
  std::string items;
  for (const std::string& note : cm.interpreter_fallbacks)
    items += (items.empty() ? "" : "; ") + note;
  return items;
}

std::string report_request() {
  return "Please report this at https://github.com/seantalts/stanli/issues "
         "with this message and the model. stanli build " +
         std::string(runtime_build_id()) + ".";
}

}  // namespace

CompiledModel compile_model(const std::string& mir_text, const DataMap& data,
                            unsigned seed) {
  const char* prep_env = std::getenv("STANLI_PROFILE_PREP");
  PrepTrace prep(prep_env && prep_env[0] != '0');
  PassDumper dumper(std::getenv("STANLI_DUMP_PASSES"),
                    std::getenv("STANLI_DUMP_STAGES"));
  if (dumper.enabled()) dumper.write("mir", "mir.sexp", mir_text);
  const auto compile_time = prep.start();
  // Shared because the interpreted write_array fallback, when needed,
  // keeps the generate_quantities statements and UDF bodies alive for the
  // model's whole life.
  const auto parse_time = prep.start();
  auto prog = std::make_shared<mir::Program>(decode_program(mir_text));
  prep.plain("compile", "parse_mir", parse_time, PrepTrace::Extra::MirBytes,
             static_cast<int64_t>(mir_text.size()));
  Lowering lo(data, prep, dumper, "log_prob", WaRng(seed));
  CompiledModel cm = lo.run(*prog);
  if (!prog->generate_quantities.empty()) {
    // A second lowering, over the transformed data the first one already
    // interpreted: re-running prepare_data would double preparation time on
    // the models where preparation is the cost (nn_rbm1bJ100, 20.7 s).
    const PreparedContext prepared = lo.prepared_context();
    Lowering wa(data, prep, dumper, "write_array", prepared);
    CompiledModel::WriteArray w = wa.run_write_array(*prog);
    for (const std::string& note : wa.out.interpreter_fallbacks)
      add_interpreter_fallback(cm, note);
    if (w.n_unconstrained != cm.n_unconstrained) {
      // The two graphs read the same draw; if they disagree on its length the
      // write_array cannot be driven at all. Keep the model, drop the columns,
      // and say so rather than emitting a silently misaligned CSV.
      w.truncated = "write_array reads " + std::to_string(w.n_unconstrained) +
                    " unconstrained values, log_prob reads " +
                    std::to_string(cm.n_unconstrained) +
                    (w.truncated.empty() ? "" : "; " + w.truncated);
      w.columns.clear();
      w.n_tp_start = w.n_gq_start = 0;
    }
    // STANLI_WA_FORCE_INTERP is a TEST-ONLY hook: it attaches the
    // interpreter beside a graph that lowered the whole section, so the
    // cross-path harness (tests/cross_path.hpp) can read both engines off
    // one model and hold them against each other on the same draw. It
    // changes which objects are retained, never what either engine
    // computes -- the graph above is built identically either way. Never
    // set it in a shipped environment: drivers PREFER an attached
    // interpreter (capi.cpp, bridgestan_abi.cpp), so it moves every caller
    // onto the slow per-draw path.
    if (!w.truncated.empty() || std::getenv("STANLI_WA_FORCE_INTERP")) {
      // The graph could not express the whole section; hand the model the
      // per-draw interpreter, seeded with data + transformed data and the
      // emission flags the guard blocks test.
      auto env = prepared.environment;
      for (const char* flag :
           {"emit_transformed_parameters__", "emit_generated_quantities__"}) {
        DataMap::Entry one;
        one.is_int = true;
        one.i = {1};
        one.r = {1.0};
        env[flag] = one;
      }
      w.interp = std::make_shared<WaInterp>(prog, std::move(env));
    }
    cm.write_array = std::move(w);
    if (!cm.write_array->truncated.empty())
      add_interpreter_fallback(cm,
                               "transformed parameters and generated "
                               "quantities (" +
                                   cm.write_array->truncated + ")");
  }
  if (prog->has_transform_inits) {
    // The inverse parameter transforms. Nothing is interpreted here: the
    // section runs only when a caller actually supplies constrained starting
    // values, so a model nobody inits by name never pays for a bound
    // expression this build cannot evaluate.
    CompiledModel::TransformInits ti;
    std::vector<InitParam> params;
    if (cm.views.size() != cm.unc_params.size()) {
      ti.truncated = "the constrained and free parameter lists disagree (" +
                     std::to_string(cm.views.size()) + " vs " +
                     std::to_string(cm.unc_params.size()) + ")";
    } else {
      for (size_t i = 0; i < cm.views.size(); ++i) {
        const CompiledModel::ParamView& view = cm.views[i];
        const CompiledModel::UncParam& unc = cm.unc_params[i];
        if (view.name != unc.name) {
          ti.truncated =
              "constrained and free parameters are out of order at " +
              view.name;
          break;
        }
        InitParam p;
        p.name = view.name;
        p.dims = view.dims;
        p.constrained_len = view.len;
        p.free_len = unc.len;
        // The leaf is the unit the arena keeps contiguous inside each
        // element of the surrounding array. An innermost matrix is one
        // whatever the transform is -- the arena stores it column-major
        // while a serial init lists it first-index-fastest, and an
        // elementwise transform over an array of matrices has to cross that
        // permutation too. Otherwise only a structured transform has a leaf;
        // an elementwise one treats each value on its own, which for a
        // vector or a plain array is the same enumeration either way.
        p.leaf_rank = view.matrix_storage                  ? 2
                      : is_structured_check(unc.transform) ? 1
                                                           : 0;
        if ((size_t)p.leaf_rank > p.dims.size()) {
          ti.truncated = view.name +
                         " declares fewer dimensions than its "
                         "transform needs";
          break;
        }
        params.push_back(std::move(p));
      }
    }
    if (ti.truncated.empty())
      ti.interp =
          std::make_shared<InitInterp>(prog, lo.td.env(), std::move(params));
    cm.transform_inits = std::move(ti);
  }
  if (!cm.interpreter_fallbacks.empty() && std::getenv("STANLI_NO_INTERPRETER"))
    throw CompileError(interpreter_error(cm));
  prep.plain("compile", "total", compile_time);
  prep.report();
  return cm;
}

std::string interpreter_error(const CompiledModel& cm) {
  return "stanli: STANLI_NO_INTERPRETER is set and parts of this model have "
         "no compiled path: " +
         fallback_items(cm) +
         ". Unset it to run them through the MIR interpreter, which is far "
         "slower. " +
         report_request();
}

std::string interpreter_warning(const CompiledModel& cm,
                                const std::string& probe_failure) {
  if (cm.interpreter_fallbacks.empty()) return "";
  std::string text =
      "stanli: parts of this model have no compiled path and run through the "
      "MIR interpreter, which is far slower: " +
      fallback_items(cm) + ".";
  if (!probe_failure.empty())
    text += " The interpreter also failed at every probe point (" +
            probe_failure +
            "), so transformed parameters and generated quantities are "
            "unavailable and draws carry only the constrained parameters.";
  return text + " " + report_request() +
         " Set STANLI_NO_INTERPRETER=1 to refuse such models.";
}

}  // namespace stanli
