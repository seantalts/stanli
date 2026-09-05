#ifndef STANLI_LOWER_INTERNAL_HPP
#define STANLI_LOWER_INTERNAL_HPP

#include <stanli/algebra.hpp>
#include <stanli/callable_transform.hpp>
#include <stanli/compile.hpp>
#include <stanli/unconstrain.hpp>
#include <stanli/constfold.hpp>
#include <stanli/cse.hpp>
#include <stanli/dae.hpp>
#include <stanli/density_registry.hpp>
#include <stanli/function_registry.hpp>
#include <stanli/function_view_shape.hpp>
#include <stanli/higher_order_eval.hpp>
#include <stanli/expression_layout.hpp>
#include <stanli/inplace.hpp>
#include <stanli/mir_message.hpp>
#include <stanli/mir_prog.hpp>
#include <stanli/mir.hpp>
#include <stanli/mir_decode.hpp>
#include <stanli/mir_interp.hpp>
#include <stanli/ode.hpp>
#include <stanli/ode_adjoint.hpp>
#include <stanli/optable.hpp>
#include <stanli/island.hpp>
#include <stanli/structured_loop.hpp>
#include <stanli/partition.hpp>
#include <stanli/quadrature.hpp>
#include <stanli/reroll.hpp>
#include <stanli/builtin_registry.hpp>
#include <stanli/structured_check.hpp>
#include <stanli/wa_interp.hpp>

