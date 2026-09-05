#include <stanli/algebra.hpp>
#include <stanli/callable_transform.hpp>
#include <stanli/compile.hpp>
#include <stanli/unconstrain.hpp>
#include <stanli/constfold.hpp>
#include <stanli/cse.hpp>
#include <stanli/dae.hpp>
#include <stanli/higher_order_eval.hpp>
#include <stanli/expression_layout.hpp>
#include <stanli/graph_print.hpp>
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
#include <stanli/regular_builtin.hpp>
#include <stanli/structured_check.hpp>
#include <stanli/wa_interp.hpp>

#include "reroll_profile.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif
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

// STANLI_DUMP_PASSES=<dir>|-: one graph dump per lowering stage, to files or
// to stdout. STANLI_DUMP_STAGES selects stages by bare name or graph:stage.
// Separate from PrepTrace, which selects a different reroll implementation
// when it is on.
struct PassDumper {
  PassDumper(const char* dir, const char* stages) : dir_(dir ? dir : "") {
    if (stages && *stages) {
      std::string_view rest(stages);
      while (!rest.empty()) {
        const size_t comma = rest.find(',');
        const std::string_view one = rest.substr(0, comma);
        if (!one.empty()) stages_.emplace_back(one);
        rest = comma == std::string_view::npos ? std::string_view()
                                               : rest.substr(comma + 1);
      }
    }
    if (stages_.size() == 1 && stages_[0] == "all") stages_.clear();
    if (dir_.empty() || to_stdout()) return;
    for (size_t i = 1; i <= dir_.size(); ++i) {
      if (i < dir_.size() && dir_[i] != '/' && dir_[i] != '\\') continue;
      const std::string part = dir_.substr(0, i);
#ifdef _WIN32
      _mkdir(part.c_str());
#else
      ::mkdir(part.c_str(), 0777);
#endif
    }
  }

  bool enabled() const { return !dir_.empty(); }

  bool selects(const std::string& label) const {
    if (stages_.empty()) return true;
    const size_t colon = label.find(':');
    const std::string stage =
        colon == std::string::npos ? label : label.substr(colon + 1);
    for (const std::string& want : stages_)
      if (want == label || want == stage) return true;
    return false;
  }

  // The sequence number advances for every stage the lowering reaches, so a
  // filtered run keeps the numbers an unfiltered one would have given.
  void write(const std::string& label, const std::string& name,
             const std::string& text, bool unfiltered = false) {
    const int n = n_++;
    if (!unfiltered && !selects(label)) return;
    if (to_stdout()) {
      std::printf(";; %s\n", label.c_str());
      std::fwrite(text.data(), 1, text.size(), stdout);
      if (!text.empty() && text.back() != '\n') std::fputc('\n', stdout);
      std::printf(";; end %s\n", label.c_str());
      std::fflush(stdout);
      return;
    }
    char prefix[8];
    std::snprintf(prefix, sizeof(prefix), "%02d-", n);
    const std::string path = dir_ + "/" + prefix + name;
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
  }

 private:
  bool to_stdout() const { return dir_ == "-"; }

  std::string dir_;
  std::vector<std::string> stages_;
  int n_ = 0;
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
    std::optional<RegularSpec> regular;
    std::optional<ScalarRng> scalar_rng;

