#include <stanli/compile.hpp>
#include <stanli/constfold.hpp>
#include <stanli/inplace.hpp>
#include <stanli/mir_prog.hpp>
#include <stanli/mir.hpp>
#include <stanli/mir_interp.hpp>
#include <stanli/ode.hpp>
#include <stanli/optable.hpp>
#include <stanli/island.hpp>
#include <stanli/reroll.hpp>
#include <stanli/sexp.hpp>
#include <stanli/structured_check.hpp>
#include <stanli/wa_interp.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <chrono>
#include <functional>
#include <initializer_list>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace stanli {
namespace {

using ShapeId = uint32_t;

// Opt-in lowering telemetry.  Preparation is normally too short to justify
// putting clocks (or even formatting) on the path, so STANLI_PROFILE_PREP is
// deliberately separate from the executor's STANLI_PROFILE and the disabled
// path never calls the clock. Rows are buffered until every timed compile
// stage is done: stderr I/O must not become part of a later pass's timing.
struct PrepTrace {
  using Clock = std::chrono::steady_clock;
  using Time = Clock::time_point;

  enum class Extra {
    None,
    Rewrites,
    Removed,
    ConstFold,
    Reroll,
    Regions,
    Truncated,
    MirBytes,
  };

  struct Row {
    const char* graph = nullptr;
    const char* stage = nullptr;
    int64_t ns = 0;
    int64_t ops = -1;
    int64_t slots = -1;
    int64_t fills = -1;
    int64_t terms = -1;
    int64_t views = -1;
    Extra extra = Extra::None;
    int64_t a = 0;
    int64_t b = 0;
    int64_t c = 0;
    int64_t d = 0;
    bool deep = false;
    int64_t params = 0;
    int64_t slot_elems = 0;
    int64_t fill_elems = 0;
    int64_t idata_arrays = 0;
    int64_t idata_elems = 0;
    int64_t udata = 0;
  };

  explicit PrepTrace(bool enabled) : enabled_(enabled) {}

  Time start() const { return enabled_ ? Clock::now() : Time{}; }

  void plain(const char* graph, const char* stage, Time from,
             Extra extra = Extra::None, int64_t a = 0) {
    if (!enabled_) return;
    Row& r = next();
    r.graph = graph;
    r.stage = stage;
    r.ns = elapsed(from);
    r.extra = extra;
    r.a = a;
  }

  void graph(const char* graph_name, const char* stage, Time from,
             const Graph& g,
             const std::vector<std::pair<int, std::vector<double>>>& fills,
             size_t terms, size_t views, Extra extra = Extra::None,
             int64_t a = 0, int64_t b = 0, bool deep = false,
             int64_t params = 0, int64_t c = 0, int64_t d = 0) {
    if (!enabled_) return;
    Row& r = next();
    r.graph = graph_name;
    r.stage = stage;
    // Stop the timer before any diagnostic scan below.
    r.ns = elapsed(from);
    r.ops = static_cast<int64_t>(g.ops.size());
    r.slots = static_cast<int64_t>(g.slots.size());
    r.fills = static_cast<int64_t>(fills.size());
    r.terms = static_cast<int64_t>(terms);
    r.views = static_cast<int64_t>(views);
    r.extra = extra;
    r.a = a;
    r.b = b;
    r.c = c;
    r.d = d;
    r.deep = deep;
    r.params = params;
    if (deep) {
      for (const Slot& s : g.slots) r.slot_elems += s.len;
      for (const auto& f : fills)
        r.fill_elems += static_cast<int64_t>(f.second.size());
      r.idata_arrays = static_cast<int64_t>(g.idata_pool.size());
      for (const auto& v : g.idata_pool)
        r.idata_elems += static_cast<int64_t>(v.size());
      r.udata = static_cast<int64_t>(g.udata_pool.size());
    }
  }

  void report() const {
    if (!enabled_) return;
    for (size_t i = 0; i < size_; ++i) {
      const Row& r = rows_[i];
      std::string line = "stanli_prep graph=" + std::string(r.graph) +
                         " stage=" + r.stage + " ns=" + std::to_string(r.ns);
      const auto field = [&](const char* name, int64_t value) {
        line += " ";
        line += name;
        line += "=";
        line += std::to_string(value);
      };
      if (r.ops >= 0) {
        field("ops", r.ops);
        field("slots", r.slots);
        field("fills", r.fills);
        field("terms", r.terms);
        field("views", r.views);
      }
      switch (r.extra) {
        case Extra::Rewrites:
          field("rewrites", r.a);
          break;
        case Extra::Removed:
          field("removed", r.a);
          break;
        case Extra::ConstFold:
          field("ops_removed", r.a);
          field("slots_folded", r.b);
          break;
        case Extra::Reroll:
          field("regions", r.a);
          field("row_steps", r.d);
          field("list_steps", r.b);
          field("candidate_steps", r.c);
          break;
        case Extra::Regions:
          field("regions", r.a);
          break;
        case Extra::Truncated:
          field("truncated", r.a);
          break;
        case Extra::MirBytes:
          field("mir_bytes", r.a);
          break;
        case Extra::None:
          break;
      }
      if (r.deep) {
        field("params", r.params);
        field("slot_elems", r.slot_elems);
        field("fill_elems", r.fill_elems);
        field("idata_arrays", r.idata_arrays);
        field("idata_elems", r.idata_elems);
        field("udata", r.udata);
      }
      std::fprintf(stderr, "%s\n", line.c_str());
    }
  }

 private:
  bool enabled_ = false;
  std::array<Row, 32> rows_{};
  size_t size_ = 0;

  int64_t elapsed(Time from) const {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
                                                                from)
        .count();
  }

  Row& next() {
    // There are currently 20 rows with a write_array graph. Keep this a fixed
    // buffer so the profiler itself cannot show up as allocator work.
    if (size_ >= rows_.size()) std::abort();
    return rows_[size_++];
  }
};

struct SlotInfo {
  int64_t rows = 0, cols = 0;  // set for matrices
  bool param_free = false;     // independent of every model parameter
  ViewKind kind = ViewKind::Flat;
  ShapeId shape = 0;  // nonzero exactly for Array values
};
static_assert(sizeof(SlotInfo) == 24);

// A proof that every definitely initialized value in a graph slot is an
// integral double in this closed interval.  Coverage is tracked separately;
// this is deliberately outside SlotInfo because logical shape and parameter
// provenance survive much more broadly than the narrow write_array grammar.
struct IntRange {
  int32_t lo = 0;
  int32_t hi = 0;
};

bool is_matrix(const SlotInfo& si) { return si.kind == ViewKind::Matrix; }
bool is_vector(const SlotInfo& si) { return si.kind == ViewKind::Vector; }
bool is_row_vector(const SlotInfo& si) {
  return si.kind == ViewKind::RowVector;
}
bool is_array(const SlotInfo& si) { return si.kind == ViewKind::Array; }

int leaf_rank(ViewKind kind) {
  if (kind == ViewKind::Matrix) return 2;
  if (kind == ViewKind::Vector || kind == ViewKind::RowVector) return 1;
  return 0;
}

struct ArrayShape {
  std::vector<int64_t> dims;  // array extents followed by leaf extents
  ViewKind leaf = ViewKind::Flat;
};

struct ShapeInterner {
  std::vector<ArrayShape> shapes{{}};  // id 0 is reserved for non-arrays
  std::map<std::pair<std::vector<int64_t>, ViewKind>, ShapeId> ids;

  ShapeId intern(std::vector<int64_t> dims, ViewKind leaf) {
    if (leaf == ViewKind::Array)
      throw CompileError("stanli compile: array shape has an array leaf");
    if (dims.size() <= (size_t)leaf_rank(leaf))
      throw CompileError("stanli compile: incomplete logical array shape");
    int64_t n = 1;
    for (int64_t d : dims) {
      if (d < 0 || (d != 0 && n > std::numeric_limits<int64_t>::max() / d))
        throw CompileError(
            "stanli compile: invalid or overflowing logical array extent");
      n *= d;
    }
    const auto key = std::make_pair(dims, leaf);
    auto it = ids.find(key);
    if (it != ids.end()) return it->second;
    const ShapeId id = static_cast<ShapeId>(shapes.size());
    shapes.push_back(ArrayShape{std::move(dims), leaf});
    ids.emplace(std::make_pair(shapes.back().dims, leaf), id);
    return id;
  }

  const ArrayShape& at(ShapeId id) const {
    if (id == 0 || id >= shapes.size())
      throw CompileError("stanli compile: invalid array shape id");
    return shapes[id];
  }
};

// Where an index path lands in a flat buffer. `stride` is 1 unless the path
// stops on a matrix row, which is not contiguous in column-major storage.
struct Addr {
  int64_t off = 0, len = 1, stride = 1;
};

// The one place that knows how a declared value is laid out.
//
// Dims are outer-to-inner, `mat` says the last two are a matrix's rows and
// columns. Array dims are array-major (outer slowest, each element
// contiguous); a matrix element is column-major inside. Those two rules
// disagree about the last two indices, which is exactly the mistake that
// transposed every array of matrices in the CSV while leaving the log
// density right, so the read path, the write path and the data repack all
// come here rather than each walking the strides themselves.
Addr flat_addr(const std::vector<int64_t>& D, bool mat,
               const std::vector<int64_t>& ix) {
  const size_t n_arr = D.size() - (mat ? 2 : 0);
  const int64_t rows = mat ? D[n_arr] : 0;
  const int64_t elem = mat ? rows * D[n_arr + 1] : 1;
  Addr a;
  int64_t stride = elem;
  for (size_t d = n_arr; d-- > 0;) {
    if (d < ix.size()) a.off += ix[d] * stride;
    stride *= D[d];
  }
  if (ix.size() <= n_arr) {  // whole elements, contiguous
    a.len = elem;
    for (size_t d = ix.size(); d < n_arr; ++d) a.len *= D[d];
    return a;
  }
  if (ix.size() == n_arr + 1) {  // one row of an element
    a.off += ix[n_arr];
    a.len = D[n_arr + 1];
    a.stride = rows;
    return a;
  }
  a.off += ix[n_arr + 1] * rows + ix[n_arr];  // one cell
  return a;
}

std::vector<double> graph_order(const DataMap::Entry& en,
                                bool standalone_matrix, bool innermost_matrix) {
  std::vector<double> vals = en.r;
  if (standalone_matrix || en.dims.size() <= 1) return vals;
  std::vector<int64_t> ix(en.dims.size(), 0);
  for (size_t src = 0; src < en.r.size(); ++src) {
    int64_t t = (int64_t)src;
    for (size_t d = 0; d < en.dims.size(); ++d) {
      ix[d] = t % en.dims[d];
      t /= en.dims[d];
    }
    vals[(size_t)flat_addr(en.dims, innermost_matrix, ix).off] = en.r[src];
  }
  return vals;
}

struct Lowering {
  struct Val {
    int slot;
    bool autodiff = false;  // instantiated C++ scalar type carries var
    SlotInfo si;
  };
  static_assert(sizeof(Val) == 32);

  const DataMap& data;
  std::shared_ptr<ShapeInterner> shape_pool;
  PrepTrace& prep;
  const char* prep_graph;
  // The MIR interpreter instance for everything DataOnly: prepare_data,
  // data-only conditions, size expressions. Its environment doubles as the
  // lowering's view of transformed data. Hooks route FnReadData to the
  // DataMap and unknown variables to the unrolled-loop int environment.
  MirInterp<double> td{
      fun_defs, "prepare_data",
      MirHooks{[this](const std::string& n) -> const DataMap::Entry* {
                 return data.has(n) ? &data.at(n) : nullptr;
               },
               [this](const std::string& n, long* out) {
                 auto it = int_env.find(n);
                 if (it == int_env.end()) return false;
                 *out = it->second;
                 return true;
               }}};
  Graph g;
  CompiledModel out;
  std::map<std::string, Val> scope;     // var -> value and logical view
  std::map<std::string, long> int_env;  // data int scalars
  std::map<int, IntRange> int_ranges;   // runtime integral slot provenance
  // Definite initialization proof for the target construction grammar.
  // Writes must extend one contiguous prefix; gaps/strides fail closed.
  std::map<int, int64_t> int_initialized_prefix;
  std::map<double, int> const_cache;
  struct ObservationKey {
    int slot;
    SlotInfo si;
    bool operator<(const ObservationKey& b) const {
      return std::tie(slot, si.kind, si.shape, si.rows, si.cols) <
             std::tie(b.slot, b.si.kind, b.si.shape, b.si.rows, b.si.cols);
    }
  };
  // Interpreter-native semantic values. Graph fills use a different physical
  // order for arrays, so compile-time observation must never reuse them.
  std::map<ObservationKey, DataMap::Entry> observations;
  std::vector<int> target_terms;
  std::vector<int> jac_slots;
  std::map<std::string, const mir::FunDef*> fun_defs;
  // A generic UDF keeps one scalar template type per formal. Locals and its
  // return use the promoted type, but a direct formal reference keeps its own.
  std::map<std::string, bool> udf_formal_autodiff;
  std::map<std::string, bool> effectful_cache;
  std::set<std::string> effectful_visiting;
  std::set<std::string> int_locals;  // SInt locals in log_prob (data-only)
  int udf_depth = 0;
  // int_env as bind_data left it, before either section's locals and loop
  // variables were folded in; the write_array lowering starts from this.
  std::map<std::string, long> int_env_data;
  // Lowering generate_quantities rather than log_prob: parameters are columns
  // to emit, not values to differentiate.
  bool in_write_array = false;
  // Where the emission guards fell, as counts of columns emitted before
  // each. Unset until the guard is reached (a section can be missing from
  // the MIR entirely), which run_write_array then reads as "at the end".
  std::optional<size_t> n_tp_start, n_gq_start;
  // CmdStan's propto__ template parameter, threaded by lower_call_udf: a
  // density inside an inlined user function is unnormalized only if the
  // call that reached it was.
  bool propto_ctx = true;
  // OR of the actual real/container scalar types for the current inlined
  // UDF. Generic AutoDiffable locals and returns instantiate to this type.
  bool udf_autodiff_ctx = false;
  bool scalar_autodiff() const {
    return udf_depth == 0 ? !in_write_array : udf_autodiff_ctx;
  }
  bool propto(const mir::Expr& e) const { return e.fn_propto && propto_ctx; }

  // Static C++ scalar type without lowering/evaluating the expression. This
  // is intentionally small: ordinary ops propagate Val::autodiff from their
  // lowered operands; only a data-condition ternary needs the unchosen arm.
  bool expression_autodiff(const mir::Expr& e) const {
    if (e.unsized.leaf == mir::UnsizedLeaf::Int || e.data_only) return false;
    if (e.promoted) return scalar_autodiff();
    if (e.kind == mir::Expr::Var) {
      const auto formal = udf_formal_autodiff.find(e.name);
      if (formal != udf_formal_autodiff.end()) return formal->second;
      const auto value = scope.find(e.name);
      return value != scope.end() ? value->second.autodiff : scalar_autodiff();
    }
    if (e.kind == mir::Expr::TernaryIf && e.args.size() == 3)
      return expression_autodiff(e.args[1]) || expression_autodiff(e.args[2]);
    bool autodiff = false;
    for (const mir::Expr& arg : e.args)
      autodiff = autodiff || expression_autodiff(arg);
    return autodiff;
  }

  explicit Lowering(
      const DataMap& d, PrepTrace& p, const char* graph_name,
      std::shared_ptr<ShapeInterner> pool = std::make_shared<ShapeInterner>())
      : data(d), shape_pool(std::move(pool)), prep(p), prep_graph(graph_name) {}

  void observe(const Val& v, DataMap::Entry en) {
    const int64_t len = g.slots[v.slot].len;
    validate_view(v.si, len, "observed value");
    if ((int64_t)en.r.size() != len)
      fail("observed value length does not match logical storage");
    if (en.is_int && !en.i.empty() && (int64_t)en.i.size() != len)
      fail("observed integer length does not match logical storage");
    if (is_array(v.si))
      en.dims = array_shape(v.si).dims;
    else if (is_matrix(v.si))
      en.dims = {v.si.rows, v.si.cols};
    else if (is_vector(v.si) || is_row_vector(v.si))
      en.dims = {len};
    else
      en.dims.clear();
    observations[{v.slot, v.si}] = std::move(en);
  }
  const DataMap::Entry* observation(const Val& v) const {
    auto it = observations.find({v.slot, v.si});
    return it == observations.end() ? nullptr : &it->second;
  }

  int add_slot(int64_t len, bool is_param) { return g.add_slot(len, is_param); }

  [[noreturn]] void fail(const std::string& msg, const std::string& raw = "") {
    throw CompileError("stanli compile: " + msg +
                       (raw.empty() ? "" : " | in: " + raw));
  }

  // Every index the graph lowering sees is a bind-time constant, so the
  // bounds CmdStan checks at runtime are checked here, before an op that
  // would silently read a neighboring arena slot can be emitted.
  void check_index(int64_t i, int64_t n, const char* what,
                   const std::string& raw) {
    if (i < 1 || i > n)
      fail(std::string(what) + ": index " + std::to_string(i) +
               " out of bounds for size " + std::to_string(n),
           raw);
  }
  // A range with hi < lo is empty and never reads, whatever the endpoints.
  void check_range(int64_t lo, int64_t hi, int64_t n, const char* what,
                   const std::string& raw) {
    if (hi >= lo && (lo < 1 || hi > n))
      fail(std::string(what) + ": range [" + std::to_string(lo) + ", " +
               std::to_string(hi) + "] out of bounds for size " +
               std::to_string(n),
           raw);
  }

  int const_slot(double v) {
    auto it = const_cache.find(v);
    if (it != const_cache.end()) return it->second;
    const int s = add_slot(1, false);
    out.fills.emplace_back(s, std::vector<double>{v});
    const_cache[v] = s;
    return s;
  }

  Val constant(double v) {
    Val out{const_slot(v), false, SlotInfo{0, 0, true}};
    DataMap::Entry en;
    en.r = {v};
    observe(out, std::move(en));
    return out;
  }

  void set_int_range(const Val& v, int64_t lo, int64_t hi) {
    int_initialized_prefix[v.slot] = g.slots[v.slot].len;
    if (lo < std::numeric_limits<int32_t>::min() ||
        hi > std::numeric_limits<int32_t>::max() || lo > hi) {
      int_ranges.erase(v.slot);
      return;
    }
    int_ranges[v.slot] =
        IntRange{static_cast<int32_t>(lo), static_cast<int32_t>(hi)};
  }

  void set_int_initialized(const Val& v) {
    int_initialized_prefix[v.slot] = g.slots[v.slot].len;
    int_ranges.erase(v.slot);
  }

  void set_uninitialized_int_array(const Val& v) {
    int_ranges.erase(v.slot);
    int_initialized_prefix[v.slot] = 0;
  }

  // Target models build int arrays in ascending contiguous writes.  Track the
  // initialized prefix in O(1) per immutable slot: overwrites inside it are
  // safe, an adjacent write extends it, and any gap/stride fails closed.  The
  // interval hull may retain overwritten values, conservatively widening the
  // later overflow proof.
  void propagate_int_update(const Val& out_v, const Val& base, const Val& rhs,
                            int64_t start, int64_t stride) {
    const auto base_prefix = int_initialized_prefix.find(base.slot);
    const auto rhs_prefix = int_initialized_prefix.find(rhs.slot);
    const int64_t rhs_len = g.slots[rhs.slot].len;
    if (rhs_len == 0 && base_prefix != int_initialized_prefix.end() &&
        g.slots[out_v.slot].len == g.slots[base.slot].len) {
      int_initialized_prefix[out_v.slot] = base_prefix->second;
      const auto base_range = int_ranges.find(base.slot);
      if (base_range == int_ranges.end())
        int_ranges.erase(out_v.slot);
      else
        int_ranges[out_v.slot] = base_range->second;
      return;
    }
    if (base_prefix == int_initialized_prefix.end() ||
        rhs_prefix == int_initialized_prefix.end() ||
        rhs_prefix->second != rhs_len || stride != 1 || start < 0 ||
        start > base_prefix->second || rhs_len < 0 ||
        start > g.slots[out_v.slot].len - rhs_len ||
        g.slots[out_v.slot].len != g.slots[base.slot].len) {
      int_ranges.erase(out_v.slot);
      int_initialized_prefix.erase(out_v.slot);
      return;
    }
    int_initialized_prefix[out_v.slot] =
        std::max(base_prefix->second, start + rhs_len);

    const auto rhs_range = int_ranges.find(rhs.slot);
    if (rhs_range == int_ranges.end()) {
      int_ranges.erase(out_v.slot);
      return;
    }
    IntRange range = rhs_range->second;
    if (base_prefix->second > 0) {
      const auto base_range = int_ranges.find(base.slot);
      if (base_range == int_ranges.end()) {
        int_ranges.erase(out_v.slot);
        return;
      }
      range.lo = std::min(range.lo, base_range->second.lo);
      range.hi = std::max(range.hi, base_range->second.hi);
    }
    int_ranges[out_v.slot] = range;
  }

  long eval_int(const mir::Expr& e) {
    if (expr_effectful(e))
      fail("effectful expression cannot be used as a compile-time integer",
           e.raw);
    switch (e.kind) {
      case mir::Expr::LitInt:
        return e.lit_i;
      case mir::Expr::Var: {
        auto it = int_env.find(e.name);
        if (it != int_env.end()) return it->second;
        DataMap::Entry* en = td.find(e.name);
        if (en && en->is_int && en->i.size() == 1) return en->i[0];
        fail("size expression needs unknown int " + e.name);
      }
      case mir::Expr::Indexed: {
        DataMap::Entry* en = e.args[0].kind == mir::Expr::Var
                                 ? td.find(e.args[0].name)
                                 : nullptr;
        if (en && en->is_int && e.args.size() == 2 &&
            e.args[1].name == "IndexSingle")
          return en->i.at(eval_int(e.args[1].args[0]) - 1);
        if (en && en->is_int && e.args.size() == 3 &&
            e.args[1].name == "IndexSingle" &&
            e.args[2].name == "IndexSingle" && en->dims.size() == 2)
          return en->i.at((eval_int(e.args[2].args[0]) - 1) * en->dims[0] +
                          (eval_int(e.args[1].args[0]) - 1));
        // dims(x)[k] and friends: evaluate the base as a compile-time
        // sequence, then index it.
        {
          std::vector<int> vals = const_ints(e.args[0]);
          if (e.args.size() == 2 && e.args[1].name == "IndexSingle") {
            const long ix = eval_int(e.args[1].args[0]);
            if (ix >= 1 && (size_t)ix <= vals.size()) return vals[ix - 1];
          }
        }
        fail("unsupported int index expression", e.raw);
      }
      case mir::Expr::FunApp:
        if (e.name == "Plus__")
          return eval_int(e.args[0]) + eval_int(e.args[1]);
        if (e.name == "Minus__")
          return eval_int(e.args[0]) - eval_int(e.args[1]);
        if (e.name == "Times__")
          return eval_int(e.args[0]) * eval_int(e.args[1]);
        // Shape queries on slot-bound values (e.g. rows(v) on an inlined
        // UDF's vector argument) answer from binding-owned metadata before
        // the interpreter, which cannot recover vector orientation.
        if ((e.name == "rows" || e.name == "cols" || e.name == "size" ||
             e.name == "num_elements" || e.name == "FnLength") &&
            e.args.size() == 1 && e.args[0].kind == mir::Expr::Var) {
          auto sit = scope.find(e.args[0].name);
          if (sit != scope.end()) {
            const SlotInfo& si = sit->second.si;
            const int64_t len = g.slots[sit->second.slot].len;
            if (is_array(si)) {
              const ArrayShape& sh = array_shape(si);
              if (e.name == "size" || e.name == "FnLength")
                return sh.dims.front();
              if (e.name == "num_elements") return len;
              fail(e.name + " is undefined for an array value", e.raw);
            }
            const LogicalDims dims = logical_dims(si, len, e.name);
            if (e.name == "rows") return dims.rows;
            if (e.name == "cols") return dims.cols;
            return len;
          }
          auto dl = decls.find(e.args[0].name);
          if (dl != decls.end()) {
            const DeclView& sh = dl->second;
            if (is_array(sh.si)) {
              const ArrayShape& arr = array_shape(sh.si);
              if (e.name == "size" || e.name == "FnLength")
                return arr.dims.front();
              if (e.name == "num_elements") return sh.len;
              fail(e.name + " is undefined for an array declaration", e.raw);
            }
            const LogicalDims dims = logical_dims(sh.si, sh.len, e.name);
            if (e.name == "rows") return dims.rows;
            if (e.name == "cols") return dims.cols;
            return sh.len;
          }
          // A name td knows but neither scope nor decls does: the scalar
          // `int` input. bind_data fills both tables from a declared shape
          // and a scalar int has none, so it falls past both -- the one
          // case, not the none this used to claim. The data_only branch
          // below does not catch it either, because a shape query in a
          // real-valued context is not data_only: `real p = size(n)` in
          // transformed parameters is AutoDiffable, so it reached the
          // failure instead and cost the census stanc3's
          // function-signatures/math/matrix/size.stan.
          //
          // Asking the interpreter is what the previous copy here should
          // have done all along. It answers rows/cols off the MIR type, so
          // the rank-1 orientation bug that copy carried cannot come back
          // through this route.
          if (td.find(e.args[0].name)) {
            try {
              return td.as_int(e);
            } catch (const CompileError&) {
            }
          }
        }
        // Shape query on a COMPUTED value: --O1 inlining substitutes call
        // arguments into the callee's size expressions, so `rows(beta)`
        // arrives as `rows(segment(beta, pos[i], m[i]))`. Lower the
        // argument and answer from its slot metadata; any op this emits
        // is one the body was about to emit anyway.
        if ((e.name == "rows" || e.name == "cols" || e.name == "size" ||
             e.name == "num_elements" || e.name == "FnLength") &&
            e.args.size() == 1 && e.args[0].kind != mir::Expr::Var) {
          const Val v = lower_expr(e.args[0]);
          const int64_t len = g.slots[v.slot].len;
          if (is_array(v.si)) {
            const ArrayShape& sh = array_shape(v.si);
            if (e.name == "size" || e.name == "FnLength")
              return sh.dims.front();
            if (e.name == "num_elements") return len;
            fail(e.name + " is undefined for an array value", e.raw);
          }
          const LogicalDims dims = logical_dims(v.si, len, e.name);
          if (e.name == "rows") return dims.rows;
          if (e.name == "cols") return dims.cols;
          return len;
        }
        // Anything else data-only the td interpreter can evaluate (sum of an
        // int array in a size expression, etc.).
        if (e.data_only) {
          try {
            return td.as_int(e);
          } catch (const CompileError&) {
          }
        }
        fail("unsupported int size function " + e.name, e.raw);
      default:
        fail("unsupported size expression", e.raw);
    }
  }