#include "reroll_profile.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <chrono>
#include <functional>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace stanli {
namespace lower_detail {

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
    Partition,
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
    int64_t packed_rows = 0;
    int64_t term_density = 0;
    int64_t element_density = 0;
    int64_t term_widen = 0;
    int64_t element_store = 0;
    bool deep = false;
    int64_t params = 0;
    int64_t slot_elems = 0;
    int64_t fill_elems = 0;
    int64_t idata_arrays = 0;
    int64_t idata_elems = 0;
    int64_t udata = 0;
  };

  explicit PrepTrace(bool enabled) : enabled_(enabled) {}

  bool enabled() const { return enabled_; }

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
             int64_t params = 0, int64_t c = 0, int64_t d = 0,
             const detail::RerollDispositionStats* dispositions = nullptr) {
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
    if (dispositions) {
      r.packed_rows = dispositions->packed_rows;
      r.term_density = dispositions->term_density;
      r.element_density = dispositions->element_density;
      r.term_widen = dispositions->term_widen;
      r.element_store = dispositions->element_store;
    }
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
          field("packed_rows", r.packed_rows);
          field("term_density", r.term_density);
          field("element_density", r.element_density);
          field("term_widen", r.term_widen);
          field("element_store", r.element_store);
          break;
        case Extra::Partition:
          field("groups", r.a);
          field("lanes", r.b);
          field("declined", r.c);
          field("list_steps", r.d);
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

enum class StructuredMode { Off, Auto, Prefer, Force };

StructuredMode read_structured_mode();

struct RealRange {
  double lo = 0;
  double hi = 0;
  double positive_lo = std::numeric_limits<double>::infinity();

  RealRange() = default;
  RealRange(double lower, double upper)
      : lo(lower),
        hi(upper),
        positive_lo(lower > 0 ? lower
                              : std::numeric_limits<double>::infinity()) {}
};

inline bool is_matrix(const SlotInfo& si) {
  return si.kind == ViewKind::Matrix;
}
inline bool is_vector(const SlotInfo& si) {
  return si.kind == ViewKind::Vector;
}
inline bool is_row_vector(const SlotInfo& si) {
  return si.kind == ViewKind::RowVector;
}
inline bool is_array(const SlotInfo& si) { return si.kind == ViewKind::Array; }

inline int leaf_rank(ViewKind kind) {
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
inline Addr flat_addr(const std::vector<int64_t>& D, bool mat,
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

inline std::vector<double> graph_order(const DataMap::Entry& en,
                                       bool standalone_matrix,
                                       bool innermost_matrix) {
  if (standalone_matrix || en.dims.size() <= 1) return en.r;
  const size_t outer_rank = en.dims.size() - (innermost_matrix ? 2u : 0u);
  return graph_container_order(en.r, en.dims, outer_rank);
}

struct Lowering {
  struct Val {
    int slot;
    bool autodiff = false;  // instantiated C++ scalar type carries var
    SlotInfo si;
    ExpressionLayout layout;
    // Logical extents that are graph values rather than preparation-time
    // constants. Entries align with the logical view; -1 keeps that axis at
    // its capacity extent. Storage remains fixed and preallocated.
    std::vector<int> runtime_dims;

    Val() = default;
    Val(int slot_in, bool autodiff_in, SlotInfo si_in,
        ExpressionLayout layout_in = ExpressionLayout::scalar(),
        std::vector<int> runtime_dims_in = {})
        : slot(slot_in),
          autodiff(autodiff_in),
          si(std::move(si_in)),
          layout(std::move(layout_in)),
          runtime_dims(std::move(runtime_dims_in)) {}
    Val(int slot_in, bool autodiff_in, SlotInfo si_in,
        std::vector<int> runtime_dims_in)
        : Val(slot_in, autodiff_in, std::move(si_in),
              ExpressionLayout::scalar(), std::move(runtime_dims_in)) {}
  };
  // A function argument starts as a MIR expression and becomes a graph value
  // only when a consumer asks for it.  Keeping the source expression here is
  // important: different function families need different representations,
  // and constructing this wrapper must not change evaluation order or cause
  // effects from an argument that the consumer never reads.
  struct LoweredArgument {
    struct Cache {
      std::optional<DataMap::Entry> owned_pure_value;
      // Observations live in Lowering::observations, whose std::map entries
      // remain stable while a call is lowered. Do not duplicate their
      // vector-owned payload merely to memoize the lookup here.
      const DataMap::Entry* borrowed_pure_value = nullptr;
      std::optional<std::vector<int>> converted_ints;
      std::optional<long> constant_int;
    };

    Lowering* owner = nullptr;
    const mir::Expr* source = nullptr;
    std::optional<Val> lowered;
    // Most arguments are only lowered to a Val. Keep the less common,
    // potentially vector-owning compile-time caches off the inline argument
    // record so ordinary calls neither construct them nor make the call frame
    // large enough to offset CallArguments' inline storage win.
    std::unique_ptr<Cache> cached;
    bool pure_checked = false;

    Cache& cache() {
      if (!cached) cached = std::make_unique<Cache>();
      return *cached;
    }

    const DataMap::Entry* pure_value() const {
      if (!cached) return nullptr;
      if (cached->borrowed_pure_value) return cached->borrowed_pure_value;
      return cached->owned_pure_value ? &*cached->owned_pure_value : nullptr;
    }

    void borrow_pure_value(const DataMap::Entry& value) {
      Cache& c = cache();
      c.owned_pure_value.reset();
      c.borrowed_pure_value = &value;
    }

    void own_pure_value(DataMap::Entry value) {
      Cache& c = cache();
      c.borrowed_pure_value = nullptr;
      c.owned_pure_value = std::move(value);
    }

    const mir::Expr& expr() const { return *source; }

    const Val& value() {
      if (!lowered) lowered = owner->lower_expr(expr());
      return *lowered;
    }

    // Some irregular calls probe a data-only actual before deciding whether
    // they need a graph value. Cache a successful fold in the same slot as
    // value(), so the fallback path cannot lower the actual twice.
    std::optional<Val> try_fold() {
      if (lowered) return std::nullopt;
      auto folded = owner->fold_const(expr());
      if (folded) lowered = *folded;
      return folded;
    }

    const DataMap::Entry* observation() {
      if (!pure_checked) {
        pure_checked = true;
        const Val& v = value();
        if (const DataMap::Entry* en = owner->observation(v)) {
          borrow_pure_value(*en);
        } else if (auto evaluated = owner->try_eval_pure(expr())) {
          owner->observe(v, std::move(*evaluated));
          const DataMap::Entry* en = owner->observation(v);
          assert(en != nullptr);
          borrow_pure_value(*en);
        }
      }
      return pure_value();
    }

    const std::vector<double>& require_constant_reals(const char* role) {
      if (!pure_value()) {
        if (owner->expr_effectful(expr()))
          owner->fail("effectful expression cannot be demanded at compile time",
                      expr().raw);
        if (!pure_checked) {
          pure_checked = true;
          if (auto evaluated = owner->try_eval_pure(expr()))
            own_pure_value(std::move(*evaluated));
        }
        if (!pure_value()) {
          const Val& v = value();
          if (const DataMap::Entry* en = owner->observation(v)) {
            borrow_pure_value(*en);
          } else if (owner->g.slots[v.slot].len == 0) {
            own_pure_value(DataMap::Entry{});
          } else {
            owner->fail(std::string(role) + " must be known at compile time",
                        expr().raw);
          }
        }
      }
      return pure_value()->r;
    }

    const std::vector<int>& require_constant_ints(const char* role) {
      if (!pure_value()) {
        if (owner->expr_effectful(expr()))
          owner->fail(
              "effectful expression cannot be demanded as compile-time "
              "integers",
              expr().raw);
        if (!pure_checked) {
          pure_checked = true;
          if (auto evaluated = owner->try_eval_pure(expr()))
            own_pure_value(std::move(*evaluated));
        }
        if (!pure_value()) (void)require_constant_reals(role);
      }
      if (pure_value()->is_int) return pure_value()->i;
      Cache& c = cache();
      if (!c.converted_ints) {
        c.converted_ints.emplace();
        c.converted_ints->reserve(pure_value()->r.size());
        for (double d : pure_value()->r)
          c.converted_ints->push_back(static_cast<int>(d));
      }
      return *c.converted_ints;
    }

    long require_constant_int(const char* role) {
      if (expr().data_only && expr().type_ == "UInt") {
        Cache& c = cache();
        if (!c.constant_int) c.constant_int = owner->eval_int(expr());
        return *c.constant_int;
      }
      const std::vector<int>& values = require_constant_ints(role);
      if (values.size() != 1)
        owner->fail(std::string(role) + " must be one integer", expr().raw);
      return values[0];
    }
  };
  struct CallArguments {
    // Six is the graph kernel input boundary and covers ordinary Stan and
    // internal calls. UDFs and variadic solver calls can exceed it, so retain
    // an exact-size heap fallback without penalizing the common case.
    static constexpr size_t inline_capacity = 6;

    Lowering* owner;
    const mir::Expr* call;
    std::array<LoweredArgument, inline_capacity> inline_args{};
    std::unique_ptr<LoweredArgument[]> overflow_args;

    CallArguments(Lowering& lowering, const mir::Expr& expression)
        : owner(&lowering), call(&expression) {
      if (size() > inline_capacity)
        overflow_args = std::make_unique<LoweredArgument[]>(size());
      for (size_t i = 0; i < size(); ++i) {
        data()[i].owner = owner;
        data()[i].source = &expression.args[i];
      }
    }

    size_t size() const { return call->args.size(); }

    LoweredArgument* data() {
      return size() <= inline_capacity ? inline_args.data()
                                       : overflow_args.get();
    }

    LoweredArgument& at(size_t i) {
      if (i >= size())
        owner->fail(call->name + ": argument index out of range", call->raw);
      return data()[i];
    }

    void require_arity(size_t n) const {
      if (size() != n)
        owner->fail(call->name + ": expected " + std::to_string(n) +
                        " arguments, got " + std::to_string(size()),
                    call->raw);
    }

    void require_arity(size_t min, size_t max) const {
      if (size() < min || size() > max)
        owner->fail(call->name + ": expected between " + std::to_string(min) +
                        " and " + std::to_string(max) + " arguments, got " +
                        std::to_string(size()),
                    call->raw);
    }

    void require_min_arity(size_t min) const {
      if (size() < min)
        owner->fail(call->name + ": expected at least " + std::to_string(min) +
                        " arguments, got " + std::to_string(size()),
                    call->raw);
    }

    const mir::Expr& call_expr() const { return *call; }
  };
  enum class BuiltinFamily {
    MapRect,
    ReduceSum,
    MultiNormalRng,
    DirichletRng,
    CategoricalRng,
    ScalarRng,
    Density,
    CallableTransform,
    Elementwise,
    AppendArray,
    Matrix,
    Algebra,
    Quadrature,
    Ode,
    Dae,
    ShapeQuery,
  };

  struct BuiltinDispatch {
    BuiltinFamily family = BuiltinFamily::Elementwise;
    const BuiltinSpec* builtin = nullptr;
    const DensitySpec* density = nullptr;
    std::optional<ScalarRng> scalar_rng;

    BuiltinDispatch() = default;
    BuiltinDispatch(BuiltinFamily selected,
                    const BuiltinSpec* builtin_call = nullptr,
                    const DensitySpec* density_call = nullptr,
                    std::optional<ScalarRng> rng = std::nullopt)
        : family(selected),
          builtin(builtin_call),
          density(density_call),
          scalar_rng(rng) {}
  };

  static BuiltinDispatch rng_dispatch(ScalarRng family) {
    return {BuiltinFamily::ScalarRng, nullptr, nullptr, family};
  }

  static bool ends_with(std::string_view name, std::string_view suffix) {
    return name.size() >= suffix.size() &&
           name.substr(name.size() - suffix.size()) == suffix;
  }
  static BuiltinDispatch resolve_builtin(const mir::Expr& e);

  bool has_runtime_shape(const Val& v) const {
    return std::any_of(v.runtime_dims.begin(), v.runtime_dims.end(),
                       [](int slot) { return slot >= 0; });
  }
  int one_runtime_extent(const Val& v, const std::string& what) {
    if (v.runtime_dims.size() != 1 || v.runtime_dims[0] < 0)
      fail(what + ": needs one runtime logical extent");
    return v.runtime_dims[0];
  }
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
               },
               [this](const mir::Expr& e, DataMap::Entry* out) {
                 return evaluate_retained_higher_order(
                     fun_defs, e,
                     [this](const mir::Expr& arg) { return td.eval(arg); },
                     out);
               }}};
  Graph g;
  CompiledModel out;
  std::map<std::string, Val> scope;
  // var -> value and logical view
  std::map<std::string, long> int_env;
  // data int scalars
  std::map<int, IntRange> int_ranges;
  // runtime integral slot provenance
  std::map<int, RealRange> real_ranges;
  // finite runtime scalar bounds
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
  std::set<std::string> int_locals;
  // SInt locals in log_prob (data-only)
  int udf_depth = 0;
  // Names the reduce_sum rewrite binds its lowered slice under, kept
  // distinct so nested calls do not share one.
  int reduce_sum_slices = 0;
  // int_env as bind_data left it, before either section's locals and loop
  // variables were folded in; the write_array lowering starts from this.
  std::map<std::string, long> int_env_data;
  // Lowering generate_quantities rather than log_prob: parameters are columns
  // to emit, not values to differentiate.
  bool in_write_array = false;
  bool write_array_known_static = false;
  // Read once per lowering. The automatic parent path stays legacy until its
  // cheap outer-loop hazard gate fires, so ordinary expressions and loops pay
  // no repeated environment lookup or recursive scan.
  StructuredMode structured_policy = read_structured_mode();
  // Where the emission guards fell, as counts of columns emitted before
  // each. Unset until the guard is reached (a section can be missing from
  // the MIR entirely), which run_write_array then reads as "at the end".
  std::optional<size_t> n_tp_start, n_gq_start;
  // CmdStan's propto__ template parameter, threaded by lower_call_udf: a
  // density inside an inlined user function is unnormalized only if the
  // call that reached it was.
  bool propto_ctx = true;
  // A loop-invariant target-only body is lowered once under the product of
  // its collapsed trip counts. TargetPE consumes the product at the edge,
  // so nested invariant loops still emit one scale rather than a MUL chain.
  double target_scale = 1.0;
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
  bool expression_autodiff(const mir::Expr& e) const;

  explicit Lowering(
      const DataMap& d, PrepTrace& p, const char* graph_name,
      std::shared_ptr<ShapeInterner> pool = std::make_shared<ShapeInterner>());

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
  void forget_observation(const Val& v) { observations.erase({v.slot, v.si}); }
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
  struct StaticRange {
    int64_t lo, hi;
  };
  static bool is_range(const mir::Expr& ix) {
    return ix.name == "IndexBetween" || ix.name == "IndexUpfrom";
  }
  std::optional<StaticRange> static_range(const mir::Expr& ix, int64_t extent) {
    if (ix.name == "IndexBetween")
      return StaticRange{eval_int(ix.args[0]), eval_int(ix.args[1])};
    if (ix.name == "IndexUpfrom")
      return StaticRange{eval_int(ix.args[0]), extent};
    if (ix.name == "IndexAll") return StaticRange{1, extent};
    return std::nullopt;
  }
  // Every index the lowering sees is a bind-time constant, so what CmdStan
  // bounds-checks at runtime is checked here instead.
  std::vector<int64_t> index_positions(const mir::Expr& ix, int64_t extent,
                                       const char* what,
                                       const std::string& raw);

  int const_slot(double v) {
    auto it = const_cache.find(v);
    if (it != const_cache.end()) return it->second;
    const int s = add_slot(1, false);
    out.fills.emplace_back(s, std::vector<double>{v});
    const_cache[v] = s;
    return s;
  }
  Val constant(double v) {
    Val out{const_slot(v), false, SlotInfo{0, 0, true},
            ExpressionLayout::scalar()};
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
  // The fill is exactly the slot's runtime content; CmdStan seeds int
  // locals with INT_MIN the same way.
  void observe_fill(const Val& v, bool int_array, double initial, int64_t len) {
    DataMap::Entry en;
    en.r.assign((size_t)len, initial);
    if (int_array) {
      en.is_int = true;
      en.i.assign((size_t)len, std::numeric_limits<int>::min());
    }
    observe(v, std::move(en));
  }
  // Target models build int arrays in ascending contiguous writes.  Track the
  // initialized prefix in O(1) per immutable slot: overwrites inside it are
  // safe, an adjacent write extends it, and any gap/stride fails closed.  The
  // interval hull may retain overwritten values, conservatively widening the
  // later overflow proof.
  void propagate_int_update(const Val& out_v, const Val& base, const Val& rhs,
                            int64_t start, int64_t stride);

  // The registered scalar shape queries, plus FnLength, the compiler
  // internal that the compile-time evaluators have always answered as size.
  static bool scalar_shape_query(const mir::Expr& e);
  long answer_shape_query(const mir::Expr& e, const SlotInfo& si, int64_t len);

  long eval_int(const mir::Expr& e);

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
                    int64_t* cols = nullptr);

  SlotInfo view_of(const mir::SizedType& t, bool param_free = false);

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
                        const std::string& out_type);

  bool same_view(const SlotInfo& a, int64_t alen, const SlotInfo& b,
                 int64_t blen) const;

  BuiltinArgumentShape view_argument_shape(const SlotInfo& si, int64_t len,
                                           BuiltinArgumentKind kind) const {
    std::vector<int64_t> array_dimensions;
    ViewKind leaf = ViewKind::Flat;
    if (si.kind == ViewKind::Array) {
      const ArrayShape& array = array_shape(si);
      array_dimensions = array.dims;
      leaf = array.leaf;
    }
    return make_view_function_shape(kind, si.kind, leaf,
                                    std::move(array_dimensions), len, si.rows,
                                    si.cols);
  }

  BuiltinArgumentShape builtin_argument_shape(const mir::Expr& source,
                                              const Val& value) const {
    return view_argument_shape(value.si, g.slots[value.slot].len,
                               source.unsized.leaf == mir::UnsizedLeaf::Int
                                   ? BuiltinArgumentKind::Integer
                                   : BuiltinArgumentKind::Real);
  }

  BuiltinLayout resolved_builtin_layout(const mir::Expr& e,
                                        const BuiltinSpec& spec,
                                        const std::vector<Val>& values) {
    std::vector<BuiltinArgumentShape> shapes;
    shapes.reserve(values.size());
    try {
      for (size_t k = 0; k < values.size(); ++k)
        shapes.push_back(builtin_argument_shape(e.args[k], values[k]));
      return builtin_layout(spec, shapes);
    } catch (const std::invalid_argument& error) {
      fail(e.name + ": " + error.what(), e.raw);
    }
  }

  bool is_scalar(const Val& v) const {
    return v.si.kind == ViewKind::Flat && v.si.shape == 0 &&
           g.slots[v.slot].len == 1;
  }
  static ExpressionLayout owning_layout(const SlotInfo& si) {
    return si.kind == ViewKind::Flat && si.shape == 0
               ? ExpressionLayout::scalar()
               : ExpressionLayout::direct();
  }
  Val with_layout(Val value, ExpressionLayout layout) const {
    value.layout = layout;
    return value;
  }
  ExpressionLayout contiguous_layout(const Val& source, int64_t offset,
                                     const std::string& what) const {
    if (offset < 0)
      throw CompileError("stanli compile: negative layout offset for " + what);
    const ExpressionLayout result =
        expression_layout::contiguous(source.layout, offset);
    if (source.layout.kind == ExpressionLayout::Kind::Direct && !result.known())
      throw CompileError("stanli compile: layout offset overflows for " + what);
    return result;
  }
  ExpressionLayout elementwise_layout(std::initializer_list<Val> inputs,
                                      bool packet_supported = true) const;

  enum class ReductionGrouping : uint8_t { Unknown, Packet, Scalar, Phased };
  // The active scalar type is an independent reason for scalar traversal:
  // Matrix<var> has no packet reducer even when its source layout is direct.
  // Otherwise the source layout describes the Eigen evaluator that Stan Math
  // reduced before graph materialization.
  ReductionGrouping reduction_grouping(const Val& value, bool active) const;

  std::vector<int> reduction_phase_idata(const Val& value,
                                         ReductionGrouping grouping,
                                         const std::string& what) {
    if (grouping != ReductionGrouping::Phased) return {};
    return {checked_immediate(value.layout.element_offset, what + " offset")};
  }
  // Reducing a slot is valid only when its logical view spans exactly the
  // container the MIR overload named. The layout controls grouping; these
  // checks only prevent a partial or padded slot from being mistaken for a
  // complete vector, matrix, or one-dimensional array.
  bool extrema_storage(mir::ExtremaSurface surface, const Val& value);

  IntRange prove_runtime_int_extrema(const mir::Expr& e, const Val& value,
                                     int64_t len);

  std::optional<IntRange> int_operand_range(const mir::Expr& e,
                                            const Val& value) const {
    if (e.kind == mir::Expr::LitInt &&
        e.lit_i >= std::numeric_limits<int32_t>::min() &&
        e.lit_i <= std::numeric_limits<int32_t>::max())
      return IntRange{static_cast<int32_t>(e.lit_i),
                      static_cast<int32_t>(e.lit_i)};
    const auto known = int_ranges.find(value.slot);
    if (known == int_ranges.end()) return std::nullopt;
    return known->second;
  }
  Val lower_extrema_reduction(const mir::Expr& e, CallArguments& actuals,
                              const mir::ExtremaCall& call);

  Val lower_extrema_pair(const mir::Expr& e, CallArguments& actuals,
                         mir::ExtremaKind kind);

  bool runtime_int_extrema_candidate(const mir::Expr& e) const {
    const mir::ExtremaCall call = mir::extrema_call(e);
    if (call.surface == mir::ExtremaSurface::IntArray) {
      if (e.args[0].kind != mir::Expr::Var) return false;
      const auto value = scope.find(e.args[0].name);
      return value != scope.end() && !value->second.si.param_free;
    }
    return call.surface == mir::ExtremaSurface::IntPair &&
           (runtime_int_value(e.args[0]) || runtime_int_value(e.args[1]));
  }
  struct LogicalDims {
    int64_t rows;
    int64_t cols;
  };
  LogicalDims logical_dims(const SlotInfo& si, int64_t len,
                           const std::string& what);

  std::vector<int64_t> logical_shape(const Val& v, const std::string& what) {
    const int64_t len = g.slots[v.slot].len;
    validate_view(v.si, len, what);
    if (is_array(v.si)) return array_shape(v.si).dims;
    if (is_matrix(v.si)) return {v.si.rows, v.si.cols};
    if (is_vector(v.si) || is_row_vector(v.si)) return {len};
    fail(what + ": dims is unsupported for a scalar value");
  }
  Val lower_dims(const mir::Expr& e, CallArguments& actuals);

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
  void validate_view(const SlotInfo& si, int64_t len, const std::string& what);

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
                       const Val& v);

  void sync_indexed_data_local(const std::string& name, const Val& v) {
    td.env().erase(name);
    if (!v.si.param_free) return;
    if (const DataMap::Entry* en = observation(v)) td.env()[name] = *en;
  }
  void observe_indexed_rhs(const mir::Expr& rhs, const Val& v);

  // CmdStan's var_context validates every declared dimension against the
  // supplied values before it reads one, and throws std::invalid_argument
  // naming the variable and both shapes. Without the same check the short
  // side is read past its end, and a host that tells bad data from a
  // broken model by the exception type sees the wrong answer. Only the
  // element count is compared: JSON carries a nested shape but stanc has
  // already flattened the read, and a declaration whose extents multiply
  // out to the supplied count is the shape the reader would produce.
  void validate_data_dims(const std::string& name, const mir::SizedType& t);

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
  static void scan_rebuild(const mir::Stmt& s, RebuildShape& shape);

  static bool canonical_input_rebuild(const mir::Stmt& s,
                                      const std::set<std::string>& inputs);

  void bind_data(const mir::Program& p);

  // Lazily materialize an env value as a data slot when log_prob uses it.
  int env_slot(const std::string& name);

  // Materialize a declared local that has not received its first value yet.
  // Stan initializes real locals and containers to NaN (and integer arrays
  // to INT_MIN).  Both ordinary expression lowering and a runtime region's
  // live-in binder must see that same value: a name can be read inside a
  // parameter-dependent branch without being assigned by the branch, so it
  // will not appear in the region's live-out/assignment scan.
  int uninitialized_decl_slot(const std::string& name);

  // ---- expressions ----------------------------------------------------------
  Val lower_expr(const mir::Expr& e);

  Val lower_expr_impl(const mir::Expr& e);

  // ---- necessity islands ---------------------------------------------------
  // A region whose control flow is not known when the graph is built has no
  // op-graph form: `if (theta > 0)` picks its arm at evaluation time, while a
  // DataOnly graph-local predicate may be unavailable to the data interpreter.
  // Such a region compiles
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
    // Scalar integer locals normally live only in int_env.  A structured
    // while mutates them in the register program, so they leave as ordinary
    // scalar slots and subsequent lowering must stop treating them as folded
    // compile-time values.
    std::vector<bool> out_is_int;
    // The register view of each live-out as the region compiler left it:
    // the authority on shape when the outside declaration was the --O1
    // inliner's zero-length sentinel and the region's assignment sized it.
    std::vector<Range> out_views;
    bool has_target = false;  // the region contributed to the target
    // A print or reject is observable even when every data live-out is empty,
    // so an emitter must not treat the region as having nothing to produce.
    bool has_effect = false;
  };
  static bool expr_has_jacobian(const mir::Expr& e);

  // Does `s` increment the target, explicitly or through a Jacobian call?
  static bool has_target_pe(const mir::Stmt& s);

  bool needs_runtime_control(const mir::Stmt& s);

  // A Break/Continue selected by a runtime condition cannot be lowered as a
  // standalone conditional island: its jump target belongs to the enclosing
  // loop. Promote that whole loop to the necessity island instead. Nested
  // loops own their own control statements and therefore stop this search.
  bool runtime_loop_control(const mir::Stmt& s, bool runtime_path = false);

  // Every name an Assignment targets anywhere in `s`, in first-seen order.
  void assigned_names(const mir::Stmt& s, std::vector<std::string>* out) {
    if (s.kind == mir::Stmt::Assignment &&
        std::find(out->begin(), out->end(), s.lhs) == out->end())
      out->push_back(s.lhs);
    for (const auto& k : s.body) assigned_names(k, out);
  }
  // Remove a return at the lexical end of a statement arm, preserving every
  // statement that precedes it.  This is the structured form used by UDFs
  // such as ctsem's mcalc: each arm returns, but one arm first updates a local
  // matrix.  The updates can lower as an ordinary statement island and the
  // two returned expressions can then join through a ternary value island.
  static bool peel_terminal_return(mir::Stmt* s, mir::Expr* value);

  // Data-only operands of a retained call are folded while the Program is
  // compiled, as their graph counterparts are.
  DataMap::Entry program_constant(ProgramCompiler& c, const mir::Expr& arg,
                                  const std::string& role) {
    auto value = try_eval_pure(arg);
    if (!value) c.bail(role + " must be data-only and known at compile time");
    return std::move(*value);
  }
  double program_scalar_real(ProgramCompiler& c, const mir::Expr& arg,
                             const std::string& role) {
    const DataMap::Entry value = program_constant(c, arg, role);
    if (value.is_int) {
      if (value.i.size() != 1) c.bail(role + " must be one real");
      return static_cast<double>(value.i[0]);
    }
    if (value.r.size() != 1) c.bail(role + " must be one real");
    return value.r[0];
  }
  long program_scalar_int(ProgramCompiler& c, const mir::Expr& arg,
                          const std::string& role) {
    const DataMap::Entry value = program_constant(c, arg, role);
    if (!value.is_int || value.i.size() != 1)
      c.bail(role + " must be one integer");
    return value.i[0];
  }
  std::vector<double> program_vector_real(ProgramCompiler& c,
                                          const mir::Expr& arg,
                                          const std::string& role) {
    DataMap::Entry value = program_constant(c, arg, role);
    if (value.is_int) c.bail(role + " must be a real vector");
    return std::move(value.r);
  }
  // Bind callback arguments [begin, end) of a retained call compiled in a
  // Program. Data arguments fold into the spec; active ones are copied into
  // one register run, which the kernel receives as theta.
  Range program_callback_theta(ProgramCompiler& c, const mir::Expr& e,
                               size_t begin, size_t end, RetainedCallback& spec,
                               int* parameter_count);

  static OdeSpec::Solver ode_solver(mir::OdeMethod method) {
    switch (method) {
      case mir::OdeMethod::Bdf:
        return OdeSpec::BDF;
      case mir::OdeMethod::Adams:
        return OdeSpec::ADAMS;
      case mir::OdeMethod::Ckrk:
        return OdeSpec::CKRK;
      default:
        return OdeSpec::RK45;
    }
  }
  // Retained higher-order algorithms use the graph kernel ABI even when
  // their call site sits in a runtime-control Program. The Program owns the
  // same specification object a graph Op would own, while kernel_call owns
  // all register binding, scratch sizing, and reverse-mode wiring.
  bool lower_program_variadic_algebra(ProgramCompiler& c, const mir::Expr& e,
                                      Range* out_range);

  bool lower_program_quadrature(ProgramCompiler& c, const mir::Expr& e,
                                Range* out_range);

  bool lower_program_ode(ProgramCompiler& c, const mir::Expr& e,
                         Range* out_range);

  bool lower_program_ode_adjoint(ProgramCompiler& c, const mir::Expr& e,
                                 Range* out_range);

  bool lower_program_dae(ProgramCompiler& c, const mir::Expr& e,
                         Range* out_range);

  bool lower_program_higher_order(ProgramCompiler& c, const mir::Expr& e,
                                  Range* out_range);

  // Compile `s` (a statement region) or `e` (a ternary) into a program.
  void lower_island(const mir::Stmt* s, const mir::Expr* e, IslandRegion* reg,
                    Range* expr_out, std::shared_ptr<IslandProg>* prog_out);

  // The OP_ISLAND for a compiled region, plus one extraction per live-out.
  void emit_island(const std::shared_ptr<IslandProg>& prog,
                   const IslandRegion& reg, const std::vector<int>& out_lens,
                   std::vector<int>* out_slots);

  void push_target_term(int slot) {
    if (target_scale != 1.0)
      slot = emit_raw(OP_MUL, {slot, const_slot(target_scale)}, 1, {}).slot;
    target_terms.push_back(slot);
  }
  // Materialize the target visible at the current source position. The
  // reduction consumes a copy, leaving the individual terms available for
  // the final model target. This is shared by direct graph target() reads and
  // the lazy live-in used by runtime-control programs.
  int current_target_slot() {
    std::vector<int> prefix = target_terms;
    prefix.insert(prefix.end(), jac_slots.begin(), jac_slots.end());
    return reduce_terms(std::move(prefix));
  }
  // `if (<not known while building the graph>) ... else ...`
  void lower_runtime_ifelse(const mir::Stmt& s);

  // `<not known while building the graph> ? a : b`
  Val lower_runtime_ternary(const mir::Expr& e);

  // Use the runtime-control compiler as a graph producer for higher-order
  // families whose shared implementation already lives there. This keeps a
  // straight-line graph call and a call under dynamic control on one callback
  // binder and one kernel path instead of growing a second graph-only parser.
  Val lower_program_expression(const mir::Expr& e);

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
  void check_fixed_input_count(size_t n, uint16_t opcode) {
    constexpr size_t kMaxInputs = sizeof(Op::in) / sizeof(Op::in[0]);
    if (n > kMaxInputs)
      fail("opcode " + std::to_string(opcode) + " has " + std::to_string(n) +
               " inputs, but Op::in holds only " + std::to_string(kMaxInputs),
           "");
  }
  // Low-level emission for dynamic slot lists and graph scaffolding whose
  // output dependency is explicit at the call site.
  Val emit_raw(uint16_t opcode, std::vector<int> ins, int64_t out_len,
               SlotInfo out_si, std::vector<int> idata = {}, int out2 = -1,
               bool autodiff = false);

  // The expression seam: a pure result is parameter-free exactly when all of
  // its inputs are. initializer_list avoids a temporary input-list allocation
  // and makes forgetting dependency propagation impossible.
  Val emit_value(uint16_t opcode, std::initializer_list<Val> ins,
                 int64_t out_len, SlotInfo out_si = {},
                 std::vector<int> idata = {}, int out2 = -1);

  // Fallback for expressions with no native lowering: a data-only subtree
  // is evaluated at compile time and materialized as a constant. Unsupported
  // expressions and Stan validation failures decline; the latter must stay
  // at model evaluation rather than move to construction. Propto densities
  // never fold because their value is instantiation-dependent.
  bool expr_effectful(const mir::Expr& e);

  bool reduce_sum_effectful(const mir::Expr& e) {
    if (e.args.empty() || e.args[0].kind != mir::Expr::Var) return true;
    bool propto = false;
    const mir::FunDef* f = mir::resolve_callback(
        fun_defs, mir::reduce_sum_partial_name(e.args[0].name, &propto),
        mir::reduce_sum_partial_views(e));
    // A functor this cannot resolve is refused at lowering. Until it gets
    // there, assume the worst rather than fold away a call whose body has
    // never been examined.
    return f == nullptr || fun_effectful(f->name);
  }
  bool stmt_effectful(const mir::Stmt& s);

  static bool expr_references(const mir::Expr& e, const std::string& name) {
    if (e.kind == mir::Expr::Var && e.name == name) return true;
    for (const auto& a : e.args)
      if (expr_references(a, name)) return true;
    return false;
  }
  // Repeating an expression fewer times is observable for more than RNGs:
  // target() reads the accumulator, compiler-internal calls may validate or
  // emit, and the callback families can hide effects in another function.
  // Admit the ordinary Stan-library expression grammar and explicitly keep
  // those effect-capable seams out. User functions are refused wholesale;
  // proving a UDF repeatable needs its own interprocedural effect summary.
  bool repeatable_target_expr(const mir::Expr& e, const std::string& loopvar);

  static void collect_loop_locals(const mir::Stmt& s,
                                  std::set<std::string>* locals) {
    if (s.kind == mir::Stmt::Decl) locals->insert(s.decl_id);
    for (const auto& child : s.body) collect_loop_locals(child, locals);
  }
  // Conservative statement whitelist for a loop whose only externally
  // visible effect is adding iterator-independent terms to target. Locals
  // declared under the loop may be initialized and updated; any assignment
  // to a name from the enclosing scope refuses the rewrite.
  bool repeatable_target_stmt(const mir::Stmt& s, const std::string& loopvar,
                              const std::set<std::string>& locals,
                              bool* has_target);

  bool repeatable_target_body(const mir::Stmt& loop) {
    std::set<std::string> locals;
    for (const auto& child : loop.body) collect_loop_locals(child, &locals);
    bool has_target = false;
    for (const auto& child : loop.body)
      if (!repeatable_target_stmt(child, loop.loopvar, locals, &has_target))
        return false;
    return has_target;
  }
  bool fun_effectful(const std::string& name);

  // Ask only the MIR interpreter.  Static-shape specialization below uses
  // this for selector values and for path-sensitive short-circuit decisions;
  // keeping it separate from try_eval_pure prevents recursive specialization.
  std::optional<DataMap::Entry> try_eval_interpreter(const mir::Expr& e);

  enum class StaticProbeState : uint8_t { Unknown, Known, Invalid };
  template <typename T>
  struct StaticProbe {
    StaticProbeState state = StaticProbeState::Unknown;
    T value{};
    std::string error;
  };
  struct StaticView {
    int64_t len = 0;
    SlotInfo si;
  };
  struct StaticSelector {
    int64_t count = 0;
    bool drops_dimension = false;
  };
  static bool is_shape_query(const mir::Expr& e) {
    return e.kind == mir::Expr::FunApp && e.args.size() == 1 &&
           (e.name == "rows" || e.name == "cols" || e.name == "size" ||
            e.name == "num_elements" || e.name == "FnLength");
  }
  StaticProbe<long> try_static_int(const mir::Expr& e) {
    auto evaluated = try_eval_interpreter(e);
    if (!evaluated) return {};
    if (!evaluated->is_int || evaluated->i.size() != 1 ||
        evaluated->r.size() != 1)
      return {StaticProbeState::Invalid, 0,
              "static matrix index is not an integer scalar"};
    return {StaticProbeState::Known, evaluated->i[0], {}};
  }
  StaticProbe<StaticSelector> try_static_selector(const mir::Expr& index,
                                                  int64_t extent);
  static bool is_scalar_type(const std::string& type) {
    return type == "UReal" || type == "UInt";
  }
  StaticProbe<StaticView> try_static_broadcast_view(const mir::Expr& e);

  // Logical geometry only: this probe must never materialize a data value or
  // emit a graph op.  Everything it does not recognize declines to the
  // existing runtime-control path.
  StaticProbe<StaticView> try_static_view(const mir::Expr& e);

  StaticProbe<int64_t> try_static_shape_query(const mir::Expr& e);

  // Replace only shape queries proven from immutable logical geometry.  The
  // walk is lazy across Stan's short-circuit forms: an invalid subview in a
  // dead RHS/arm must not become a bind-time error merely because this probe
  // visited it.
  bool specialize_static_shapes(mir::Expr* e);

  std::optional<DataMap::Entry> try_eval_pure(const mir::Expr& e) {
    if (expr_effectful(e)) return std::nullopt;
    if (auto evaluated = try_eval_interpreter(e)) return evaluated;
    mir::Expr specialized = e;
    if (!specialize_static_shapes(&specialized)) return std::nullopt;
    return try_eval_interpreter(specialized);
  }
  DataMap::Entry eval_pure(const mir::Expr& e, const std::string& use) {
    if (expr_effectful(e))
      fail("effectful expression cannot be used for " + use, e.raw);
    return td.eval(e);
  }
  std::optional<Val> fold_const(const mir::Expr& e);

  // Integer argument of a density/pmf: values must be known at compile
  // time (int data, loop variables, or compile-time expressions).
  std::vector<int> int_arg_values(LoweredArgument& actual);

  // Matrix shape of an elementwise result: whichever operand carries one
  // (both must agree when both do).
  SlotInfo shape_of(const Val& a, const Val& b);

  // Two-argument scalar math with one int argument
  // (STANLI_SCALAR_BINARY_INT_FIRST_LIST and its SECOND twin): elementwise
  // with scalar broadcast like the all-real binaries, but shape_of does not
  // apply. Those two sides may legitimately carry different views --
  // `ldexp(matrix, array[,] int)` is a matrix, `falling_factorial(real,
  // array[,] int)` is an array -- so the result takes the real side's view
  // when it has one and the int side's when the real side is a scalar,
  // which is what the signature list says in every case.
  Val lower_binary_int(const BuiltinSpec& spec, CallArguments& actuals);

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
  std::optional<Val> lower_callable_transform(const mir::Expr& e,
                                              CallArguments& actuals);

  // The inverse transforms. stan-math has no rev overloads for these: its
  // `log(y - lb)` is ordinary var arithmetic, which is what these
  // elementwise ops emit, so the composition is the reference rather than an
  // approximation of it, and no new kernel is needed.
  Val free_transform(uint16_t opcode, const std::vector<Val>& a, SlotInfo si,
                     int64_t n);

  // Value of a data-only expression at compile time. The interpreter
  // handles most cases; a UDF-local constant lives only as a slot, so fall
  // back to that slot's recorded fill.
  std::vector<double> const_values(const mir::Expr& e);

  std::vector<int> const_ints(const mir::Expr& e);

  // Thrown by a Return statement inside an inlined UDF body.
  struct LpReturn {
    Val v;
  };
  struct LoopBreak {};
  struct LoopContinue {};
  // Inline a user-defined function at its call site: arguments are lowered
  // in the caller's scope, bound under the parameter names in a shadowed
  // scope, and the body lowers like any other statements (loops unroll,
  // data-only conditions resolve). Return throws the result value out.
  Val lower_call_udf(const mir::Expr& e,
                     const std::function<void()>& before_body = {});

  Val lower_multi_normal_rng(const mir::Expr& e, CallArguments& actuals);

  Val lower_dirichlet_rng(const mir::Expr& e, CallArguments& actuals);

  Val lower_regular_unary(uint16_t opcode, const std::string& type_,
                          const std::string& name, const std::string& raw,
                          Val a);

  Val lower_categorical_rng(const mir::Expr& e, CallArguments& actuals);

  Val lower_scalar_rng(const mir::Expr& e, CallArguments& actuals,
                       ScalarRng family);

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
    return expr_effectful(e) || runtime_int_sum_candidate(e) ||
           runtime_int_extrema_candidate(e);
  }
  // Availability is independent of both MIR's AD type and param_free. A
  // data-only loop result lives in a slot without a compile-time observation.
  // This probe does not lower or execute anything (in particular, no UDF loop).
  bool needs_runtime_value(const mir::Expr& e);

  bool runtime_int_value(const mir::Expr& e) const;

  Val lower_runtime_int_sum(const mir::Expr& e, CallArguments& actuals);

  // map_rect checks that the three job arrays have matching OUTER sizes and
  // returns an empty vector before touching the shared parameters or the UDF
  // when that size is zero.  This is the one map_rect case which needs no
  // runtime callback at all (and is exercised by stanc3's mother model).
  // Nonempty calls deliberately keep falling through to the unsupported
  // function diagnostic.
  std::optional<Val> lower_empty_map_rect(const mir::Expr& e,
                                          CallArguments& actuals);

  mir::Expr slice_bound_literal(int64_t value, const std::string& raw);

  // reduce_sum(f, sliced, grainsize, shared...) sums f over the terms of a
  // partition of `sliced`, and its contract is that the partition is
  // unobservable: the terms must sum to the same value however the slice is
  // cut. Stan Math without STAN_THREADS takes that freedom to its limit and
  // makes exactly one call over the whole slice, returning zero for an empty
  // one (prim/functor/reduce_sum.hpp). stanli has no threading, so it lowers
  // to that same single call. That is not an approximation to be reconciled
  // later: it agrees with default CmdStan term for term, and it is also the
  // fastest shape available here, because cutting the slice would shorten
  // the callee's vectorized densities and buy nothing back.
  //
  // Written out, the call is an ordinary user-function call, so this rewrites
  // it to f(sliced, 1, size(sliced), shared...) and hands that to the
  // inliner, which already owns argument binding, propto threading, and the
  // data-only formal rules.
  Val lower_reduce_sum(const mir::Expr& e, CallArguments& actuals);

  Val lower_append_array(const mir::Expr& e, CallArguments& actuals);

  Val lower_funapp(const mir::Expr& e);

  // Density calls: the registry-planned kernels.
  std::optional<Val> lower_density_fn(const mir::Expr& e,
                                      CallArguments& actuals,
                                      const DensitySpec* selected);

  // Elementwise math, reductions, and dot products.
  std::optional<Val> lower_eltwise_fn(const mir::Expr& e,
                                      CallArguments& actuals,
                                      const BuiltinSpec* builtin);

  // Matrix shape and algebra: transposes, reshapes, factorizations,
  // slices, and concatenations.
  std::optional<Val> lower_matrix_fn(const mir::Expr& e,
                                     CallArguments& actuals);

  // The deprecated algebra_solver interfaces (Powell and Newton):
  //
  //   algebra_solver(f, x, y, x_r, x_i[, rel_tol, f_tol, max_steps])
  //
  // x is an initial guess.  It influences which root is selected but legacy
  // Stan Math intentionally returns value_type_t<y>, so only y participates
  // in autodiff.  Keep x as a graph input for values while stamping the op's
  // activity and result scalar type from y alone.
  Val lower_quadrature_fn(const mir::Expr& e, CallArguments& actuals);

  Val lower_algebra_fn(const mir::Expr& e, CallArguments& actuals);

  // stan-math's own defaults differ per solver: rk45 1e-6/1e-6/1e6 (the
  // OdeSpec field initializers), BDF/Adams 1e-10/1e-10/1e8. Using one set
  // for both left one_comp_mm's gradients 2.9e-6 off CmdStan, so both
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
               int64_t N, int64_t S, SlotInfo result_si,
               std::optional<Val> t0 = std::nullopt,
               std::optional<Val> ts = std::nullopt);

  SlotInfo ode_result_view(const mir::Expr& e, int64_t N, int64_t S);

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
  std::optional<Val> lower_ode_variadic(const mir::Expr& e,
                                        CallArguments& actuals);

  // The integrate_ode_* family.
  std::optional<Val> lower_ode_fn(const mir::Expr& e, CallArguments& actuals);

  // ---- statements -----------------------------------------------------------
  CompiledModel::ParamView parameter_view(const mir::Stmt& s, int slot,
                                          int64_t len);

  void lower_read_param(const mir::Stmt& s);

  struct DeclView {
    int64_t len = 0;
    bool autodiff = false;
    SlotInfo si;
    bool int_array = false;
    bool deferred_shape = false;
    std::vector<int> runtime_dims;
  };
  // The only name-keyed declaration protocol. Runtime values carry the same
  // static scalar type and SlotInfo in `scope`; this registry is needed only
  // before first binding.
  std::map<std::string, DeclView> decls;