    BuiltinDispatch() = default;
    BuiltinDispatch(BuiltinFamily selected,
                    std::optional<RegularSpec> regular_call = std::nullopt,
                    std::optional<ScalarRng> rng = std::nullopt)
        : family(selected), regular(regular_call), scalar_rng(rng) {}
  };

  static BuiltinDispatch rng_dispatch(ScalarRng family) {
    return {BuiltinFamily::ScalarRng, std::nullopt, family};
  }

  static bool ends_with(std::string_view name, std::string_view suffix) {
    return name.size() >= suffix.size() &&
           name.substr(name.size() - suffix.size()) == suffix;
  }

  static BuiltinDispatch resolve_builtin(const mir::Expr& e) {
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
    static const std::unordered_map<std::string_view, BuiltinDispatch>
        kBuiltins = {
            {"multi_normal_rng", BuiltinFamily::MultiNormalRng},
            {"dirichlet_rng", BuiltinFamily::DirichletRng},
            {"categorical_rng", BuiltinFamily::CategoricalRng},
            {"append_array", BuiltinFamily::AppendArray},
            {"Transpose__", BuiltinFamily::Matrix},
            {"transpose", BuiltinFamily::Matrix},
            {"tcrossprod", BuiltinFamily::Matrix},
            {"crossprod", BuiltinFamily::Matrix},
            {"diag_pre_multiply", BuiltinFamily::Matrix},
            {"diag_post_multiply", BuiltinFamily::Matrix},
            {"multiply_lower_tri_self_transpose", BuiltinFamily::Matrix},
            {"to_matrix", BuiltinFamily::Matrix},
            {"to_vector", BuiltinFamily::Matrix},
            {"to_row_vector", BuiltinFamily::Matrix},
            {"to_array_1d", BuiltinFamily::Matrix},
            {"rep_matrix", BuiltinFamily::Matrix},
            {"gp_exp_quad_cov", BuiltinFamily::Matrix},
            {"diag_matrix", BuiltinFamily::Matrix},
            {"cholesky_decompose", BuiltinFamily::Matrix},
            {"matrix_exp", BuiltinFamily::Matrix},
            {"inverse", BuiltinFamily::Matrix},
            {"inverse_spd", BuiltinFamily::Matrix},
            {"log_determinant", BuiltinFamily::Matrix},
            {"eigenvalues_sym", BuiltinFamily::Matrix},
            {"eigenvectors_sym", BuiltinFamily::Matrix},
            {"quad_form_diag", BuiltinFamily::Matrix},
            {"quad_form_sym", BuiltinFamily::Matrix},
            {"quad_form", BuiltinFamily::Matrix},
            {"add_diag", BuiltinFamily::Matrix},
            {"append_row", BuiltinFamily::Matrix},
            {"append_col", BuiltinFamily::Matrix},
            {"segment", BuiltinFamily::Matrix},
            {"sub_col", BuiltinFamily::Matrix},
            {"block", BuiltinFamily::Matrix},
            {"col", BuiltinFamily::Matrix},
            {"diagonal", BuiltinFamily::Matrix},
            {"row", BuiltinFamily::Matrix},
            {"head", BuiltinFamily::Matrix},
            {"tail", BuiltinFamily::Matrix},
            {"reverse", BuiltinFamily::Matrix},
            {"rows", BuiltinFamily::ShapeQuery},
            {"cols", BuiltinFamily::ShapeQuery},
            {"size", BuiltinFamily::ShapeQuery},
            {"num_elements", BuiltinFamily::ShapeQuery},

        };
    const auto builtin = kBuiltins.find(e.name);
    if (builtin != kBuiltins.end()) return builtin->second;
    if (const auto regular = resolve_regular_builtin(e.name, e.args.size()))
      return {BuiltinFamily::Elementwise, *regular};
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
  PassDumper& dumper;
  const char* prep_graph;
  const char* last_stage = "start";
  std::vector<int> last_roots;
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
  std::map<std::string, Val> scope;      // var -> value and logical view
  std::map<std::string, long> int_env;   // data int scalars
  std::map<int, IntRange> int_ranges;    // runtime integral slot provenance
  std::map<int, RealRange> real_ranges;  // finite runtime scalar bounds
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
  std::set<std::string> int_locals;  // SInt locals in log_prob (data-only)
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
      const DataMap& d, PrepTrace& p, PassDumper& dump_to,
      const char* graph_name,
      std::shared_ptr<ShapeInterner> pool = std::make_shared<ShapeInterner>())
      : data(d),
        shape_pool(std::move(pool)),
        prep(p),
        dumper(dump_to),
        prep_graph(graph_name) {}

  void dump_named(const std::string& label, const std::string& name,
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

  void dump(const char* stage, const std::vector<int>& roots) {
    if (!dumper.enabled()) return;
    dump_named(std::string(prep_graph) + ":" + stage,
               std::string(prep_graph) + "-" + stage + ".txt", roots, false);
    last_stage = stage;
    last_roots = roots;
  }

  struct DumpOnThrow {
    Lowering& lo;
    bool done = false;
    ~DumpOnThrow() {
      if (done || !lo.dumper.enabled()) return;
      try {
        lo.dump_named(
            std::string(lo.prep_graph) + ":FAILED-after-" + lo.last_stage,
            std::string(lo.prep_graph) + "-FAILED-after-" + lo.last_stage +
                ".txt",
            lo.last_roots, true);
      } catch (...) {
      }
    }
  };

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

  // Every index the lowering sees is a bind-time constant, so what CmdStan
  // bounds-checks at runtime is checked here instead.
  std::vector<int64_t> index_positions(const mir::Expr& ix, int64_t extent,
                                       const char* what,
                                       const std::string& raw) {
    std::vector<int64_t> out;
    if (ix.name == "IndexAll") {
      for (int64_t i = 0; i < extent; ++i) out.push_back(i);
      return out;
    }
    if (ix.name == "IndexSingle") {
      const int64_t i = eval_int(ix.args[0]);
      check_index(i, extent, what, raw);
      return {i - 1};
    }
    if (ix.name == "IndexBetween") {
      const int64_t lo = eval_int(ix.args[0]), hi = eval_int(ix.args[1]);
      check_range(lo, hi, extent, what, raw);
      for (int64_t i = lo; i <= hi; ++i) out.push_back(i - 1);
      return out;
    }
    if (ix.name == "IndexMulti") {
      DataMap::Entry iv = eval_pure(ix.args[0], "an index list");
      if (!iv.is_int) fail(std::string(what) + " needs int data", raw);
      for (int i : iv.i) {
        check_index(i, extent, what, raw);
        out.push_back(i - 1);
      }
      return out;
    }
    fail(std::string("unsupported ") + what + " " + ix.name, raw);
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
                            int64_t start, int64_t stride) {
    // A write of an observed value into an observed base stays observed:
    // splice the element into a copy of the base's entry.
    if (const DataMap::Entry* be = observation(base)) {
      const DataMap::Entry* re = observation(rhs);
      const int64_t rl = g.slots[rhs.slot].len;
      if ((rl == 0 || re) &&
          g.slots[out_v.slot].len == g.slots[base.slot].len) {
        DataMap::Entry en = *be;
        bool ok = true;
        for (int64_t k = 0; k < rl; ++k) {
          const int64_t at = start + k * stride;
          if (at < 0 || at >= (int64_t)en.r.size()) {
            ok = false;
            break;
          }
          const double v = k < (int64_t)re->r.size()
                               ? re->r[(size_t)k]
                               : static_cast<double>(re->i.at((size_t)k));
          en.r[(size_t)at] = v;
          if (!en.i.empty()) en.i[(size_t)at] = (int)v;
        }
        if (ok) observe(out_v, std::move(en));
      }
    }
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
        // A structured integer may live in a graph slot while its interval
        // proof has collapsed to one value (for example d = rows(mat)).  That
        // value is safe for fixed storage geometry even though it is computed
        // again when the retained body executes.
        if (region_current) {
          const auto value = scope.find(e.name);
          if (value != scope.end()) {
            const auto range = int_ranges.find(value->second.slot);
            if (range != int_ranges.end() &&
                range->second.lo == range->second.hi)
              return range->second.lo;
          }
        }
        fail("size expression needs unknown int " + e.name);
      }
      case mir::Expr::Indexed: {
        // O1 can leave an empty Indexed wrapper around a fully composed
        // integer access, just as it does for real-valued expressions.
        if (e.args.size() == 1) return eval_int(e.args[0]);
        DataMap::Entry* en = e.args[0].kind == mir::Expr::Var
                                 ? td.find(e.args[0].name)
                                 : nullptr;
        if (en && en->is_int && e.args.size() == 2 &&
            e.args[1].name == "IndexSingle") {
          const long index = eval_int(e.args[1].args[0]);
          if (index < 1 || (size_t)index > en->i.size())
            fail("integer index " + std::to_string(index) +
                     " out of bounds for size " + std::to_string(en->i.size()),
                 e.raw);
          return en->i[(size_t)index - 1];
        }
        if (en && en->is_int && e.args.size() == 3 &&
            e.args[1].name == "IndexSingle" &&
            e.args[2].name == "IndexSingle" && en->dims.size() == 2) {
          const long row = eval_int(e.args[1].args[0]);
          const long col = eval_int(e.args[2].args[0]);
          if (row < 1 || row > en->dims[0] || col < 1 || col > en->dims[1])
            fail("integer matrix index out of bounds", e.raw);
          return en->i[(size_t)((col - 1) * en->dims[0] + row - 1)];
        }
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
      case mir::Expr::TernaryIf: {
        if (e.args.size() != 3)
          fail("malformed conditional size expression", e.raw);
        const bool condition = eval_int(e.args[0]) != 0;
        return eval_int(e.args[condition ? 1 : 2]);
      }
      case mir::Expr::EOr: {
        if (e.args.size() != 2)
          fail("malformed logical size expression", e.raw);
        return eval_int(e.args[0]) != 0 || eval_int(e.args[1]) != 0;
      }
      case mir::Expr::EAnd: {
        if (e.args.size() != 2)
          fail("malformed logical size expression", e.raw);
        return eval_int(e.args[0]) != 0 && eval_int(e.args[1]) != 0;
      }
      case mir::Expr::Promotion:
        if (e.args.size() != 1)
          fail("malformed promoted size expression", e.raw);
        return eval_int(e.args[0]);
      case mir::Expr::FunApp:
        if (e.name == "sum" && e.args.size() == 1) {
          long acc = 0;
          for (int v : const_ints(e.args[0])) acc += v;
          return acc;
        }
        if (e.name == "Plus__")
          return eval_int(e.args[0]) + eval_int(e.args[1]);
        if (e.name == "Minus__")
          return eval_int(e.args[0]) - eval_int(e.args[1]);
        if (e.name == "Times__")
          return eval_int(e.args[0]) * eval_int(e.args[1]);
        if ((e.name == "Equals__" || e.name == "NEquals__" ||
             e.name == "Greater__" || e.name == "Geq__" || e.name == "Less__" ||
             e.name == "Leq__") &&
            e.args.size() == 2) {
          const auto scalar = [&](const mir::Expr& arg) -> double {
            if (arg.type_ == "UInt") return (double)eval_int(arg);
            if (auto evaluated = try_eval_pure(arg)) {
              if (evaluated->r.size() == 1) return evaluated->r[0];
            }
            if (arg.kind == mir::Expr::Var) {
              const auto it = scope.find(arg.name);
              if (it != scope.end())
                if (const DataMap::Entry* en = observation(it->second))
                  if (en->r.size() == 1) return en->r[0];
            }
            fail("comparison operand is not known data", arg.raw);
          };
          const double lhs = scalar(e.args[0]), rhs = scalar(e.args[1]);
          if (e.name == "Equals__") return lhs == rhs;
          if (e.name == "NEquals__") return lhs != rhs;
          if (e.name == "Greater__") return lhs > rhs;
          if (e.name == "Geq__") return lhs >= rhs;
          if (e.name == "Less__") return lhs < rhs;
          return lhs <= rhs;
        }
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
          CallArguments actuals(*this, e);
          const Val v = actuals.at(0).value();
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
                                      bool packet_supported = true) const {
    if (inputs.size() == 0) return ExpressionLayout::unknown();
    bool all_scalar = true;
    bool all_known = true;
    bool all_packet_access = true;
    for (const Val& input : inputs) all_scalar = all_scalar && is_scalar(input);
    for (const Val& input : inputs) {
      if (is_scalar(input)) continue;
      all_known = all_known && input.layout.known();
      all_packet_access = all_packet_access && input.layout.packet_access();
    }
    return expression_layout::elementwise(all_scalar, packet_supported,
                                          all_known, all_packet_access);
  }

  enum class ReductionGrouping : uint8_t { Unknown, Packet, Scalar, Phased };

  // The active scalar type is an independent reason for scalar traversal:
  // Matrix<var> has no packet reducer even when its source layout is direct.
  // Otherwise the source layout describes the Eigen evaluator that Stan Math
  // reduced before graph materialization.
  ReductionGrouping reduction_grouping(const Val& value, bool active) const {
    if (active) return ReductionGrouping::Scalar;
    switch (value.layout.kind) {
      case ExpressionLayout::Kind::Unknown:
        return ReductionGrouping::Unknown;
      case ExpressionLayout::Kind::Scalar:
        return ReductionGrouping::Scalar;
      case ExpressionLayout::Kind::Packet:
        return ReductionGrouping::Packet;
      case ExpressionLayout::Kind::Direct:
        return value.layout.element_offset == 0 ? ReductionGrouping::Packet
                                                : ReductionGrouping::Phased;
    }
    return ReductionGrouping::Unknown;
  }

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
  bool extrema_storage(mir::ExtremaSurface surface, const Val& value) {
    const int64_t len = g.slots[value.slot].len;
    switch (surface) {
      case mir::ExtremaSurface::RealVector:
        return is_vector(value.si) || is_row_vector(value.si);
      case mir::ExtremaSurface::RealMatrix:
        return is_matrix(value.si) &&
               checked_product({value.si.rows, value.si.cols},
                               "min/max matrix shape") == len;
      case mir::ExtremaSurface::RealArray:
      case mir::ExtremaSurface::IntArray: {
        if (!is_array(value.si)) return false;
        const ArrayShape& shape = array_shape(value.si);
        return shape.leaf == ViewKind::Flat && shape.dims.size() == 1 &&
               shape.dims[0] == len;
      }
      default:
        return false;
    }
  }

  IntRange prove_runtime_int_extrema(const mir::Expr& e, const Val& value,
                                     int64_t len) {
    if (!in_write_array)
      fail("runtime integer min/max is supported only in generated quantities",
           e.raw);
    // Stan Math raises for an empty integer container; the forward graph
    // kernel cannot reproduce that exception at execution time.
    if (len == 0)
      fail("min/max over an empty int array stays on WaInterp", e.raw);
    if (value.si.param_free)
      fail("min/max needs a runtime-produced int array", e.raw);
    const auto initialized = int_initialized_prefix.find(value.slot);
    if (initialized == int_initialized_prefix.end() ||
        initialized->second != len)
      fail("min/max int array is not definitely initialized", e.raw);
    const auto known = int_ranges.find(value.slot);
    if (known == int_ranges.end())
      fail("min/max int array has unproved integral slot values", e.raw);
    return known->second;
  }

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
                              const mir::ExtremaCall& call) {
    actuals.require_arity(1);
    Val value = actuals.at(0).value();
    const int64_t len = g.slots[value.slot].len;
    if (!extrema_storage(call.surface, value) || len < 0)
      fail("min/max argument is not the whole declared container", e.raw);

    const bool int_array = call.surface == mir::ExtremaSurface::IntArray;
    const bool active = value.autodiff && !in_write_array;
    const ReductionGrouping grouping = int_array
                                           ? ReductionGrouping::Scalar
                                           : reduction_grouping(value, active);
    if (grouping == ReductionGrouping::Unknown)
      fail("min/max expression grouping is not native", e.raw);
    const bool scalar = grouping == ReductionGrouping::Scalar;
    const bool phased = grouping == ReductionGrouping::Phased;
    const IntRange range =
        int_array ? prove_runtime_int_extrema(e, value, len) : IntRange{};
    Val result = with_layout(
        emit_value(OP_EXTREMA_VEC, {value}, 1, view_of(e.type_),
                   reduction_phase_idata(value, grouping, "min/max")),
        ExpressionLayout::scalar());
    if (in_write_array || int_array) result.autodiff = false;
    // Bit 0 selects max. Bits 1 and 2 are an exclusive grouping selector:
    // scalar coefficient order and phased packet order respectively.
    g.ops.back().variant =
        static_cast<uint8_t>((call.kind == mir::ExtremaKind::Max ? 1u : 0u) |
                             (scalar ? 2u : 0u) | (phased ? 4u : 0u));
    if (int_array) {
      result.si.param_free = false;
      set_int_range(result, range.lo, range.hi);
    }
    return result;
  }

  Val lower_extrema_pair(const mir::Expr& e, CallArguments& actuals,
                         mir::ExtremaKind kind) {
    actuals.require_arity(2);
    Val x = actuals.at(0).value();
    Val y = actuals.at(1).value();
    if (!is_scalar(x) || !is_scalar(y))
      fail("min/max scalar overload needs two scalar int arguments", e.raw);
    const bool maximum = kind == mir::ExtremaKind::Max;
    Val result = with_layout(
        emit_value(maximum ? OP_FMAX : OP_FMIN, {x, y}, 1, view_of("UInt")),
        ExpressionLayout::scalar());
    result.autodiff = false;
    result.si.param_free = false;
    const std::optional<IntRange> a = int_operand_range(e.args[0], x);
    const std::optional<IntRange> b = int_operand_range(e.args[1], y);
    if (a && b) {
      set_int_range(result,
                    maximum ? std::max(a->lo, b->lo) : std::min(a->lo, b->lo),
                    maximum ? std::max(a->hi, b->hi) : std::min(a->hi, b->hi));
    } else {
      set_int_initialized(result);
    }
    return result;
  }

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

  Val lower_dims(const mir::Expr& e, CallArguments& actuals) {
    if (e.args.size() != 1) fail("dims arity", e.raw);
    const std::vector<int64_t> dims =
        logical_shape(actuals.at(0).value(), "dims");
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

  void sync_indexed_data_local(const std::string& name, const Val& v) {
    td.env().erase(name);
    if (!v.si.param_free) return;
    if (const DataMap::Entry* en = observation(v)) td.env()[name] = *en;
  }

  void observe_indexed_rhs(const mir::Expr& rhs, const Val& v) {
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
    Val v{s, false, si, owning_layout(si)};
    scope[name] = v;
    observe(v, *en);
    return s;
  }

  // Materialize a declared local that has not received its first value yet.
  // Stan initializes real locals and containers to NaN (and integer arrays
  // to INT_MIN).  Both ordinary expression lowering and a runtime region's
  // live-in binder must see that same value: a name can be read inside a
  // parameter-dependent branch without being assigned by the branch, so it
  // will not appear in the region's live-out/assignment scan.
  int uninitialized_decl_slot(const std::string& name) {
    auto dl = decls.find(name);
    if (dl == decls.end()) return -1;
    if (dl->second.deferred_shape)
      fail("unsized local read before its first assignment: " + name);
    SlotInfo si = dl->second.si;
    si.param_free = true;
    Val value{add_slot(dl->second.len, false), dl->second.autodiff, si,
              owning_layout(si), dl->second.runtime_dims};
    const double initial =
        dl->second.int_array
            ? static_cast<double>(std::numeric_limits<int>::min())
            : std::numeric_limits<double>::quiet_NaN();
    out.fills.emplace_back(value.slot,
                           std::vector<double>(dl->second.len, initial));
    if (dl->second.int_array) set_uninitialized_int_array(value);
    observe_fill(value, dl->second.int_array, initial, dl->second.len);
    scope[name] = value;
    return value.slot;
  }

  // ---- expressions ----------------------------------------------------------
  Val lower_expr(const mir::Expr& e) {
    std::optional<Val> structured;
    if (region_current)
      structured = region_expr(e);
    else if (runtime_int_expression(e))
      structured = lower_runtime_scalar(e);
    Val value = structured ? *structured : lower_expr_impl(e);
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
          // A declared local read before its first write: Materialize
          // the same uninitialized container the indexed-assignment path would.
          if (uninitialized_decl_slot(e.name) >= 0) return scope.at(e.name);
          fail("unknown variable " + e.name);
        }
        return it->second;
      }
      case mir::Expr::Indexed: {
        // O1 index composition can leave an empty outer Indexed node around
        // an already-indexed value. The outer node owns the final result
        // type: for M[idx, idx] passed to a UDF that reads x[i, j], the inner
        // single/single access still says UMatrix and this wrapper says UReal.
        // Collapse the wrapper and lower the composed access with that final
        // type instead of rejecting the stale intermediate matrix type.
        if (e.args.size() == 1 && e.args[0].kind == mir::Expr::Indexed) {
          mir::Expr composed = e.args[0];
          composed.type_ = e.type_;
          composed.unsized = e.unsized;
          composed.data_only = e.data_only;
          composed.promoted = e.promoted;
          composed.raw = e.raw;
          return lower_expr(composed);
        }
        // All-Single indices with compile-time values -> element read.
        Val base = lower_expr(e.args[0]);
        // O1 drops a full-span read's All indices, so `m[:, :]` arrives as an
        // Indexed node with none left.
        if (e.args.size() == 1) return base;
        if (e.args.size() == 2 && e.args[1].name == "IndexAll") return base;
        if (std::any_of(
                e.args.begin() + 1, e.args.end(),
                [&](const mir::Expr& ix) { return runtime_selector(ix); }))
          return region_index(base, {e.args.begin() + 1, e.args.end()}, e.type_,
                              e.unsized);
        if (in_write_array && e.args.size() == 2 &&
            e.args[1].name == "IndexSingle" &&
            runtime_int_value(e.args[1].args[0])) {
          const Val index = lower_expr(e.args[1].args[0]);
          if (!is_scalar(index)) fail("runtime index is not scalar", e.raw);
          int64_t count = 0, width = 0;
          if (is_array(base.si)) {
            const ArrayShape& shape = array_shape(base.si);
            const size_t outer =
                shape.dims.size() - (size_t)leaf_rank(shape.leaf);
            if (outer != 1 || shape.dims.empty() ||
                shape.leaf == ViewKind::Matrix)
              fail("runtime index needs one outer array dimension", e.raw);
            count = shape.dims.front();
            width = count == 0 ? 0 : g.slots[base.slot].len / count;
          } else if (is_vector(base.si) || is_row_vector(base.si)) {
            count = g.slots[base.slot].len;
            width = 1;
          } else {
            fail("runtime index needs a vector or flat outer array", e.raw);
          }
          if (count <= 0 || width <= 0 ||
              g.slots[base.slot].len != count * width)
            fail("runtime index has an invalid base shape", e.raw);
          SlotInfo si = indexed_view(base.si, 1, width, e.type_);
          Val value =
              emit_value(OP_DYNAMIC_SLICE, {base, index}, width, si,
                         {checked_immediate(count, "runtime index extent")});
          value.si.param_free = false;
          value.layout = owning_layout(value.si);
          return value;
        }
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
          return with_layout(
              emit_value(OP_GATHER, {base}, (int64_t)rows.size() * base.si.cols,
                         si, gather),
              ExpressionLayout::scalar());
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
          const int64_t offset = hi >= lo ? (lo - 1) * width : 0;
          SlotInfo si = array_view(std::move(out_dims), sh.leaf);
          return with_layout(
              emit_value(OP_SLICE, {base}, len, si,
                         {checked_immediate(offset, "array range offset")}),
              owning_layout(si));
        }
        // Between subrange read on a 1-D value: v[a:b] is contiguous.
        // hi < lo is empty, not negative-length.
        if (e.args.size() == 2 && e.args[1].name == "IndexBetween") {
          const int64_t lo = eval_int(e.args[1].args[0]);
          const int64_t hi = eval_int(e.args[1].args[1]);
          check_range(lo, hi, g.slots[base.slot].len, "range", e.raw);
          const int64_t len = hi >= lo ? hi - lo + 1 : 0;
          const int64_t offset = len ? lo - 1 : 0;
          return with_layout(
              emit_value(OP_SLICE, {base}, len, view_of(e.type_),
                         {checked_immediate(offset, "range offset")}),
              contiguous_layout(base, offset, "range"));
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
          SlotInfo si = array_view({(int64_t)idata.size()}, ViewKind::Flat);
          return with_layout(
              emit_value(OP_GATHER, {base}, (int64_t)idata.size(), si, idata),
              owning_layout(si));
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
          return with_layout(
              emit_value(OP_GATHER, {base}, (int64_t)idata.size(),
                         view_of(e.type_), idata),
              ExpressionLayout::scalar());
        }
        // Matrix row/column slices use the explicit logical view; physical
        // storage remains column-major even when either extent is zero.
        if (e.args.size() == 3 && is_matrix(base.si) &&
            e.args[1].name == "IndexSingle" && e.args[2].name == "IndexAll") {
          const int64_t i = eval_int(e.args[1].args[0]);
          check_index(i, base.si.rows, "matrix row", e.raw);
          return with_layout(
              emit_value(
                  OP_SLICE_STRIDED, {base}, base.si.cols, view_of(e.type_),
                  {checked_immediate(i - 1, "matrix row offset"),
                   checked_immediate(base.si.rows, "matrix row stride")}),
              ExpressionLayout::scalar());
        }
        if (e.args.size() == 3 && is_matrix(base.si) &&
            e.args[1].name == "IndexAll" && e.args[2].name == "IndexSingle") {
          const int64_t j = eval_int(e.args[2].args[0]);
          check_index(j, base.si.cols, "matrix column", e.raw);
          const int64_t offset = (j - 1) * base.si.rows;
          return with_layout(
              emit_value(OP_SLICE, {base}, base.si.rows, view_of(e.type_),
                         {checked_immediate(offset, "matrix column offset")}),
              contiguous_layout(base, offset, "matrix column"));
        }
        // Column of a canonical graph-order 2-D array (array[N, S] real):
        // each outer element is contiguous, so successive rows sit S apart.
        if (e.args.size() == 3 && is_array(base.si) && bdims &&
            array_shape(base.si).leaf == ViewKind::Flat && bdims->size() == 2 &&
            e.args[1].name == "IndexAll" && e.args[2].name == "IndexSingle") {
          const int64_t k = eval_int(e.args[2].args[0]) - 1;
          const int64_t N = (*bdims)[0], S = (*bdims)[1];
          if (k < 0 || k >= S) fail("array column out of bounds", e.raw);
          return with_layout(
              emit_value(OP_SLICE_STRIDED, {base}, N,
                         array_view({N}, ViewKind::Flat),
                         {checked_immediate(k, "array column offset"),
                          checked_immediate(S, "array column stride")}),
              ExpressionLayout::scalar());
        }
        // Row range of the same layout: A[i, lo:hi] is contiguous.
        if (e.args.size() == 3 && is_array(base.si) && bdims &&
            (array_shape(base.si).leaf == ViewKind::Flat ||
             array_shape(base.si).leaf == ViewKind::Vector ||
             array_shape(base.si).leaf == ViewKind::RowVector) &&
            bdims->size() == 2 && e.args[1].name == "IndexSingle" &&
            e.args[2].name == "IndexBetween") {
          const int64_t i = eval_int(e.args[1].args[0]);
          const int64_t lo = eval_int(e.args[2].args[0]);
          const int64_t hi = eval_int(e.args[2].args[1]);
          const int64_t S = (*bdims)[1];
          check_index(i, (*bdims)[0], "array index", e.raw);
          check_range(lo, hi, S, "array range", e.raw);
          const int64_t len = hi >= lo ? hi - lo + 1 : 0;
          SlotInfo si = array_shape(base.si).leaf == ViewKind::Flat
                            ? array_view({len}, ViewKind::Flat)
                            : view_of(e.type_);
          const int64_t offset = len ? (i - 1) * S + lo - 1 : 0;
          const ExpressionLayout layout =
              array_shape(base.si).leaf == ViewKind::Flat
                  ? owning_layout(si)
                  : ExpressionLayout::direct(len ? lo - 1 : 0);
          return with_layout(
              emit_value(OP_SLICE, {base}, len, si,
                         {checked_immediate(offset, "array row range offset")}),
              layout);
        }
        // A whole vector leaf selected from array[N] vector[S]. The explicit
        // trailing All survives O1 for this spelling and addresses the same
        // contiguous outer-element block as the range directly above.
        if (e.args.size() == 3 && is_array(base.si) && bdims &&
            (array_shape(base.si).leaf == ViewKind::Vector ||
             array_shape(base.si).leaf == ViewKind::RowVector) &&
            bdims->size() == 2 && e.args[1].name == "IndexSingle" &&
            e.args[2].name == "IndexAll") {
          const int64_t i = eval_int(e.args[1].args[0]);
          const int64_t count = (*bdims)[0], width = (*bdims)[1];
          check_index(i, count, "array index", e.raw);
          const int64_t offset = (i - 1) * width;
          SlotInfo si = view_of(e.type_);
          return with_layout(
              emit_value(OP_SLICE, {base}, width, si,
                         {checked_immediate(offset, "array vector offset")}),
              owning_layout(si));
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
          const int64_t offset = len ? (j - 1) * base.si.rows + lo - 1 : 0;
          return with_layout(
              emit_value(
                  OP_SLICE, {base}, len, view_of(e.type_),
                  {checked_immediate(offset, "matrix row range offset")}),
              contiguous_layout(base, offset, "matrix row range"));
        }
        // Any two-axis matrix selection the slices above leave is the
        // Cartesian selection M[rows, cols], not a pairwise zip. Preserve
        // index-array order and duplicates; column-major output means
        // selected columns are outer and selected rows are inner in the flat
        // gather list.
        const auto is_matrix_selector = [](const mir::Expr& index) {
          return index.name == "IndexAll" || index.name == "IndexSingle" ||
                 index.name == "IndexBetween" || index.name == "IndexMulti";
        };
        if (e.args.size() == 3 && is_matrix(base.si) &&
            is_matrix_selector(e.args[1]) && is_matrix_selector(e.args[2]) &&
            (e.args[1].name != "IndexSingle" ||
             e.args[2].name != "IndexSingle")) {
          const std::vector<int64_t> rows = index_positions(
              e.args[1], base.si.rows, "matrix row gather", e.raw);
          const std::vector<int64_t> cols = index_positions(
              e.args[2], base.si.cols, "matrix column gather", e.raw);
          std::vector<int> gather;
          gather.reserve(rows.size() * cols.size());
          for (int64_t j : cols)
            for (int64_t i : rows)
              gather.push_back(checked_immediate(j * base.si.rows + i,
                                                 "matrix gather offset"));
          SlotInfo si = view_of(e.type_);
          si.param_free = base.si.param_free;
          if (e.type_ == "UMatrix")
            si = matrix_view((int64_t)rows.size(), (int64_t)cols.size(),
                             base.si.param_free);
          return with_layout(
              emit_value(OP_GATHER, {base},
                         (int64_t)rows.size() * (int64_t)cols.size(), si,
                         gather),
              ExpressionLayout::scalar());
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
            return with_layout(
                emit_value(OP_SLICE_STRIDED, {base}, a.len,
                           indexed_view(base.si, n_idx, a.len, e.type_),
                           {checked_immediate(a.off, "indexed offset"),
                            checked_immediate(a.stride, "indexed stride")}),
                ExpressionLayout::scalar());
          if (a.len == 1)
            return with_layout(
                emit_value(OP_INDEX, {base}, 1,
                           indexed_view(base.si, n_idx, 1, e.type_),
                           {checked_immediate(a.off, "indexed offset")}),
                ExpressionLayout::scalar());
          // One whole matrix out of the array keeps its shape, so a later
          // index on it can take the column-major paths above.
          SlotInfo si = indexed_view(base.si, n_idx, a.len, e.type_);
          return with_layout(
              emit_value(OP_SLICE, {base}, a.len, si,
                         {checked_immediate(a.off, "indexed offset")}),
              owning_layout(si));
        }
        // A full array-index prefix pins one vector/row_vector leaf element;
        // exactly one trailing range/all index then reads inside that leaf.
        // The prefix is not all-single-index in stanc's own sense (the trailing
        // index is a range), so this falls outside the block above even
        // though every array position is fixed. Graph storage keeps the
        // pinned leaf contiguous, so this is one contiguous read from its
        // start once flat_addr locates it.
        if (bdims && (array_shape(base.si).leaf == ViewKind::Vector ||
                      array_shape(base.si).leaf == ViewKind::RowVector)) {
          const size_t n_arr = bdims->size() - 1;
          const mir::Expr& last = e.args.back();
          bool prefix_single =
              e.args.size() == n_arr + 2 &&
              (last.name == "IndexBetween" || last.name == "IndexAll");
          for (size_t d = 0; prefix_single && d < n_arr; ++d)
            if (e.args[1 + d].name != "IndexSingle") prefix_single = false;
          if (prefix_single) {
            std::vector<int64_t> ix;
            ix.reserve(n_arr);
            for (size_t d = 0; d < n_arr; ++d) {
              const int64_t one = eval_int(e.args[1 + d].args[0]);
              check_index(one, (*bdims)[d], "array index", e.raw);
              ix.push_back(one - 1);
            }
            const Addr a = flat_addr(*bdims, false, ix);
            int64_t lo = 1, hi = a.len;
            if (last.name == "IndexBetween") {
              lo = eval_int(last.args[0]);
              hi = eval_int(last.args[1]);
              check_range(lo, hi, a.len, "array leaf range", e.raw);
            }
            const int64_t len = hi >= lo ? hi - lo + 1 : 0;
            SlotInfo si = view_of(e.type_);
            const int64_t offset = len ? a.off + lo - 1 : a.off;
            const ExpressionLayout layout =
                last.name == "IndexBetween"
                    ? ExpressionLayout::direct(len ? lo - 1 : 0)
                    : owning_layout(si);
            return with_layout(
                emit_value(
                    OP_SLICE, {base}, len, si,
                    {checked_immediate(offset, "array vector slice offset")}),
                layout);
          }
        }
        // A single outer-array range kept in full, with fixed row/column
        // indices into every element's matrix: array[N] matrix[R, C][:, i,
        // j]. Graph storage keeps each matrix contiguous and array-major, so
        // this is a strided read of one scalar out of every element.
        if (e.args.size() == 4 && is_array(base.si) && bdims &&
            bdims->size() == 3 &&
            array_shape(base.si).leaf == ViewKind::Matrix &&
            e.args[1].name == "IndexAll" && e.args[2].name == "IndexSingle" &&
            e.args[3].name == "IndexSingle") {
          const int64_t N = (*bdims)[0], R = (*bdims)[1], C = (*bdims)[2];
          const int64_t ri = eval_int(e.args[2].args[0]);
          const int64_t cj = eval_int(e.args[3].args[0]);
          check_index(ri, R, "matrix row", e.raw);
          check_index(cj, C, "matrix column", e.raw);
          const int64_t off = (cj - 1) * R + (ri - 1);
          SlotInfo si = array_view({N}, ViewKind::Flat, base.si.param_free);
          return with_layout(
              emit_value(
                  OP_SLICE_STRIDED, {base}, N, si,
                  {checked_immediate(off, "matrix array cell offset"),
                   checked_immediate(R * C, "matrix array cell stride")}),
              owning_layout(si));
        }
        // Row of a column-major data matrix / 2-D array: strided slice.
        if (all_single && e.args.size() == 2 && is_matrix(base.si) &&
            e.type_ != "UReal" && e.type_ != "UInt") {
          const int64_t t = eval_int(e.args[1].args[0]);
          check_index(t, base.si.rows, "matrix row", e.raw);
          return with_layout(
              emit_value(
                  OP_SLICE_STRIDED, {base}, base.si.cols, view_of(e.type_),
                  {checked_immediate(t - 1, "matrix row offset"),
                   checked_immediate(base.si.rows, "matrix row stride")}),
              ExpressionLayout::scalar());
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
        return with_layout(
            emit_value(OP_INDEX, {base}, 1, view_of(e.type_),
                       {checked_immediate(flat, "index offset")}),
            ExpressionLayout::scalar());
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
        if (expr_effectful(e.args[0]))
          fail("effectful expression cannot be a compile-time condition",
               e.raw);
        // Shape specialization and ordinary data evaluation can decide a
        // condition even when the complete expression's MIR adlevel is not
        // DataOnly (for example `rows(x) == 0 || theta > 0`).  Only the
        // genuinely unresolved case needs runtime control.
        if (auto condition = try_eval_pure(e.args[0]))
          return lower_expr(e.args[condition->r.at(0) != 0.0 ? 1 : 2]);
        return lower_runtime_ternary(e);
      }
      case mir::Expr::EOr:
      case mir::Expr::EAnd: {
        if (auto v = fold_const(e)) return *v;
        if (runtime_only(e)) return lower_runtime_ternary(e);
        fail("boolean operator on parameters unsupported", e.raw);
      }
      default: {
        if (auto v = fold_const(e)) return *v;
        fail("unsupported expression", e.raw.empty() ? e.name : e.raw);
      }
    }
  }

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

  static bool expr_has_jacobian(const mir::Expr& e) {
    if (e.kind == mir::Expr::FunApp) {
      CallableTransformSpec transform;
      if (callable_transform(e.name, &transform) &&
          transform.direction == TransformDirection::Jacobian)
        return true;
      // Stan permits Jacobian adjustments in a UDF precisely when its name
      // has this suffix. Conservatively carry a target through such a call;
      // an unused zero is cheaper than dropping a nested adjustment.
      if (e.fn_lib == mir::Expr::Lib::UserDefined &&
          transform_suffix(e.name, "_jacobian"))
        return true;
    }
    for (const auto& a : e.args)
      if (expr_has_jacobian(a)) return true;
    return false;
  }

  // Does `s` increment the target, explicitly or through a Jacobian call?
  static bool has_target_pe(const mir::Stmt& s) {
    if (s.kind == mir::Stmt::TargetPE) return true;
    if ((s.has_init && expr_has_jacobian(s.init)) || expr_has_jacobian(s.rhs) ||
        expr_has_jacobian(s.target) || expr_has_jacobian(s.lower) ||
        expr_has_jacobian(s.upper) || expr_has_jacobian(s.cond))
      return true;
    for (const auto& e : s.fn_args)
      if (expr_has_jacobian(e)) return true;
    for (const auto& e : s.lhs_idx)
      if (expr_has_jacobian(e)) return true;
    for (const auto& k : s.body)
      if (has_target_pe(k)) return true;
    return false;
  }

  bool needs_runtime_control(const mir::Stmt& s) {
    // A structured while owns every runtime decision in its body.  Promoting
    // its enclosing block would absorb UDF-local declarations and returns,
    // which are not live-outs of that outer region.
    if (s.kind == mir::Stmt::While) return false;
    if (s.kind == mir::Stmt::Block || s.kind == mir::Stmt::SList) {
      // This scan runs before the block is lowered, but loop bounds later in
      // the block can depend on scalar-int locals established by earlier
      // statements.  Mirror just that compile-time environment in statement
      // order.  In particular, stanc spells `int d = rows(x)` as a default
      // declaration followed by an assignment, and UDFs commonly use d to
      // size locals and loops.  Looking through the whole block without this
      // lexical state rejects an otherwise static write-array UDF.
      const auto saved = int_env;
      std::set<std::string> local_ints;
      bool found = false;
      try {
        for (const auto& child : s.body) {
          if (needs_runtime_control(child)) {
            found = true;
            break;
          }
          if (child.kind == mir::Stmt::Decl && child.decl_type.base == "SInt") {
            local_ints.insert(child.decl_id);
            int_env.erase(child.decl_id);
            if (child.has_init) int_env[child.decl_id] = eval_int(child.init);
          } else if (child.kind == mir::Stmt::Assignment &&
                     child.lhs_idx.empty() && local_ints.count(child.lhs)) {
            int_env[child.lhs] = eval_int(child.rhs);
          }
        }
      } catch (...) {
        int_env = saved;
        throw;
      }
      int_env = saved;
      return found;
    }
    if (s.kind == mir::Stmt::IfElse) {
      // This is a speculative write_array scan, so follow an already-known
      // arm exactly as ordinary lowering will.  Besides avoiding needless
      // work, this preserves Stan's reachability semantics for invalid shape
      // selectors in a dead statement arm.
      if (auto evaluated = try_eval_pure(s.cond)) {
        const size_t arm = evaluated->r.at(0) != 0.0 ? 0 : 1;
        return arm < s.body.size() && needs_runtime_control(s.body[arm]);
      }
      if (s.cond.data_only) return true;
    }
    if (s.kind == mir::Stmt::For) {
      const long lo = eval_int(s.lower), hi = eval_int(s.upper);
      if (lo > hi) return false;
      const auto old = int_env.find(s.loopvar);
      const bool had_old = old != int_env.end();
      const long old_value = had_old ? old->second : 0;
      bool found = false;
      // Scan under the same compile-time loop bindings ordinary lowering
      // will use. This keeps static conditions such as `if (t < N)` out of
      // a region without overlooking an arm that exists only at a later t.
      for (long v = lo; v <= hi && !found; ++v) {
        int_env[s.loopvar] = v;
        for (const auto& k : s.body)
          if (needs_runtime_control(k)) {
            found = true;
            break;
          }
      }
      if (had_old)
        int_env[s.loopvar] = old_value;
      else
        int_env.erase(s.loopvar);
      return found;
    }
    for (const auto& k : s.body)
      if (needs_runtime_control(k)) return true;
    return false;
  }

  // A Break/Continue selected by a runtime condition cannot be lowered as a
  // standalone conditional island: its jump target belongs to the enclosing
  // loop. Promote that whole loop to the necessity island instead. Nested
  // loops own their own control statements and therefore stop this search.
  bool runtime_loop_control(const mir::Stmt& s, bool runtime_path = false) {
    if (s.kind == mir::Stmt::Break || s.kind == mir::Stmt::Continue)
      return runtime_path;
    if (s.kind == mir::Stmt::For || s.kind == mir::Stmt::While) return false;
    if (s.kind == mir::Stmt::IfElse) {
      if (auto evaluated = try_eval_pure(s.cond)) {
        const bool take_then = evaluated->r.at(0) != 0.0;
        if (take_then && !s.body.empty())
          return runtime_loop_control(s.body[0], runtime_path);
        if (!take_then && s.body.size() > 1)
          return runtime_loop_control(s.body[1], runtime_path);
        return false;
      }
      for (const auto& arm : s.body)
        if (runtime_loop_control(arm, true)) return true;
      return false;
    }
    for (const auto& child : s.body)
      if (runtime_loop_control(child, runtime_path)) return true;
    return false;
  }

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
  static bool peel_terminal_return(mir::Stmt* s, mir::Expr* value) {
    if (s->kind == mir::Stmt::Return) {
      if (!s->has_init) return false;
      *value = s->rhs;
      s->kind = mir::Stmt::Skip;
      s->body.clear();
      return true;
    }
    if ((s->kind == mir::Stmt::Block || s->kind == mir::Stmt::SList) &&
        !s->body.empty())
      return peel_terminal_return(&s->body.back(), value);
    return false;
  }

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
                               int* parameter_count) {
    std::vector<Range> active = pack_callback_arguments<Range>(
        spec, e.args, begin, end,
        [&](size_t i) {
          Range value = c.expr(e.args[i]);
          return std::make_pair(value, value.len);
        },
        [&](size_t i) {
          DataMap::Entry value =
              program_constant(c, e.args[i], e.name + " data argument");
          if (value.is_int)
            c.bail(e.name + ": real data argument is integer-valued");
          const bool matrix = e.args[i].type_ == "UMatrix";
          const bool nested_matrix =
              e.args[i].unsized.depth != 0 &&
              e.args[i].unsized.leaf == mir::UnsizedLeaf::Matrix;
          return graph_order(value, matrix, nested_matrix);
        },
        [&](size_t i) {
          DataMap::Entry value =
              program_constant(c, e.args[i], e.name + " integer argument");
          if (!value.is_int)
            c.bail(e.name + ": integer argument is real-valued");
          return value.i;
        },
        [&](const std::string& message) { c.bail(e.name + ": " + message); });
    int total = 0;
    for (const Range& value : active) {
      if (value.len > ProgramCompiler::kMaxRegs - total)
        c.bail(e.name + ": active callback arguments are too large");
      total += value.len;
    }
    *parameter_count = total;
    Range theta{total == 0 ? c.konst(0.0) : c.alloc(total),
                total == 0 ? 1 : total};
    theta.kind = ViewKind::Vector;
    int at = 0;
    for (const Range& value : active)
      for (int k = 0; k < value.len; ++k)
        c.emit(Program::MOV, theta.reg + at++, value.reg + k);
    return theta;
  }

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
                                      Range* out_range) {
    const auto call = mir::algebra_call(e.name);
    if (!call || call->legacy) return false;
    if (e.args.size() < call->callback_args_begin ||
        e.args[0].kind != mir::Expr::Var)
      c.bail(e.name + ": expected a callback and an initial guess");
    if (e.unsized.depth != 0 || e.unsized.leaf != mir::UnsizedLeaf::Vector)
      c.bail(e.name + ": result must be a vector");

    std::vector<mir::UnsizedView> views{{0, mir::UnsizedLeaf::Vector}};
    for (size_t i = call->callback_args_begin; i < e.args.size(); ++i)
      views.push_back(e.args[i].unsized);
    const mir::FunDef* system =
        mir::resolve_callback(fun_defs, e.args[0].name, views);
    if (system == nullptr)
      c.bail(e.name + ": unknown algebraic system " + e.args[0].name);

    auto spec = std::make_shared<AlgebraSpec>();
    spec->adopt(fun_defs);
    spec->system_name = system->name;
    spec->select(*call);
    if (call->with_tolerance) {
      spec->relative_tolerance =
          program_scalar_real(c, e.args[2], e.name + " relative tolerance");
      spec->function_tolerance =
          program_scalar_real(c, e.args[3], e.name + " function tolerance");
      spec->max_num_steps =
          program_scalar_int(c, e.args[4], e.name + " maximum steps");
    }

    int parameter_count = 0;
    const Range theta =
        program_callback_theta(c, e, call->callback_args_begin, e.args.size(),
                               *spec, &parameter_count);
    const Range x = c.expr(e.args[1]);
    if (x.kind != ViewKind::Vector)
      c.bail(e.name + ": initial guess must be a vector");
    spec->prog = compile_rhs_args(with_leading_time(*spec->system()),
                                  *spec->funs(), x.len, spec->args);

    Range result{0, x.len};
    result.kind = ViewKind::Vector;
    *out_range =
        c.kernel_call(OP_ALGEBRA_SOLVER, {x, theta}, result,
                      parameter_count == 0 ? 0u : 0x1u, 0x2u, {}, spec, e.name);
    return true;
  }

  bool lower_program_quadrature(ProgramCompiler& c, const mir::Expr& e,
                                Range* out_range) {
    const auto call = mir::quadrature_call(e.name);
    if (!call) return false;
    if (e.unsized.depth != 0 || e.unsized.leaf != mir::UnsizedLeaf::Real)
      c.bail(e.name + ": result must be a real");

    size_t callback_end = e.args.size();
    if (call->legacy) {
      if (e.args.size() != 6 && e.args.size() != 7)
        c.bail(e.name + ": expected 6 or 7 arguments");
      callback_end = 6;
    } else if (call->with_tolerance) {
      if (e.args.size() < 6)
        c.bail(e.name + ": expected controls followed by callback arguments");
    } else if (e.args.size() < 3) {
      c.bail(e.name + ": expected callback and integration bounds");
    }
    if (e.args[0].kind != mir::Expr::Var)
      c.bail(e.name + ": integrand is not a function name");

    std::vector<mir::UnsizedView> views{{0, mir::UnsizedLeaf::Real},
                                        {0, mir::UnsizedLeaf::Real}};
    for (size_t i = call->callback_args_begin; i < callback_end; ++i)
      views.push_back(e.args[i].unsized);
    const mir::FunDef* integrand =
        mir::resolve_callback(fun_defs, e.args[0].name, views);
    if (!integrand) c.bail(e.name + ": unknown integrand " + e.args[0].name);

    auto spec = std::make_shared<QuadratureSpec>();
    spec->adopt(fun_defs);
    spec->callback_name = integrand->name;
    spec->method = call->method;
    if (call->legacy && e.args.size() == 7) {
      spec->relative_tolerance =
          program_scalar_real(c, e.args[6], "quadrature tolerance");
    } else if (call->with_tolerance) {
      spec->relative_tolerance =
          program_scalar_real(c, e.args[3], "quadrature relative tolerance");
      spec->absolute_tolerance =
          program_scalar_real(c, e.args[4], "quadrature absolute tolerance");
      spec->max_steps = static_cast<int>(
          program_scalar_int(c, e.args[5], "quadrature maximum steps"));
    }

    const Range theta =
        program_callback_theta(c, e, call->callback_args_begin, callback_end,
                               *spec, &spec->parameter_count);
    spec->prog =
        compile_rhs_args(*spec->callback(), *spec->funs(), 1, spec->args);

    const Range a = c.expr(e.args[1]);
    const Range b = c.expr(e.args[2]);
    if (!c.is_scalar(a) || !c.is_scalar(b))
      c.bail(e.name + ": integration bounds must be scalar");
    const uint8_t variant =
        static_cast<uint8_t>((!e.args[1].data_only ? 0x1u : 0u) |
                             (!e.args[2].data_only ? 0x2u : 0u) |
                             (spec->parameter_count != 0 ? 0x4u : 0u));
    Range result{0, 1};
    *out_range = c.kernel_call(OP_QUADRATURE, {a, b, theta}, result, variant,
                               variant, {}, spec, e.name);
    return true;
  }

  bool lower_program_ode(ProgramCompiler& c, const mir::Expr& e,
                         Range* out_range) {
    const auto call = mir::ode_call(e.name);
    if (!call || call->method == mir::OdeMethod::Adjoint) return false;
    if (e.args.size() < call->callback_args_begin ||
        e.args[0].kind != mir::Expr::Var)
      c.bail(e.name + ": expected a right-hand side, state, and times");

    auto spec = std::make_shared<OdeSpec>();
    spec->adopt(fun_defs);
    spec->rhs_name = e.args[0].name;
    if (!spec->rhs())
      c.bail(e.name + ": unknown right-hand side " + spec->rhs_name);
    spec->solver = ode_solver(call->method);
    spec->legacy = call->legacy;
    spec->stiff =
        spec->solver == OdeSpec::BDF || spec->solver == OdeSpec::ADAMS;
    stamp_ode_defaults(*spec);
    const Range z0 = c.expr(e.args[1]);
    if ((!call->legacy && z0.kind != ViewKind::Vector) ||
        (call->legacy && z0.kind != ViewKind::Array))
      c.bail(e.name + ": initial state has the wrong logical type");
    const int S = z0.len;
    Range t0{0, 1}, ts;
    if (call->legacy) {
      spec->t0 = program_scalar_real(c, e.args[2], "ODE initial time");
      spec->ts = program_vector_real(c, e.args[3], "ODE output times");
    } else {
      t0 = c.expr(e.args[2]);
      ts = c.expr(e.args[3]);
      if (!c.is_scalar(t0)) c.bail("ODE initial time must be scalar");
      if (ts.kind != ViewKind::Array)
        c.bail("ODE output times must be an array");
      spec->ts.resize((size_t)ts.len);
    }
    const int64_t N = call->legacy ? (int64_t)spec->ts.size() : ts.len;
    if (S < 0 || N < 0 || (N && S > ProgramCompiler::kMaxRegs / N))
      c.bail(e.name + ": result is too large");

    Range theta;
    bool theta_active = false;
    if (call->legacy) {
      if (e.args.size() != 7 && e.args.size() != 10)
        c.bail(e.name + ": expected 7 or 10 arguments");
      theta = c.expr(e.args[4]);
      if (theta.kind != ViewKind::Array)
        c.bail(e.name + ": parameters are not an array");
      theta_active = !e.args[4].data_only;
      spec->x_r = program_vector_real(c, e.args[5], "ODE real data");
      DataMap::Entry xi = program_constant(c, e.args[6], "ODE integer data");
      if (!xi.is_int) c.bail(e.name + ": integer data is real-valued");
      spec->x_i.assign(xi.i.begin(), xi.i.end());
      if (e.args.size() == 10) {
        spec->rtol =
            program_scalar_real(c, e.args[7], "ODE relative tolerance");
        spec->atol =
            program_scalar_real(c, e.args[8], "ODE absolute tolerance");
        spec->max_steps = program_scalar_int(c, e.args[9], "ODE maximum steps");
      }
      spec->args.resize(3);
      spec->args[0].is_param = true;
      spec->args[0].len = theta.len;
      spec->args[1].len = (int)spec->x_r.size();
      spec->args[2].is_int = true;
      spec->args[2].ints = spec->x_i;
      spec->prog = compile_rhs(*spec->rhs(), *spec->funs(), S, theta.len,
                               (int)spec->x_r.size(), spec->x_i);
    } else {
      if (call->with_tolerance) {
        spec->rtol =
            program_scalar_real(c, e.args[4], "ODE relative tolerance");
        spec->atol =
            program_scalar_real(c, e.args[5], "ODE absolute tolerance");
        spec->max_steps = program_scalar_int(c, e.args[6], "ODE maximum steps");
      }
      int parameter_count = 0;
      theta = program_callback_theta(c, e, call->callback_args_begin,
                                     e.args.size(), *spec, &parameter_count);
      theta_active = parameter_count != 0;
      spec->prog = compile_rhs_args(*spec->rhs(), *spec->funs(), S, spec->args);
    }

    Range result{0, (int)(N * S)};
    result.kind = ViewKind::Array;
    result.dims = {N, S};
    result.leaf = call->legacy ? ViewKind::Flat : ViewKind::Vector;
    const uint8_t activity = static_cast<uint8_t>(
        (e.args[1].data_only ? 0u : 0x1u) | (theta_active ? 0x2u : 0u) |
        (!call->legacy && !e.args[2].data_only ? 0x4u : 0u) |
        (!call->legacy && !e.args[3].data_only ? 0x8u : 0u));
    if (call->legacy) {
      *out_range = c.kernel_call(OP_ODE, {z0, theta}, result,
                                 static_cast<uint8_t>(0x4u | activity),
                                 activity, {(int)N, S}, spec, e.name);
    } else {
      *out_range = c.kernel_call(OP_ODE, {z0, theta, t0, ts}, result,
                                 static_cast<uint8_t>(0x10u | activity),
                                 activity, {(int)N, S}, spec, e.name);
    }
    return true;
  }

  bool lower_program_ode_adjoint(ProgramCompiler& c, const mir::Expr& e,
                                 Range* out_range) {
    const auto call = mir::ode_call(e.name);
    if (!call || call->method != mir::OdeMethod::Adjoint) return false;
    if (e.args.size() < call->callback_args_begin ||
        e.args[0].kind != mir::Expr::Var)
      c.bail(e.name + ": expected a right-hand side and solver controls");

    auto spec = std::make_shared<OdeAdjointSpec>();
    spec->adopt(fun_defs);
    spec->rhs_name = e.args[0].name;
    spec->callback_name = spec->rhs_name;
    if (!spec->rhs())
      c.bail(e.name + ": unknown right-hand side " + spec->rhs_name);
    const auto real = [&](size_t i, const char* role) {
      return program_scalar_real(c, e.args[i],
                                 std::string("adjoint ODE ") + role);
    };
    const auto reals = [&](size_t i, const char* role) {
      return program_vector_real(c, e.args[i],
                                 std::string("adjoint ODE ") + role);
    };
    const auto integer = [&](size_t i, const char* role) {
      return program_scalar_int(c, e.args[i],
                                std::string("adjoint ODE ") + role);
    };
    spec->relative_tolerance_forward = real(4, "forward relative tolerance");
    spec->absolute_tolerance_forward = reals(5, "forward absolute tolerance");
    spec->relative_tolerance_backward = real(6, "backward relative tolerance");
    spec->absolute_tolerance_backward = reals(7, "backward absolute tolerance");
    spec->relative_tolerance_quadrature =
        real(8, "quadrature relative tolerance");
    spec->absolute_tolerance_quadrature =
        real(9, "quadrature absolute tolerance");
    spec->max_num_steps = integer(10, "maximum steps");
    spec->num_steps_between_checkpoints = integer(11, "checkpoint interval");
    spec->interpolation_polynomial =
        (int)integer(12, "interpolation polynomial");
    spec->solver_forward = (int)integer(13, "forward solver");
    spec->solver_backward = (int)integer(14, "backward solver");

    const Range y0 = c.expr(e.args[1]);
    const Range t0 = c.expr(e.args[2]);
    const Range ts = c.expr(e.args[3]);
    if (y0.kind != ViewKind::Vector)
      c.bail(e.name + ": initial state must be a vector");
    if (!c.is_scalar(t0)) c.bail(e.name + ": initial time must be scalar");
    if (ts.kind != ViewKind::Array)
      c.bail(e.name + ": output times must be an array");
    const int S = y0.len;
    const int N = ts.len;
    if ((int)spec->absolute_tolerance_forward.size() != S ||
        (int)spec->absolute_tolerance_backward.size() != S)
      c.bail(e.name + ": absolute tolerance vectors must match state size");
    if (S < 0 || N < 0 || (N && S > ProgramCompiler::kMaxRegs / N))
      c.bail(e.name + ": result is too large");

    int parameter_count = 0;
    const Range theta =
        program_callback_theta(c, e, call->callback_args_begin, e.args.size(),
                               *spec, &parameter_count);
    spec->prog = compile_rhs_args(*spec->rhs(), *spec->funs(), S, spec->args);

    Range result{0, N * S};
    result.kind = ViewKind::Array;
    result.dims = {N, S};
    result.leaf = ViewKind::Vector;
    const uint8_t activity =
        static_cast<uint8_t>((!e.args[1].data_only ? 0x1u : 0u) |
                             (!e.args[2].data_only ? 0x2u : 0u) |
                             (!e.args[3].data_only ? 0x4u : 0u) |
                             (parameter_count != 0 ? 0x8u : 0u));
    *out_range = c.kernel_call(OP_ODE_ADJOINT, {y0, t0, ts, theta}, result,
                               static_cast<uint8_t>(0x10u | activity), activity,
                               {N, S}, spec, e.name);
    return true;
  }

  bool lower_program_dae(ProgramCompiler& c, const mir::Expr& e,
                         Range* out_range) {
    const auto call = mir::dae_call(e.name);
    if (!call) return false;
    if (e.args.size() < call->callback_args_begin ||
        e.args[0].kind != mir::Expr::Var)
      c.bail(e.name + ": expected a residual, initial conditions, and times");

    auto spec = std::make_shared<DaeSpec>();
    spec->adopt(fun_defs);
    spec->residual_name = e.args[0].name;
    spec->callback_name = spec->residual_name;
    if (!spec->residual())
      c.bail(e.name + ": unknown residual " + spec->residual_name);
    spec->t0 = program_scalar_real(c, e.args[3], "DAE initial time");
    spec->ts = program_vector_real(c, e.args[4], "DAE output times");
    if (call->with_tolerance) {
      spec->rtol = program_scalar_real(c, e.args[5], "DAE relative tolerance");
      spec->atol = program_scalar_real(c, e.args[6], "DAE absolute tolerance");
      spec->max_steps = program_scalar_int(c, e.args[7], "DAE maximum steps");
    }

    const Range y0 = c.expr(e.args[1]);
    const Range yp0 = c.expr(e.args[2]);
    if (y0.kind != ViewKind::Vector || yp0.kind != ViewKind::Vector)
      c.bail(e.name + ": initial state and derivative must be vectors");
    if (y0.len != yp0.len)
      c.bail(e.name + ": initial state and derivative sizes differ");
    const int S = y0.len;
    const int64_t N = (int64_t)spec->ts.size();
    if (S < 0 || N < 0 || (N && S > ProgramCompiler::kMaxRegs / N))
      c.bail(e.name + ": result is too large");

    int parameter_count = 0;
    const Range theta =
        program_callback_theta(c, e, call->callback_args_begin, e.args.size(),
                               *spec, &parameter_count);
    spec->prog =
        compile_dae_args(*spec->residual(), *spec->funs(), S, spec->args);

    Range result{0, (int)(N * S)};
    result.kind = ViewKind::Array;
    result.dims = {N, S};
    result.leaf = ViewKind::Vector;
    const uint8_t activity =
        static_cast<uint8_t>((!e.args[1].data_only ? 0x1u : 0u) |
                             (!e.args[2].data_only ? 0x2u : 0u) |
                             (parameter_count != 0 ? 0x4u : 0u));
    *out_range = c.kernel_call(OP_DAE, {y0, yp0, theta}, result,
                               static_cast<uint8_t>(0x8u | activity), activity,
                               {(int)N, S}, spec, e.name);
    return true;
  }

  bool lower_program_higher_order(ProgramCompiler& c, const mir::Expr& e,
                                  Range* out_range) {
    const auto higher_order = mir::higher_order_call(e);
    if (!higher_order) return false;
    if (higher_order->family == mir::HigherOrderFamily::Integrate1D)
      return lower_program_quadrature(c, e, out_range);
    if (higher_order->family == mir::HigherOrderFamily::Ode)
      return lower_program_ode_adjoint(c, e, out_range) ||
             lower_program_ode(c, e, out_range);
    if (higher_order->family == mir::HigherOrderFamily::Dae)
      return lower_program_dae(c, e, out_range);
    if (higher_order->family != mir::HigherOrderFamily::Algebra) return false;
    if (lower_program_variadic_algebra(c, e, out_range)) return true;
    if ((e.name != "algebra_solver" && e.name != "algebra_solver_newton"))
      return false;
    if (e.args.size() != 5 && e.args.size() != 8)
      c.bail("algebra_solver: expected 5 or 8 arguments");
    if (e.args[0].kind != mir::Expr::Var)
      c.bail("algebra_solver: system is not a function name");
    if (e.unsized.depth != 0 || e.unsized.leaf != mir::UnsizedLeaf::Vector)
      c.bail("algebra_solver: result must be a vector");

    const std::vector<mir::UnsizedView> views{{0, mir::UnsizedLeaf::Vector},
                                              {0, mir::UnsizedLeaf::Vector},
                                              {1, mir::UnsizedLeaf::Real},
                                              {1, mir::UnsizedLeaf::Int}};
    const mir::FunDef* system =
        mir::resolve_callback(fun_defs, e.args[0].name, views);
    if (system == nullptr)
      c.bail("algebra_solver: unknown algebraic system " + e.args[0].name);
    if (system->arg_names.size() != views.size())
      c.bail("algebra_solver: system argument metadata is incomplete");

    auto spec = std::make_shared<AlgebraSpec>();
    spec->adopt(fun_defs);
    spec->system_name = system->name;
    spec->select(*mir::algebra_call(e.name));
    spec->x_r = program_vector_real(c, e.args[3], "algebra_solver x_r");
    DataMap::Entry xi = program_constant(c, e.args[4], "algebra_solver x_i");
    if (!xi.is_int || xi.i.size() != xi.r.size())
      c.bail("algebra_solver: malformed integer data argument");
    spec->x_i.assign(xi.i.begin(), xi.i.end());
    if (e.args.size() == 8) {
      spec->relative_tolerance = program_scalar_real(
          c, e.args[5], "algebra_solver relative tolerance");
      spec->function_tolerance = program_scalar_real(
          c, e.args[6], "algebra_solver function tolerance");
      spec->max_num_steps =
          program_scalar_int(c, e.args[7], "algebra_solver maximum steps");
    }

    const Range x = c.expr(e.args[1]);
    const Range y = c.expr(e.args[2]);
    if (x.kind != ViewKind::Vector || y.kind != ViewKind::Vector)
      c.bail("algebra_solver: initial guess and parameters must be vectors");
    if (x.len < 0 || y.len < 0 ||
        spec->x_r.size() > (size_t)std::numeric_limits<int>::max())
      c.bail("algebra_solver: argument is too large");

    std::vector<RhsArg> args(3);
    args[0].is_param = true;
    args[0].len = y.len;
    args[1].len = (int)spec->x_r.size();
    args[2].is_int = true;
    args[2].ints = spec->x_i;
    spec->prog = compile_rhs_args(with_leading_time(*spec->system()),
                                  *spec->funs(), x.len, args);

    Range result{0, x.len};
    result.kind = ViewKind::Vector;
    const uint8_t active = e.args[2].data_only ? 0u : 0x1u;
    *out_range = c.kernel_call(OP_ALGEBRA_SOLVER, {x, y}, result, active, 0x2u,
                               {}, spec, e.name);
    return true;
  }

  // Compile `s` (a statement region) or `e` (a ternary) into a program.
  void lower_island(const mir::Stmt* s, const mir::Expr* e, IslandRegion* reg,
                    Range* expr_out, std::shared_ptr<IslandProg>* prog_out) {
    auto prog = std::make_shared<IslandProg>();
    ProgramCompiler c{*prog, fun_defs};
    c.in_write_array = in_write_array;
    // Non-returning statement calls may print or reject. A register program
    // would replay them during reverse mode, so ProgramCompiler refuses them
    // until necessity islands have an execute-once effect path.
    for (const auto& [name, v] : int_env) c.ints[name] = {v};
    // Data the region reads as a compile-time integer, answered by the
    // same interpreter that answers a size expression. The region has
    // already resolved the indices, so what arrives is a literal read of a
    // data-only value -- nothing here depends on the region's own scope.
    c.extern_int = [&](const mir::Expr& x, long* out) {
      if (!x.data_only) return false;
      auto evaluated = try_eval_pure(x);
      if (!evaluated || !evaluated->is_int || evaluated->i.size() != 1)
        return false;
      *out = evaluated->i[0];
      return true;
    };
    c.extern_ints = [&](const mir::Expr& x, std::vector<long>* values,
                        std::vector<int64_t>* dims) {
      if (!x.data_only || x.unsized.depth == 0 ||
          x.unsized.leaf != mir::UnsizedLeaf::Int)
        return false;
      auto evaluated = try_eval_pure(x);
      if (!evaluated || !evaluated->is_int ||
          evaluated->i.size() != evaluated->r.size())
        return false;
      values->assign(evaluated->i.begin(), evaluated->i.end());
      *dims = evaluated->dims;
      return true;
    };
    c.extern_real = [&](const mir::Expr& x, double* value) {
      if (x.type_ != "UReal") return false;
      auto evaluated = try_eval_pure(x);
      if (!evaluated || evaluated->is_int || evaluated->r.size() != 1)
        return false;
      *value = evaluated->r[0];
      return true;
    };
    c.lower_higher_order = [&](const mir::Expr& x, Range* result) {
      return lower_program_higher_order(c, x, result);
    };
    if (!in_write_array) {
      c.bind_target = [&](Range* r) {
        const int slot = current_target_slot();
        r->reg = c.alloc(1);
        r->len = 1;
        prog->ins.push_back(IslandProg::LiveIn{r->reg, 1});
        reg->in_slots.push_back(slot);
        return true;
      };
    }
    std::set<std::string> outer_names;
    for (const auto& [name, value] : scope) outer_names.insert(name);
    for (const auto& [name, value] : decls) outer_names.insert(name);
    const std::set<std::string> outer_int_names = int_locals;
    c.bind_extern = [&](const std::string& name, Range* r) {
      auto sc = scope.find(name);
      int slot = sc != scope.end() ? sc->second.slot : env_slot(name);
      if (slot < 0) slot = uninitialized_decl_slot(name);
      if (slot < 0) return false;
      const int64_t len = g.slots[slot].len;
      r->reg = c.alloc((int)len);
      r->len = (int)len;
      const SlotInfo& si = scope.at(name).si;
      r->rows = si.rows;
      r->cols = si.cols;
      r->kind = si.kind;
      if (is_array(si)) {
        const ArrayShape& arr = array_shape(si);
        r->dims = arr.dims;
        r->leaf = arr.leaf;
      }
      if (len > 0) {
        prog->ins.push_back(IslandProg::LiveIn{r->reg, (int)len});
        reg->in_slots.push_back(slot);
      }
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
          if (is_array(dl->second.si)) {
            const ArrayShape& arr = array_shape(dl->second.si);
            view.dims = arr.dims;
            view.leaf = arr.leaf;
          }
          const double fill =
              dl->second.int_array
                  ? static_cast<double>(std::numeric_limits<int>::min())
                  : std::numeric_limits<double>::quiet_NaN();
          c.declare(name, (int)dl->second.len, view, fill);
        }
        c.stmt(*s);
        std::vector<std::string> assigned;
        assigned_names(*s, &assigned);
        for (const std::string& name : assigned) {
          const bool is_outer_int = outer_int_names.count(name) != 0;
          if (!outer_names.count(name) && !is_outer_int) continue;
          auto it = c.reals.find(name);
          if (it == c.reals.end()) continue;
          reg->out_names.push_back(name);
          reg->out_is_int.push_back(is_outer_int);
          reg->out_views.push_back(it->second);
          for (int k = 0; k < it->second.len; ++k)
            prog->out_regs.push_back(it->second.reg + k);
        }
        if (has_target_pe(*s)) {
          reg->has_target = true;
          prog->out_regs.push_back(target_reg);
        }
        // An integer the region folded is one this lowering holds a copy
        // of, and the copy is a compile-time constant every later size,
        // index and read would keep using. The region compiler folds only
        // what certainly happens, so the value it ends with is the one
        // every path through the region leaves behind. Nothing carries an
        // integer out of the program itself: a live-out is a register, and
        // registers hold doubles.
        for (const std::string& name : assigned) {
          auto folded = c.ints.find(name);
          auto held = int_env.find(name);
          if (folded != c.ints.end() && folded->second.size() == 1 &&
              held != int_env.end())
            held->second = folded->second[0];
        }
      } else {
        *expr_out = c.expr(*e);
        for (int k = 0; k < expr_out->len; ++k)
          prog->out_regs.push_back(expr_out->reg + k);
      }
      c.finish();
    } catch (Bail& b) {
      fail("runtime-control region: " + b.why, s ? s->raw : e->raw);
    }
    // No live-out register is legitimate when the region found live-outs
    // and every one of them is zero-width: the data made the values empty,
    // as `matrix[0, 0]` from a dimension table does, so there is nothing
    // for the program to carry out. Finding no live-out at all is the
    // mistake this catches -- a region that lost what it was to produce --
    // unless the region's entire purpose was a conditional effect: that has
    // no data output by design, its value being the output or exception.
    reg->has_effect = island_has_effect(*prog);
    if (prog->out_regs.empty() && !(e && expr_out->len == 0) &&
        (s == nullptr || reg->out_names.empty()) && !reg->has_effect)
      fail("runtime-control region produces nothing", s ? s->raw : e->raw);
    // A region with a runtime branch keeps the var replay -- reversing
    // control flow needs the structured form the flat program has already
    // lost -- so this usually declines. It is asked anyway because a region
    // can reach here branch-free: a `~` refusal or an unknown name is not
    // the only way to end up compiled.
    // The register compactor's liveness analysis is straight-line (with
    // forward branches as barriers).  A while adds a back edge, so retaining
    // the uncompact program is the correctness-first choice: a state register
    // written in one iteration is necessarily live at the next head.
    bool has_back_edge = false;
    bool has_unmodelled_ranges = false;
    for (size_t pc = 0; pc < prog->code.size(); ++pc) {
      const Program::Instr& instr = prog->code[pc];
      if (program_code_spec(instr.code).has(kProgramNoAdjoint))
        has_unmodelled_ranges = true;
      if ((instr.code == Program::JZ || instr.code == Program::JMP) &&
          instr.dst <= static_cast<int>(pc)) {
        has_back_edge = true;
      }
    }
    // The straight-line compactor derives every range width from Instr::len.
    // Structured matrix calls use that field for the result width while
    // their operands can have different widths, so retain the original
    // register numbering until those instructions carry explicit spans.
    if (!has_back_edge && !has_unmodelled_ranges) compact_island(*prog);
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
    // Variant stays zero: kIslandSoftmax3Variant is a tagged-payload contract
    // and may only accompany Softmax3IslandProg (the graph carver creates it).
    std::vector<int> inputs = reg.in_slots;
    if (inputs.size() <= 6) {
      for (size_t k = 0; k < prog->ins.size(); ++k) {
        prog->ins[k].input = (int)k;
        prog->ins[k].offset = 0;
      }
    } else {
      // Op::in is deliberately compact. Pack just enough leading live-ins
      // to leave five ordinary descriptors; the program's LiveIn records
      // retain the individual register ranges and point into the packed one.
      const size_t packed_count = inputs.size() - 5;
      int packed = inputs[0];
      int64_t packed_len = g.slots[packed].len;
      for (size_t k = 1; k < packed_count; ++k) {
        packed_len += g.slots[inputs[k]].len;
        packed = emit_raw(OP_CONCAT2, {packed, inputs[k]}, packed_len, {}).slot;
      }
      int offset = 0;
      for (size_t k = 0; k < packed_count; ++k) {
        prog->ins[k].input = 0;
        prog->ins[k].offset = offset;
        offset += prog->ins[k].len;
      }
      std::vector<int> compact{packed};
      for (size_t k = packed_count; k < inputs.size(); ++k) {
        prog->ins[k].input = (int)compact.size();
        prog->ins[k].offset = 0;
        compact.push_back(inputs[k]);
      }
      inputs = std::move(compact);
    }
    is.n_in = (int)inputs.size();
    for (int k = 0; k < is.n_in; ++k) is.in[k] = inputs[k];
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
  void lower_runtime_ifelse(const mir::Stmt& s) {
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
    // Nothing to carry out and no target to accumulate: every live-out is
    // zero-width, so the region has no observable effect and its values
    // keep the empty shape they already have outside. A `target +=` would
    // have put its own register here, so this cannot drop one -- and a
    // print()/reject() have no live-out by design, so it cannot either.
    if (prog->out_regs.empty() && !reg.has_effect) return;
    std::vector<int> out_slots;
    emit_island(prog, reg, out_lens, &out_slots);
    // Later statements read the island's results, not the old values.
    for (size_t k = 0; k < reg.out_names.size(); ++k) {
      const std::string& name = reg.out_names[k];
      SlotInfo si;
      if (reg.out_is_int[k]) {
        // This local was an SInt before the loop.  Its loop-carried value is
        // now a register-program result; retain the UInt type but make it a
        // graph-local runtime value so later branches and scalar reads use
        // the value the loop actually produced.
        si = view_of("UInt");
        si.param_free = false;
        scope[name] = Val{out_slots[k], false, si};
        decls[name] = DeclView{1, false, si};
        int_env.erase(name);
        int_locals.erase(name);
        td.env().erase(name);
        continue;
      }
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
      // Runtime regions conservatively return parameter-dependent live-outs;
      // treating one as data without a per-output dependency proof would
      // select kernels that deliberately omit adjoints for that input.
      si.param_free = false;
      scope[name] = Val{out_slots[k], scalar_autodiff(), si};
    }
    if (reg.has_target) push_target_term(out_slots.back());
  }

  // `<not known while building the graph> ? a : b`
  Val lower_runtime_ternary(const mir::Expr& e) {
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

  // Use the runtime-control compiler as a graph producer for higher-order
  // families whose shared implementation already lives there. This keeps a
  // straight-line graph call and a call under dynamic control on one callback
  // binder and one kernel path instead of growing a second graph-only parser.
  Val lower_program_expression(const mir::Expr& e) {
    IslandRegion reg;
    std::shared_ptr<IslandProg> prog;
    Range value;
    lower_island(nullptr, &e, &reg, &value, &prog);
    std::vector<int> out_slots;
    emit_island(prog, reg, {value.len}, &out_slots);
    SlotInfo si;
    if (value.kind == ViewKind::Array)
      si = array_view(value.dims, value.leaf, e.data_only);
    else {
      si = view_of(e.type_);
      si.rows = value.rows;
      si.cols = value.cols;
      si.kind = value.kind;
      si.param_free = e.data_only;
    }
    return {out_slots[0], expression_autodiff(e), si};
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
               bool autodiff = false) {
    check_fixed_input_count(ins.size(), opcode);
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
    check_fixed_input_count(ins.size(), opcode);
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
    if (mir::stateful_intrinsic_kind(e)) return true;
    if (e.kind == mir::Expr::FunApp && e.name.size() >= 4 &&
        e.name.compare(e.name.size() - 4, 4, "_rng") == 0)
      return true;
    if (e.kind == mir::Expr::FunApp &&
        e.fn_lib == mir::Expr::Lib::UserDefined && fun_effectful(e.name))
      return true;
    // reduce_sum reaches its partial-sum function through a Var, so the
    // UserDefined test above cannot see a print or reject in that body.
    if (mir::is_reduce_sum(e) && reduce_sum_effectful(e)) return true;
    for (const auto& a : e.args)
      if (expr_effectful(a)) return true;
    return false;
  }

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

  bool stmt_effectful(const mir::Stmt& s) {
    if (s.kind == mir::Stmt::NRFunApp && message_action(s.fn_name)) return true;
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
  bool repeatable_target_expr(const mir::Expr& e, const std::string& loopvar) {
    if (e.kind == mir::Expr::Unsupported || expr_references(e, loopvar))
      return false;
    if (e.kind == mir::Expr::FunApp) {
      if (e.fn_lib != mir::Expr::Lib::StanLib) return false;
      const std::string& name = e.name;
      const bool rng =
          name.size() >= 4 && name.compare(name.size() - 4, 4, "_rng") == 0;
      if (rng || mir::higher_order_call(e) || mir::stateful_intrinsic_kind(e))
        return false;
    }
    for (const auto& a : e.args)
      if (!repeatable_target_expr(a, loopvar)) return false;
    return true;
  }

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
                              bool* has_target) {
    const auto expression_ok = [&](const mir::Expr& e) {
      return repeatable_target_expr(e, loopvar);
    };
    switch (s.kind) {
      case mir::Stmt::Block:
      case mir::Stmt::SList:
        for (const auto& child : s.body)
          if (!repeatable_target_stmt(child, loopvar, locals, has_target))
            return false;
        return true;
      case mir::Stmt::TargetPE:
        if (!expression_ok(s.target)) return false;
        *has_target = true;
        return true;
      case mir::Stmt::Decl:
        if (s.read_transform) return false;
        for (const auto& dim : s.decl_type.dims)
          if (!expression_ok(dim)) return false;
        return !s.has_init || expression_ok(s.init);
      case mir::Stmt::Assignment:
        if (!locals.count(s.lhs) || !expression_ok(s.rhs)) return false;
        for (const auto& index : s.lhs_idx)
          if (!expression_ok(index)) return false;
        return true;
      case mir::Stmt::For:
        if (!expression_ok(s.lower) || !expression_ok(s.upper)) return false;
        for (const auto& child : s.body)
          if (!repeatable_target_stmt(child, loopvar, locals, has_target))
            return false;
        return true;
      case mir::Stmt::IfElse:
        if (!expression_ok(s.cond)) return false;
        for (const auto& arm : s.body)
          if (!repeatable_target_stmt(arm, loopvar, locals, has_target))
            return false;
        return true;
      case mir::Stmt::Skip:
        return true;
      default:
        // Checks, print/reject, while/control transfer, returns, and new
        // statement kinds all keep the ordinary per-iteration path.
        return false;
    }
  }

  bool repeatable_target_body(const mir::Stmt& loop) {
    std::set<std::string> locals;
    for (const auto& child : loop.body) collect_loop_locals(child, &locals);
    bool has_target = false;
    for (const auto& child : loop.body)
      if (!repeatable_target_stmt(child, loop.loopvar, locals, &has_target))
        return false;
    return has_target;
  }

  bool fun_effectful(const std::string& name) {
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
      if (s.kind == mir::Stmt::NRFunApp && message_action(s.fn_name))
        return true;
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

  // Ask only the MIR interpreter.  Static-shape specialization below uses
  // this for selector values and for path-sensitive short-circuit decisions;
  // keeping it separate from try_eval_pure prevents recursive specialization.
  std::optional<DataMap::Entry> try_eval_interpreter(const mir::Expr& e) {
    if (expr_effectful(e)) return std::nullopt;
    if (region_current) {
      // A pure user function can still contain a huge loop. Do not execute
      // it as a speculative control/shape probe inside a retained body.
      std::function<bool(const mir::Expr&)> calls_user =
          [&](const mir::Expr& x) {
            if (x.kind == mir::Expr::FunApp &&
                x.fn_lib == mir::Expr::Lib::UserDefined)
              return true;
            for (const auto& arg : x.args)
              if (calls_user(arg)) return true;
            return false;
          };
      if (calls_user(e)) return std::nullopt;
    }
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
                                                  int64_t extent) {
    if (index.name == "IndexAll")
      return {StaticProbeState::Known, {extent, false}, {}};
    if (index.name == "IndexSingle" && index.args.size() == 1) {
      const auto at = try_static_int(index.args[0]);
      if (at.state != StaticProbeState::Known) return {at.state, {}, at.error};
      if (at.value < 1 || at.value > extent)
        return {
            StaticProbeState::Invalid, {}, "static matrix index out of bounds"};
      return {StaticProbeState::Known, {1, true}, {}};
    }
    if (index.name == "IndexBetween" && index.args.size() == 2) {
      const auto lo = try_static_int(index.args[0]);
      if (lo.state != StaticProbeState::Known) return {lo.state, {}, lo.error};
      const auto hi = try_static_int(index.args[1]);
      if (hi.state != StaticProbeState::Known) return {hi.state, {}, hi.error};
      // Stan's range indexing treats hi < lo as empty and performs no bounds
      // check on either endpoint (the same rule check_range implements).
      if (hi.value < lo.value) return {StaticProbeState::Known, {0, false}, {}};
      if (lo.value < 1 || hi.value > extent)
        return {
            StaticProbeState::Invalid, {}, "static matrix range out of bounds"};
      return {StaticProbeState::Known, {hi.value - lo.value + 1, false}, {}};
    }
    if (index.name == "IndexMulti" && index.args.size() == 1) {
      auto evaluated = try_eval_interpreter(index.args[0]);
      if (!evaluated) return {};
      if (!evaluated->is_int || evaluated->i.size() != evaluated->r.size())
        return {StaticProbeState::Invalid,
                {},
                "static matrix gather index is not integer data"};
      for (int at : evaluated->i)
        if (at < 1 || at > extent)
          return {StaticProbeState::Invalid,
                  {},
                  "static matrix gather index out of bounds"};
      return {StaticProbeState::Known,
              {static_cast<int64_t>(evaluated->i.size()), false},
              {}};
    }
    return {};
  }

  // Logical geometry only: this probe must never materialize a data value or
  // emit a graph op.  The first tranche deliberately handles the expression
  // forms responsible for the ctsem false island -- named values and matrix
  // subviews selected by compile-time integer data.  Everything else declines
  // to the existing runtime-control path.
  StaticProbe<StaticView> try_static_view(const mir::Expr& e) {
    if (e.kind == mir::Expr::Var) {
      auto value = scope.find(e.name);
      if (value != scope.end())
        return {StaticProbeState::Known,
                {g.slots[value->second.slot].len, value->second.si},
                {}};
      auto declaration = decls.find(e.name);
      if (declaration != decls.end())
        return {StaticProbeState::Known,
                {declaration->second.len, declaration->second.si},
                {}};
      return {};
    }
    if (e.kind != mir::Expr::Indexed || e.args.size() < 2 || e.args.size() > 3)
      return {};
    const auto base = try_static_view(e.args[0]);
    if (base.state != StaticProbeState::Known)
      return {base.state, {}, base.error};
    if (!is_matrix(base.value.si)) return {};

    const auto rows = try_static_selector(e.args[1], base.value.si.rows);
    if (rows.state != StaticProbeState::Known)
      return {rows.state, {}, rows.error};
    StaticProbe<StaticSelector> cols{
        StaticProbeState::Known, {base.value.si.cols, false}, {}};
    if (e.args.size() == 3)
      cols = try_static_selector(e.args[2], base.value.si.cols);
    if (cols.state != StaticProbeState::Known)
      return {cols.state, {}, cols.error};

    const bool rd = rows.value.drops_dimension;
    const bool cd = cols.value.drops_dimension;
    StaticView out;
    out.len = checked_product({rows.value.count, cols.value.count},
                              "static matrix subview");
    out.si.param_free = base.value.si.param_free;
    if (!rd && !cd) {
      if (e.type_ != "UMatrix")
        return {StaticProbeState::Invalid,
                {},
                "static matrix subview has an inconsistent result type"};
      out.si = matrix_view(rows.value.count, cols.value.count,
                           base.value.si.param_free);
    } else if (rd && !cd) {
      if (e.type_ != "URowVector")
        return {StaticProbeState::Invalid,
                {},
                "static matrix row has an inconsistent result type"};
      out.si = view_of("URowVector");
      out.si.param_free = base.value.si.param_free;
    } else if (!rd && cd) {
      if (e.type_ != "UVector")
        return {StaticProbeState::Invalid,
                {},
                "static matrix column has an inconsistent result type"};
      out.si = view_of("UVector");
      out.si.param_free = base.value.si.param_free;
    } else {
      if (e.type_ != "UReal")
        return {StaticProbeState::Invalid,
                {},
                "static matrix element has an inconsistent result type"};
      out.si = view_of("UReal");
      out.si.param_free = base.value.si.param_free;
    }
    return {StaticProbeState::Known, out, {}};
  }

  StaticProbe<int64_t> try_static_shape_query(const mir::Expr& e) {
    if (!is_shape_query(e)) return {};
    const auto view = try_static_view(e.args[0]);
    if (view.state != StaticProbeState::Known)
      return {view.state, 0, view.error};
    const StaticView& v = view.value;
    if (is_array(v.si)) {
      const ArrayShape& shape = array_shape(v.si);
      if (e.name == "size" || e.name == "FnLength")
        return {StaticProbeState::Known, shape.dims.front(), {}};
      if (e.name == "num_elements") return {StaticProbeState::Known, v.len, {}};
      return {StaticProbeState::Invalid, 0,
              e.name + " is undefined for an array value"};
    }
    const LogicalDims dims = logical_dims(v.si, v.len, e.name);
    if (e.name == "rows") return {StaticProbeState::Known, dims.rows, {}};
    if (e.name == "cols") return {StaticProbeState::Known, dims.cols, {}};
    return {StaticProbeState::Known, v.len, {}};
  }

  // Replace only shape queries proven from immutable logical geometry.  The
  // walk is lazy across Stan's short-circuit forms: an invalid subview in a
  // dead RHS/arm must not become a bind-time error merely because this probe
  // visited it.
  bool specialize_static_shapes(mir::Expr* e) {
    bool changed = false;
    if (e->kind == mir::Expr::EAnd || e->kind == mir::Expr::EOr) {
      if (e->args.size() != 2) return false;
      changed = specialize_static_shapes(&e->args[0]);
      auto lhs = try_eval_interpreter(e->args[0]);
      if (!lhs || lhs->r.size() != 1) return changed;
      const bool value = lhs->r[0] != 0.0;
      const bool reaches_rhs = e->kind == mir::Expr::EAnd ? value : !value;
      if (reaches_rhs) changed |= specialize_static_shapes(&e->args[1]);
      return changed;
    }
    if (e->kind == mir::Expr::TernaryIf) {
      if (e->args.size() != 3) return false;
      changed = specialize_static_shapes(&e->args[0]);
      auto condition = try_eval_interpreter(e->args[0]);
      if (!condition || condition->r.size() != 1) return changed;
      const size_t arm = condition->r[0] != 0.0 ? 1 : 2;
      changed |= specialize_static_shapes(&e->args[arm]);
      return changed;
    }
    if (is_shape_query(*e)) {
      const auto value = try_static_shape_query(*e);
      if (value.state == StaticProbeState::Invalid) fail(value.error, e->raw);
      if (value.state == StaticProbeState::Known) {
        if (value.value < std::numeric_limits<int>::min() ||
            value.value > std::numeric_limits<int>::max())
          fail("static shape query exceeds the Stan integer range", e->raw);
        mir::Expr literal;
        literal.kind = mir::Expr::LitInt;
        literal.lit_i = static_cast<long>(value.value);
        literal.type_ = "UInt";
        literal.unsized = {0, mir::UnsizedLeaf::Int};
        literal.data_only = true;
        literal.raw = e->raw;
        *e = std::move(literal);
        return true;
      }
    }
    for (mir::Expr& arg : e->args) changed |= specialize_static_shapes(&arg);
    return changed;
  }

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

  std::optional<Val> fold_const(const mir::Expr& e) {
    if (!e.data_only || e.fn_propto || expr_effectful(e)) return std::nullopt;
    auto evaluated = try_eval_pure(e);
    if (!evaluated) return std::nullopt;
    DataMap::Entry en = std::move(*evaluated);
    if (en.r.size() == 1 &&
        (e.type_ == "UReal" || e.type_ == "UInt" || e.type_ == "UComplex"))
      return constant(en.r[0]);
    SlotInfo si;
    si.param_free = true;
    if (e.unsized.depth != 0) {
      ViewKind leaf = ViewKind::Flat;
      if (e.unsized.leaf == mir::UnsizedLeaf::Vector)
        leaf = ViewKind::Vector;
      else if (e.unsized.leaf == mir::UnsizedLeaf::RowVector)
        leaf = ViewKind::RowVector;
      else if (e.unsized.leaf == mir::UnsizedLeaf::Matrix)
        leaf = ViewKind::Matrix;
      if (en.dims.empty()) en.dims = {(int64_t)en.r.size()};
      si = array_view(en.dims, leaf, true);
    } else {
      stamp_kind(&si, e.type_);
    }
    if (e.type_ == "UMatrix" && en.dims.size() == 2)
      si = matrix_view(en.dims[0], en.dims[1], true);
    const bool nested_matrix =
        e.unsized.depth != 0 && e.unsized.leaf == mir::UnsizedLeaf::Matrix;
    std::vector<double> vals =
        graph_order(en, e.type_ == "UMatrix", nested_matrix);
    const int s = add_slot((int64_t)vals.size(), false);
    out.fills.emplace_back(s, vals);
    Val v{s, false, si, owning_layout(si)};
    observe(v, std::move(en));
    return v;
  }

  // Integer argument of a density/pmf: values must be known at compile
  // time (int data, loop variables, or compile-time expressions).
  std::vector<int> int_arg_values(LoweredArgument& actual) {
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

  // Two-argument scalar math with one int argument
  // (STANLI_SCALAR_BINARY_INT_FIRST_LIST and its SECOND twin): elementwise
  // with scalar broadcast like the all-real binaries, but shape_of does not
  // apply. Those two sides may legitimately carry different views --
  // `ldexp(matrix, array[,] int)` is a matrix, `falling_factorial(real,
  // array[,] int)` is an array -- so the result takes the real side's view
  // when it has one and the int side's when the real side is a scalar,
  // which is what the signature list says in every case.
  Val lower_binary_int(uint16_t opcode, bool int_first,
                       CallArguments& actuals) {
    actuals.require_arity(2);
    const mir::Expr& e = actuals.call_expr();
    Val a = actuals.at(0).value();
    Val b = actuals.at(1).value();
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
    return with_layout(
        emit_value(opcode, {a, b}, std::max(lr, li), si, std::move(idata)),
        elementwise_layout({a, b}));
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
  std::optional<Val> lower_callable_transform(const mir::Expr& e,
                                              CallArguments& actuals) {
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
          if (leaf != ViewKind::Vector)
            fail(e.name + ": expected vector", e.raw);
          out_rows = dims.back();
          break;
        case CallableTransformKind::Simplex:
          if (leaf != ViewKind::Vector)
            fail(e.name + ": expected vector", e.raw);
          out_rows = dims.back() + 1;
          break;
        case CallableTransformKind::UnitVector:
          if (leaf != ViewKind::Vector)
            fail(e.name + ": expected vector", e.raw);
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
          if (leaf != ViewKind::Matrix)
            fail(e.name + ": expected matrix", e.raw);
          out_rows =
              raw_rows + (tr.kind == CallableTransformKind::StochasticColumn);
          out_cols =
              raw_cols + (tr.kind == CallableTransformKind::StochasticRow);
          break;
        case CallableTransformKind::CholeskyFactorCorr:
        case CallableTransformKind::CorrMatrix:
        case CallableTransformKind::CovMatrix: {
          if (leaf != ViewKind::Vector)
            fail(e.name + ": expected vector", e.raw);
          const int64_t k =
              actuals.at(1).require_constant_int("matrix dimension");
          out_leaf = ViewKind::Matrix;
          out_rows = out_cols = k;
          break;
        }
        case CallableTransformKind::CholeskyFactorCov:
          if (leaf != ViewKind::Vector)
            fail(e.name + ": expected vector", e.raw);
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
        si =
            view_of(out_leaf == ViewKind::RowVector ? "URowVector" : "UVector");
        si.param_free = raw.si.param_free;
      }
      std::vector<int> idata = {
          checked_immediate(batch, e.name + " batch"),
          checked_immediate(inner_raw, e.name + " raw leaf"),
          checked_immediate(out_leaf == ViewKind::Matrix ? out_rows : inner_con,
                            e.name + " result rows")};
      if (out_leaf == ViewKind::Matrix)
        idata.push_back(
            checked_immediate(out_cols, e.name + " result columns"));
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
        fail(e.name + ": bound is neither one value nor one per element",
             e.raw);
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
  Val free_transform(uint16_t opcode, const std::vector<Val>& a, SlotInfo si,
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
  struct LoopBreak {};
  struct LoopContinue {};

  // Inline a user-defined function at its call site: arguments are lowered
  // in the caller's scope, bound under the parameter names in a shadowed
  // scope, and the body lowers like any other statements (loops unroll,
  // data-only conditions resolve). Return throws the result value out.
  Val lower_call_udf(const mir::Expr& e,
                     const std::function<void()>& before_body = {}) {
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
        if (const DataMap::Entry* en = actual.observation())
          binds[i].data = *en;
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
    auto sc_saved = scope;
    auto region_cells_saved = region_cells;
    const int region_depth_saved = region_control_depth;
    if (region_current) {
      region_cells.clear();
      region_control_depth = 0;
    }
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

  Val lower_multi_normal_rng(const mir::Expr& e, CallArguments& actuals) {
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

  Val lower_dirichlet_rng(const mir::Expr& e, CallArguments& actuals) {
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

  Val lower_categorical_rng(const mir::Expr& e, CallArguments& actuals) {
    if (!in_write_array)
      fail("categorical_rng is supported only in generated quantities", e.raw);
    if (e.args.size() != 1 || e.type_ != "UInt" ||
        e.unsized.leaf != mir::UnsizedLeaf::Int || e.unsized.depth != 0)
      fail("categorical_rng: expected one scalar int result", e.raw);
    const mir::Expr& probabilities = actuals.at(0).expr();
    if (probabilities.type_ != "UVector" || probabilities.unsized.depth != 0 ||
        probabilities.unsized.leaf != mir::UnsizedLeaf::Vector)
      fail("categorical_rng: expected one probability-vector argument", e.raw);

    Val argument = actuals.at(0).value();
    if (!is_vector(argument.si))
      fail("categorical_rng: argument is not a logical vector", e.raw);
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

  Val lower_scalar_rng(const mir::Expr& e, CallArguments& actuals,
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
        arity == 1 ? emit_value(OP_RNG, {args[0]}, 1, view_of(e.type_))
        : arity == 2
            ? emit_value(OP_RNG, {args[0], args[1]}, 1, view_of(e.type_))
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
  bool needs_runtime_value(const mir::Expr& e) {
    if (is_shape_query(e) &&
        try_static_shape_query(e).state == StaticProbeState::Known)
      return false;
    if (e.kind == mir::Expr::Var) {
      const auto v = scope.find(e.name);
      return v != scope.end() && !observation(v->second) &&
             (!v->second.si.param_free ||
              (!int_env.count(e.name) && !td.find(e.name)));
    }
    if (expr_effectful(e)) return true;
    for (const auto& arg : e.args)
      if (needs_runtime_value(arg)) return true;
    return false;
  }

  bool runtime_int_value(const mir::Expr& e) const {
    if (e.type_ != "UInt" || e.unsized.leaf != mir::UnsizedLeaf::Int ||
        e.unsized.depth != 0)
      return false;
    if (e.kind == mir::Expr::Var) {
      auto it = scope.find(e.name);
      return it != scope.end() && !it->second.si.param_free;
    }
    if (e.kind == mir::Expr::Indexed && !e.args.empty() &&
        e.args[0].kind == mir::Expr::Var) {
      auto it = scope.find(e.args[0].name);
      return it != scope.end() && !it->second.si.param_free;
    }
    return false;
  }

  Val lower_runtime_int_sum(const mir::Expr& e, CallArguments& actuals) {
    if (!in_write_array)
      fail("runtime integer sum is supported only in generated quantities",
           e.raw);
    if (!is_int_sum_surface(e))
      fail(
          "runtime integer sum needs one one-dimensional int-array argument "
          "and a scalar int result",
          e.raw);

    actuals.require_arity(1);
    Val a = actuals.at(0).value();
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

    Val result = with_layout(emit_value(OP_SUM_VEC, {a}, 1, view_of("UInt")),
                             ExpressionLayout::scalar());
    result.autodiff = false;
    // A range is only a static proof; the source itself was required to be
    // runtime-produced.  Keeping this result non-constant prevents later
    // compile-time geometry/control from consuming it through Val metadata.
    result.si.param_free = false;
    set_int_range(result, static_cast<int64_t>(range.lo) * len,
                  static_cast<int64_t>(range.hi) * len);
    return result;
  }

  // map_rect checks that the three job arrays have matching OUTER sizes and
  // returns an empty vector before touching the shared parameters or the UDF
  // when that size is zero.  This is the one map_rect case which needs no
  // runtime callback at all (and is exercised by stanc3's mother model).
  // Nonempty calls deliberately keep falling through to the unsupported
  // function diagnostic.
  std::optional<Val> lower_empty_map_rect(const mir::Expr& e,
                                          CallArguments& actuals) {
    if (e.name != "map_rect") return std::nullopt;
    if (actuals.size() != 5)
      fail(
          "map_rect: expected function, shared parameters, job parameters, "
          "real data, and integer data",
          e.raw);

    // A non-variable shared-parameter expression still has to be evaluated
    // before map_rect can take its empty-job return.  Plain zero-length
    // locals have no materialized slot, so their declared view is enough.
    SlotInfo shared_si;
    const mir::Expr& shared_expr = actuals.at(1).expr();
    if (shared_expr.kind == mir::Expr::Var) {
      auto declared = decls.find(shared_expr.name);
      if (declared != decls.end()) shared_si = declared->second.si;
    }
    if (!is_vector(shared_si)) shared_si = actuals.at(1).value().si;
    if (!is_vector(shared_si))
      fail("map_rect: shared parameters are not a vector", e.raw);

    // A default-initialized zero-length local has declaration geometry but
    // no scope value: there are no elements to initialize or materialize.
    // map_rect does not read it on this branch, so consult decls before
    // asking lower_expr for a slot (mother's `tmp2` has exactly this form).
    SlotInfo job_si;
    const mir::Expr& job_expr = actuals.at(2).expr();
    if (job_expr.kind == mir::Expr::Var) {
      auto declared = decls.find(job_expr.name);
      if (declared != decls.end()) job_si = declared->second.si;
    }
    if (!is_array(job_si)) job_si = actuals.at(2).value().si;
    if (!is_array(job_si)) return std::nullopt;
    const ArrayShape& job_shape = array_shape(job_si);
    const size_t job_outer =
        job_shape.dims.size() - (size_t)leaf_rank(job_shape.leaf);
    if (job_shape.leaf != ViewKind::Vector || job_outer != 1 ||
        job_shape.dims.front() != 0)
      return std::nullopt;

    Val real_data = actuals.at(3).value();
    Val int_data = actuals.at(4).value();
    if (!is_array(real_data.si) || !is_array(int_data.si))
      fail("map_rect: job data arguments are not arrays", e.raw);
    const ArrayShape& real_shape = array_shape(real_data.si);
    const ArrayShape& int_shape = array_shape(int_data.si);
    if (real_shape.leaf != ViewKind::Flat || int_shape.leaf != ViewKind::Flat ||
        real_shape.dims.size() != 2 || int_shape.dims.size() != 2)
      fail("map_rect: job data arguments do not have two array dimensions",
           e.raw);
    if (real_shape.dims.front() != 0 || int_shape.dims.front() != 0)
      fail("map_rect: job parameters and job data sizes do not match", e.raw);
    if (e.unsized.leaf != mir::UnsizedLeaf::Vector || e.unsized.depth != 0)
      fail("map_rect: result is not a vector", e.raw);

    SlotInfo si = view_of(e.type_);
    si.param_free = true;
    const int slot = add_slot(0, false);
    out.fills.emplace_back(slot, std::vector<double>{});
    return Val{slot, false, si, owning_layout(si)};
  }

  mir::Expr slice_bound_literal(int64_t value, const std::string& raw) {
    if (value > std::numeric_limits<int32_t>::max())
      fail("reduce_sum: slice bound exceeds the Stan integer range", raw);
    mir::Expr literal;
    literal.kind = mir::Expr::LitInt;
    literal.lit_i = static_cast<long>(value);
    literal.lit = static_cast<double>(value);
    literal.type_ = "UInt";
    literal.unsized = {0, mir::UnsizedLeaf::Int};
    literal.data_only = true;
    literal.raw = raw;
    return literal;
  }

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
  Val lower_reduce_sum(const mir::Expr& e, CallArguments& actuals) {
    if (actuals.size() < 3)
      fail(
          "reduce_sum: expected a partial-sum function, a sliced argument, "
          "and a grainsize",
          e.raw);
    const mir::Expr& partial_expr = actuals.at(0).expr();
    if (partial_expr.kind != mir::Expr::Var)
      fail("reduce_sum: the partial-sum argument is not a function name",
           e.raw);
    if (e.unsized.depth != 0 || e.unsized.leaf != mir::UnsizedLeaf::Real)
      fail("reduce_sum: result is not a real", e.raw);

    Val slice = actuals.at(1).value();
    if (!is_array(slice.si))
      fail("reduce_sum: the sliced argument is not an array", e.raw);
    // Grainsize does not choose a partition here, but evaluating it and
    // checking positivity are still observable. Do not swallow a failed
    // compile-time probe or execute effectful expressions while lowering.
    // Keep pure data integer operations on the interpreter path: operations
    // such as divide(int, int) need not have a runtime graph kernel. Failed
    // folding still lowers (or refuses) the expression; it never drops it.
    const auto folded_grainsize = actuals.at(2).try_fold();
    const Val grainsize =
        folded_grainsize ? *folded_grainsize : actuals.at(2).value();
    const mir::Expr& grainsize_expr = actuals.at(2).expr();
    if (grainsize_expr.unsized.depth != 0 ||
        grainsize_expr.unsized.leaf != mir::UnsizedLeaf::Int ||
        !is_scalar(grainsize))
      fail("reduce_sum: grainsize is not an integer scalar", e.raw);
    const auto check_grainsize = [&] {
      auto spec = std::make_shared<BoundCheckSpec>();
      spec->name = "reduce_sum grainsize";
      spec->bound_is_scalar = true;
      spec->shapes_match = true;
      (void)emit_value(OP_CHECK_LOWER, {grainsize, constant(1.0)}, 1);
      g.ops.back().udata = spec.get();
      g.udata_pool.push_back(std::move(spec));
    };
    const int64_t n = array_shape(slice.si).dims.front();
    if (n == 0) {
      // C++ evaluates shared arguments before reduce_sum can return zero.
      // Only the partial-sum body is skipped for an empty slice.
      for (size_t i = 3; i < actuals.size(); ++i) (void)actuals.at(i).value();
      check_grainsize();
      return constant(0.0);
    }

    bool propto = false;
    const std::string base =
        mir::reduce_sum_partial_name(partial_expr.name, &propto);
    const std::vector<mir::UnsizedView> views =
        mir::reduce_sum_partial_views(e);
    const mir::FunDef* f = mir::resolve_callback(fun_defs, base, views);
    if (f == nullptr)
      fail("reduce_sum: unknown partial-sum function " + base, e.raw);
    if (f->arg_views.size() != views.size())
      fail("reduce_sum: " + base + " takes " +
               std::to_string(f->arg_views.size()) +
               " arguments, but reduce_sum calls it with " +
               std::to_string(views.size()),
           e.raw);
    if (f->arg_views[1].depth != 0 ||
        f->arg_views[1].leaf != mir::UnsizedLeaf::Int ||
        f->arg_views[2].depth != 0 ||
        f->arg_views[2].leaf != mir::UnsizedLeaf::Int)
      fail(
          "reduce_sum: " + base + " must take the two slice bounds as integers",
          e.raw);

    // Bind the lowered slice under a name no Stan identifier can collide
    // with, so the rewritten call can name it instead of lowering the slice
    // expression a second time. resolve_overloads borrows Stan's syntax for
    // the same purpose.
    const std::string bound =
        "(reduce_sum slice " + std::to_string(reduce_sum_slices++) + ")";
    scope[bound] = slice;
    decls[bound] = DeclView{g.slots[slice.slot].len, slice.autodiff, slice.si,
                            false, false};

    mir::Expr call;
    call.kind = mir::Expr::FunApp;
    call.fn_lib = mir::Expr::Lib::UserDefined;
    call.name = f->name;
    // lower_call_udf reads this exactly as CmdStan reads the generated
    // functor's propto__ argument: an `_lupdf` functor inherits the caller's
    // normalization, an `_lpdf` one forces the normalized density.
    call.fn_propto = propto;
    call.type_ = e.type_;
    call.unsized = e.unsized;
    call.data_only = e.data_only;
    call.raw = e.raw;
    call.args.reserve(views.size());
    mir::Expr sliced;
    sliced.kind = mir::Expr::Var;
    sliced.name = bound;
    const mir::Expr& slice_expr = actuals.at(1).expr();
    sliced.type_ = slice_expr.type_;
    sliced.unsized = slice_expr.unsized;
    sliced.data_only = slice_expr.data_only;
    sliced.raw = slice_expr.raw;
    call.args.push_back(std::move(sliced));
    call.args.push_back(slice_bound_literal(1, e.raw));
    call.args.push_back(slice_bound_literal(n, e.raw));
    for (size_t i = 3; i < actuals.size(); ++i)
      call.args.push_back(actuals.at(i).expr());

    Val result{-1, false, {}};
    try {
      result = lower_call_udf(call, check_grainsize);
    } catch (...) {
      scope.erase(bound);
      decls.erase(bound);
      throw;
    }
    scope.erase(bound);
    decls.erase(bound);
    return result;
  }

  Val lower_append_array(const mir::Expr& e, CallArguments& actuals) {
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
      const int64_t observation_lanes =
          a_outer + b_outer == 0 ? 0 : suffix_count;
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

  Val lower_funapp(const mir::Expr& e) {
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
        if (auto v = lower_density_fn(e, actuals)) return *v;
        break;
      case BuiltinFamily::CallableTransform:
        if (auto v = lower_callable_transform(e, actuals)) return *v;
        break;
      case BuiltinFamily::Elementwise:
        if (auto v = lower_eltwise_fn(
                e, actuals, dispatch.regular ? &*dispatch.regular : nullptr))
          return *v;
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
        (udf_depth == 0 &&
         arg.autodiff != (!in_write_array && !e.args[1].data_only)))
      fail(e.name + ": MIR adlevel contradicts lowered dependencies", e.raw);
    auto spec = std::make_shared<CategoricalSpec>();
    spec->logit = logit;
    spec->scalar_outcome = scalar_outcome;
    // The graph dependency and instantiated C++ scalar type are independent:
    // write_array varies with q but uses double, while an autodiff local can
    // be graph-constant and still make Stan retain a propto summand.
    spec->arg_autodiff = arg.autodiff;
    spec->propto = propto(e);
    Val checked = with_layout(emit_value(OP_CATEGORICAL, {outcome, arg}, 1, {}),
                              ExpressionLayout::scalar());
    g.ops.back().udata = spec.get();
    g.udata_pool.push_back(std::move(spec));
    return checked;
  }

  std::optional<Val> lower_density_fn(const mir::Expr& e,
                                      CallArguments& actuals) {
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
      if ((int)actuals.size() != spec.arity) {
        // The compact cases below used to decline a bad arity and let the
        // common unsupported-function diagnostic report it.
        if (spec.shape != DensityShape::Plain || spec.activity_mask >= 0)
          return std::nullopt;
        actuals.require_arity((size_t)spec.arity);
      }
      std::vector<int> idata;
      // Whether argument 0 was written as a bare `int` rather than an
      // array. Same test as the two-group path below, and for the same
      // reason: a length-1 array is a container that must match the other
      // arguments' size, a scalar broadcasts.
      bool scalar_outcome = false;
      if (spec.integer_args == 1) {
        idata = int_arg_values(actuals.at(0));
        scalar_outcome =
            actuals.at(0).expr().type_ == "UInt" && idata.size() == 1;
      } else if (spec.integer_args == 2) {
        // Group length -1 marks a language-level scalar (broadcast in
        // stan-math); a length-1 array stays a vector, as CmdStan would
        // instantiate it.
        auto put = [&](LoweredArgument& actual) {
          auto vals = int_arg_values(actual);
          const bool scalar = actual.expr().type_ == "UInt" && vals.size() == 1;
          idata.push_back(scalar ? -1 : (int)vals.size());
          idata.insert(idata.end(), vals.begin(), vals.end());
        };
        put(actuals.at(0));
        put(actuals.at(1));
      }
      std::vector<int> ins;
      SlotInfo shapes[6]{};
      SlotInfo result_si{0, 0, true};
      bool result_autodiff = false;
      uint8_t variant =
          spec.activity_mask < 0 ? 0 : (uint8_t)spec.activity_mask;
      for (size_t i = spec.integer_args; i < e.args.size(); ++i) {
        const Val arg = actuals.at(i).value();
        shapes[i - spec.integer_args] = arg.si;
        ins.push_back(arg.slot);
        result_si.param_free = result_si.param_free && arg.si.param_free;
        result_autodiff = result_autodiff || arg.autodiff;
        if (spec.activity_mask < 0 && !actuals.at(i).expr().data_only)
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
        if (K < 0 || shapes[last].cols != K)
          fail(e.name + ": matrix argument must be square", e.raw);

        // The native kernels accept one vector/row-vector location and a
        // vector or array of vectors on the left. Derive repetitions from
        // the logical view, not a division by K: that remains defined for
        // legal zero-dimensional vectors and catches short flat storage
        // before a kernel can read past it. multi_student_t has nu between
        // y and mu; the multi_normal forms do not.
        const auto vector_repetitions = [&](size_t arg, const char* role) {
          const SlotInfo& si = shapes[arg];
          const int64_t len = g.slots[ins[arg]].len;
          if (is_vector(si) || is_row_vector(si)) {
            if (len != K) {
              if (arg == 0)
                fail(e.name + ": random variable length " +
                         std::to_string(len) +
                         " is not a positive multiple of matrix size " +
                         std::to_string(K),
                     e.raw);
              fail(e.name + ": " + role + " length " + std::to_string(len) +
                       " does not match matrix size " + std::to_string(K),
                   e.raw);
            }
            return int64_t{1};
          }
          if (is_array(si)) {
            const ArrayShape& array = array_shape(si);
            if ((array.leaf != ViewKind::Vector &&
                 array.leaf != ViewKind::RowVector) ||
                array.dims.empty() || array.dims.back() != K)
              fail(e.name + ": " + role +
                       " must be a vector or an array of vectors",
                   e.raw);
            std::vector<int64_t> outer(array.dims.begin(),
                                       array.dims.end() - 1);
            const int64_t repetitions =
                checked_product(outer, e.name + ": " + role + " shape");
            if (checked_product({repetitions, K},
                                e.name + ": " + role + " storage") != len)
              fail(e.name + ": " + role +
                       " logical shape does not match storage length",
                   e.raw);
            return repetitions;
          }
          fail(
              e.name + ": " + role + " must be a vector or an array of vectors",
              e.raw);
        };
        const int64_t repetitions = vector_repetitions(0, "random variable");
        if (repetitions == 0)
          fail(e.name + ": an empty array of random variables is unsupported",
               e.raw);
        const size_t location = e.name.rfind("multi_student_t", 0) == 0 ? 2 : 1;
        if (vector_repetitions(location, "location") != 1)
          fail(e.name + ": an array-valued location is unsupported", e.raw);
        idata = {(int)K, (int)repetitions};
      }
      Val dv =
          emit_raw(spec.opcode, ins, 1, result_si, idata, -1, result_autodiff);
      dv.layout = ExpressionLayout::scalar();
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
      const Val outcome = actuals.at(0).value();
      Val b = actuals.at(1).value();
      return emit_categorical(e, outcome, b, true);
    }
    if (e.name == "categorical_lpmf" && e.args.size() == 2) {
      const Val outcome = actuals.at(0).value();
      Val th = actuals.at(1).value();
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
      std::vector<int> idata = int_arg_values(actuals.at(0));
      std::vector<int> NN = int_arg_values(actuals.at(1));
      Val X = actuals.at(2).value();
      Val alpha = actuals.at(3).value();
      Val beta = actuals.at(4).value();
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
      Val v = with_layout(emit_value(OP_BINOMIAL_LOGIT_GLM_LPMF,
                                     {X, alpha, beta}, 1, {}, idata),
                          ExpressionLayout::scalar());
      g.ops.back().variant = (uint8_t)((propto(e) ? 0x80u : 0u) | 0x7u);
      return v;
    }
    if ((e.name == "categorical_logit_glm_lpmf" ||
         e.name == "ordered_logistic_glm_lpmf") &&
        e.args.size() == 4) {
      std::vector<int> idata = int_arg_values(actuals.at(0));
      Val X = actuals.at(1).value();
      Val a2 = actuals.at(2).value();
      Val a3 = actuals.at(3).value();
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
      Val v = with_layout(emit_value(e.name == "categorical_logit_glm_lpmf"
                                         ? OP_CATEGORICAL_LOGIT_GLM_LPMF
                                         : OP_ORDERED_LOGISTIC_GLM_LPMF,
                                     {X, a2, a3}, 1, {}, idata),
                          ExpressionLayout::scalar());
      g.ops.back().variant = (uint8_t)((propto(e) ? 0x80u : 0u) | 0x7u);
      return v;
    }

    if (e.name == "normal_id_glm_lpdf" && e.args.size() == 5) {
      Val y = actuals.at(0).value();
      Val X = actuals.at(1).value();
      if (!is_matrix(X.si)) fail("normal_id_glm: X must be a matrix", e.raw);
      Val alpha = actuals.at(2).value();
      Val beta = actuals.at(3).value();
      Val sigma = actuals.at(4).value();
      uint8_t variant = 0;
      for (int i = 0; i < 5; ++i)
        if (!actuals.at(i).expr().data_only) variant |= (uint8_t)(1u << i);
      if (propto(e)) variant |= 0x80u;
      Val v = with_layout(
          emit_value(OP_NORMAL_ID_GLM_LPDF, {y, X, alpha, beta, sigma}, 1, {},
                     {(int)X.si.rows, (int)X.si.cols}),
          ExpressionLayout::scalar());
      g.ops.back().variant = variant;
      return v;
    }
    return std::nullopt;
  }

  // Elementwise math, reductions, and dot products.
  std::optional<Val> lower_eltwise_fn(const mir::Expr& e,
                                      CallArguments& actuals,
                                      const RegularSpec* regular) {
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
    struct NamedSolve {
      const char* name;
      bool left;
      uint16_t opcode;
    };
    static constexpr NamedSolve kNamedSolves[] = {
        {"mdivide_left", true, OP_MDIVIDE_LEFT},
        {"mdivide_right", false, OP_MDIVIDE_RIGHT},
        {"mdivide_left_spd", true, OP_MDIVIDE_LEFT_SPD},
        {"mdivide_right_spd", false, OP_MDIVIDE_RIGHT_SPD},
        {"mdivide_left_tri_low", true, OP_MDIVIDE_LEFT_TRI_LOW},
        {"mdivide_right_tri_low", false, OP_MDIVIDE_RIGHT_TRI_LOW},
    };
    const NamedSolve* named_solve = nullptr;
    if (e.args.size() == 2)
      for (const NamedSolve& candidate : kNamedSolves)
        if (e.name == candidate.name) named_solve = &candidate;
    if (named_solve != nullptr || e.name == "LDivide__" ||
        (e.name == "Divide__" && e.args.at(1).type_ == "UMatrix")) {
      const bool left =
          named_solve != nullptr ? named_solve->left : e.name == "LDivide__";
      const uint16_t opcode = named_solve != nullptr
                                  ? named_solve->opcode
                                  : (left ? OP_MDIVIDE_LEFT : OP_MDIVIDE_RIGHT);
      actuals.require_arity(2);
      Val a = actuals.at(0).value();
      Val b = actuals.at(1).value();
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
      Val v = emit_value(opcode, {a, b}, n * k, dividend.si, {(int)n, (int)k});
      // The kernel solves through the operand types CmdStan's generated code
      // would have used, because stan-math answers differently for each: bit
      // 0 says the result is var, bit 1 says the dividend is a vector rather
      // than a one-column matrix, and bits 2/3 retain the divisor/dividend
      // scalar types so mixed vv/vd/dv overloads do not collapse to vv.
      g.ops.back().variant = (uint8_t)((v.autodiff ? 1u : 0u) | (dm ? 0u : 2u) |
                                       (divisor.autodiff ? 4u : 0u) |
                                       (dividend.autodiff ? 8u : 0u));
      return with_layout(v, owning_layout(dividend.si));
    }
    // multiply is the named spelling of `*`, including its linear algebra:
    // the branches below pick matvec, GEMM, outer and inner products off
    // the operand views and the result type, which the alias shares.
    if (e.name == "Times__" || (e.name == "multiply" && e.args.size() == 2)) {
      actuals.require_arity(2);
      Val a = actuals.at(0).value();
      Val b = actuals.at(1).value();
      // Scalar on either side is an elementwise scale, whatever shape the
      // other operand carries.
      const bool a_scalar = is_scalar(a);
      const bool b_scalar = is_scalar(b);
      if (a_scalar || b_scalar) {
        const Val& shaped = a_scalar ? b : a;
        SlotInfo si = shaped.si;
        si.param_free = a.si.param_free && b.si.param_free;
        const int64_t n = a_scalar ? g.slots[b.slot].len : g.slots[a.slot].len;
        return with_layout(emit_value(OP_MUL, {a, b}, n, si),
                           elementwise_layout({a, b}));
      }
      if (is_matrix(a.si)) {
        if (a.si.param_free && is_vector(b.si)) {
          // Data matrix * vector keeps the tuned MATVEC kernel (its
          // accumulation order is matched to the var path).
          if (g.slots[b.slot].len != a.si.cols)
            fail(e.name + ": inner dimension mismatch", e.raw);
          return with_layout(
              emit_value(OP_MATVEC, {a, b}, a.si.rows, view_of("UVector"),
                         {(int)a.si.rows, (int)a.si.cols}),
              owning_layout(view_of("UVector")));
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
        SlotInfo si =
            e.type_ == "UMatrix"
                ? matrix_view(a.si.rows, cb)
                : (cb == 1 ? view_of("UVector") : matrix_view(a.si.rows, cb));
        Val v = emit_value(OP_GEMM, {a, b}, a.si.rows * cb, si,
                           {(int)a.si.rows, (int)a.si.cols, (int)cb});
        return with_layout(v, owning_layout(si));
      }
      // vector * row_vector with a matrix result is an outer product.
      if (is_vector(a.si) && is_row_vector(b.si) && e.type_ == "UMatrix") {
        const int64_t nr = g.slots[a.slot].len, nc = g.slots[b.slot].len;
        SlotInfo si = matrix_view(nr, nc);
        return with_layout(
            emit_value(OP_GEMM, {a, b}, nr * nc, si, {(int)nr, 1, (int)nc}),
            owning_layout(si));
      }
      if (is_row_vector(a.si) && is_matrix(b.si)) {
        const int64_t k = g.slots[a.slot].len;
        if (k != b.si.rows) fail(e.name + ": inner dimension mismatch", e.raw);
        return with_layout(
            emit_value(OP_GEMM, {a, b}, b.si.cols, view_of("URowVector"),
                       {1, (int)k, (int)b.si.cols}),
            owning_layout(view_of("URowVector")));
      }
      // row_vector * vector with scalar result type is an inner product.
      if (is_row_vector(a.si) && is_vector(b.si) &&
          (e.type_ == "UReal" || e.type_ == "UInt")) {
        if (g.slots[a.slot].len != g.slots[b.slot].len)
          fail(e.name + ": inner dimension mismatch", e.raw);
        return with_layout(emit_value(OP_DOT, {a, b}, 1),
                           ExpressionLayout::scalar());
      }
      if (a.si.kind != ViewKind::Flat || b.si.kind != ViewKind::Flat)
        fail(e.name + ": unsupported container product", e.raw);
      const int64_t len = std::max(g.slots[a.slot].len, g.slots[b.slot].len);
      return with_layout(emit_value(OP_MUL, {a, b}, len),
                         elementwise_layout({a, b}));
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
    if (regular != nullptr && regular->kind == RegularKind::Binary) {
      actuals.require_arity(2);
      Val a = actuals.at(0).value();
      Val b = actuals.at(1).value();
      const int64_t la = g.slots[a.slot].len, lb = g.slots[b.slot].len;
      const bool as = is_scalar(a), bs = is_scalar(b);
      if (!as && !bs && !same_view(a.si, la, b.si, lb))
        fail(e.name + ": incompatible logical views");
      // Elementwise results keep the matrix shape of whichever operand
      // has one; losing it would make a later Times__ miss the matvec.
      SlotInfo si = shape_of(a, b);
      const int64_t n = as ? lb : (bs ? la : la);
      return with_layout(emit_value(regular->opcode, {a, b}, n, si),
                         elementwise_layout({a, b}));
    }

    if (regular != nullptr && (regular->kind == RegularKind::BinaryIntFirst ||
                               regular->kind == RegularKind::BinaryIntSecond)) {
      return lower_binary_int(regular->opcode,
                              regular->kind == RegularKind::BinaryIntFirst,
                              actuals);
    }

    if (regular != nullptr && regular->kind == RegularKind::Unary) {
      actuals.require_arity(1);
      Val a = actuals.at(0).value();
      SlotInfo si = a.si;
      // Shape-preserving unaries keep rows/cols (softmax/cumulative_sum
      // are vector-only, so they never carry one).
      if (regular->opcode != OP_SOFTMAX && regular->opcode != OP_CUMSUM) {
        if (e.type_ == "UMatrix" && !is_matrix(si))
          fail(e.name + ": matrix result has unknown logical extents", e.raw);
        stamp_kind(&si, e.type_);
      } else {
        si = view_of(e.type_);
      }
      si.param_free = a.si.param_free;
      const bool packet_supported = regular->opcode != OP_SOFTMAX &&
                                    regular->opcode != OP_LOG_SOFTMAX &&
                                    regular->opcode != OP_CUMSUM;
      // These functions return freshly allocated Eigen containers. Their
      // result starts at lane zero independently of the input's provenance;
      // the other unary operations are elementwise evaluator expressions.
      const ExpressionLayout layout =
          packet_supported ? elementwise_layout({a}) : owning_layout(si);
      return with_layout(
          emit_value(regular->opcode, {a}, g.slots[a.slot].len, si), layout);
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
    if (e.name == "mean") {
      actuals.require_arity(1);
      Val a = actuals.at(0).value();
      return with_layout(emit_value(OP_MEAN, {a}, 1),
                         ExpressionLayout::scalar());
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
      if ((!is_vector(a.si) && !is_row_vector(a.si)) ||
          g.slots[a.slot].len <= 0)
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
    if (e.name == "sd" || e.name == "variance") {
      actuals.require_arity(1);
      Val a = actuals.at(0).value();
      if (g.slots[a.slot].len <= 0)
        fail(e.name + ": input must have a positive size", e.raw);
      return with_layout(
          emit_value(e.name == "sd" ? OP_SD : OP_VARIANCE, {a}, 1),
          ExpressionLayout::scalar());
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
    if ((e.name == "zeros_vector" || e.name == "zeros_row_vector" ||
         e.name == "ones_vector" || e.name == "ones_row_vector") &&
        e.args.size() == 1) {
      // A broadcast of the constant fill, exactly as rep_vector lowers.
      actuals.require_arity(1);
      const long n = actuals.at(0).require_constant_int("vector extent");
      const double fill = e.name.rfind("ones", 0) == 0 ? 1.0 : 0.0;
      (void)checked_container_size({n}, e.name);
      const SlotInfo result_si = view_of(e.type_);
      return with_layout(emit_value(OP_REP_VEC, {constant(fill)}, n, result_si),
                         owning_layout(result_si));
    }
    if (e.name == "log_sum_exp" || e.name == "sum") {
      // The two-argument log_sum_exp overload is resolved through the
      // regular binary registry before reaching this reduction path.
      const bool int_surface =
          e.name == "sum" &&
          (e.type_ == "UInt" || e.unsized.leaf == mir::UnsizedLeaf::Int ||
           (!e.args.empty() &&
            e.args[0].unsized.leaf == mir::UnsizedLeaf::Int));
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
      if (e.args.size() != 1)
        fail(e.name + ": reduction needs exactly one argument", e.raw);
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
      return with_layout(
          emit_value(e.name == "sum" ? OP_SUM_VEC : OP_LOG_SUM_EXP, {a}, 1),
          ExpressionLayout::scalar());
    }
    if (e.name == "log_mix" && e.args.size() == 3) {
      Val a = actuals.at(0).value();
      Val b = actuals.at(1).value();
      Val c = actuals.at(2).value();
      return with_layout(emit_value(OP_LOG_MIX, {a, b, c}, 1),
                         ExpressionLayout::scalar());
    }
    if (e.name == "dot_product") {
      actuals.require_arity(2);
      Val a = actuals.at(0).value();
      Val b = actuals.at(1).value();
      return with_layout(emit_value(OP_DOT, {a, b}, 1),
                         ExpressionLayout::scalar());
    }

    if (e.name == "dot_self") {
      actuals.require_arity(1);
      Val a = actuals.at(0).value();
      return with_layout(emit_value(OP_DOT, {a, a}, 1),
                         ExpressionLayout::scalar());
    }

    if ((e.name == "columns_dot_product" || e.name == "rows_dot_product" ||
         e.name == "columns_dot_self" || e.name == "rows_dot_self") &&
        (e.args.size() == 1 || e.args.size() == 2)) {
      actuals.require_arity(1, 2);
      Val a = actuals.at(0).value();
      Val b = actuals.size() == 2 ? actuals.at(1).value() : a;
      if (!is_matrix(a.si) || !is_matrix(b.si) || a.si.rows != b.si.rows ||
          a.si.cols != b.si.cols)
        fail(e.name + ": arguments must be matrices of the same size", e.raw);
      SlotInfo product_si =
          matrix_view(a.si.rows, a.si.cols, a.si.param_free && b.si.param_free);
      Val product = with_layout(
          emit_value(OP_MUL, {a, b}, g.slots[a.slot].len, product_si),
          elementwise_layout({a, b}));
      const bool by_columns = e.name.rfind("columns_", 0) == 0;
      const int64_t ones_len = by_columns ? a.si.rows : a.si.cols;
      const int ones_slot = add_slot(ones_len, false);
      out.fills.emplace_back(ones_slot, std::vector<double>(ones_len, 1.0));
      SlotInfo ones_si = view_of(by_columns ? "URowVector" : "UVector");
      ones_si.param_free = true;
      Val ones{ones_slot, false, ones_si, owning_layout(ones_si)};
      if (by_columns)
        return with_layout(emit_value(OP_GEMM, {ones, product}, a.si.cols,
                                      view_of("URowVector"),
                                      {1, (int)a.si.rows, (int)a.si.cols}),
                           owning_layout(view_of("URowVector")));
      return with_layout(
          emit_value(OP_GEMM, {product, ones}, a.si.rows, view_of("UVector"),
                     {(int)a.si.rows, (int)a.si.cols, 1}),
          owning_layout(view_of("UVector")));
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
        result = row == 0
                     ? row_sum
                     : with_layout(emit_value(OP_CONCAT2, {result, row_sum},
                                              row + 1, view_of("UVector")),
                                   owning_layout(view_of("UVector")));
      }
      result.si = view_of("UVector");
      result.layout = owning_layout(result.si);
      return result;
    }

    // squared_distance(x, y) = dot_self(x - y). Two graph kernels that
    // already carry native adjoints, so no new opcode. It does not go
    // through shape_of: the language pairs a vector with a row_vector
    // here, which same_view rejects and stan-math accepts, and the only
    // thing the difference could change -- element order -- is the same
    // on both sides because a length is all either view carries.
    if (e.name == "squared_distance" && e.args.size() == 2) {
      Val a = actuals.at(0).value();
      Val b = actuals.at(1).value();
      const int64_t la = g.slots[a.slot].len, lb = g.slots[b.slot].len;
      if (la != lb) fail(e.name + ": arguments must match in size", e.raw);
      SlotInfo si;
      si.param_free = a.si.param_free && b.si.param_free;
      if (la > 1) si.kind = ViewKind::Vector;
      Val d = with_layout(emit_value(OP_SUB, {a, b}, la, si),
                          elementwise_layout({a, b}));
      return with_layout(emit_value(OP_DOT, {d, d}, 1),
                         ExpressionLayout::scalar());
    }
    return std::nullopt;
  }

  // Matrix shape and algebra: transposes, reshapes, factorizations,
  // slices, and concatenations.
  std::optional<Val> lower_matrix_fn(const mir::Expr& e,
                                     CallArguments& actuals) {
    if ((e.name == "Transpose__" || e.name == "transpose") &&
        e.args.size() == 1) {
      Val a = actuals.at(0).value();
      // Vector <-> row_vector transpose is a type change, not a layout one.
      if (!is_matrix(a.si)) {
        stamp_kind(&a.si, e.type_);
        return a;
      }
      SlotInfo si = matrix_view(a.si.cols, a.si.rows, a.si.param_free);
      return with_layout(emit_value(OP_TRANSPOSE, {a}, g.slots[a.slot].len, si,
                                    {(int)a.si.rows, (int)a.si.cols}),
                         ExpressionLayout::unknown());
    }
    if (e.name == "tcrossprod" && e.args.size() == 1) {
      Val a = actuals.at(0).value();
      if (!is_matrix(a.si)) fail("tcrossprod: needs a matrix", e.raw);
      SlotInfo transpose_si =
          matrix_view(a.si.cols, a.si.rows, a.si.param_free);
      Val transpose = with_layout(
          emit_value(OP_TRANSPOSE, {a}, g.slots[a.slot].len, transpose_si,
                     {(int)a.si.rows, (int)a.si.cols}),
          a.layout);
      SlotInfo si = matrix_view(a.si.rows, a.si.rows, a.si.param_free);
      return with_layout(
          emit_value(OP_GEMM, {a, transpose}, a.si.rows * a.si.rows, si,
                     {(int)a.si.rows, (int)a.si.cols, (int)a.si.rows}),
          owning_layout(si));
    }
    if (e.name == "crossprod" && e.args.size() == 1) {
      Val a = actuals.at(0).value();
      if (!is_matrix(a.si)) fail("crossprod: needs a matrix", e.raw);
      SlotInfo si = matrix_view(a.si.cols, a.si.cols, a.si.param_free);
      Val v = emit_value(OP_CROSSPROD, {a}, a.si.cols * a.si.cols, si,
                         {checked_immediate(a.si.rows, "crossprod rows"),
                          checked_immediate(a.si.cols, "crossprod cols")});
      g.ops.back().variant = v.autodiff ? 1u : 0u;
      return with_layout(v, owning_layout(si));
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
    if (e.name == "multiply_lower_tri_self_transpose" && e.args.size() == 1) {
      // Not L * L': stan-math drops L's upper triangle first, and only a
      // cholesky_factor_* value already has zeros there. A TRANSPOSE/GEMM
      // pair would read the dropped entries and disagree on every result
      // touching one.
      Val L = actuals.at(0).value();
      if (!is_matrix(L.si)) fail("multiply_lower_tri: needs a matrix", e.raw);
      SlotInfo si = matrix_view(L.si.rows, L.si.rows, L.si.param_free);
      Val v = with_layout(
          emit_value(OP_MULT_LOWER_TRI_SELF_TRANSPOSE, {L},
                     L.si.rows * L.si.rows, si,
                     {checked_immediate(L.si.rows, "multiply_lower_tri rows"),
                      checked_immediate(L.si.cols, "multiply_lower_tri cols")}),
          owning_layout(si));
      g.ops.back().variant = v.autodiff ? 1u : 0u;
      return v;
    }
    if (e.name == "to_matrix" && (e.args.size() == 1 || e.args.size() == 3)) {
      // Col-major storage makes reshaping a relabelling. One argument on an
      // array[N] vector[S] value yields the N x S matrix stan-math builds
      // from it, which is the transpose of our array-major flat order.
      Val a = actuals.at(0).value();
      SlotInfo si;
      si.param_free = a.si.param_free;
      if (e.args.size() == 3) {
        const int64_t rows =
            actuals.at(1).require_constant_int("to_matrix rows");
        const int64_t cols =
            actuals.at(2).require_constant_int("to_matrix cols");
        if (checked_product({rows, cols}, "to_matrix") != g.slots[a.slot].len)
          fail("to_matrix: requested shape does not match source length",
               e.raw);
        si = matrix_view(rows, cols, a.si.param_free);
        return Val{a.slot, a.autodiff, si, owning_layout(si)};
      }
      if (is_matrix(a.si)) return Val{a.slot, a.autodiff, a.si, a.layout};
      if (is_vector(a.si)) {
        const SlotInfo result_si =
            matrix_view(g.slots[a.slot].len, 1, a.si.param_free);
        return Val{a.slot, a.autodiff, result_si, owning_layout(result_si)};
      }
      if (is_row_vector(a.si)) {
        const SlotInfo result_si =
            matrix_view(1, g.slots[a.slot].len, a.si.param_free);
        return Val{a.slot, a.autodiff, result_si, owning_layout(result_si)};
      }
      std::vector<int64_t> dims;
      if (is_array(a.si)) dims = array_shape(a.si).dims;
      if (dims.size() != 2) fail("to_matrix: unknown source shape", e.raw);
      if (dims[0] == 0) dims[1] = 0;
      // array-major (row-major) source -> col-major matrix of the same
      // logical shape: transpose the storage.
      si = matrix_view(dims[0], dims[1], a.si.param_free);
      return with_layout(emit_value(OP_TRANSPOSE, {a}, g.slots[a.slot].len, si,
                                    {(int)dims[1], (int)dims[0]}),
                         owning_layout(si));
    }
    if ((e.name == "to_vector" || e.name == "to_row_vector") &&
        e.args.size() == 1) {
      // Col-major flattening is the identity on our storage.
      Val a = actuals.at(0).value();
      SlotInfo si = view_of(e.type_);
      si.param_free = a.si.param_free;
      return Val{a.slot, a.autodiff, si, owning_layout(si)};
    }
    if (e.name == "to_array_1d" && e.args.size() == 1) {
      Val a = actuals.at(0).value();
      SlotInfo si =
          array_view({g.slots[a.slot].len}, ViewKind::Flat, a.si.param_free);
      return Val{a.slot, a.autodiff, si, owning_layout(si)};
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
    if (e.name == "gp_exp_quad_cov" && e.args.size() == 3) {
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
      return with_layout(emit_value(OP_GP_EXP_QUAD_COV, {x, alpha, rho}, N * N,
                                    si, {(int)N, (int)D}),
                         owning_layout(si));
    }
    if (e.name == "diag_matrix" && e.args.size() == 1) {
      Val v = actuals.at(0).value();
      const int64_t n = g.slots[v.slot].len;
      SlotInfo si = matrix_view(n, n);
      return with_layout(emit_value(OP_DIAG_MATRIX, {v}, n * n, si),
                         owning_layout(si));
    }
    if (e.name == "cholesky_decompose" && e.args.size() == 1) {
      Val a = actuals.at(0).value();
      if (!is_matrix(a.si)) fail("cholesky_decompose needs a matrix", e.raw);
      if (a.si.rows != a.si.cols)
        fail("cholesky_decompose needs a square matrix", e.raw);
      SlotInfo si = a.si;
      si.param_free = a.si.param_free;
      return with_layout(emit_value(OP_CHOLESKY, {a}, g.slots[a.slot].len, si,
                                    {(int)a.si.rows}),
                         owning_layout(si));
    }
    if (e.name == "matrix_exp" && e.args.size() == 1) {
      Val a = actuals.at(0).value();
      if (!is_matrix(a.si)) fail("matrix_exp: needs a matrix", e.raw);
      if (a.si.rows != a.si.cols)
        fail("matrix_exp: needs a square matrix", e.raw);
      if (has_runtime_shape(a)) {
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
      return with_layout(
          emit_value(OP_MATRIX_EXP, {a}, g.slots[a.slot].len, a.si,
                     {checked_immediate(a.si.rows, "matrix_exp extent")}),
          owning_layout(a.si));
    }
    if ((e.name == "inverse" || e.name == "inverse_spd") &&
        e.args.size() == 1) {
      Val a = actuals.at(0).value();
      if (!is_matrix(a.si)) fail(e.name + ": needs a matrix", e.raw);
      if (a.si.rows != a.si.cols)
        fail(e.name + ": needs a square matrix", e.raw);
      Val v = emit_value(e.name == "inverse" ? OP_INVERSE : OP_INVERSE_SPD, {a},
                         g.slots[a.slot].len, a.si,
                         {checked_immediate(a.si.rows, e.name + " extent")});
      if (e.name == "inverse_spd") g.ops.back().variant = v.autodiff ? 1u : 0u;
      return with_layout(v, owning_layout(a.si));
    }
    if (e.name == "log_determinant" && e.args.size() == 1) {
      Val a = actuals.at(0).value();
      if (!is_matrix(a.si)) fail("log_determinant: needs a matrix", e.raw);
      if (a.si.rows != a.si.cols)
        fail("log_determinant: needs a square matrix", e.raw);
      return with_layout(
          emit_value(OP_LOG_DETERMINANT, {a}, 1, {},
                     {checked_immediate(a.si.rows, "log_determinant extent")}),
          ExpressionLayout::scalar());
    }

    if ((e.name == "eigenvalues_sym" || e.name == "eigenvectors_sym") &&
        e.args.size() == 1) {
      Val a = actuals.at(0).value();
      if (!is_matrix(a.si)) fail(e.name + ": needs a matrix", e.raw);
      if (a.si.rows != a.si.cols)
        fail(e.name + ": needs a square matrix", e.raw);
      const int64_t n = a.si.rows;
      if (e.name == "eigenvalues_sym")
        return with_layout(
            emit_value(OP_EIGENVALUES_SYM, {a}, n, view_of(e.type_), {(int)n}),
            owning_layout(view_of(e.type_)));
      SlotInfo si = matrix_view(n, n);
      return with_layout(
          emit_value(OP_EIGENVECTORS_SYM, {a}, n * n, si, {(int)n}),
          owning_layout(si));
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

    if (e.name == "quad_form_sym" && e.args.size() == 2) {
      // 0.5 * (C + C') with C = B' A B, and the plain scalar B' A B when B
      // is a vector. This stays one op rather than a transpose and two
      // GEMMs because stan-math's own association is part of the answer:
      // the kernel makes the same calls CmdStan does, including the
      // symmetry check on A, which throws when A is only nearly symmetric.
      Val a = actuals.at(0).value();
      Val b = actuals.at(1).value();
      if (!is_matrix(a.si)) fail("quad_form_sym: needs a matrix", e.raw);
      if (a.si.rows != a.si.cols)
        fail("quad_form_sym: needs a square matrix", e.raw);
      const bool b_matrix = is_matrix(b.si);
      if (!b_matrix && !is_vector(b.si))
        fail("quad_form_sym: second argument is not a matrix or vector", e.raw);
      const int64_t n = a.si.rows;
      const int64_t rb = b_matrix ? b.si.rows : g.slots[b.slot].len;
      const int64_t m = b_matrix ? b.si.cols : 1;
      if (rb != n)
        fail("quad_form_sym: inner dimension mismatch (" + std::to_string(n) +
                 "x" + std::to_string(n) + " against " + std::to_string(rb) +
                 ")",
             e.raw);
      const SlotInfo si = b_matrix ? matrix_view(m, m) : SlotInfo{};
      Val v = emit_value(OP_QUAD_FORM_SYM, {a, b}, m * m, si,
                         {checked_immediate(n, "quad_form_sym extent"),
                          checked_immediate(m, "quad_form_sym extent")});
      // Bit 0 is the operand shape. Bit 1 says CmdStan would have typed
      // this expression `var`, which for a vector B picks stan-math's other
      // association of the same product -- the same distinction the matrix
      // solves make, and for the same reason.
      g.ops.back().variant =
          (uint8_t)((b_matrix ? 0u : 1u) | (v.autodiff ? 2u : 0u));
      return with_layout(
          v, b_matrix ? owning_layout(si) : ExpressionLayout::scalar());
    }

    if (e.name == "quad_form" && e.args.size() == 2) {
      Val a = actuals.at(0).value();
      Val b = actuals.at(1).value();
      if (!is_matrix(a.si)) fail("quad_form: needs a matrix", e.raw);
      if (a.si.rows != a.si.cols)
        fail("quad_form: needs a square matrix", e.raw);
      const bool b_matrix = is_matrix(b.si);
      if (!b_matrix && !is_vector(b.si))
        fail("quad_form: second argument is not a matrix or vector", e.raw);
      const int64_t n = a.si.rows;
      const int64_t rb = b_matrix ? b.si.rows : g.slots[b.slot].len;
      const int64_t m = b_matrix ? b.si.cols : 1;
      if (rb != n)
        fail("quad_form: inner dimension mismatch (" + std::to_string(n) + "x" +
                 std::to_string(n) + " against " + std::to_string(rb) + ")",
             e.raw);
      const SlotInfo si = b_matrix ? matrix_view(m, m) : SlotInfo{};
      Val v = emit_value(OP_QUAD_FORM, {a, b}, m * m, si,
                         {checked_immediate(n, "quad_form extent"),
                          checked_immediate(m, "quad_form extent")});
      g.ops.back().variant =
          (uint8_t)((b_matrix ? 0u : 1u) | (v.autodiff ? 2u : 0u));
      return with_layout(
          v, b_matrix ? owning_layout(si) : ExpressionLayout::scalar());
    }

    if (e.name == "add_diag" && e.args.size() == 2) {
      Val a = actuals.at(0).value();
      Val d = actuals.at(1).value();
      if (!is_matrix(a.si)) fail("add_diag: needs a matrix", e.raw);
      const bool scalar = is_scalar(d);
      const int64_t n = std::min(a.si.rows, a.si.cols);
      if (!scalar && !is_vector(d.si) && !is_row_vector(d.si))
        fail("add_diag: diagonal must be a scalar or vector", e.raw);
      if (!scalar && g.slots[d.slot].len != n)
        fail("add_diag: diagonal length mismatch", e.raw);
      Val v = emit_value(OP_ADD_DIAG, {a, d}, g.slots[a.slot].len, a.si,
                         {checked_immediate(a.si.rows, "add_diag rows"),
                          checked_immediate(a.si.cols, "add_diag cols")});
      g.ops.back().variant = scalar ? 1u : 0u;
      return with_layout(v, owning_layout(a.si));
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
    if (e.name == "segment" && e.args.size() == 3) {
      Val a = actuals.at(0).value();
      const long from = actuals.at(1).require_constant_int("segment start");
      const long cnt = actuals.at(2).require_constant_int("segment count");
      const int64_t offset = from - 1;
      const int immediate = checked_immediate(offset, "segment offset");
      return with_layout(
          emit_value(OP_SLICE, {a}, cnt, view_of(e.type_), {immediate}),
          contiguous_layout(a, offset, "segment"));
    }
    if (e.name == "sub_col" && e.args.size() == 4) {
      // sub_col(M, i, j, n) = M[i .. i+n-1, j]: contiguous in col-major.
      Val a = actuals.at(0).value();
      if (!is_matrix(a.si)) fail("sub_col on a slot without matrix shape");
      const long i = actuals.at(1).require_constant_int("sub_col row");
      const long j = actuals.at(2).require_constant_int("sub_col column");
      const long n = actuals.at(3).require_constant_int("sub_col count");
      const int64_t offset = (j - 1) * a.si.rows + i - 1;
      const int immediate = checked_immediate(offset, "sub_col offset");
      return with_layout(
          emit_value(OP_SLICE, {a}, n, view_of(e.type_), {immediate}),
          contiguous_layout(a, offset, "sub_col"));
    }
    if (e.name == "block" && e.args.size() == 5) {
      // block(M, i, j, nr, nc) = M[i .. i+nr-1, j .. j+nc-1]. Each result
      // column is contiguous in col-major storage, but consecutive result
      // columns sit M.rows apart, so a 2-D window needs a gather rather
      // than the single slice sub_col gets.
      Val a = actuals.at(0).value();
      if (!is_matrix(a.si)) fail("block on a slot without matrix shape");
      const long i = actuals.at(1).require_constant_int("block row");
      const long j = actuals.at(2).require_constant_int("block column");
      const long nr = actuals.at(3).require_constant_int("block rows");
      const long nc = actuals.at(4).require_constant_int("block columns");
      check_block_shape(a.si.rows, a.si.cols, i, j, nr, nc);
      std::vector<int> gather;
      gather.reserve((size_t)(nr * nc));
      for (long c = 0; c < nc; ++c)
        for (long k = 0; k < nr; ++k)
          gather.push_back(checked_immediate(
              (j - 1 + c) * a.si.rows + (i - 1 + k), "block gather offset"));
      return with_layout(
          emit_value(OP_GATHER, {a}, nr * nc, matrix_view(nr, nc), gather),
          ExpressionLayout::scalar());
    }
    if (e.name == "col" && e.args.size() == 2) {
      Val a = actuals.at(0).value();
      if (!is_matrix(a.si)) fail("col on a slot without matrix shape");
      const long j = actuals.at(1).require_constant_int("col index");
      const int64_t offset = (j - 1) * a.si.rows;
      const int immediate = checked_immediate(offset, "col offset");
      return with_layout(
          emit_value(OP_SLICE, {a}, a.si.rows, view_of(e.type_), {immediate}),
          contiguous_layout(a, offset, "col"));
    }
    if (e.name == "diagonal" && e.args.size() == 1) {
      Val a = actuals.at(0).value();
      if (!is_matrix(a.si)) fail("diagonal on a slot without matrix shape");
      // Eigen's diagonal steps one row and one column at a time, which in
      // column-major storage is rows + 1 apart, and stops at the shorter
      // side.
      const int64_t n = std::min(a.si.rows, a.si.cols);
      return with_layout(
          emit_value(OP_SLICE_STRIDED, {a}, n, view_of(e.type_),
                     {0, checked_immediate(a.si.rows + 1, "diagonal stride")}),
          ExpressionLayout::scalar());
    }
    if (e.name == "row" && e.args.size() == 2) {
      Val a = actuals.at(0).value();
      if (!is_matrix(a.si)) fail("row on a slot without matrix shape");
      const long i = actuals.at(1).require_constant_int("row index");
      const int64_t offset = i - 1;
      return with_layout(
          emit_value(OP_SLICE_STRIDED, {a}, a.si.cols, view_of(e.type_),
                     {checked_immediate(offset, "row offset"),
                      checked_immediate(a.si.rows, "row stride")}),
          ExpressionLayout::scalar());
    }
    if ((e.name == "head" || e.name == "tail") && e.args.size() == 2) {
      Val a = actuals.at(0).value();
      const long n = actuals.at(1).require_constant_int("head/tail count");
      const long off = e.name == "head" ? 0 : g.slots[a.slot].len - n;
      const int immediate = checked_immediate(off, e.name + " offset");
      return with_layout(
          emit_value(OP_SLICE, {a}, n, view_of(e.type_), {immediate}),
          contiguous_layout(a, off, e.name));
    }
    if (e.name == "reverse" && e.args.size() == 1) {
      Val a = actuals.at(0).value();
      const int64_t len = g.slots[a.slot].len;
      std::vector<int> gather;
      gather.reserve((size_t)len);
      if (is_array(a.si)) {
        // Graph arrays keep each outer element contiguous. Reverse those
        // complete chunks so an array of vectors/matrices retains the order
        // inside every element.
        const ArrayShape& shape = array_shape(a.si);
        if (shape.dims.empty()) fail("reverse: array has no dimensions", e.raw);
        const int64_t outer = shape.dims.front();
        const std::vector<int64_t> suffix(shape.dims.begin() + 1,
                                          shape.dims.end());
        const int64_t width = checked_product(suffix, "reverse array element");
        if (checked_product(shape.dims, "reverse array") != len)
          fail("reverse: array shape does not match storage", e.raw);
        for (int64_t i = outer; i-- > 0;)
          for (int64_t k = 0; k < width; ++k)
            gather.push_back(
                checked_immediate(i * width + k, "reverse gather offset"));
      } else {
        if (!is_vector(a.si) && !is_row_vector(a.si))
          fail("reverse: argument is not a vector, row-vector, or array",
               e.raw);
        for (int64_t i = len; i-- > 0;)
          gather.push_back(checked_immediate(i, "reverse gather offset"));
      }
      return with_layout(emit_value(OP_GATHER, {a}, len, a.si, gather),
                         ExpressionLayout::scalar());
    }
    return std::nullopt;
  }

  // The deprecated algebra_solver interfaces (Powell and Newton):
  //
  //   algebra_solver(f, x, y, x_r, x_i[, rel_tol, f_tol, max_steps])
  //
  // x is an initial guess.  It influences which root is selected but legacy
  // Stan Math intentionally returns value_type_t<y>, so only y participates
  // in autodiff.  Keep x as a graph input for values while stamping the op's
  // activity and result scalar type from y alone.
  Val lower_quadrature_fn(const mir::Expr& e, CallArguments& actuals) {
    const auto call = mir::quadrature_call(e.name);
    if (!call) fail(e.name + ": missing quadrature metadata", e.raw);
    if (e.unsized.depth != 0 || e.unsized.leaf != mir::UnsizedLeaf::Real)
      fail(e.name + ": result must be a real", e.raw);

    size_t callback_end = actuals.size();
    if (call->legacy) {
      if (actuals.size() != 6 && actuals.size() != 7)
        fail(e.name + ": expected 6 or 7 arguments", e.raw);
      callback_end = 6;
    } else if (call->with_tolerance) {
      if (actuals.size() < 6)
        fail(e.name + ": expected controls followed by callback arguments",
             e.raw);
    } else if (actuals.size() < 3) {
      fail(e.name + ": expected callback and integration bounds", e.raw);
    }
    if (e.args[0].kind != mir::Expr::Var)
      fail(e.name + ": integrand is not a function name", e.raw);

    std::vector<mir::UnsizedView> views{{0, mir::UnsizedLeaf::Real},
                                        {0, mir::UnsizedLeaf::Real}};
    for (size_t i = call->callback_args_begin; i < callback_end; ++i)
      views.push_back(e.args[i].unsized);
    const mir::FunDef* integrand =
        mir::resolve_callback(fun_defs, e.args[0].name, views);
    if (!integrand)
      fail(e.name + ": unknown integrand " + e.args[0].name, e.raw);

    auto spec = std::make_shared<QuadratureSpec>();
    spec->adopt(fun_defs);
    spec->callback_name = integrand->name;
    spec->method = call->method;
    if (call->legacy && actuals.size() == 7) {
      spec->relative_tolerance =
          actuals.at(6).require_constant_reals("quadrature tolerance").at(0);
    } else if (call->with_tolerance) {
      spec->relative_tolerance =
          actuals.at(3)
              .require_constant_reals("quadrature relative tolerance")
              .at(0);
      spec->absolute_tolerance =
          actuals.at(4)
              .require_constant_reals("quadrature absolute tolerance")
              .at(0);
      spec->max_steps = static_cast<int>(
          actuals.at(5).require_constant_int("quadrature maximum steps"));
    }

    std::vector<Val> active = pack_callback_arguments<Val>(
        *spec, e.args, call->callback_args_begin, callback_end,
        [&](size_t i) {
          Val value = actuals.at(i).value();
          if (g.slots[value.slot].len > std::numeric_limits<int>::max())
            fail(e.name + ": callback argument is too large", e.raw);
          return std::make_pair(value,
                                static_cast<int>(g.slots[value.slot].len));
        },
        [&](size_t i) {
          const auto& values =
              actuals.at(i).require_constant_reals("quadrature data argument");
          return std::vector<double>(values.begin(), values.end());
        },
        [&](size_t i) {
          const auto& values = actuals.at(i).require_constant_ints(
              "quadrature integer argument");
          return std::vector<int>(values.begin(), values.end());
        },
        [&](const std::string& message) {
          fail(e.name + ": " + message, e.raw);
        });

    Val theta = constant(0.0);  // unread placeholder when there are no params
    spec->parameter_count = 0;
    if (!active.empty()) {
      theta = active.front();
      spec->parameter_count = static_cast<int>(g.slots[theta.slot].len);
      for (size_t i = 1; i < active.size(); ++i) {
        const int64_t add = g.slots[active[i].slot].len;
        if (add > std::numeric_limits<int>::max() - spec->parameter_count)
          fail(e.name + ": active callback arguments are too large", e.raw);
        theta = emit_value(OP_CONCAT2, {theta, active[i]},
                           spec->parameter_count + add);
        spec->parameter_count += static_cast<int>(add);
      }
    }
    spec->prog =
        compile_rhs_args(*spec->callback(), *spec->funs(), 1, spec->args);

    Val a = actuals.at(1).value();
    Val b = actuals.at(2).value();
    if (!is_scalar(a) || !is_scalar(b))
      fail(e.name + ": integration bounds must be scalar", e.raw);
    const uint8_t variant = static_cast<uint8_t>(
        (a.autodiff ? 0x1u : 0u) | (b.autodiff ? 0x2u : 0u) |
        (spec->parameter_count != 0 ? 0x4u : 0u));
    SlotInfo si = view_of(e.type_);
    si.param_free = variant == 0;
    Val result = emit_raw(OP_QUADRATURE, {a.slot, b.slot, theta.slot}, 1, si,
                          {}, -1, variant != 0);
    g.ops.back().variant = variant;
    g.ops.back().udata = spec.get();
    g.udata_pool.push_back(std::move(spec));
    return result;
  }

  Val lower_algebra_fn(const mir::Expr& e, CallArguments& actuals) {
    if (actuals.size() != 5 && actuals.size() != 8)
      fail(e.name + ": expected 5 or 8 arguments", e.raw);
    if (e.unsized.leaf != mir::UnsizedLeaf::Vector || e.unsized.depth != 0)
      fail("algebra_solver: result must be a vector", e.raw);

    // Argument zero is a callback name, not a value acquisition. It must stay
    // source-level so the callback can be retained in AlgebraSpec.
    const mir::Expr& system_expr = actuals.at(0).expr();
    const std::vector<mir::UnsizedView> views{{0, mir::UnsizedLeaf::Vector},
                                              {0, mir::UnsizedLeaf::Vector},
                                              {1, mir::UnsizedLeaf::Real},
                                              {1, mir::UnsizedLeaf::Int}};
    const mir::FunDef* resolved =
        mir::resolve_callback(fun_defs, system_expr.name, views);
    if (resolved == nullptr)
      fail("algebra_solver: unknown algebraic system " + system_expr.name,
           e.raw);
    const mir::FunDef& f = *resolved;
    if (f.arg_views.size() != 4 || f.arg_names.size() != 4 ||
        f.arg_types.size() != 4 || f.arg_views[0].depth != 0 ||
        f.arg_views[0].leaf != mir::UnsizedLeaf::Vector ||
        f.arg_views[1].depth != 0 ||
        f.arg_views[1].leaf != mir::UnsizedLeaf::Vector ||
        f.arg_views[2].depth != 1 ||
        f.arg_views[2].leaf != mir::UnsizedLeaf::Real ||
        f.arg_views[3].depth != 1 ||
        f.arg_views[3].leaf != mir::UnsizedLeaf::Int)
      fail(
          "algebra_solver: system must take (vector, vector, array[] real, "
          "array[] int)",
          e.raw);

    auto spec = std::make_shared<AlgebraSpec>();
    spec->adopt(fun_defs);
    spec->system_name = f.name;
    spec->select(*mir::algebra_call(e.name));
    spec->x_r = actuals.at(3).require_constant_reals("algebra_solver x_r");
    spec->x_i = actuals.at(4).require_constant_ints("algebra_solver x_i");
    if (actuals.size() == 8) {
      spec->relative_tolerance =
          actuals.at(5)
              .require_constant_reals("algebra_solver relative tolerance")
              .at(0);
      spec->function_tolerance =
          actuals.at(6)
              .require_constant_reals("algebra_solver function tolerance")
              .at(0);
      spec->max_num_steps =
          actuals.at(7).require_constant_int("algebra_solver maximum steps");
    }

    Val x = actuals.at(1).value();
    Val y = actuals.at(2).value();
    if (!is_vector(x.si) || !is_vector(y.si))
      fail("algebra_solver: initial guess and parameters must be vectors",
           e.raw);
    const int64_t n = g.slots[x.slot].len;
    if (n > std::numeric_limits<int>::max() ||
        g.slots[y.slot].len > std::numeric_limits<int>::max() ||
        spec->x_r.size() > (size_t)std::numeric_limits<int>::max())
      fail(
          "algebra_solver: argument is too large for the callback register "
          "program",
          e.raw);

    std::vector<RhsArg> args(3);
    args[0].is_param = true;
    args[0].len = (int)g.slots[y.slot].len;
    args[1].len = (int)spec->x_r.size();
    args[2].is_int = true;
    args[2].ints = spec->x_i;
    spec->prog = compile_rhs_args(with_leading_time(*spec->system()),
                                  *spec->funs(), (int)n, args);
    if (!spec->prog.ok && std::getenv("STANLI_DEBUG_ALGEBRA"))
      std::fprintf(stderr,
                   "stanli: algebraic system %s falls back to the "
                   "interpreter: %s\n",
                   spec->system_name.c_str(), spec->prog.why.c_str());

    SlotInfo si = view_of(e.type_);
    si.param_free = x.si.param_free && y.si.param_free;
    Val result = emit_raw(OP_ALGEBRA_SOLVER, {x.slot, y.slot}, n, si, {}, -1,
                          y.autodiff);
    g.ops.back().variant = y.autodiff ? 0x1u : 0x0u;
    g.ops.back().udata = spec.get();
    g.udata_pool.push_back(std::move(spec));
    return result;
  }

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
               std::optional<Val> ts = std::nullopt) {
    // Falling back to the interpreter is correct but ~30x slower, so make
    // it findable rather than silent.
    if (!spec->prog.ok && std::getenv("STANLI_DEBUG_ODE"))
      std::fprintf(stderr,
                   "stanli: ODE right-hand side %s falls back to the "
                   "interpreter: %s\n",
                   spec->rhs_name.c_str(), spec->prog.why.c_str());
    if (spec->prog.ok &&
        (spec->solver == OdeSpec::RK45 || spec->solver == OdeSpec::CKRK)) {
      spec->direct_rk =
          make_rhs_adjoint_program(spec->prog, &spec->direct_rk_why);
      spec->direct_rk_enabled =
          spec->direct_rk && !std::getenv("STANLI_NO_ODE_DIRECT_RK");
    }
    if (spec->prog.ok &&
        (spec->solver == OdeSpec::RK45 || spec->solver == OdeSpec::CKRK) &&
        std::getenv("STANLI_DEBUG_ODE")) {
      if (spec->direct_rk)
        std::fprintf(stderr,
                     "stanli: ODE right-hand side %s is direct-RK eligible%s\n",
                     spec->rhs_name.c_str(),
                     spec->direct_rk_enabled ? "" : " (oracle selected)");
      else
        std::fprintf(stderr,
                     "stanli: ODE right-hand side %s keeps the RK oracle: %s\n",
                     spec->rhs_name.c_str(), spec->direct_rk_why.c_str());
    }
    Val v = t0 && ts ? emit_value(OP_ODE, {z0, theta, *t0, *ts}, N * S,
                                  result_si, {(int)N, (int)S})
                     : emit_value(OP_ODE, {z0, theta}, N * S, result_si,
                                  {(int)N, (int)S});
    // The new four-input form uses bit 4 as its marker and includes initial
    // and output-time scalar types in bits 2 and 3. The old two-input form
    // retains its bit-2 compatibility encoding.
    g.ops.back().variant =
        t0 && ts
            ? (uint8_t)(0x10u | (z0.autodiff ? 0x1u : 0u) |
                        (theta.autodiff ? 0x2u : 0u) |
                        (t0->autodiff ? 0x4u : 0u) | (ts->autodiff ? 0x8u : 0u))
            : (uint8_t)(0x4u | (z0.autodiff ? 0x1u : 0u) |
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
  std::optional<Val> lower_ode_variadic(const mir::Expr& e,
                                        CallArguments& actuals) {
    const auto call = mir::ode_call(e.name);
    if (!call || call->legacy || call->method == mir::OdeMethod::Adjoint)
      return std::nullopt;
    const size_t fixed = call->callback_args_begin;
    if (actuals.size() < fixed) fail(e.name + ": unexpected arity", e.raw);
    auto spec = std::make_shared<OdeSpec>();
    // Argument zero is the callback name retained in OdeSpec, not a lowered
    // value. The remaining fixed arguments are acquired in the historical
    // order below because constant probing can be observable through errors.
    const mir::Expr& rhs_expr = actuals.at(0).expr();
    if (fun_defs.find(rhs_expr.name) == fun_defs.end())
      fail(e.name + ": unknown right-hand side " + rhs_expr.name, e.raw);
    spec->adopt(fun_defs);
    spec->rhs_name = rhs_expr.name;
    spec->solver = ode_solver(call->method);
    spec->stiff =
        spec->solver == OdeSpec::BDF || spec->solver == OdeSpec::ADAMS;
    stamp_ode_defaults(*spec);
    const bool runtime_times = !e.args[2].data_only || !e.args[3].data_only;
    Val t0, ts;
    if (runtime_times) {
      t0 = actuals.at(2).value();
      ts = actuals.at(3).value();
      if (!is_scalar(t0) || !is_array(ts.si))
        fail(e.name + ": initial time or output times has the wrong type",
             e.raw);
    } else {
      spec->t0 = actuals.at(2).require_constant_reals("ODE initial time").at(0);
      spec->ts = actuals.at(3).require_constant_reals("ODE output times");
    }
    if (call->with_tolerance) {
      spec->rtol =
          actuals.at(4).require_constant_reals("ODE relative tolerance").at(0);
      spec->atol =
          actuals.at(5).require_constant_reals("ODE absolute tolerance").at(0);
      spec->max_steps =
          (long)actuals.at(6).require_constant_reals("ODE maximum steps").at(0);
    }

    // Classify and pack. Data arguments fold into the spec here and never
    // reach the graph; autodiff ones are concatenated in argument order
    // into the single theta input the op takes, which is the order
    // compile_rhs_args assigns their register sub-ranges in.
    std::vector<RhsArg> rargs;
    std::vector<Val> param_parts;
    for (size_t k = fixed; k < actuals.size(); ++k) {
      LoweredArgument& actual = actuals.at(k);
      const mir::Expr& a = actual.expr();
      RhsArg ra;
      const bool is_int = a.unsized.leaf == mir::UnsizedLeaf::Int;
      if (is_int && a.data_only) {
        ra.is_int = true;
        ra.ints = actual.require_constant_ints("ODE integer argument");
      } else if (a.data_only) {
        // One evaluation, held in a local. Calling const_values(a) twice
        // and taking begin() from one temporary and end() from the other
        // is an invalid range, and it does not fail loudly: it appended
        // hundreds of garbage doubles to x_r and surfaced much later as
        // "ode parameters and data[927] is nan".
        const std::vector<double>& vals =
            actual.require_constant_reals("ODE data argument");
        ra.len = (int)vals.size();
        spec->x_r.insert(spec->x_r.end(), vals.begin(), vals.end());
      } else {
        if (is_int)
          fail(e.name + ": integer argument " + std::to_string(k - fixed + 1) +
                   " is not data",
               e.raw);
        const Val v = actual.value();
        ra.is_param = true;
        ra.len = (int)g.slots[v.slot].len;
        param_parts.push_back(v);
      }
      rargs.push_back(std::move(ra));
    }

    Val z0 = actuals.at(1).value();
    const int64_t S = g.slots[z0.slot].len;
    const int64_t N =
        runtime_times ? g.slots[ts.slot].len : (int64_t)spec->ts.size();
    if (runtime_times) spec->ts.resize((size_t)N);

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
    if (runtime_times)
      return emit_ode(std::move(spec), z0, theta, N, S,
                      ode_result_view(e, N, S), t0, ts);
    return emit_ode(std::move(spec), z0, theta, N, S, ode_result_view(e, N, S));
  }

  // The integrate_ode_* family.
  std::optional<Val> lower_ode_fn(const mir::Expr& e, CallArguments& actuals) {
    if (auto v = lower_ode_variadic(e, actuals)) return v;
    const auto call = mir::ode_call(e.name);
    if (call && call->legacy) {
      const OdeSpec::Solver legacy_solver =
          call->method == mir::OdeMethod::Rk45  ? OdeSpec::RK45
          : call->method == mir::OdeMethod::Bdf ? OdeSpec::BDF
                                                : OdeSpec::ADAMS;
      // integrate_ode_*(f, z_init, t0, ts, theta, x_r, x_i[, rtol, atol,
      // max_steps]). Everything but z_init and theta is data, and is
      // captured in the spec the kernel reads through the op payload.
      if (actuals.size() < 7) fail(e.name + ": unexpected arity", e.raw);
      auto spec = std::make_shared<OdeSpec>();
      // The callback name is retained as source metadata; all value-bearing
      // actuals use the lazy wrapper so each is acquired once.
      const mir::Expr& rhs_expr = actuals.at(0).expr();
      auto fit = fun_defs.find(rhs_expr.name);
      if (fit == fun_defs.end())
        fail(e.name + ": unknown right-hand side " + rhs_expr.name, e.raw);
      spec->adopt(fun_defs);
      spec->rhs_name = rhs_expr.name;
      spec->legacy = true;
      spec->solver = legacy_solver;
      spec->stiff =
          spec->solver == OdeSpec::BDF || spec->solver == OdeSpec::ADAMS;
      stamp_ode_defaults(*spec);
      spec->t0 = actuals.at(2).require_constant_reals("ODE initial time").at(0);
      spec->ts = actuals.at(3).require_constant_reals("ODE output times");
      spec->x_r = actuals.at(5).require_constant_reals("ODE real data");
      spec->x_i = actuals.at(6).require_constant_ints("ODE integer data");
      if (actuals.size() >= 10) {
        spec->rtol = actuals.at(7)
                         .require_constant_reals("ODE relative tolerance")
                         .at(0);
        spec->atol = actuals.at(8)
                         .require_constant_reals("ODE absolute tolerance")
                         .at(0);
        spec->max_steps = (long)actuals.at(9)
                              .require_constant_reals("ODE maximum steps")
                              .at(0);
      }
      Val z0 = actuals.at(1).value();
      Val theta = actuals.at(4).value();
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
    con.layout = owning_layout(psi);
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

  void lower_stmt_impl(const mir::Stmt& s) {
    switch (s.kind) {
      case mir::Stmt::Decl:
        if (s.read_transform) {
          lower_read_param(s);
        } else if (s.decl_type.base == "SInt") {
          if (s.has_init && in_write_array && runtime_int_binding(s.init)) {
            bind_runtime_int(s.decl_id, s.init, s.raw);
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
          // Only compile-time integers belong in int_env. MIR's DataOnly
          // AD level alone does not make a parameter-selected integer known.
          int_locals.insert(s.decl_id);
          // eval_int, not the interpreter directly: the initializer may be
          // a shape query on a slot-bound value (rows(lscale) inside an
          // inlined function), which only eval_int can answer.
          if (s.has_init) {
            if (auto value = static_int(s.init))
              int_env[s.decl_id] = *value;
            else
              bind_runtime_int(s.decl_id, s.init, s.raw);
          }
        } else if (s.decl_type.base.empty() &&
                   s.decl_type.unsized.leaf != mir::UnsizedLeaf::Unknown) {
          // O1 introduces unsized container temporaries for expressions such
          // as a for-loop sequence built with append_array. C++ assignment
          // gives these locals the RHS shape, so delay allocating or checking
          // their view until the first whole-variable assignment does likewise.
          scope.erase(s.decl_id);
          DeclView sh;
          sh.autodiff = s.decl_type.unsized.leaf != mir::UnsizedLeaf::Int &&
                        !s.decl_data_only && scalar_autodiff();
          sh.int_array = s.decl_type.unsized.leaf == mir::UnsizedLeaf::Int;
          sh.deferred_shape = true;
          if (s.has_init) {
            Val v = lower_expr(s.init);
            sh.len = g.slots[v.slot].len;
            sh.si = v.si;
            sh.deferred_shape = false;
            v.autodiff = sh.autodiff;
            v.layout = owning_layout(v.si);
            scope[s.decl_id] = v;
            sync_data_local(s.decl_id, s.init, v);
          } else {
            td.env().erase(s.decl_id);
          }
          decls[s.decl_id] = sh;
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
            v.layout = owning_layout(v.si);
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
            bind_runtime_int(s.lhs, s.rhs, s.raw);
            return;
          }
          if (auto value = static_int(s.rhs))
            int_env[s.lhs] = *value;
          else
            bind_runtime_int(s.lhs, s.rhs, s.raw);
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
            prev_v = Val{add_slot(dl->second.len, false), dl->second.autodiff,
                         si, owning_layout(si)};
            const double initial =
                dl->second.int_array
                    ? static_cast<double>(std::numeric_limits<int>::min())
                    : std::numeric_limits<double>::quiet_NaN();
            out.fills.emplace_back(
                prev_v.slot, std::vector<double>(dl->second.len, initial));
            if (dl->second.int_array) set_uninitialized_int_array(prev_v);
            observe_fill(prev_v, dl->second.int_array, initial, dl->second.len);
          }
          const int prev = prev_v.slot;
          bool all_single = true;
          for (const auto& ix : s.lhs_idx)
            if (ix.name != "IndexSingle") all_single = false;
          const std::vector<int64_t>* dd =
              is_array(prev_v.si) ? &array_shape(prev_v.si).dims : nullptr;
          const Val rhs_v = lower_expr(s.rhs);
          if (std::any_of(
                  s.lhs_idx.begin(), s.lhs_idx.end(),
                  [&](const mir::Expr& ix) { return runtime_selector(ix); })) {
            Val nv = region_index(prev_v, s.lhs_idx, s.rhs.type_, s.rhs.unsized,
                                  &rhs_v);
            nv.autodiff = prev_v.autodiff;
            scope[s.lhs] = nv;
            sync_indexed_data_local(s.lhs, nv);
            return;
          }
          observe_indexed_rhs(s.rhs, rhs_v);
          const int rhs = rhs_v.slot;
          SlotInfo out_si = prev_v.si;
          // A one-index All spans the complete logical value. Keep this as
          // an indexed functional update rather than silently rewriting the
          // MIR statement: the ordinary binding checks still enforce width
          // and logical view, while the store path preserves integer-array
          // initialization and observation metadata. Matrix `[:, j]` is a
          // separate two-index form below and never enters this branch.
          if (s.lhs_idx.size() == 1 && s.lhs_idx[0].name == "IndexAll") {
            if (is_scalar(prev_v))
              fail("full-span assignment needs a container for " + s.lhs,
                   s.raw);
            require_binding(rhs_v, g.slots[prev].len, prev_v.si, s.lhs, s.raw);
            Val nv = with_layout(emit_value(OP_SET_SLICE, {prev_v, rhs_v},
                                            g.slots[prev].len, out_si, {0}),
                                 owning_layout(out_si));
            propagate_int_update(nv, prev_v, rhs_v, 0, 1);
            scope[s.lhs] = nv;
            sync_indexed_data_local(s.lhs, nv);
            return;
          }
          // Whole matrix row write M[i] = row_vector: one value per column,
          // strided by the physical row count.
          if (s.lhs_idx.size() == 1 && s.lhs_idx[0].name == "IndexSingle" &&
              is_matrix(prev_v.si) && is_row_vector(rhs_v.si)) {
            const int64_t i = eval_int(s.lhs_idx[0].args[0]) - 1;
            if (i < 0 || i >= prev_v.si.rows)
              fail("row assignment index out of bounds for " + s.lhs);
            if (g.slots[rhs].len != prev_v.si.cols)
              fail("row assignment size mismatch for " + s.lhs);
            Val nv =
                with_layout(emit_value(OP_SET_SLICE_STRIDED, {prev_v, rhs_v},
                                       g.slots[prev].len, out_si,
                                       {(int)i, (int)prev_v.si.rows}),
                            owning_layout(out_si));
            propagate_int_update(nv, prev_v, rhs_v, i, prev_v.si.rows);
            scope[s.lhs] = nv;
            sync_indexed_data_local(s.lhs, nv);
            return;
          }
          // Whole vector leaf write A[i, :] = rhs for array[N] vector[S].
          // Graph array storage keeps each outer element contiguous, so this
          // is the assignment mirror of the read path above.
          if (s.lhs_idx.size() == 2 && s.lhs_idx[0].name == "IndexSingle" &&
              s.lhs_idx[1].name == "IndexAll" && dd && dd->size() == 2 &&
              (array_shape(prev_v.si).leaf == ViewKind::Vector ||
               array_shape(prev_v.si).leaf == ViewKind::RowVector)) {
            const int64_t i = eval_int(s.lhs_idx[0].args[0]);
            const int64_t width = (*dd)[1];
            check_index(i, (*dd)[0], "array assignment index", s.raw);
            SlotInfo expected = indexed_view(prev_v.si, 1, width, s.rhs.type_);
            require_binding(rhs_v, width, expected, s.lhs, s.raw);
            const int64_t start = (i - 1) * width;
            Val nv =
                with_layout(emit_value(OP_SET_SLICE, {prev_v, rhs_v},
                                       g.slots[prev].len, out_si, {(int)start}),
                            owning_layout(out_si));
            propagate_int_update(nv, prev_v, rhs_v, start, 1);
            scope[s.lhs] = nv;
            sync_indexed_data_local(s.lhs, nv);
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
            Val nv =
                with_layout(emit_value(OP_SET_SLICE, {prev_v, rhs_v},
                                       g.slots[prev].len, out_si, {(int)start}),
                            owning_layout(out_si));
            propagate_int_update(nv, prev_v, rhs_v, start, 1);
            scope[s.lhs] = nv;
            sync_indexed_data_local(s.lhs, nv);
            return;
          }
          // Scatter write x[idx] = rhs. The indices are data, so spell it as
          // one element write each; repeats then resolve last-wins as CmdStan.
          if (s.lhs_idx.size() == 1 && s.lhs_idx[0].name == "IndexMulti" &&
              !is_matrix(prev_v.si)) {
            DataMap::Entry iv =
                eval_pure(s.lhs_idx[0].args[0], "a scatter index");
            if (!iv.is_int) fail("scatter index must be int data", s.raw);
            if ((int64_t)iv.i.size() != g.slots[rhs].len)
              fail("scatter assignment size mismatch for " + s.lhs);
            Val nv = prev_v;
            for (size_t k = 0; k < iv.i.size(); ++k) {
              check_index(iv.i[k], g.slots[prev].len, "scatter index", s.raw);
              const Val el =
                  emit_value(OP_INDEX, {rhs_v}, 1, view_of("UReal"), {(int)k});
              const Val next =
                  emit_value(OP_SET_INDEX, {nv, el}, g.slots[prev].len, out_si,
                             {(int)(iv.i[k] - 1)});
              propagate_int_update(next, nv, el, iv.i[k] - 1, 1);
              nv = next;
            }
            scope[s.lhs] = nv;
            sync_indexed_data_local(s.lhs, nv);
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
            sync_indexed_data_local(s.lhs, nv);
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
            Val nv =
                with_layout(emit_value(OP_SET_SLICE, {prev_v, rhs_v},
                                       g.slots[prev].len, out_si, {(int)start}),
                            owning_layout(out_si));
            propagate_int_update(nv, prev_v, rhs_v, start, 1);
            scope[s.lhs] = nv;
            sync_indexed_data_local(s.lhs, nv);
            return;
          }
          // Columns outermost, as CmdStan's assign walks them: a repeated
          // index has to resolve last-wins in the same order.
          if (!all_single && s.lhs_idx.size() == 2 && is_matrix(prev_v.si)) {
            const std::vector<int64_t> ri = index_positions(
                s.lhs_idx[0], prev_v.si.rows, "block assignment row", s.raw);
            const std::vector<int64_t> ci = index_positions(
                s.lhs_idx[1], prev_v.si.cols, "block assignment column", s.raw);
            if ((int64_t)(ri.size() * ci.size()) != g.slots[rhs].len)
              fail("block assignment size mismatch for " + s.lhs, s.raw);
            Val nv = prev_v;
            for (size_t j = 0; j < ci.size(); ++j)
              for (size_t i = 0; i < ri.size(); ++i) {
                const Val el =
                    emit_value(OP_INDEX, {rhs_v}, 1, view_of("UReal"),
                               {(int)(j * ri.size() + i)});
                const Val next =
                    emit_value(OP_SET_INDEX, {nv, el}, g.slots[prev].len,
                               out_si, {(int)(ci[j] * prev_v.si.rows + ri[i])});
                propagate_int_update(next, nv, el,
                                     ci[j] * prev_v.si.rows + ri[i], 1);
                nv = next;
              }
            scope[s.lhs] = nv;
            sync_indexed_data_local(s.lhs, nv);
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
            sync_indexed_data_local(s.lhs, nv);
            return;
          }
          // A full array-index prefix followed by an explicit `:` for every
          // remaining dimension: H[i, :, :] on array[N] matrix[R, C] (a
          // container leaf), or y_approx[i, :] on a plain array[N, S] real
          // (the remaining dimension is just another array axis, no
          // container leaf at all) -- either way this spells the same
          // whole-remainder replacement flat_addr's "whole elements" case
          // already gives an implicit-rest prefix. Not `all_single` (the
          // trailing indices are All, not omitted or Single), so it falls
          // outside the block above.
          if (dd) {
            size_t prefix_len = 0;
            while (prefix_len < s.lhs_idx.size() &&
                   s.lhs_idx[prefix_len].name == "IndexSingle")
              ++prefix_len;
            bool trailing_all = true;
            for (size_t d = prefix_len; d < s.lhs_idx.size(); ++d)
              if (s.lhs_idx[d].name != "IndexAll") trailing_all = false;
            if (prefix_len > 0 && trailing_all && prefix_len < dd->size() &&
                s.lhs_idx.size() == dd->size()) {
              std::vector<int64_t> ix;
              ix.reserve(prefix_len);
              for (size_t d = 0; d < prefix_len; ++d) {
                const int64_t one = eval_int(s.lhs_idx[d].args[0]);
                check_index(one, (*dd)[d], "array assignment index", s.raw);
                ix.push_back(one - 1);
              }
              const bool mat = array_shape(prev_v.si).leaf == ViewKind::Matrix;
              const Addr a = flat_addr(*dd, mat, ix);
              require_binding(
                  rhs_v, a.len,
                  indexed_view(prev_v.si, prefix_len, a.len, s.rhs.type_),
                  s.lhs, s.raw);
              Val nv = emit_value(OP_SET_SLICE, {prev_v, rhs_v},
                                  g.slots[prev].len, out_si, {(int)a.off});
              propagate_int_update(nv, prev_v, rhs_v, a.off, 1);
              scope[s.lhs] = nv;
              sync_indexed_data_local(s.lhs, nv);
              return;
            }
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
          Val nv =
              with_layout(emit_value(OP_SET_INDEX, {prev_v, rhs_v},
                                     g.slots[prev].len, out_si, {(int)flat}),
                          owning_layout(out_si));
          propagate_int_update(nv, prev_v, rhs_v, flat, 1);
          scope[s.lhs] = nv;
          sync_indexed_data_local(s.lhs, nv);
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
              if (dl->second.deferred_shape) {
                dl->second.len = g.slots[rhs.slot].len;
                dl->second.si = rhs.si;
                dl->second.deferred_shape = false;
              } else if (dl->second.len == 0 &&
                         (g.slots[rhs.slot].len != 0 ||
                          (is_matrix(dl->second.si) && is_matrix(rhs.si) &&
                           (dl->second.si.rows != rhs.si.rows ||
                            dl->second.si.cols != rhs.si.cols)))) {
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
          rhs.layout = owning_layout(rhs.si);
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
        push_target_term(t.slot);
        return;
      }
      case mir::Stmt::Block:
      case mir::Stmt::SList:
        if (in_write_array && needs_runtime_control(s)) {
          lower_runtime_ifelse(s);
          return;
        }
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
        if (const auto action = message_action(s.fn_name)) {
          auto spec = std::make_shared<MessageSpec>();
          std::vector<int> ins;
          *spec = lower_message_arguments(
              s.fn_args, [&](const mir::Expr& argument) {
                // Op::in holds six. Keep that backend capacity check here;
                // parsing and semantic dispatch remain shared.
                if (ins.size() >= 6)
                  fail(std::string(*action == MessageAction::Reject ? "reject"
                                                                    : "print") +
                           " with more than 6 printed values",
                       s.raw);
                ins.push_back(lower_expr(argument).slot);
              });
          Op op;
          op.opcode = *action == MessageAction::Reject ? OP_REJECT : OP_PRINT;
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
        long lo = 0, hi = 0;
        try {
          lo = eval_int(s.lower);
          hi = eval_int(s.upper);
        } catch (const CompileError&) {
          if (in_write_array ||
              !(needs_runtime_value(s.lower) || needs_runtime_value(s.upper)) ||
              !try_lower_region(s))
            throw;
          return;
        }
        if (lo > hi) {
          int_env.erase(s.loopvar);
          return;
        }
        // Both the pre-control target fold and the ordinary path ask the same
        // structural question.  A nonselected automatic candidate reaches
        // both sites, so retain the answer for this lowering encounter rather
        // than walking a potentially large body twice.
        std::optional<bool> repeatable_target;
        const auto has_repeatable_target = [&]() {
          if (!repeatable_target) repeatable_target = repeatable_target_body(s);
          return *repeatable_target;
        };
        // Both cheap invariant folding and retained selection precede the
        // per-iteration control scan. Neither needs an expanded graph.
        if ((structured_policy == StructuredMode::Prefer ||
             structured_policy == StructuredMode::Force) &&
            lo != hi && has_repeatable_target()) {
          const double old_scale = target_scale;
          target_scale *= static_cast<double>(hi) - static_cast<double>(lo) + 1;
          int_env[s.loopvar] = lo;
          try {
            for (const auto& child : s.body) lower_stmt(child);
          } catch (...) {
            target_scale = old_scale;
            int_env.erase(s.loopvar);
            throw;
          }
          target_scale = old_scale;
          int_env.erase(s.loopvar);
          return;
        }
        if (try_lower_region(s, std::pair<int64_t, int64_t>{lo, hi})) return;
        // runtime_loop_control evaluates data-only conditions while looking
        // for a parameter-selected break/continue. Scan under the same loop
        // binding that ordinary unrolling will use: without it, an indexed
        // condition such as idx[ri] is either treated as spuriously dynamic
        // or can escape static-shape specialization as an unknown variable.
        // The bounds come first so a zero-trip loop never evaluates its body.
        const auto old = int_env.find(s.loopvar);
        const bool had_old = old != int_env.end();
        const long old_value = had_old ? old->second : 0;
        bool has_runtime_loop_control = false;
        try {
          for (long v = lo; v <= hi && !has_runtime_loop_control; ++v) {
            int_env[s.loopvar] = v;
            for (const auto& child : s.body)
              if (runtime_loop_control(child)) {
                has_runtime_loop_control = true;
                break;
              }
          }
        } catch (...) {
          if (had_old)
            int_env[s.loopvar] = old_value;
          else
            int_env.erase(s.loopvar);
          throw;
        }
        if (had_old)
          int_env[s.loopvar] = old_value;
        else
          int_env.erase(s.loopvar);
        if (has_runtime_loop_control) {
          lower_runtime_ifelse(s);
          return;
        }
        if (lo != hi && has_repeatable_target()) {
          const double old_scale = target_scale;
          target_scale *=
              static_cast<double>(hi) - static_cast<double>(lo) + 1.0;
          int_env[s.loopvar] = lo;
          try {
            for (const auto& child : s.body) lower_stmt(child);
          } catch (...) {
            target_scale = old_scale;
            int_env.erase(s.loopvar);
            throw;
          }
          target_scale = old_scale;
          int_env.erase(s.loopvar);
          return;
        }
        for (long v = lo; v <= hi; ++v) {
          int_env[s.loopvar] = v;
          try {
            for (const auto& k : s.body) lower_stmt(k);
          } catch (LoopContinue&) {
            continue;
          } catch (LoopBreak&) {
            break;
          }
        }
        int_env.erase(s.loopvar);
        return;
      }
      case mir::Stmt::While: {
        if (try_lower_region(s)) return;
        // Unlike `for`, a `while` has no statically supplied trip count.
        // Compile it as one structured register-program island, which
        // rechecks its guard at execution time and replays the executed
        // iterations under autodiff.  This deliberately has no lowering-time
        // iteration cap: nontermination is the model's runtime behaviour,
        // not a reason to silently truncate or reject a finite long loop.
        lower_runtime_ifelse(s);
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
        if (udf_depth > 0 && s.body.size() == 2) {
          mir::Stmt effects = s;
          mir::Expr then_value, else_value;
          if (peel_terminal_return(&effects.body[0], &then_value) &&
              peel_terminal_return(&effects.body[1], &else_value)) {
            std::vector<std::string> assigned;
            assigned_names(effects, &assigned);
            if (!assigned.empty() || has_target_pe(effects) ||
                stmt_effectful(effects))
              lower_runtime_ifelse(effects);

            mir::Expr choice;
            choice.kind = mir::Expr::TernaryIf;
            choice.args = {s.cond, then_value, else_value};
            choice.type_ = then_value.type_;
            choice.unsized = then_value.unsized;
            choice.data_only = s.cond.data_only && then_value.data_only &&
                               else_value.data_only;
            choice.raw = s.raw;
            throw LpReturn{lower_expr(choice)};
          }
        }
        // Data-only or not, an unfoldable condition compiles to an island.
        // Data-only says the MIR adlevel is DataOnly, not that the values are
        // in the interpreter's frame: a UDF local built by indexed assignment
        // lives in the graph, and only the region compiler can read it there.
        // The island's live-outs come back parameter-dependent, which costs
        // adjoints such a branch does not need but is never wrong.
        lower_runtime_ifelse(s);
        return;
      }
      case mir::Stmt::Return:
        // Only reachable inside an inlined UDF body (log_prob itself has no
        // value returns); unwinds to lower_call_udf.
        if (!s.has_init) fail("void return unsupported in UDF inlining");
        throw LpReturn{lower_expr(s.rhs)};
      case mir::Stmt::Break:
        throw LoopBreak{};
      case mir::Stmt::Continue:
        throw LoopContinue{};
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
    prep.graph(prep_graph, "lower", lower_time, g, out.fills,
               target_terms.size(), out.views.size(),
               PrepTrace::Extra::Truncated, !wa.truncated.empty());
    dump("lower", roots);
    const auto inplace_time = prep.start();
    const int inplace = make_inplace_updates(g, roots);
    prep.graph(prep_graph, "inplace", inplace_time, g, out.fills,
               target_terms.size(), out.views.size(),
               PrepTrace::Extra::Rewrites, inplace);
    dump("inplace", roots);
    const auto forward_time = prep.start();
    const int forwarded = forward_stores_to_loads(g, roots);
    prep.graph(prep_graph, "store_forward", forward_time, g, out.fills,
               target_terms.size(), out.views.size(), PrepTrace::Extra::Removed,
               forwarded);
    dump("store_forward", roots);
    const auto reroll_time = prep.start();
    RerollStats rerolled;
    detail::RerollDispositionStats reroll_dispositions;
    if (prep.enabled()) {
      detail::ProfiledRerollStats profiled =
          detail::reroll_profiled(g, out.fills, target_terms, roots);
      rerolled = profiled.work;
      reroll_dispositions = profiled.dispositions;
    } else {
      rerolled = reroll(g, out.fills, target_terms, roots);
    }
    prep.graph(prep_graph, "reroll", reroll_time, g, out.fills,
               target_terms.size(), out.views.size(), PrepTrace::Extra::Reroll,
               rerolled.regions, rerolled.list_steps, false, 0,
               rerolled.candidate_steps, rerolled.row_steps,
               &reroll_dispositions);
    dump("reroll", roots);
    // Re-roll can replace many element writes with copying slice stores.
    // Give those new ops the same last-use proof as the scalar stores.
    const auto post_reroll_inplace_time = prep.start();
    const int post_reroll_inplace =
        rerolled.regions ? make_inplace_updates(g, roots) : 0;
    prep.graph(prep_graph, "post_reroll_inplace", post_reroll_inplace_time, g,
               out.fills, target_terms.size(), out.views.size(),
               PrepTrace::Extra::Rewrites, post_reroll_inplace);
    dump("post_reroll_inplace", roots);
    const auto finalize_time = prep.start();
    // Nothing reads a result here, but forward() asserts a scalar result
    // slot, so point it at one.
    g.result_slot = const_slot(0.0);
    wa.n_unconstrained = out.n_unconstrained;
    prep.graph(prep_graph, "finalize", finalize_time, g, out.fills,
               target_terms.size(), out.views.size());
    dump("finalize", roots);
    prep.graph(prep_graph, "total", total_time, g, out.fills,
               target_terms.size(), out.views.size(), PrepTrace::Extra::None, 0,
               0, true, out.n_unconstrained);
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

  CompiledModel run(const mir::Program& p) {
    DumpOnThrow guard{*this};
    const auto total_time = prep.start();
    for (const auto& f : p.fun_defs) fun_defs[f.name] = &f;
    const auto bind_time = prep.start();
    bind_data(p);
    prep.graph(prep_graph, "bind_data", bind_time, g, out.fills,
               target_terms.size(), out.views.size());
    dump("bind_data", {});
    const auto lower_time = prep.start();
    for (const auto& s : p.log_prob) lower_stmt(s);
    prep.graph(prep_graph, "lower", lower_time, g, out.fills,
               target_terms.size(), out.views.size());
    dump("lower", {});
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
    dump("inplace", update_roots);
    // Deleting the write/read-back pairs first is what leaves a plain
    // arithmetic lane for reroll to vectorize.
    const auto forward_time = prep.start();
    const int forwarded = forward_stores_to_loads(g, update_roots);
    prep.graph(prep_graph, "store_forward", forward_time, g, out.fills,
               target_terms.size(), out.views.size(), PrepTrace::Extra::Removed,
               forwarded);
    dump("store_forward", update_roots);
    // After the update chains collapse, so a data-only chain is one slot
    // rather than N; before reroll, so the lanes it sees have data operands.
    const auto constfold_time = prep.start();
    const ConstFoldStats constfolded = const_fold(g, out.fills, update_roots);
    prep.graph(prep_graph, "constfold", constfold_time, g, out.fills,
               target_terms.size(), out.views.size(),
               PrepTrace::Extra::ConstFold, constfolded.ops_removed,
               constfolded.slots_folded);
    dump("constfold", update_roots);
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
    prep.graph(prep_graph, "reroll", reroll_time, g, out.fills,
               target_terms.size(), out.views.size(), PrepTrace::Extra::Reroll,
               rerolled.regions, rerolled.list_steps, false, 0,
               rerolled.candidate_steps, rerolled.row_steps,
               &reroll_dispositions);
    dump("reroll", roots);
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
    dump("post_reroll_inplace", post_reroll_roots);
    // After re-roll, which keeps first crack at the contiguous shapes it
    // already handles, and before CSE, which would merge ops shared between
    // lanes and leave the lanes no longer whole.
    const auto partition_time = prep.start();
    const PartitionStats parted =
        partition_lanes(g, out.fills, target_terms, roots);
    prep.graph(prep_graph, "partition", partition_time, g, out.fills,
               target_terms.size(), out.views.size(),
               PrepTrace::Extra::Partition, parted.groups, parted.lanes, false,
               0, parted.declined, parted.list_steps);
    dump("partition", roots);
    // Same proof the slice stores re-roll makes get: rebuilt from the terms
    // partition just replaced.
    std::vector<int> post_partition_roots = roots;
    post_partition_roots.insert(post_partition_roots.end(),
                                target_terms.begin(), target_terms.end());
    const auto post_partition_inplace_time = prep.start();
    const int post_partition_inplace =
        parted.groups ? make_inplace_updates(g, post_partition_roots) : 0;
    prep.graph(prep_graph, "post_partition_inplace",
               post_partition_inplace_time, g, out.fills, target_terms.size(),
               out.views.size(), PrepTrace::Extra::Rewrites,
               post_partition_inplace);
    dump("post_partition_inplace", post_partition_roots);
    // After every pass that emits a slice store, and before islands, whose
    // bodies name outer slots in a payload this rename cannot reach.
    const auto elide_time = prep.start();
    const int elided = elide_full_extent_stores(g, post_partition_roots);
    prep.graph(prep_graph, "elide_stores", elide_time, g, out.fills,
               target_terms.size(), out.views.size(), PrepTrace::Extra::Removed,
               elided);
    dump("elide_stores", post_partition_roots);
    // After reroll, whose lane matching needs the repeated ops it hoists to
    // still be there, and before islands, so they compile the smaller
    // residue.
    const auto cse_time = prep.start();
    const CseStats cse_st = cse(g, out.fills, target_terms, roots);
    prep.graph(prep_graph, "cse", cse_time, g, out.fills, target_terms.size(),
               out.views.size(), PrepTrace::Extra::Removed, cse_st.ops_removed);
    dump("cse", roots);
    // LAST, after every other pass has had first crack: compile whatever
    // scalar residue survives (recurrences the re-roll can never widen)
    // into island ops. Off under STANLI_NO_ISLAND.
    const auto island_time = prep.start();
    const int islands = carve_islands(g, out.fills, target_terms, roots);
    prep.graph(prep_graph, "island", island_time, g, out.fills,
               target_terms.size(), out.views.size(), PrepTrace::Extra::Regions,
               islands);
    dump("island", roots);
    const auto reduce_time = prep.start();
    std::vector<int> all = target_terms;
    all.insert(all.end(), jac_slots.begin(), jac_slots.end());
    g.result_slot = reduce_terms(all);
    prep.graph(prep_graph, "reduce", reduce_time, g, out.fills,
               target_terms.size(), out.views.size());
    dump("reduce", roots);
    prep.graph(prep_graph, "total", total_time, g, out.fills,
               target_terms.size(), out.views.size(), PrepTrace::Extra::None, 0,
               0, true, out.n_unconstrained);
    guard.done = true;
    out.graph = std::move(g);
    return std::move(out);
  }
};

}  // namespace

CompiledModel compile_model(const std::string& mir_text, const DataMap& data) {
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
  Lowering lo(data, prep, dumper, "log_prob");
  CompiledModel cm = lo.run(*prog);
  if (!prog->generate_quantities.empty()) {
    // A second lowering, over the transformed data the first one already
    // interpreted: re-running prepare_data would double preparation time on
    // the models where preparation is the cost (nn_rbm1bJ100, 20.7 s).
    Lowering wa(data, prep, dumper, "write_array", lo.shape_pool);
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
  prep.plain("compile", "total", compile_time);
  prep.report();
  return cm;
}

}  // namespace stanli
