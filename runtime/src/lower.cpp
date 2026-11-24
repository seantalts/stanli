#include "lower_internal.hpp"

#include "build_id.hpp"
#include "lower_work.hpp"
#include "lower_worker.hpp"
#include "pass_util.hpp"
#include <stanli/debug_switch.hpp>
#include <stanli/gp_cov_fusion.hpp>
#include <stanli/target_sums.hpp>
#include <stanli/execution_report.hpp>
#include <stanli/large_stack.hpp>
#include <stanli/threading.hpp>

#include <stan/math/rev/core/chainablestack.hpp>

#include <cstring>
#include <thread>

namespace stanli {
namespace lower_detail {

StructuredMode read_structured_mode() {
  const char* flag = debug_env("STANLI_STRUCTURED_LOOPS");
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
      td_rng(std::move(stream)) {
  td.set_native_extrema(true);
  td.refuse_propto_densities();
}
template <typename Map>
static void copy_named(Map& to, const Map& from,
                       const std::set<std::string>* names) {
  if (!names) {
    to = from;
    return;
  }
  for (const std::string& name : *names)
    if (const auto found = from.find(name); found != from.end())
      to.emplace(name, found->second);
}
Lowering::Lowering(const DataMap& d, PrepTrace& p, PassDumper& dump_to,
                   const char* graph_name,
                   const PreparedContext& prepared_context)
    : Lowering(d, p, dump_to, graph_name, prepared_context.rng,
               std::make_shared<ShapeInterner>(*prepared_context.shapes)) {
  const auto env_copy_time = prep.start();
  copy_named(td.env(), prepared_context.environment, &prepared_context.names);
  count_data_copy(td.env());
  zero_size_data = prepared_context.zero_size_data;
  int_env = prepared_context.integers;
  copy_named(decls, prepared_context.declarations, &prepared_context.names);
  prepared_names = prepared_context.names;
  prep.plain(prep_graph, "env_copy", env_copy_time);
}
Lowering::Lowering(Lowering& parent, RegionTrialTag,
                   const std::set<std::string>* names)
    : Lowering(parent.data, parent.prep, parent.dumper, parent.prep_graph,
               parent.td_rng,
               std::make_shared<ShapeInterner>(*parent.shape_pool)) {
  compile_options = parent.compile_options;
  zero_size_data = parent.zero_size_data;
  fun_defs = parent.fun_defs;
  copy_named(decls, parent.decls, names);
  copy_named(td.env(), parent.td.env(), names);
  count_data_copy(td.env());
  copy_named(int_env, parent.int_env, names);
  if (!names)
    int_locals = parent.int_locals;
  else
    for (const std::string& name : *names)
      if (parent.int_locals.count(name)) int_locals.insert(name);
  td.set_propto_context(parent.td.propto_context());
  udf_depth = parent.udf_depth;
  udf_autodiff_ctx = parent.udf_autodiff_ctx;
  udf_formal_autodiff = parent.udf_formal_autodiff;
}
PreparedContext Lowering::prepared_context() {
  if (!data_prepared)
    throw std::logic_error("lowering has not prepared model data");
  return {td_rng, shape_pool,     td.env(),      int_env_data,
          decls,  zero_size_data, prepared_names};
}
Lowering Lowering::fork_region_trial(const std::set<std::string>* names) {
  return Lowering(*this, RegionTrialTag{}, names);
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
    td_erase(name);
    return;
  }
  if (const DataMap::Entry* en = observation(v)) {
    td_assign(name, *en);
    return;
  }
  // Evaluate before erasing the old binding: `x = x + data_step` reads the
  // previous x, and data-only while loops depend on retaining that value for
  // their next condition.
  auto evaluated = try_eval_pure(rhs);
  td_erase(name);
  if (evaluated) {
    DataMap::Entry en = std::move(*evaluated);
    td_assign(name, en);
    observe(v, std::move(en));
  }
}
void Lowering::observe_indexed_rhs(const mir::Expr& rhs, const Val& v) {
  if (observation(v) || !v.si.param_free) return;
  if (auto evaluated = try_eval_pure(rhs)) {
    observe(v, std::move(*evaluated));
    return;
  }
  if (!mir::is_unsized(rhs, mir::UnsizedLeaf::Int) || g.slots[v.slot].len != 1)
    return;
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
// CmdStan checks every data variable's dims before it reads one.
void Lowering::validate_data_dims(const std::string& name,
                                  const mir::SizedType& t) {
  if (!data.has(name)) return;
  const std::vector<int64_t> declared = sized_dims(t);
  for (int64_t d : declared)
    if (d < 0) fail("negative extent for data " + name, t.raw);
  check_declared_dims(
      VarStage::Data, name, declared, data.at(name),
      t.base == "SInt" || (t.base == "SArray" && t.elem_base == "SInt"));
}
void Lowering::report_missing_data(const mir::Program& p) {
  std::string names;
  for (const auto& input : p.input_vars) {
    const std::string& key = mir::detail::data_key(p, input.first);
    if (!data_entry(key)) names += (names.empty() ? "" : ", ") + key;
  }
  fail("missing data for " + names +
       "; the data must give every variable in the data block");
}
namespace {
struct ExtentScope {
  bool& flag;
  explicit ExtentScope(bool& f) : flag(f) { flag = true; }
  ~ExtentScope() { flag = false; }
};
}  // namespace
void Lowering::bind_zero_size(const std::string& name,
                              const mir::SizedType& type) {
  if (type.dims.empty()) return;
  std::vector<int64_t> dims;
  try {
    ExtentScope extents(data_extent_scope);
    dims = sized_dims(type);
  } catch (const CompileError&) {
    return;
  }
  int64_t size = 1;
  for (int64_t d : dims) size *= d;
  if (size != 0) return;
  if (type.base == "SInt" ||
      (type.base == "SArray" && type.elem_base == "SInt"))
    zero_size_data.set_int_array(name, {}, std::move(dims));
  else
    zero_size_data.set_real_array(name, {}, std::move(dims));
}
void Lowering::bind_data(const mir::Program& p) {
  std::set<std::string> input_names;
  bool all_inputs_bound = true;
  bool use_prebound = debug_env("STANLI_NO_DATA_PRELOAD") == nullptr;
  for (const auto& [name, type] : p.input_vars) {
    input_names.insert(name);
    // A tuple's leaf is read under its CmdStan key.
    const std::string& key = mir::detail::data_key(p, name);
    bind_zero_size(key, type);
    if (!data_entry(key)) {
      all_inputs_bound = false;
      continue;
    }
    // DataMap does not have the Stan schema, so JSON values spelled with
    // integer tokens carry an int mirror even when the declaration is
    // real. Reconstruct the typed value directly, without copying an
    // irrelevant mirror for a large real matrix.
    const DataMap::Entry& src = *data_entry(key);
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
  if (!all_inputs_bound) report_missing_data(p);
  use_prebound = use_prebound && all_inputs_bound;
  if (use_prebound) {
    // The generated reconstruction allocated the MIR-declared shape and
    // copied exactly that many flat elements. Normalize to the same shape;
    // a malformed length falls back to that checked interpreter path.
    ExtentScope extents(data_extent_scope);
    for (const auto& [name, type] : p.input_vars) {
      DataMap::Entry& dst = td.env().at(name);
      if (static_cast<int64_t>(dst.r.size()) !=
          sized_len(type, nullptr, nullptr, name)) {
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
    sh.len = sized_len(type, nullptr, nullptr, name);
    sh.si = view_of(type, true);
    decls[name] = sh;
  };
  {
    ExtentScope extents(data_extent_scope);
    for (const auto& [name, type] : p.input_vars) {
      try {
        record(name, type);
        validate_data_dims(mir::detail::data_key(p, name), type);
      } catch (const std::exception&) {
        // CmdStan computes each size, printing if it prints, before it
        // checks that input. Print the sizes prepare_data computes ahead of
        // the failing input's declaration, then report its error.
        data_extent_scope = false;
        for (const auto& st : p.prepare_data) {
          if (st.kind != mir::Stmt::Decl) continue;
          if (st.decl_id == name) break;
          if (!st.has_init || input_names.count(st.decl_id) ||
              expr_effects(st.init) != kMessageEffect)
            continue;
          try {
            (void)td.exec(st);
          } catch (const std::exception&) {
            break;
          }
        }
        throw;
      }
    }
  }
  for (const auto& st : p.prepare_data) {
    if (st.kind == mir::Stmt::Decl) {
      if (input_names.count(st.decl_id)) {
        ExtentScope extents(data_extent_scope);
        record(st.decl_id, st.decl_type);
      } else {
        record(st.decl_id, st.decl_type);
      }
    }
    // stanc's prepare_data first declares every input and reads it with one
    // FnReadData statement. DataMap has already parsed the data into the
    // same typed, column-major representation above, so both are skipped.
    // FnReadData is compiler-internal and cannot occur in source
    // transformed-data code, so such a statement is input hydration, not
    // user computation.
    if (use_prebound &&
        ((st.kind == mir::Stmt::Decl && input_names.count(st.decl_id)) ||
         direct_input_load(st, input_names)))
      continue;
    record_interpreter_event("prepare_data", "statement_entry");
    (void)td.exec(st);
  }
  // The interpreter keeps a block's locals after the block ends. Drop them,
  // so a later local of the same name cannot start from transformed data's
  // last value for it.
  prepared_names = input_names;
  const auto top_level = [&](const auto& self, const mir::Stmt& st) -> void {
    if (st.kind == mir::Stmt::Decl) prepared_names.insert(st.decl_id);
    if (st.kind == mir::Stmt::SList)
      for (const auto& k : st.body) self(self, k);
  };
  for (const auto& st : p.prepare_data) top_level(top_level, st);
  for (auto it = td.env().begin(); it != td.env().end();)
    it = prepared_names.count(it->first) ? std::next(it) : td.env().erase(it);
  for (auto it = decls.begin(); it != decls.end();)
    it = prepared_names.count(it->first) ? std::next(it) : decls.erase(it);
  for (auto& [name, e] : td.env()) {
    if (e.is_int && e.i.size() == 1 && e.dims.empty()) int_env[name] = e.i[0];
  }
  int_env_data = int_env;
  const auto shadowed = [&](const auto& self, const mir::Stmt& st) -> void {
    if (st.kind == mir::Stmt::Decl && prepared_names.count(st.decl_id) &&
        !shadowed_prepared.count(st.decl_id)) {
      ShadowedPrepared& saved = shadowed_prepared[st.decl_id];
      if (const auto it = td.env().find(st.decl_id); it != td.env().end())
        saved.value = it->second;
      if (const auto it = decls.find(st.decl_id); it != decls.end())
        saved.decl = it->second;
    }
    for (const auto& child : st.body) self(self, child);
  };
  for (const auto& st : p.log_prob) shadowed(shadowed, st);
  data_prepared = true;
  if (after_data_prepared) after_data_prepared();
}
void Lowering::restore_shadowed_prepared() {
  for (auto& [name, saved] : shadowed_prepared) {
    if (saved.value)
      td.env()[name] = *saved.value;
    else
      td.env().erase(name);
    if (saved.decl)
      decls[name] = *saved.decl;
    else
      decls.erase(name);
  }
}
// ---- statements -----------------------------------------------------------
CompiledModel::ParamView Lowering::parameter_view(const mir::Stmt& s, int slot,
                                                  int64_t len) {
  using Naming = CompiledModel::ParamView::Naming;
  CompiledModel::ParamView view{parameter_name(s.decl_id), slot, len};
  if (view.name != s.decl_id) view.binding = s.decl_id;
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
Lowering::ParamShape Lowering::param_shape(
    const mir::SizedType& type, const mir::Transform& tr,
    const std::vector<mir::Expr>& read_dims, const std::string& raw) {
  ParamShape shape;
  std::vector<int64_t>& declared_dims = shape.declared_dims;
  declared_dims = sized_dims(type);
  ViewKind& leaf = shape.leaf;
  leaf =
      type.base == "SArray" ? leaf_kind(type.elem_base) : leaf_kind(type.base);
  const size_t leaf_dims = static_cast<size_t>(leaf_rank(leaf));
  if (declared_dims.size() < leaf_dims)
    fail("parameter declaration has incomplete leaf dimensions", raw);
  const std::vector<int64_t> outer_dims(declared_dims.begin(),
                                        declared_dims.end() - leaf_dims);
  int64_t& n_batch = shape.n_batch;
  n_batch = checked_product(outer_dims, "parameter batch");

  // Unstructured transforms use FnReadParam's declared raw shape. A
  // structured leaf replaces only its innermost dimensions below; the
  // outer array geometry remains declaration-owned and orthogonal.
  std::vector<int64_t>& raw_dims = shape.raw_dims;
  for (const auto& d : read_dims) raw_dims.push_back(eval_int(d));
  std::vector<int64_t> expected_read_dims = declared_dims;
  if (tr.kind == mir::Transform::CholeskyCorr ||
      tr.kind == mir::Transform::Correlation ||
      tr.kind == mir::Transform::Covariance) {
    if (leaf != ViewKind::Matrix || expected_read_dims.size() < 2)
      fail("matrix parameter transform has a non-matrix declaration", raw);
    expected_read_dims.pop_back();
  }
  if (raw_dims != expected_read_dims)
    fail("parameter read dimensions do not match its declaration", raw);
  int64_t& con_len = shape.con_len;
  int64_t& raw_len = shape.raw_len;
  int64_t& inner_raw = shape.inner_raw;
  int64_t& inner_con = shape.inner_con;
  int64_t& matrix_rows = shape.matrix_rows;
  int64_t& matrix_cols = shape.matrix_cols;
  con_len = checked_product(raw_dims, "parameter read shape");
  raw_len = con_len;

  auto vector_leaf = [&](int64_t free_size) {
    if (leaf != ViewKind::Vector || declared_dims.empty())
      fail("vector parameter transform has a non-vector declaration", raw);
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
      fail("matrix parameter transform has a non-matrix declaration", raw);
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
      fail("sum-to-zero or simplex transform has an invalid declaration", raw);
    }
  } else if (tr.kind == mir::Transform::Ordered ||
             tr.kind == mir::Transform::PositiveOrdered ||
             tr.kind == mir::Transform::UnitVector) {
    if (leaf != ViewKind::Vector || declared_dims.empty())
      fail("vector parameter transform has a non-vector declaration", raw);
    vector_leaf(declared_dims.back());
  } else if (tr.kind == mir::Transform::CholeskyCorr ||
             tr.kind == mir::Transform::Correlation ||
             tr.kind == mir::Transform::Covariance) {
    if (leaf != ViewKind::Matrix || declared_dims.size() < 2)
      fail("matrix parameter transform has a non-matrix declaration", raw);
    const int64_t K = declared_dims.back();
    if (declared_dims[declared_dims.size() - 2] != K)
      fail("square matrix transform has a rectangular declaration", raw);
    const int64_t free_size = tr.kind == mir::Transform::Covariance
                                  ? K * (K + 1) / 2
                                  : K * (K - 1) / 2;
    flat_matrix_leaf(free_size);
  } else if (tr.kind == mir::Transform::StochasticColumn ||
             tr.kind == mir::Transform::StochasticRow) {
    if (leaf != ViewKind::Matrix || declared_dims.size() < 2)
      fail("matrix parameter transform has a non-matrix declaration", raw);
    matrix_rows = declared_dims[declared_dims.size() - 2];
    matrix_cols = declared_dims.back();
    const bool column = tr.kind == mir::Transform::StochasticColumn;
    const int64_t raw_rows = matrix_rows - (column ? 1 : 0);
    const int64_t raw_cols = matrix_cols - (column ? 0 : 1);
    inner_raw =
        checked_product({raw_rows, raw_cols}, "stochastic matrix raw shape");
    inner_con =
        checked_product({matrix_rows, matrix_cols}, "stochastic matrix leaf");
    raw_dims = outer_dims;
    raw_dims.push_back(raw_rows);
    raw_dims.push_back(raw_cols);
    raw_len = checked_product({n_batch, inner_raw}, "parameter raw shape");
    con_len =
        checked_product({n_batch, inner_con}, "parameter constrained shape");
  } else if (tr.kind == mir::Transform::CholeskyCov) {
    if (leaf != ViewKind::Matrix || declared_dims.size() < 2)
      fail("matrix parameter transform has a non-matrix declaration", raw);
    const int64_t M = declared_dims[declared_dims.size() - 2];
    const int64_t N = declared_dims.back();
    if (M < N) fail("cholesky-factor-cov rows are smaller than columns", raw);
    flat_matrix_leaf(N * (N + 1) / 2 + (M - N) * N);
  }

  return shape;
}
void Lowering::lower_read_param(const mir::Stmt& s) {
  const mir::Transform& tr = *s.read_transform;
  const ParamShape shape = param_shape(s.decl_type, tr, s.read_dims, s.raw);
  const ViewKind leaf = shape.leaf;
  const std::vector<int64_t>& raw_dims = shape.raw_dims;
  const int64_t n_batch = shape.n_batch, con_len = shape.con_len,
                raw_len = shape.raw_len, inner_raw = shape.inner_raw,
                inner_con = shape.inner_con, matrix_rows = shape.matrix_rows,
                matrix_cols = shape.matrix_cols;
  SlotInfo psi = view_of(s.decl_type);
  const int64_t declared_len =
      sized_len(s.decl_type, nullptr, nullptr, s.decl_id);
  if (declared_len != con_len)
    fail("parameter view length does not match constrained storage", s.raw);
  validate_view(psi, con_len, "parameter ", s.decl_id);
  decls[s.decl_id] = DeclView{con_len, scalar_autodiff(), psi};
  const mir::detail::TupleLeaf* tuple = tuple_leaf(s.decl_id);
  int raw = -1;
  if (tuple) {
    raw = tuple_leaf_free(*tuple, raw_len, s.raw);
  } else {
    raw = add_slot(raw_len, /*is_param=*/true);
    out.param_names.push_back(s.decl_id);
    // The unconstrained layout the caller needs to slice a draw: how
    // long this parameter's piece is, and what it means. raw_len and
    // con_len part company for every structured transform.
    CompiledModel::UncParam u;
    u.name = s.decl_id;
    u.len = raw_len;
    u.dims = raw_dims;
    u.transform = tr.kind;
    out.unc_params.push_back(std::move(u));
    out.n_unconstrained += raw_len;
  }
  // Whole variables for the interpreter and the inits; the columns, which
  // a tuple parameter adds once every leaf is read.
  const auto add_views = [&](int slot) {
    if (in_write_array) return;
    if (tuple) {
      TupleBlock& block = tuple_blocks.at(tuple->base);
      block.constrained[s.decl_id] = {slot, shape};
      if (block.constrained.size() == tuples->leaves_of.at(tuple->base).size())
        tuple_parameter_views(*tuple);
      return;
    }
    out.param_leaves.push_back(
        {(int)out.views.size(), {}, raw_len, tr.kind, s.decl_id});
    out.views.push_back(parameter_view(s, slot, con_len));
  };

  if (tr.kind == mir::Transform::Identity) {
    Val value{raw, scalar_autodiff(), psi, owning_layout(psi)};
    scope[s.decl_id] = value;
    // In write_array mode the column order is dictated by the FnWriteParam
    // statements, which come later and cover transformed parameters and
    // generated quantities too; declaration order would be wrong.
    add_views(value.slot);
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
    case mir::Transform::StochasticColumn:
      opcode = OP_CONSTRAIN_STOCHASTIC_COLUMN;
      break;
    case mir::Transform::StochasticRow:
      opcode = OP_CONSTRAIN_STOCHASTIC_ROW;
      break;
    default:
      fail("this parameter constraint is not supported yet", tr.raw);
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
      opcode == OP_CONSTRAIN_COV_MATRIX || opcode == OP_CONSTRAIN_CHOL_COV ||
      opcode == OP_CONSTRAIN_STOCHASTIC_COLUMN ||
      opcode == OP_CONSTRAIN_STOCHASTIC_ROW)
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
  if (opcode == OP_CONSTRAIN_UNIT_VECTOR && !scalar_autodiff())
    g.ops.back().variant = kUnitVectorChecksNorm;
  con.layout = owning_layout(psi);
  jac_slots.push_back(jac);
  scope[s.decl_id] = con;
  add_views(con.slot);
}
int Lowering::tuple_leaf_free(const mir::detail::TupleLeaf& leaf,
                              int64_t raw_len, const std::string& raw) {
  TupleBlock& block = tuple_blocks[leaf.base];
  const std::vector<std::string>& names = tuples->leaves_of.at(leaf.base);
  if (!block.laid_out) {
    // The first leaf read lays out the whole parameter's block.
    CompiledModel::UncParam u;
    u.name = leaf.base;
    for (const std::string& name : names) {
      const mir::detail::TupleLeaf& other = tuples->leaves.at(name);
      if (!other.read_transform)
        fail("the tuple parameter " + leaf.base + " has a leaf without a read",
             raw);
      const ParamShape shape =
          param_shape(other.type, *other.read_transform, other.read_dims, raw);
      CompiledModel::UncParam::Leaf free;
      for (const mir::detail::TupleSlot& slot : other.path)
        free.path.emplace_back(slot.before, slot.component);
      free.dims = shape.raw_dims;
      free.len = shape.raw_len;
      free.transform = other.read_transform->kind;
      u.leaves.push_back(std::move(free));
    }
    std::vector<std::vector<int>> gathers(names.size());
    for (size_t k = 0; k < names.size(); ++k)
      gathers[k].resize((size_t)u.leaves[k].len);
    int64_t next = 0;
    u.for_each_element([&](size_t k, const std::vector<int64_t>& index) {
      const CompiledModel::UncParam::Leaf& free = u.leaves[k];
      const int64_t per = free.len / free.elements();
      // The leaf holds its tuple-array elements outer-major, each one's
      // free values together, as an array parameter does.
      int64_t element = 0;
      for (size_t d = 0; d < index.size(); ++d)
        element = element * free.dims[d] + index[d];
      for (int64_t j = 0; j < per; ++j)
        gathers[k][(size_t)(element * per + j)] =
            checked_immediate(next++, "tuple parameter block");
    });
    u.len = next;
    bool leaf_major = true;
    int expected = 0;
    for (const std::vector<int>& gather : gathers)
      for (const int at : gather) leaf_major = leaf_major && at == expected++;
    block.laid_out = true;
    if (!leaf_major) block.slot = add_slot(next, /*is_param=*/true);
    for (size_t k = 0; k < names.size(); ++k)
      block.gather[names[k]] = std::move(gathers[k]);
    out.param_names.push_back(leaf.base);
    out.n_unconstrained += next;
    out.unc_params.push_back(std::move(u));
  }
  const std::vector<int>& gather = block.gather.at(leaf.variable);
  if ((int64_t)gather.size() != raw_len)
    fail("tuple parameter leaf " + leaf.variable +
             " reads a different number of free values than its block holds",
         raw);
  if (block.slot < 0) {
    // Parameter slots make the free vector in the order they are added.
    if (block.next_leaf >= names.size() ||
        names[block.next_leaf] != leaf.variable)
      fail("the leaves of tuple parameter " + leaf.base +
               " are read out of order",
           raw);
    ++block.next_leaf;
    return add_slot(raw_len, /*is_param=*/true);
  }
  SlotInfo flat = view_of(mir::UnsizedView{0, mir::UnsizedLeaf::Vector});
  const Val whole{block.slot, scalar_autodiff(), flat};
  bool contiguous = true;
  for (size_t j = 1; j < gather.size() && contiguous; ++j)
    contiguous = gather[j] == gather[j - 1] + 1;
  if (contiguous)
    return emit_value(OP_SLICE, {whole}, raw_len, flat,
                      {gather.empty() ? 0 : gather[0]})
        .slot;
  return emit_value(OP_GATHER, {whole}, raw_len, flat, gather).slot;
}
void Lowering::tuple_parameter_views(const mir::detail::TupleLeaf& leaf) {
  using Naming = CompiledModel::ParamView::Naming;
  const TupleBlock& block = tuple_blocks.at(leaf.base);
  const std::vector<std::string>& names = tuples->leaves_of.at(leaf.base);
  const CompiledModel::UncParam* free = nullptr;
  for (const CompiledModel::UncParam& u : out.unc_params)
    if (u.name == leaf.base) free = &u;
  if (free == nullptr) fail("tuple parameter " + leaf.base + " has no block");
  // The constrained geometry has the free one's tuple structure.
  CompiledModel::UncParam constrained;
  constrained.name = leaf.base;
  for (size_t k = 0; k < names.size(); ++k) {
    const mir::detail::TupleLeaf& each = tuples->leaves.at(names[k]);
    const auto& [slot, shape] = block.constrained.at(names[k]);
    CompiledModel::UncParam::Leaf whole = free->leaves[k];
    whole.dims = shape.declared_dims;
    whole.len = shape.con_len;
    constrained.leaves.push_back(whole);
    const bool matrix = shape.leaf == ViewKind::Matrix;
    CompiledModel::ParamView view{names[k], slot, shape.con_len};
    if (shape.declared_dims.empty()) {
      view.naming = Naming::Scalar;
    } else {
      view.naming = matrix ? Naming::Matrix : Naming::Container;
      view.set_serial_layout(shape.declared_dims, matrix);
      if (matrix) view.rows = view.dims.at(view.dims.size() - 2);
    }
    out.param_leaves.push_back({-1, std::move(view), free->leaves[k].len,
                                free->leaves[k].transform, each.key});
  }
  constrained.for_each_element([&](size_t k,
                                   const std::vector<int64_t>& index) {
    const mir::detail::TupleLeaf& each = tuples->leaves.at(names[k]);
    const auto& [slot, shape] = block.constrained.at(names[k]);
    const CompiledModel::UncParam::Leaf& whole = constrained.leaves[k];
    const int64_t per = whole.len / whole.elements();
    int64_t element = 0;
    std::vector<int64_t> position;
    for (size_t d = 0; d < index.size(); ++d) {
      element = element * whole.dims[d] + index[d];
      position.push_back(index[d] + 1);
    }
    auto named = mir::detail::output_name(leaf.base, &each, position);
    if (!named) fail("cannot name a column of tuple parameter " + leaf.base);
    CompiledModel::ParamView view{std::move(named->name), slot, per};
    view.variable = std::move(named->variable);
    view.index = std::move(named->index);
    view.binding = names[k];
    const std::vector<int64_t> own(
        whole.dims.begin() + (std::ptrdiff_t)index.size(), whole.dims.end());
    const bool matrix = shape.leaf == ViewKind::Matrix;
    if (own.empty()) {
      view.naming = Naming::Scalar;
    } else {
      view.naming = matrix ? Naming::Matrix : Naming::Container;
      view.set_serial_layout(own, matrix);
      if (matrix) view.rows = own.at(own.size() - 2);
    }
    if (element != 0 || index.size() > 0) {
      std::vector<int64_t> order((size_t)per);
      for (int64_t j = 0; j < per; ++j)
        order[(size_t)j] = element * per + view.storage_index(j);
      view.storage_order = std::move(order);
    }
    out.views.push_back(std::move(view));
  });
}
namespace {

// Whether every evaluation of the log density runs all of `s`: no branch,
// loop, short circuit or user function decides what part of it runs.
bool always_runs(const mir::Expr& e) {
  if (e.kind == mir::Expr::TernaryIf || e.kind == mir::Expr::EOr ||
      e.kind == mir::Expr::EAnd ||
      (e.kind == mir::Expr::FunApp && e.fn_lib == mir::Expr::Lib::UserDefined))
    return false;
  return std::all_of(e.args.begin(), e.args.end(),
                     [](const mir::Expr& a) { return always_runs(a); });
}

bool always_runs(const mir::Stmt& s) {
  if (s.kind == mir::Stmt::IfElse || s.kind == mir::Stmt::While ||
      s.kind == mir::Stmt::For)
    return false;
  for (const mir::Expr* e : {&s.init, &s.rhs, &s.target})
    if (!always_runs(*e)) return false;
  for (const auto* exprs : {&s.lhs_idx, &s.fn_args})
    for (const mir::Expr& e : *exprs)
      if (!always_runs(e)) return false;
  return std::all_of(s.body.begin(), s.body.end(),
                     [](const mir::Stmt& c) { return always_runs(c); });
}

// Stan's var accumulator folds in source order (its 128-entry buffer carries
// each reduced prefix into the next buffer). Carry that same prefix through
// our six-input ADD_N calls; independent chunks would reassociate the sum.
template <typename EmitChunk>
int reduce_terms_grouped(std::vector<int> terms, EmitChunk emit_chunk) {
  if (terms.empty()) return -1;
  int prefix = terms[0];
  for (size_t i = 1; i < terms.size(); i += 5) {
    const size_t end = std::min(i + 5, terms.size());
    std::vector<int> chunk{prefix};
    chunk.insert(chunk.end(), terms.begin() + i, terms.begin() + end);
    prefix = emit_chunk(chunk);
  }
  return prefix;
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
                         const RerollStats* disp = nullptr,
                         int64_t term_steps = 0) {
    prep.graph(prep_graph, stage, from, g, out.fills, target_terms.size(),
               out.views.size(), extra, a, b, deep, params, c, d, disp,
               term_steps);
    dump(stage, stage_roots);
  };

  // Target terms have no consuming op yet either: reduce_terms (log_prob)
  // or the arena reads (write_array) see them only after the passes run.
  // Each call reflects the terms as they stand, since passes replace them.
  const auto roots_with_terms = [&] {
    std::vector<int> all = roots;
    all.insert(all.end(), target_terms.begin(), target_terms.end());
    return all;
  };
  // The target is a sum whose order stanli already chooses (reduce_terms
  // adds the terms in groups), so a local that accumulates terms before
  // `target +=` can hand them over one by one, for reroll to vectorize.
  // First, so that every pass configuration sees the same terms.
  if (!in_write_array) split_target_sums(g, out.fills, target_terms, roots);
  // A slot that `target +=` added k times becomes one term times k here,
  // whichever passes run, so that switching a pass off does not change how
  // the target is summed.
  scale_repeated_terms(g, out.fills, target_terms);
  const std::vector<int> update_roots = roots_with_terms();
  fuse_gp_diagonal_updates(g, update_roots);
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
    const ConstFoldStats constfolded =
        const_fold(g, out.fills, update_roots, known_outputs);
    trace("constfold", constfold_time, update_roots,
          PrepTrace::Extra::ConstFold, constfolded.ops_removed,
          constfolded.slots_folded);
  }
  const auto reroll_time = prep.start();
  const RerollStats rerolled =
      reroll(g, out.fills, target_terms, roots,
             compile_options.fast_math);  // STANLI_NO_REROLL
  trace("reroll", reroll_time, roots, PrepTrace::Extra::Reroll,
        rerolled.regions, rerolled.list_steps, false, 0,
        rerolled.candidate_steps, rerolled.row_steps, &rerolled,
        rerolled.term_steps);
  const bool lane_report = debug_env("STANLI_LANE_REPORT") != nullptr;
  if (lane_report) print_lane_declines(rerolled.declines, "reroll", prep_graph);
  // Re-roll can replace many element writes with copying slice stores, and
  // may have replaced target terms with vector reductions: rebuild the
  // implicit-root set before giving those new ops the same last-use proof
  // as the scalar stores.
  const std::vector<int> post_reroll_roots = roots_with_terms();
  const auto post_reroll_inplace_time = prep.start();
  const int post_reroll_inplace =
      rerolled.regions ? make_inplace_updates(g, post_reroll_roots) : 0;
  trace("post_reroll_inplace", post_reroll_inplace_time, post_reroll_roots,
        PrepTrace::Extra::Rewrites, post_reroll_inplace);
  std::vector<int> current_roots = post_reroll_roots;
  if (compile_options.fast_math && !in_write_array) {
    // After re-roll, which builds the vector terms this reads; before
    // partition, CSE and islands, which then work on what is left.
    const auto collapse_time = prep.start();
    const bool report_terms = debug_env("STANLI_COLLAPSE_REPORT") != nullptr;
    CollapseReport report;
    const CollapseStats collapsed = collapse_observations(
        g, out.fills, target_terms, roots, report_terms ? &report : nullptr);
    if (report_terms) print_collapse_report(report, prep_graph);
    current_roots = roots_with_terms();
    trace("collapse", collapse_time, current_roots, PrepTrace::Extra::Removed,
          collapsed.ops_removed);
  }
  if (plan.partition) {
    // After re-roll, which keeps first crack at the contiguous shapes it
    // already handles, and before CSE, which would merge ops shared
    // between lanes and leave the lanes no longer whole.
    const auto partition_time = prep.start();
    const PartitionStats parted = partition_lanes(
        g, out.fills, target_terms, roots, compile_options.fast_math);
    trace("partition", partition_time, roots, PrepTrace::Extra::Partition,
          parted.groups, parted.lanes, false, 0, parted.declined,
          parted.list_steps, nullptr, parted.term_steps);
    if (lane_report)
      print_lane_declines(parted.declines, "partition", prep_graph);
    // Same proof the slice stores re-roll makes get: rebuilt from the
    // terms partition just replaced.
    const std::vector<int> post_partition_roots = roots_with_terms();
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
    const CseStats cse_st =
        cse(g, out.fills, target_terms, roots, compile_options.fast_math);
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
  for (const auto& f : p.fun_defs) fun_defs[mir::function_key(f)] = &f;
  tuples = p.tuples.get();
  in_write_array = true;
  output_cursor =
      mir::OutputCursor(p.output_vars, mir::detail::whole_leaf_tuples(tuples));
  // The readers drop stanc3's section guards on these flags; a guard they
  // did not recognize still folds away with both pinned on.
  int_env["emit_transformed_parameters__"] = 1;
  int_env["emit_generated_quantities__"] = 1;
  const std::function<void(const mir::Stmt&)> sizes = [&](const mir::Stmt& s) {
    if (s.kind == mir::Stmt::Decl)
      for (const mir::Expr& d : s.decl_type.dims)
        mir::collect_names(d, &write_array_size_names);
    for (const mir::Stmt& child : s.body) sizes(child);
  };
  for (const mir::Stmt& s : p.generate_quantities) sizes(s);
  CompiledModel::WriteArray wa;
  DumpOnThrow guard{*this};
  const auto lower_time = prep.start();
  for (size_t i = 0; i < p.generate_quantities.size(); ++i) {
    const mir::Stmt& s = p.generate_quantities[i];
    if (i == p.transformed_parameters_begin)
      wa_section = mir::OutputVar::Block::TransformedParameters;
    if (i == p.generated_quantities_begin)
      wa_section = mir::OutputVar::Block::GeneratedQuantities;
    const auto mark = wa_mark();
    const auto fail_section = [&](const std::string& what) {
      const char* section =
          wa_section == mir::OutputVar::Block::GeneratedQuantities
              ? "generated quantities"
          : wa_section == mir::OutputVar::Block::TransformedParameters
              ? "transformed parameters"
              : "write_array";
      wa_rollback(mark);
      // Keep the valid prefix for diagnostics, but drivers select WaInterp
      // whenever this marker is set and it evaluates the whole section from
      // statement zero. There is no continuation frame for an arbitrary
      // nested failure or its lexical live-outs.
      wa.truncated = std::string(section) + " (" + what + ")";
    };
    try {
      lower_top_stmt(s);
    } catch (const CompileError& e) {
      fail_section(e.what());
      break;
    } catch (const std::logic_error& e) {
      fail_section(e.what());
      break;
    }
  }
  std::vector<int> roots = jac_slots;
  for (const auto& v : out.views) roots.push_back(v.slot);
  roots.insert(roots.end(), extra_roots.begin(), extra_roots.end());
  prep.graph(prep_graph, "lower", lower_time, g, out.fills, target_terms.size(),
             out.views.size(), PrepTrace::Extra::Truncated,
             wa.truncated.has_value());
  prep.work(prep_graph);
  dump("lower", roots);

  run_passes(roots, kWriteArrayPasses);

  const auto finalize_time = prep.start();
  // Nothing reads a result here, but forward() asserts a scalar result
  // slot, so point it at one.
  g.result_slot = const_slot(0.0);
  g.forward_only = true;
  // Callers read the columns after a run, and bind writes the fills once,
  // so neither can share storage with another slot.
  for (int slot : roots) g.slots[(size_t)slot].pinned = true;
  for (const auto& fill : out.fills) g.slots[(size_t)fill.first].pinned = true;
  wa.n_unconstrained = out.n_unconstrained;
  prep.graph(prep_graph, "finalize", finalize_time, g, out.fills,
             target_terms.size(), out.views.size());
  dump("finalize", roots);
  prep.graph(prep_graph, "total", total_time, g, out.fills, target_terms.size(),
             out.views.size(), PrepTrace::Extra::None, 0, 0, true,
             out.n_unconstrained);
  guard.done = true;
  wa.graph = std::move(g);
  // A section with no columns (or one lowering stopped short of) starts
  // where the next one does; the transformed-parameter boundary falls back
  // to the generated-quantities one, so the two cannot come out ordered
  // backwards and hand a reader a negative count.
  wa.n_gq_start = n_gq_start.value_or(out.views.size());
  wa.n_tp_start = n_tp_start.value_or(wa.n_gq_start);
  wa.columns = std::move(out.views);
  wa.fills = std::move(out.fills);
  return wa;
}
CompiledModel Lowering::run(const mir::Program& p) {
  DumpOnThrow guard{*this};
  const auto total_time = prep.start();
  for (const auto& f : p.fun_defs) fun_defs[mir::function_key(f)] = &f;
  tuples = p.tuples.get();
  const auto bind_time = prep.start();
  if (!data_prepared) bind_data(p);
  prep.graph(prep_graph, "bind_data", bind_time, g, out.fills,
             target_terms.size(), out.views.size());
  dump("bind_data", {});
  const auto lower_time = prep.start();
  for (size_t k = 0; k < p.log_prob.size(); ++k) {
    try {
      lower_top_stmt(p.log_prob[k]);
    } catch (const std::domain_error& e) {
      // A statement after the parameter reads that fails while lowering
      // and always runs fails the same way at every point in CmdStan.
      const auto read = [](const mir::Stmt& s) {
        return s.kind == mir::Stmt::Decl && s.read_transform;
      };
      if (!always_runs(p.log_prob[k]) ||
          std::any_of(p.log_prob.begin() + k, p.log_prob.end(), read))
        throw;
      emit_error(MessageError::Domain, e.what());
      break;
    }
  }
  prep.graph(prep_graph, "lower", lower_time, g, out.fills, target_terms.size(),
             out.views.size());
  prep.work(prep_graph);
  dump("lower", {});
  // Jacobian terms and constrained-parameter views are read straight out
  // of the arena, so no op consumes them and the pass cannot infer them.
  std::vector<int> roots = jac_slots;
  for (const auto& v : out.views) roots.push_back(v.slot);
  roots.insert(roots.end(), extra_roots.begin(), extra_roots.end());

  run_passes(roots, kLogProbPasses);

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

// The first bounded alternative covers effect-free loop bodies. Pure shape
// queries are allowed; unknown loop guards still refuse during lowering. A
// user call qualifies when its body does, as it would once inlined. An
// observable statement keeps the established representation, rather than
// broadening this proof through another engine.
bool bounded_specialization_candidate(Lowering& lo, const mir::Program& p) {
  bool controlled_loop = false;
  std::set<const mir::FunDef*> visited;
  std::function<bool(const mir::Stmt&)> statement;
  std::function<bool(const mir::Expr&)> shape = [&](const mir::Expr& e) {
    // Expansion gains were established for single selectors. Range/gather
    // bodies also expose different reduction/fusion choices; keep their
    // established representation until that separate arithmetic is proved.
    if (mir::is_index(e) && e.kind != mir::Expr::IndexSingle) return false;
    if (e.kind == mir::Expr::FunApp &&
        e.fn_lib == mir::Expr::Lib::UserDefined) {
      const auto found = lo.fun_defs.find(mir::function_key(e));
      if (found == lo.fun_defs.end()) return false;
      const auto specialized = lo.call_site_def(*found->second, e.args);
      const mir::FunDef* f = specialized ? specialized.get() : found->second;
      if (visited.insert(f).second)
        for (const auto& s : f->body)
          if (!statement(s)) return false;
    }
    for (const auto& a : e.args)
      if (!shape(a)) return false;
    return true;
  };
  const auto expression = [&](const mir::Expr& e) {
    return shape(e) && !lo.expr_effectful(e);
  };
  statement = [&](const mir::Stmt& s) {
    if (s.kind == mir::Stmt::NRFunApp && mir::internal_name(s) != "FnCheck" &&
        mir::internal_name(s) != "FnValidateSize")
      return false;
    if ((s.kind == mir::Stmt::For || s.kind == mir::Stmt::While) &&
        lo.region_runtime_control(s))
      controlled_loop = true;
    for (const auto* e :
         {&s.init, &s.rhs, &s.target, &s.lower, &s.upper, &s.cond})
      if (!expression(*e)) return false;
    for (const auto& e : s.lhs_idx)
      if (!expression(e)) return false;
    for (const auto& e : s.fn_args)
      if (!expression(e)) return false;
    for (const auto& e : s.decl_type.dims)
      if (!expression(e)) return false;
    for (const auto& child : s.body)
      if (!statement(child)) return false;
    return true;
  };
  for (const auto& s : p.log_prob)
    if (!statement(s)) return false;
  return controlled_loop;
}

std::optional<CompiledModel> try_bounded_specialization(Lowering& lo,
                                                        const mir::Program& p) {
  const char* policy = debug_env("STANLI_BOUNDED_SPECIALIZATION");
  // Explicit zero/unknown values keep the established path for ablation.
  if ((policy && std::string_view(policy) != "1") ||
      lo.structured_policy != StructuredMode::Auto)
    return {};
  for (const auto& f : p.fun_defs) lo.fun_defs[mir::function_key(f)] = &f;
  if (!bounded_specialization_candidate(lo, p)) return {};
  // Transformed data, including its RNG and observable effects, runs once.
  // Both paths start after it; the trial owns every mutable lowering field.
  lo.bind_data(p);
  Lowering trial(lo.data, lo.prep, lo.dumper, "bounded_log_prob",
                 lo.prepared_context());
  trial.compile_options = lo.compile_options;
  trial.int_env_data = lo.int_env_data;
  trial.data_prepared = true;
  trial.out.transformed_data_draws = lo.out.transformed_data_draws;
  trial.bounded_specialization = true;
  trial.structured_policy = StructuredMode::Off;
  try {
    return trial.run(p);
  } catch (const Lowering::SpecializationRefused&) {
  } catch (const CompileError&) {
    // The original path remains authoritative for unsupported constructs and
    // their diagnostics; a failed alternative is not a model error.
  }
  return {};
}

// The program budget. Each chain's executor holds its own copy of the
// program: one kernel context and dispatch entry per op. With twelve chains
// on one machine, unrolled programs of 10-24 MB per executor made each
// gradient 2.4-3.0x as slow as one chain alone, while programs up to 5 MB
// cost 1.1-1.3x (notes/triage/2026-11-24-issue-657-region-map-boundary.md).
// Past the budget, loops take the region map's lane form, which was faster
// than unrolling at 1, 4 and 12 chains. OP_LOOP was 1.2-1.5x slower than
// unrolling on 1 and 4 chains for the corpus programs of 9-30 MB, and as
// fast or faster for those of 44-67 MB, so loops take it only past the
// second budget. STANLI_PROGRAM_BUDGET and STANLI_LOOP_BUDGET override them
// in bytes; 0 turns the first off.
constexpr int64_t kProgramBudgetBytes = int64_t{8} << 20;
constexpr int64_t kLoopBudgetBytes = int64_t{32} << 20;
// The most ops one outermost loop may unroll to before the passes. The
// largest corpus loop unrolls to about 180,000.
constexpr size_t kUnrollCeilingOps = size_t{1} << 20;
// A loop charged less than this is never lowered compact for the budget.
constexpr int64_t kMinCompactBytes = kProgramBudgetBytes / 8;

int64_t switch_value(const char* name, int64_t fallback) {
  const char* v = debug_env(name);
  return v ? std::strtoll(v, nullptr, 10) : fallback;
}

// The outermost loops to lower compact so that the program fits `budget`.
// Each loop is charged the ops that survived the passes and write a slot
// that its unroll created; ops that the passes built, such as reroll's
// vector ops, are charged to no loop. The largest loops are taken until the
// rest fits.
std::set<const mir::Stmt*> loops_over_budget(
    const std::vector<Lowering::UnrolledLoop>& loops, const Graph& g,
    int64_t budget, bool op_loop) {
  const int64_t total = Executor::program_bytes(g);
  if (budget <= 0 || total <= budget || loops.empty() || g.ops.empty())
    return {};
  const int64_t per_op = total / static_cast<int64_t>(g.ops.size());
  std::vector<int64_t> ops(loops.size(), 0);
  for (const Op& op : g.ops) {
    const auto after =
        std::upper_bound(loops.begin(), loops.end(), op.out,
                         [](int slot, const Lowering::UnrolledLoop& l) {
                           return slot < l.first_slot;
                         });
    if (after == loops.begin()) continue;
    const size_t k = static_cast<size_t>(after - loops.begin()) - 1;
    if (op.out < loops[k].end_slot) ++ops[k];
  }
  // Without OP_LOOP, a loop that the region map cannot take stays
  // unrolled, so lowering it again would only cost time.
  std::map<const mir::Stmt*, int64_t> charged;
  for (size_t k = 0; k < loops.size(); ++k)
    if (op_loop || loops[k].lane_shape)
      charged[loops[k].loop] += ops[k] * per_op;
  std::vector<std::pair<int64_t, const mir::Stmt*>> order;
  for (const auto& [loop, bytes] : charged) order.emplace_back(bytes, loop);
  std::sort(order.begin(), order.end(),
            [](const auto& a, const auto& b) { return a.first > b.first; });
  std::set<const mir::Stmt*> chosen;
  int64_t rest = total;
  for (const auto& [bytes, loop] : order) {
    if (rest <= budget || bytes < std::min(kMinCompactBytes, budget)) break;
    chosen.insert(loop);
    rest -= bytes;
  }
  return chosen;
}

// Lowers log_prob again with `chosen` (the budget's loops) and `capped`
// (loops past the unroll ceiling) compact. A loop that passes the ceiling
// joins `capped` and the lowering starts over; the last attempt has no
// ceiling, so a capped loop that no compact form takes still unrolls. A
// chosen loop that no compact form takes leaves `chosen` and the lowering
// starts over, and with no loops left there is nothing to gain.
std::optional<CompiledModel> lower_compact(Lowering& lo, const mir::Program& p,
                                           std::set<const mir::Stmt*> chosen,
                                           std::set<const mir::Stmt*> capped,
                                           size_t ceiling, bool op_loop) {
  constexpr int kAttempts = 8;
  for (int attempt = 1;; ++attempt) {
    if (chosen.empty() && capped.empty()) return std::nullopt;
    std::set<const mir::Stmt*> loops = chosen;
    loops.insert(capped.begin(), capped.end());
    Lowering trial(lo.data, lo.prep, lo.dumper, "budget_log_prob",
                   lo.prepared_context());
    trial.compile_options = lo.compile_options;
    trial.int_env_data = lo.int_env_data;
    trial.data_prepared = true;
    trial.out.transformed_data_draws = lo.out.transformed_data_draws;
    trial.compact_loops = loops;
    trial.required_compact = chosen;
    trial.compact_op_loop = op_loop;
    trial.unroll_ceiling = attempt < kAttempts ? ceiling : 0;
    try {
      return trial.run(p);
    } catch (const Lowering::UnrollCeiling& c) {
      capped.insert(c.loop);
    } catch (const Lowering::CompactRefused& c) {
      chosen.erase(c.loop);
    }
  }
}

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

constexpr uint64_t kParallelWriteArrayMinWork = 1000;
constexpr uint64_t kParallelWriteArrayMaxDataBytes = uint64_t{64} << 20;

ParallelWriteArray parallel_write_array(const CompileOptions& options) {
  if (options.parallel_write_array != ParallelWriteArray::Auto)
    return options.parallel_write_array;
  const char* v = debug_env("STANLI_PARALLEL_WA");
  if (!v || !*v || std::string_view(v) == "auto")
    return ParallelWriteArray::Auto;
  if (std::string_view(v) == "1" || std::string_view(v) == "force")
    return ParallelWriteArray::Force;
  return ParallelWriteArray::Off;
}

class WriteArrayLowering {
 public:
  WriteArrayLowering(const DataMap& data, const mir::Program& prog,
                     bool trace_prep, PassDumper& dumper,
                     ParallelWriteArray policy)
      : data_(data),
        prog_(prog),
        trace_(trace_prep),
        dumper_(dumper),
        policy_(policy) {}

  void begin(Lowering& log_prob) {
    for (const auto& e : log_prob.td.env()) bound_env_.push_back(e.first);
    for (const auto& d : log_prob.decls) bound_decls_.push_back(d.first);
    if (!should_run_alongside(log_prob)) return;
    wa_.emplace(data_, trace_, off_, "write_array",
                log_prob.prepared_context());
    started_ = worker_.start([this] { result_ = wa_->run_write_array(prog_); });
  }

  CompiledModel::WriteArray finish(Lowering& log_prob, PrepTrace& prep) {
    CompiledModel::WriteArray w;
    if (started_) {
      worker_.rethrow_if_failed();
      w = std::move(*result_);
      w.lowered_in_parallel = true;
    } else {
      const bool lent = !wa_;
      if (lent) build_after_log_prob(log_prob);
      w = wa_->run_write_array(prog_);
      if (lent) return_environment(log_prob);
    }
    // A size that reads an integer a retained loop produced cannot be laid
    // out. Lower write_array again with those loops unrolled, so the section
    // stays in the graph instead of falling back to the interpreter. The
    // second attempt lowers further and may retain another such loop past
    // the point where the first stopped, so a third unrolls all of them.
    for (int attempt = 1; attempt <= 2 && w.truncated; ++attempt) {
      std::set<const mir::Stmt*> unrolled = wa_->write_array_unrolled_loops;
      const size_t before = unrolled.size();
      unrolled.insert(wa_->write_array_int_loops.begin(),
                      wa_->write_array_int_loops.end());
      if (unrolled.size() == before) break;
      build_after_log_prob(log_prob);
      wa_->write_array_unrolled_loops = std::move(unrolled);
      wa_->write_array_unroll_int_loops = attempt == 2;
      w = wa_->run_write_array(prog_);
      return_environment(log_prob);
    }
    prep.append(std::move(trace_));
    return w;
  }

  const std::vector<std::string>& interpreter_fallbacks() const {
    return wa_->out.interpreter_fallbacks;
  }

 private:
  bool should_run_alongside(Lowering& log_prob) {
    const ParallelWriteArray policy = policy_;
    if (policy == ParallelWriteArray::Off || !thread_safe_build() ||
        std::thread::hardware_concurrency() == 1 || dumper_.enabled() ||
        execution_trace_active())
      return false;
    // A program this deep is being lowered on the large stack, which the
    // second thread does not have.
    if (prog_.nesting > kInlineNesting) return false;
    if (policy == ParallelWriteArray::Force) return true;
    if (data_payload_bytes(log_prob.td.env()) > kParallelWriteArrayMaxDataBytes)
      return false;
    LoweringWork work(prog_, log_prob.int_env_data, log_prob.td.env(),
                      kParallelWriteArrayMinWork);
    return work.of(prog_.generate_quantities) >= kParallelWriteArrayMinWork &&
           work.of(prog_.log_prob) >= kParallelWriteArrayMinWork;
  }

  void build_after_log_prob(Lowering& log_prob) {
    const std::map<std::string, DataMap::Entry> no_environment;
    const std::map<std::string, DeclView> no_declarations;
    wa_.emplace(
        data_, trace_, dumper_, "write_array",
        PreparedContext{log_prob.td_rng, log_prob.shape_pool, no_environment,
                        log_prob.int_env_data, no_declarations,
                        log_prob.zero_size_data, log_prob.prepared_names});
    const auto select_time = trace_.start();
    // Moved, not copied: log_prob is done with them, and finish() moves
    // them back.
    for (const std::string& name : bound_env_) {
      const auto it = log_prob.td.env().find(name);
      if (it != log_prob.td.env().end())
        wa_->td.env().emplace(name, std::move(it->second));
    }
    for (const std::string& name : bound_decls_) {
      const auto it = log_prob.decls.find(name);
      if (it != log_prob.decls.end()) wa_->decls.emplace(name, it->second);
    }
    trace_.plain("write_array", "env_select", select_time);
  }

  // Moves back the data environment that build_after_log_prob lent.
  void return_environment(Lowering& log_prob) {
    for (const std::string& name : bound_env_) {
      const auto it = wa_->td.env().find(name);
      if (it != wa_->td.env().end())
        log_prob.td.env()[name] = std::move(it->second);
    }
  }

  const DataMap& data_;
  const mir::Program& prog_;
  PrepTrace trace_;
  PassDumper off_{nullptr, nullptr};
  PassDumper& dumper_;
  ParallelWriteArray policy_;
  std::vector<std::string> bound_env_, bound_decls_;
  std::optional<Lowering> wa_;
  std::optional<CompiledModel::WriteArray> result_;
  bool started_ = false;
  LowerWorker worker_;
};

// CmdStan's JSON reader lays out an array of tuples element by element:
// `z.2` of `array[3, 2] tuple(real, vector[4]) z` holds z[1, 1].2, then
// z[2, 1].2, and so on, each vector whole. A leaf variable is an ordinary
// array, which DataMap holds first index fastest across all its dimensions.
std::optional<DataMap> tuple_data_in_array_order(const mir::Program& p,
                                                 const DataMap& data) {
  std::optional<DataMap> out;
  if (!p.tuples) return out;
  for (const auto& input : p.input_vars) {
    const mir::detail::TupleLeaf* leaf = p.tuples->leaf(input.first);
    if (!leaf || !data.has(leaf->key)) continue;
    const std::string& key = leaf->key;
    const DataMap::Entry& entry = data.at(key);
    const std::vector<size_t> source =
        mir::detail::reader_order(*leaf, entry.dims);
    if (source.empty()) continue;
    const int64_t len = (int64_t)source.size();
    if (!out) out = data;
    if (entry.is_int) {
      std::vector<int> values((size_t)len);
      for (int64_t k = 0; k < len; ++k) values[(size_t)k] = entry.i[source[k]];
      out->set_int_array(key, std::move(values), entry.dims);
    } else {
      std::vector<double> values((size_t)len);
      for (int64_t k = 0; k < len; ++k) values[(size_t)k] = entry.r[source[k]];
      out->set_real_array(key, std::move(values), entry.dims);
    }
  }
  return out;
}

// Whether a user function can reach itself through calls, directly or
// through other functions. Any spelling of a defined function's key counts
// as a call, which includes a callback passed to a higher-order function.
bool has_recursive_functions(const mir::Program& p) {
  std::map<std::string, std::set<std::string>> callees;
  for (const mir::FunDef& f : p.fun_defs) callees[mir::function_key(f)];
  for (const mir::FunDef& f : p.fun_defs) {
    std::set<std::string>& out = callees[mir::function_key(f)];
    const auto called = [&](const std::string& key) {
      if (callees.count(key)) out.insert(key);
    };
    std::function<void(const mir::Expr&)> expr = [&](const mir::Expr& e) {
      called(mir::function_key(e));
      for (const mir::Expr& arg : e.args) expr(arg);
    };
    std::function<void(const mir::Stmt&)> stmt = [&](const mir::Stmt& s) {
      if (!s.fn_name.empty()) called(mir::function_key(s));
      mir::for_each_expr(s, expr);
      for (const mir::Stmt& child : s.body) stmt(child);
    };
    for (const mir::Stmt& s : f.body) stmt(s);
  }
  // Depth-first search for a back edge: 1 is on the current path, 2 done.
  std::map<std::string, int> state;
  std::function<bool(const std::string&)> cycle = [&](const std::string& f) {
    int& here = state[f];
    if (here) return here == 1;
    here = 1;
    for (const std::string& g : callees[f])
      if (cycle(g)) return true;
    state[f] = 2;
    return false;
  };
  for (const auto& [f, unused] : callees)
    if (cycle(f)) return true;
  return false;
}

}  // namespace

CompiledModel compile_model(const std::string& mir_text, const DataMap& data,
                            unsigned seed) {
  return compile_model(mir_text, data, seed, CompileOptions{});
}
CompiledModel compile_model(const std::string& mir_text, const DataMap& data,
                            unsigned seed, const CompileOptions& options) {
  std::optional<stan::math::ChainableStack> autodiff_stack_of_caller;
  if (thread_safe_build() && stan::math::ChainableStack::instance_ == nullptr)
    autodiff_stack_of_caller.emplace();
  const bool report_enabled =
      options.execution_report || execution_reporting_enabled();
  ExecutionTrace execution_trace;
  std::optional<ExecutionTraceScope> execution_scope;
  if (report_enabled) execution_scope.emplace(execution_trace);
  if (options.reduce_sum_threads < 1 || options.reduce_sum_min_chunk_ns < 0 ||
      options.reduce_sum_max_chunks < 1)
    throw std::invalid_argument("invalid reduce_sum compilation options");
  const char* prep_env = debug_env("STANLI_PROFILE_PREP");
  PrepTrace prep(prep_env && prep_env[0] != '0');
  PassDumper dumper = options.dump_passes.empty()
                          ? PassDumper(debug_env("STANLI_DUMP_PASSES"),
                                       debug_env("STANLI_DUMP_STAGES"))
                          : PassDumper(options.dump_passes.c_str(),
                                       options.dump_stages.c_str());
  if (dumper.enabled()) dumper.write("mir", "mir.txt", mir_text);
  const auto compile_time = prep.start();
  // Shared because the interpreted write_array fallback, when needed,
  // keeps the generate_quantities statements and UDF bodies alive for the
  // model's whole life.
  const auto parse_time = prep.start();
  auto prog = std::make_shared<mir::Program>(decode_program(mir_text));
  if (needs_large_stack(prog->nesting) ||
      (can_move_to_large_stack() && has_recursive_functions(*prog))) {
    try {
      run_on_large_stack([&] { prog.reset(); });
      return run_on_large_stack(
          [&] { return compile_model(mir_text, data, seed, options); });
    } catch (const NestingTooDeep& e) {
      throw CompileError(e.what());
    }
  }
  prep.plain("compile", "parse_mir", parse_time, PrepTrace::Extra::MirBytes,
             static_cast<int64_t>(mir_text.size()));
  const std::optional<DataMap> reordered =
      tuple_data_in_array_order(*prog, data);
  const DataMap& model_data = reordered ? *reordered : data;
  Lowering lo(model_data, prep, dumper, "log_prob", WaRng(seed));
  lo.compile_options = options;
  if (const char* refuse = std::getenv("STANLI_NO_INTERPRETER");
      refuse != nullptr && refuse[0] != '\0' && std::strcmp(refuse, "0") != 0)
    lo.compile_options.allow_interpreter = false;
  // A second lowering, over the transformed data the first one already
  // interpreted: re-running prepare_data would double preparation time on
  // the models where preparation is the cost (nn_rbm1bJ100, 20.7 s).
  std::optional<WriteArrayLowering> wa_lowering;
  if (!prog->generate_quantities.empty()) {
    wa_lowering.emplace(model_data, *prog, prep.enabled(), dumper,
                        parallel_write_array(options));
    lo.after_data_prepared = [&] { wa_lowering->begin(lo); };
  }
  auto specialized = try_bounded_specialization(lo, *prog);
  const int64_t budget =
      switch_value("STANLI_PROGRAM_BUDGET", kProgramBudgetBytes);
  const int64_t loop_budget =
      switch_value("STANLI_LOOP_BUDGET", kLoopBudgetBytes);
  const size_t ceiling = static_cast<size_t>(std::max<int64_t>(
      0, switch_value("STANLI_UNROLL_CEILING", (int64_t)kUnrollCeilingOps)));
  std::optional<CompiledModel> lowered = std::move(specialized);
  const bool specialized_path = lowered.has_value();
  const mir::Stmt* past_ceiling = nullptr;
  if (!lowered) {
    lo.unroll_ceiling = ceiling;
    try {
      lowered = lo.run(*prog);
    } catch (const Lowering::UnrollCeiling& c) {
      past_ceiling = c.loop;
    }
  }
  lo.restore_shadowed_prepared();
  if (!lowered) {
    lowered = lower_compact(lo, *prog, {}, {past_ceiling}, ceiling, true);
  } else if (!specialized_path) {
    const bool op_loop = Executor::program_bytes(lowered->graph) > loop_budget;
    const auto loops =
        loops_over_budget(lo.unrolled_loops, lowered->graph, budget, op_loop);
    if (!loops.empty()) {
      auto compact = lower_compact(lo, *prog, loops, {}, ceiling, op_loop);
      if (compact && Executor::program_bytes(compact->graph) <
                         Executor::program_bytes(lowered->graph))
        lowered = std::move(compact);
    }
  }
  CompiledModel cm = std::move(*lowered);
  for (const mir::OutputVar& out : prog->output_vars)
    if (!out.annotations.empty())
      cm.annotations.emplace_back(out.name, out.annotations);
  lo.after_data_prepared = nullptr;
  // The interpreters below only read data and transformed data, and nothing
  // reads the log_prob lowering's environment after this, so they share it.
  std::shared_ptr<const std::map<std::string, DataMap::Entry>> interp_env;
  const auto shared_interp_env = [&] {
    if (!interp_env)
      interp_env =
          std::make_shared<const std::map<std::string, DataMap::Entry>>(
              std::move(lo.td.env()));
    return interp_env;
  };
  if (wa_lowering) {
    CompiledModel::WriteArray w = wa_lowering->finish(lo, prep);
    for (const std::string& note : wa_lowering->interpreter_fallbacks())
      add_interpreter_fallback(cm, note);
    wa_lowering.reset();
    if (w.n_unconstrained != cm.n_unconstrained) {
      // The two graphs read the same draw; if they disagree on its length the
      // write_array cannot be driven at all. Keep the model, drop the columns,
      // and say so rather than emitting a silently misaligned CSV.
      w.truncated = "write_array reads " + std::to_string(w.n_unconstrained) +
                    " unconstrained values, log_prob reads " +
                    std::to_string(cm.n_unconstrained) +
                    (w.truncated ? "; " + *w.truncated : "");
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
    if (w.truncated || debug_env("STANLI_WA_FORCE_INTERP")) {
      // The graph could not express the whole section; hand the model the
      // per-draw interpreter, seeded with data + transformed data and the
      // emission flags the guard blocks test.
      std::map<std::string, DataMap::Entry> flags;
      for (const char* flag :
           {"emit_transformed_parameters__", "emit_generated_quantities__"}) {
        DataMap::Entry one;
        one.is_int = true;
        one.i = {1};
        one.r = {1.0};
        flags[flag] = one;
      }
      w.interp = std::make_shared<WaInterp>(prog, shared_interp_env(),
                                            std::move(flags));
    }
    cm.write_array = std::move(w);
    if (cm.write_array->truncated)
      add_interpreter_fallback(cm, *cm.write_array->truncated);
  }
  {
    // The inverse parameter transforms. Nothing is interpreted here: the
    // section runs only when a caller actually supplies constrained starting
    // values, so a model nobody inits by name never pays for a bound
    // expression this build cannot evaluate.
    CompiledModel::TransformInits ti;
    std::vector<InitParam> params;
    for (const CompiledModel::ParamLeaf& leaf : cm.param_leaves) {
      const CompiledModel::ParamView& view = cm.leaf_view(leaf);
      InitParam p;
      p.name = view.key();
      p.key = leaf.init_key;
      p.dims = view.dims;
      p.constrained_len = view.len;
      p.free_len = leaf.free_len;
      // The leaf is the unit the arena keeps contiguous inside each
      // element of the surrounding array. An innermost matrix is one
      // whatever the transform is -- the arena stores it column-major
      // while a serial init lists it first-index-fastest, and an
      // elementwise transform over an array of matrices has to cross that
      // permutation too. Otherwise only a structured transform has a leaf;
      // an elementwise one treats each value on its own, which for a
      // vector or a plain array is the same enumeration either way.
      p.leaf_rank = view.matrix_storage                   ? 2
                    : is_structured_check(leaf.transform) ? 1
                                                          : 0;
      if ((size_t)p.leaf_rank > p.dims.size()) {
        ti.truncated = view.name +
                       " declares fewer dimensions than its "
                       "transform needs";
        break;
      }
      params.push_back(std::move(p));
    }
    if (!ti.truncated)
      ti.interp = std::make_shared<InitInterp>(prog, shared_interp_env(),
                                               std::move(params));
    cm.transform_inits = std::move(ti);
  }
  if (!cm.interpreter_fallbacks.empty() &&
      !lo.compile_options.allow_interpreter)
    throw CompileError(interpreter_error(cm));
  prep.plain("compile", "total", compile_time);
  prep.report();
  cm.execution_report = report_enabled;
  if (report_enabled) {
    execution_trace.check();
    emit_diagnostic(execution_report(cm, &execution_trace));
  }
  return cm;
}

namespace {

// The names of free values with logical shape `dims`, after `prefix`.
// Structured matrix leaves are flat in `dims`, so a simplex contributes K-1
// indices and a structured matrix its flat raw width.
void append_free_names(const std::string& prefix,
                       const std::vector<int64_t>& dims, int64_t len,
                       std::vector<std::string>& out) {
  int64_t prod = 1;
  bool sane = true;
  for (int64_t d : dims) {
    if (d < 0) sane = false;
    prod *= d;
  }
  if (sane && prod == len && !dims.empty()) {
    // Last-index-major, the way Stan serializes everything: the FIRST
    // index varies fastest, so matrix[2,3] m is m.1.1, m.2.1, m.1.2, ...
    std::vector<int64_t> idx(dims.size(), 0);
    for (int64_t k = 0; k < len; ++k) {
      std::string s = prefix;
      for (size_t d = 0; d < idx.size(); ++d)
        s += "." + std::to_string(idx[d] + 1);
      out.push_back(std::move(s));
      for (size_t d = 0; d < idx.size(); ++d) {
        if (++idx[d] < dims[d]) break;
        idx[d] = 0;
      }
    }
    return;
  }
  if (dims.empty() && len == 1) {
    out.push_back(prefix);
    return;
  }
  for (int64_t i = 0; i < len; ++i)
    out.push_back(prefix + "." + std::to_string(i + 1));
}

}  // namespace

void CompiledModel::UncParam::append_names(
    std::vector<std::string>& out) const {
  if (leaves.empty()) return append_free_names(name, dims, len, out);
  // A tuple element is named by each array index before a slot and the
  // slot after ':', then its own free values: g.2.1:2.3.
  for_each_element([&](size_t k, const std::vector<int64_t>& index) {
    const Leaf& leaf = leaves[k];
    std::string prefix = name;
    size_t used = 0;
    for (const auto& [before, component] : leaf.path) {
      for (; used < before; ++used)
        prefix += "." + std::to_string(index[used] + 1);
      prefix += ":" + std::to_string(component);
    }
    const std::vector<int64_t> own(leaf.dims.begin() + (std::ptrdiff_t)used,
                                   leaf.dims.end());
    append_free_names(prefix, own, leaf.len / leaf.elements(), out);
  });
}

std::string interpreter_error(const CompiledModel& cm) {
  return "stanli: the MIR interpreter is not allowed (allow_interpreter is "
         "false, or STANLI_NO_INTERPRETER is set) and parts of this model "
         "have no compiled path: " +
         fallback_items(cm) +
         ". Allow it to run them through the MIR interpreter, which is far "
         "slower. " +
         report_request();
}

std::string interpreter_warning(const CompiledModel& cm) {
  if (cm.interpreter_fallbacks.empty()) return "";
  return "stanli: parts of this model have no compiled path and run through "
         "the MIR interpreter, which is far slower: " +
         fallback_items(cm) + ". " + report_request() +
         " Set STANLI_NO_INTERPRETER=1 to refuse such models.";
}

std::string interpreter_probe_error(const CompiledModel& cm,
                                    const std::string& failure) {
  return "stanli: transformed parameters and generated quantities run "
         "through the MIR interpreter here, and it failed at every probe "
         "point (" +
         failure + "), so their columns cannot be found. " +
         (cm.interpreter_fallbacks.empty()
              ? std::string()
              : "No compiled path because: " + fallback_items(cm) + ". ") +
         report_request();
}

}  // namespace stanli