#include "lower_structured_loop.inc"

  void lower_stmt(const mir::Stmt& s) {
    if (region_current) {
      lower_region_stmt(s);
      return;
    }
    const bool loop = s.kind == mir::Stmt::For || s.kind == mir::Stmt::While;
    structured_outer_depth += loop;
    try {
      lower_stmt_impl(s);
    } catch (...) {
      structured_outer_depth -= loop;
      throw;
    }
    structured_outer_depth -= loop;
  }
  void lower_stmt_impl(const mir::Stmt& s);

  // Scalar terms reduce through chained ADD_N ops (6-input limit per op).
  int reduce_terms(std::vector<int> terms);

  // The write_array graph: same unconstrained draw in, every CSV column out.
  // Forward-only, so no target, no jacobian, no adjoints, and no per-lane
  // partitioning or islands -- reroll, constfold, and CSE still apply,
  // because generated quantities are unrolled over the data exactly like
  // the model block is.
  struct PassPlan {
    bool constfold;
    bool partition;
    bool elide_stores;
    bool cse;
    bool island;
  };
  // Shared tail of both lowerings: inplace/store-forward/reroll always run;
  // the rest is gated by plan so write_array can skip the passes that assume
  // a scalar log-density result. Ordering constraints between the stages
  // that do run are noted where each stage starts.
  void run_passes(const std::vector<int>& roots, const PassPlan& plan);

  CompiledModel::WriteArray run_write_array(const mir::Program& p);

  CompiledModel run(const mir::Program& p);
};

}  // namespace lower_detail
}  // namespace stanli

#endif