  int64_t checked_product(const std::vector<int64_t>& dims,
                          const std::string& what) {
    int64_t n = 1;
    for (int64_t d : dims) {
      if (d < 0 || (d != 0 && n > std::numeric_limits<int64_t>::max() / d))
        fail(what + ": invalid or overflowing extent");
      n *= d;
    }
    return n;
  }

  int checked_immediate(int64_t value, const std::string& what) {
    if (value < std::numeric_limits<int>::min() ||
        value > std::numeric_limits<int>::max())
      fail(what + ": dimension exceeds the graph immediate range");
    return static_cast<int>(value);
  }

  static ViewKind leaf_kind(const std::string& base) {
    if (base == "SVector" || base == "UVector") return ViewKind::Vector;
    if (base == "SRowVector" || base == "URowVector")
      return ViewKind::RowVector;
    if (base == "SMatrix" || base == "UMatrix") return ViewKind::Matrix;
    return ViewKind::Flat;
  }

  static ViewKind leaf_kind(mir::UnsizedLeaf leaf) {
    if (leaf == mir::UnsizedLeaf::Vector) return ViewKind::Vector;
    if (leaf == mir::UnsizedLeaf::RowVector) return ViewKind::RowVector;
    if (leaf == mir::UnsizedLeaf::Matrix) return ViewKind::Matrix;
    return ViewKind::Flat;
  }

  std::vector<int64_t> sized_dims(const mir::SizedType& t) {
    std::vector<int64_t> dims;
    dims.reserve(t.dims.size());
    for (const auto& d : t.dims) dims.push_back(eval_int(d));
    return dims;
  }

  int64_t sized_len(const mir::SizedType& t, int64_t* rows = nullptr,
                    int64_t* cols = nullptr) {
    if (t.base == "SInt" || t.base == "SReal") return 1;
    if (t.base == "SVector" || t.base == "SRowVector") {
      const int64_t n = eval_int(t.dims[0]);
      if (n < 0) fail("negative vector extent", t.raw);
      return n;
    }
    if (t.base == "SMatrix") {
      const int64_t r = eval_int(t.dims[0]), c = eval_int(t.dims[1]);
      if (r < 0 || c < 0) fail("negative matrix extent", t.raw);
      if (rows) *rows = r;
      if (cols) *cols = c;
      return checked_product({r, c}, "matrix shape");
    }
    if (t.base == "SArray") {
      return checked_product(sized_dims(t), "array shape");
    }
    fail("unsupported sized type " + t.base, t.raw);
  }

  SlotInfo view_of(const mir::SizedType& t, bool param_free = false) {
    SlotInfo si;
    si.param_free = param_free;
    if (t.base == "SVector")
      si.kind = ViewKind::Vector;
    else if (t.base == "SRowVector")
      si.kind = ViewKind::RowVector;
    else if (t.base == "SMatrix") {
      si.kind = ViewKind::Matrix;
      si.rows = eval_int(t.dims[0]);
      si.cols = eval_int(t.dims[1]);
    } else if (t.base == "SArray") {
      const std::vector<int64_t> dims = sized_dims(t);
      si.kind = ViewKind::Array;
      si.shape = shape_pool->intern(dims, leaf_kind(t.elem_base));
    }
    return si;
  }

  static void stamp_kind(SlotInfo* si, const std::string& type) {
    if (type == "UVector") {
      si->kind = ViewKind::Vector;
      si->rows = si->cols = 0;
    } else if (type == "URowVector") {
      si->kind = ViewKind::RowVector;
      si->rows = si->cols = 0;
    }
  }

  static SlotInfo view_of(const std::string& type) {
    SlotInfo si;
    stamp_kind(&si, type);
    return si;
  }

  static SlotInfo matrix_view(int64_t rows, int64_t cols,
                              bool param_free = false) {
    SlotInfo si;
    si.rows = rows;
    si.cols = cols;
    si.param_free = param_free;
    si.kind = ViewKind::Matrix;
    return si;
  }

  SlotInfo array_view(std::vector<int64_t> dims, ViewKind leaf,
                      bool param_free = false) {
    SlotInfo si;
    si.kind = ViewKind::Array;
    si.shape = shape_pool->intern(std::move(dims), leaf);
    si.param_free = param_free;
    return si;
  }

  const ArrayShape& array_shape(const SlotInfo& si) const {
    return shape_pool->at(si.shape);
  }

  SlotInfo indexed_view(const SlotInfo& base, size_t n_single, int64_t out_len,
                        const std::string& out_type) {
    SlotInfo si = view_of(out_type);
    si.param_free = base.param_free;
    if (!is_array(base)) return si;
    const ArrayShape& a = array_shape(base);
    const size_t outer = a.dims.size() - (size_t)leaf_rank(a.leaf);
    if (n_single < outer) {
      std::vector<int64_t> suffix(a.dims.begin() + n_single, a.dims.end());
      return array_view(std::move(suffix), a.leaf, base.param_free);
    }
    if (n_single == outer) {
      if (a.leaf == ViewKind::Matrix) {
        const size_t n = a.dims.size();
        return matrix_view(a.dims[n - 2], a.dims[n - 1], base.param_free);
      }
      si.kind = a.leaf;
      si.shape = 0;
      return si;
    }
    (void)out_len;
    return si;
  }

  bool same_view(const SlotInfo& a, int64_t alen, const SlotInfo& b,
                 int64_t blen) const {
    if (a.kind != b.kind) return false;
    switch (a.kind) {
      case ViewKind::Flat:
        return a.shape == 0 && b.shape == 0 && alen == 1 && blen == 1;
      case ViewKind::Vector:
      case ViewKind::RowVector:
        return alen == blen;
      case ViewKind::Matrix:
        return a.rows == b.rows && a.cols == b.cols &&
               a.rows * a.cols == alen && b.rows * b.cols == blen;
      case ViewKind::Array:
        return a.shape != 0 && a.shape == b.shape && alen == blen;
    }
    return false;
  }

  bool is_scalar(const Val& v) const {
    return v.si.kind == ViewKind::Flat && v.si.shape == 0 &&
           g.slots[v.slot].len == 1;
  }

  struct LogicalDims {
    int64_t rows;
    int64_t cols;
  };

  LogicalDims logical_dims(const SlotInfo& si, int64_t len,
                           const std::string& what) {
    if (si.kind == ViewKind::Flat) {
      if (si.shape != 0 || len != 1) fail(what + ": malformed scalar view");
      return {1, 1};
    }
    if (is_vector(si)) return {len, 1};
    if (is_row_vector(si)) return {1, len};
    if (is_matrix(si)) {
      if (checked_product({si.rows, si.cols}, what) != len)
        fail(what + ": malformed matrix view");
      return {si.rows, si.cols};
    }
    fail(what + ": array values do not have one rows/cols view");
  }

  std::vector<int64_t> logical_shape(const Val& v, const std::string& what) {
    const int64_t len = g.slots[v.slot].len;
    validate_view(v.si, len, what);
    if (is_array(v.si)) return array_shape(v.si).dims;
    if (is_matrix(v.si)) return {v.si.rows, v.si.cols};
    if (is_vector(v.si) || is_row_vector(v.si)) return {len};
    fail(what + ": dims is unsupported for a scalar value");
  }

  Val lower_dims(const mir::Expr& e) {
    if (e.args.size() != 1) fail("dims arity", e.raw);
    const std::vector<int64_t> dims =
        logical_shape(lower_expr(e.args[0]), "dims");
    std::vector<double> vals(dims.begin(), dims.end());
    const int slot = add_slot((int64_t)vals.size(), false);
    out.fills.emplace_back(slot, vals);
    Val v{slot, false,
          array_view({(int64_t)dims.size()}, ViewKind::Flat, true)};
    DataMap::Entry en;
    en.is_int = true;
    en.r = std::move(vals);
    en.i.assign(dims.begin(), dims.end());
    observe(v, std::move(en));
    return v;
  }

  SlotInfo view_for_dims(const std::string& type, LogicalDims d,
                         bool param_free = false) {
    if (type == "UMatrix") return matrix_view(d.rows, d.cols, param_free);
    SlotInfo si = view_of(type);
    si.param_free = param_free;
    if (type == "UVector" && d.cols == 1) return si;
    if (type == "URowVector" && d.rows == 1) return si;
    if ((type == "UReal" || type == "UInt" || type == "UComplex") &&
        d.rows == 1 && d.cols == 1)
      return si;
    fail("result type does not match logical rows/cols");
  }

  void validate_view(const SlotInfo& si, int64_t len, const std::string& what) {
    if (is_array(si) != (si.shape != 0))
      fail(what + ": array kind and shape id disagree");
    if (si.kind == ViewKind::Flat) {
      if (len != 1) fail(what + ": flat logical value is not a scalar");
      return;
    }
    if (is_matrix(si)) {
      if (checked_product({si.rows, si.cols}, what) != len)
        fail(what + ": matrix extents do not match storage length");
      return;
    }
    if (is_array(si)) {
      if (checked_product(array_shape(si).dims, what) != len)
        fail(what + ": array extents do not match storage length");
      return;
    }
  }

  void require_binding(const Val& v, int64_t len, const SlotInfo& expected,
                       const std::string& name, const std::string& raw = "") {
    validate_view(v.si, g.slots[v.slot].len, "binding " + name);
    validate_view(expected, len, "declaration " + name);
    if (g.slots[v.slot].len != len)
      fail("assignment width mismatch for " + name, raw);
    if (!same_view(v.si, g.slots[v.slot].len, expected, len))
      fail("assignment logical view mismatch for " + name, raw);
  }

