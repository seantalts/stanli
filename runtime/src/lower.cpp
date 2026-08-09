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
#include <stanli/wa_interp.hpp>


#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace stanli {
namespace {

struct SlotInfo {
  int64_t len = 1;
  int64_t rows = 0, cols = 0;  // set for matrices
  bool data_like = false;      // no adjoint (data or constant)
};

struct Lowering {
  const DataMap& data;
  // The MIR interpreter instance for everything DataOnly: prepare_data,
  // data-only conditions, size expressions. Its environment doubles as the
  // lowering's view of transformed data. Hooks route FnReadData to the
  // DataMap and unknown variables to the unrolled-loop int environment.
  MirInterp<double> td{fun_defs, "prepare_data",
      MirHooks{
          [this](const std::string& n) -> const DataMap::Entry* {
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
  std::map<std::string, int> scope;            // var -> slot
  std::map<std::string, long> int_env;         // data int scalars
  std::map<double, int> const_cache;
  std::map<int, std::vector<double>> slot_values;  // constant/data fills
  std::vector<SlotInfo> info;                  // parallel to g.slots
  std::vector<int> target_terms;
  std::vector<int> jac_slots;
  std::map<std::string, const mir::FunDef*> fun_defs;
  std::set<std::string> int_locals;  // SInt locals in log_prob (data-only)
  std::vector<int64_t> decl_dims_pending;  // shape of the last ODE result
  int udf_depth = 0;
  // int_env as bind_data left it, before either section's locals and loop
  // variables were folded in; the write_array lowering starts from this.
  std::map<std::string, long> int_env_data;
  // Lowering generate_quantities rather than log_prob: parameters are columns
  // to emit, not values to differentiate.
  bool in_write_array = false;

  explicit Lowering(const DataMap& d) : data(d) {}

  int add_slot(int64_t len, bool is_param, SlotInfo si = {}) {
    si.len = len;
    const int s = g.add_slot(len, is_param);
    info.push_back(si);
    return s;
  }

  [[noreturn]] void fail(const std::string& msg, const std::string& raw = "") {
    throw CompileError("stanli compile: " + msg +
                       (raw.empty() ? "" : " | in: " + raw));
  }

  int const_slot(double v) {
    auto it = const_cache.find(v);
    if (it != const_cache.end()) return it->second;
    SlotInfo si;
    si.data_like = true;
    const int s = add_slot(1, false, si);
    out.fills.emplace_back(s, std::vector<double>{v});
    const_cache[v] = s;
    slot_values[s] = {v};
    return s;
  }

  long eval_int(const mir::Expr& e) {
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
        if (e.name == "Plus__") return eval_int(e.args[0]) + eval_int(e.args[1]);
        if (e.name == "Minus__") return eval_int(e.args[0]) - eval_int(e.args[1]);
        if (e.name == "Times__") return eval_int(e.args[0]) * eval_int(e.args[1]);
        if (e.name == "dims" && e.args.size() == 1 &&
            e.args[0].kind == mir::Expr::Var) {
          auto sit = scope.find(e.args[0].name);
          if (sit != scope.end()) {
            const SlotInfo& si = info[sit->second];
            // Only reachable through an index, which eval_int resolves on
            // the returned sequence; expose rows for matrices, len else.
            return si.rows > 0 ? si.rows : si.len;
          }
        }
        // Anything data-only the td interpreter can evaluate (sum of an
        // int array in a size expression, etc.).
        if (e.data_only) {
          try {
            return td.as_int(e);
          } catch (const CompileError&) {
          }
        }
        // Shape queries on slot-bound values (e.g. rows(v) on an inlined
        // UDF's vector argument) answer from the slot's SlotInfo.
        if ((e.name == "rows" || e.name == "cols" || e.name == "size" ||
             e.name == "num_elements" || e.name == "FnLength") &&
            e.args.size() == 1 && e.args[0].kind == mir::Expr::Var) {
          auto sit = scope.find(e.args[0].name);
          if (sit != scope.end()) {
            const SlotInfo& si = info[sit->second];
            if (e.name == "rows") return si.rows > 0 ? si.rows : si.len;
            if (e.name == "cols") return si.rows > 0 ? si.cols : 1;
            return si.len;
          }
          DataMap::Entry* en = td.find(e.args[0].name);
          if (en) {
            if (e.name == "rows")
              return en->dims.size() == 2 ? en->dims[0]
                                          : (long)en->r.size();
            if (e.name == "cols") return en->dims.size() == 2 ? en->dims[1] : 1;
            return (long)std::max(en->r.size(), en->i.size());
          }
        }
        fail("unsupported int size function " + e.name, e.raw);
      default:
        fail("unsupported size expression", e.raw);
    }
  }

  int64_t sized_len(const mir::SizedType& t, int64_t* rows = nullptr,
                    int64_t* cols = nullptr) {
    if (t.base == "SInt" || t.base == "SReal") return 1;
    if (t.base == "SVector" || t.base == "SRowVector")
      return eval_int(t.dims[0]);
    if (t.base == "SMatrix") {
      const int64_t r = eval_int(t.dims[0]), c = eval_int(t.dims[1]);
      if (rows) *rows = r;
      if (cols) *cols = c;
      return r * c;
    }
    if (t.base == "SArray") {
      int64_t n = 1;
      for (const auto& d : t.dims) n *= eval_int(d);
      return n;
    }
    fail("unsupported sized type " + t.base, t.raw);
  }

  // Data declared `array[N] vector[K]` (or row_vector). The interpreter
  // stores every 2-D value with the first index fastest, the way it stores
  // a matrix. The graph wants an array of containers laid out the way
  // parameters of the same type are: element n contiguous in K. env_slot
  // repacks these on the way out.
  std::set<std::string> array_of_container;

  void bind_data(const mir::Program& p) {
    for (const auto& [name, type] : p.input_vars) {
      (void)type;
      if (data.has(name)) td.env()[name] = data.at(name);
    }
    for (const auto& st : p.prepare_data) {
      if (st.kind == mir::Stmt::Decl && st.decl_type.base == "SArray" &&
          st.decl_type.dims.size() == 2 &&
          (st.decl_type.raw == "SVector" || st.decl_type.raw == "SRowVector"))
        array_of_container.insert(st.decl_id);
    }
    for (const auto& st : p.prepare_data) td.exec(st);
    for (auto& [name, e] : td.env()) {
      if (e.is_int && e.i.size() == 1 && e.dims.empty())
        int_env[name] = e.i[0];
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
    if (en->r.empty() && !en->dims.empty() && en->dims[0] == 0) {
      const int s = add_slot(0, false, SlotInfo{0, 0, 0, true});
      scope[name] = s;
      return s;
    }
    if (en->r.empty()) return -1;
    SlotInfo si;
    si.data_like = true;
    // An array of containers is not a matrix, so it gets no rows/cols: the
    // kernels that take one read element n from n*K, which is where the
    // repack below puts it, and which is where a parameter of the same
    // type already sits.
    const bool aoc = array_of_container.count(name) > 0 &&
                     en->dims.size() == 2;
    if (en->dims.size() == 2 && !aoc) {
      si.rows = en->dims[0];
      si.cols = en->dims[1];
    }
    std::vector<double> vals = en->r;
    if (aoc) {
      const int64_t N = en->dims[0], K = en->dims[1];
      for (int64_t i = 0; i < N; ++i)
        for (int64_t k = 0; k < K; ++k) vals[i * K + k] = en->r[k * N + i];
    }
    const int s = add_slot((int64_t)vals.size(), false, si);
    out.fills.emplace_back(s, vals);
    slot_values[s] = vals;
    scope[name] = s;
    return s;
  }

  // ---- expressions ----------------------------------------------------------
  struct Val {
    int slot;
    SlotInfo si;
  };

  Val lower_expr(const mir::Expr& e) {
    switch (e.kind) {
      case mir::Expr::Var: {
        auto it = scope.find(e.name);
        if (it == scope.end()) {
          auto ii = int_env.find(e.name);
          if (ii != int_env.end())
            return {const_slot(static_cast<double>(ii->second)), {}};
          const int s = env_slot(e.name);
          if (s >= 0) return {s, info[s]};
          fail("unknown variable " + e.name);
        }
        return {it->second, info[it->second]};
      }
      case mir::Expr::Indexed: {
        // All-Single indices with compile-time values -> element read.
        Val base = lower_expr(e.args[0]);
        if (e.args.size() == 2 && e.args[1].name == "IndexAll") return base;
        bool all_single = true;
        for (size_t k = 1; k < e.args.size(); ++k)
          if (e.args[k].name != "IndexSingle") all_single = false;
        const std::vector<int64_t>* bdims = nullptr;
        if (e.args[0].kind == mir::Expr::Var) {
          auto dd = decl_dims.find(e.args[0].name);
          if (dd != decl_dims.end() && !dd->second.empty())
            bdims = &dd->second;
        }
        const size_t n_idx = e.args.size() - 1;
        // Between subrange read on a 1-D value: v[a:b] is contiguous.
        if (e.args.size() == 2 && e.args[1].name == "IndexBetween") {
          const int64_t lo = eval_int(e.args[1].args[0]);
          const int64_t hi = eval_int(e.args[1].args[1]);
          return emit(OP_SLICE, {base.slot}, hi - lo + 1, {}, {(int)(lo - 1)});
        }
        // Gather by a data int array: v[idx].
        if (e.args.size() == 2 && e.args[1].name == "IndexMulti") {
          DataMap::Entry iv = td.eval(e.args[1].args[0]);
          if (!iv.is_int || iv.i.empty())
            fail("gather index must be int data", e.raw);
          std::vector<int> idata;
          idata.reserve(iv.i.size());
          for (int x : iv.i) idata.push_back(x - 1);
          return emit(OP_GATHER, {base.slot}, (int64_t)idata.size(), {},
                      idata);
        }
        // Matrix row/column slices (col-major storage; rows>0 marks a
        // matrix slot).
        if (e.args.size() == 3 && base.si.rows > 0 &&
            e.args[1].name == "IndexSingle" && e.args[2].name == "IndexAll") {
          const int64_t i = eval_int(e.args[1].args[0]) - 1;
          return emit(OP_SLICE_STRIDED, {base.slot}, base.si.cols, {},
                      {(int)i, (int)base.si.rows});
        }
        if (e.args.size() == 3 && base.si.rows > 0 &&
            e.args[1].name == "IndexAll" && e.args[2].name == "IndexSingle") {
          const int64_t j = eval_int(e.args[2].args[0]) - 1;
          return emit(OP_SLICE, {base.slot}, base.si.rows, {},
                      {(int)(j * base.si.rows)});
        }
        // Column of an array-major 2-D value (array[N, S] real): elements
        // sit S apart, so this is a strided slice, not a contiguous one.
        if (e.args.size() == 3 && base.si.rows == 0 && bdims &&
            bdims->size() == 2 && e.args[1].name == "IndexAll" &&
            e.args[2].name == "IndexSingle") {
          const int64_t k = eval_int(e.args[2].args[0]) - 1;
          const int64_t N = (*bdims)[0], S = (*bdims)[1];
          return emit(OP_SLICE_STRIDED, {base.slot}, N, {}, {(int)k, (int)S});
        }
        // Row-range column read M[a:b, j] (contiguous within the column).
        if (e.args.size() == 3 && base.si.rows > 0 &&
            e.args[1].name == "IndexBetween" &&
            e.args[2].name == "IndexSingle") {
          const int64_t lo = eval_int(e.args[1].args[0]);
          const int64_t hi = eval_int(e.args[1].args[1]);
          const int64_t j = eval_int(e.args[2].args[0]) - 1;
          return emit(OP_SLICE, {base.slot}, hi - lo + 1, {},
                      {(int)(j * base.si.rows + lo - 1)});
        }
        // Params/locals with recorded dims use array-major layout (outer
        // index slowest, inner contiguous), matching stanc's read order.
        // Matrix slots (rows>0) are col-major and never take this path.
        if (all_single && bdims && n_idx <= bdims->size() &&
            base.si.rows == 0) {
          const auto& D = *bdims;
          int64_t inner = 1;
          for (size_t d = n_idx; d < D.size(); ++d) inner *= D[d];
          int64_t off = 0;
          for (size_t d = 0; d < n_idx; ++d) {
            int64_t stride = inner;
            for (size_t d2 = d + 1; d2 < n_idx; ++d2) stride *= D[d2];
            off += (eval_int(e.args[1 + d].args[0]) - 1) * stride;
          }
          if (inner == 1)
            return emit(OP_INDEX, {base.slot}, 1, {}, {(int)off});
          return emit(OP_SLICE, {base.slot}, inner, {}, {(int)off});
        }
        // Row of a column-major data matrix / 2-D array: strided slice.
        if (all_single && e.args.size() == 2 && base.si.rows > 0 &&
            e.type_ != "UReal" && e.type_ != "UInt") {
          const int64_t t = eval_int(e.args[1].args[0]) - 1;
          return emit(OP_SLICE_STRIDED, {base.slot}, base.si.cols, {},
                      {(int)t, (int)base.si.rows});
        }
        // Data-only slicing with no native path (e.g. one matrix out of a
        // data array of matrices) evaluates at compile time.
        if (e.data_only) {
          Val v;
          if (try_fold_const(e, &v)) return v;
        }
        int64_t flat = 0;
        if (all_single && e.args.size() == 2 &&
            (e.type_ == "UReal" || e.type_ == "UInt")) {
          flat = eval_int(e.args[1].args[0]) - 1;
        } else if (all_single && e.args.size() == 3 && base.si.rows > 0 &&
                   (e.type_ == "UReal" || e.type_ == "UInt")) {
          flat = (eval_int(e.args[2].args[0]) - 1) * base.si.rows +
                 (eval_int(e.args[1].args[0]) - 1);
        } else {
          std::string desc = "unsupported index expression: base=" +
                             (e.args[0].kind == mir::Expr::Var
                                  ? e.args[0].name
                                  : std::string("<expr>"));
          for (size_t k = 1; k < e.args.size(); ++k)
            desc += " [" +
                    (e.args[k].name.empty() ? "?" : e.args[k].name) + "]";
          desc += " type=" + e.type_;
          fail(desc, e.raw);
        }
        return emit(OP_INDEX, {base.slot}, 1, {}, {(int)flat});
      }
      case mir::Expr::LitInt:
        return {const_slot(static_cast<double>(e.lit_i)), {}};
      case mir::Expr::LitReal:
        return {const_slot(e.lit), {}};
      case mir::Expr::FunApp:
        return lower_funapp(e);
      case mir::Expr::TernaryIf: {
        // Data-only conditions resolve at compile time; either branch may
        // reference parameters.
        // A parameter-dependent condition cannot pick an arm at load
        // time, so the whole expression becomes an island.
        if (!e.args[0].data_only) return lower_param_ternary(e);
        const bool c = td.eval(e.args[0]).r.at(0) != 0.0;
        return lower_expr(e.args[c ? 1 : 2]);
      }
      case mir::Expr::EOr:
      case mir::Expr::EAnd: {
        Val v;
        if (try_fold_const(e, &v)) return v;
        fail("boolean operator on parameters unsupported", e.raw);
      }
      default: {
        Val v;
        if (try_fold_const(e, &v)) return v;
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
    for (const auto& [name, v] : int_env) c.ints[name] = {v};
    c.bind_extern = [&](const std::string& name, Range* r) {
      auto sc = scope.find(name);
      const int slot = sc != scope.end() ? sc->second : env_slot(name);
      if (slot < 0) return false;
      const int64_t len = g.slots[slot].len;
      if (len <= 0) return false;
      // An op takes at most six inputs (graph.hpp), and each outside
      // value the region reads is one of them.
      if ((int)reg->in_slots.size() >= 6)
        c.bail("a parameter-dependent region may read at most 6 values "
               "from outside it; " + name + " is one too many");
      r->reg = c.alloc((int)len);
      r->len = (int)len;
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
          auto dl = decl_lens.find(name);
          if (dl == decl_lens.end()) continue;
          c.declare(name, (int)dl->second.len,
                    std::numeric_limits<double>::quiet_NaN());
        }
        c.stmt(*s);
        std::vector<std::string> assigned;
        assigned_names(*s, &assigned);
        for (const std::string& name : assigned) {
          auto it = c.reals.find(name);
          if (it == c.reals.end()) continue;
          reg->out_names.push_back(name);
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
    } catch (Bail& b) {
      fail("parameter-dependent region: " + b.why, s ? s->raw : e->raw);
    }
    if (prog->out_regs.empty())
      fail("parameter-dependent region produces nothing", s ? s->raw : e->raw);
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
      const Val v =
          emit(len == 1 ? OP_INDEX : OP_SLICE, {is.out}, len, {}, {(int)off});
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
    std::vector<int> out_lens;
    for (const std::string& name : reg.out_names) {
      auto it = scope.find(name);
      if (it != scope.end()) {
        out_lens.push_back((int)g.slots[it->second].len);
        continue;
      }
      auto dl = decl_lens.find(name);
      if (dl == decl_lens.end())
        fail("parameter-dependent region assigns " + name +
             ", which has no declared shape", s.raw);
      out_lens.push_back((int)dl->second.len);
    }
    if (reg.has_target) out_lens.push_back(1);
    std::vector<int> out_slots;
    emit_island(prog, reg, out_lens, &out_slots);
    // Later statements read the island's results, not the old values.
    for (size_t k = 0; k < reg.out_names.size(); ++k)
      scope[reg.out_names[k]] = out_slots[k];
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
    return {out_slots[0], info[out_slots[0]]};
  }

  Val emit(uint16_t opcode, std::vector<int> ins, int64_t out_len,
           SlotInfo out_si = {}, std::vector<int> idata = {}, int out2 = -1) {
    const int o = add_slot(out_len, false, out_si);
    Op op;
    op.opcode = opcode;
    op.out = o;
    op.out2 = out2;
    op.n_in = 0;
    for (int s : ins) op.in[op.n_in++] = s;
    if (!idata.empty()) {
      g.idata_pool.push_back(std::move(idata));
      op.idata = g.idata_pool.back().data();
      op.n_idata = (int64_t)g.idata_pool.back().size();
    }
    g.ops.push_back(op);
    return {o, info[o]};
  }

  // Fallback for expressions with no native lowering: a data-only subtree
  // is evaluated at compile time and materialized as a constant. Returns
  // false (leaving v untouched) when the interpreter can't evaluate it
  // either; propto densities never fold (their value is
  // instantiation-dependent).
  bool try_fold_const(const mir::Expr& e, Val* v) {
    if (!e.data_only || e.fn_propto) return false;
    DataMap::Entry en;
    try {
      en = td.eval(e);
    } catch (const CompileError&) {
      return false;
    }
    if (en.r.empty()) return false;
    if (en.r.size() == 1) {
      *v = {const_slot(en.r[0]), {}};
      return true;
    }
    SlotInfo si;
    si.data_like = true;
    if (en.dims.size() == 2) {
      si.rows = en.dims[0];
      si.cols = en.dims[1];
    }
    const int s = add_slot((int64_t)en.r.size(), false, si);
    out.fills.emplace_back(s, en.r);
    slot_values[s] = en.r;
    *v = {s, si};
    return true;
  }

  // Integer argument of a density/pmf: values must be known at compile
  // time (int data, loop variables, or compile-time expressions).
  std::vector<int> int_arg_values(const mir::Expr& oc) {
    if (oc.kind == mir::Expr::Var) {
      DataMap::Entry* en = td.find(oc.name);
      if (en && en->is_int && !en->i.empty()) return en->i;
      if (int_env.count(oc.name))
        return {static_cast<int>(int_env[oc.name])};
    }
    if (oc.kind == mir::Expr::LitInt) return {static_cast<int>(oc.lit_i)};
    if (oc.kind == mir::Expr::Indexed) {
      // May be a slice (y[i] on a 2-D array yields a whole row), so
      // evaluate through the data interpreter, not scalar eval_int.
      DataMap::Entry v = td.eval(oc);
      if (v.is_int && !v.i.empty()) return v.i;
    }
    if (oc.kind == mir::Expr::FunApp) {
      // Compile-time int expression (e.g. sum(y[n]) under an unrolled loop).
      return {static_cast<int>(eval_int(oc))};
    }
    fail("int argument must be int data (kind=" +
             std::to_string((int)oc.kind) + " type=" + oc.type_ + ")",
         oc.raw);
  }

  // Matrix shape of an elementwise result: whichever operand carries one
  // (both must agree when both do).
  SlotInfo shape_of(const Val& a, const Val& b) {
    SlotInfo si;
    const SlotInfo& src = a.si.rows > 0 ? a.si : b.si;
    if (a.si.rows > 0 && b.si.rows > 0 &&
        (a.si.rows != b.si.rows || a.si.cols != b.si.cols))
      fail("elementwise op on matrices of different shapes");
    si.rows = src.rows;
    si.cols = src.cols;
    // An op over data-only inputs is itself data (no adjoint), which is
    // what lets a transformed data matrix still drive OP_MATVEC.
    si.data_like = info[a.slot].data_like && info[b.slot].data_like;
    return si;
  }

  // Value of a data-only expression at compile time. The interpreter
  // handles most cases; a UDF-local constant lives only as a slot, so fall
  // back to that slot's recorded fill.
  std::vector<double> const_values(const mir::Expr& e) {
    try {
      DataMap::Entry en = td.eval(e);
      return en.r;
    } catch (const CompileError&) {
    }
    Val v = lower_expr(e);
    auto it = slot_values.find(v.slot);
    if (it != slot_values.end()) return it->second;
    // A zero-length slot carries no values by construction (`array[0] real`
    // is how ODE models spell "no data for the system").
    if (info[v.slot].len == 0) return {};
    fail("value must be known at compile time: " +
             (e.kind == mir::Expr::Var ? e.name : ("<" + e.name + ">")),
         e.raw);
  }
  std::vector<int> const_ints(const mir::Expr& e) {
    try {
      DataMap::Entry en = td.eval(e);
      if (en.is_int) return en.i;
      std::vector<int> out;
      for (double d : en.r) out.push_back((int)d);
      return out;
    } catch (const CompileError&) {
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
    if (e.args.size() != f.arg_names.size())
      fail(e.name + ": arity mismatch");
    if (++udf_depth > 64) fail("UDF recursion too deep in " + e.name);
    struct Binding {
      bool is_int = false;
      long iv = 0;
      Val v{-1, {}};
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
    }
    auto sc_saved = scope;
    auto ie_saved = int_env;
    auto dd_saved = decl_dims;
    auto dl_saved = decl_lens;
    auto env_saved = td.env();
    for (size_t i = 0; i < binds.size(); ++i) {
      const std::string& name = f.arg_names[i];
      decl_dims.erase(name);
      decl_lens.erase(name);
      // Data-only arguments also enter the interpreter's environment, so
      // shape and size queries inside the body (dims, size, rows) resolve
      // at compile time just as they do in transformed data.
      td.env().erase(name);
      // Bind whenever the argument's value is computable at compile time,
      // not just when the MIR flags it DataOnly: a function may take a data
      // array without the `data` qualifier, and its body still asks for
      // shapes and sizes. Parameter expressions simply fail to evaluate
      // (their names are not in the data environment), so this cannot bind
      // something that varies.
      {
        try {
          DataMap::Entry en = td.eval(e.args[i]);
          if (en.dims.size() > 1) decl_dims[name] = en.dims;
          td.env()[name] = std::move(en);
        } catch (const CompileError&) {
          // Not interpretable, but a data-only value still has a constant
          // slot; bind that so shape and size queries inside the body work.
          if (!binds[i].is_int && binds[i].v.slot >= 0) {
            auto it = slot_values.find(binds[i].v.slot);
            if (it != slot_values.end()) {
              DataMap::Entry en;
              en.r = it->second;
              en.dims = {(int64_t)en.r.size()};
              if (binds[i].v.si.rows > 0)
                en.dims = {binds[i].v.si.rows, binds[i].v.si.cols};
              td.env()[name] = std::move(en);
            }
          }
        }
      }
      if (binds[i].is_int) {
        int_env[name] = binds[i].iv;
        scope.erase(name);
      } else {
        scope[name] = binds[i].v.slot;
        int_env.erase(name);
      }
    }
    Val ret{-1, {}};
    bool returned = false;
    try {
      for (const auto& st : f.body) lower_stmt(st);
    } catch (LpReturn& r) {
      ret = r.v;
      returned = true;
    }
    scope = std::move(sc_saved);
    int_env = std::move(ie_saved);
    decl_dims = std::move(dd_saved);
    decl_lens = std::move(dl_saved);
    td.env() = std::move(env_saved);
    --udf_depth;
    if (!returned)
      fail(e.name + ": no return value on the executed path");
    return ret;
  }

  Val lower_funapp(const mir::Expr& e) {
    if (e.fn_lib == mir::Expr::Lib::UserDefined) {
      Val v;
      if (e.data_only && try_fold_const(e, &v)) return v;
      return lower_call_udf(e);
    }
    if (e.fn_lib == mir::Expr::Lib::Internal &&
        (e.name == "FnMakeArray" || e.name == "FnMakeRowVec")) {
      // Array/row-vector literal: concatenate the pieces. Data-only ones
      // fold; the rest become a CONCAT2 chain.
      Val v;
      if (e.data_only && try_fold_const(e, &v)) return v;
      std::vector<Val> parts;
      for (const auto& a : e.args) parts.push_back(lower_expr(a));
      Val acc = parts[0];
      for (size_t i = 1; i < parts.size(); ++i) {
        const int64_t len = info[acc.slot].len + info[parts[i].slot].len;
        acc = emit(OP_CONCAT2, {acc.slot, parts[i].slot}, len);
      }
      return acc;
    }
    if (e.fn_lib != mir::Expr::Lib::StanLib) {
      Val v;
      if (try_fold_const(e, &v)) return v;
      fail("unsupported function kind for " + e.name, e.raw);
    }
    // The stan-library names split into disjoint groups; each helper owns
    // one and declines the rest.
    if (auto v = lower_density_fn(e)) return *v;
    if (auto v = lower_eltwise_fn(e)) return *v;
    if (auto v = lower_matrix_fn(e)) return *v;
    if (auto v = lower_ode_fn(e)) return *v;
    // `y ~ foo(...)` with every argument data is EXACTLY zero in CmdStan:
    // the generated C++ calls foo_lpdf<propto=true> on all-double
    // arguments, include_summand comes back false, and the whole term is
    // dropped before any arithmetic happens. So the rewrite is not an
    // approximation, and it covers densities no kernel exists for --
    // hypergeometric and discrete_range are all-int, so ALL their uses
    // land here or in the constant fold below. (categorical_logit already
    // had this rule privately; this is the general form.) The one
    // divergence is a model whose data is outside the density's support:
    // CmdStan's checks throw before the early return, stanli contributes
    // 0. That model rejects every draw anyway.
    const bool is_density = e.name.size() > 5 &&
                            (e.name.compare(e.name.size() - 5, 5, "_lpdf") == 0 ||
                             e.name.compare(e.name.size() - 5, 5, "_lpmf") == 0);
    if (is_density && e.fn_propto) {
      bool all_data = true;
      for (const auto& a : e.args) all_data = all_data && a.data_only;
      if (all_data) return Val{const_slot(0.0), {}};
    }
    {
      Val v;
      if (try_fold_const(e, &v)) return v;
    }
    // A shape query in a REAL-valued expression. eval_int already answers
    // rows/cols/size from the slot or the data map, but only where an
    // integer was expected; brms's mo() helper writes
    // `rows(scale) * sum(scale[1:i])`, where the same call sits in the
    // middle of arithmetic and reached the failure below instead.
    if ((e.name == "rows" || e.name == "cols" || e.name == "size" ||
         e.name == "num_elements") &&
        e.args.size() == 1) {
      try {
        return Val{const_slot((double)eval_int(e)), {}};
      } catch (const CompileError&) {
      }
    }
    fail("unsupported function " + e.name);
  }

  // Density calls: the table-driven kernels plus the ones that decompose
  // onto existing ops (categorical) or carry matrix arguments
  // (multi_normal, lkj, glm).
  std::optional<Val> lower_density_fn(const mir::Expr& e) {
    // Densities. n_int leading args come from int data (idata); the rest are
    // real slots. Layouts: one int group = raw values; two groups =
    // [len, vals..., len, vals...]; glm = [y..., rows, cols].
    struct Dens { uint16_t op; int nargs; int n_int; bool glm = false; };
    static const std::map<std::string, Dens> kDens = {
        {"poisson_log_lpmf", {OP_POISSON_LOG_LPMF, 2, 1}},
        {"bernoulli_logit_lpmf", {OP_BERNOULLI_LOGIT_LPMF, 2, 1}},
        // Generated from STANLI_SCALAR_DENSITY_LIST (optable.hpp): the same
        // one line that made the opcode and the kernel makes this entry.
#define STANLI_DENSITY_TABLE(code, fn, n, m) {#fn, {code, n, 0}},
        STANLI_SCALAR_DENSITY_LIST(STANLI_DENSITY_TABLE)
#undef STANLI_DENSITY_TABLE
        // Discrete densities: outcome + n real arguments, one int group.
        // Ordered ones have the same lowering shape -- their cutpoint
        // vector is an ordinary real slot -- and differ only in that
        // reroll never fuses them.
#define STANLI_INT_DENSITY_TABLE(code, fn, nreal, t) {#fn, {code, nreal + 1, 1}},
        STANLI_INT_DENSITY_LIST(STANLI_INT_DENSITY_TABLE)
#undef STANLI_INT_DENSITY_TABLE
        // cdf/lcdf/lccdf: all-real arguments, no int group, and
        // fn_propto is never set on them.
#define STANLI_CDF_TABLE(code, fn, n, t) {#fn, {code, n, 0}},
        STANLI_SCALAR_CDF_LIST(STANLI_CDF_TABLE)
#undef STANLI_CDF_TABLE
        // Integer-outcome cdfs: the count is the one int group.
#define STANLI_INT_CDF_TABLE(code, fn, nreal, t) {#fn, {code, nreal + 1, 1}},
        STANLI_INT_CDF_LIST(STANLI_INT_CDF_TABLE)
        STANLI_ORDERED_DENSITY_LIST(STANLI_INT_CDF_TABLE)
#undef STANLI_INT_CDF_TABLE
        {"bernoulli_lpmf", {OP_BERNOULLI_LPMF, 2, 1}},
        {"poisson_lpmf", {OP_POISSON_LPMF, 2, 1}},
        {"neg_binomial_2_lpmf", {OP_NEG_BINOMIAL_2_LPMF, 3, 1}},
        {"binomial_lpmf", {OP_BINOMIAL_LPMF, 3, 2}},
        {"binomial_logit_lpmf", {OP_BINOMIAL_LOGIT_LPMF, 3, 2}},
        {"poisson_log_glm_lpmf", {OP_POISSON_LOG_GLM_LPMF, 4, 1, true}},
        {"neg_binomial_2_log_glm_lpmf",
         {OP_NEG_BINOMIAL_2_LOG_GLM_LPMF, 5, 1, true}},
        {"beta_binomial_lpmf", {OP_BETA_BINOMIAL_LPMF, 4, 2}},
        {"bernoulli_logit_glm_lpmf",
         {OP_BERNOULLI_LOGIT_GLM_LPMF, 4, 1, true}},
        {"dirichlet_lpdf", {OP_DIRICHLET_LPDF, 2, 0}},
    };
    auto dit = kDens.find(e.name);
    if (dit != kDens.end()) {
      const Dens& d = dit->second;
      if ((int)e.args.size() != d.nargs)
        fail(e.name + ": expected " + std::to_string(d.nargs) + " args");
      std::vector<int> idata;
      if (d.n_int == 1) {
        idata = int_arg_values(e.args[0]);
      } else if (d.n_int == 2) {
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
      uint8_t variant = 0;
      for (size_t i = d.n_int; i < e.args.size(); ++i) {
        ins.push_back(lower_expr(e.args[i]).slot);
        if (!e.args[i].data_only) variant |= (uint8_t)(1u << (i - d.n_int));
      }
      if (e.fn_propto) variant |= 0x80u;
      if (d.glm) {
        // X must be a data matrix; append its dims to idata.
        const SlotInfo& xsi = info[ins[0]];
        if (xsi.rows == 0 || !xsi.data_like)
          fail(e.name + ": X must be a data matrix");
        idata.push_back((int)xsi.rows);
        idata.push_back((int)xsi.cols);
      }
      Val dv = emit(d.op, ins, 1, {}, idata);
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
      // stan-math evaluates log_softmax(beta) then picks the outcomes,
      // which is exactly this composition.
      if (e.fn_propto && e.args[1].data_only) return Val{const_slot(0.0), {}};
      Val b = lower_expr(e.args[1]);
      Val ls = emit(OP_LOG_SOFTMAX, {b.slot}, info[b.slot].len);
      auto ns = int_arg_values(e.args[0]);
      if (e.args[0].type_ == "UInt" && ns.size() == 1)
        return emit(OP_INDEX, {ls.slot}, 1, {}, {ns[0] - 1});
      std::vector<int> idata;
      for (int n : ns) idata.push_back(n - 1);
      Val ga = emit(OP_GATHER, {ls.slot}, (int64_t)idata.size(), {}, idata);
      return emit(OP_SUM_VEC, {ga.slot}, 1);
    }
    if (e.name == "categorical_lpmf" && e.args.size() == 2) {
      // stan-math computes log(theta[n-1]) on the scalar type directly (no
      // ops_partials), so this decomposes exactly onto existing ops. For an
      // array outcome the reference logs the whole simplex once and gathers,
      // which also fixes the adjoint association for repeated categories.
      if (e.fn_propto && e.args[1].data_only) return Val{const_slot(0.0), {}};
      Val th = lower_expr(e.args[1]);
      auto ns = int_arg_values(e.args[0]);
      if (e.args[0].type_ == "UInt" && ns.size() == 1) {
        Val el = emit(OP_INDEX, {th.slot}, 1, {}, {ns[0] - 1});
        return emit(OP_LOGV, {el.slot}, 1);
      }
      Val lg = emit(OP_LOGV, {th.slot}, info[th.slot].len);
      std::vector<int> idata;
      for (int n : ns) idata.push_back(n - 1);
      Val ga = emit(OP_GATHER, {lg.slot}, (int64_t)idata.size(), {}, idata);
      return emit(OP_SUM_VEC, {ga.slot}, 1);
    }

    if ((e.name == "multi_normal_cholesky_lpdf" ||
         e.name == "multi_normal_lpdf" ||
         e.name == "multi_normal_prec_lpdf") && e.args.size() == 3) {
      Val y = lower_expr(e.args[0]);
      Val mu = lower_expr(e.args[1]);
      Val m = lower_expr(e.args[2]);
      uint8_t variant = 0;
      for (int i = 0; i < 3; ++i)
        if (!e.args[i].data_only) variant |= (uint8_t)(1u << i);
      if (e.fn_propto) variant |= 0x80u;
      // K comes from the matrix argument; y may be one K-vector or an
      // array of m of them (stan-math's vectorized signature).
      if (m.si.rows == 0)
        fail(e.name + ": needs a matrix argument (got length " +
                 std::to_string(info[m.slot].len) + ")",
             e.raw);
      const int64_t K = m.si.rows;
      const int64_t reps = info[y.slot].len / K;
      const uint16_t mn_op =
          e.name.find("cholesky") != std::string::npos
              ? OP_MULTI_NORMAL_CHOL_LPDF
              : (e.name.find("prec") != std::string::npos
                     ? OP_MULTI_NORMAL_PREC_LPDF
                     : OP_MULTI_NORMAL_LPDF);
      Val v = emit(mn_op,
                   {y.slot, mu.slot, m.slot}, 1, {},
                   {(int)K, (int)reps});
      g.ops.back().variant = variant;
      return v;
    }
    if ((e.name == "lkj_corr_cholesky_lpdf" || e.name == "lkj_corr_lpdf") &&
        e.args.size() == 2) {
      Val L = lower_expr(e.args[0]);
      Val eta = lower_expr(e.args[1]);
      if (L.si.rows == 0) fail(e.name + " needs a matrix", e.raw);
      Val v = emit(e.name == "lkj_corr_lpdf" ? OP_LKJ_CORR_LPDF
                                             : OP_LKJ_CORR_CHOL_LPDF,
                   {L.slot, eta.slot}, 1, {}, {(int)L.si.rows});
      g.ops.back().variant = (uint8_t)((e.fn_propto ? 0x80u : 0u) | 0x1u);
      return v;
    }
    // lkj_cov(Sigma | mu, sigma, eta).
    if (e.name == "lkj_cov_lpdf" && e.args.size() == 4) {
      Val S = lower_expr(e.args[0]);
      Val mu = lower_expr(e.args[1]);
      Val sig = lower_expr(e.args[2]);
      Val eta = lower_expr(e.args[3]);
      if (S.si.rows == 0) fail("lkj_cov needs a matrix", e.raw);
      Val v = emit(OP_LKJ_COV_LPDF, {S.slot, mu.slot, sig.slot, eta.slot}, 1,
                   {}, {(int)S.si.rows});
      g.ops.back().variant = (uint8_t)((e.fn_propto ? 0x80u : 0u) | 0xfu);
      return v;
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
      if (X.si.rows == 0) fail("binomial_logit_glm needs a matrix", e.raw);
      // Both int groups are one value per row, so a scalar broadcasts.
      const int64_t rows = X.si.rows;
      if ((int64_t)idata.size() == 1) idata.assign(rows, idata[0]);
      if ((int64_t)NN.size() == 1) NN.assign(rows, NN[0]);
      idata.insert(idata.end(), NN.begin(), NN.end());
      idata.push_back((int)rows);
      idata.push_back((int)X.si.cols);
      Val v = emit(OP_BINOMIAL_LOGIT_GLM_LPMF,
                   {X.slot, alpha.slot, beta.slot}, 1, {}, idata);
      g.ops.back().variant = (uint8_t)((e.fn_propto ? 0x80u : 0u) | 0x7u);
      return v;
    }
    if ((e.name == "categorical_logit_glm_lpmf" ||
         e.name == "ordered_logistic_glm_lpmf") &&
        e.args.size() == 4) {
      std::vector<int> idata = int_arg_values(e.args[0]);
      Val X = lower_expr(e.args[1]);
      Val a2 = lower_expr(e.args[2]);
      Val a3 = lower_expr(e.args[3]);
      if (X.si.rows == 0) fail(e.name + " needs a matrix", e.raw);
      if ((int64_t)idata.size() == 1) idata.assign(X.si.rows, idata[0]);
      idata.push_back((int)X.si.rows);
      idata.push_back((int)X.si.cols);
      // categorical: (y, x, alpha, beta). ordered: (y, x, beta, cuts).
      // Both pass their two real arguments in order, so the kernel reads
      // in[1] and in[2] and knows from its Kind what they mean.
      Val v = emit(e.name == "categorical_logit_glm_lpmf"
                       ? OP_CATEGORICAL_LOGIT_GLM_LPMF
                       : OP_ORDERED_LOGISTIC_GLM_LPMF,
                   {X.slot, a2.slot, a3.slot}, 1, {}, idata);
      g.ops.back().variant = (uint8_t)((e.fn_propto ? 0x80u : 0u) | 0x7u);
      return v;
    }

    // multi_gp: (matrix y[K x N], matrix Sigma[K x K], vector w[K]).
    if ((e.name == "multi_gp_lpdf" || e.name == "multi_gp_cholesky_lpdf") &&
        e.args.size() == 3) {
      Val y = lower_expr(e.args[0]);
      Val S = lower_expr(e.args[1]);
      Val w = lower_expr(e.args[2]);
      if (y.si.rows == 0 || S.si.rows == 0)
        fail(e.name + " needs matrix arguments", e.raw);
      Val v = emit(e.name == "multi_gp_lpdf" ? OP_MULTI_GP_LPDF
                                             : OP_MULTI_GP_CHOL_LPDF,
                   {y.slot, S.slot, w.slot}, 1, {},
                   {(int)y.si.rows, (int)y.si.cols});
      g.ops.back().variant = (uint8_t)((e.fn_propto ? 0x80u : 0u) | 0x7u);
      return v;
    }

    // multi_student_t: (vector y, real nu, vector mu, matrix Sigma). K from
    // the matrix; y may be an array of K-vectors, as multi_normal allows.
    if ((e.name == "multi_student_t_lpdf" ||
         e.name == "multi_student_t_cholesky_lpdf") &&
        e.args.size() == 4) {
      Val y = lower_expr(e.args[0]);
      Val nu = lower_expr(e.args[1]);
      Val mu = lower_expr(e.args[2]);
      Val S = lower_expr(e.args[3]);
      if (S.si.rows == 0) fail(e.name + " needs a matrix argument", e.raw);
      const int64_t K = S.si.rows;
      const int64_t reps = info[y.slot].len / K;
      Val v = emit(e.name == "multi_student_t_lpdf"
                       ? OP_MULTI_STUDENT_T_LPDF
                       : OP_MULTI_STUDENT_T_CHOL_LPDF,
                   {y.slot, nu.slot, mu.slot, S.slot}, 1, {},
                   {(int)K, (int)reps});
      g.ops.back().variant = (uint8_t)((e.fn_propto ? 0x80u : 0u) | 0xfu);
      return v;
    }

    // The multinomial family: (array[] int ns, vector theta). The counts
    // must be data (they ride in idata); theta is the only edge.
    {
      static const std::map<std::string, uint16_t> kMult = {
          {"multinomial_lpmf", OP_MULTINOMIAL_LPMF},
          {"multinomial_logit_lpmf", OP_MULTINOMIAL_LOGIT_LPMF},
          {"dirichlet_multinomial_lpmf", OP_DIRICHLET_MULTINOMIAL_LPMF},
      };
      auto mit = kMult.find(e.name);
      if (mit != kMult.end() && e.args.size() == 2) {
        std::vector<int> ns = int_arg_values(e.args[0]);
        Val th = lower_expr(e.args[1]);
        Val v = emit(mit->second, {th.slot}, 1, {}, ns);
        g.ops.back().variant = (uint8_t)((e.fn_propto ? 0x80u : 0u) | 0x1u);
        return v;
      }
    }

    // ordered_probit(y | lambda, c): outcome in idata, lambda and the
    // cutpoints are real edges.
    if (e.name == "ordered_probit_lpmf" && e.args.size() == 3) {
      std::vector<int> y = int_arg_values(e.args[0]);
      Val lam = lower_expr(e.args[1]);
      Val c = lower_expr(e.args[2]);
      Val v = emit(OP_ORDERED_PROBIT_LPMF, {lam.slot, c.slot}, 1, {}, y);
      g.ops.back().variant = (uint8_t)((e.fn_propto ? 0x80u : 0u) | 0x3u);
      return v;
    }

    // wiener(y | alpha, tau, beta, delta): five real arguments.
    if (e.name == "wiener_lpdf" && e.args.size() == 5) {
      std::vector<int> ins;
      for (int i = 0; i < 5; ++i) ins.push_back(lower_expr(e.args[i]).slot);
      Val v = emit(OP_WIENER_LPDF, ins, 1, {}, {});
      g.ops.back().variant = (uint8_t)((e.fn_propto ? 0x80u : 0u) | 0x1fu);
      return v;
    }

    // The wishart family: (matrix, real, matrix), all four of them. K
    // comes from the first matrix; the kernels bind every argument as var
    // (docs/coverage.md, "write the kernel" tier) so no activity mask is
    // needed, only the propto bit.
    {
      static const std::map<std::string, uint16_t> kWish = {
          {"wishart_lpdf", OP_WISHART_LPDF},
          {"inv_wishart_lpdf", OP_INV_WISHART_LPDF},
          {"wishart_cholesky_lpdf", OP_WISHART_CHOL_LPDF},
          {"inv_wishart_cholesky_lpdf", OP_INV_WISHART_CHOL_LPDF},
      };
      auto wit = kWish.find(e.name);
      if (wit != kWish.end() && e.args.size() == 3) {
        Val W = lower_expr(e.args[0]);
        Val nu = lower_expr(e.args[1]);
        Val S = lower_expr(e.args[2]);
        if (W.si.rows == 0) fail(e.name + " needs a matrix", e.raw);
        Val v = emit(wit->second, {W.slot, nu.slot, S.slot}, 1, {},
                     {(int)W.si.rows});
        g.ops.back().variant = (uint8_t)((e.fn_propto ? 0x80u : 0u) | 0x7u);
        return v;
      }
    }
    if (e.name == "normal_id_glm_lpdf" && e.args.size() == 5) {
      Val y = lower_expr(e.args[0]);
      Val X = lower_expr(e.args[1]);
      if (X.si.rows == 0 || !info[X.slot].data_like)
        fail("normal_id_glm: X must be a data matrix", e.raw);
      Val alpha = lower_expr(e.args[2]);
      Val beta = lower_expr(e.args[3]);
      Val sigma = lower_expr(e.args[4]);
      uint8_t variant = 0;
      for (int i = 0; i < 5; ++i)
        if (!e.args[i].data_only) variant |= (uint8_t)(1u << i);
      if (e.fn_propto) variant |= 0x80u;
      Val v = emit(OP_NORMAL_ID_GLM_LPDF,
                   {y.slot, X.slot, alpha.slot, beta.slot, sigma.slot}, 1, {},
                   {(int)X.si.rows, (int)X.si.cols});
      g.ops.back().variant = variant;
      return v;
    }
    return std::nullopt;
  }

  // Elementwise math, reductions, and dot products.
  std::optional<Val> lower_eltwise_fn(const mir::Expr& e) {
    // Elementwise binaries.
    static const std::map<std::string, uint16_t> kBin = {
        {"Plus__", OP_ADD},      {"Minus__", OP_SUB},
        {"Divide__", OP_DIV},    {"EltTimes__", OP_MUL},
        {"EltDivide__", OP_DIV}, {"Pow__", OP_POW}, {"pow", OP_POW},
    };
    if (e.name == "Times__") {
      Val a = lower_expr(e.args[0]);
      Val b = lower_expr(e.args[1]);
      // Scalar on either side is an elementwise scale, whatever shape the
      // other operand carries.
      if (info[a.slot].len == 1 || info[b.slot].len == 1) {
        const Val& shaped = info[a.slot].len == 1 ? b : a;
        SlotInfo si = shaped.si;
        si.data_like = info[a.slot].data_like && info[b.slot].data_like;
        return emit(OP_MUL, {a.slot, b.slot},
                    std::max(info[a.slot].len, info[b.slot].len), si);
      }
      if (a.si.rows > 0) {
        if (info[a.slot].data_like && b.si.rows == 0) {
          // Data matrix * vector keeps the tuned MATVEC kernel (its
          // accumulation order is matched to the var path).
          SlotInfo si;
          return emit(OP_MATVEC, {a.slot, b.slot}, a.si.rows, si,
                      {(int)a.si.rows, (int)a.si.cols});
        }
        // General product; a vector operand is one column.
        const int64_t cb = b.si.rows > 0 ? b.si.cols : 1;
        const int64_t rb = b.si.rows > 0 ? b.si.rows : info[b.slot].len;
        if (rb != a.si.cols)
          fail("Times__: inner dimension mismatch (" +
                   std::to_string(a.si.rows) + "x" +
                   std::to_string(a.si.cols) + " times " +
                   std::to_string(rb) + "x" + std::to_string(cb) + ")",
               e.raw);
        SlotInfo si;
        si.rows = a.si.rows;
        si.cols = cb;
        if (cb == 1) si.rows = 0, si.cols = 0;  // result is a vector
        Val v = emit(OP_GEMM, {a.slot, b.slot}, a.si.rows * cb, si,
                     {(int)a.si.rows, (int)a.si.cols, (int)cb});
        return v;
      }
      // vector * row_vector with a matrix result is an outer product.
      if (b.si.rows == 0 && e.type_ == "UMatrix") {
        const int64_t nr = info[a.slot].len, nc = info[b.slot].len;
        SlotInfo si;
        si.rows = nr;
        si.cols = nc;
        return emit(OP_GEMM, {a.slot, b.slot}, nr * nc, si,
                    {(int)nr, 1, (int)nc});
      }
      // row_vector * vector with scalar result type is an inner product.
      if ((e.type_ == "UReal" || e.type_ == "UInt") &&
          info[a.slot].len > 1 && info[b.slot].len > 1)
        return emit(OP_DOT, {a.slot, b.slot}, 1);
      const int64_t len = std::max(info[a.slot].len, info[b.slot].len);
      return emit(OP_MUL, {a.slot, b.slot}, len);
    }
    auto bit = kBin.find(e.name);
    if (bit != kBin.end()) {
      Val a = lower_expr(e.args[0]);
      Val b = lower_expr(e.args[1]);
      const int64_t la = info[a.slot].len, lb = info[b.slot].len;
      if (la != lb && la != 1 && lb != 1)
        fail(e.name + ": incompatible lengths");
      // Elementwise results keep the matrix shape of whichever operand
      // has one; losing it would make a later Times__ miss the matvec.
      SlotInfo si = shape_of(a, b);
      return emit(bit->second, {a.slot, b.slot}, std::max(la, lb), si);
    }

    // Elementwise unaries + reductions.
    static const std::map<std::string, uint16_t> kUn = {
// Generated from STANLI_SCALAR_UNARY_LIST (optable.hpp), which also made
// the opcode and the kernel.
#define STANLI_UNARY_TABLE(code, name, v, d) {#name, code},
        STANLI_SCALAR_UNARY_LIST(STANLI_UNARY_TABLE)
#undef STANLI_UNARY_TABLE
        {"PMinus__", OP_NEG}, {"exp", OP_EXPV},      {"log", OP_LOGV},
        {"inv_logit", OP_INV_LOGIT}, {"sqrt", OP_SQRT},
        {"square", OP_SQUARE}, {"log1m", OP_LOG1M},  {"softmax", OP_SOFTMAX},
        {"tanh", OP_TANHV},    {"cumulative_sum", OP_CUMSUM},
        {"log_inv_logit", OP_LOG_INV_LOGIT},
        {"log1m_inv_logit", OP_LOG1M_INV_LOGIT},
        {"log_softmax", OP_LOG_SOFTMAX},
    };
    auto uit = kUn.find(e.name);
    if (uit != kUn.end()) {
      Val a = lower_expr(e.args[0]);
      SlotInfo si;
      // Shape-preserving unaries keep rows/cols (softmax/cumulative_sum
      // are vector-only, so they never carry one).
      if (uit->second != OP_SOFTMAX && uit->second != OP_CUMSUM) {
        si.rows = a.si.rows;
        si.cols = a.si.cols;
      }
      si.data_like = info[a.slot].data_like;
      return emit(uit->second, {a.slot}, info[a.slot].len, si);
    }
    if (e.name == "PPlus__") return lower_expr(e.args[0]);
    if (e.name == "logit") {
      Val a = lower_expr(e.args[0]);
      return emit(OP_LOGIT, {a.slot}, info[a.slot].len);
    }
    if (e.name == "mean") {
      Val a = lower_expr(e.args[0]);
      return emit(OP_MEAN, {a.slot}, 1);
    }
    if (e.name == "rep_vector") {
      Val a = lower_expr(e.args[0]);
      const long n = eval_int(e.args[1]);
      return emit(OP_REP_VEC, {a.slot}, n);
    }
    if (e.name == "log_sum_exp" || e.name == "sum") {
      if (e.name == "log_sum_exp" && e.args.size() == 2) {
        Val a = lower_expr(e.args[0]);
        Val b = lower_expr(e.args[1]);
        return emit(OP_LSE2, {a.slot, b.slot}, 1);
      }
      Val a = lower_expr(e.args[0]);
      return emit(e.name == "sum" ? OP_SUM_VEC : OP_LOG_SUM_EXP, {a.slot}, 1);
    }
    if (e.name == "log_diff_exp" && e.args.size() == 2) {
      Val a = lower_expr(e.args[0]);
      Val b = lower_expr(e.args[1]);
      return emit(OP_LOG_DIFF_EXP, {a.slot, b.slot}, 1);
    }
    if (e.name == "log_mix" && e.args.size() == 3) {
      Val a = lower_expr(e.args[0]);
      Val b = lower_expr(e.args[1]);
      Val c = lower_expr(e.args[2]);
      return emit(OP_LOG_MIX, {a.slot, b.slot, c.slot}, 1);
    }
    if (e.name == "dot_product") {
      Val a = lower_expr(e.args[0]);
      Val b = lower_expr(e.args[1]);
      return emit(OP_DOT, {a.slot, b.slot}, 1);
    }

    if (e.name == "dot_self") {
      Val a = lower_expr(e.args[0]);
      return emit(OP_DOT, {a.slot, a.slot}, 1);
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
      if (a.si.rows == 0) return a;
      SlotInfo si;
      si.rows = a.si.cols;
      si.cols = a.si.rows;
      si.data_like = info[a.slot].data_like;
      return emit(OP_TRANSPOSE, {a.slot}, info[a.slot].len, si,
                  {(int)a.si.rows, (int)a.si.cols});
    }
    if ((e.name == "diag_pre_multiply" || e.name == "diag_post_multiply") &&
        e.args.size() == 2) {
      // diag_pre_multiply(v, M) = diag_matrix(v) * M (and the mirror);
      // the explicit zeros contribute exactly nothing to each sum.
      const bool pre = e.name.find("_pre_") != std::string::npos;
      Val v = lower_expr(e.args[pre ? 0 : 1]);
      Val m = lower_expr(e.args[pre ? 1 : 0]);
      const int64_t n = info[v.slot].len;
      SlotInfo dsi;
      dsi.rows = n;
      dsi.cols = n;
      dsi.data_like = info[v.slot].data_like;
      Val d = emit(OP_DIAG_MATRIX, {v.slot}, n * n, dsi);
      Val a = pre ? d : m, b = pre ? m : d;
      SlotInfo si;
      si.rows = a.si.rows;
      si.cols = b.si.cols;
      return emit(OP_GEMM, {a.slot, b.slot}, si.rows * si.cols, si,
                  {(int)a.si.rows, (int)a.si.cols, (int)b.si.cols});
    }
    if (e.name == "multiply_lower_tri_self_transpose" && e.args.size() == 1) {
      Val L = lower_expr(e.args[0]);
      if (L.si.rows == 0) fail("multiply_lower_tri: needs a matrix", e.raw);
      SlotInfo tsi;
      tsi.rows = L.si.cols;
      tsi.cols = L.si.rows;
      Val Lt = emit(OP_TRANSPOSE, {L.slot}, info[L.slot].len, tsi,
                    {(int)L.si.rows, (int)L.si.cols});
      SlotInfo si;
      si.rows = L.si.rows;
      si.cols = L.si.rows;
      return emit(OP_GEMM, {L.slot, Lt.slot}, si.rows * si.cols, si,
                  {(int)L.si.rows, (int)L.si.cols, (int)L.si.rows});
    }
    if (e.name == "to_matrix" && (e.args.size() == 1 || e.args.size() == 3)) {
      // Col-major storage makes reshaping a relabelling. One argument on an
      // array[N] vector[S] value yields the N x S matrix stan-math builds
      // from it, which is the transpose of our array-major flat order.
      Val a = lower_expr(e.args[0]);
      SlotInfo si;
      si.data_like = info[a.slot].data_like;
      if (e.args.size() == 3) {
        si.rows = eval_int(e.args[1]);
        si.cols = eval_int(e.args[2]);
        return Val{a.slot, si};
      }
      if (a.si.rows > 0) return Val{a.slot, a.si};
      std::vector<int64_t> dims;
      if (e.args[0].kind == mir::Expr::Var) {
        auto dd = decl_dims.find(e.args[0].name);
        if (dd != decl_dims.end()) dims = dd->second;
      }
      if (dims.size() != 2)
        fail("to_matrix: unknown source shape", e.raw);
      // array-major (row-major) source -> col-major matrix of the same
      // logical shape: transpose the storage.
      si.rows = dims[0];
      si.cols = dims[1];
      return emit(OP_TRANSPOSE, {a.slot}, info[a.slot].len, si,
                  {(int)dims[1], (int)dims[0]});
    }
    if ((e.name == "to_vector" || e.name == "to_row_vector") &&
        e.args.size() == 1) {
      // Col-major flattening is the identity on our storage.
      Val a = lower_expr(e.args[0]);
      SlotInfo si = a.si;
      si.rows = 0;
      si.cols = 0;
      return Val{a.slot, si};
    }
    if (e.name == "rep_matrix") {
      SlotInfo si;
      if (e.args.size() == 3) {
        Val x = lower_expr(e.args[0]);  // scalar fill
        const long R = eval_int(e.args[1]), C = eval_int(e.args[2]);
        si.rows = R;
        si.cols = C;
        return emit(OP_REP_MAT, {x.slot}, R * C, si, {(int)R, (int)C, 0});
      }
      if (e.args.size() == 2) {
        Val v = lower_expr(e.args[0]);
        const long n = eval_int(e.args[1]);
        const bool rowvec = e.args[0].type_ == "URowVector";
        const long R = rowvec ? n : info[v.slot].len;
        const long C = rowvec ? info[v.slot].len : n;
        si.rows = R;
        si.cols = C;
        return emit(OP_REP_MAT, {v.slot}, R * C, si,
                    {(int)R, (int)C, rowvec ? 2 : 1});
      }
      fail("rep_matrix arity", e.raw);
    }
    if (e.name == "gp_exp_quad_cov" && e.args.size() == 3) {
      Val x = lower_expr(e.args[0]);
      Val alpha = lower_expr(e.args[1]);
      Val rho = lower_expr(e.args[2]);
      if (!info[x.slot].data_like)
        fail("gp_exp_quad_cov: parameter inputs unsupported", e.raw);
      // x is array[N] real (D == 1) or array[N] vector[D], stored
      // array-major, so D falls out of the declared dims.
      int64_t D = 1;
      if (e.args[0].kind == mir::Expr::Var) {
        auto dd = decl_dims.find(e.args[0].name);
        if (dd != decl_dims.end() && dd->second.size() == 2)
          D = dd->second[1];
        else if (DataMap::Entry* en = td.find(e.args[0].name))
          if (en->dims.size() == 2) D = en->dims[1];
      }
      const int64_t N = info[x.slot].len / D;
      SlotInfo si;
      si.rows = N;
      si.cols = N;
      return emit(OP_GP_EXP_QUAD_COV, {x.slot, alpha.slot, rho.slot}, N * N,
                  si, {(int)N, (int)D});
    }
    if (e.name == "diag_matrix" && e.args.size() == 1) {
      Val v = lower_expr(e.args[0]);
      const int64_t n = info[v.slot].len;
      SlotInfo si;
      si.rows = n;
      si.cols = n;
      return emit(OP_DIAG_MATRIX, {v.slot}, n * n, si);
    }
    if (e.name == "cholesky_decompose" && e.args.size() == 1) {
      Val a = lower_expr(e.args[0]);
      if (a.si.rows == 0) fail("cholesky_decompose needs a matrix", e.raw);
      SlotInfo si = a.si;
      si.data_like = info[a.slot].data_like;
      return emit(OP_CHOLESKY, {a.slot}, info[a.slot].len, si,
                  {(int)a.si.rows});
    }

    if ((e.name == "eigenvalues_sym" || e.name == "eigenvectors_sym") &&
        e.args.size() == 1) {
      Val a = lower_expr(e.args[0]);
      if (a.si.rows == 0) fail(e.name + ": needs a matrix", e.raw);
      const int64_t n = a.si.rows;
      if (e.name == "eigenvalues_sym")
        return emit(OP_EIGENVALUES_SYM, {a.slot}, n, {}, {(int)n});
      SlotInfo si;
      si.rows = n;
      si.cols = n;
      return emit(OP_EIGENVECTORS_SYM, {a.slot}, n * n, si, {(int)n});
    }
    if (e.name == "quad_form_diag" && e.args.size() == 2) {
      // quad_form_diag(M, v) = diag(v) * M * diag(v).
      Val m = lower_expr(e.args[0]);
      Val v = lower_expr(e.args[1]);
      if (m.si.rows == 0) fail("quad_form_diag: needs a matrix", e.raw);
      const int64_t n = info[v.slot].len;
      SlotInfo dsi;
      dsi.rows = n;
      dsi.cols = n;
      dsi.data_like = info[v.slot].data_like;
      Val d = emit(OP_DIAG_MATRIX, {v.slot}, n * n, dsi);
      SlotInfo si;
      si.rows = n;
      si.cols = n;
      Val left = emit(OP_GEMM, {d.slot, m.slot}, n * n, si,
                      {(int)n, (int)n, (int)n});
      return emit(OP_GEMM, {left.slot, d.slot}, n * n, si,
                  {(int)n, (int)n, (int)n});
    }

    if ((e.name == "append_row" || e.name == "append_col") &&
        e.args.size() == 2) {
      Val a = lower_expr(e.args[0]);
      Val b = lower_expr(e.args[1]);
      const int64_t la = info[a.slot].len, lb = info[b.slot].len;
      SlotInfo si;
      if (e.name == "append_col") {
        // Col-major storage: appending columns is a contiguous concat.
        const int64_t ra = a.si.rows > 0 ? a.si.rows : la;
        const int64_t rb = b.si.rows > 0 ? b.si.rows : lb;
        if (ra != rb) fail("append_col row mismatch", e.raw);
        si.rows = ra;
        si.cols = (a.si.rows > 0 ? a.si.cols : 1) +
                  (b.si.rows > 0 ? b.si.cols : 1);
      } else if (a.si.rows > 0 || b.si.rows > 0) {
        // Stacking rows interleaves columns in col-major storage:
        // concatenate flat, then gather into destination order.
        const int64_t ra = a.si.rows > 0 ? a.si.rows : 1;
        const int64_t rb = b.si.rows > 0 ? b.si.rows : 1;
        const int64_t ca = a.si.rows > 0 ? a.si.cols : la;
        const int64_t cb = b.si.rows > 0 ? b.si.cols : lb;
        if (ca != cb) fail("append_row column mismatch", e.raw);
        SlotInfo csi;
        Val cat = emit(OP_CONCAT2, {a.slot, b.slot}, la + lb, csi);
        std::vector<int> idx;
        idx.reserve(la + lb);
        for (int64_t j = 0; j < ca; ++j) {
          for (int64_t i = 0; i < ra; ++i) idx.push_back((int)(j * ra + i));
          for (int64_t i = 0; i < rb; ++i)
            idx.push_back((int)(la + j * rb + i));
        }
        si.rows = ra + rb;
        si.cols = ca;
        return emit(OP_GATHER, {cat.slot}, la + lb, si, idx);
      }
      return emit(OP_CONCAT2, {a.slot, b.slot}, la + lb, si);
    }
    if (e.name == "segment" && e.args.size() == 3) {
      Val a = lower_expr(e.args[0]);
      const long from = eval_int(e.args[1]);
      const long cnt = eval_int(e.args[2]);
      return emit(OP_SLICE, {a.slot}, cnt, {}, {(int)(from - 1)});
    }
    if (e.name == "sub_col" && e.args.size() == 4) {
      // sub_col(M, i, j, n) = M[i .. i+n-1, j]: contiguous in col-major.
      Val a = lower_expr(e.args[0]);
      if (a.si.rows == 0) fail("sub_col on a slot without matrix shape");
      const long i = eval_int(e.args[1]);
      const long j = eval_int(e.args[2]);
      const long n = eval_int(e.args[3]);
      return emit(OP_SLICE, {a.slot}, n,
                  {}, {(int)((j - 1) * a.si.rows + i - 1)});
    }
    if (e.name == "col" && e.args.size() == 2) {
      Val a = lower_expr(e.args[0]);
      if (a.si.rows == 0) fail("col on a slot without matrix shape");
      const long j = eval_int(e.args[1]);
      return emit(OP_SLICE, {a.slot}, a.si.rows,
                  {}, {(int)((j - 1) * a.si.rows)});
    }
    if (e.name == "row" && e.args.size() == 2) {
      Val a = lower_expr(e.args[0]);
      if (a.si.rows == 0) fail("row on a slot without matrix shape");
      const long i = eval_int(e.args[1]);
      return emit(OP_SLICE_STRIDED, {a.slot}, a.si.cols, {},
                  {(int)(i - 1), (int)a.si.rows});
    }
    if ((e.name == "head" || e.name == "tail") && e.args.size() == 2) {
      Val a = lower_expr(e.args[0]);
      const long n = eval_int(e.args[1]);
      const long off = e.name == "head" ? 0 : info[a.slot].len - n;
      return emit(OP_SLICE, {a.slot}, n, {}, {(int)off});
    }
    return std::nullopt;
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
    static const std::vector<std::pair<const char*, OdeSpec::Solver>>
        kSolvers = {{"ode_bdf", OdeSpec::BDF},
                    {"ode_adams", OdeSpec::ADAMS},
                    {"ode_rk45", OdeSpec::RK45},
                    {"ode_ckrk", OdeSpec::CKRK}};
    std::string base = e.name;
    bool with_tol = false;
    if (base.size() > 4 && base.compare(base.size() - 4, 4, "_tol") == 0) {
      with_tol = true;
      base = base.substr(0, base.size() - 4);
    }
    const auto sit = std::find_if(
        kSolvers.begin(), kSolvers.end(),
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
    spec->stiff = spec->solver == OdeSpec::BDF ||
                  spec->solver == OdeSpec::ADAMS;
    // stan-math's defaults, per solver: the CVODES pair is far tighter
    // than the Runge-Kutta pair, and using one set for both is how
    // one_comp_mm's gradients ended up 2.9e-6 off CmdStan once already.
    if (spec->stiff) {
      spec->rtol = 1e-10;
      spec->atol = 1e-10;
      spec->max_steps = 100000000;
    }
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
      const bool is_int = a.type_.find("UInt") != std::string::npos;
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
        ra.len = (int)info[v.slot].len;
        param_parts.push_back(v);
      }
      rargs.push_back(std::move(ra));
    }

    Val z0 = lower_expr(e.args[1]);
    const int64_t S = info[z0.slot].len;
    const int64_t N = (int64_t)spec->ts.size();

    // One contiguous theta. A model with a single parameter argument --
    // which is most of them -- gets its slot used directly and pays for
    // no copy at all; more than one chains through CONCAT2, whose
    // backward already splits the adjoint back out.
    Val theta;
    if (param_parts.empty()) {
      theta = {const_slot(0.0), {}};  // len 1, unread: n_th is 0
    } else {
      theta = param_parts[0];
      int64_t acc = info[theta.slot].len;
      for (size_t k = 1; k < param_parts.size(); ++k) {
        const int64_t add = info[param_parts[k].slot].len;
        theta = emit(OP_CONCAT2, {theta.slot, param_parts[k].slot}, acc + add);
        acc += add;
      }
    }

    spec->args = rargs;
    spec->prog = compile_rhs_args(*spec->rhs(), *spec->funs(), (int)S, rargs);
    // Falling back to the interpreter is correct but ~30x slower, so make
    // it findable rather than silent.
    if (!spec->prog.ok && std::getenv("STANLI_DEBUG_ODE"))
      std::fprintf(stderr,
                   "stanli: ODE right-hand side %s falls back to the "
                   "interpreter: %s\n",
                   spec->rhs_name.c_str(), spec->prog.why.c_str());

    Val v = emit(OP_ODE, {z0.slot, theta.slot}, N * S, {}, {(int)N, (int)S});
    g.ops.back().udata = spec.get();
    g.udata_pool.push_back(std::move(spec));
    decl_dims_pending = {N, S};
    return v;
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
      // stan-math's own defaults differ per solver: rk45 1e-6/1e-6/1e6,
      // bdf 1e-10/1e-10/1e8. Using one set for both left one_comp_mm's
      // gradients 2.9e-6 off CmdStan.
      if (spec->stiff) {
        spec->rtol = 1e-10;
        spec->atol = 1e-10;
        spec->max_steps = 100000000;
      }
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
      const int64_t S = info[z0.slot].len;
      const int64_t N = (int64_t)spec->ts.size();
      // Compile the right-hand side now that its argument sizes are known.
      // A failure here is not a compile error: the interpreter still runs it.
      spec->args.resize(3);
      spec->args[0].is_param = true;
      spec->args[0].len = (int)info[theta.slot].len;
      spec->args[1].len = (int)spec->x_r.size();
      spec->args[2].is_int = true;
      spec->args[2].ints = spec->x_i;
      spec->prog = compile_rhs(*spec->rhs(), *spec->funs(), (int)S,
                               (int)info[theta.slot].len,
                               (int)spec->x_r.size(), spec->x_i);
      // Falling back is correct but ~30x slower, so make it findable.
      if (!spec->prog.ok && std::getenv("STANLI_DEBUG_ODE"))
        std::fprintf(stderr,
                     "stanli: ODE right-hand side %s falls back to the "
                     "interpreter: %s\n",
                     spec->rhs_name.c_str(), spec->prog.why.c_str());
      Val v = emit(OP_ODE, {z0.slot, theta.slot}, N * S, {}, {(int)N, (int)S});
      g.ops.back().udata = spec.get();
      g.udata_pool.push_back(std::move(spec));
      decl_dims_pending = {N, S};
      return v;
    }
    return std::nullopt;
  }


  // ---- statements -----------------------------------------------------------
  void lower_read_param(const mir::Stmt& s) {
    // Declared (constrained) size from the read dims; the unconstrained raw
    // size depends on the transform (simplex uses K-1).
    int64_t con_len = 1;
    for (const auto& d : s.read_dims) con_len *= eval_int(d);
    const mir::Transform& tr = *s.read_transform;
    // Batched structured transforms: the last read dim is the per-element
    // size; outer dims multiply into a batch count.
    int64_t inner_con = con_len, n_batch = 1;
    if (!s.read_dims.empty()) {
      inner_con = eval_int(s.read_dims.back());
      n_batch = con_len / inner_con;
    }
    int64_t raw_len = con_len;
    if (tr.kind == mir::Transform::Simplex)
      raw_len = n_batch * (inner_con - 1);
    // sum_to_zero_vector[K]: K constrained from K-1 unconstrained, and
    // unlike the simplex it is volume-preserving, so there is no jacobian
    // term at all.
    if (tr.kind == mir::Transform::SumToZero)
      raw_len = n_batch * (inner_con - 1);
    // unit_vector[K] is K from K: the constraint costs no dimension, it
    // just curves the space (and does carry a jacobian).
    if (tr.kind == mir::Transform::CholeskyCorr) {
      // cholesky_factor_corr[K]: K*K constrained, K*(K-1)/2 unconstrained.
      const int64_t K = inner_con;
      n_batch = 1;
      raw_len = K * (K - 1) / 2;
      con_len = K * K;
    }
    // The matrix-valued transforms take their dimensions from the read
    // dims rather than from the flattened length, which cannot tell
    // corr_matrix[3] from cov_matrix[2].
    int64_t chol_M = 0, chol_N = 0;
    if (tr.kind == mir::Transform::Correlation ||
        tr.kind == mir::Transform::Covariance) {
      const int64_t K = inner_con;
      n_batch = 1;
      raw_len = tr.kind == mir::Transform::Correlation
                    ? K * (K - 1) / 2        // k_choose_2
                    : K + K * (K - 1) / 2;   // K + k_choose_2
      con_len = K * K;
    }
    if (tr.kind == mir::Transform::CholeskyCov) {
      // cholesky_factor_cov[M, N] reads two dims; [K] is the square case.
      chol_M = eval_int(s.read_dims[s.read_dims.size() >= 2
                                        ? s.read_dims.size() - 2
                                        : s.read_dims.size() - 1]);
      chol_N = s.read_dims.size() >= 2 ? inner_con : chol_M;
      n_batch = 1;
      raw_len = (chol_N * (chol_N + 1)) / 2 + (chol_M - chol_N) * chol_N;
      con_len = chol_M * chol_N;
    }
    if (s.read_dims.size() > 1) {
      std::vector<int64_t> dims;
      for (const auto& d : s.read_dims) dims.push_back(eval_int(d));
      decl_dims[s.decl_id] = dims;
    }
    SlotInfo psi;
    if (s.decl_type.base == "SMatrix" && s.read_dims.size() == 2) {
      // Matrix params are column-major in the unconstrained vector; the
      // slot advertises its shape so index lowering picks col-major paths.
      psi.rows = eval_int(s.read_dims[0]);
      psi.cols = eval_int(s.read_dims[1]);
    }
    const int raw = add_slot(raw_len, /*is_param=*/true, psi);
    out.param_names.push_back(s.decl_id);
    out.n_unconstrained += raw_len;

    if (tr.kind == mir::Transform::Identity) {
      scope[s.decl_id] = raw;
      // In write_array mode the column order is dictated by the FnWriteParam
      // statements, which come later and cover transformed parameters and
      // generated quantities too; declaration order would be wrong.
      if (!in_write_array) out.views.push_back({s.decl_id, raw, raw_len});
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
        psi.rows = inner_con;
        psi.cols = inner_con;
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
        opcode = OP_CONSTRAIN_SUM_TO_ZERO;
        break;
      case mir::Transform::Correlation:
        opcode = OP_CONSTRAIN_CORR_MATRIX;
        psi.rows = inner_con;
        psi.cols = inner_con;
        break;
      case mir::Transform::Covariance:
        opcode = OP_CONSTRAIN_COV_MATRIX;
        psi.rows = inner_con;
        psi.cols = inner_con;
        break;
      case mir::Transform::CholeskyCov:
        opcode = OP_CONSTRAIN_CHOL_COV;
        psi.rows = chol_M;
        psi.cols = chol_N;
        break;
      default:
        fail("unsupported parameter transform", tr.raw);
    }
    const int jac = add_slot(1, false);
    std::vector<int> tr_idata;
    if (opcode == OP_CONSTRAIN_SIMPLEX || opcode == OP_CONSTRAIN_ORDERED ||
        opcode == OP_CONSTRAIN_POS_ORDERED)
      tr_idata = {(int)n_batch, (int)inner_con};
    if (opcode == OP_CONSTRAIN_UNIT_VECTOR ||
        opcode == OP_CONSTRAIN_SUM_TO_ZERO)
      tr_idata = {(int)n_batch, (int)inner_con};
    if (opcode == OP_CONSTRAIN_CHOL_CORR ||
        opcode == OP_CONSTRAIN_CORR_MATRIX ||
        opcode == OP_CONSTRAIN_COV_MATRIX)
      tr_idata = {(int)inner_con};
    if (opcode == OP_CONSTRAIN_CHOL_COV)
      tr_idata = {(int)chol_M, (int)chol_N};
    Val con = emit(opcode, ins, con_len, psi, tr_idata, jac);
    jac_slots.push_back(jac);
    scope[s.decl_id] = con.slot;
    if (!in_write_array) out.views.push_back({s.decl_id, con.slot, con_len});
  }

  std::map<std::string, std::vector<int64_t>> decl_dims;

  struct DeclShape {
    int64_t len = 0, rows = 0, cols = 0;
    std::vector<int64_t> dims;
  };
  std::map<std::string, DeclShape> decl_lens;

  void lower_stmt(const mir::Stmt& s) {
    switch (s.kind) {
      case mir::Stmt::Decl:
        if (s.read_transform) {
          lower_read_param(s);
        } else if (s.decl_type.base == "SInt") {
          // Int locals are always data-only in Stan; keep them in int_env
          // so size expressions and indices resolve at compile time.
          int_locals.insert(s.decl_id);
          // eval_int, not the interpreter directly: the initializer may be
          // a shape query on a slot-bound value (rows(lscale) inside an
          // inlined function), which only eval_int can answer.
          if (s.has_init) int_env[s.decl_id] = eval_int(s.init);
        } else if (s.has_init) {
          decl_dims_pending.clear();
          scope[s.decl_id] = lower_expr(s.init).slot;
          if (!decl_dims_pending.empty()) {
            decl_dims[s.decl_id] = decl_dims_pending;
            DeclShape sh;
            sh.len = decl_dims_pending[0] * decl_dims_pending[1];
            sh.dims = decl_dims_pending;
            decl_lens[s.decl_id] = sh;
            decl_dims_pending.clear();
          }
        } else {
          DeclShape sh;
          sh.len = sized_len(s.decl_type, &sh.rows, &sh.cols);
          if (s.decl_type.base == "SArray")
            for (const auto& d : s.decl_type.dims)
              sh.dims.push_back(eval_int(d));
          else if (sh.rows > 0)
            sh.dims = {sh.rows, sh.cols};
          decl_lens[s.decl_id] = sh;
          decl_dims[s.decl_id] = sh.dims;
        }
        return;
      case mir::Stmt::Assignment: {
        if (s.lhs_idx.empty() && int_locals.count(s.lhs)) {
          int_env[s.lhs] = eval_int(s.rhs);
          return;
        }
        if (!s.lhs_idx.empty()) {
          // Element write under unrolled control flow: functional update.
          int prev;
          auto it = scope.find(s.lhs);
          if (it != scope.end()) {
            prev = it->second;
          } else {
            auto dl = decl_lens.find(s.lhs);
            if (dl == decl_lens.end())
              fail("indexed assignment to undeclared " + s.lhs);
            SlotInfo si;
            si.data_like = true;
            si.rows = dl->second.rows;
            si.cols = dl->second.cols;
            prev = add_slot(dl->second.len, false, si);
            out.fills.emplace_back(
                prev, std::vector<double>(dl->second.len, 0.0));
          }
          bool all_single = true;
          for (const auto& ix : s.lhs_idx)
            if (ix.name != "IndexSingle") all_single = false;
          auto dd = decl_dims.find(s.lhs);
          const int rhs = lower_expr(s.rhs).slot;
          // Between write w[a:b] = rhs (contiguous on 1-D values).
          if (s.lhs_idx.size() == 1 &&
              s.lhs_idx[0].name == "IndexBetween") {
            const int64_t lo = eval_int(s.lhs_idx[0].args[0]);
            const int64_t hi = eval_int(s.lhs_idx[0].args[1]);
            if (info[rhs].len != hi - lo + 1)
              fail("range assignment size mismatch for " + s.lhs);
            Val nv = emit(OP_SET_SLICE, {prev, rhs}, info[prev].len,
                          info[prev], {(int)(lo - 1)});
            scope[s.lhs] = nv.slot;
            return;
          }
          // Column write M[:, j] = rhs (contiguous in col-major storage).
          if (s.lhs_idx.size() == 2 && s.lhs_idx[0].name == "IndexAll" &&
              s.lhs_idx[1].name == "IndexSingle" && info[prev].rows > 0) {
            const int64_t j = eval_int(s.lhs_idx[1].args[0]) - 1;
            Val nv = emit(OP_SET_SLICE, {prev, rhs}, info[prev].len,
                          info[prev], {(int)(j * info[prev].rows)});
            scope[s.lhs] = nv.slot;
            return;
          }
          // Row-range column write M[a:b, j] = rhs (contiguous within the
          // column).
          if (s.lhs_idx.size() == 2 &&
              s.lhs_idx[0].name == "IndexBetween" &&
              s.lhs_idx[1].name == "IndexSingle" && info[prev].rows > 0) {
            const int64_t lo = eval_int(s.lhs_idx[0].args[0]);
            const int64_t hi = eval_int(s.lhs_idx[0].args[1]);
            const int64_t j = eval_int(s.lhs_idx[1].args[0]) - 1;
            if (info[rhs].len != hi - lo + 1)
              fail("range assignment size mismatch for " + s.lhs);
            Val nv = emit(OP_SET_SLICE, {prev, rhs}, info[prev].len,
                          info[prev],
                          {(int)(j * info[prev].rows + lo - 1)});
            scope[s.lhs] = nv.slot;
            return;
          }
          if (all_single && dd != decl_dims.end() &&
              s.lhs_idx.size() <= dd->second.size() &&
              info[prev].rows == 0) {
            // Array-major offset; sub-array writes become SET_SLICE.
            const auto& D = dd->second;
            const size_t n_idx = s.lhs_idx.size();
            int64_t inner = 1;
            for (size_t d = n_idx; d < D.size(); ++d) inner *= D[d];
            int64_t off = 0;
            for (size_t d = 0; d < n_idx; ++d) {
              int64_t stride = inner;
              for (size_t d2 = d + 1; d2 < n_idx; ++d2) stride *= D[d2];
              off += (eval_int(s.lhs_idx[d].args[0]) - 1) * stride;
            }
            if (inner != info[rhs].len && inner != 1)
              fail("indexed assignment size mismatch for " + s.lhs);
            Val nv = inner == 1
                         ? emit(OP_SET_INDEX, {prev, rhs}, info[prev].len,
                                info[prev], {(int)off})
                         : emit(OP_SET_SLICE, {prev, rhs}, info[prev].len,
                                info[prev], {(int)off});
            scope[s.lhs] = nv.slot;
            return;
          }
          int64_t flat = 0;
          if (all_single && s.lhs_idx.size() == 1) {
            flat = eval_int(s.lhs_idx[0].args[0]) - 1;
          } else if (all_single && s.lhs_idx.size() == 2 &&
                     info[prev].rows > 0) {
            flat = (eval_int(s.lhs_idx[1].args[0]) - 1) * info[prev].rows +
                   (eval_int(s.lhs_idx[0].args[0]) - 1);
          } else {
            std::string desc = "unsupported indexed assignment: lhs=" + s.lhs;
            for (const auto& ix : s.lhs_idx)
              desc += " [" + (ix.name.empty() ? "?" : ix.name) + "]";
            fail(desc, s.raw);
          }
          Val nv = emit(OP_SET_INDEX, {prev, rhs}, info[prev].len, info[prev],
                        {(int)flat});
          scope[s.lhs] = nv.slot;
          return;
        }
        scope[s.lhs] = lower_expr(s.rhs).slot;
        return;
      }
      case mir::Stmt::TargetPE:
        target_terms.push_back(lower_expr(s.target).slot);
        return;
      case mir::Stmt::Block:
      case mir::Stmt::SList:
        for (const auto& k : s.body) lower_stmt(k);
        return;
      case mir::Stmt::Skip:
        return;
      case mir::Stmt::NRFunApp:
        // Compiler-internal checks (FnCheck / FnValidateSize): sizes are
        // enforced at data binding; value checks are skipped.
        if (s.fn_name == "FnCheck" || s.fn_name == "FnValidateSize") return;
        // A vector offset/multiplier makes stanc emit check_matching_dims
        // as a named call rather than an FnCheck. It is a pure SHAPE
        // check, and every shape here is static -- a mismatch would have
        // failed this lowering long before the check ran -- so skipping
        // it is exact rather than a relaxation. Deliberately not a
        // `check_*` prefix match: a value check like check_positive_finite
        // rejects a draw at runtime, and skipping one of those would
        // silently accept points CmdStan refuses.
        if (s.fn_name == "check_matching_dims") return;
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
          CompiledModel::ParamView pv{name, v.slot, info[v.slot].len};
          if (t == "UReal" || t == "UInt" || t == "UComplex") {
            pv.naming = Naming::Scalar;
          } else if (t == "UMatrix") {
            pv.rows = info[v.slot].rows;
            if (pv.rows <= 0)
              fail("FnWriteParam of a matrix with unknown shape: " + name,
                   s.raw);
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
        if (!s.cond.data_only) {  // an island, not a compile error
          lower_param_ifelse(s);
          return;
        }
        const bool c = td.eval(s.cond).r.at(0) != 0.0;
        if (c && !s.body.empty()) lower_stmt(s.body[0]);
        if (!c && s.body.size() > 1) lower_stmt(s.body[1]);
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
        next.push_back(emit(OP_ADD_N, chunk, 1).slot);
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
    for (const auto& f : p.fun_defs) fun_defs[f.name] = &f;
    in_write_array = true;
    // stanc3 guards the two emission groups on these flags; the sampler wants
    // both, so pin them and let the data-only IfElse fold them away.
    int_env["emit_transformed_parameters__"] = 1;
    int_env["emit_generated_quantities__"] = 1;
    CompiledModel::WriteArray wa;
    try {
      for (const auto& s : p.generate_quantities) lower_stmt(s);
    } catch (const CompileError& e) {
      // Whatever lowered before the failure is still correct and still worth
      // emitting: an `normal_rng` late in generated quantities should not
      // cost us the transformed parameters ahead of it.
      wa.truncated = e.what();
    }
    std::vector<int> roots = jac_slots;
    for (const auto& v : out.views) roots.push_back(v.slot);
    make_inplace_updates(g, roots);
    forward_stores_to_loads(g, roots);
    reroll(g, out.fills, target_terms, roots);
    info.resize(g.slots.size());
    // Nothing reads a result here, but forward() asserts a scalar result
    // slot, so point it at one.
    g.result_slot = const_slot(0.0);
    wa.n_unconstrained = out.n_unconstrained;
    wa.graph = std::move(g);
    wa.columns = std::move(out.views);
    wa.fills = std::move(out.fills);
    return wa;
  }

  CompiledModel run(const mir::Program& p) {
    for (const auto& f : p.fun_defs) fun_defs[f.name] = &f;
    bind_data(p);
    for (const auto& s : p.log_prob) lower_stmt(s);
    // Jacobian terms and constrained-parameter views are read straight out
    // of the arena, so no op consumes them and the pass cannot infer them.
    std::vector<int> roots = jac_slots;
    for (const auto& v : out.views) roots.push_back(v.slot);
    // Target terms have no consuming op yet either: reduce_terms builds
    // their ADD_N tree below, after the passes have run.
    std::vector<int> update_roots = roots;
    update_roots.insert(update_roots.end(), target_terms.begin(),
                        target_terms.end());
    make_inplace_updates(g, update_roots);  // off under STANLI_NO_INPLACE
    // Deleting the write/read-back pairs first is what leaves a plain
    // arithmetic lane for reroll to vectorize.
    forward_stores_to_loads(g, update_roots);
    // After the update chains collapse, so a data-only chain is one slot
    // rather than N; before reroll, so the lanes it sees have data operands.
    info.resize(g.slots.size());
    std::vector<int> folded;
    const_fold(g, out.fills, update_roots, &folded);
    for (int s : folded) info[s].data_like = true;
    reroll(g, out.fills, target_terms, roots);  // off under STANLI_NO_REROLL
    // LAST, after every other pass has had first crack: compile whatever
    // scalar residue survives (recurrences the re-roll can never widen)
    // into island ops. Off under STANLI_NO_ISLAND.
    carve_islands(g, out.fills, target_terms, roots);
    info.resize(g.slots.size());  // keep SlotInfo parallel: emit() in
                                  // reduce_terms reads info[o] by slot id
    std::vector<int> all = target_terms;
    all.insert(all.end(), jac_slots.begin(), jac_slots.end());
    g.result_slot = reduce_terms(all);
    out.graph = std::move(g);
    return std::move(out);
  }
};

}  // namespace

CompiledModel compile_model(const std::string& tmir_text, const DataMap& data) {
  // Shared because the interpreted write_array fallback, when needed,
  // keeps the generate_quantities statements and UDF bodies alive for the
  // model's whole life.
  auto prog = std::make_shared<mir::Program>(
      mir::read_program(sexp::parse(tmir_text)));
  Lowering lo(data);
  CompiledModel cm = lo.run(*prog);
  if (!prog->generate_quantities.empty()) {
    // A second lowering, over the transformed data the first one already
    // interpreted: re-running prepare_data would double preparation time on
    // the models where preparation is the cost (nn_rbm1bJ100, 20.7 s).
    Lowering wa(data);
    wa.td.env() = lo.td.env();
    wa.int_env = lo.int_env_data;
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
    }
    if (!w.truncated.empty()) {
      // The graph could not express the whole section; hand the model the
      // per-draw interpreter, seeded with data + transformed data and the
      // emission flags the guard blocks test.
      auto env = lo.td.env();
      for (const char* flag : {"emit_transformed_parameters__",
                               "emit_generated_quantities__"}) {
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
  return cm;
}

}  // namespace stanli