  void sync_data_local(const std::string& name, const mir::Expr& rhs,
                       const Val& v) {
    td.env().erase(name);
    if (!v.si.param_free) return;
    if (const DataMap::Entry* en = observation(v)) {
      td.env()[name] = *en;
      return;
    }
    if (auto evaluated = try_eval_pure(rhs)) {
      DataMap::Entry en = std::move(*evaluated);
      td.env()[name] = en;
      observe(v, std::move(en));
      return;
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
  void validate_data_dims(const std::string& name, const mir::SizedType& t) {
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

  static void data_reads(const mir::Expr& e, std::set<std::string>& names) {
    if (e.kind == mir::Expr::FunApp && e.fn_lib == mir::Expr::Lib::Internal &&
        e.name == "FnReadData" && !e.args.empty() &&
        e.args[0].kind == mir::Expr::LitStr)
      names.insert(e.args[0].lit_s);
    for (const auto& a : e.args) data_reads(a, names);
  }

  static bool direct_input_load(const mir::Stmt& s,
                                const std::set<std::string>& inputs) {
    if (s.kind != mir::Stmt::Assignment || !s.lhs_idx.empty() ||
        !inputs.count(s.lhs))
      return false;
    std::set<std::string> reads;
    data_reads(s.rhs, reads);
    return reads.size() == 1 && *reads.begin() == s.lhs;
  }

  struct RebuildShape {
    bool supported = true;
    int loaders = 0;
    std::string loader_lhs;
    std::set<std::string> reads;
    std::set<std::string> decls;
    std::set<std::string> writes;
  };

  static void scan_rebuild(const mir::Stmt& s, RebuildShape& shape) {
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

  static bool canonical_input_rebuild(const mir::Stmt& s,
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

  void bind_data(const mir::Program& p) {
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
  }

  // Lazily materialize an env value as a data slot when log_prob uses it.
  int env_slot(const std::string& name) {
    DataMap::Entry* en = td.find(name);
    // Empty entries are real: `array[0] real x_r` is how ODE models spell
    // "no data for the system", and it still has to become a (zero-length)
    // slot when passed around.
    if (!en) return -1;
    auto dl = decls.find(name);
    if (en->r.empty() && dl == decls.end()) return -1;
    SlotInfo si;
    si.param_free = true;
    if (dl != decls.end()) si = dl->second.si;
    si.param_free = true;
    const bool nested_matrix =
        is_array(si) && array_shape(si).leaf == ViewKind::Matrix;
    // DataMap is first-index-fast. Graph arrays are outer-major, with only
    // an innermost matrix kept column-major, so normalize at materialization.
    std::vector<double> vals = graph_order(*en, is_matrix(si), nested_matrix);
    validate_view(si, (int64_t)vals.size(), "data value " + name);
    const int s = add_slot((int64_t)vals.size(), false);
    out.fills.emplace_back(s, vals);
    Val v{s, false, si};
    scope[name] = v;
    observe(v, *en);
    return s;
  }

  // ---- expressions ----------------------------------------------------------
  Val lower_expr(const mir::Expr& e) {
    Val value = lower_expr_impl(e);
    if (e.promoted) {
      value.autodiff = expression_autodiff(e);
    } else if (e.kind == mir::Expr::Var) {
      const auto formal = udf_formal_autodiff.find(e.name);
      if (formal != udf_formal_autodiff.end()) value.autodiff = formal->second;
    } else if (e.kind == mir::Expr::TernaryIf && e.args.size() == 3) {
      // A known data condition chooses one implementation value, but C++ has
      // already promoted the expression. MIR records that promoted type, so
      // no arm may be evaluated merely to rediscover it.
      value.autodiff = expression_autodiff(e);
    }
    return value;
  }

  Val lower_expr_impl(const mir::Expr& e) {
    switch (e.kind) {
      case mir::Expr::Var: {
        auto it = scope.find(e.name);
        if (it == scope.end()) {
          auto ii = int_env.find(e.name);
          if (ii != int_env.end())
            return constant(static_cast<double>(ii->second));
          const int s = env_slot(e.name);
          if (s >= 0) return scope.at(e.name);
          fail("unknown variable " + e.name);
        }
        return it->second;
      }
      case mir::Expr::Indexed: {
        // All-Single indices with compile-time values -> element read.
        Val base = lower_expr(e.args[0]);
        if (e.args.size() == 2 && e.args[1].name == "IndexAll") return base;
        bool all_single = true;
        for (size_t k = 1; k < e.args.size(); ++k)
          if (e.args[k].name != "IndexSingle") all_single = false;
        const std::vector<int64_t>* bdims = nullptr;
        if (is_array(base.si)) bdims = &array_shape(base.si).dims;
        const size_t n_idx = e.args.size() - 1;
        // One index on a matrix selects rows. A range or gather is not a
        // contiguous slice in column-major storage, so spell its gather.
        if (e.args.size() == 2 && is_matrix(base.si) &&
            (e.args[1].name == "IndexBetween" ||
             e.args[1].name == "IndexMulti")) {
          std::vector<int> rows;
          if (e.args[1].name == "IndexBetween") {
            const int64_t lo = eval_int(e.args[1].args[0]);
            const int64_t hi = eval_int(e.args[1].args[1]);
            check_range(lo, hi, base.si.rows, "matrix row range", e.raw);
            for (int64_t i = lo; i <= hi; ++i) rows.push_back((int)i - 1);
          } else {
            DataMap::Entry iv = eval_pure(e.args[1].args[0], "a gather index");
            if (!iv.is_int) fail("matrix row gather needs int data", e.raw);
            for (int i : iv.i) {
              if (i < 1 || i > base.si.rows)
                fail("matrix row gather out of bounds", e.raw);
              rows.push_back(i - 1);
            }
          }
          std::vector<int> gather;
          gather.reserve(rows.size() * (size_t)base.si.cols);
          for (int64_t j = 0; j < base.si.cols; ++j)
            for (int i : rows) gather.push_back((int)(j * base.si.rows) + i);
          SlotInfo si = matrix_view((int64_t)rows.size(), base.si.cols,
                                    base.si.param_free);
          return emit_value(OP_GATHER, {base},
                            (int64_t)rows.size() * base.si.cols, si, gather);
        }
        // A range over the outermost array dimension is contiguous because
        // graph storage keeps each whole outer element together. Preserve
        // the complete suffix shape even when its storage width is zero.
        if (e.args.size() == 2 && is_array(base.si) &&
            e.args[1].name == "IndexBetween") {
          const ArrayShape& sh = array_shape(base.si);
          const int64_t lo = eval_int(e.args[1].args[0]);
          const int64_t hi = eval_int(e.args[1].args[1]);
          // hi < lo is an empty slice whatever the endpoints (CmdStan's
          // rvalue checks bounds only when a range is nonempty), and the
          // bounds are data, so rejecting it would make compilation
          // data-dependent.
          if (hi >= lo && (lo < 1 || hi > sh.dims.front()))
            fail("array outer range out of bounds", e.raw);
          std::vector<int64_t> out_dims = sh.dims;
          out_dims[0] = hi >= lo ? hi - lo + 1 : 0;
          const std::vector<int64_t> suffix(sh.dims.begin() + 1, sh.dims.end());
          const int64_t width = checked_product(suffix, "array element");
          const int64_t len = checked_product(out_dims, "array range");
          return emit_value(OP_SLICE, {base}, len,
                            array_view(std::move(out_dims), sh.leaf),
                            {(int)(hi >= lo ? (lo - 1) * width : 0)});
        }
        // Between subrange read on a 1-D value: v[a:b] is contiguous.
        // hi < lo is empty, not negative-length.
        if (e.args.size() == 2 && e.args[1].name == "IndexBetween") {
          const int64_t lo = eval_int(e.args[1].args[0]);
          const int64_t hi = eval_int(e.args[1].args[1]);
          check_range(lo, hi, g.slots[base.slot].len, "range", e.raw);
          const int64_t len = hi >= lo ? hi - lo + 1 : 0;
          return emit_value(OP_SLICE, {base}, len, view_of(e.type_),
                            {(int)(len ? lo - 1 : 0)});
        }
        // A data gather on a one-dimensional scalar array keeps an exact
        // one-dimensional Array view. More structural forms need strides
        // and therefore stay fail-loud rather than masquerading as vectors.
        if (e.args.size() == 2 && is_array(base.si) &&
            e.args[1].name == "IndexMulti") {
          const ArrayShape& sh = array_shape(base.si);
          if (sh.leaf != ViewKind::Flat || sh.dims.size() != 1)
            fail("unsupported index expression", e.raw);
          DataMap::Entry iv = eval_pure(e.args[1].args[0], "a gather index");
          if (!iv.is_int || iv.i.size() != iv.r.size())
            fail("gather index must be int data", e.raw);
          std::vector<int> idata;
          idata.reserve(iv.i.size());
          for (int x : iv.i) {
            if (x < 1 || x > sh.dims[0])
              fail("array gather out of bounds", e.raw);
            idata.push_back(x - 1);
          }
          return emit_value(OP_GATHER, {base}, (int64_t)idata.size(),
                            array_view({(int64_t)idata.size()}, ViewKind::Flat),
                            idata);
        }
        // Gather by a data int array: v[idx].
        if (e.args.size() == 2 && e.args[1].name == "IndexMulti") {
          // An empty index is a legitimate data-dependent gather (a slice
          // whose computed length is zero); an int-flagged entry whose int
          // mirror disagrees with its values is not.
          DataMap::Entry iv = eval_pure(e.args[1].args[0], "a gather index");
          if (!iv.is_int || iv.i.size() != iv.r.size())
            fail("gather index must be int data", e.raw);
          std::vector<int> idata;
          idata.reserve(iv.i.size());
          for (int x : iv.i) {
            check_index(x, g.slots[base.slot].len, "gather index", e.raw);
            idata.push_back(x - 1);
          }
          return emit_value(OP_GATHER, {base}, (int64_t)idata.size(),
                            view_of(e.type_), idata);
        }
        // Matrix row/column slices use the explicit logical view; physical
        // storage remains column-major even when either extent is zero.
        if (e.args.size() == 3 && is_matrix(base.si) &&
            e.args[1].name == "IndexSingle" && e.args[2].name == "IndexAll") {
          const int64_t i = eval_int(e.args[1].args[0]);
          check_index(i, base.si.rows, "matrix row", e.raw);
          return emit_value(OP_SLICE_STRIDED, {base}, base.si.cols,
                            view_of(e.type_),
                            {(int)(i - 1), (int)base.si.rows});
        }
        if (e.args.size() == 3 && is_matrix(base.si) &&
            e.args[1].name == "IndexAll" && e.args[2].name == "IndexSingle") {
          const int64_t j = eval_int(e.args[2].args[0]);
          check_index(j, base.si.cols, "matrix column", e.raw);
          return emit_value(OP_SLICE, {base}, base.si.rows, view_of(e.type_),
                            {(int)((j - 1) * base.si.rows)});
        }
        // Column of a canonical graph-order 2-D array (array[N, S] real):
        // each outer element is contiguous, so successive rows sit S apart.
        if (e.args.size() == 3 && is_array(base.si) && bdims &&
            array_shape(base.si).leaf == ViewKind::Flat && bdims->size() == 2 &&
            e.args[1].name == "IndexAll" && e.args[2].name == "IndexSingle") {
          const int64_t k = eval_int(e.args[2].args[0]) - 1;
          const int64_t N = (*bdims)[0], S = (*bdims)[1];
          if (k < 0 || k >= S) fail("array column out of bounds", e.raw);
          return emit_value(OP_SLICE_STRIDED, {base}, N,
                            array_view({N}, ViewKind::Flat), {(int)k, (int)S});
        }
        // Row-range column read M[a:b, j] (contiguous within the column).
        if (e.args.size() == 3 && is_matrix(base.si) &&
            e.args[1].name == "IndexBetween" &&
            e.args[2].name == "IndexSingle") {
          const int64_t lo = eval_int(e.args[1].args[0]);
          const int64_t hi = eval_int(e.args[1].args[1]);
          const int64_t j = eval_int(e.args[2].args[0]);
          check_index(j, base.si.cols, "matrix column", e.raw);
          check_range(lo, hi, base.si.rows, "matrix row range", e.raw);
          const int64_t len = hi >= lo ? hi - lo + 1 : 0;
          return emit_value(OP_SLICE, {base}, len, view_of(e.type_),
                            {(int)(len ? (j - 1) * base.si.rows + lo - 1 : 0)});
        }
        // Params/locals with recorded dims, laid out by flat_addr above.
        // Matrix views are col-major and never take this array-major path.
        if (all_single && bdims && n_idx <= bdims->size() &&
            !is_matrix(base.si)) {
          const auto& D = *bdims;
          const bool mat = array_shape(base.si).leaf == ViewKind::Matrix;
          std::vector<int64_t> ix;
          for (size_t d = 0; d < n_idx; ++d) {
            const int64_t one = eval_int(e.args[1 + d].args[0]);
            check_index(one, D[d], "array index", e.raw);
            ix.push_back(one - 1);
          }
          const Addr a = flat_addr(D, mat, ix);
          if (a.stride != 1)
            return emit_value(OP_SLICE_STRIDED, {base}, a.len,
                              indexed_view(base.si, n_idx, a.len, e.type_),
                              {(int)a.off, (int)a.stride});
          if (a.len == 1)
            return emit_value(OP_INDEX, {base}, 1,
                              indexed_view(base.si, n_idx, 1, e.type_),
                              {(int)a.off});
          // One whole matrix out of the array keeps its shape, so a later
          // index on it can take the column-major paths above.
          SlotInfo si = indexed_view(base.si, n_idx, a.len, e.type_);
          return emit_value(OP_SLICE, {base}, a.len, si, {(int)a.off});
        }
        // Row of a column-major data matrix / 2-D array: strided slice.
        if (all_single && e.args.size() == 2 && is_matrix(base.si) &&
            e.type_ != "UReal" && e.type_ != "UInt") {
          const int64_t t = eval_int(e.args[1].args[0]);
          check_index(t, base.si.rows, "matrix row", e.raw);
          return emit_value(OP_SLICE_STRIDED, {base}, base.si.cols,
                            view_of(e.type_),
                            {(int)(t - 1), (int)base.si.rows});
        }
        // Data-only slicing with no native path (e.g. one matrix out of a
        // data array of matrices) evaluates at compile time.
        if (auto v = fold_const(e)) return *v;
        int64_t flat = 0;
        if (all_single && e.args.size() == 2 &&
            (e.type_ == "UReal" || e.type_ == "UInt")) {
          const int64_t one = eval_int(e.args[1].args[0]);
          check_index(one, g.slots[base.slot].len, "element", e.raw);
          flat = one - 1;
        } else if (all_single && e.args.size() == 3 && is_matrix(base.si) &&
                   (e.type_ == "UReal" || e.type_ == "UInt")) {
          const int64_t ri = eval_int(e.args[1].args[0]);
          const int64_t cj = eval_int(e.args[2].args[0]);
          check_index(ri, base.si.rows, "matrix row", e.raw);
          check_index(cj, base.si.cols, "matrix column", e.raw);
          flat = (cj - 1) * base.si.rows + (ri - 1);
        } else {
          std::string desc =
              "unsupported index expression: base=" +
              (e.args[0].kind == mir::Expr::Var ? e.args[0].name
                                                : std::string("<expr>"));
          for (size_t k = 1; k < e.args.size(); ++k)
            desc +=
                " [" + (e.args[k].name.empty() ? "?" : e.args[k].name) + "]";
          desc += " type=" + e.type_;
          fail(desc, e.raw);
        }
        return emit_value(OP_INDEX, {base}, 1, view_of(e.type_), {(int)flat});
      }
      case mir::Expr::LitInt: {
        Val v = constant(static_cast<double>(e.lit_i));
        set_int_range(v, e.lit_i, e.lit_i);
        return v;
      }
      case mir::Expr::LitReal:
        return constant(e.lit);
      case mir::Expr::FunApp:
        return lower_funapp(e);
      case mir::Expr::TernaryIf: {
        // Data-only conditions resolve at compile time; either branch may
        // reference parameters.
        // A parameter-dependent condition cannot pick an arm at load
        // time, so the whole expression becomes an island.
        if (!e.args[0].data_only) return lower_param_ternary(e);
        if (expr_effectful(e.args[0]))
          fail("effectful expression cannot be a compile-time condition",
               e.raw);
        const bool c =
            eval_pure(e.args[0], "a compile-time condition").r.at(0) != 0.0;
        return lower_expr(e.args[c ? 1 : 2]);
      }
      case mir::Expr::EOr:
      case mir::Expr::EAnd: {
        if (auto v = fold_const(e)) return *v;
        fail("boolean operator on parameters unsupported", e.raw);
      }
      default: {
        if (auto v = fold_const(e)) return *v;
        fail("unsupported expression", e.raw.empty() ? e.name : e.raw);
      }
    }
  }

  // ---- necessity islands ---------------------------------------------------
  // A region whose control flow depends on a parameter has no op-graph
  // form: `if (theta > 0)` picks its arm at evaluation time, and an op
  // graph is fixed when the model is loaded. Such a region compiles
  // instead into a register program (mir_prog.hpp) that one OP_ISLAND
  // runs -- forward on doubles, backward replayed under stan-math's
  // nested autodiff, which differentiates the arm that actually ran.
  // That is what CmdStan's generated C++ does for the same statement,
  // because it is the same autodiff over the same arithmetic.
  //
  // Unlike the carver's islands (island.cpp) this is not an optimization
  // and STANLI_NO_ISLAND does not disable it: without it the model does
  // not compile at all.
  //
  // Names the region reads that live outside it bind as live-ins; names
  // it assigns become live-outs, extracted into fresh slots that later
  // statements refer to. An assigned name is usually both: the arm that
  // does not run has to leave the old value in place.
  struct IslandRegion {
    std::vector<int> in_slots;
    std::vector<std::string> out_names;
    // The register view of each live-out as the region compiler left it:
    // the authority on shape when the outside declaration was the --O1
    // inliner's zero-length sentinel and the region's assignment sized it.
    std::vector<Range> out_views;
    bool has_target = false;  // the region contributed to the target
  };

  // Does `s` increment the target anywhere?
  static bool has_target_pe(const mir::Stmt& s) {
    if (s.kind == mir::Stmt::TargetPE) return true;
    for (const auto& k : s.body)
      if (has_target_pe(k)) return true;
    return false;
  }

  // Every name an Assignment targets anywhere in `s`, in first-seen order.
  void assigned_names(const mir::Stmt& s, std::vector<std::string>* out) {
    if (s.kind == mir::Stmt::Assignment &&
        std::find(out->begin(), out->end(), s.lhs) == out->end())
      out->push_back(s.lhs);
    for (const auto& k : s.body) assigned_names(k, out);
  }

  // Compile `s` (a statement region) or `e` (a ternary) into a program.
  void lower_island(const mir::Stmt* s, const mir::Expr* e, IslandRegion* reg,
                    Range* expr_out, std::shared_ptr<IslandProg>* prog_out) {
    auto prog = std::make_shared<IslandProg>();
    ProgramCompiler c{*prog, fun_defs};
    // Non-returning statement calls may print or reject. A register program
    // would replay them during reverse mode, so ProgramCompiler refuses them
    // until necessity islands have an execute-once effect path.
    for (const auto& [name, v] : int_env) c.ints[name] = {v};
    c.bind_extern = [&](const std::string& name, Range* r) {
      auto sc = scope.find(name);
      const int slot = sc != scope.end() ? sc->second.slot : env_slot(name);
      if (slot < 0) return false;
      const int64_t len = g.slots[slot].len;
      // An op takes at most six inputs (graph.hpp), and each outside
      // value the region reads is one of them.
      if ((int)reg->in_slots.size() >= 6)
        c.bail(
            "a parameter-dependent region may read at most 6 values "
            "from outside it; " +
            name + " is one too many");
      r->reg = c.alloc((int)len);
      r->len = (int)len;
      const SlotInfo& si = scope.at(name).si;
      r->rows = si.rows;
      r->cols = si.cols;
      r->kind = si.kind;
      if (is_array(si)) {
        const ArrayShape& arr = array_shape(si);
        if (arr.leaf != ViewKind::Flat || arr.dims.size() != 1)
          c.bail("conditional arms of different logical views");
      }
      prog->ins.push_back(IslandProg::LiveIn{r->reg, (int)len});
      reg->in_slots.push_back(slot);
      return true;
    };
    // `target +=` inside the region accumulates into a register of its
    // own, seeded to zero, and the total leaves as one more live-out that
    // lowering registers as a target term. A `~` statement cannot go here
    // (its dropped constants depend on argument types the program binds
    // uniformly), and stanc lowers `~` to TargetPE with the propto form
    // already chosen, so the compiler refuses what it cannot reproduce.
    int target_reg = -1;
    if (s) {
      target_reg = c.alloc(1);
      const double zero = 0.0;
      c.emit_const(target_reg, &zero, 1);
      c.target_reg = target_reg;
    }
    try {
      if (s) {
        // A local declared before the region but never assigned has no
        // slot yet (lowering makes one on first assignment), so there is
        // no outside value to read: the region declares it itself. Stan
        // initializes a local to NaN, and an arm that does not assign it
        // has to leave it that way.
        std::vector<std::string> pre;
        assigned_names(*s, &pre);
        for (const std::string& name : pre) {
          if (scope.count(name) || int_locals.count(name)) continue;
          auto dl = decls.find(name);
          if (dl == decls.end()) continue;
          Range view;
          view.rows = dl->second.si.rows;
          view.cols = dl->second.si.cols;
          view.kind = dl->second.si.kind;
          c.declare(name, (int)dl->second.len, view,
                    std::numeric_limits<double>::quiet_NaN());
        }
        c.stmt(*s);
        std::vector<std::string> assigned;
        assigned_names(*s, &assigned);
        for (const std::string& name : assigned) {
          auto it = c.reals.find(name);
          if (it == c.reals.end()) continue;
          reg->out_names.push_back(name);
          reg->out_views.push_back(it->second);
          for (int k = 0; k < it->second.len; ++k)
            prog->out_regs.push_back(it->second.reg + k);
        }
        if (has_target_pe(*s)) {
          reg->has_target = true;
          prog->out_regs.push_back(target_reg);
        }
      } else {
        *expr_out = c.expr(*e);
        for (int k = 0; k < expr_out->len; ++k)
          prog->out_regs.push_back(expr_out->reg + k);
      }
      c.finish();
    } catch (Bail& b) {
      fail("parameter-dependent region: " + b.why, s ? s->raw : e->raw);
    }
    if (prog->out_regs.empty() && !(e && expr_out->len == 0))
      fail("parameter-dependent region produces nothing", s ? s->raw : e->raw);
    // A region with a runtime branch keeps the var replay -- reversing
    // control flow needs the structured form the flat program has already
    // lost -- so this usually declines. It is asked anyway because a region
    // can reach here branch-free: a `~` refusal or an unknown name is not
    // the only way to end up compiled.
    prog->native_adj =
        gen_adjoint(*prog) && !std::getenv("STANLI_NO_NATIVE_ADJ");
    *prog_out = std::move(prog);
  }

  // The OP_ISLAND for a compiled region, plus one extraction per live-out.
  void emit_island(const std::shared_ptr<IslandProg>& prog,
                   const IslandRegion& reg, const std::vector<int>& out_lens,
                   std::vector<int>* out_slots) {
    int64_t packed = 0;
    for (int len : out_lens) packed += len;
    Op is;
    is.opcode = OP_ISLAND;
    is.n_in = (int)reg.in_slots.size();
    for (int k = 0; k < is.n_in; ++k) is.in[k] = reg.in_slots[k];
    is.out = add_slot(packed, false);
    is.udata = prog.get();
    g.udata_pool.push_back(prog);
    g.ops.push_back(is);
    int64_t off = 0;
    for (size_t k = 0; k < out_lens.size(); ++k) {
      const int len = out_lens[k];
      const Val v = emit_raw(len == 1 ? OP_INDEX : OP_SLICE, {is.out}, len, {},
                             {(int)off});
      out_slots->push_back(v.slot);
      off += len;
    }
  }

  // `if (<depends on a parameter>) ... else ...`
  void lower_param_ifelse(const mir::Stmt& s) {
    IslandRegion reg;
    std::shared_ptr<IslandProg> prog;
    Range ignored;
    lower_island(&s, nullptr, &reg, &ignored, &prog);
    // Widths come from the region compiler's own registers: they are what
    // out_regs packs, and they already reflect a zero-length sentinel
    // declaration the region's assignment sized.
    std::vector<int> out_lens;
    for (const Range& v : reg.out_views) out_lens.push_back(v.len);
    if (reg.has_target) out_lens.push_back(1);
    std::vector<int> out_slots;
    emit_island(prog, reg, out_lens, &out_slots);
    // Later statements read the island's results, not the old values.
    for (size_t k = 0; k < reg.out_names.size(); ++k) {
      const std::string& name = reg.out_names[k];
      SlotInfo si;
      bool shaped_outside = false;
      auto old = scope.find(name);
      if (old != scope.end()) {
        si = old->second.si;
        shaped_outside = g.slots[old->second.slot].len != 0;
      } else {
        auto dl = decls.find(name);
        if (dl != decls.end()) {
          si = dl->second.si;
          shaped_outside = dl->second.len != 0;
        }
      }
      if (!shaped_outside) {
        // The outside declaration was the inliner's zero-length sentinel;
        // the region's registers carry the real shape.
        si = SlotInfo{};
        si.rows = reg.out_views[k].rows;
        si.cols = reg.out_views[k].cols;
        si.kind = reg.out_views[k].kind;
        auto dl = decls.find(name);
        if (dl != decls.end()) {
          dl->second.len = reg.out_views[k].len;
          dl->second.si = si;
        }
      }
      // The island is parameter-dependent regardless of the old binding's
      // provenance; treating its live-out as data would select kernels that
      // deliberately omit adjoints for that input.
      si.param_free = false;
      scope[name] = Val{out_slots[k], scalar_autodiff(), si};
    }
    if (reg.has_target) target_terms.push_back(out_slots.back());
  }

  // `<depends on a parameter> ? a : b`
  Val lower_param_ternary(const mir::Expr& e) {
    IslandRegion reg;
    std::shared_ptr<IslandProg> prog;
    Range value;
    lower_island(nullptr, &e, &reg, &value, &prog);
    std::vector<int> out_slots;
    emit_island(prog, reg, {value.len}, &out_slots);
    SlotInfo si;
    si.rows = value.rows;
    si.cols = value.cols;
    si.kind = value.kind;
    return {out_slots[0], scalar_autodiff(), si};
  }

  Val finish_emit(Op op, int64_t out_len, SlotInfo out_si,
                  std::vector<int> idata, bool autodiff) {
    const int o = add_slot(out_len, false);
    op.out = o;
    if (!idata.empty()) {
      g.idata_pool.push_back(std::move(idata));
      op.idata = g.idata_pool.back().data();
      op.n_idata = (int64_t)g.idata_pool.back().size();
    }
    g.ops.push_back(op);
    return {o, autodiff, out_si};
  }

  // Low-level emission for dynamic slot lists and graph scaffolding whose
  // output dependency is explicit at the call site.
  Val emit_raw(uint16_t opcode, std::vector<int> ins, int64_t out_len,
               SlotInfo out_si, std::vector<int> idata = {}, int out2 = -1,
               bool autodiff = false) {
    Op op;
    op.opcode = opcode;
    op.out2 = out2;
    op.n_in = 0;
    for (int s : ins) op.in[op.n_in++] = s;
    return finish_emit(op, out_len, out_si, std::move(idata), autodiff);
  }

  // The expression seam: a pure result is parameter-free exactly when all of
  // its inputs are. initializer_list avoids a temporary input-list allocation
  // and makes forgetting dependency propagation impossible.
  Val emit_value(uint16_t opcode, std::initializer_list<Val> ins,
                 int64_t out_len, SlotInfo out_si = {},
                 std::vector<int> idata = {}, int out2 = -1) {
    Op op;
    op.opcode = opcode;
    op.out2 = out2;
    op.n_in = 0;
    out_si.param_free = true;
    bool autodiff = false;
    for (const Val& in : ins) {
      op.in[op.n_in++] = in.slot;
      out_si.param_free = out_si.param_free && in.si.param_free;
      autodiff = autodiff || in.autodiff;
    }
    return finish_emit(op, out_len, out_si, std::move(idata), autodiff);
  }

  // Fallback for expressions with no native lowering: a data-only subtree
  // is evaluated at compile time and materialized as a constant. Unsupported
  // expressions and Stan validation failures decline; the latter must stay
  // at model evaluation rather than move to construction. Propto densities
  // never fold because their value is instantiation-dependent.
  bool expr_effectful(const mir::Expr& e) {
    if (e.kind == mir::Expr::FunApp && e.name.size() >= 4 &&
        e.name.compare(e.name.size() - 4, 4, "_rng") == 0)
      return true;
    if (e.kind == mir::Expr::FunApp &&
        e.fn_lib == mir::Expr::Lib::UserDefined && fun_effectful(e.name))
      return true;
    for (const auto& a : e.args)
      if (expr_effectful(a)) return true;
    return false;
  }

  bool stmt_effectful(const mir::Stmt& s) {
    if (s.kind == mir::Stmt::NRFunApp &&
        (s.fn_name == "FnPrint" || s.fn_name == "FnReject"))
      return true;
    for (const auto& e : s.fn_args)
      if (expr_effectful(e)) return true;
    if (s.has_init && expr_effectful(s.init)) return true;
    if (expr_effectful(s.rhs) || expr_effectful(s.target) ||
        expr_effectful(s.lower) || expr_effectful(s.upper) ||
        expr_effectful(s.cond))
      return true;
    for (const auto& e : s.lhs_idx)
      if (expr_effectful(e)) return true;
    for (const auto& k : s.body)
      if (stmt_effectful(k)) return true;
    return false;
  }

  bool fun_effectful(const std::string& name) {
    auto memo = effectful_cache.find(name);
    if (memo != effectful_cache.end()) return memo->second;
    if (!effectful_visiting.insert(name).second) return true;
    bool effect = false;
    auto f = fun_defs.find(name);
    if (f != fun_defs.end())
      for (const auto& s : f->second->body)
        if (stmt_effectful(s)) {
          effect = true;
          break;
        }
    effectful_visiting.erase(name);
    effectful_cache[name] = effect;
    return effect;
  }

  std::optional<DataMap::Entry> try_eval_pure(const mir::Expr& e) {
    if (expr_effectful(e)) return std::nullopt;
    try {
      return td.eval(e);
    } catch (const CompileError&) {
      return std::nullopt;
    } catch (const std::domain_error&) {
      return std::nullopt;
    } catch (const std::invalid_argument&) {
      return std::nullopt;
    }
  }

  DataMap::Entry eval_pure(const mir::Expr& e, const std::string& use) {
    if (expr_effectful(e))
      fail("effectful expression cannot be used for " + use, e.raw);
    return td.eval(e);
  }

  std::optional<Val> fold_const(const mir::Expr& e) {
    if (!e.data_only || e.fn_propto || expr_effectful(e) || e.unsized.depth)
      return std::nullopt;
    auto evaluated = try_eval_pure(e);
    if (!evaluated) return std::nullopt;
    DataMap::Entry en = std::move(*evaluated);
    if (en.r.size() == 1 &&
        (e.type_ == "UReal" || e.type_ == "UInt" || e.type_ == "UComplex"))
      return constant(en.r[0]);
    SlotInfo si;
    si.param_free = true;
    stamp_kind(&si, e.type_);
    if (e.type_ == "UMatrix" && en.dims.size() == 2)
      si = matrix_view(en.dims[0], en.dims[1], true);
    std::vector<double> vals = graph_order(en, e.type_ == "UMatrix", false);
    const int s = add_slot((int64_t)vals.size(), false);
    out.fills.emplace_back(s, vals);
    Val v{s, false, si};
    observe(v, std::move(en));
    return v;
  }

  // Integer argument of a density/pmf: values must be known at compile
  // time (int data, loop variables, or compile-time expressions).
  std::vector<int> int_arg_values(const mir::Expr& oc) {
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

  // Matrix shape of an elementwise result: whichever operand carries one
  // (both must agree when both do).
  SlotInfo shape_of(const Val& a, const Val& b) {
    const bool as = is_scalar(a), bs = is_scalar(b);
    const int64_t la = g.slots[a.slot].len, lb = g.slots[b.slot].len;
    if (!as && !bs && !same_view(a.si, la, b.si, lb))
      fail("elementwise op on different logical views");
    SlotInfo si = as && !bs ? b.si : a.si;
    // A pure op is parameter-free when both inputs are; this lets a
    // transformed data matrix still drive OP_MATVEC.
    si.param_free = a.si.param_free && b.si.param_free;
    return si;
  }

  // Two-argument log_sum_exp / log_diff_exp. Stan vectorizes these over
  // every container shape, so they are elementwise binaries with scalar
  // broadcast, not reductions -- `log_diff_exp(vector[N], real)` is N
  // values, not one. mixture.cpp's kernels already dispatch on length
  // (each argument len 1 or len N, out len N); emitting them at width 1
  // was what truncated the result, which the assignment then rejected.
  Val lower_binary_mix(uint16_t opcode, const mir::Expr& e) {
    Val a = lower_expr(e.args[0]);
    Val b = lower_expr(e.args[1]);
    // shape_of rejects two containers whose views disagree; what is left
    // is one width, or one width and a broadcast scalar.
    SlotInfo si = shape_of(a, b);
    const int64_t n = std::max(g.slots[a.slot].len, g.slots[b.slot].len);
    return emit_value(opcode, {a, b}, n, si);
  }

  // Two-argument scalar math with one int argument
  // (STANLI_SCALAR_BINARY_INT_FIRST_LIST and its SECOND twin): elementwise
  // with scalar broadcast like the all-real binaries, but shape_of does not
  // apply. Those two sides may legitimately carry different views --
  // `ldexp(matrix, array[,] int)` is a matrix, `falling_factorial(real,
  // array[,] int)` is an array -- so the result takes the real side's view
  // when it has one and the int side's when the real side is a scalar,
  // which is what the signature list says in every case.
  Val lower_binary_int(uint16_t opcode, bool int_first, const mir::Expr& e) {
    Val a = lower_expr(e.args[0]);
    Val b = lower_expr(e.args[1]);
    const Val& re = int_first ? b : a;
    const Val& iv = int_first ? a : b;
    const int64_t lr = g.slots[re.slot].len, li = g.slots[iv.slot].len;
    // Only a language scalar broadcasts. A one-element container against a
    // wider one is the size error stan-math throws, not a broadcast.
    if (!is_scalar(re) && !is_scalar(iv) && lr != li)
      fail(e.name + ": arguments must match in size", e.raw);
    const SlotInfo si = is_scalar(re) ? iv.si : re.si;
    // The one place the two flat orders disagree: a matrix leaf is stored
    // column-major and an int array's trailing two extents are row-major.
    // Handing the kernel that leaf's rows and cols is what tells it to undo
    // the difference; see IntLane in kernels/scalar_binary.cpp.
    std::vector<int> idata;
    if (!is_scalar(iv)) {
      if (is_matrix(re.si)) {
        idata = {(int)re.si.rows, (int)re.si.cols};
      } else if (is_array(re.si)) {
        const ArrayShape& s = array_shape(re.si);
        if (s.leaf == ViewKind::Matrix)
          idata = {(int)s.dims[s.dims.size() - 2], (int)s.dims.back()};
      }
    }
    return emit_value(opcode, {a, b}, std::max(lr, li), si, std::move(idata));
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
  std::optional<Val> lower_bound_transform(const mir::Expr& e) {
    struct Transform {
      const char* stem;
      uint16_t opcode;
      size_t arity;
    };
    static const Transform kTransforms[] = {
        {"lower_bound_", OP_CONSTRAIN_LOWER, 2},
        {"upper_bound_", OP_CONSTRAIN_UPPER, 2},
        {"lower_upper_bound_", OP_CONSTRAIN_LU, 3},
        {"offset_multiplier_", OP_CONSTRAIN_OFFSET_MULT, 3},
    };
    const Transform* tr = nullptr;
    std::string direction;
    for (const Transform& t : kTransforms) {
      const std::string prefix(t.stem);
      if (e.name.compare(0, prefix.size(), prefix) != 0) continue;
      const std::string tail = e.name.substr(prefix.size());
      if (tail != "constrain" && tail != "jacobian" && tail != "unconstrain")
        continue;
      tr = &t;
      direction = tail;
    }
    if (tr == nullptr || e.args.size() != tr->arity) return std::nullopt;

    std::vector<Val> a;
    a.reserve(e.args.size());
    for (const mir::Expr& arg : e.args) a.push_back(lower_expr(arg));
    const int64_t n = g.slots[a[0].slot].len;
    SlotInfo si = a[0].si;
    std::vector<int> ins;
    bool autodiff = false;
    for (const Val& v : a) {
      const int64_t len = g.slots[v.slot].len;
      if (len != 1 && len != n)
        fail(e.name + ": bound is neither one value nor one per element",
             e.raw);
      si.param_free = si.param_free && v.si.param_free;
      autodiff = autodiff || v.autodiff;
      ins.push_back(v.slot);
    }

    if (direction == "unconstrain") return free_transform(tr->opcode, a, si, n);
    // The declaration kernels, unchanged: they carry the arithmetic that was
    // measured against stan-math's rev overloads, which composing exp,
    // inv_logit, and fma out of the elementwise ops would not reproduce.
    // They always write the jacobian, so `_constrain` allocates the output
    // and simply leaves it unrooted -- no term reaches the target, and its
    // adjoint stays zero, which is exactly the no-lp overload's gradient.
    const int jac = add_slot(1, /*is_param=*/false);
    Val v = emit_raw(tr->opcode, ins, n, si, {}, jac, autodiff);
    if (direction == "jacobian" && !in_write_array) target_terms.push_back(jac);
    return v;
  }

  // The inverse transforms. stan-math has no rev overloads for these: its
  // `log(y - lb)` is ordinary var arithmetic, which is what these
  // elementwise ops emit, so the composition is the reference rather than an
  // approximation of it, and no new kernel is needed.
  Val free_transform(uint16_t opcode, const std::vector<Val>& a, SlotInfo si,
                     int64_t n) {
    // An intermediate keeps the argument's logical view only when it is as
    // wide as the argument; `ub - lb` on two scalars is one value.
    const auto elt = [&](uint16_t op, const Val& x, const Val& y) {
      const int64_t w = std::max(g.slots[x.slot].len, g.slots[y.slot].len);
      return emit_value(op, {x, y}, w, w == n ? si : SlotInfo{});
    };
    const auto un = [&](uint16_t op, const Val& x) {
      return emit_value(op, {x}, g.slots[x.slot].len, x.si);
    };
    switch (opcode) {
      case OP_CONSTRAIN_LOWER:  // lb_free: log(y - lb)
        return un(OP_LOGV, elt(OP_SUB, a[0], a[1]));
      case OP_CONSTRAIN_UPPER:  // ub_free: log(ub - y)
        return un(OP_LOGV, elt(OP_SUB, a[1], a[0]));
      case OP_CONSTRAIN_LU:  // lub_free: logit((y - lb) / (ub - lb))
        return un(OP_LOGIT, elt(OP_DIV, elt(OP_SUB, a[0], a[1]),
                                elt(OP_SUB, a[2], a[1])));
      default:  // offset_multiplier_free: (y - mu) / sigma
        return elt(OP_DIV, elt(OP_SUB, a[0], a[1]), a[2]);
    }
  }

  // Value of a data-only expression at compile time. The interpreter
  // handles most cases; a UDF-local constant lives only as a slot, so fall
  // back to that slot's recorded fill.
  std::vector<double> const_values(const mir::Expr& e) {
    if (expr_effectful(e))
      fail("effectful expression cannot be demanded at compile time", e.raw);
    if (auto evaluated = try_eval_pure(e)) {
      DataMap::Entry en = std::move(*evaluated);
      return en.r;
    }
    Val v = lower_expr(e);
    if (const DataMap::Entry* en = observation(v)) return en->r;
    // A zero-length slot carries no values by construction (`array[0] real`
    // is how ODE models spell "no data for the system").
    if (g.slots[v.slot].len == 0) return {};
    fail("value must be known at compile time: " +
             (e.kind == mir::Expr::Var ? e.name : ("<" + e.name + ">")),
         e.raw);
  }
  std::vector<int> const_ints(const mir::Expr& e) {
    if (expr_effectful(e))
      fail("effectful expression cannot be demanded as compile-time integers",
           e.raw);
    if (auto evaluated = try_eval_pure(e)) {
      DataMap::Entry en = std::move(*evaluated);
      if (en.is_int) return en.i;
      std::vector<int> out;
      for (double d : en.r) out.push_back((int)d);
      return out;
    }
    std::vector<int> out;
    for (double d : const_values(e)) out.push_back((int)d);
    return out;
  }

  // Thrown by a Return statement inside an inlined UDF body.
  struct LpReturn {
    Val v;
  };

  // Inline a user-defined function at its call site: arguments are lowered
  // in the caller's scope, bound under the parameter names in a shadowed
  // scope, and the body lowers like any other statements (loops unroll,
  // data-only conditions resolve). Return throws the result value out.
  Val lower_call_udf(const mir::Expr& e) {
    auto it = fun_defs.find(e.name);
    if (it == fun_defs.end()) fail("unknown function " + e.name, e.raw);
    const mir::FunDef& f = *it->second;
    if (e.args.size() != f.arg_names.size()) fail(e.name + ": arity mismatch");
    struct Binding {
      bool is_int = false;
      long iv = 0;
      Val v{-1, false, {}};
      std::optional<DataMap::Entry> data;
    };
    std::vector<Binding> binds(e.args.size());
    for (size_t i = 0; i < e.args.size(); ++i) {
      const mir::Expr& a = e.args[i];
      if (a.data_only && a.type_ == "UInt") {
        binds[i].is_int = true;
        binds[i].iv = eval_int(a);
      } else {
        binds[i].v = lower_expr(a);
      }
      if (!binds[i].is_int && binds[i].v.slot >= 0) {
        if (const DataMap::Entry* en = observation(binds[i].v))
          binds[i].data = *en;
      }
      if (!binds[i].data) {
        if (auto evaluated = try_eval_pure(a)) {
          binds[i].data = std::move(*evaluated);
          if (!binds[i].is_int && binds[i].v.slot >= 0)
            observe(binds[i].v, *binds[i].data);
        }
      }
      const bool formal_data = i < f.arg_data_only.size() && f.arg_data_only[i];
      if (!in_write_array && formal_data && !binds[i].is_int &&
          (binds[i].v.autodiff || !binds[i].v.si.param_free))
        fail(e.name + ": data-only argument depends on a parameter", e.raw);
    }
    if (++udf_depth > 64) {
      --udf_depth;
      fail("UDF recursion too deep in " + e.name);
    }
    auto sc_saved = scope;
    auto formal_autodiff_saved = udf_formal_autodiff;
    auto ie_saved = int_env;
    auto decls_saved = decls;
    auto il_saved = int_locals;
    auto env_saved = td.env();
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
    propto_ctx = propto_ctx && e.fn_propto;
    udf_autodiff_ctx = false;
    for (size_t i = 0; i < binds.size(); ++i)
      if (!binds[i].is_int && f.arg_views[i].leaf != mir::UnsizedLeaf::Int)
        udf_autodiff_ctx = udf_autodiff_ctx || binds[i].v.autodiff;
    auto restore = [&] {
      propto_ctx = propto_saved;
      udf_autodiff_ctx = autodiff_saved;
      scope = std::move(sc_saved);
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
    return ret;
  }

  std::optional<Val> lower_multi_normal_rng(const mir::Expr& e) {
    if (e.name != "multi_normal_rng") return std::nullopt;
    if (!in_write_array)
      fail("multi_normal_rng is supported only in generated quantities", e.raw);
    if (e.args.size() != 2 || e.type_ != "UVector" ||
        e.unsized.leaf != mir::UnsizedLeaf::Vector || e.unsized.depth != 0)
      fail("multi_normal_rng: expected one vector result", e.raw);
    const mir::Expr& location_expr = e.args[0];
    const mir::Expr& covariance_expr = e.args[1];
    if (location_expr.type_ != "UVector" ||
        location_expr.unsized.leaf != mir::UnsizedLeaf::Vector ||
        location_expr.unsized.depth != 0)
      fail("multi_normal_rng: expected one vector location", e.raw);
    if (covariance_expr.type_ != "UMatrix" ||
        covariance_expr.unsized.leaf != mir::UnsizedLeaf::Matrix ||
        covariance_expr.unsized.depth != 0)
      fail("multi_normal_rng: expected one covariance matrix", e.raw);

    Val location = lower_expr(location_expr);
    Val covariance = lower_expr(covariance_expr);
    if (!is_vector(location.si))
      fail("multi_normal_rng: location is not a logical vector", e.raw);
    if (!is_matrix(covariance.si))
      fail("multi_normal_rng: covariance has no known matrix shape", e.raw);
    const int64_t k = g.slots[location.slot].len;
    if (k > std::numeric_limits<int>::max() || covariance.si.rows != k ||
        covariance.si.cols != k ||
        g.slots[covariance.slot].len != checked_product({k, k}, "covariance"))
      fail("multi_normal_rng: covariance shape must match the location", e.raw);

    Val draw = emit_value(OP_RNG, {location, covariance}, k, view_of(e.type_),
                          {static_cast<int>(k)});
    g.ops.back().variant = kMultiNormalRngVariant;
    draw.si.param_free = false;
    draw.autodiff = false;
    return draw;
  }

  std::optional<Val> lower_categorical_rng(const mir::Expr& e) {
    if (e.name != "categorical_rng") return std::nullopt;
    if (!in_write_array)
      fail("categorical_rng is supported only in generated quantities", e.raw);
    if (e.args.size() != 1 || e.type_ != "UInt" ||
        e.unsized.leaf != mir::UnsizedLeaf::Int || e.unsized.depth != 0)
      fail("categorical_rng: expected one scalar int result", e.raw);
    const mir::Expr& probabilities = e.args[0];
    if (probabilities.type_ != "UVector" || probabilities.unsized.depth != 0 ||
        probabilities.unsized.leaf != mir::UnsizedLeaf::Vector)
      fail("categorical_rng: expected one probability-vector argument", e.raw);

    Val argument = lower_expr(probabilities);
    if (!is_vector(argument.si))
      fail("categorical_rng: argument is not a logical vector", e.raw);
    Val draw = emit_value(OP_RNG, {argument}, 1, view_of(e.type_));
    g.ops.back().variant = kCategoricalRngVariant;
    // A successful call returns a Stan int, but deliberately do not widen
    // this tranche into runtime-sum range reasoning. Survey only needs the
    // scalar value; dynamic integer control and indexing still fail closed.
    draw.si.param_free = false;
    draw.autodiff = false;
    set_int_initialized(draw);
    return draw;
  }

  std::optional<Val> lower_scalar_rng(const mir::Expr& e) {
    static const std::map<std::string, ScalarRng> kFamilies = {
        {"poisson_log_rng", ScalarRng::PoissonLog},
        {"uniform_rng", ScalarRng::Uniform},
        {"bernoulli_rng", ScalarRng::Bernoulli},
        {"normal_rng", ScalarRng::Normal},
        {"lognormal_rng", ScalarRng::Lognormal},
        {"binomial_rng", ScalarRng::Binomial},
    };
    const auto found = kFamilies.find(e.name);
    if (found == kFamilies.end()) return std::nullopt;
    if (!in_write_array)
      fail(e.name + " is supported only in generated quantities", e.raw);
    const ScalarRng family = found->second;
    const size_t arity = scalar_rng_arity(family);
    if (e.args.size() != arity || e.unsized.depth != 0)
      fail(e.name + ": expected scalar result and " + std::to_string(arity) +
               " scalar argument(s)",
           e.raw);
    const mir::UnsizedLeaf result_leaf = scalar_rng_is_int(family)
                                             ? mir::UnsizedLeaf::Int
                                             : mir::UnsizedLeaf::Real;
    if (e.unsized.leaf != result_leaf)
      fail(e.name + ": result type does not match RNG family", e.raw);
    // Unlike the other scalar families, binomial's first argument is a
    // population count. Valid stanc MIR always marks it UInt; fail closed on
    // malformed hand-authored MIR rather than silently truncating a real in
    // the runtime helper's graph-storage conversion.
    if (family == ScalarRng::Binomial &&
        e.args[0].unsized.leaf != mir::UnsizedLeaf::Int)
      fail("binomial_rng: first argument must be int", e.raw);
    std::vector<Val> args;
    args.reserve(arity);
    for (const mir::Expr& arg : e.args) {
      if (arg.unsized.depth != 0)
        fail(e.name + ": container arguments stay on WaInterp", e.raw);
      args.push_back(lower_expr(arg));
      if (!is_scalar(args.back()))
        fail(e.name + ": container arguments stay on WaInterp", e.raw);
    }
    Val draw = arity == 1 ? emit_value(OP_RNG, {args[0]}, 1, view_of(e.type_))
                          : emit_value(OP_RNG, {args[0], args[1]}, 1,
                                       view_of(e.type_));
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

  static bool is_int_sum_surface(const mir::Expr& e) {
    return e.kind == mir::Expr::FunApp && e.fn_lib == mir::Expr::Lib::StanLib &&
           e.name == "sum" && e.args.size() == 1 && e.type_ == "UInt" &&
           e.unsized.leaf == mir::UnsizedLeaf::Int && e.unsized.depth == 0 &&
           e.args[0].unsized.leaf == mir::UnsizedLeaf::Int &&
           e.args[0].unsized.depth == 1;
  }

  // Scalar int declarations normally stay in int_env.  A sum over an array
  // assembled from runtime RNG draws must instead bind to a graph slot.  The
  // named-value probe is intentionally non-lowering so ordinary compile-time
  // sums retain the interpreter path they already had.  Range validity is
  // checked by the guarded lowering, so an unknown runtime source fails
  // closed instead of falling back through the legacy real-valued reduction.
  bool runtime_int_sum_candidate(const mir::Expr& e) const {
    if (!is_int_sum_surface(e) || e.args[0].kind != mir::Expr::Var)
      return false;
    const auto value = scope.find(e.args[0].name);
    return value != scope.end() && !value->second.si.param_free;
  }

  bool runtime_int_binding(const mir::Expr& e) {
    return expr_effectful(e) || runtime_int_sum_candidate(e);
  }

  static bool prod_transpose_of(const mir::Expr& e, mir::Expr::Kind kind) {
    return e.kind == mir::Expr::FunApp && e.fn_lib == mir::Expr::Lib::StanLib &&
           e.args.size() == 1 &&
           (e.name == "Transpose__" || e.name == "transpose") &&
           e.args[0].kind == kind;
  }

  bool prod_literal_length(const mir::Expr& e) const {
    if (e.kind == mir::Expr::LitInt) return true;
    return e.kind == mir::Expr::Var && int_env.count(e.name) != 0;
  }

  bool prod_minus_operand(const mir::Expr& e) const {
    // Promotion is transparent in the reader, so a promoted scalar literal
    // still has its LitInt/LitReal kind here.
    if (e.unsized.depth == 0 &&
        (e.kind == mir::Expr::LitInt || e.kind == mir::Expr::LitReal))
      return true;
    if (e.kind == mir::Expr::FunApp && e.fn_lib == mir::Expr::Lib::StanLib &&
        e.name == "rep_vector" && e.args.size() == 2 &&
        (e.args[0].kind == mir::Expr::LitInt ||
         e.args[0].kind == mir::Expr::LitReal) &&
        prod_literal_length(e.args[1]))
      return true;
    const bool vector_leaf =
        e.unsized.depth == 0 && (e.unsized.leaf == mir::UnsizedLeaf::Vector ||
                                 e.unsized.leaf == mir::UnsizedLeaf::RowVector);
    if (!vector_leaf) return false;
    if (e.kind == mir::Expr::Var) return true;
    if (e.kind == mir::Expr::Indexed) return mir::is_matrix_row_value(e);
    if (prod_transpose_of(e, mir::Expr::Var)) return true;
    return mir::is_matrix_row_value(e);
  }

  bool prod_native_surface(const mir::Expr& e) const {
    if (e.kind == mir::Expr::Var || prod_transpose_of(e, mir::Expr::Var))
      return true;
    if (e.kind != mir::Expr::FunApp || e.fn_lib != mir::Expr::Lib::StanLib ||
        e.args.size() != 2 || e.name != "Minus__")
      return false;
    return prod_minus_operand(e.args[0]) && prod_minus_operand(e.args[1]);
  }

  Val lower_runtime_int_sum(const mir::Expr& e) {
    if (!in_write_array)
      fail("runtime integer sum is supported only in generated quantities",
           e.raw);
    if (!is_int_sum_surface(e))
      fail(
          "runtime integer sum needs one one-dimensional int-array argument "
          "and a scalar int result",
          e.raw);

    Val a = lower_expr(e.args[0]);
    if (!is_array(a.si))
      fail("runtime integer sum argument is not an array", e.raw);
    const ArrayShape& shape = array_shape(a.si);
    const int64_t len = g.slots[a.slot].len;
    if (shape.leaf != ViewKind::Flat || shape.dims.size() != 1)
      fail("runtime integer sum needs a one-dimensional int array", e.raw);
    if (len <= 0) fail("runtime integer sum needs a nonempty int array", e.raw);
    if (a.si.param_free)
      fail("runtime integer sum needs a runtime-produced int array", e.raw);

    const auto initialized = int_initialized_prefix.find(a.slot);
    if (initialized == int_initialized_prefix.end() ||
        initialized->second != len)
      fail("runtime integer sum array is not definitely initialized", e.raw);
    const auto known = int_ranges.find(a.slot);
    if (known == int_ranges.end())
      fail("runtime integer sum has unproved integral slot values", e.raw);
    const IntRange range = known->second;
    const uint64_t n = static_cast<uint64_t>(len);
    if (range.lo < 0) {
      const uint64_t magnitude =
          static_cast<uint64_t>(-static_cast<int64_t>(range.lo));
      const uint64_t capacity = static_cast<uint64_t>(
          -static_cast<int64_t>(std::numeric_limits<int32_t>::min()));
      if (n > capacity / magnitude)
        fail("runtime integer sum may overflow int32 in a partial sum", e.raw);
    }
    if (range.hi > 0 &&
        n > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) /
                static_cast<uint64_t>(range.hi))
      fail("runtime integer sum may overflow int32 in a partial sum", e.raw);

    Val result = emit_value(OP_SUM_VEC, {a}, 1, view_of("UInt"));
    result.autodiff = false;
    // A range is only a static proof; the source itself was required to be
    // runtime-produced.  Keeping this result non-constant prevents later
    // compile-time geometry/control from consuming it through Val metadata.
    result.si.param_free = false;
    set_int_range(result, static_cast<int64_t>(range.lo) * len,
                  static_cast<int64_t>(range.hi) * len);
    return result;
  }

  Val lower_funapp(const mir::Expr& e) {
    if (e.fn_lib == mir::Expr::Lib::StanLib && e.name == "dims")
      return lower_dims(e);
    if (e.fn_lib == mir::Expr::Lib::UserDefined) {
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
        acc = Val{add_slot(0, false), false, empty};
        out.fills.emplace_back(acc.slot, std::vector<double>{});
      } else {
        acc = parts[0];
        for (size_t i = 1; i < parts.size(); ++i) {
          const int64_t len =
              g.slots[acc.slot].len + g.slots[parts[i].slot].len;
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
          acc = emit_value(OP_GATHER, {acc}, rows * cols,
                           matrix_view(rows, cols), gather);
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
              fail("array literal elements have different logical views",
                   e.raw);
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
      return acc;
    }
    if (e.fn_lib != mir::Expr::Lib::StanLib) {
      if (auto v = fold_const(e)) return *v;
      fail("unsupported function kind for " + e.name, e.raw);
    }
    // The stan-library names split into disjoint groups; each helper owns
    // one and declines the rest.
    if (auto v = lower_multi_normal_rng(e)) return *v;
    if (auto v = lower_categorical_rng(e)) return *v;
    if (auto v = lower_scalar_rng(e)) return *v;
    if (auto v = lower_density_fn(e)) return *v;
    if (auto v = lower_bound_transform(e)) return *v;
    if (auto v = lower_eltwise_fn(e)) return *v;
    if (auto v = lower_matrix_fn(e)) return *v;
    if (auto v = lower_ode_fn(e)) return *v;
    // A shape query in a REAL-valued expression. eval_int already answers
    // rows/cols/size from the slot or the data map, but only where an
    // integer was expected; brms's mo() helper writes
    // `rows(scale) * sum(scale[1:i])`, where the same call sits in the
    // middle of arithmetic and reached the failure below instead.
    if ((e.name == "rows" || e.name == "cols" || e.name == "size" ||
         e.name == "num_elements") &&
        e.args.size() == 1) {
      try {
        return constant((double)eval_int(e));
      } catch (const CompileError&) {
      }
    }
    if (auto v = fold_const(e)) return *v;
    fail("unsupported function " + e.name);
  }

  // Density calls: the table-driven kernels plus exact categorical and
  // matrix-argument implementations (multi_normal, lkj, glm).
  Val emit_categorical(const mir::Expr& e, const Val& outcome, const Val& arg,
                       bool logit) {
    const bool scalar_outcome = e.args[0].unsized.depth == 0;
    if (e.args[0].unsized.leaf != mir::UnsizedLeaf::Int ||
        e.args[0].unsized.depth > 1 ||
        e.args[1].unsized.leaf != mir::UnsizedLeaf::Vector ||
        e.args[1].unsized.depth != 0)
      fail(e.name + ": expected int or array[] int and vector", e.raw);
    const bool array_outcome = is_array(outcome.si) &&
                               array_shape(outcome.si).leaf == ViewKind::Flat &&
                               array_shape(outcome.si).dims.size() == 1;
    if ((scalar_outcome && !is_scalar(outcome)) ||
        (!scalar_outcome && !array_outcome) || !is_vector(arg.si))
      fail(e.name + ": MIR type does not match lowered values", e.raw);
    if (!e.args[0].data_only || !outcome.si.param_free ||
        (udf_depth == 0 && e.args[1].data_only == arg.autodiff))
      fail(e.name + ": MIR adlevel contradicts lowered dependencies", e.raw);
    auto spec = std::make_shared<CategoricalSpec>();
    spec->logit = logit;
    spec->scalar_outcome = scalar_outcome;
    // The graph dependency and instantiated C++ scalar type are independent:
    // write_array varies with q but uses double, while an autodiff local can
    // be graph-constant and still make Stan retain a propto summand.
    spec->arg_autodiff = arg.autodiff;
    spec->propto = propto(e);
    Val checked = emit_value(OP_CATEGORICAL, {outcome, arg}, 1, {});
    g.ops.back().udata = spec.get();
    g.udata_pool.push_back(std::move(spec));
    return checked;
  }

  std::optional<Val> lower_density_fn(const mir::Expr& e) {
    // Leading integer arguments become idata; the rest become real slots.
    // Layouts: one integer group = raw values; two groups =
    // [len, vals..., len, vals...]; glm = [y..., rows, cols].
    enum class DensityShape {
      Plain,
      FirstMatrixRows,
      FirstMatrixDimensions,
      LastMatrixRowsAndRepetitions,
    };
    struct DensitySpec {
      uint16_t opcode;
      int arity;
      int integer_args;
      bool glm_layout = false;
      DensityShape shape = DensityShape::Plain;
      int activity_mask = -1;  // negative: derive from MIR arguments
      // The single integer group is one outcome per lane of the vectorized
      // reduction, so a language-level scalar broadcasts across the real
      // arguments (see the expansion below). False for the densities whose
      // integer group means something else: multinomial's outcome is the
      // whole count vector, and the ordinal pair reads a cutpoint vector
      // that is one argument rather than lanes.
      bool lane_outcome = false;
    };
    static const std::map<std::string, DensitySpec> kDensities = {
        {"poisson_log_lpmf",
         {OP_POISSON_LOG_LPMF, 2, 1, false, DensityShape::Plain, -1, true}},
        {"bernoulli_logit_lpmf",
         {OP_BERNOULLI_LOGIT_LPMF, 2, 1, false, DensityShape::Plain, -1, true}},

    // clang-format off
        // These macros use the same lists that define opcodes and kernels.
#define STANLI_DENSITY_TABLE(code, fn, n, m) {#fn, {code, n, 0}},
        STANLI_SCALAR_DENSITY_LIST(STANLI_DENSITY_TABLE)
#undef STANLI_DENSITY_TABLE

        // Discrete densities: outcome + n real arguments, one int group,
        // one outcome per lane.
#define STANLI_INT_DENSITY_TABLE(code, fn, nreal, t) \
  {#fn, {code, nreal + 1, 1, false, DensityShape::Plain, -1, true}},
        STANLI_INT_DENSITY_LIST(STANLI_INT_DENSITY_TABLE)
#undef STANLI_INT_DENSITY_TABLE

        // Continuous cdf/lcdf/lccdf functions have no integer group.
#define STANLI_CDF_TABLE(code, fn, n, t) {#fn, {code, n, 0}},
        STANLI_SCALAR_CDF_LIST(STANLI_CDF_TABLE)
#undef STANLI_CDF_TABLE

        // Integer-outcome cdfs keep the count in the one integer group, and
        // it is per-lane there too: a vectorized cdf is the product over
        // lanes, an lcdf/lccdf the sum.
#define STANLI_INT_CDF_TABLE(code, fn, nreal, t) \
  {#fn, {code, nreal + 1, 1, false, DensityShape::Plain, -1, true}},
        STANLI_INT_CDF_LIST(STANLI_INT_CDF_TABLE)
#undef STANLI_INT_CDF_TABLE
        // The binomials' cdfs: an outcome group and a trials group, so
        // the two-group branch below writes both as [len, vals...] and
        // spells a language-level scalar -1. lane_outcome stays false --
        // that flag replicates the ONE group these do not have, and the
        // -1 length is how these broadcast instead.
#define STANLI_TWO_INT_CDF_TABLE(code, fn, nreal, t) {#fn, {code, nreal + 2, 2}},
        STANLI_TWO_INT_CDF_LIST(STANLI_TWO_INT_CDF_TABLE)
#undef STANLI_TWO_INT_CDF_TABLE
        // The var-tape cdfs. Same argument shapes as the two lists above,
        // and a fixed all-active mask because their kernel binds every
        // argument as var whatever the MIR says: one instantiation, no
        // activity-mask expansion, and a data argument's partials
        // computed and dropped.
#define STANLI_TAIL_CDF_TABLE(code, fn, n, t) \
  {#fn, {code, n, 0, false, DensityShape::Plain, (1 << n) - 1}},
        STANLI_TAIL_CDF_LIST(STANLI_TAIL_CDF_TABLE)
#undef STANLI_TAIL_CDF_TABLE
#define STANLI_TAIL_INT_CDF_TABLE(code, fn, nreal, t)                        \
  {#fn,                                                                      \
   {code, nreal + 1, 1, false, DensityShape::Plain, (1 << nreal) - 1, true}},
        STANLI_TAIL_INT_CDF_LIST(STANLI_TAIL_INT_CDF_TABLE)
#undef STANLI_TAIL_INT_CDF_TABLE
        // The ordinal densities have the same argument counts but not the
        // same meaning: their trailing cutpoint vector is one argument, so
        // a scalar outcome stays one lane whatever its length.
#define STANLI_ORDERED_TABLE(code, fn, nreal, t) {#fn, {code, nreal + 1, 1}},
        STANLI_ORDERED_DENSITY_LIST(STANLI_ORDERED_TABLE)
#undef STANLI_ORDERED_TABLE
        // clang-format on

        {"bernoulli_lpmf",
         {OP_BERNOULLI_LPMF, 2, 1, false, DensityShape::Plain, -1, true}},
        {"poisson_lpmf",
         {OP_POISSON_LPMF, 2, 1, false, DensityShape::Plain, -1, true}},
        {"neg_binomial_2_lpmf",
         {OP_NEG_BINOMIAL_2_LPMF, 3, 1, false, DensityShape::Plain, -1, true}},
        {"binomial_lpmf", {OP_BINOMIAL_LPMF, 3, 2}},
        {"binomial_logit_lpmf", {OP_BINOMIAL_LOGIT_LPMF, 3, 2}},
        {"poisson_log_glm_lpmf", {OP_POISSON_LOG_GLM_LPMF, 4, 1, true}},
        {"neg_binomial_2_log_glm_lpmf",
         {OP_NEG_BINOMIAL_2_LOG_GLM_LPMF, 5, 1, true}},
        {"beta_binomial_lpmf", {OP_BETA_BINOMIAL_LPMF, 4, 2}},
        {"bernoulli_logit_glm_lpmf", {OP_BERNOULLI_LOGIT_GLM_LPMF, 4, 1, true}},
        {"dirichlet_lpdf", {OP_DIRICHLET_LPDF, 2, 0}},
        {"multi_normal_cholesky_lpdf",
         {OP_MULTI_NORMAL_CHOL_LPDF, 3, 0, false,
          DensityShape::LastMatrixRowsAndRepetitions}},
        {"multi_normal_lpdf",
         {OP_MULTI_NORMAL_LPDF, 3, 0, false,
          DensityShape::LastMatrixRowsAndRepetitions}},
        {"multi_normal_prec_lpdf",
         {OP_MULTI_NORMAL_PREC_LPDF, 3, 0, false,
          DensityShape::LastMatrixRowsAndRepetitions}},
        {"lkj_corr_cholesky_lpdf",
         {OP_LKJ_CORR_CHOL_LPDF, 2, 0, false, DensityShape::FirstMatrixRows,
          0x1}},
        {"lkj_corr_lpdf",
         {OP_LKJ_CORR_LPDF, 2, 0, false, DensityShape::FirstMatrixRows, 0x1}},
        {"lkj_cov_lpdf",
         {OP_LKJ_COV_LPDF, 4, 0, false, DensityShape::FirstMatrixRows, 0xf}},
        {"multi_gp_lpdf",
         {OP_MULTI_GP_LPDF, 3, 0, false, DensityShape::FirstMatrixDimensions,
          0x7}},
        {"multi_gp_cholesky_lpdf",
         {OP_MULTI_GP_CHOL_LPDF, 3, 0, false,
          DensityShape::FirstMatrixDimensions, 0x7}},
        {"multi_student_t_lpdf",
         {OP_MULTI_STUDENT_T_LPDF, 4, 0, false,
          DensityShape::LastMatrixRowsAndRepetitions, 0xf}},
        {"multi_student_t_cholesky_lpdf",
         {OP_MULTI_STUDENT_T_CHOL_LPDF, 4, 0, false,
          DensityShape::LastMatrixRowsAndRepetitions, 0xf}},
        {"multinomial_lpmf",
         {OP_MULTINOMIAL_LPMF, 2, 1, false, DensityShape::Plain, 0x1}},
        {"multinomial_logit_lpmf",
         {OP_MULTINOMIAL_LOGIT_LPMF, 2, 1, false, DensityShape::Plain, 0x1}},
        {"dirichlet_multinomial_lpmf",
         {OP_DIRICHLET_MULTINOMIAL_LPMF, 2, 1, false, DensityShape::Plain,
          0x1}},
        {"ordered_probit_lpmf",
         {OP_ORDERED_PROBIT_LPMF, 3, 1, false, DensityShape::Plain, 0x3}},
        {"wiener_lpdf",
         {OP_WIENER_LPDF, 5, 0, false, DensityShape::Plain, 0x1f}},
        {"wishart_lpdf",
         {OP_WISHART_LPDF, 3, 0, false, DensityShape::FirstMatrixRows, 0x7}},
        {"inv_wishart_lpdf",
         {OP_INV_WISHART_LPDF, 3, 0, false, DensityShape::FirstMatrixRows,
          0x7}},
        {"wishart_cholesky_lpdf",
         {OP_WISHART_CHOL_LPDF, 3, 0, false, DensityShape::FirstMatrixRows,
          0x7}},
        {"inv_wishart_cholesky_lpdf",
         {OP_INV_WISHART_CHOL_LPDF, 3, 0, false, DensityShape::FirstMatrixRows,
          0x7}},
    };
    auto density = kDensities.find(e.name);
    if (density != kDensities.end()) {
      const DensitySpec& spec = density->second;
      if ((int)e.args.size() != spec.arity) {
        // The compact cases below used to decline a bad arity and let the
        // common unsupported-function diagnostic report it.
        if (spec.shape != DensityShape::Plain || spec.activity_mask >= 0)
          return std::nullopt;
        fail(e.name + ": expected " + std::to_string(spec.arity) + " args");
      }
      std::vector<int> idata;
      // Whether argument 0 was written as a bare `int` rather than an
      // array. Same test as the two-group path below, and for the same
      // reason: a length-1 array is a container that must match the other
      // arguments' size, a scalar broadcasts.
      bool scalar_outcome = false;
      if (spec.integer_args == 1) {
        idata = int_arg_values(e.args[0]);
        scalar_outcome = e.args[0].type_ == "UInt" && idata.size() == 1;
      } else if (spec.integer_args == 2) {
        // Group length -1 marks a language-level scalar (broadcast in
        // stan-math); a length-1 array stays a vector, as CmdStan would
        // instantiate it.
        auto put = [&](const mir::Expr& a) {
          auto vals = int_arg_values(a);
          const bool scalar = a.type_ == "UInt" && vals.size() == 1;
          idata.push_back(scalar ? -1 : (int)vals.size());
          idata.insert(idata.end(), vals.begin(), vals.end());
        };
        put(e.args[0]);
        put(e.args[1]);
      }
      std::vector<int> ins;
      SlotInfo shapes[6]{};
      SlotInfo result_si{0, 0, true};
      bool result_autodiff = false;
      uint8_t variant =
          spec.activity_mask < 0 ? 0 : (uint8_t)spec.activity_mask;
      for (size_t i = spec.integer_args; i < e.args.size(); ++i) {
        const Val arg = lower_expr(e.args[i]);
        shapes[i - spec.integer_args] = arg.si;
        ins.push_back(arg.slot);
        result_si.param_free = result_si.param_free && arg.si.param_free;
        result_autodiff = result_autodiff || arg.autodiff;
        if (spec.activity_mask < 0 && !e.args[i].data_only)
          variant |= (uint8_t)(1u << (i - spec.integer_args));
      }
      // A scalar outcome against vectorized real arguments: replicate it to
      // the lane count. The kernels map the whole integer group as one
      // Eigen::VectorXi, so a scalar arrived at stan-math as a size-1
      // container and lost against a longer argument on
      // check_consistent_sizes -- "Failures variable has size = 1, but
      // Number of successes parameter has size 2". Expanding here rather
      // than adding a scalar-bound instantiation to every kernel keeps the
      // graph identical to the one the equivalent array-outcome model
      // produces, which is already the verified shape: each of these is a
      // per-lane reduction (sum for the lpmfs and lcdf/lccdf, product for
      // the cdfs), so N copies of the outcome is the same math in the same
      // order as one broadcast scalar. binomial_logit_glm below does the
      // same thing for the same reason.
      if (spec.lane_outcome && scalar_outcome) {
        int64_t lanes = 1;
        for (int slot : ins) lanes = std::max(lanes, g.slots[slot].len);
        if (lanes > 1) {
          // By value: assign() may reallocate, and a reference into the
          // vector being assigned would dangle mid-fill.
          const int outcome = idata[0];
          idata.assign((size_t)lanes, outcome);
        }
      }
      if (propto(e)) variant |= 0x80u;
      if (spec.glm_layout) {
        // X must be a data matrix; append its dims to idata.
        const SlotInfo& xsi = shapes[0];
        if (!is_matrix(xsi) || !xsi.param_free)
          fail(e.name + ": X must be a data matrix");
        // A GLM's outcome group is one value per ROW of X, and the kernels
        // map exactly that many out of idata -- so a group of any other
        // length is refused here rather than read past the end of the
        // vector. These are the entries left lane_outcome = false, and not
        // by oversight: that expansion sizes itself from the longest real
        // argument, which for a GLM is X at rows*cols.
        //
        // Nor could poisson_log_glm simply replicate to `rows`. stan-math
        // accepts a language-level scalar outcome and broadcasts it, but
        // its <false> form then subtracts lgamma(y+1) ONCE rather than once
        // per row (four rows of y = 3: -4.98, against the -10.36 the
        // replicated array gives, with identical gradients). Replicating
        // would buy the right gradients and an lp a constant off CmdStan's,
        // which is the one thing these kernels exist to get right. The
        // other two were measured and do not have that problem; they share
        // this layout and this check, so they are refused with it. Refusing
        // by name is what the vector-alpha form already gets; see
        // docs/coverage.md.
        if ((int64_t)idata.size() != xsi.rows)
          fail(e.name + ": outcome has " + std::to_string(idata.size()) +
                   " value(s) but X has " + std::to_string(xsi.rows) +
                   " rows; a scalar or short outcome is unsupported",
               e.raw);
        idata.push_back((int)xsi.rows);
        idata.push_back((int)xsi.cols);
      }
      if (spec.shape == DensityShape::FirstMatrixRows) {
        if (!is_matrix(shapes[0])) {
          if (e.name == "lkj_cov_lpdf") fail("lkj_cov needs a matrix", e.raw);
          fail(e.name + " needs a matrix", e.raw);
        }
        idata = {(int)shapes[0].rows};
      } else if (spec.shape == DensityShape::FirstMatrixDimensions) {
        if (!is_matrix(shapes[0]) || !is_matrix(shapes[1]))
          fail(e.name + " needs matrix arguments", e.raw);
        idata = {(int)shapes[0].rows, (int)shapes[0].cols};
      } else if (spec.shape == DensityShape::LastMatrixRowsAndRepetitions) {
        const size_t last = ins.size() - 1;
        if (!is_matrix(shapes[last])) {
          if (e.name.rfind("multi_normal", 0) == 0)
            fail(e.name + ": needs a matrix argument (got length " +
                     std::to_string(g.slots[ins[last]].len) + ")",
                 e.raw);
          fail(e.name + " needs a matrix argument", e.raw);
        }
        const int64_t K = shapes[last].rows;
        idata = {(int)K, (int)(g.slots[ins[0]].len / K)};
      }
      Val dv =
          emit_raw(spec.opcode, ins, 1, result_si, idata, -1, result_autodiff);
      // GLM ops used to be the one density shape that got no variant at
      // all, so their kernels hardcoded propto=false and poisson_log_glm's
      // lp landed sum(log(y!)) -- 10.45 on a six-row test -- away from
      // CmdStan's, with the gradients already exact. They get the same
      // variant as everything else now. Their forwards bind arguments
      // explicitly rather than through mask_dispatch, so the mask reaches
      // only density_bwd, where it says to skip X -- which it already did
      // by X's null adjoint. Setting only the propto bit is NOT an option:
      // density_bwd reads a nonzero variant as a literal mask, so 0x80
      // alone means "no argument is active" and every gradient comes back
      // zero.
      g.ops.back().variant = variant;
      return dv;
    }

    if (e.name == "categorical_logit_lpmf" && e.args.size() == 2) {
      const Val outcome = lower_expr(e.args[0]);
      Val b = lower_expr(e.args[1]);
      return emit_categorical(e, outcome, b, true);
    }
    if (e.name == "categorical_lpmf" && e.args.size() == 2) {
      const Val outcome = lower_expr(e.args[0]);
      Val th = lower_expr(e.args[1]);
      return emit_categorical(e, outcome, th, false);
    }

    // gaussian_dlm_obs takes seven arguments and Op::in holds six, so it
    // cannot be lowered as one op at all. Raising the limit would add
    // bytes to every Op and every KernelCtx in every model for the sake
    // of one dynamic-linear-model density, so this refuses instead and
    // names the reason. See docs/coverage.md.
    if (e.name == "gaussian_dlm_obs_lpdf")
      fail("gaussian_dlm_obs takes 7 arguments and an op holds 6", e.raw);

    // The three GLMs whose argument shapes the recorder cannot express.
    // idata is the outcome (two groups for binomial) then rows, cols.
    if (e.name == "binomial_logit_glm_lpmf" && e.args.size() == 5) {
      std::vector<int> idata = int_arg_values(e.args[0]);
      std::vector<int> NN = int_arg_values(e.args[1]);
      Val X = lower_expr(e.args[2]);
      Val alpha = lower_expr(e.args[3]);
      Val beta = lower_expr(e.args[4]);
      if (!is_matrix(X.si)) fail("binomial_logit_glm needs a matrix", e.raw);
      // Both int groups are one value per row, so a scalar broadcasts.
      // By value, as the lane_outcome expansion above is: assign() may
      // reallocate, and a reference into the vector being assigned would
      // dangle mid-fill.
      const int64_t rows = X.si.rows;
      if ((int64_t)idata.size() == 1) {
        const int outcome = idata[0];
        idata.assign(rows, outcome);
      }
      if ((int64_t)NN.size() == 1) {
        const int trials = NN[0];
        NN.assign(rows, trials);
      }
      idata.insert(idata.end(), NN.begin(), NN.end());
      idata.push_back((int)rows);
      idata.push_back((int)X.si.cols);
      Val v = emit_value(OP_BINOMIAL_LOGIT_GLM_LPMF, {X, alpha, beta}, 1, {},
                         idata);
      g.ops.back().variant = (uint8_t)((propto(e) ? 0x80u : 0u) | 0x7u);
      return v;
    }
    if ((e.name == "categorical_logit_glm_lpmf" ||
         e.name == "ordered_logistic_glm_lpmf") &&
        e.args.size() == 4) {
      std::vector<int> idata = int_arg_values(e.args[0]);
      Val X = lower_expr(e.args[1]);
      Val a2 = lower_expr(e.args[2]);
      Val a3 = lower_expr(e.args[3]);
      if (!is_matrix(X.si)) fail(e.name + " needs a matrix", e.raw);
      if ((int64_t)idata.size() == 1) {
        // By value, as the lane_outcome expansion is: assign() may
        // reallocate, and a reference into the vector being assigned would
        // dangle mid-fill.
        const int outcome = idata[0];
        idata.assign(X.si.rows, outcome);
      }
      idata.push_back((int)X.si.rows);
      idata.push_back((int)X.si.cols);
      // categorical: (y, x, alpha, beta). ordered: (y, x, beta, cuts).
      // Both pass their two real arguments in order, so the kernel reads
      // in[1] and in[2] and knows from its Kind what they mean.
      Val v = emit_value(e.name == "categorical_logit_glm_lpmf"
                             ? OP_CATEGORICAL_LOGIT_GLM_LPMF
                             : OP_ORDERED_LOGISTIC_GLM_LPMF,
                         {X, a2, a3}, 1, {}, idata);
      g.ops.back().variant = (uint8_t)((propto(e) ? 0x80u : 0u) | 0x7u);
      return v;
    }

    if (e.name == "normal_id_glm_lpdf" && e.args.size() == 5) {
      Val y = lower_expr(e.args[0]);
      Val X = lower_expr(e.args[1]);
      if (!is_matrix(X.si) || !X.si.param_free)
        fail("normal_id_glm: X must be a data matrix", e.raw);
      Val alpha = lower_expr(e.args[2]);
      Val beta = lower_expr(e.args[3]);
      Val sigma = lower_expr(e.args[4]);
      uint8_t variant = 0;
      for (int i = 0; i < 5; ++i)
        if (!e.args[i].data_only) variant |= (uint8_t)(1u << i);
      if (propto(e)) variant |= 0x80u;
      Val v = emit_value(OP_NORMAL_ID_GLM_LPDF, {y, X, alpha, beta, sigma}, 1,
                         {}, {(int)X.si.rows, (int)X.si.cols});
      g.ops.back().variant = variant;
      return v;
    }
    return std::nullopt;
  }

  // Elementwise math, reductions, and dot products.
  std::optional<Val> lower_eltwise_fn(const mir::Expr& e) {
    // Elementwise binaries.
    static const std::map<std::string, uint16_t> kBin = {
        {"Plus__", OP_ADD},
        {"Minus__", OP_SUB},
        {"Divide__", OP_DIV},
        {"EltTimes__", OP_MUL},
        {"EltDivide__", OP_DIV},
        {"Pow__", OP_POW},
        {"pow", OP_POW},
        // The named spellings of the same operators. `divide` is the one
        // that is not simply the operator renamed: `rv / A` is a solve,
        // but stan::math::divide only ever divides by a scalar or divides
        // a scalar elementwise, so it is OP_DIV on every one of its
        // overloads (deps/math/stan/math/prim/fun/divide.hpp).
        {"add", OP_ADD},
        {"subtract", OP_SUB},
        {"divide", OP_DIV},
        {"elt_multiply", OP_MUL},
        {"elt_divide", OP_DIV},
// Generated from STANLI_SCALAR_BINARY_LIST (optable.hpp), which also made
// the opcode and the kernel. multiply_log is the pre-optimizer spelling of
// lmultiply, so it rides the same opcode.
#define STANLI_BINARY_TABLE(code, name, fn) {#name, code},
        STANLI_SCALAR_BINARY_LIST(STANLI_BINARY_TABLE)
#undef STANLI_BINARY_TABLE
            {"multiply_log", OP_LMULTIPLY},
    };
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
    if (e.name == "LDivide__" ||
        (e.name == "Divide__" && e.args.at(1).type_ == "UMatrix")) {
      const bool left = e.name == "LDivide__";
      Val a = lower_expr(e.args[0]);
      Val b = lower_expr(e.args[1]);
      const Val& divisor = left ? a : b;
      const Val& dividend = left ? b : a;
      // rows <= 0 is a matrix view whose shape the lowering never resolved;
      // the kernel would map n x n over the slot and read past it.
      if (!is_matrix(divisor.si) || divisor.si.rows != divisor.si.cols ||
          divisor.si.rows <= 0)
        fail(e.name + ": divisor is not a square matrix of known size", e.raw);
      const int64_t n = divisor.si.rows;
      // A non-matrix dividend is the vector its side implies -- a column
      // under `\`, a row under `/` -- the same rule Times__ follows. Either
      // way the shared extent is n and the result has the dividend's shape.
      const bool dm = is_matrix(dividend.si);
      if (!dm && !is_vector(dividend.si) && !is_row_vector(dividend.si))
        fail(e.name + ": dividend is not a matrix or vector", e.raw);
      const int64_t shared = dm ? (left ? dividend.si.rows : dividend.si.cols)
                                : g.slots[dividend.slot].len;
      if (shared != n)
        fail(e.name + ": inner dimension mismatch (" + std::to_string(n) + "x" +
                 std::to_string(n) + " against " + std::to_string(shared) + ")",
             e.raw);
      const int64_t k = dm ? (left ? dividend.si.cols : dividend.si.rows) : 1;
      Val v = emit_value(left ? OP_MDIVIDE_LEFT : OP_MDIVIDE_RIGHT, {a, b},
                         n * k, dividend.si, {(int)n, (int)k});
      // The kernel solves through the operand types CmdStan's generated code
      // would have used, because stan-math answers differently for each: bit
      // 0 is the scalar type (var reaches other overloads than double), bit 1
      // says the dividend is a vector rather than a one-column matrix.
      g.ops.back().variant = (uint8_t)((v.autodiff ? 1u : 0u) | (dm ? 0u : 2u));
      return v;
    }
    // multiply is the named spelling of `*`, including its linear algebra:
    // the branches below pick matvec, GEMM, outer and inner products off
    // the operand views and the result type, which the alias shares.
    if (e.name == "Times__" || (e.name == "multiply" && e.args.size() == 2)) {
      Val a = lower_expr(e.args[0]);
      Val b = lower_expr(e.args[1]);
      // Scalar on either side is an elementwise scale, whatever shape the
      // other operand carries.
      const bool a_scalar = is_scalar(a);
      const bool b_scalar = is_scalar(b);
      if (a_scalar || b_scalar) {
        const Val& shaped = a_scalar ? b : a;
        SlotInfo si = shaped.si;
        si.param_free = a.si.param_free && b.si.param_free;
        const int64_t n = a_scalar ? g.slots[b.slot].len : g.slots[a.slot].len;
        return emit_value(OP_MUL, {a, b}, n, si);
      }
      if (is_matrix(a.si)) {
        if (a.si.param_free && is_vector(b.si)) {
          // Data matrix * vector keeps the tuned MATVEC kernel (its
          // accumulation order is matched to the var path).
          if (g.slots[b.slot].len != a.si.cols)
            fail(e.name + ": inner dimension mismatch", e.raw);
          return emit_value(OP_MATVEC, {a, b}, a.si.rows, view_of("UVector"),
                            {(int)a.si.rows, (int)a.si.cols});
        }
        // General product; a vector operand is one column.
        const int64_t cb = is_matrix(b.si) ? b.si.cols : 1;
        const int64_t rb = is_matrix(b.si) ? b.si.rows : g.slots[b.slot].len;
        if (rb != a.si.cols)
          fail(e.name + ": inner dimension mismatch (" +
                   std::to_string(a.si.rows) + "x" + std::to_string(a.si.cols) +
                   " times " + std::to_string(rb) + "x" + std::to_string(cb) +
                   ")",
               e.raw);
        SlotInfo si = cb == 1 ? view_of("UVector") : matrix_view(a.si.rows, cb);
        Val v = emit_value(OP_GEMM, {a, b}, a.si.rows * cb, si,
                           {(int)a.si.rows, (int)a.si.cols, (int)cb});
        return v;
      }
      // vector * row_vector with a matrix result is an outer product.
      if (is_vector(a.si) && is_row_vector(b.si) && e.type_ == "UMatrix") {
        const int64_t nr = g.slots[a.slot].len, nc = g.slots[b.slot].len;
        SlotInfo si = matrix_view(nr, nc);
        return emit_value(OP_GEMM, {a, b}, nr * nc, si, {(int)nr, 1, (int)nc});
      }
      if (is_row_vector(a.si) && is_matrix(b.si)) {
        const int64_t k = g.slots[a.slot].len;
        if (k != b.si.rows) fail(e.name + ": inner dimension mismatch", e.raw);
        return emit_value(OP_GEMM, {a, b}, b.si.cols, view_of("URowVector"),
                          {1, (int)k, (int)b.si.cols});
      }
      // row_vector * vector with scalar result type is an inner product.
      if (is_row_vector(a.si) && is_vector(b.si) &&
          (e.type_ == "UReal" || e.type_ == "UInt")) {
        if (g.slots[a.slot].len != g.slots[b.slot].len)
          fail(e.name + ": inner dimension mismatch", e.raw);
        return emit_value(OP_DOT, {a, b}, 1);
      }
      if (a.si.kind != ViewKind::Flat || b.si.kind != ViewKind::Flat)
        fail(e.name + ": unsupported container product", e.raw);
      const int64_t len = std::max(g.slots[a.slot].len, g.slots[b.slot].len);
      return emit_value(OP_MUL, {a, b}, len);
    }
    // fma from --O1 partial evaluation (`c + a*b`) or written explicitly:
    // fused (std::fma), elementwise with scalar broadcast on any argument.
    if (e.name == "fma" && e.args.size() == 3) {
      Val a = lower_expr(e.args[0]);
      Val b = lower_expr(e.args[1]);
      Val c = lower_expr(e.args[2]);
      const int64_t la = g.slots[a.slot].len, lb = g.slots[b.slot].len,
                    lc = g.slots[c.slot].len;
      const int64_t n = std::max(la, std::max(lb, lc));
      for (int64_t l : {la, lb, lc})
        if (l != n && l != 1) fail("fma: incompatible lengths", e.raw);
      // The shape of whichever operand carries one, like the binaries.
      SlotInfo si = shape_of(a, b);
      if (is_scalar(a) && is_scalar(b)) si = shape_of(a, c);
      si.param_free = a.si.param_free && b.si.param_free && c.si.param_free;
      return emit_value(OP_FMA, {a, b, c}, n, si);
    }
    auto bit = kBin.find(e.name);
    if (bit != kBin.end()) {
      Val a = lower_expr(e.args[0]);
      Val b = lower_expr(e.args[1]);
      const int64_t la = g.slots[a.slot].len, lb = g.slots[b.slot].len;
      const bool as = is_scalar(a), bs = is_scalar(b);
      if (!as && !bs && !same_view(a.si, la, b.si, lb))
        fail(e.name + ": incompatible logical views");
      // Elementwise results keep the matrix shape of whichever operand
      // has one; losing it would make a later Times__ miss the matvec.
      SlotInfo si = shape_of(a, b);
      const int64_t n = as ? lb : (bs ? la : la);
      return emit_value(bit->second, {a, b}, n, si);
    }

    // The same surface with one int argument, from the two int lists in
    // optable.hpp. The flag is which position holds the int, which is the
    // only thing that varies across the nine.
    static const std::map<std::string, std::pair<uint16_t, bool>> kBinInt = {
#define STANLI_BINARY_INT_TABLE(code, name, fn) {#name, {code, true}},
        STANLI_SCALAR_BINARY_INT_FIRST_LIST(STANLI_BINARY_INT_TABLE)
#undef STANLI_BINARY_INT_TABLE
#define STANLI_BINARY_INT_TABLE(code, name, fn) {#name, {code, false}},
            STANLI_SCALAR_BINARY_INT_SECOND_LIST(STANLI_BINARY_INT_TABLE)
#undef STANLI_BINARY_INT_TABLE
    };
    auto iit = kBinInt.find(e.name);
    if (iit != kBinInt.end() && e.args.size() == 2)
      return lower_binary_int(iit->second.first, iit->second.second, e);

    // Elementwise unaries + reductions.
    static const std::map<std::string, uint16_t> kUn = {
// Generated from STANLI_SCALAR_UNARY_LIST (optable.hpp), which also made
// the opcode and the kernel.
#define STANLI_UNARY_TABLE(code, name, value, delta, topology) {#name, code},
        STANLI_SCALAR_UNARY_LIST(STANLI_UNARY_TABLE)
#undef STANLI_UNARY_TABLE
            {"PMinus__", OP_NEG},
        // minus is the named spelling of the unary operator, so it is the
        // same negation over the same shapes.
        {"minus", OP_NEG},
        // stanc3's Lower_expr.ml maps std_normal_qf onto stan::math::inv_Phi;
        // one opcode keeps the two spellings from drifting apart.
        {"std_normal_qf", OP_INV_PHI},
        // trigamma is the one unary whose derivative has no closed form to
        // put in the shared list: Math differentiates AS121's recurrence
        // through its own tape, so the kernel does too (scalar_unary_ad.cpp).
        {"trigamma", OP_TRIGAMMA},
        {"exp", OP_EXPV},
        {"log", OP_LOGV},
        {"inv_logit", OP_INV_LOGIT},
        {"sqrt", OP_SQRT},
        {"square", OP_SQUARE},
        {"log1m", OP_LOG1M},
        {"softmax", OP_SOFTMAX},
        {"tanh", OP_TANHV},
        {"cumulative_sum", OP_CUMSUM},
        {"log_softmax", OP_LOG_SOFTMAX},
    };
    auto uit = kUn.find(e.name);
    if (uit != kUn.end()) {
      Val a = lower_expr(e.args[0]);
      SlotInfo si = a.si;
      // Shape-preserving unaries keep rows/cols (softmax/cumulative_sum
      // are vector-only, so they never carry one).
      if (uit->second != OP_SOFTMAX && uit->second != OP_CUMSUM) {
        if (e.type_ == "UMatrix" && !is_matrix(si))
          fail(e.name + ": matrix result has unknown logical extents", e.raw);
        stamp_kind(&si, e.type_);
      } else {
        si = view_of(e.type_);
      }
      si.param_free = a.si.param_free;
      return emit_value(uit->second, {a}, g.slots[a.slot].len, si);
    }
    // plus, and its operator spelling, are the identity on every shape.
    if (e.name == "PPlus__" || (e.name == "plus" && e.args.size() == 1))
      return lower_expr(e.args[0]);
    if (e.name == "logit") {
      Val a = lower_expr(e.args[0]);
      return emit_value(OP_LOGIT, {a}, g.slots[a.slot].len, a.si);
    }
    if ((e.name == "min" || e.name == "max") && in_write_array) {
      // Preserve the construction-time path for well-formed data-only
      // extrema, including the scalar two-argument overload.  Dynamic
      // lowering is deliberately much narrower.
      if (e.args.size() == 1 || e.args.size() == 2)
        if (auto v = fold_const(e)) return *v;
      const mir::ExtremaKind kind = mir::extrema_kind(e);
      if (udf_depth != 0 || kind == mir::ExtremaKind::Legacy)
        fail("min/max expression surface stays on WaInterp", e.raw);
      Val a = lower_expr(e.args[0]);
      if ((!is_vector(a.si) && !is_row_vector(a.si)) || g.slots[a.slot].len < 0)
        fail("min/max needs one vector or row-vector argument", e.raw);
      Val result = emit_value(OP_EXTREMA_VEC, {a}, 1);
      result.autodiff = false;
      g.ops.back().variant = kind == mir::ExtremaKind::Max ? 1u : 0u;
      return result;
    }
    if (e.name == "mean") {
      Val a = lower_expr(e.args[0]);
      return emit_value(OP_MEAN, {a}, 1);
    }
    if (e.name == "prod" && in_write_array) {
      // Preserve the pre-existing construction-time behavior for data-only
      // products.  OP_PROD_VEC is only the dynamic write_array tranche.
      if (auto v = fold_const(e)) return *v;
      if (e.args.size() != 1 || e.type_ != "UReal" ||
          e.unsized.leaf != mir::UnsizedLeaf::Real || e.unsized.depth != 0)
        fail("prod needs exactly one scalar-real result", e.raw);
      const mir::Expr& arg = e.args[0];
      const bool vector_arg = arg.type_ == "UVector" &&
                              arg.unsized.leaf == mir::UnsizedLeaf::Vector &&
                              arg.unsized.depth == 0;
      const bool row_vector_arg =
          arg.type_ == "URowVector" &&
          arg.unsized.leaf == mir::UnsizedLeaf::RowVector &&
          arg.unsized.depth == 0;
      if (!vector_arg && !row_vector_arg)
        fail("prod needs one vector or row-vector argument", e.raw);
      if (udf_depth != 0 || !prod_native_surface(arg))
        fail("prod expression surface stays on WaInterp", e.raw);
      Val a = lower_expr(arg);
      if ((!is_vector(a.si) && !is_row_vector(a.si)) ||
          g.slots[a.slot].len <= 0)
        fail("prod needs a nonempty vector or row-vector argument", e.raw);
      const mir::ProdGrouping grouping = mir::prod_grouping(arg);
      if (grouping == mir::ProdGrouping::Legacy)
        fail("prod expression grouping is not native", e.raw);
      Val result = emit_value(OP_PROD_VEC, {a}, 1);
      g.ops.back().variant = grouping == mir::ProdGrouping::Scalar ? 1u : 0u;
      return result;
    }
    if (e.name == "rep_vector") {
      Val a = lower_expr(e.args[0]);
      const long n = eval_int(e.args[1]);
      return emit_value(OP_REP_VEC, {a}, n, view_of("UVector"));
    }
    if (e.name == "log_sum_exp" || e.name == "sum") {
      // One argument is the reduction; two is the elementwise form below.
      if (e.name == "log_sum_exp" && e.args.size() == 2)
        return lower_binary_mix(OP_LSE2, e);
      const bool int_surface =
          e.name == "sum" &&
          (e.type_ == "UInt" || e.unsized.leaf == mir::UnsizedLeaf::Int ||
           (!e.args.empty() &&
            e.args[0].unsized.leaf == mir::UnsizedLeaf::Int));
      if (int_surface && in_write_array) {
        if (runtime_int_sum_candidate(e)) return lower_runtime_int_sum(e);
        if (!is_int_sum_surface(e))
          fail(
              "runtime integer sum needs one one-dimensional int-array "
              "argument and a scalar int result",
              e.raw);
        if (e.args[0].kind != mir::Expr::Var || expr_effectful(e))
          fail("direct runtime integer sum stays on WaInterp", e.raw);
        // A param-free named array retains the legacy OP_SUM_VEC/fold path.
      }
      if (e.args.size() != 1)
        fail(e.name + ": reduction needs exactly one argument", e.raw);
      Val a = lower_expr(e.args[0]);
      return emit_value(e.name == "sum" ? OP_SUM_VEC : OP_LOG_SUM_EXP, {a}, 1);
    }
    if (e.name == "log_diff_exp" && e.args.size() == 2)
      return lower_binary_mix(OP_LOG_DIFF_EXP, e);
    if (e.name == "log_mix" && e.args.size() == 3) {
      Val a = lower_expr(e.args[0]);
      Val b = lower_expr(e.args[1]);
      Val c = lower_expr(e.args[2]);
      return emit_value(OP_LOG_MIX, {a, b, c}, 1);
    }
    if (e.name == "dot_product") {
      Val a = lower_expr(e.args[0]);
      Val b = lower_expr(e.args[1]);
      return emit_value(OP_DOT, {a, b}, 1);
    }

    if (e.name == "dot_self") {
      Val a = lower_expr(e.args[0]);
      return emit_value(OP_DOT, {a, a}, 1);
    }

    // squared_distance(x, y) = dot_self(x - y). Two graph kernels that
    // already carry native adjoints, so no new opcode. It does not go
    // through shape_of: the language pairs a vector with a row_vector
    // here, which same_view rejects and stan-math accepts, and the only
    // thing the difference could change -- element order -- is the same
    // on both sides because a length is all either view carries.
    if (e.name == "squared_distance" && e.args.size() == 2) {
      Val a = lower_expr(e.args[0]);
      Val b = lower_expr(e.args[1]);
      const int64_t la = g.slots[a.slot].len, lb = g.slots[b.slot].len;
      if (la != lb) fail(e.name + ": arguments must match in size", e.raw);
      SlotInfo si;
      si.param_free = a.si.param_free && b.si.param_free;
      if (la > 1) si.kind = ViewKind::Vector;
      Val d = emit_value(OP_SUB, {a, b}, la, si);
      return emit_value(OP_DOT, {d, d}, 1);
    }
    return std::nullopt;
  }

  // Matrix shape and algebra: transposes, reshapes, factorizations,
  // slices, and concatenations.
  std::optional<Val> lower_matrix_fn(const mir::Expr& e) {
    if ((e.name == "Transpose__" || e.name == "transpose") &&
        e.args.size() == 1) {
      Val a = lower_expr(e.args[0]);
      // Vector <-> row_vector transpose is a type change, not a layout one.
      if (!is_matrix(a.si)) {
        stamp_kind(&a.si, e.type_);
        return a;
      }
      SlotInfo si = matrix_view(a.si.cols, a.si.rows, a.si.param_free);
      return emit_value(OP_TRANSPOSE, {a}, g.slots[a.slot].len, si,
                        {(int)a.si.rows, (int)a.si.cols});
    }
    if ((e.name == "diag_pre_multiply" || e.name == "diag_post_multiply") &&
        e.args.size() == 2) {
      // diag_pre_multiply(v, M) = diag_matrix(v) * M (and the mirror);
      // the explicit zeros contribute exactly nothing to each sum.
      const bool pre = e.name.find("_pre_") != std::string::npos;
      Val v = lower_expr(e.args[pre ? 0 : 1]);
      Val m = lower_expr(e.args[pre ? 1 : 0]);
      const int64_t n = g.slots[v.slot].len;
      SlotInfo dsi = matrix_view(n, n, v.si.param_free);
      Val d = emit_value(OP_DIAG_MATRIX, {v}, n * n, dsi);
      Val a = pre ? d : m, b = pre ? m : d;
      SlotInfo si = matrix_view(a.si.rows, b.si.cols);
      return emit_value(OP_GEMM, {a, b}, si.rows * si.cols, si,
                        {(int)a.si.rows, (int)a.si.cols, (int)b.si.cols});
    }
    if (e.name == "multiply_lower_tri_self_transpose" && e.args.size() == 1) {
      Val L = lower_expr(e.args[0]);
      if (!is_matrix(L.si)) fail("multiply_lower_tri: needs a matrix", e.raw);
      SlotInfo tsi = matrix_view(L.si.cols, L.si.rows);
      Val Lt = emit_value(OP_TRANSPOSE, {L}, g.slots[L.slot].len, tsi,
                          {(int)L.si.rows, (int)L.si.cols});
      SlotInfo si = matrix_view(L.si.rows, L.si.rows);
      return emit_value(OP_GEMM, {L, Lt}, si.rows * si.cols, si,
                        {(int)L.si.rows, (int)L.si.cols, (int)L.si.rows});
    }
    if (e.name == "to_matrix" && (e.args.size() == 1 || e.args.size() == 3)) {
      // Col-major storage makes reshaping a relabelling. One argument on an
      // array[N] vector[S] value yields the N x S matrix stan-math builds
      // from it, which is the transpose of our array-major flat order.
      Val a = lower_expr(e.args[0]);
      SlotInfo si;
      si.param_free = a.si.param_free;
      if (e.args.size() == 3) {
        const int64_t rows = eval_int(e.args[1]);
        const int64_t cols = eval_int(e.args[2]);
        if (checked_product({rows, cols}, "to_matrix") != g.slots[a.slot].len)
          fail("to_matrix: requested shape does not match source length",
               e.raw);
        si = matrix_view(rows, cols, a.si.param_free);
        return Val{a.slot, a.autodiff, si};
      }
      if (is_matrix(a.si)) return Val{a.slot, a.autodiff, a.si};
      std::vector<int64_t> dims;
      if (is_array(a.si)) dims = array_shape(a.si).dims;
      if (dims.size() != 2) fail("to_matrix: unknown source shape", e.raw);
      // array-major (row-major) source -> col-major matrix of the same
      // logical shape: transpose the storage.
      si = matrix_view(dims[0], dims[1], a.si.param_free);
      return emit_value(OP_TRANSPOSE, {a}, g.slots[a.slot].len, si,
                        {(int)dims[1], (int)dims[0]});
    }
    if ((e.name == "to_vector" || e.name == "to_row_vector") &&
        e.args.size() == 1) {
      // Col-major flattening is the identity on our storage.
      Val a = lower_expr(e.args[0]);
      SlotInfo si = a.si;
      si.rows = 0;
      si.cols = 0;
      stamp_kind(&si, e.type_);
      return Val{a.slot, a.autodiff, si};
    }
    if (e.name == "rep_matrix") {
      SlotInfo si;
      if (e.args.size() == 3) {
        Val x = lower_expr(e.args[0]);  // scalar fill
        const long R = eval_int(e.args[1]), C = eval_int(e.args[2]);
        si = matrix_view(R, C);
        return emit_value(OP_REP_MAT, {x}, R * C, si, {(int)R, (int)C, 0});
      }
      if (e.args.size() == 2) {
        Val v = lower_expr(e.args[0]);
        const long n = eval_int(e.args[1]);
        const bool rowvec = e.args[0].type_ == "URowVector";
        const long R = rowvec ? n : g.slots[v.slot].len;
        const long C = rowvec ? g.slots[v.slot].len : n;
        si = matrix_view(R, C);
        return emit_value(OP_REP_MAT, {v}, R * C, si,
                          {(int)R, (int)C, rowvec ? 2 : 1});
      }
      fail("rep_matrix arity", e.raw);
    }
    if (e.name == "gp_exp_quad_cov" && e.args.size() == 3) {
      Val x = lower_expr(e.args[0]);
      Val alpha = lower_expr(e.args[1]);
      Val rho = lower_expr(e.args[2]);
      if (!x.si.param_free)
        fail("gp_exp_quad_cov: parameter inputs unsupported", e.raw);
      // x is array[N] real (D == 1) or array[N] vector[D], stored
      // array-major, so D falls out of the declared dims.
      int64_t D = 1;
      if (is_array(x.si) && array_shape(x.si).dims.size() == 2)
        D = array_shape(x.si).dims[1];
      const int64_t N = g.slots[x.slot].len / D;
      SlotInfo si = matrix_view(N, N);
      return emit_value(OP_GP_EXP_QUAD_COV, {x, alpha, rho}, N * N, si,
                        {(int)N, (int)D});
    }
    if (e.name == "diag_matrix" && e.args.size() == 1) {
      Val v = lower_expr(e.args[0]);
      const int64_t n = g.slots[v.slot].len;
      SlotInfo si = matrix_view(n, n);
      return emit_value(OP_DIAG_MATRIX, {v}, n * n, si);
    }
    if (e.name == "cholesky_decompose" && e.args.size() == 1) {
      Val a = lower_expr(e.args[0]);
      if (!is_matrix(a.si)) fail("cholesky_decompose needs a matrix", e.raw);
      if (a.si.rows != a.si.cols)
        fail("cholesky_decompose needs a square matrix", e.raw);
      SlotInfo si = a.si;
      si.param_free = a.si.param_free;
      return emit_value(OP_CHOLESKY, {a}, g.slots[a.slot].len, si,
                        {(int)a.si.rows});
    }

    if ((e.name == "eigenvalues_sym" || e.name == "eigenvectors_sym") &&
        e.args.size() == 1) {
      Val a = lower_expr(e.args[0]);
      if (!is_matrix(a.si)) fail(e.name + ": needs a matrix", e.raw);
      if (a.si.rows != a.si.cols)
        fail(e.name + ": needs a square matrix", e.raw);
      const int64_t n = a.si.rows;
      if (e.name == "eigenvalues_sym")
        return emit_value(OP_EIGENVALUES_SYM, {a}, n, view_of(e.type_),
                          {(int)n});
      SlotInfo si = matrix_view(n, n);
      return emit_value(OP_EIGENVECTORS_SYM, {a}, n * n, si, {(int)n});
    }
    if (e.name == "quad_form_diag" && e.args.size() == 2) {
      // quad_form_diag(M, v) = diag(v) * M * diag(v).
      Val m = lower_expr(e.args[0]);
      Val v = lower_expr(e.args[1]);
      if (!is_matrix(m.si)) fail("quad_form_diag: needs a matrix", e.raw);
      const int64_t n = g.slots[v.slot].len;
      SlotInfo dsi = matrix_view(n, n, v.si.param_free);
      Val d = emit_value(OP_DIAG_MATRIX, {v}, n * n, dsi);
      SlotInfo si = matrix_view(n, n);
      Val left =
          emit_value(OP_GEMM, {d, m}, n * n, si, {(int)n, (int)n, (int)n});
      return emit_value(OP_GEMM, {left, d}, n * n, si,
                        {(int)n, (int)n, (int)n});
    }

    if ((e.name == "append_row" || e.name == "append_col") &&
        e.args.size() == 2) {
      Val a = lower_expr(e.args[0]);
      Val b = lower_expr(e.args[1]);
      const int64_t la = g.slots[a.slot].len, lb = g.slots[b.slot].len;
      const LogicalDims da = logical_dims(a.si, la, e.name);
      const LogicalDims db = logical_dims(b.si, lb, e.name);
      if (e.name == "append_col") {
        if (da.rows != db.rows) fail("append_col row mismatch", e.raw);
        const LogicalDims out_dims{da.rows, da.cols + db.cols};
        const SlotInfo si = view_for_dims(e.type_, out_dims);
        // Every supported value is column-major under this logical view;
        // adding columns is therefore always a contiguous concatenation.
        return emit_value(OP_CONCAT2, {a, b}, la + lb, si);
      }
      if (da.cols != db.cols) fail("append_row column mismatch", e.raw);
      const LogicalDims out_dims{da.rows + db.rows, da.cols};
      const SlotInfo si = view_for_dims(e.type_, out_dims);
      if (out_dims.cols == 1)
        return emit_value(OP_CONCAT2, {a, b}, la + lb, si);

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
      return emit_value(OP_GATHER, {cat}, la + lb, si, idx);
    }
    if (e.name == "segment" && e.args.size() == 3) {
      Val a = lower_expr(e.args[0]);
      const long from = eval_int(e.args[1]);
      const long cnt = eval_int(e.args[2]);
      return emit_value(OP_SLICE, {a}, cnt, view_of(e.type_),
                        {(int)(from - 1)});
    }
    if (e.name == "sub_col" && e.args.size() == 4) {
      // sub_col(M, i, j, n) = M[i .. i+n-1, j]: contiguous in col-major.
      Val a = lower_expr(e.args[0]);
      if (!is_matrix(a.si)) fail("sub_col on a slot without matrix shape");
      const long i = eval_int(e.args[1]);
      const long j = eval_int(e.args[2]);
      const long n = eval_int(e.args[3]);
      return emit_value(OP_SLICE, {a}, n, view_of(e.type_),
                        {(int)((j - 1) * a.si.rows + i - 1)});
    }
    if (e.name == "col" && e.args.size() == 2) {
      Val a = lower_expr(e.args[0]);
      if (!is_matrix(a.si)) fail("col on a slot without matrix shape");
      const long j = eval_int(e.args[1]);
      return emit_value(OP_SLICE, {a}, a.si.rows, view_of(e.type_),
                        {(int)((j - 1) * a.si.rows)});
    }
    if (e.name == "row" && e.args.size() == 2) {
      Val a = lower_expr(e.args[0]);
      if (!is_matrix(a.si)) fail("row on a slot without matrix shape");
      const long i = eval_int(e.args[1]);
      return emit_value(OP_SLICE_STRIDED, {a}, a.si.cols, view_of(e.type_),
                        {(int)(i - 1), (int)a.si.rows});
    }
    if ((e.name == "head" || e.name == "tail") && e.args.size() == 2) {
      Val a = lower_expr(e.args[0]);
      const long n = eval_int(e.args[1]);
      const long off = e.name == "head" ? 0 : g.slots[a.slot].len - n;
      return emit_value(OP_SLICE, {a}, n, view_of(e.type_), {(int)off});
    }
    return std::nullopt;
  }

  // stan-math's own defaults differ per solver: rk45 1e-6/1e-6/1e6 (the
  // OdeSpec field initializers), bdf 1e-10/1e-10/1e8. Using one set for
  // both left one_comp_mm's gradients 2.9e-6 off CmdStan, so both
  // families stamp them from here.
  void stamp_ode_defaults(OdeSpec& spec) {
    if (!spec.stiff) return;
    spec.rtol = 1e-10;
    spec.atol = 1e-10;
    spec.max_steps = 100000000;
  }

  // The op tail both ODE families share: report an interpreter fallback,
  // emit OP_ODE and hand the spec to the graph.
  Val emit_ode(std::shared_ptr<OdeSpec> spec, const Val& z0, const Val& theta,
               int64_t N, int64_t S, SlotInfo result_si) {
    // Falling back to the interpreter is correct but ~30x slower, so make
    // it findable rather than silent.
    if (!spec->prog.ok && std::getenv("STANLI_DEBUG_ODE"))
      std::fprintf(stderr,
                   "stanli: ODE right-hand side %s falls back to the "
                   "interpreter: %s\n",
                   spec->rhs_name.c_str(), spec->prog.why.c_str());
    Val v = emit_value(OP_ODE, {z0, theta}, N * S, result_si, {(int)N, (int)S});
    // Bit 2 says the low bits explicitly describe the C++ scalar types
    // selected by stanc's adlevels: bit 0 for y0, bit 1 for theta. Runtime
    // adjoint storage is deliberately not used for this decision -- a
    // write_array value can depend on q while still instantiating on double.
    g.ops.back().variant = (uint8_t)(0x4u | (z0.autodiff ? 0x1u : 0u) |
                                     (theta.autodiff ? 0x2u : 0u));
    g.ops.back().udata = spec.get();
    g.udata_pool.push_back(std::move(spec));
    return v;
  }

  SlotInfo ode_result_view(const mir::Expr& e, int64_t N, int64_t S) {
    if (e.unsized.depth == 1 && e.unsized.leaf == mir::UnsizedLeaf::Vector)
      return array_view({N, S}, ViewKind::Vector);
    if (e.unsized.depth == 2 && (e.unsized.leaf == mir::UnsizedLeaf::Real ||
                                 e.unsized.leaf == mir::UnsizedLeaf::Int))
      return array_view({N, S}, ViewKind::Flat);
    fail("ODE result has unsupported logical type", e.raw);
  }

  // The modern variadic family: ode_rk45 / ode_bdf / ode_adams / ode_ckrk
  // and their _tol forms.
  //
  //   ode_SOLVER(f, y0, t0, ts, ...args)
  //   ode_SOLVER_tol(f, y0, t0, ts, rtol, atol, max_steps, ...args)
  //
  // Everything after the fixed prefix is passed straight through to the
  // right-hand side, in any number and any type. They reduce to the same
  // calling convention integrate_ode_* has always used -- autodiff reals
  // packed in order, data reals packed in order, integers as compile-time
  // constants -- so the kernel and the register machine are unchanged;
  // only the packing at this end is new.
  std::optional<Val> lower_ode_variadic(const mir::Expr& e) {
    static const std::vector<std::pair<const char*, OdeSpec::Solver>> kSolvers =
        {{"ode_bdf", OdeSpec::BDF},
         {"ode_adams", OdeSpec::ADAMS},
         {"ode_rk45", OdeSpec::RK45},
         {"ode_ckrk", OdeSpec::CKRK}};
    std::string base = e.name;
    bool with_tol = false;
    if (base.size() > 4 && base.compare(base.size() - 4, 4, "_tol") == 0) {
      with_tol = true;
      base = base.substr(0, base.size() - 4);
    }
    const auto sit =
        std::find_if(kSolvers.begin(), kSolvers.end(),
                     [&](const auto& s) { return base == s.first; });
    if (sit == kSolvers.end()) return std::nullopt;

    const size_t fixed = with_tol ? 7 : 4;
    if (e.args.size() < fixed) fail(e.name + ": unexpected arity", e.raw);
    auto spec = std::make_shared<OdeSpec>();
    if (fun_defs.find(e.args[0].name) == fun_defs.end())
      fail(e.name + ": unknown right-hand side " + e.args[0].name, e.raw);
    spec->adopt(fun_defs);
    spec->rhs_name = e.args[0].name;
    spec->solver = sit->second;
    spec->stiff =
        spec->solver == OdeSpec::BDF || spec->solver == OdeSpec::ADAMS;
    stamp_ode_defaults(*spec);
    spec->t0 = const_values(e.args[2]).at(0);
    spec->ts = const_values(e.args[3]);
    if (with_tol) {
      spec->rtol = const_values(e.args[4]).at(0);
      spec->atol = const_values(e.args[5]).at(0);
      spec->max_steps = (long)const_values(e.args[6]).at(0);
    }

    // Classify and pack. Data arguments fold into the spec here and never
    // reach the graph; autodiff ones are concatenated in argument order
    // into the single theta input the op takes, which is the order
    // compile_rhs_args assigns their register sub-ranges in.
    std::vector<RhsArg> rargs;
    std::vector<Val> param_parts;
    for (size_t k = fixed; k < e.args.size(); ++k) {
      const mir::Expr& a = e.args[k];
      RhsArg ra;
      const bool is_int = a.unsized.leaf == mir::UnsizedLeaf::Int;
      if (is_int && a.data_only) {
        ra.is_int = true;
        ra.ints = const_ints(a);
      } else if (a.data_only) {
        // One evaluation, held in a local. Calling const_values(a) twice
        // and taking begin() from one temporary and end() from the other
        // is an invalid range, and it does not fail loudly: it appended
        // hundreds of garbage doubles to x_r and surfaced much later as
        // "ode parameters and data[927] is nan".
        const std::vector<double> vals = const_values(a);
        ra.len = (int)vals.size();
        spec->x_r.insert(spec->x_r.end(), vals.begin(), vals.end());
      } else {
        if (is_int)
          fail(e.name + ": integer argument " + std::to_string(k - fixed + 1) +
                   " is not data",
               e.raw);
        const Val v = lower_expr(a);
        ra.is_param = true;
        ra.len = (int)g.slots[v.slot].len;
        param_parts.push_back(v);
      }
      rargs.push_back(std::move(ra));
    }

    Val z0 = lower_expr(e.args[1]);
    const int64_t S = g.slots[z0.slot].len;
    const int64_t N = (int64_t)spec->ts.size();

    // One contiguous theta. A model with a single parameter argument --
    // which is most of them -- gets its slot used directly and pays for
    // no copy at all; more than one chains through CONCAT2, whose
    // backward already splits the adjoint back out.
    Val theta;
    if (param_parts.empty()) {
      theta = constant(0.0);
      // len 1, unread: n_th is 0
    } else {
      theta = param_parts[0];
      int64_t acc = g.slots[theta.slot].len;
      for (size_t k = 1; k < param_parts.size(); ++k) {
        const int64_t add = g.slots[param_parts[k].slot].len;
        theta = emit_value(OP_CONCAT2, {theta, param_parts[k]}, acc + add);
        acc += add;
      }
    }

    spec->args = rargs;
    spec->prog = compile_rhs_args(*spec->rhs(), *spec->funs(), (int)S, rargs);
    return emit_ode(std::move(spec), z0, theta, N, S, ode_result_view(e, N, S));
  }

  // The integrate_ode_* family.
  std::optional<Val> lower_ode_fn(const mir::Expr& e) {
    if (auto v = lower_ode_variadic(e)) return v;
    if (e.name.rfind("integrate_ode_", 0) == 0) {
      // integrate_ode_*(f, z_init, t0, ts, theta, x_r, x_i[, rtol, atol,
      // max_steps]). Everything but z_init and theta is data, and is
      // captured in the spec the kernel reads through the op payload.
      if (e.args.size() < 7) fail(e.name + ": unexpected arity", e.raw);
      auto spec = std::make_shared<OdeSpec>();
      auto fit = fun_defs.find(e.args[0].name);
      if (fit == fun_defs.end())
        fail(e.name + ": unknown right-hand side " + e.args[0].name, e.raw);
      spec->adopt(fun_defs);
      spec->rhs_name = e.args[0].name;
      spec->stiff = e.name.find("bdf") != std::string::npos;
      spec->legacy = true;
      spec->solver = spec->stiff ? OdeSpec::BDF : OdeSpec::RK45;
      stamp_ode_defaults(*spec);
      spec->t0 = const_values(e.args[2]).at(0);
      spec->ts = const_values(e.args[3]);
      spec->x_r = const_values(e.args[5]);
      spec->x_i = const_ints(e.args[6]);
      if (e.args.size() >= 10) {
        spec->rtol = const_values(e.args[7]).at(0);
        spec->atol = const_values(e.args[8]).at(0);
        spec->max_steps = (long)const_values(e.args[9]).at(0);
      }
      Val z0 = lower_expr(e.args[1]);
      Val theta = lower_expr(e.args[4]);
      const int64_t S = g.slots[z0.slot].len;
      const int64_t N = (int64_t)spec->ts.size();
      // Compile the right-hand side now that its argument sizes are known.
      // A failure here is not a compile error: the interpreter still runs it.
      spec->args.resize(3);
      spec->args[0].is_param = true;
      spec->args[0].len = (int)g.slots[theta.slot].len;
      spec->args[1].len = (int)spec->x_r.size();
      spec->args[2].is_int = true;
      spec->args[2].ints = spec->x_i;
      spec->prog = compile_rhs(*spec->rhs(), *spec->funs(), (int)S,
                               (int)g.slots[theta.slot].len,
                               (int)spec->x_r.size(), spec->x_i);
      return emit_ode(std::move(spec), z0, theta, N, S,
                      ode_result_view(e, N, S));
    }
    return std::nullopt;
  }

  // ---- statements -----------------------------------------------------------
  CompiledModel::ParamView parameter_view(const mir::Stmt& s, int slot,
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

  void lower_read_param(const mir::Stmt& s) {
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
        con_len = checked_product({n_batch, inner_con},
                                  "parameter constrained shape");
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
      if (M < N)
        fail("cholesky-factor-cov rows are smaller than columns", s.raw);
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
      Val value{raw, scalar_autodiff(), psi};
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
        opcode == OP_CONSTRAIN_UNIT_VECTOR ||
        opcode == OP_CONSTRAIN_SUM_TO_ZERO)
      tr_idata = {checked_immediate(n_batch, "structured parameter batch"),
                  checked_immediate(inner_raw, "structured raw leaf"),
                  checked_immediate(inner_con, "structured constrained leaf")};
    if (opcode == OP_CONSTRAIN_CHOL_CORR ||
        opcode == OP_CONSTRAIN_CORR_MATRIX ||
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
    jac_slots.push_back(jac);
    scope[s.decl_id] = con;
    if (!in_write_array)
      out.views.push_back(parameter_view(s, con.slot, con_len));
  }

  struct DeclView {
    int64_t len = 0;
    bool autodiff = false;
    SlotInfo si;
    bool int_array = false;
  };
  // The only name-keyed declaration protocol. Runtime values carry the same
  // static scalar type and SlotInfo in `scope`; this registry is needed only
  // before first binding.
  std::map<std::string, DeclView> decls;

  void lower_stmt(const mir::Stmt& s) {
    switch (s.kind) {
      case mir::Stmt::Decl:
        if (s.read_transform) {
          lower_read_param(s);
        } else if (s.decl_type.base == "SInt") {
          if (in_write_array && s.has_init && runtime_int_binding(s.init)) {
            Val v = lower_expr(s.init);
            SlotInfo expected = view_of(s.decl_type);
            require_binding(v, 1, expected, s.decl_id, s.raw);
            v.autodiff = false;
            v.si = expected;
            v.si.param_free = false;
            scope[s.decl_id] = v;
            decls[s.decl_id] = DeclView{1, false, expected};
            int_env.erase(s.decl_id);
            int_locals.erase(s.decl_id);
            td.env().erase(s.decl_id);
            return;
          }
          // A fresh scalar-int declaration shadows every representation of
          // an earlier declaration with the same optimized MIR id.  In
          // particular, a preceding runtime sum may have installed a graph
          // value in scope/decls; leaving it there would make a later Var
          // read win over the compile-time literal installed below.
          scope.erase(s.decl_id);
          decls.erase(s.decl_id);
          td.env().erase(s.decl_id);
          int_env.erase(s.decl_id);
          int_locals.erase(s.decl_id);
          // Int locals are always data-only in Stan; keep them in int_env
          // so size expressions and indices resolve at compile time.
          int_locals.insert(s.decl_id);
          // eval_int, not the interpreter directly: the initializer may be
          // a shape query on a slot-bound value (rows(lscale) inside an
          // inlined function), which only eval_int can answer.
          if (s.has_init) int_env[s.decl_id] = eval_int(s.init);
        } else {
          // A redeclaration shadows whatever the name held: --O1 inlining
          // reuses one symbol for a callee's local across loop iterations,
          // and its size can differ per iteration. The stale binding must
          // not constrain the fresh variable's width.
          scope.erase(s.decl_id);
          DeclView sh;
          sh.len = sized_len(s.decl_type);
          sh.autodiff = !s.decl_data_only && scalar_autodiff();
          sh.si = view_of(s.decl_type);
          // CmdStan fills every uninitialized integer container with the
          // INT_MIN sentinel.  Runtime-sum provenance remains deliberately
          // one-dimensional, but the value-level initialization contract is
          // independent of rank.
          sh.int_array =
              s.decl_type.base == "SArray" && s.decl_type.elem_base == "SInt";
          if (s.has_init) {
            Val v = lower_expr(s.init);
            SlotInfo expected = view_of(s.decl_type, v.si.param_free);
            require_binding(v, sh.len, expected, s.decl_id, s.raw);
            v.autodiff = sh.autodiff;
            v.si = expected;
            scope[s.decl_id] = v;
          }
          decls[s.decl_id] = sh;
          if (s.has_init)
            sync_data_local(s.decl_id, s.init, scope.at(s.decl_id));
          else
            td.env().erase(s.decl_id);
        }
        return;
      case mir::Stmt::Assignment: {
        if (s.lhs_idx.empty() && int_locals.count(s.lhs)) {
          if (in_write_array && runtime_int_binding(s.rhs)) {
            Val rhs = lower_expr(s.rhs);
            SlotInfo expected = view_of("UInt");
            require_binding(rhs, 1, expected, s.lhs, s.raw);
            rhs.autodiff = false;
            rhs.si = expected;
            rhs.si.param_free = false;
            scope[s.lhs] = rhs;
            decls[s.lhs] = DeclView{1, false, expected};
            int_env.erase(s.lhs);
            int_locals.erase(s.lhs);
            td.env().erase(s.lhs);
            return;
          }
          int_env[s.lhs] = eval_int(s.rhs);
          return;
        }
        if (!s.lhs_idx.empty()) {
          // Element write under unrolled control flow: functional update.
          Val prev_v{-1, false, {}};
          auto it = scope.find(s.lhs);
          if (it != scope.end()) {
            prev_v = it->second;
          } else {
            auto dl = decls.find(s.lhs);
            if (dl == decls.end())
              fail("indexed assignment to undeclared " + s.lhs);
            SlotInfo si = dl->second.si;
            si.param_free = true;
            prev_v =
                Val{add_slot(dl->second.len, false), dl->second.autodiff, si};
            const double initial =
                dl->second.int_array
                    ? static_cast<double>(std::numeric_limits<int>::min())
                    : 0.0;
            out.fills.emplace_back(
                prev_v.slot, std::vector<double>(dl->second.len, initial));
            if (dl->second.int_array) set_uninitialized_int_array(prev_v);
          }
          const int prev = prev_v.slot;
          bool all_single = true;
          for (const auto& ix : s.lhs_idx)
            if (ix.name != "IndexSingle") all_single = false;
          const std::vector<int64_t>* dd =
              is_array(prev_v.si) ? &array_shape(prev_v.si).dims : nullptr;
          const Val rhs_v = lower_expr(s.rhs);
          const int rhs = rhs_v.slot;
          SlotInfo out_si = prev_v.si;
          // Whole matrix row write M[i] = row_vector: one value per column,
          // strided by the physical row count.
          if (s.lhs_idx.size() == 1 && s.lhs_idx[0].name == "IndexSingle" &&
              is_matrix(prev_v.si) && is_row_vector(rhs_v.si)) {
            const int64_t i = eval_int(s.lhs_idx[0].args[0]) - 1;
            if (i < 0 || i >= prev_v.si.rows)
              fail("row assignment index out of bounds for " + s.lhs);
            if (g.slots[rhs].len != prev_v.si.cols)
              fail("row assignment size mismatch for " + s.lhs);
            Val nv = emit_value(OP_SET_SLICE_STRIDED, {prev_v, rhs_v},
                                g.slots[prev].len, out_si,
                                {(int)i, (int)prev_v.si.rows});
            propagate_int_update(nv, prev_v, rhs_v, i, prev_v.si.rows);
            scope[s.lhs] = nv;
            td.env().erase(s.lhs);
            return;
          }
          // Between write w[a:b] = rhs (contiguous on 1-D values).
          if (s.lhs_idx.size() == 1 && s.lhs_idx[0].name == "IndexBetween") {
            const bool flat_1d_array =
                is_array(prev_v.si) &&
                array_shape(prev_v.si).dims.size() == 1 &&
                array_shape(prev_v.si).leaf == ViewKind::Flat;
            if (!is_vector(prev_v.si) && !is_row_vector(prev_v.si) &&
                !flat_1d_array)
              fail("range assignment needs a one-dimensional flat value for " +
                       s.lhs,
                   s.raw);
            const int64_t lo = eval_int(s.lhs_idx[0].args[0]);
            const int64_t hi = eval_int(s.lhs_idx[0].args[1]);
            const int64_t len = hi >= lo ? hi - lo + 1 : 0;
            check_range(lo, hi, g.slots[prev].len, "range assignment", s.raw);
            if (g.slots[rhs].len != len)
              fail("range assignment size mismatch for " + s.lhs);
            const int64_t start = len == 0 ? 0 : lo - 1;
            Val nv = emit_value(OP_SET_SLICE, {prev_v, rhs_v},
                                g.slots[prev].len, out_si, {(int)start});
            propagate_int_update(nv, prev_v, rhs_v, start, 1);
            scope[s.lhs] = nv;
            td.env().erase(s.lhs);
            return;
          }
          // Column write M[:, j] = rhs (contiguous in col-major storage).
          if (s.lhs_idx.size() == 2 && s.lhs_idx[0].name == "IndexAll" &&
              s.lhs_idx[1].name == "IndexSingle" && is_matrix(prev_v.si)) {
            const int64_t j = eval_int(s.lhs_idx[1].args[0]) - 1;
            if (j < 0 || j >= prev_v.si.cols)
              fail("column assignment index out of bounds for " + s.lhs);
            if (g.slots[rhs].len != prev_v.si.rows)
              fail("column assignment size mismatch for " + s.lhs);
            Val nv =
                emit_value(OP_SET_SLICE, {prev_v, rhs_v}, g.slots[prev].len,
                           out_si, {(int)(j * prev_v.si.rows)});
            propagate_int_update(nv, prev_v, rhs_v, j * prev_v.si.rows, 1);
            scope[s.lhs] = nv;
            td.env().erase(s.lhs);
            return;
          }
          // Row-range column write M[a:b, j] = rhs (contiguous within the
          // column).
          if (s.lhs_idx.size() == 2 && s.lhs_idx[0].name == "IndexBetween" &&
              s.lhs_idx[1].name == "IndexSingle" && is_matrix(prev_v.si)) {
            const int64_t lo = eval_int(s.lhs_idx[0].args[0]);
            const int64_t hi = eval_int(s.lhs_idx[0].args[1]);
            const int64_t j = eval_int(s.lhs_idx[1].args[0]) - 1;
            if (j < 0 || j >= prev_v.si.cols)
              fail("column assignment index out of bounds for " + s.lhs);
            const int64_t len = hi >= lo ? hi - lo + 1 : 0;
            check_range(lo, hi, prev_v.si.rows, "row-range assignment", s.raw);
            if (g.slots[rhs].len != len)
              fail("range assignment size mismatch for " + s.lhs);
            const int64_t start = len == 0 ? 0 : j * prev_v.si.rows + lo - 1;
            Val nv = emit_value(OP_SET_SLICE, {prev_v, rhs_v},
                                g.slots[prev].len, out_si, {(int)start});
            propagate_int_update(nv, prev_v, rhs_v, start, 1);
            scope[s.lhs] = nv;
            td.env().erase(s.lhs);
            return;
          }
          if (all_single && dd && s.lhs_idx.size() <= dd->size() &&
              !is_matrix(prev_v.si)) {
            // The mirror of the read path, through the same flat_addr.
            const auto& D = *dd;
            const bool mat = array_shape(prev_v.si).leaf == ViewKind::Matrix;
            std::vector<int64_t> ix;
            for (const auto& k : s.lhs_idx)
              ix.push_back(eval_int(k.args[0]) - 1);
            const Addr a = flat_addr(D, mat, ix);
            if (a.len != g.slots[rhs].len && a.len != 1)
              fail("indexed assignment size mismatch for " + s.lhs);
            Val nv =
                a.stride != 1
                    ? emit_value(OP_SET_SLICE_STRIDED, {prev_v, rhs_v},
                                 g.slots[prev].len, out_si,
                                 {(int)a.off, (int)a.stride})
                    : (a.len == 1
                           ? emit_value(OP_SET_INDEX, {prev_v, rhs_v},
                                        g.slots[prev].len, out_si, {(int)a.off})
                           : emit_value(OP_SET_SLICE, {prev_v, rhs_v},
                                        g.slots[prev].len, out_si,
                                        {(int)a.off}));
            propagate_int_update(nv, prev_v, rhs_v, a.off, a.stride);
            scope[s.lhs] = nv;
            td.env().erase(s.lhs);
            return;
          }
          int64_t flat = 0;
          if (all_single && s.lhs_idx.size() == 1) {
            flat = eval_int(s.lhs_idx[0].args[0]) - 1;
          } else if (all_single && s.lhs_idx.size() == 2 &&
                     is_matrix(prev_v.si)) {
            flat = (eval_int(s.lhs_idx[1].args[0]) - 1) * prev_v.si.rows +
                   (eval_int(s.lhs_idx[0].args[0]) - 1);
          } else {
            std::string desc = "unsupported indexed assignment: lhs=" + s.lhs;
            for (const auto& ix : s.lhs_idx)
              desc += " [" + (ix.name.empty() ? "?" : ix.name) + "]";
            fail(desc, s.raw);
          }
          Val nv = emit_value(OP_SET_INDEX, {prev_v, rhs_v}, g.slots[prev].len,
                              out_si, {(int)flat});
          propagate_int_update(nv, prev_v, rhs_v, flat, 1);
          scope[s.lhs] = nv;
          td.env().erase(s.lhs);
          return;
        }
        {
          Val rhs = lower_expr(s.rhs);
          auto old = scope.find(s.lhs);
          if (old != scope.end()) {
            require_binding(rhs, g.slots[old->second.slot].len, old->second.si,
                            s.lhs, s.raw);
            const bool param_free = rhs.si.param_free;
            rhs.autodiff = old->second.autodiff;
            rhs.si = old->second.si;
            rhs.si.param_free = param_free;
          } else {
            auto dl = decls.find(s.lhs);
            if (dl != decls.end()) {
              if (dl->second.len == 0 && g.slots[rhs.slot].len != 0) {
                // stanc3's --O1 inliner declares a function's return
                // variable zero-length (`array[real, 0]`, `vector[0]`)
                // because the returned size is the callee's business, and
                // C++ assignment resizes. Slots do not, so the first
                // whole-variable assignment defines the shape instead.
                dl->second.len = g.slots[rhs.slot].len;
                dl->second.si = rhs.si;
              } else {
                SlotInfo expected = dl->second.si;
                require_binding(rhs, dl->second.len, expected, s.lhs, s.raw);
                const bool pf = rhs.si.param_free;
                rhs.si = expected;
                rhs.si.param_free = pf;
              }
              rhs.autodiff = dl->second.autodiff;
            }
          }
          scope[s.lhs] = rhs;
          sync_data_local(s.lhs, s.rhs, rhs);
        }
        return;
      }
      case mir::Stmt::TargetPE: {
        // Stan defines `target += e` for a container `e` as adding `sum(e)`
        // -- CmdStan's `lp_accum__.add(e)` reduces the whole container. A
        // target term is consumed as a scalar, so the reduction has to
        // happen here; pushing the container's slot would silently
        // contribute element zero alone.
        Val t = lower_expr(s.target);
        if (g.slots[t.slot].len != 1) t = emit_value(OP_SUM_VEC, {t}, 1);
        target_terms.push_back(t.slot);
        return;
      }
      case mir::Stmt::Block:
      case mir::Stmt::SList:
        for (const auto& k : s.body) lower_stmt(k);
        return;
      case mir::Stmt::Skip:
        return;
      case mir::Stmt::NRFunApp:
        if (s.fn_name == "FnCheck") {
          // prepare_data checks already ran in bind_data. Any check reaching
          // this lowering belongs to log_prob/write_array and must retain its
          // per-evaluation position, even when its value is parameter-free.
          if (!s.check_transform) fail("malformed FnCheck", s.raw);
          if (mir::is_structured_check(s.check_transform->kind)) {
            if (!s.check_transform->args.empty() || s.fn_args.size() != 1)
              fail("malformed structured FnCheck", s.raw);
            const Val value = lower_expr(s.fn_args[0]);
            const int64_t value_len = g.slots[value.slot].len;
            validate_view(value.si, value_len, "structured FnCheck value");

            auto spec = std::make_shared<StructuredCheckSpec>();
            spec->kind = s.check_transform->kind;
            spec->name =
                s.check_var_name.empty() ? s.fn_args[0].name : s.check_var_name;
            if (is_array(value.si)) {
              const ArrayShape& shape = array_shape(value.si);
              spec->dims = shape.dims;
              if (shape.leaf == ViewKind::Vector)
                spec->leaf = StructuredLeaf::Vector;
              else if (shape.leaf == ViewKind::Matrix)
                spec->leaf = StructuredLeaf::Matrix;
              else
                fail("structured FnCheck requires vector or matrix leaves",
                     s.raw);
            } else if (is_vector(value.si)) {
              spec->dims = {value_len};
              spec->leaf = StructuredLeaf::Vector;
            } else if (is_matrix(value.si)) {
              spec->dims = {value.si.rows, value.si.cols};
              spec->leaf = StructuredLeaf::Matrix;
            } else {
              fail("structured FnCheck requires a vector or matrix", s.raw);
            }

            const size_t leaf_rank =
                spec->leaf == StructuredLeaf::Matrix ? 2 : 1;
            const mir::UnsizedLeaf expr_leaf = s.fn_args[0].unsized.leaf;
            if (s.fn_args[0].unsized.depth != spec->dims.size() - leaf_rank ||
                (spec->leaf == StructuredLeaf::Vector &&
                 expr_leaf != mir::UnsizedLeaf::Vector) ||
                (spec->leaf == StructuredLeaf::Matrix &&
                 expr_leaf != mir::UnsizedLeaf::Matrix))
              fail("structured FnCheck type does not match its value", s.raw);
            const bool matrix_only =
                spec->kind == mir::Transform::CholeskyCorr ||
                spec->kind == mir::Transform::Correlation ||
                spec->kind == mir::Transform::Covariance ||
                spec->kind == mir::Transform::CholeskyCov;
            const bool vector_only =
                spec->kind != mir::Transform::SumToZero && !matrix_only;
            if ((matrix_only && spec->leaf != StructuredLeaf::Matrix) ||
                (vector_only && spec->leaf != StructuredLeaf::Vector))
              fail("structured FnCheck transform and leaf disagree", s.raw);

            (void)emit_value(OP_CHECK_STRUCTURED, {value}, 1);
            g.ops.back().udata = spec.get();
            g.udata_pool.push_back(std::move(spec));
            return;
          }
          if (s.check_transform->args.size() != 1 || s.fn_args.size() != 2)
            fail("malformed FnCheck", s.raw);
          const uint16_t opcode =
              s.check_transform->kind == mir::Transform::Lower ? OP_CHECK_LOWER
              : s.check_transform->kind == mir::Transform::Upper
                  ? OP_CHECK_UPPER
                  : 0;
          if (opcode == 0) fail("unsupported FnCheck transform", s.raw);

          const Val value = lower_expr(s.fn_args[0]);
          const Val bound = lower_expr(s.fn_args[1]);
          const int64_t value_len = g.slots[value.slot].len;
          const int64_t bound_len = g.slots[bound.slot].len;
          validate_view(value.si, value_len, "FnCheck value");
          validate_view(bound.si, bound_len, "FnCheck bound");
          const bool bound_is_scalar = is_scalar(bound);
          const bool shapes_match =
              is_scalar(value)
                  ? bound_is_scalar
                  : (bound_is_scalar ||
                     same_view(value.si, value_len, bound.si, bound_len));

          auto spec = std::make_shared<BoundCheckSpec>();
          spec->name =
              s.check_var_name.empty() ? s.fn_args[0].name : s.check_var_name;
          spec->bound_is_scalar = bound_is_scalar;
          spec->shapes_match = shapes_match;
          (void)emit_value(opcode, {value, bound}, 1);
          g.ops.back().udata = spec.get();
          g.udata_pool.push_back(std::move(spec));
          return;
        }
        // Size validation remains a separate compatibility seam.
        if (s.fn_name == "FnValidateSize") return;
        if (s.fn_name == "check_matching_dims") {
          if (s.fn_args.size() != 5 || s.fn_args[0].kind != mir::Expr::LitStr ||
              s.fn_args[1].kind != mir::Expr::LitStr ||
              s.fn_args[3].kind != mir::Expr::LitStr)
            fail("malformed check_matching_dims", s.raw);
          const Val value = lower_expr(s.fn_args[2]);
          const Val bound = lower_expr(s.fn_args[4]);
          const int64_t value_len = g.slots[value.slot].len;
          const int64_t bound_len = g.slots[bound.slot].len;
          validate_view(value.si, value_len, "check_matching_dims value");
          validate_view(bound.si, bound_len, "check_matching_dims bound");
          auto spec = std::make_shared<BoundCheckSpec>();
          spec->name = s.fn_args[1].lit_s;
          spec->shapes_match =
              same_view(value.si, value_len, bound.si, bound_len);
          (void)emit_value(OP_CHECK_MATCHING_DIMS, {value, bound}, 1);
          g.ops.back().udata = spec.get();
          g.udata_pool.push_back(std::move(spec));
          return;
        }
        // Deliberately not a `check_*` prefix match: a value check like
        // check_positive_finite rejects a draw at runtime, and skipping one
        // would silently accept points CmdStan refuses.
        // reject() and print(): the message is a mix of string literals
        // and expressions, so the literals become the op's chunk list and
        // the expressions become its inputs. reject throws
        // std::domain_error at forward time, which is the same exception
        // from the same place CmdStan's generated code throws it, so the
        // sampler counts it as a rejected proposal rather than a failure.
        if (s.fn_name == "FnReject" || s.fn_name == "FnPrint") {
          auto spec = std::make_shared<MessageSpec>();
          std::vector<int> ins;
          std::string pending;
          for (const auto& a : s.fn_args) {
            if (a.kind == mir::Expr::LitStr) {
              pending += a.lit_s;
              continue;
            }
            // Each value input closes the chunk that precedes it. Op::in
            // holds six, and a message longer than that is a diagnostic
            // nobody will miss the tail of -- but say so rather than
            // corrupting the op.
            if (ins.size() >= 6)
              fail(std::string(s.fn_name == "FnReject" ? "reject" : "print") +
                       " with more than 6 printed values",
                   s.raw);
            spec->chunks.push_back(pending);
            pending.clear();
            ins.push_back(lower_expr(a).slot);
          }
          spec->chunks.push_back(pending);  // trailing literal, if any
          Op op;
          op.opcode = s.fn_name == "FnReject" ? OP_REJECT : OP_PRINT;
          op.n_in = (int)ins.size();
          for (size_t k = 0; k < ins.size(); ++k) op.in[k] = ins[k];
          // The output is a dead scalar: every op writes somewhere, and
          // nothing reads this one.
          op.out = add_slot(1, false);
          op.udata = spec.get();
          g.udata_pool.push_back(spec);
          g.ops.push_back(op);
          return;
        }
        if (s.fn_name == "FnWriteParam") {
          // One CSV column, at the point the emission happens: this is what
          // fixes the column order to CmdStan's. Arrays of containers are
          // emitted one element at a time -- `array[K] simplex[K] theta`
          // arrives as K writes of `theta[k]` -- and CmdStan names those
          // columns outer-index-first, theta.1.1 .. theta.1.K, theta.2.1 ...
          // so the index path becomes part of the column name.
          if (s.fn_args.size() != 1) fail("FnWriteParam arity", s.raw);
          std::vector<long> ixs;
          const mir::Expr* base = &s.fn_args[0];
          while (base->kind == mir::Expr::Indexed) {
            for (size_t k = base->args.size(); k-- > 1;) {
              if (base->args[k].name != "IndexSingle")
                fail("FnWriteParam under a non-scalar index", s.raw);
              ixs.push_back(eval_int(base->args[k].args[0]));
            }
            base = &base->args[0];
          }
          if (base->kind != mir::Expr::Var)
            fail("FnWriteParam of a non-variable", s.raw);
          std::string name = base->name;
          for (auto it = ixs.rbegin(); it != ixs.rend(); ++it)
            name += "." + std::to_string(*it);
          const Val v = lower_expr(s.fn_args[0]);
          // stanc peels the array dimensions, so what is left here is a
          // scalar, a vector/row_vector, or a matrix -- and its type decides
          // how CmdStan indexes the columns.
          using Naming = CompiledModel::ParamView::Naming;
          const std::string& t = s.fn_args[0].type_;
          CompiledModel::ParamView pv{name, v.slot, g.slots[v.slot].len};
          if (t == "UReal" || t == "UInt" || t == "UComplex") {
            pv.naming = Naming::Scalar;
          } else if (t == "UMatrix") {
            if (!is_matrix(v.si))
              fail("FnWriteParam of a matrix with unknown shape: " + name,
                   s.raw);
            pv.rows = v.si.rows;
            pv.naming = Naming::Matrix;
          } else {
            pv.naming = Naming::Container;
          }
          out.views.push_back(pv);
          return;
        }
        fail("unsupported statement function " + s.fn_name);
      case mir::Stmt::For: {
        const long lo = eval_int(s.lower), hi = eval_int(s.upper);
        for (long v = lo; v <= hi; ++v) {
          int_env[s.loopvar] = v;
          for (const auto& k : s.body) lower_stmt(k);
        }
        int_env.erase(s.loopvar);
        return;
      }
      case mir::Stmt::IfElse: {
        // The guards are data-only and fold away below (both flags are
        // pinned on), so this is the only chance to note that a CSV
        // section ended here.
        if (in_write_array) {
          switch (mir::emit_guard(s)) {
            case mir::EmitGuard::TransformedParams:
              if (!n_tp_start) n_tp_start = out.views.size();
              break;
            case mir::EmitGuard::GeneratedQuantities:
              if (!n_gq_start) n_gq_start = out.views.size();
              break;
            case mir::EmitGuard::None:
              break;
          }
        }
        bool known = false, c = false;
        if (auto evaluated = try_eval_pure(s.cond)) {
          c = evaluated->r.at(0) != 0.0;
          known = true;
        }
        if (known) {
          if (c && !s.body.empty()) lower_stmt(s.body[0]);
          if (!c && s.body.size() > 1) lower_stmt(s.body[1]);
          return;
        }
        if (s.cond.data_only)
          fail("data-only condition is unavailable in the lexical frame",
               s.raw);
        lower_param_ifelse(s);
        return;
      }
      case mir::Stmt::Return:
        // Only reachable inside an inlined UDF body (log_prob itself has no
        // value returns); unwinds to lower_call_udf.
        if (!s.has_init) fail("void return unsupported in UDF inlining");
        throw LpReturn{lower_expr(s.rhs)};
      default:
        fail("unsupported statement", s.raw);
    }
  }

  // Scalar terms reduce through chained ADD_N ops (6-input limit per op).
  int reduce_terms(std::vector<int> terms) {
    // The target is a scalar, and every consumer of a term reads one value
    // from it. A container term is therefore not a shape to accommodate but
    // a lowering bug -- one whose symptom, before this check, was a model
    // that sampled a wrong posterior without saying anything.
    for (int t : terms)
      if (g.slots[t].len != 1) fail("target term is not a scalar");
    if (terms.empty()) return const_slot(0.0);
    while (terms.size() > 1) {
      std::vector<int> next;
      for (size_t i = 0; i < terms.size(); i += 6) {
        const size_t n = std::min<size_t>(6, terms.size() - i);
        if (n == 1) {
          next.push_back(terms[i]);
          continue;
        }
        std::vector<int> chunk(terms.begin() + i, terms.begin() + i + n);
        next.push_back(emit_raw(OP_ADD_N, chunk, 1, {}).slot);
      }
      terms = std::move(next);
    }
    return terms[0];
  }

  // The write_array graph: same unconstrained draw in, every CSV column out.
  // Forward-only, so no target, no jacobian, no adjoints -- but the same
  // lowering, and the same passes, because generated quantities are unrolled
  // over the data exactly like the model block is.
  CompiledModel::WriteArray run_write_array(const mir::Program& p) {
    const auto total_time = prep.start();
    for (const auto& f : p.fun_defs) fun_defs[f.name] = &f;
    in_write_array = true;
    // stanc3 guards the two emission groups on these flags; the sampler wants
    // both, so pin them and let the data-only IfElse fold them away.
    int_env["emit_transformed_parameters__"] = 1;
    int_env["emit_generated_quantities__"] = 1;
    CompiledModel::WriteArray wa;
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
    prep.graph(prep_graph, "lower", lower_time, g, out.fills,
               target_terms.size(), out.views.size(),
               PrepTrace::Extra::Truncated, !wa.truncated.empty());
    const auto inplace_time = prep.start();
    const int inplace = make_inplace_updates(g, roots);
    prep.graph(prep_graph, "inplace", inplace_time, g, out.fills,
               target_terms.size(), out.views.size(),
               PrepTrace::Extra::Rewrites, inplace);
    const auto forward_time = prep.start();
    const int forwarded = forward_stores_to_loads(g, roots);
    prep.graph(prep_graph, "store_forward", forward_time, g, out.fills,
               target_terms.size(), out.views.size(), PrepTrace::Extra::Removed,
               forwarded);
    const auto reroll_time = prep.start();
    const RerollStats rerolled = reroll(g, out.fills, target_terms, roots);
    prep.graph(prep_graph, "reroll", reroll_time, g, out.fills,
               target_terms.size(), out.views.size(), PrepTrace::Extra::Reroll,
               rerolled.regions, rerolled.list_steps, false, 0,
               rerolled.candidate_steps, rerolled.row_steps);
    // Re-roll can replace many element writes with copying slice stores.
    // Give those new ops the same last-use proof as the scalar stores.
    const auto post_reroll_inplace_time = prep.start();
    const int post_reroll_inplace =
        rerolled.regions ? make_inplace_updates(g, roots) : 0;
    prep.graph(prep_graph, "post_reroll_inplace", post_reroll_inplace_time, g,
               out.fills, target_terms.size(), out.views.size(),
               PrepTrace::Extra::Rewrites, post_reroll_inplace);
    const auto finalize_time = prep.start();
    // Nothing reads a result here, but forward() asserts a scalar result
    // slot, so point it at one.
    g.result_slot = const_slot(0.0);
    wa.n_unconstrained = out.n_unconstrained;
    prep.graph(prep_graph, "finalize", finalize_time, g, out.fills,
               target_terms.size(), out.views.size());
    prep.graph(prep_graph, "total", total_time, g, out.fills,
               target_terms.size(), out.views.size(), PrepTrace::Extra::None, 0,
               0, true, out.n_unconstrained);
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

  CompiledModel run(const mir::Program& p) {
    const auto total_time = prep.start();
    for (const auto& f : p.fun_defs) fun_defs[f.name] = &f;
    const auto bind_time = prep.start();
    bind_data(p);
    prep.graph(prep_graph, "bind_data", bind_time, g, out.fills,
               target_terms.size(), out.views.size());
    const auto lower_time = prep.start();
    for (const auto& s : p.log_prob) lower_stmt(s);
    prep.graph(prep_graph, "lower", lower_time, g, out.fills,
               target_terms.size(), out.views.size());
    // Jacobian terms and constrained-parameter views are read straight out
    // of the arena, so no op consumes them and the pass cannot infer them.
    std::vector<int> roots = jac_slots;
    for (const auto& v : out.views) roots.push_back(v.slot);
    // Target terms have no consuming op yet either: reduce_terms builds
    // their ADD_N tree below, after the passes have run.
    std::vector<int> update_roots = roots;
    update_roots.insert(update_roots.end(), target_terms.begin(),
                        target_terms.end());
    const auto inplace_time = prep.start();
    const int inplace =
        make_inplace_updates(g, update_roots);  // off under STANLI_NO_INPLACE
    prep.graph(prep_graph, "inplace", inplace_time, g, out.fills,
               target_terms.size(), out.views.size(),
               PrepTrace::Extra::Rewrites, inplace);
    // Deleting the write/read-back pairs first is what leaves a plain
    // arithmetic lane for reroll to vectorize.
    const auto forward_time = prep.start();
    const int forwarded = forward_stores_to_loads(g, update_roots);
    prep.graph(prep_graph, "store_forward", forward_time, g, out.fills,
               target_terms.size(), out.views.size(), PrepTrace::Extra::Removed,
               forwarded);
    // After the update chains collapse, so a data-only chain is one slot
    // rather than N; before reroll, so the lanes it sees have data operands.
    const auto constfold_time = prep.start();
    const ConstFoldStats constfolded = const_fold(g, out.fills, update_roots);
    prep.graph(prep_graph, "constfold", constfold_time, g, out.fills,
               target_terms.size(), out.views.size(),
               PrepTrace::Extra::ConstFold, constfolded.ops_removed,
               constfolded.slots_folded);
    const auto reroll_time = prep.start();
    const RerollStats rerolled =
        reroll(g, out.fills, target_terms, roots);  // STANLI_NO_REROLL
    prep.graph(prep_graph, "reroll", reroll_time, g, out.fills,
               target_terms.size(), out.views.size(), PrepTrace::Extra::Reroll,
               rerolled.regions, rerolled.list_steps, false, 0,
               rerolled.candidate_steps, rerolled.row_steps);
    // Target terms may have been replaced by vector reductions, so rebuild
    // the implicit-root set before considering the slice stores reroll made.
    std::vector<int> post_reroll_roots = roots;
    post_reroll_roots.insert(post_reroll_roots.end(), target_terms.begin(),
                             target_terms.end());
    const auto post_reroll_inplace_time = prep.start();
    const int post_reroll_inplace =
        rerolled.regions ? make_inplace_updates(g, post_reroll_roots) : 0;
    prep.graph(prep_graph, "post_reroll_inplace", post_reroll_inplace_time, g,
               out.fills, target_terms.size(), out.views.size(),
               PrepTrace::Extra::Rewrites, post_reroll_inplace);
    // LAST, after every other pass has had first crack: compile whatever
    // scalar residue survives (recurrences the re-roll can never widen)
    // into island ops. Off under STANLI_NO_ISLAND.
    const auto island_time = prep.start();
    const int islands = carve_islands(g, out.fills, target_terms, roots);
    prep.graph(prep_graph, "island", island_time, g, out.fills,
               target_terms.size(), out.views.size(), PrepTrace::Extra::Regions,
               islands);
    const auto reduce_time = prep.start();
    std::vector<int> all = target_terms;
    all.insert(all.end(), jac_slots.begin(), jac_slots.end());
    g.result_slot = reduce_terms(all);
    prep.graph(prep_graph, "reduce", reduce_time, g, out.fills,
               target_terms.size(), out.views.size());
    prep.graph(prep_graph, "total", total_time, g, out.fills,
               target_terms.size(), out.views.size(), PrepTrace::Extra::None, 0,
               0, true, out.n_unconstrained);
    out.graph = std::move(g);
    return std::move(out);
  }
};

}  // namespace

CompiledModel compile_model(const std::string& tmir_text, const DataMap& data) {
  const char* prep_env = std::getenv("STANLI_PROFILE_PREP");
  PrepTrace prep(prep_env && prep_env[0] != '0');
  const auto compile_time = prep.start();
  // Shared because the interpreted write_array fallback, when needed,
  // keeps the generate_quantities statements and UDF bodies alive for the
  // model's whole life.
  const auto parse_time = prep.start();
  auto prog =
      std::make_shared<mir::Program>(mir::read_program(sexp::parse(tmir_text)));
  prep.plain("compile", "parse_mir", parse_time, PrepTrace::Extra::MirBytes,
             static_cast<int64_t>(tmir_text.size()));
  Lowering lo(data, prep, "log_prob");
  CompiledModel cm = lo.run(*prog);
  if (!prog->generate_quantities.empty()) {
    // A second lowering, over the transformed data the first one already
    // interpreted: re-running prepare_data would double preparation time on
    // the models where preparation is the cost (nn_rbm1bJ100, 20.7 s).
    Lowering wa(data, prep, "write_array", lo.shape_pool);
    const auto env_copy_time = prep.start();
    wa.td.env() = lo.td.env();
    wa.int_env = lo.int_env_data;
    // bind_data owns immutable declaration shape and physical-layout facts;
    // write_array skips that expensive pass, so its fresh lexical lowering
    // receives the facts together with the already-prepared environment.
    wa.decls = lo.decls;
    prep.plain("write_array", "env_copy", env_copy_time);
    CompiledModel::WriteArray w = wa.run_write_array(*prog);
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
      auto env = lo.td.env();
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
  }
  prep.plain("compile", "total", compile_time);
  prep.report();
  return cm;
}

}  // namespace stanli
