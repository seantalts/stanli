// MIR -> Program: the shared front end.
//
// Several callers compile MIR into the register machine. A solver
// callback (ODE right-hand side, DAE residual) has to stay callable at
// runtime because the solver picks the points (ode_prog.hpp). A region
// whose control flow depends on a parameter has to become a program
// because it cannot become graph ops at all: `if (theta > 0)` has no
// op-graph form, and until this existed it was a compile error
// (lower.cpp). The region-map lowering and standalone Stan functions
// (function.cpp) compile through it as well.
//
// Storage sizes are known at compile time, even when loop bounds are
// runtime values, and most indices and integers are also known at
// preparation. Runtime integer values use scalar registers and integer
// operations; dynamic storage still requires a separate shape proof.
//
// Names the compiler does not know are the one thing that differs
// between callers. The solver side knows them all up front (t, y, theta,
// x_r, x_i). Lowering does not: a region reads model-block variables
// that already live in graph slots, so it hands over `bind_extern`,
// which allocates registers for such a name and records it as a live-in.
#ifndef STANLI_MIR_PROG_HPP
#define STANLI_MIR_PROG_HPP

#include <stanli/assign_size.hpp>
#include <stanli/debug_switch.hpp>
#include <stanli/builtin_registry.hpp>
#include <stanli/mir_message.hpp>
#include <stanli/mir.hpp>
#include <stanli/density_registry.hpp>
#include <stanli/function_registry.hpp>
#include <stanli/function_view_shape.hpp>
#include <stanli/optable.hpp>
#include <stanli/program.hpp>
#include <stanli/rng_family.hpp>
#include <stanli/structured_loop.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace stanli {

// A value is a contiguous run of registers: scalars are runs of one, arrays
// and vectors are runs of their length.
struct Range {
  int reg = 0;
  int len = 0;
  int64_t rows = 0;
  int64_t cols = 0;
  ViewKind kind = ViewKind::Flat;
  // Complete logical extents for arrays: array extents outer-major, then the
  // leaf's own extents. A matrix leaf occupies its two trailing extents
  // column-major, so `leaf` is what distinguishes array[2] matrix[2,3] from
  // array[2, 2] vector[3], which share both storage width and dims.
  std::vector<int64_t> dims;
  ViewKind leaf = ViewKind::Flat;
};

// One UDF argument in source order. Compile-time integers live outside the
// register file; every other value is a Range. Keeping the tag beside the
// value prevents the old real/int partition from reordering mixed calls.
struct InlineArg {
  Range real;
  std::vector<long> ints;
  std::vector<int64_t> int_dims;
  bool is_const_int = false;
  bool is_const_real = false;
  double const_real = 0.0;
  // Whether CmdStan instantiates the actual as a var.
  bool autodiff = true;
};

struct Bail {
  std::string why;
};

// Every admitted integer operation needs an integer implementation, including
// integer-valued subexpressions promoted into a real return. Register storage
// is double, while the I* instructions execute with Stan's integer type.
inline void check_program_integer_contract(
    const std::vector<stanli::mir::Stmt>& body,
    const std::map<std::string, const stanli::mir::FunDef*>& functions,
    std::set<const stanli::mir::FunDef*>& visited) {
  std::function<void(const mir::Expr&)> expression;
  expression = [&](const mir::Expr& value) {
    if (value.kind == mir::Expr::FunApp) {
      if (value.fn_lib == mir::Expr::Lib::UserDefined) {
        const auto found = functions.find(mir::function_key(value));
        if (found != functions.end() && visited.insert(found->second).second)
          check_program_integer_contract(found->second->body, functions,
                                         visited);
      } else if (const auto* spec = function_spec(value);
                 spec && spec->builtin() &&
                 spec->result() == FunctionArgumentKind::Integer) {
        const auto policy = spec->builtin()->shape;
        const auto op = spec->builtin()->opcode;
        const bool native_integer =
            op == OP_ADD || op == OP_SUB || op == OP_MUL || op == OP_DIV ||
            op == OP_NEG || op == OP_ABS || op == OP_CHOOSE ||
            op == OP_SUM_VEC || op == OP_TO_INT;
        // Stanc retains a negative integer literal as unary negation even
        // when a real comparison consumes it. Its exact value is proven here
        // without invoking integer arithmetic on unknown runtime operands.
        const bool literal_negation =
            spec->builtin()->opcode == OP_NEG && value.args.size() == 1 &&
            value.args[0].kind == mir::Expr::LitInt &&
            value.args[0].lit_i >= -(int64_t)std::numeric_limits<int>::max() &&
            value.args[0].lit_i <= std::numeric_limits<int>::max();
        if (!native_integer && !literal_negation &&
            spec->builtin()->ordering == BuiltinOrdering::None &&
            policy != BuiltinShapePolicy::Predicate &&
            policy != BuiltinShapePolicy::ShapeQuery &&
            policy != BuiltinShapePolicy::SliceView &&
            policy != BuiltinShapePolicy::Constructor &&
            policy != BuiltinShapePolicy::Rng)
          throw Bail{"integer arithmetic requires the MIR interpreter"};
      }
    }
    for (const auto& child : value.args) expression(child);
  };
  std::function<void(const mir::Stmt&)> statement;
  statement = [&](const mir::Stmt& value) {
    for (const auto* expr : {&value.init, &value.rhs, &value.target,
                             &value.lower, &value.upper, &value.cond})
      expression(*expr);
    for (const auto* exprs : {&value.read_dims, &value.lhs_idx, &value.fn_args})
      for (const auto& expr : *exprs) expression(expr);
    for (const auto& child : value.body) statement(child);
  };
  for (const auto& value : body) statement(value);
}

struct ProgramCompiler {
  Program& p;
  const std::map<std::string, const mir::FunDef*>& funs;
  std::map<std::string, Range> reals;
  std::map<std::string, std::vector<long>> ints;
  // Known scalar real formals are retained beside their register binding.
  // The register still supplies ordinary execution; this map is only what
  // lets compile-time control decisions and nested calls retain the value.
  std::map<std::string, double> known_reals;
  // Integer containers still occupy registers when a real-valued expression
  // consumes them, but their values are data and therefore available while
  // the program is being compiled.  Keep that provenance beside the register
  // view: stanc's O1 lowering spells `for (i in {2, 3})` as an unsized array
  // temporary, a whole-array assignment, then scalar indexed reads.
  std::map<std::string, std::vector<long>> known_int_arrays;
  std::map<std::string, std::vector<int64_t>> known_int_array_dims;
  std::set<std::string> int_array_names;
  // Optimizer temporaries declared with an Unsized container acquire their
  // complete Range from the first whole-variable assignment.  A zero-width
  // sized declaration is not the same thing, so keep the protocol explicit
  // rather than inferring it from Range::len.
  std::map<std::string, mir::UnsizedView> deferred_shapes;
  // Containers whose size a runtime-control region chose: an unsized or
  // zero-size local that took the size of an assignment in this region or
  // an earlier one. Another size for one of them is not an error that
  // the program can raise at run time, because the size may belong to a
  // branch that did not run.
  std::set<std::string> region_sized;
  // Where each `ints` entry was declared: its branch depth, and how many
  // loops enclosed it. Folding an assignment records a value for every
  // later read of the name, so it is only sound where the assignment
  // certainly runs -- which is where the declaration itself is. A name the
  // caller seeded has no entry and is treated as declared outside
  // everything, which is what it is.
  std::map<std::string, std::pair<int, size_t>> int_decl_at;
  // Names bind_extern brought in. They are the caller's values rather than
  // the region's, which is what makes them the caller's to answer about --
  // being in `reals` only says the region has read one as a value.
  std::set<std::string> extern_bound;
  // Whether this region belongs to generated quantities. Only there is an
  // RNG draw legal.
  bool in_write_array = false;
  // CmdStan instantiates a user function body with the actual argument
  // types: a formal is a var when its actual is, and a local not declared
  // data is a var when any real actual is. Empty outside a function body,
  // where the MIR adlevel of a variable is its C++ type.
  struct CallTypes {
    std::map<std::string, bool> formals;
    bool locals = true;
  };
  std::optional<CallTypes> call_types;
  // The caller's propto__, which an `_lp` or `_lupdf` call passes on.
  bool propto_ctx = true;
  bool autodiff(const mir::Expr& e) const {
    if (in_write_array) return false;
    const bool locals = !call_types || call_types->locals;
    return mir::instantiated_autodiff(e, locals, [&](const mir::Expr& v) {
      if (!call_types) return true;
      const auto formal = call_types->formals.find(v.name);
      return formal != call_types->formals.end() ? formal->second : locals;
    });
  }
  int branch_depth = 0;  // inside a branch on a runtime value
  // A while is a genuinely runtime loop: its condition and state must be
  // evaluated again on every trip, rather than folded once while the program
  // is being built.  Scalar integers written by such a loop consequently
  // live in ordinary double registers (their producers still preserve Stan's
  // integer-valued operations where they are supported below).
  int structured_while_depth = 0;
  // Runtime for bodies execute repeatedly too. Keep their scalar integer
  // locals in registers so declarations initialize on each executed trip.
  int runtime_for_depth = 0;
  bool structured_while_seen = false;
  // A callee may return normally while its caller is inside a loop. Only
  // loops entered within this function make its return a runtime exit.
  int return_while_base = 0;
  int inline_depth = 0;
  std::vector<std::string> inline_stack;
  struct LoopFrame {
    std::vector<int> breaks;
    std::vector<int> continues;
    bool structured = false;
    int branch_depth = 0;
  };
  std::vector<LoopFrame> loops;
  // A name that is neither a local nor a compile-time integer. The ODE
  // caller leaves this empty (its arguments are all bound up front);
  // lowering installs a hook that allocates registers for the graph slot
  // backing the name and records it as a live-in. Returning false means
  // "no such value", and the compile bails.
  std::function<bool(const std::string&, Range*)> bind_extern;
  // One compile-time integer the region cannot reach on its own: a read
  // from a data array. The caller has the data interpreter that answers
  // sizes outside the region, and cint hands it a read whose indices are
  // already literals -- they are the region's own loop variables, which
  // the caller has never heard of. Lowering installs this; the ODE caller
  // leaves it empty, and cint then refuses as before.
  std::function<bool(const mir::Expr&, long*)> extern_int;
  // The container counterpart to extern_int.  A data-only UDF can produce
  // an integer array whose extent depends on its values (for example
  // `whichequals(x, 1, 1)`).  The host's MIR interpreter already owns those
  // semantics, so let it provide both values and complete logical extents
  // without turning the surrounding structured region into an unroll.
  std::function<bool(const mir::Expr&, std::vector<long>*,
                     std::vector<int64_t>*)>
      extern_ints;
  // A scalar real expression whose value the surrounding lowerer can prove
  // at model-construction time. This is a value callback, not an activity
  // test: generated-quantity draws are inactive but still unknown. Evaluating
  // a complete data-only UDF here also handles recursion without trying to
  // turn a dynamic call stack into finite inline instructions.
  std::function<bool(const mir::Expr&, double*)> extern_real;
  // Families whose algorithm repeatedly invokes its callback are constructed
  // by the owning backend and emitted through Program::CALL. Nested UDFs use
  // this same hook, so they do not need another higher-order dispatch path.
  std::function<bool(const mir::Expr&, Range*)> lower_higher_order;
  // Resolve the target accumulated before this program began. Lowering binds
  // it lazily as a graph live-in; ODE and algebra callers leave it absent.
  std::function<bool(Range*)> bind_target;
  // Where this program's `target +=` delta accumulates, or -1 when the
  // region may not modify target. The caller seeds it to zero and publishes
  // only this delta, never the preceding target supplied by bind_target.
  int target_reg = -1;
  // A region-map body whose only output is a local real that it adds to,
  // `name = name + term`: each term accumulates into reg, as `target +=`
  // accumulates into target_reg. The caller seeds reg to zero.
  struct Accumulator {
    std::string name;
    int reg = -1;
  };
  std::optional<Accumulator> accumulator;
  // Cached register returned by bind_target. Keeping it separate from the
  // delta prevents a region target contribution from double-counting the
  // target that existed before the region.
  int target_base_reg = -1;
  // Register runs allocated by the zero-length adoption in Assignment,
  // which is the one allocation site whose write can sit under a jump.
  // finish() fills them with NaN ahead of the program, restoring the
  // contract run_program states (program.hpp): every register is written
  // before it is read.
  std::vector<std::pair<int, int>> late_bound;
  // Scalar integers are discovered as runtime values while compiling a
  // branch body, after its JZ is already in the stream. Their old value must
  // nevertheless be initialized before control reaches that jump so the
  // untaken path preserves it.
  std::vector<Program::Instr> hoisted_int_initializers;
  // A folded scalar local inside a repeated scope may later need storage.
  // Preserve its lexical position with a temporary marker, so loop backedges
  // can distinguish "before declaration" from "after declaration". finish()
  // removes unused markers; promoted locals replace theirs with a real fill.
  std::map<std::string, int> int_reset_at;
  std::set<int> unused_int_resets;
  void mark_int_reset(const std::string& name) {
    if (!structured_while_depth) return;
    const int at = emit(Program::JMP, 0);
    int_reset_at[name] = at;
    unused_int_resets.insert(at);
  }

  static void assigned_names(const mir::Stmt& s, std::set<std::string>* names) {
    mir::for_each_assigned(
        s, [&](const std::string& name) { names->insert(name); });
    for (const auto& child : s.body) assigned_names(child, names);
  }

  // Unrolling may capture an upper bound only when reevaluation cannot
  // change its value or produce effects. Other bounds run at the loop head.
  bool invariant_for_bound(const mir::Expr& e,
                           const std::set<std::string>& written) {
    // Fixed container geometry does not depend on element mutations.
    // Retain the effect check on the operand even when only its shape matters.
    if (is_shape_query(e)) {
      Range view;
      if (static_view(e.args[0], &view))
        return invariant_for_bound(e.args[0], {});
    }
    switch (e.kind) {
      case mir::Expr::Var:
        return !written.count(e.name);
      case mir::Expr::LitInt:
      case mir::Expr::LitReal:
        return true;
      case mir::Expr::Promotion:
      case mir::Expr::TernaryIf:
      case mir::Expr::EAnd:
      case mir::Expr::EOr:
      case mir::Expr::Indexed:
        break;
      case mir::Expr::IndexAll:
      case mir::Expr::IndexSingle:
      case mir::Expr::IndexUpfrom:
      case mir::Expr::IndexBetween:
      case mir::Expr::IndexMulti:
        break;
      case mir::Expr::FunApp: {
        if (e.name == "FnMakeArray" || e.name == "FnMakeRowVec") break;
        if (e.fn_lib != mir::Expr::Lib::StanLib) return false;
        const auto* spec = function_spec(e);
        if (!spec || !spec->builtin()) return false;
        if (spec->builtin()->shape == BuiltinShapePolicy::Rng) return false;
        break;
      }
      default:
        return false;
    }
    for (const auto& arg : e.args)
      if (!invariant_for_bound(arg, written)) return false;
    return true;
  }

  // Move an already-declared scalar integer into a register before compiling
  // a structured while which writes it.  Keeping it in `ints` would fold the
  // first assignment into every later use and turn a runtime recurrence back
  // into a compile-time unroll.
  void reify_written_int(const std::string& name) {
    auto it = ints.find(name);
    if (it == ints.end()) return;
    if (it->second.size() != 1)
      bail("structured while writes an integer array: " + name);
    const int r = alloc(1);
    const double value = static_cast<double>(it->second[0]);
    const auto reset = int_reset_at.find(name);
    if (reset == int_reset_at.end()) {
      hoisted_int_initializers.push_back(const_instr(r, &value, 1));
    } else {
      p.code[reset->second] = const_instr(r, &value, 1);
      unused_int_resets.erase(reset->second);
      int_reset_at.erase(reset);
    }
    reals[name] = Range{r, 1};
    ints.erase(it);
    int_decl_at.erase(name);
  }

  void reify_loop_write(const std::string& name) {
    if (int_array_names.count(name)) {
      auto known = known_int_arrays.find(name);
      if (!reals.count(name) && known != known_int_arrays.end()) {
        const auto shape = known_int_array_dims.find(name);
        const std::vector<int64_t> dims =
            shape == known_int_array_dims.end()
                ? std::vector<int64_t>{static_cast<int64_t>(
                      known->second.size())}
                : shape->second;
        const auto values = int_array_graph_values(known->second, dims);
        Range value{alloc(static_cast<int>(values.size())),
                    static_cast<int>(values.size())};
        value.kind = ViewKind::Array;
        value.dims = dims;
        emit_const(value.reg, values.data(), value.len);
        reals[name] = value;
      }
      known_int_arrays.erase(name);
      known_int_array_dims.erase(name);
    } else {
      reify_written_int(name);
    }
    // An imported value written on an earlier iteration is no longer a
    // preparation constant, even before the first write in this body.
    if (!reals.count(name) && bind_extern) {
      Range value;
      if (bind_extern(name, &value)) reals[name] = value;
    }
    extern_bound.erase(name);
  }

  // Registers are never recycled, so an unrolled generated-quantities
  // region over a long series (hmm_gaussian, T=500) needs well over 2^16.
  // The cap is a backstop against a pathological unroll.
  static constexpr int kMaxRegs = 1 << 20;

  [[noreturn]] void bail(const std::string& why) { throw Bail{why}; }

  int alloc(int n) {
    if (n < 0 || n > kMaxRegs - p.n_regs)
      bail("right-hand side needs too many registers");
    const int r = p.n_regs;
    p.n_regs += n;
    return r;
  }

  int emit(Program::Code c, int dst, int a = 0, int b = 0, int cc = 0,
           int len = 0) {
    p.code.push_back(Program::Instr{c, dst, a, b, cc, len});
    return (int)p.code.size() - 1;
  }

  // Constants live in the pool, not in the instruction. Equal values share
  // an entry: `declare` zeroes every register it allocates, so a program
  // over a few arrays would otherwise carry hundreds of copies of 0.
  int pool_at(const double* v, int n);

  // dst[0..n) = the given values, as one instruction.
  Program::Instr const_instr(int dst, const double* v, int n);

  void emit_const(int dst, const double* v, int n) {
    if (n == 0) return;
    p.code.push_back(const_instr(dst, v, n));
  }

  int konst(double v) {
    const int r = alloc(1);
    emit_const(r, &v, 1);
    return r;
  }

  // ---- shape queries -------------------------------------------------------
  // rows/cols/size/num_elements are integer functions of a logical view,
  // and every view inside a region is fixed at compile time even where the
  // value is a parameter and the branch around it is not. Answering from
  // the view is the only way these can be answered at all: the register
  // file holds doubles, and no opcode reports an extent.
  static bool is_shape_query(const mir::Expr& e);

  // The registered names answer through the shared resolver, matching the
  // graph's compile-time evaluator; FnLength keeps the size rule those
  // evaluators have always applied to it.
  long shape_query(const mir::Expr& e, Range v);

  int64_t checked_shape_product(const std::vector<int64_t>& dims,
                                const std::string& what);

  std::vector<int64_t> validated_int_array_dims(const std::vector<long>& values,
                                                std::vector<int64_t> dims,
                                                const std::string& what);

  mir::Expr int_array_literal(const std::vector<long>& values,
                              const std::vector<int64_t>& dims);

  void literalize_external_ints(mir::Expr* e);

  bool refers_to_local_runtime(const mir::Expr& e) const;

  bool external_int_array(const mir::Expr& e, std::vector<long>* values,
                          std::vector<int64_t>* dims);

  // As external_int_array: the caller's values are from before this
  // region, so an expression that reads one of the region's own runtime
  // values is not the caller's to answer.
  bool external_real(const mir::Expr& e, double* value);

  // The view of a named value, without building it: a shape query in an
  // integer position (a declared extent, a loop bound, an index) may not
  // emit, because try_cint swallows a Bail and a half-built branch would
  // leave an unpatched jump in the program behind it. Only `len`, `kind`,
  // `rows`, `cols` and `dims` of the result mean anything; `reg` is not a
  // register, because no value was built.
  bool static_view(const mir::Expr& e, Range* out);

  bool static_indexed_view(const mir::Expr& e, Range* out);

  // ---- compile-time integers ----------------------------------------------
  // A comparison returns an integer whatever it compares, so the result
  // type does not say whether cint may answer it: `2.5 > 1` is UInt with
  // real operands, and cint reads a real literal by truncation, which
  // would make it false. The operands have to be integers themselves.
  static bool int_operand(const mir::Expr& e) {
    return e.unsized.leaf == mir::UnsizedLeaf::Int;
  }

  std::optional<double> constant_real(const mir::Expr& e);

  double creal(const mir::Expr& e) {
    if (const auto value = constant_real(e)) return *value;
    bail("real expression is not known at compile time");
  }

  bool try_creal(const mir::Expr& e, double* out);

  long cint(const mir::Expr& e);

  bool try_cint(const mir::Expr& e, long* out);

  // A data-only integer array used by IndexMulti.  Literal arrays are the
  // form O1 emits for source selectors such as `{3, 1}`.  Named arrays cover
  // both ODE x_i arguments and statement-island data bindings; the latter are
  // read one scalar at a time through extern_int, the callback that already
  // owns their storage-order semantics.
  std::vector<long> cints(const mir::Expr& e);

  bool try_cints(const mir::Expr& e, std::vector<long>* out);

  // The 0-based positions that `index` selects along an axis of `extent`.
  // A range selector follows Stan's rule for `site`. When `emit` is set, a
  // range that Stan rejects compiles to a run-time rejection and selects
  // nothing, because the code may sit in a branch that never runs;
  // otherwise it refuses, as a query that cannot answer.
  std::vector<int64_t> matrix_positions(const mir::Expr& index, int64_t extent,
                                        const std::string& axis,
                                        SelectorSite site, bool emit = false,
                                        bool assign = false);

  std::vector<int64_t> graph_array_offsets(
      const std::vector<int64_t>& dims, ViewKind leaf,
      const std::vector<std::vector<int64_t>>& positions);

  std::vector<int64_t> first_fast_array_offsets(
      const std::vector<int64_t>& dims,
      const std::vector<std::vector<int64_t>>& positions);

  std::vector<double> int_array_graph_values(const std::vector<long>& values,
                                             const std::vector<int64_t>& dims);

  static bool same_view(const Range& a, const Range& b);

  static BuiltinArgumentShape builtin_argument_shape(const mir::Expr& source,
                                                     const Range& value) {
    const BuiltinArgumentKind kind =
        source.unsized.leaf == mir::UnsizedLeaf::Int
            ? BuiltinArgumentKind::Integer
            : BuiltinArgumentKind::Real;
    return make_view_function_shape(kind, value.kind, value.leaf, value.dims,
                                    value.len, value.rows, value.cols);
  }

  // Rewrite a Range's geometry to a resolver-reported result shape; `reg`
  // and `len` stay the caller's.
  static Range shaped(Range out, const BuiltinArgumentShape& shape) {
    out.kind = function_view_kind(shape.container);
    out.rows = out.cols = 0;
    out.dims.clear();
    out.leaf = ViewKind::Flat;
    if (out.kind == ViewKind::Matrix) {
      out.rows = shape.dimensions[0];
      out.cols = shape.dimensions[1];
    } else if (out.kind == ViewKind::Array) {
      out.dims = shape.dimensions;
      out.leaf = function_view_kind(shape.array_leaf);
    }
    return out;
  }

  BuiltinLayout resolved_builtin_layout(const mir::Expr& e,
                                        const BuiltinSpec& spec,
                                        const std::vector<Range>& values) {
    std::vector<BuiltinArgumentShape> shapes;
    shapes.reserve(values.size());
    try {
      for (size_t k = 0; k < values.size(); ++k)
        shapes.push_back(builtin_argument_shape(e.args[k], values[k]));
      return builtin_layout(spec, shapes, e.name,
                            !in_write_array && mir::any_argument_active(e));
    } catch (const std::invalid_argument& error) {
      throw Bail{builtin_error_message(e.name, error)};
    }
  }

  // Does an assignment to `name` here run on every path that can reach a
  // read of it? It does when it sits at the declaration's own branch
  // depth, and when no loop entered since the declaration has already
  // taken a data-dependent break or continue -- either would jump from
  // ahead of the assignment to behind it, past a read that the fold has
  // already been applied to.
  bool fold_is_certain(const std::string& name) const;

  static bool is_scalar(const Range& r) {
    return r.kind == ViewKind::Flat && r.len == 1;
  }

  Range typed(Range r, const mir::UnsizedView& type);

  static ViewKind leaf_of(const std::string& base);

  static size_t leaf_rank(ViewKind leaf);

  Range declared(Range r, const mir::SizedType& type);

  static bool unsized_accepts(const mir::UnsizedView& type, const Range& r);

  // ---- expressions ---------------------------------------------------------
  Range expr(const mir::Expr& e);

  void patch_jumps(const std::vector<int>& jumps) {
    for (int jump : jumps) p.code[(size_t)jump].dst = (int)p.code.size();
  }

  static bool is_logical_not(const mir::Expr& e) {
    return e.kind == mir::Expr::FunApp && e.args.size() == 1 &&
           (e.name == "PNot__" || e.name == "logical_negation");
  }

  static bool is_zero_or_one(const mir::Expr& e);

  void logical_value(int dst, int src, const mir::Expr& operand);

  // Emits control flow for a condition. Execution falls through when the
  // condition's truth differs from `jump_when` and otherwise leaves through
  // one of the jumps appended to `targets`, which the caller patches.
  void cond_jump(const mir::Expr& e, bool jump_when, std::vector<int>* targets);

  // A ternary on a runtime condition: both arms write the same registers.
  Range branchy_select(const mir::Expr& c, const mir::Expr& a,
                       const mir::Expr& b);

  // One row of a matrix, copied into a run of its own. Column-major
  // storage puts a row's elements `rows` apart, and a strided Eigen block
  // has no packet access -- so stan-math reduces such a row in ascending
  // scalar order, which is the order every reduction this compiler emits
  // walks a run in. That is what makes the copy safe rather than merely
  // convenient: `sum` accumulates ascending, `max` compares, and the rest
  // are elementwise. A reduction with its own grouping (DOT, SOFTMAX,
  // LSE_RANGE) is not one this compiler emits, and adding one would have
  // to answer this question again.
  Range matrix_row(const Range& m, long i);

  // The ordinary UDF path and higher-order families must bind callback
  // arguments identically.  In particular, data integers retain their
  // compile-time values and data reals retain both their register and known
  // value, so nested calls automatically inherit every improvement made to
  // UDF argument handling here.
  InlineArg inline_argument(const mir::Expr& e) {
    InlineArg arg;
    long v;
    if (mir::is_unsized(e, mir::UnsizedLeaf::Int) && try_cint(e, &v)) {
      arg.is_const_int = true;
      arg.ints = {v};
    } else if (e.unsized.depth != 0 &&
               e.unsized.leaf == mir::UnsizedLeaf::Int &&
               try_cints(e, &arg.ints)) {
      arg.is_const_int = true;
      Range view;
      if (!static_view(e, &view) || view.kind != ViewKind::Array)
        bail("integer function argument has no static array view");
      arg.int_dims =
          view.dims.empty() ? std::vector<int64_t>{view.len} : view.dims;
    } else {
      if (mir::is_unsized(e, mir::UnsizedLeaf::Real))
        arg.is_const_real = try_creal(e, &arg.const_real);
      arg.real = expr(e);
      arg.autodiff = autodiff(e);
    }
    return arg;
  }

  std::vector<InlineArg> inline_arguments(const std::vector<mir::Expr>& exprs,
                                          size_t begin = 0) {
    std::vector<InlineArg> args;
    args.reserve(exprs.size() - begin);
    for (size_t i = begin; i < exprs.size(); ++i)
      args.push_back(inline_argument(exprs[i]));
    return args;
  }

  void require_positive(const Range& value, const std::string& name) {
    if (!is_scalar(value)) bail(name + " is not a scalar");
    const int ok = alloc(1);
    emit(Program::GE, ok, value.reg, konst(1.0));
    const int reject = emit(Program::JZ, 0, ok);
    const int done = emit(Program::JMP, 0);
    p.code[(size_t)reject].dst = (int)p.code.size();
    Program::Message message;
    message.spec.chunks = {name + " must be positive"};
    p.messages.push_back(std::move(message));
    emit(Program::REJECT, 0, (int)p.messages.size() - 1);
    p.code[(size_t)done].dst = (int)p.code.size();
  }

  // Serial reduce_sum is exactly one call over the complete slice, matching
  // Stan Math without STAN_THREADS.  Only family-specific argument synthesis
  // lives here; callback lookup and UDF binding are shared with every other
  // backend and ordinary inline calls respectively.
  Range reduce_sum_call(const mir::Expr& e) {
    if (e.args.size() < 3)
      bail(
          "reduce_sum: expected a partial-sum function, a sliced argument, "
          "and a grainsize");
    if (e.args[0].kind != mir::Expr::Var)
      bail("reduce_sum: the partial-sum argument is not a function name");
    if (e.unsized.depth != 0 || e.unsized.leaf != mir::UnsizedLeaf::Real)
      bail("reduce_sum: result is not a real");

    const Range slice = expr(e.args[1]);
    if (slice.kind != ViewKind::Array || slice.dims.empty())
      bail("reduce_sum: the sliced argument is not an array");
    const Range grainsize = expr(e.args[2]);
    if (e.args[2].unsized.depth != 0 ||
        e.args[2].unsized.leaf != mir::UnsizedLeaf::Int ||
        !is_scalar(grainsize))
      bail("reduce_sum: grainsize is not an integer scalar");

    // Evaluate shared arguments before the empty-slice return, as C++ does.
    std::vector<InlineArg> shared = inline_arguments(e.args, 3);
    require_positive(grainsize, "reduce_sum grainsize");
    const int64_t n = slice.dims.front();
    if (n == 0) return {konst(0.0), 1};
    if (n > std::numeric_limits<int32_t>::max())
      bail("reduce_sum: slice bound exceeds the Stan integer range");

    const bool propto = e.args[0].fn_propto;
    const std::string& base = e.args[0].name;
    const std::vector<mir::UnsizedView> views =
        mir::reduce_sum_partial_views(e);
    const mir::FunDef* f = mir::find_function(funs, e.args[0]);
    if (f == nullptr) bail("reduce_sum: unknown partial-sum function " + base);
    if (f->arg_names.size() != views.size())
      bail("reduce_sum: partial-sum arity does not match the call");

    std::vector<InlineArg> args;
    args.reserve(views.size());
    InlineArg sliced;
    sliced.real = slice;
    sliced.autodiff = autodiff(e.args[1]);
    args.push_back(std::move(sliced));
    InlineArg start;
    start.is_const_int = true;
    start.ints = {1};
    args.push_back(std::move(start));
    InlineArg end;
    end.is_const_int = true;
    end.ints = {(long)n};
    args.push_back(std::move(end));
    for (InlineArg& arg : shared) args.push_back(std::move(arg));
    // As in CmdStan, an `_lupdf` functor inherits the caller's normalization
    // and an `_lpdf` one forces the normalized density.
    return inline_call(*f, args, false, propto_ctx && propto);
  }

  // map_rect's job count and every input shape are fixed when the model is
  // lowered, so the serial implementation needs no retained runtime
  // algorithm: compile one ordinary callback invocation per job and
  // concatenate their vector results. This is also the exact execution
  // order of Stan Math's non-threaded map_rect path.
  Range map_rect_call(const mir::Expr& e) {
    if (e.args.size() != 5)
      bail(
          "map_rect: expected function, shared parameters, job parameters, "
          "real data, and integer data");
    if (e.args[0].kind != mir::Expr::Var)
      bail("map_rect: callback argument is not a function name");
    if (e.unsized.depth != 0 || e.unsized.leaf != mir::UnsizedLeaf::Vector)
      bail("map_rect: result is not a vector");

    const Range shared = expr(e.args[1]);
    const Range jobs = expr(e.args[2]);
    const Range real_data = expr(e.args[3]);
    if (shared.kind != ViewKind::Vector)
      bail("map_rect: shared parameters are not a vector");
    if (jobs.kind != ViewKind::Array || jobs.leaf != ViewKind::Vector ||
        jobs.dims.size() != 2)
      bail("map_rect: job parameters are not an array of vectors");
    if (real_data.kind != ViewKind::Array || real_data.leaf != ViewKind::Flat ||
        real_data.dims.size() != 2)
      bail("map_rect: real data are not a two-dimensional array");

    std::vector<long> ints;
    if (!try_cints(e.args[4], &ints))
      bail("map_rect: integer data are not known at compile time");
    Range int_view;
    if (!static_view(e.args[4], &int_view) ||
        int_view.kind != ViewKind::Array || e.args[4].unsized.depth != 2 ||
        e.args[4].unsized.leaf != mir::UnsizedLeaf::Int ||
        int_view.dims.empty() || int_view.dims.size() > 2)
      bail("map_rect: integer data are not a two-dimensional array");

    const int64_t n = jobs.dims[0];
    if (n != real_data.dims[0] || n != int_view.dims[0])
      bail("map_rect: job parameters and job data sizes do not match");
    const int64_t job_width = jobs.dims[1];
    const int64_t real_width = real_data.dims[1];
    // DataMap omits a trailing singleton dimension from an integer array's
    // stored shape. The MIR type retains its rank, and the value count then
    // recovers that one-element inner row unambiguously.
    const int64_t int_width = int_view.dims.size() == 2
                                  ? int_view.dims[1]
                                  : (n == 0 ? 0 : (int64_t)ints.size() / n);
    if (n < 0 || job_width < 0 || real_width < 0 || int_width < 0 ||
        n > kMaxRegs || (n && job_width > kMaxRegs / n) ||
        (n && real_width > kMaxRegs / n) || (n && int_width > kMaxRegs / n))
      bail("map_rect: input shape is invalid or too large");
    if ((int64_t)ints.size() != n * int_width)
      bail("map_rect: integer data storage and shape disagree");

    const std::vector<mir::UnsizedView> views{{0, mir::UnsizedLeaf::Vector},
                                              {0, mir::UnsizedLeaf::Vector},
                                              {1, mir::UnsizedLeaf::Real},
                                              {1, mir::UnsizedLeaf::Int}};
    const mir::FunDef* f = mir::find_function(funs, e.args[0]);
    if (f == nullptr)
      bail("map_rect: unknown callback function " + e.args[0].name);
    if (f->arg_names.size() != views.size())
      bail("map_rect: callback arity does not match the call");

    std::vector<Range> results;
    int total = 0;
    results.reserve((size_t)n);
    for (int64_t job = 0; job < n; ++job) {
      std::vector<InlineArg> args(4);
      args[0].real = shared;
      args[1].real = Range{jobs.reg + (int)(job * job_width), (int)job_width};
      args[1].real.kind = ViewKind::Vector;
      args[2].real =
          Range{real_data.reg + (int)(job * real_width), (int)real_width};
      args[2].real.kind = ViewKind::Array;
      args[2].real.dims = {real_width};
      args[2].real.leaf = ViewKind::Flat;
      args[2].autodiff = false;
      args[3].is_const_int = true;
      args[3].int_dims = {int_width};
      args[3].ints.reserve((size_t)int_width);
      // DataMap's flat integer storage has the first array dimension varying
      // fastest. A map_rect job fixes that dimension and ranges over the
      // second, so its row is strided rather than contiguous.
      for (int64_t k = 0; k < int_width; ++k)
        args[3].ints.push_back(ints[(size_t)(job + k * n)]);

      Range result = inline_call(*f, args);
      if (result.kind != ViewKind::Vector)
        bail("map_rect: callback result is not a vector");
      if (result.len > kMaxRegs - total)
        bail("map_rect: result needs too many registers");
      total += result.len;
      results.push_back(result);
    }

    const int out_reg = alloc(total);
    int at = 0;
    for (const Range& result : results)
      for (int k = 0; k < result.len; ++k)
        emit(Program::MOV, out_reg + at++, result.reg + k);
    Range out{out_reg, total};
    out.kind = ViewKind::Vector;
    return out;
  }

  // One adapter from register ranges to the graph kernel ABI. Regular
  // builtins, RNGs, and retained higher-order algorithms all use the same
  // binding, scratch sizing, ownership, and reverse-mode contract.
  bool has_runtime_selector(const std::vector<mir::Expr>& selectors);

  // Fixed shapes with runtime selectors use the same checked gather/scatter
  // kernels as structured loops. Only selector values vary; result extents
  // must be proven here before allocating any result registers.
  Range dynamic_index(const Range& base,
                      const std::vector<mir::Expr>& selectors,
                      const Range* rhs = nullptr);

  // A registered matrix operation, `map_out` receiving its shapes.
  Range matrix_op_call(const mir::Expr& e, const BuiltinSpec* matrix,
                       BuiltinMatrixMap* map_out = nullptr) {
    std::vector<Range> args;
    args.reserve(matrix->arity);
    for (const mir::Expr& argument : e.args) args.push_back(expr(argument));
    BuiltinMatrixMap map;
    try {
      std::vector<BuiltinArgumentShape> shapes;
      shapes.reserve(args.size());
      for (size_t k = 0; k < args.size(); ++k)
        shapes.push_back(builtin_argument_shape(e.args[k], args[k]));
      map = builtin_matrix_map(*matrix, shapes);
    } catch (const std::invalid_argument& error) {
      bail(e.name + ": " + std::string(error.what()));
    }
    bool active = false;
    for (const mir::Expr& argument : e.args)
      if (!argument.data_only && !in_write_array) active = true;
    Range out{0, (int)map.result.storage_size};
    out = shaped(std::move(out), map.result);
    std::vector<int> idata;
    idata.reserve(map.idata.size());
    for (const int64_t value : map.idata) idata.push_back((int)value);
    if (map_out) *map_out = map;
    return kernel_call(
        matrix->opcode, args, out,
        (uint8_t)(map.variant | (active ? map.active_variant : 0u)),
        matrix->activity_mask, std::move(idata), {}, e.name);
  }

  Range kernel_call(uint16_t opcode, const std::vector<Range>& args, Range out,
                    uint8_t variant = 0, uint8_t input_adjoint_mask = 0x3f,
                    std::vector<int> idata = {},
                    std::shared_ptr<void> udata = {},
                    const std::string& name = "function") {
    if (args.size() > 6) bail(name + ": too many kernel arguments");
    out.reg = alloc(out.len);
    Program::Call call;
    call.opcode = opcode;
    call.variant = variant;
    call.input_adjoint_mask = input_adjoint_mask;
    call.n_in = (int8_t)args.size();
    for (size_t k = 0; k < args.size(); ++k) {
      call.in[k] = args[k].reg;
      call.in_len[k] = args[k].len;
    }
    call.out = out.reg;
    call.out_len = out.len;
    call.idata = std::move(idata);
    call.udata_owner = std::move(udata);

    const Kernel* kernel = find_kernel(opcode);
    if (kernel == nullptr) bail(name + ": graph kernel is unavailable");
    const int64_t scratch = kernel_call_scratch(
        kernel->scratch_size, opcode, variant, call.n_in, call.in_len, out.len,
        call.idata.data(), (int64_t)call.idata.size(), call.udata_owner.get());
    if (scratch < 0 || scratch > kMaxRegs)
      bail(name + ": kernel needs excessive scratch storage");
    call.scratch_len = (int32_t)scratch;
    call.scratch = scratch ? alloc((int)scratch) : 0;
    if (!bind_call(call)) bail(name + ": graph kernel is unavailable");
    p.calls.push_back(std::move(call));
    p.code.push_back(Program::Instr{Program::CALL, 0, (int)p.calls.size() - 1});
    return out;
  }

  // Every RNG spelling the region can carry, which is exactly the set the
  // graph's OP_RNG kernel speaks.
  static bool rng_call_name(const std::string& name) {
    return scalar_rng_family(name) != nullptr || name == "categorical_rng" ||
           name == "categorical_logit_rng" || name == "poisson_binomial_rng" ||
           name == "multi_normal_cholesky_rng" || name == "multi_normal_rng" ||
           name == "dirichlet_rng";
  }

  // A draw inside a runtime-control region, spelled as one Program::CALL on
  // the graph's own OP_RNG kernel rather than transcribed family by family.
  // The stream, the stan-math call and the argument contract are then the
  // kernel's, so the region and the graph cannot disagree about what a draw
  // is or where in the stream it lands. Generated quantities never runs a
  // gradient, and OP_RNG consequently needs no backward implementation.
  Range rng_call(const mir::Expr& e) {
    if (!in_write_array)
      bail(e.name + " is supported only in generated quantities");
    std::vector<Range> args;
    args.reserve(e.args.size());
    for (const mir::Expr& a : e.args) args.push_back(expr(a));

    uint8_t variant = 0;
    int out_len = 1;
    ViewKind out_kind = ViewKind::Flat;
    std::vector<int> idata;
    if (const ScalarRng* family = scalar_rng_family(e.name)) {
      // Stan ints fit exactly in double registers. Draws remain effectful
      // runtime values: cint cannot fold them, shape demands still refuse,
      // and dynamic indexing uses the existing checked register operations.
      const auto leaf = scalar_rng_is_int(*family) ? mir::UnsizedLeaf::Int
                                                   : mir::UnsizedLeaf::Real;
      const bool promoted_int = scalar_rng_is_int(*family) && e.promoted &&
                                e.unsized.leaf == mir::UnsizedLeaf::Real;
      if (e.unsized.depth > 1 || (e.unsized.leaf != leaf && !promoted_int))
        bail(e.name + ": result type does not match scalar RNG family");
      if (args.size() != scalar_rng_arity(*family))
        bail(e.name + ": wrong number of arguments");
      if ((*family == ScalarRng::Binomial ||
           *family == ScalarRng::BetaBinomial) &&
          e.args[0].unsized.leaf != mir::UnsizedLeaf::Int)
        bail(e.name + ": first argument must be int");
      int container_mask = 0;
      for (size_t k = 0; k < args.size(); ++k) {
        const auto& type = e.args[k].unsized;
        const bool scalar =
            type.depth == 0 && (type.leaf == mir::UnsizedLeaf::Real ||
                                type.leaf == mir::UnsizedLeaf::Int);
        const bool array =
            type.depth == 1 && (type.leaf == mir::UnsizedLeaf::Real ||
                                type.leaf == mir::UnsizedLeaf::Int);
        if (scalar) {
          if (!is_scalar(args[k])) bail(e.name + ": expected scalar storage");
        } else {
          if (!array &&
              !(type.depth == 0 && (type.leaf == mir::UnsizedLeaf::Vector ||
                                    type.leaf == mir::UnsizedLeaf::RowVector)))
            bail(e.name + ": expected scalar or one-dimensional arguments");
          if (container_mask != 0 && args[k].len != out_len)
            bail(e.name + ": argument lengths disagree");
          container_mask |= 1 << k;
          out_len = args[k].len;
        }
      }
      if ((e.unsized.depth == 1) != (container_mask != 0))
        bail(e.name + ": result shape does not match RNG arguments");
      if (container_mask != 0) {
        idata.push_back(container_mask);
        out_kind = ViewKind::Array;
      }
      variant = static_cast<uint8_t>(*family);
    } else if (e.name == "categorical_rng" ||
               e.name == "categorical_logit_rng" ||
               e.name == "poisson_binomial_rng") {
      const bool promoted_int =
          e.promoted && e.unsized.leaf == mir::UnsizedLeaf::Real;
      if (e.unsized.depth != 0 ||
          (e.unsized.leaf != mir::UnsizedLeaf::Int && !promoted_int) ||
          args.size() != 1 || args[0].kind != ViewKind::Vector)
        bail(e.name + ": expected a vector argument and scalar integer result");
      variant = e.name == "categorical_logit_rng"  ? kCategoricalLogitRngVariant
                : e.name == "poisson_binomial_rng" ? kPoissonBinomialRngVariant
                                                   : kCategoricalRngVariant;
    } else if (e.name == "dirichlet_rng") {
      if (args.size() != 1 || args[0].kind != ViewKind::Vector ||
          args[0].len <= 0)
        bail("dirichlet_rng: expected one concentration vector");
      variant = kDirichletRngVariant;
      out_len = args[0].len;
      out_kind = ViewKind::Vector;
    } else {
      if (args.size() != 2 || args[0].kind != ViewKind::Vector ||
          args[1].kind != ViewKind::Matrix)
        bail(
            "multi_normal_rng: expected a location vector and a covariance "
            "matrix");
      if (args[1].rows != args[0].len || args[1].cols != args[0].len)
        bail("multi_normal_rng: covariance shape must match the location");
      variant = e.name == "multi_normal_cholesky_rng"
                    ? kMultiNormalCholeskyRngVariant
                    : kMultiNormalRngVariant;
      out_len = args[0].len;
      out_kind = ViewKind::Vector;
      idata.push_back(out_len);
    }

    Range out{0, out_len};
    out.kind = out_kind;
    if (out_kind == ViewKind::Array) out.dims = {out_len};
    return kernel_call(OP_RNG, args, out, variant, 0, std::move(idata), {},
                       e.name);
  }

  // Program-native instructions cover the hot elementary subset. Everything
  // else in the shared regular-function registry reaches the exact same graph
  // kernel through CALL, so adding an optable entry also makes it available in
  // parameter-dependent control flow without another name table here.
  Range builtin_kernel_call(const mir::Expr& e, const BuiltinSpec& spec) {
    const size_t arity = spec.arity;
    if (e.args.size() != arity) bail(e.name + ": wrong number of arguments");
    std::vector<Range> args;
    args.reserve(arity);
    for (const mir::Expr& arg : e.args) args.push_back(expr(arg));
    const BuiltinLayout layout = resolved_builtin_layout(e, spec, args);
    Range out = args[layout.result_argument];
    if (spec.shape == BuiltinShapePolicy::Ordering) {
      out.kind = ViewKind::Flat;
      out.rows = out.cols = 0;
      out.dims.clear();
      out.leaf = ViewKind::Flat;
      if (spec.ordering == BuiltinOrdering::Rank) {
        out.len = 1;
      } else {
        out.kind = ViewKind::Array;
        out.dims = {out.len};
      }
    }
    out = typed(out, e.unsized);
    std::vector<int> idata;
    if (layout.groups >= 0)
      idata = {static_cast<int>(layout.groups),
               static_cast<int>(layout.group_width), 0};
    if (layout.integer_matrix_rows != 0)
      idata = {(int)layout.integer_matrix_rows,
               (int)layout.integer_matrix_cols};
    uint8_t activity = spec.activity_mask;
    for (size_t k = 0; k < arity; ++k) {
      // Runtime integers and data-only values still execute normally, but
      // have no derivative. A promotion alone is not proof of inactivity.
      if (e.args[k].data_only || int_operand(e.args[k]))
        activity &= (uint8_t)~(1u << k);
    }
    const uint8_t variant =
        spec.opcode == OP_ORDERING ? static_cast<uint8_t>(spec.ordering) : 0;
    if (spec.opcode == OP_ORDERING) activity = 0;
    return kernel_call(spec.opcode, args, out, variant, activity,
                       std::move(idata), {}, e.name);
  }

  static Program::Code integer_instruction(Program::Code code) {
    switch (code) {
      case Program::ADD:
        return Program::IADD;
      case Program::SUB:
        return Program::ISUB;
      case Program::MUL:
        return Program::IMUL;
      case Program::DIV:
        return Program::IDIV;
      case Program::NEG:
        return Program::INEG;
      case Program::FABS:
        return Program::IABS;
      default:
        return code;
    }
  }

  static Program::Code native_math_code(uint16_t opcode) {
    switch (opcode) {
      case OP_ERFC:
        return Program::ERFC;
      case OP_LOG1P:
        return Program::LOG1P;
      case OP_LOG1M_EXP:
        return Program::LOG1M_EXP;
      default:
        return Program::INV_SQUARE;
    }
  }

  static std::optional<Program::Code> native_builtin_code(uint16_t opcode) {
    switch (opcode) {
      case OP_ERFC:
      case OP_LOG1P:
      case OP_LOG1M_EXP:
      case OP_INV_SQUARE:
        if (debug_env("STANLI_NO_NATIVE_MATH")) return std::nullopt;
        return native_math_code(opcode);
      case OP_ADD:
        return Program::ADD;
      case OP_SUB:
        return Program::SUB;
      case OP_MUL:
        return Program::MUL;
      case OP_DIV:
        return Program::DIV;
      case OP_POW:
        return Program::POW;
      case OP_FMAX:
        return Program::FMAX;
      case OP_FMIN:
        return Program::FMIN;
      case OP_LSE2:
        return Program::LSE2;
      case OP_LOG_DIFF_EXP:
        return Program::LOG_DIFF_EXP;
      case OP_LMULTIPLY:
        return Program::LMULTIPLY;
      case OP_NEG:
        return Program::NEG;
      case OP_EXPV:
        return Program::EXP;
      case OP_LOGV:
        return Program::LOG;
      case OP_SQRT:
        return Program::SQRT;
      case OP_SQUARE:
        return Program::SQUARE;
      case OP_INV:
        return Program::INV;
      case OP_ABS:
        return Program::FABS;
      case OP_INV_LOGIT:
        return Program::INV_LOGIT;
      case OP_LOG1P_EXP:
        return Program::LOG1P_EXP;
      default:
        return std::nullopt;
    }
  }

  Range native_builtin_call(const mir::Expr& e, const BuiltinSpec& spec,
                            Program::Code native_code) {
    std::vector<Range> args;
    args.reserve(spec.arity);
    for (const mir::Expr& argument : e.args) args.push_back(expr(argument));
    if (args.empty() || args.size() > 2)
      bail(e.name + ": unsupported native builtin arity");
    const BuiltinLayout layout = resolved_builtin_layout(e, spec, args);
    Range out = args[layout.result_argument];
    const int n = (int)layout.lanes;

    Program::Code code = native_code;
    if (spec.result == FunctionArgumentKind::Integer)
      code = integer_instruction(code);
    // fmax/fmin ties and NaN adjoints depend on which operands are data
    // (adjoint.cpp, program_extremum), the same way pow's zero-base law
    // depends on the exponent's.
    const int law =
        code == Program::POW
            ? mir::pow_zero_base_law(e.args[0], e.args[1], !e.args[1].data_only)
        : code == Program::FMAX || code == Program::FMIN
            ? (e.args[0].data_only ? 0 : 0x1) | (e.args[1].data_only ? 0 : 0x2)
            : 0;
    const int result = alloc(n);
    for (int i = 0; i < n; ++i) {
      const int a = args[0].reg + (is_scalar(args[0]) ? 0 : i);
      const int b =
          args.size() == 2 ? args[1].reg + (is_scalar(args[1]) ? 0 : i) : 0;
      emit(code, result + i, a, b, 0, law);
    }
    out.reg = result;
    out.len = n;
    return typed(out, e.unsized);
  }

  // Probability functions use their shared registry policy. Graph-backed
  // descriptors marshal register ranges and integer payloads into KernelCtx;
  // all-integer descriptors evaluate once as constants, with the same Stan
  // Math implementation used by graph lowering and MIR interpretation.
  Range density_call(const mir::Expr& e, const DensitySpec& spec) {
    if (spec.opcode == OP_MULTINOMIAL_LPMF)
      if (const mir::Expr* logits = mir::multinomial_softmax_logits(e)) {
        mir::Expr call = e;
        call.args[1] = *logits;
        return density_call(call, multinomial_softmax_density());
      }
    if ((int)e.args.size() != spec.arity)
      bail(e.name + ": wrong number of arguments");

    const auto integer_values = [&](const mir::Expr& arg) {
      const std::vector<long> source =
          mir::is_unsized(arg, mir::UnsizedLeaf::Int)
              ? std::vector<long>{cint(arg)}
              : cints(arg);
      std::vector<int> result;
      result.reserve(source.size());
      for (long value : source) {
        if (value < std::numeric_limits<int>::min() ||
            value > std::numeric_limits<int>::max())
          bail(e.name + ": integer argument is out of range");
        result.push_back((int)value);
      }
      return result;
    };
    std::vector<Range> args;
    args.reserve(e.args.size() - (size_t)spec.integer_args);
    std::vector<DensityCallArgument> plan_arguments;
    plan_arguments.reserve(e.args.size());
    try {
      for (size_t k = 0; k < e.args.size(); ++k) {
        const mir::Expr& source = e.args[k];
        DensityCallArgument argument;
        if (spec.evaluation == DensityEvaluationPolicy::AllInteger ||
            k < static_cast<size_t>(spec.integer_args)) {
          argument = integer_density_argument(integer_values(source),
                                              source.unsized.depth == 0,
                                              source.data_only);
        } else {
          argument.scalar = source.unsized.depth == 0;
          argument.data_only = source.data_only;
          argument.active = autodiff(source);
          args.push_back(expr(source));
          argument.shape = builtin_argument_shape(source, args.back());
        }
        plan_arguments.push_back(std::move(argument));
      }
      const DensityCallPlan plan =
          density_call_plan(spec, plan_arguments, e.fn_propto && propto_ctx);
      if (plan.empty_result) return {konst(0.0), 1};
      if (plan.data_term) args.push_back(Range{konst(*plan.data_term), 1});
      Range out{0, 1};
      return kernel_call(spec.opcode, args, out, plan.variant,
                         plan.activity_mask, plan.idata, {}, e.name);
    } catch (const DensitySizeError& error) {
      // Stan rejects the call when it runs, so the program rejects there too.
      Program::Message message;
      message.spec.chunks = {e.name + ": " + error.what()};
      message.spec.error = MessageError::InvalidArgument;
      p.messages.push_back(std::move(message));
      emit(Program::REJECT, 0, (int)p.messages.size() - 1);
      return {konst(0.0), 1};
    } catch (const std::exception& error) {
      bail(builtin_error_message(e.name, error));
    }

    return {};
  }

  Range matrix_gram(const Range& m, bool transpose_first) {
    if (m.kind != ViewKind::Matrix)
      bail(std::string(transpose_first ? "crossprod" : "tcrossprod") +
           " requires a matrix");
    const int64_t outer = transpose_first ? m.cols : m.rows;
    const int64_t inner = transpose_first ? m.rows : m.cols;
    if (outer != 0 && outer > kMaxRegs / outer)
      bail("matrix Gram product needs too many registers");
    const int r = alloc((int)(outer * outer));
    const auto at = [&](int64_t row, int64_t col) {
      return m.reg + (int)(col * m.rows + row);
    };
    for (int64_t j = 0; j < outer; ++j)
      for (int64_t i = 0; i < outer; ++i) {
        const int dst = r + (int)(j * outer + i);
        if (inner == 0) {
          const double zero = 0.0;
          emit_const(dst, &zero, 1);
          continue;
        }
        const auto lhs = [&](int64_t k) {
          return transpose_first ? at(k, i) : at(i, k);
        };
        const auto rhs = [&](int64_t k) {
          return transpose_first ? at(k, j) : at(j, k);
        };
        emit(Program::MUL, dst, lhs(0), rhs(0));
        for (int64_t k = 1; k < inner; ++k) {
          const int term = alloc(1);
          emit(Program::MUL, term, lhs(k), rhs(k));
          emit(Program::ADD, dst, dst, term);
        }
      }
    Range out{r, (int)(outer * outer)};
    out.kind = ViewKind::Matrix;
    out.rows = out.cols = outer;
    return out;
  }

  Range inverse_transform_call(const mir::Expr& e) {
    CallableTransformSpec spec;
    callable_transform(e.name, &spec);
    Program::Transform tr;
    tr.kind = spec.kind;
    tr.direction = spec.direction;
    tr.n_in = 1;
    const Range arg = expr(e.args[0]);
    tr.in[0] = arg.reg;
    tr.in_len[0] = arg.len;
    ViewKind leaf = arg.kind;
    std::vector<int64_t> dims;
    if (leaf == ViewKind::Array) {
      dims = arg.dims;
      leaf = arg.leaf;
    } else if (leaf == ViewKind::Matrix) {
      dims = {arg.rows, arg.cols};
    } else if (leaf == ViewKind::Vector || leaf == ViewKind::RowVector) {
      dims = {arg.len};
    }
    if (leaf != ViewKind::Vector && leaf != ViewKind::Matrix)
      bail(e.name + ": expected a vector or matrix");
    const size_t outer_rank = dims.size() - leaf_rank(leaf);
    int64_t batch = 1;
    for (size_t i = 0; i < outer_rank; ++i) batch *= dims[i];
    tr.in_rows = (int32_t)dims[outer_rank];
    tr.in_cols = leaf == ViewKind::Matrix ? (int32_t)dims.back() : 0;
    int64_t rows = 0, cols = 0;
    bool out_matrix = false;
    if (!free_leaf_shape(spec.kind, tr.in_rows, tr.in_cols, &rows, &cols,
                         &out_matrix) ||
        rows < 0 || cols < 0)
      bail(e.name + ": invalid input container");
    const int64_t inner_free = out_matrix ? rows * cols : rows;
    if (batch > kMaxRegs || (batch && inner_free > kMaxRegs / batch))
      bail(e.name + ": result needs too many registers");
    tr.batch = (int32_t)batch;
    tr.inner_raw = (int32_t)(tr.in_cols ? tr.in_rows * tr.in_cols : tr.in_rows);
    tr.out_rows = (int32_t)rows;
    tr.out_cols = (int32_t)cols;
    tr.out_len = (int32_t)(batch * inner_free);
    Range out = arg;
    out.len = tr.out_len;
    out.kind = outer_rank ? ViewKind::Array
                          : (out_matrix ? ViewKind::Matrix : ViewKind::Vector);
    out.rows = out_matrix && !outer_rank ? rows : 0;
    out.cols = out_matrix && !outer_rank ? cols : 0;
    if (outer_rank) {
      out.dims.assign(dims.begin(), dims.begin() + outer_rank);
      out.dims.push_back(rows);
      if (out_matrix) out.dims.push_back(cols);
      out.leaf = out_matrix ? ViewKind::Matrix : ViewKind::Vector;
    }
    tr.out = alloc(tr.out_len);
    tr.jac = alloc(1);
    out.reg = tr.out;
    p.transforms.push_back(tr);
    p.code.push_back(
        Program::Instr{Program::TRANSFORM, 0, (int)p.transforms.size() - 1});
    return out;
  }

  Range transform_call(const mir::Expr& e, const CallableTransformSpec& spec) {
    if (e.args.size() != spec.arity)
      bail(e.name + ": wrong number of arguments");
    if (spec.structured && spec.direction == TransformDirection::Unconstrain)
      return inverse_transform_call(e);

    Program::Transform tr;
    tr.kind = spec.kind;
    tr.direction = spec.direction;
    tr.n_in = spec.structured ? 1 : (int8_t)spec.arity;
    std::vector<Range> args;
    args.reserve((size_t)tr.n_in);
    for (int k = 0; k < tr.n_in; ++k) {
      args.push_back(expr(e.args[(size_t)k]));
      tr.in[k] = args.back().reg;
      tr.in_len[k] = args.back().len;
    }
    Range out = args[0];
    if (!spec.structured) {
      for (int k = 1; k < tr.n_in; ++k)
        if (args[k].len != 1 && args[k].len != args[0].len)
          bail(e.name + ": bound is neither scalar nor the input size");
      tr.out_len = args[0].len;
      tr.inner_raw = args[0].len;
      tr.eigen = args[0].kind == ViewKind::Vector      ? 1
                 : args[0].kind == ViewKind::RowVector ? 2
                 : args[0].kind == ViewKind::Matrix    ? 3
                                                       : 0;
      tr.rows = static_cast<int32_t>(args[0].rows);
    } else {
      ViewKind leaf = args[0].kind;
      std::vector<int64_t> dims;
      if (leaf == ViewKind::Array) {
        dims = args[0].dims;
        leaf = args[0].leaf;
      } else if (leaf == ViewKind::Matrix) {
        dims = {args[0].rows, args[0].cols};
      } else if (leaf == ViewKind::Vector || leaf == ViewKind::RowVector) {
        dims = {args[0].len};
      }
      const size_t rank = leaf_rank(leaf);
      if (rank == 0 || dims.size() < rank)
        bail(e.name + ": invalid input container");
      const size_t outer_rank = dims.size() - rank;
      int64_t batch = 1;
      for (size_t i = 0; i < outer_rank; ++i) batch *= dims[i];
      int64_t raw_rows = leaf == ViewKind::Matrix ? dims[dims.size() - 2] : 0;
      int64_t raw_cols = leaf == ViewKind::Matrix ? dims.back() : 0;
      int64_t rows = 0, cols = 0;
      ViewKind out_leaf = leaf;
      switch (spec.kind) {
        case CallableTransformKind::Ordered:
        case CallableTransformKind::PositiveOrdered:
          if (leaf != ViewKind::Vector) bail(e.name + ": expected vector");
          rows = dims.back();
          break;
        case CallableTransformKind::Simplex:
          if (leaf != ViewKind::Vector) bail(e.name + ": expected vector");
          rows = dims.back() + 1;
          break;
        case CallableTransformKind::UnitVector:
          if (leaf != ViewKind::Vector) bail(e.name + ": expected vector");
          rows = dims.back();
          break;
        case CallableTransformKind::SumToZero:
          if (leaf == ViewKind::Vector) {
            rows = dims.back() + 1;
          } else if (leaf == ViewKind::Matrix) {
            rows = raw_rows + 1;
            cols = raw_cols + 1;
          } else {
            bail(e.name + ": expected vector or matrix");
          }
          break;
        case CallableTransformKind::StochasticColumn:
        case CallableTransformKind::StochasticRow:
          if (leaf != ViewKind::Matrix) bail(e.name + ": expected matrix");
          rows =
              raw_rows + (spec.kind == CallableTransformKind::StochasticColumn);
          cols = raw_cols + (spec.kind == CallableTransformKind::StochasticRow);
          break;
        case CallableTransformKind::CholeskyFactorCorr:
        case CallableTransformKind::CorrMatrix:
        case CallableTransformKind::CovMatrix:
          if (leaf != ViewKind::Vector) bail(e.name + ": expected vector");
          out_leaf = ViewKind::Matrix;
          rows = cols = cint(e.args[1]);
          break;
        case CallableTransformKind::CholeskyFactorCov:
          if (leaf != ViewKind::Vector) bail(e.name + ": expected vector");
          out_leaf = ViewKind::Matrix;
          rows = cint(e.args[1]);
          cols = cint(e.args[2]);
          break;
        default:
          bail(e.name + ": invalid structured transform");
      }
      if (batch < 0 || rows < 0 || cols < 0) bail(e.name + ": invalid shape");
      tr.batch = (int32_t)batch;
      tr.inner_raw = leaf == ViewKind::Matrix ? (int32_t)(raw_rows * raw_cols)
                                              : (int32_t)dims.back();
      tr.out_rows = (int32_t)rows;
      tr.out_cols = (int32_t)cols;
      const int64_t inner_con =
          out_leaf == ViewKind::Matrix ? rows * cols : rows;
      if (inner_con < 0 || batch > kMaxRegs ||
          (batch && inner_con > kMaxRegs / batch))
        bail(e.name + ": result needs too many registers");
      tr.out_len = (int32_t)(batch * inner_con);
      out.len = tr.out_len;
      out.kind = outer_rank ? ViewKind::Array : out_leaf;
      out.rows = out.kind == ViewKind::Matrix ? rows : 0;
      out.cols = out.kind == ViewKind::Matrix ? cols : 0;
      if (outer_rank) {
        out.dims.assign(dims.begin(), dims.begin() + outer_rank);
        out.dims.push_back(rows);
        if (out_leaf == ViewKind::Matrix) out.dims.push_back(cols);
        out.leaf = out_leaf;
      }
    }
    tr.out = alloc(tr.out_len);
    tr.jac = alloc(1);
    out.reg = tr.out;
    p.transforms.push_back(tr);
    p.code.push_back(
        Program::Instr{Program::TRANSFORM, 0, (int)p.transforms.size() - 1});
    if (spec.direction == TransformDirection::Jacobian && !in_write_array) {
      if (target_reg < 0) bail("jacobian transform has no target");
      emit(Program::ADD, target_reg, target_reg, tr.jac);
    }
    return out;
  }

  Range fun(const mir::Expr& e) {
    // A scalar reduction does not need a variable-width register result.
    // Keep the source's fixed capacity and check both bounds when executed.
    // Other dynamic slices retain their existing refusal; no array padding
    // or unused tail values participate in the reduction or its derivative.
    if (e.fn_lib == mir::Expr::Lib::StanLib && e.name == "log_sum_exp" &&
        e.args.size() == 1 && e.args[0].kind == mir::Expr::Indexed) {
      const auto& slice = e.args[0];
      if (slice.args.size() == 2 &&
          slice.args[1].kind == mir::Expr::IndexBetween &&
          slice.args[1].args.size() == 2 &&
          (mir::is_unsized(slice.args[0], mir::UnsizedLeaf::Vector) ||
           mir::is_unsized(slice.args[0], mir::UnsizedLeaf::RowVector))) {
        long lo, hi;
        if (!try_cint(slice.args[1].args[0], &lo) ||
            !try_cint(slice.args[1].args[1], &hi)) {
          const Range base = expr(slice.args[0]);
          const Range lower = expr(slice.args[1].args[0]);
          const Range upper = expr(slice.args[1].args[1]);
          if (!is_scalar(lower) || !is_scalar(upper))
            bail("non-scalar slice bound");
          const int r = alloc(1);
          emit(Program::DYN_LSE_RANGE, r, base.reg, lower.reg, upper.reg,
               base.len);
          return {r, 1};
        }
      }
    }
    if (const auto intrinsic = mir::stateful_intrinsic_kind(e)) {
      switch (*intrinsic) {
        case mir::StatefulIntrinsicKind::Target: {
          if (target_base_reg < 0) {
            Range base;
            if (!bind_target || !bind_target(&base) || !is_scalar(base))
              bail("target() is unavailable in this context");
            target_base_reg = base.reg;
          }
          if (target_reg < 0) return {target_base_reg, 1};
          const int current = alloc(1);
          emit(Program::ADD, current, target_base_reg, target_reg);
          return {current, 1};
        }
      }
    }
    if (const auto value = mir::nullary_constant(e)) return {konst(*value), 1};
    if (const auto higher_order = mir::higher_order_call(e)) {
      switch (higher_order->family) {
        case mir::HigherOrderFamily::ReduceSum:
          return reduce_sum_call(e);
        case mir::HigherOrderFamily::MapRect:
          return map_rect_call(e);
        default:
          Range result;
          if (lower_higher_order && lower_higher_order(e, &result))
            return result;
          break;
      }
    }
    // A shape query is a constant whatever surrounds it. Ahead of every
    // other case because `FnLength` is an internal function and the rest
    // are library ones, and they are all answered the same way: from the
    // named value's view where there is one, and otherwise from the view
    // of the value the argument builds.
    if (is_shape_query(e)) {
      Range v;
      if (!static_view(e.args[0], &v)) v = expr(e.args[0]);
      return {konst((double)shape_query(e, v)), 1};
    }
    // dims is the container-answer shape query: every extent, as constants.
    if (const BuiltinSpec* query =
            e.args.size() == 1
                ? shaped_builtin_spec(e.name, 1, BuiltinShapePolicy::ShapeQuery)
                : nullptr) {
      Range v;
      if (!static_view(e.args[0], &v)) v = expr(e.args[0]);
      if (v.kind == ViewKind::Array && v.dims.empty()) v.dims = {v.len};
      const std::vector<int64_t> extents =
          builtin_shape_query(*query, builtin_argument_shape(e.args[0], v));
      const std::vector<double> values(extents.begin(), extents.end());
      const int r = alloc((int)values.size());
      if (!values.empty()) emit_const(r, values.data(), (int)values.size());
      Range out{r, (int)values.size()};
      out.kind = ViewKind::Array;
      out.dims = {(int64_t)values.size()};
      return out;
    }
    if (e.data_only && e.unsized.depth != 0 &&
        e.unsized.leaf == mir::UnsizedLeaf::Int) {
      std::vector<long> values;
      std::vector<int64_t> dims;
      if (external_int_array(e, &values, &dims)) {
        if (values.size() > (size_t)kMaxRegs)
          bail("integer array constant needs too many registers");
        const std::vector<int64_t> logical_dims =
            dims.empty() ? std::vector<int64_t>{(int64_t)values.size()} : dims;
        const std::vector<double> real_values =
            int_array_graph_values(values, logical_dims);
        const int r = alloc((int)values.size());
        emit_const(r, real_values.data(), (int)real_values.size());
        Range out{r, (int)values.size()};
        out.kind = ViewKind::Array;
        out.dims = logical_dims;
        return out;
      }
    }
    if (rng_call_name(e.name)) return rng_call(e);
    CallableTransformSpec transform;
    if (callable_transform(e.name, &transform))
      return transform_call(e, transform);
    const FunctionSpec* registered = function_spec(e);
    const BuiltinSpec* builtin =
        registered != nullptr && registered->builtin() != nullptr
            ? registered->builtin()
            : nullptr;
    // Registered constructors fold to one constant register range through
    // the shared Stan Math evaluator; every argument is a data scalar, so
    // extents, spacing rules, and domain errors are CmdStan's own.
    if (const BuiltinSpec* ctor = shaped_builtin_spec(
            e.name, e.args.size(), BuiltinShapePolicy::Constructor)) {
      std::vector<double> ctor_args;
      ctor_args.reserve(e.args.size());
      for (size_t k = 0; k < e.args.size(); ++k)
        ctor_args.push_back(ctor->arguments[k] == BuiltinArgumentKind::Integer
                                ? (double)cint(e.args[k])
                                : creal(e.args[k]));
      ConstructorValue built;
      try {
        built = evaluate_constructor_builtin(*ctor, ctor_args);
      } catch (const std::domain_error&) {
        // Stan Math's own validation, the rejection CmdStan throws.
        throw;
      } catch (const std::invalid_argument& error) {
        bail(e.name + ": " + std::string(error.what()));
      }
      const int len = (int)built.values.size();
      const int r = alloc(len);
      if (len != 0) emit_const(r, built.values.data(), len);
      Range out{r, len};
      switch (ctor->constructor_container) {
        case FunctionContainerKind::Vector:
          out.kind = ViewKind::Vector;
          break;
        case FunctionContainerKind::RowVector:
          out.kind = ViewKind::RowVector;
          break;
        case FunctionContainerKind::Matrix:
          out.kind = ViewKind::Matrix;
          out.rows = built.dimensions[0];
          out.cols = built.dimensions[1];
          break;
        default:
          out.kind = ViewKind::Array;
          out.leaf = ViewKind::Flat;
          out.dims = built.dimensions;
          break;
      }
      return out;
    }

    // Registered slice/view selections: the shared resolver maps result
    // cells to source registers over the register file's outer-major array
    // storage (Eigen leaves are column-major under both conventions), with
    // Stan Math's own index checks (out_of_range and domain_error propagate
    // as CmdStan's rejections). Every selection is a per-lane MOV, so later
    // writes to a source variable cannot alias the result and adjoints
    // accumulate through MOV's reverse pass.
    if (const BuiltinSpec* slice = shaped_builtin_spec(
            e.name, e.args.size(), BuiltinShapePolicy::SliceView)) {
      const Range a = expr(e.args[0]);
      Range b{};
      BuiltinSliceMap map;
      try {
        if (builtin_slice_is_append(slice->slice)) {
          // Appends map result cells over both operands' concatenated
          // storage; a source cell at or past the left run reads the right.
          b = expr(e.args[1]);
          map = builtin_append_map(*slice, builtin_argument_shape(e.args[0], a),
                                   builtin_argument_shape(e.args[1], b),
                                   SliceStorageOrder::OuterMajor);
        } else {
          std::vector<int64_t> indexes;
          indexes.reserve(e.args.size() - 1);
          for (size_t k = 1; k < e.args.size(); ++k)
            indexes.push_back(cint(e.args[k]));
          map = builtin_slice_map(*slice, builtin_argument_shape(e.args[0], a),
                                  indexes, SliceStorageOrder::OuterMajor);
        }
      } catch (const std::invalid_argument& error) {
        bail(e.name + ": " + std::string(error.what()));
      }
      const auto stamp = [&](Range out) {
        return shaped(std::move(out), map.result);
      };
      // A reshape's identity map relabels the source run outright -- the
      // zero-instruction lowering the named vector transpose always used;
      // persistence still copies at the assignment site.
      if (builtin_slice_is_reshape(slice->slice) &&
          map.kind == BuiltinSliceMap::Kind::Contiguous)
        return stamp(a);
      const int r = alloc((int)map.count);
      const auto source_cell = [&](int64_t k) {
        switch (map.kind) {
          case BuiltinSliceMap::Kind::Contiguous:
            return map.offset + k;
          case BuiltinSliceMap::Kind::Strided:
            return map.offset + k * map.stride;
          case BuiltinSliceMap::Kind::Transpose: {
            const int64_t rows = map.result.dimensions[0];
            const int64_t cols = map.result.dimensions[1];
            return k / rows + cols * (k % rows);
          }
          case BuiltinSliceMap::Kind::Gather:
            break;
        }
        return map.gather[(size_t)k];
      };
      for (int64_t k = 0; k < map.count; ++k) {
        const int64_t cell = source_cell(k);
        const int source =
            cell < a.len ? a.reg + (int)cell : b.reg + (int)(cell - a.len);
        emit(Program::MOV, r + (int)k, source);
      }
      return stamp(Range{r, (int)map.count});
    }

    // Registered paired reductions: the dot kernel over two equal-length
    // ranges (or one, paired with itself); squared_distance subtracts first
    // with the same native SUB lanes the graph's OP_SUB computes.
    if (const BuiltinSpec* paired = shaped_builtin_spec(
            e.name, e.args.size(), BuiltinShapePolicy::PairedReduction)) {
      Range a = expr(e.args[0]);
      Range b = paired->arity == 2 ? expr(e.args[1]) : a;
      (void)resolved_builtin_layout(e, *paired,
                                    paired->arity == 2
                                        ? std::vector<Range>{a, b}
                                        : std::vector<Range>{a});
      Range out{0, 1};
      if (paired->difference) {
        const int d = alloc(a.len);
        for (int i = 0; i < a.len; ++i)
          emit(Program::SUB, d + i, a.reg + i, b.reg + i);
        const Range difference{d, a.len};
        return kernel_call(OP_DOT, {difference, difference}, out, 0, 0x3, {},
                           {}, e.name);
      }
      return kernel_call(OP_DOT, {a, b}, out, 0, 0x3, {}, {}, e.name);
    }

    // Registered grouped reductions: the shared grouped dot kernel over the
    // operands' column-major ranges, one in-order dot per column or row --
    // the accumulation the AoS reverse-mode overloads perform. Previously
    // unsupported here.
    if (const BuiltinSpec* grouped = shaped_builtin_spec(
            e.name, e.args.size(), BuiltinShapePolicy::GroupedReduction)) {
      Range a = expr(e.args[0]);
      Range b = grouped->arity == 2 ? expr(e.args[1]) : a;
      BuiltinGroupedDotMap map;
      try {
        map = builtin_grouped_dot_map(
            *grouped, builtin_argument_shape(e.args[0], a),
            builtin_argument_shape(e.args[grouped->arity == 2 ? 1 : 0], b));
      } catch (const std::invalid_argument& error) {
        bail(e.name + ": " + std::string(error.what()));
      }
      Range out{0, (int)map.groups};
      out.kind = function_view_kind(map.result.container);
      return kernel_call(OP_GROUP_DOT, {a, b}, out, 0, 0x3,
                         {(int)map.groups, (int)map.width,
                          (int)map.group_stride, (int)map.cell_stride},
                         {}, e.name);
    }

    if (e.name == "eval" && e.args.size() == 1) return expr(e.args[0]);

    // Registered matrix operations: the same dedicated kernels the graph
    // emits, called over the operands' column-major ranges. The active
    // variant bit mirrors the graph's autodiff stamp -- any non-data
    // operand -- with the kernels' values_only() guard covering
    // values-only executions, exactly as it does for graph ops.
    if (const BuiltinSpec* matrix = shaped_builtin_spec(
            e.name, e.args.size(), BuiltinShapePolicy::MatrixOp))
      return matrix_op_call(e, matrix);

    // Registered reductions never take the elementwise dispatch below. sum
    // and log_sum_exp keep their native register loops further down; the
    // remaining reductions call their registered kernel with a scalar out.
    if (const BuiltinSpec* reduction =
            reduction_builtin_spec(e.name, e.args.size());
        reduction != nullptr) {
      builtin = nullptr;
      if (reduction->opcode != OP_SUM_VEC &&
          reduction->opcode != OP_LOG_SUM_EXP) {
        const std::vector<Range> args{expr(e.args[0])};
        (void)resolved_builtin_layout(e, *reduction, args);
        // mean groups its sum as Stan Math's Eigen mean does: in order for
        // an autodiff operand, by the source layout for a data vector, in
        // packets for a data std::vector.
        std::pair<SumGrouping, int64_t> grouping{SumGrouping::InOrder, 0};
        if (reduction->opcode == OP_MEAN &&
            (in_write_array || e.args[0].data_only))
          grouping = sum_grouping(mir::eigen_leaf(e.args[0]) &&
                                          e.args[0].unsized.depth == 0
                                      ? mir::source_expression_layout(e.args[0])
                                      : ExpressionLayout::direct());
        std::vector<int> idata;
        if (grouping.first == SumGrouping::Phased)
          idata.push_back(static_cast<int>(grouping.second));
        Range out{0, 1};
        const bool var_array = reduction->opcode == OP_VARIANCE &&
                               e.args[0].unsized.depth == 1 &&
                               !in_write_array && !e.args[0].data_only;
        return kernel_call(reduction->opcode, args, out,
                           var_array ? kVarianceOfVarArray
                                     : static_cast<uint8_t>(grouping.first),
                           reduction->activity_mask, std::move(idata), {},
                           e.name);
      }
    }
    // Matrix right-division is a solve, not the scalar/elementwise DIV in the
    // descriptor. Its specialized shape-aware lowering remains below, as does
    // predicate lowering: a Predicate descriptor has no kernel, and its
    // comparison-opcode spelling lives with the other operators.
    if (builtin != nullptr && builtin->shape != BuiltinShapePolicy::Predicate &&
        builtin->shape != BuiltinShapePolicy::Product &&
        builtin->shape != BuiltinShapePolicy::Solve &&
        !(e.name == "Divide__" && e.args.size() == 2 &&
          mir::is_unsized(e.args[1], mir::UnsizedLeaf::Matrix))) {
      if (const auto native = native_builtin_code(builtin->opcode))
        return native_builtin_call(e, *builtin, *native);
      return builtin_kernel_call(e, *builtin);
    }
    if (const auto wiener = wiener_form(e.name, e.args.size())) {
      const int nargs = wiener->reals;
      std::vector<Range> args;
      for (int k = 0; k < nargs; ++k) args.push_back(expr(e.args[k]));
      int lanes = 1;
      for (const Range& a : args) lanes = std::max(lanes, a.len);
      int active = 0;
      for (int k = 0; k < nargs; ++k) {
        if (args[k].len != lanes) bail(e.name + ": arguments differ in length");
        if (autodiff(e.args[k])) active |= 1 << k;
      }
      const int packed = alloc(nargs * lanes);
      for (int k = 0; k < nargs; ++k)
        for (int i = 0; i < lanes; ++i)
          emit(Program::MOV, packed + k * lanes + i, args[k].reg + i);
      const Range precision = wiener->has_precision
                                  ? expr(e.args[nargs])
                                  : Range{konst(wiener->default_precision), 1};
      const bool propto_density =
          wiener->kind == WienerKind::Lpdf && e.fn_propto && propto_ctx;
      return kernel_call(
          OP_WIENER_PACKED_LPDF, {Range{packed, nargs * lanes}, precision},
          Range{0, 1}, 0, active ? 0x1 : 0,
          {nargs, lanes, active, propto_density ? 1 : 0, (int)wiener->kind}, {},
          e.name);
    }
    if (registered != nullptr && registered->density() != nullptr)
      return density_call(e, *registered->density());
    if (e.name == "tcrossprod" && e.args.size() == 1)
      return matrix_gram(expr(e.args[0]), false);
    if ((e.name == "LDivide__" || e.name == "mdivide_left") &&
        e.args.size() == 2) {
      const Range divisor = expr(e.args[0]);
      const Range rhs = expr(e.args[1]);
      if (divisor.kind != ViewKind::Matrix || divisor.rows != divisor.cols)
        bail("mdivide_left requires a square matrix divisor");
      if ((rhs.kind != ViewKind::Matrix && rhs.kind != ViewKind::Vector) ||
          (rhs.kind == ViewKind::Matrix && rhs.rows != divisor.rows) ||
          (rhs.kind == ViewKind::Vector && rhs.len != divisor.rows))
        bail("mdivide_left right-hand side size mismatch");
      const int r = alloc(rhs.len);
      const int32_t encoded_rows = rhs.kind == ViewKind::Vector
                                       ? -(int32_t)divisor.rows
                                       : (int32_t)divisor.rows;
      if (rhs.len != 0)
        p.code.push_back(Program::Instr{Program::MDIVIDE_LEFT, r, divisor.reg,
                                        rhs.reg, encoded_rows, rhs.len});
      Range out = rhs;
      out.reg = r;
      return out;
    }
    if (e.name == "mdivide_right_spd" && e.args.size() == 2) {
      const Range lhs = expr(e.args[0]);
      const Range divisor = expr(e.args[1]);
      if (divisor.kind != ViewKind::Matrix || divisor.rows != divisor.cols)
        bail("mdivide_right_spd requires a square matrix divisor");
      if ((lhs.kind != ViewKind::Matrix && lhs.kind != ViewKind::RowVector) ||
          (lhs.kind == ViewKind::Matrix && lhs.cols != divisor.cols) ||
          (lhs.kind == ViewKind::RowVector && lhs.len != divisor.cols))
        bail("mdivide_right_spd left-hand side size mismatch");
      const int r = alloc(lhs.len);
      const int32_t encoded_cols = lhs.kind == ViewKind::RowVector
                                       ? -(int32_t)divisor.cols
                                       : (int32_t)divisor.cols;
      if (lhs.len != 0)
        p.code.push_back(Program::Instr{Program::MDIVIDE_RIGHT_SPD, r,
                                        divisor.reg, lhs.reg, encoded_cols,
                                        lhs.len});
      Range out = lhs;
      out.reg = r;
      return out;
    }
    // The remaining registered solves (and the operator spellings the two
    // native instructions above do not cover) go through the graph solve
    // kernels: the shared resolver validates the square divisor and the
    // dividend's conformity, and the variant carries the same operand-type
    // bits the graph lowering stamps -- result var, vector-vs-one-column
    // dividend, and the divisor/dividend scalar types.
    {
      const BuiltinSpec* solve =
          shaped_builtin_spec(e.name, e.args.size(), BuiltinShapePolicy::Solve);
      if (solve == nullptr && e.name == "Divide__" && e.args.size() == 2 &&
          mir::is_unsized(e.args[1], mir::UnsizedLeaf::Matrix))
        solve =
            shaped_builtin_spec("mdivide_right", 2, BuiltinShapePolicy::Solve);
      if (solve != nullptr) {
        std::vector<Range> args;
        args.reserve(2);
        for (const mir::Expr& argument : e.args) args.push_back(expr(argument));
        BuiltinSolveMap map;
        try {
          map = builtin_solve_map(*solve,
                                  builtin_argument_shape(e.args[0], args[0]),
                                  builtin_argument_shape(e.args[1], args[1]));
        } catch (const std::invalid_argument& error) {
          bail(e.name + ": " + std::string(error.what()));
        }
        const size_t dividend = solve->solve_left ? 1 : 0;
        const bool divisor_active = !e.args[1 - dividend].data_only;
        const bool dividend_active = !e.args[dividend].data_only;
        const bool dm = args[dividend].kind == ViewKind::Matrix;
        Range out{0, (int)map.result.storage_size};
        out = shaped(std::move(out), map.result);
        const uint8_t variant =
            (uint8_t)(((divisor_active || dividend_active) ? 1u : 0u) |
                      (dm ? 0u : 2u) | (divisor_active ? 4u : 0u) |
                      (dividend_active ? 8u : 0u));
        return kernel_call(solve->opcode, args, out, variant,
                           solve->activity_mask,
                           {(int)map.order, (int)map.columns}, {}, e.name);
      }
    }
    if (e.fn_lib == mir::Expr::Lib::UserDefined) {
      auto it = funs.find(mir::function_key(e));
      if (it == funs.end()) bail("unknown function " + e.name);
      return inline_call(*it->second, inline_arguments(e.args), false,
                         propto_ctx && mir::inherits_propto(e));
    }
    if (e.fn_lib == mir::Expr::Lib::Internal) {
      if (e.name == "FnMakeArray" || e.name == "FnMakeRowVec") {
        std::vector<Range> parts;
        int total = 0;
        for (const auto& a : e.args) {
          parts.push_back(expr(a));
          if (parts.back().len > kMaxRegs - total)
            bail("array literal needs too many registers");
          total += parts.back().len;
        }
        // A matrix literal is rows of row-vectors, while a matrix register
        // run is column-major, so this is the one literal that transposes
        // rather than concatenating.
        if (e.name == "FnMakeRowVec" &&
            mir::is_unsized(e, mir::UnsizedLeaf::Matrix)) {
          if (parts.empty()) bail("matrix literal has no rows");
          for (const Range& q : parts)
            if (q.kind != ViewKind::RowVector || q.len != parts.front().len)
              bail("matrix literal rows have different logical views");
          const int64_t rows = (int64_t)parts.size();
          const int64_t cols = parts.front().len;
          const int r = alloc(total);
          for (int64_t j = 0; j < cols; ++j)
            for (int64_t i = 0; i < rows; ++i)
              emit(Program::MOV, r + (int)(j * rows + i),
                   parts[(size_t)i].reg + (int)j);
          Range out{r, total};
          out.kind = ViewKind::Matrix;
          out.rows = rows;
          out.cols = cols;
          return out;
        }
        ViewKind array_leaf = ViewKind::Flat;
        std::vector<int64_t> array_leaf_dims;
        if (e.name == "FnMakeArray") {
          if (!parts.empty() && !is_scalar(parts.front())) {
            for (const Range& q : parts)
              if (!same_view(q, parts.front()))
                bail("array literal elements have different logical views");
            if (parts.front().kind == ViewKind::Array) {
              // An outer array literal adds one axis to the complete child
              // geometry; the scalar width of a singleton child must not
              // collapse that axis. Storage is already element-contiguous,
              // so only the neutral logical view needs extending.
              array_leaf = parts.front().leaf;
              array_leaf_dims = parts.front().dims;
            } else if (parts.front().kind == ViewKind::Matrix) {
              array_leaf = ViewKind::Matrix;
              array_leaf_dims = {parts.front().rows, parts.front().cols};
            } else {
              array_leaf = parts.front().kind;
              array_leaf_dims = {(int64_t)parts.front().len};
            }
          } else {
            for (const Range& q : parts)
              if (!is_scalar(q))
                bail("array literal mixes scalar and container elements");
          }
        }
        const int r = alloc(total);
        int at = 0;
        for (const Range& q : parts)
          for (int k = 0; k < q.len; ++k)
            emit(Program::MOV, r + at++, q.reg + k);
        Range out{r, total};
        out.kind =
            e.name == "FnMakeRowVec" ? ViewKind::RowVector : ViewKind::Array;
        if (e.name == "FnMakeArray") {
          out.dims = {(int64_t)e.args.size()};
          out.leaf = array_leaf;
          for (int64_t d : array_leaf_dims) out.dims.push_back(d);
        }
        return out;
      }
      bail("internal function " + e.name);
    }
    const mir::ExtremaCall extrema = mir::extrema_call(e);
    if (extrema.kind != mir::ExtremaKind::Legacy) {
      Range a;
      if (extrema.surface == mir::ExtremaSurface::IntPair) {
        const Range lhs = expr(e.args[0]);
        const Range rhs = expr(e.args[1]);
        if (!is_scalar(lhs) || !is_scalar(rhs))
          bail("min/max integer pair needs scalar arguments");
        a = Range{alloc(2), 2};
        emit(Program::MOV, a.reg, lhs.reg);
        emit(Program::MOV, a.reg + 1, rhs.reg);
      } else {
        a = expr(e.args[0]);
      }
      const int r = alloc(1);
      const bool maximum = extrema.kind == mir::ExtremaKind::Max;
      const bool integer = extrema.surface == mir::ExtremaSurface::IntArray ||
                           extrema.surface == mir::ExtremaSurface::IntPair;
      // Matrix<var>, vector<var>, and std::vector<var> all reduce in ascending
      // scalar order. Otherwise retain the source expression's double
      // evaluator grouping even though `expr` materialized it into a flat
      // register run above. This mirrors Lowering::reduction_grouping.
      const bool active = !in_write_array && !e.args[0].data_only;
      const ExpressionLayout layout =
          integer || active ? ExpressionLayout::scalar()
                            : mir::source_expression_layout(e.args[0]);
      if (!layout.known()) bail("min/max expression grouping is not native");
      int32_t flags = integer ? kProgramExtremaInteger : 0;
      if (layout.kind == ExpressionLayout::Kind::Scalar) {
        flags |= kProgramExtremaScalar;
      } else if (layout.kind == ExpressionLayout::Kind::Direct &&
                 layout.element_offset != 0) {
        flags |= kProgramExtremaPhased;
        const int64_t phase = layout.element_offset % extrema_phase_modulus();
        flags |= static_cast<int32_t>(phase << kProgramExtremaPhaseShift);
      }
      p.code.push_back(Program::Instr{Program::EXTREMA_RANGE, r, a.reg,
                                      maximum ? 1 : 0, flags, a.len});
      return typed(Range{r, 1}, e.unsized);
    }
    if (e.name == "fma" && e.args.size() == 3) {
      // Fused, elementwise with scalar broadcast, mirroring OP_FMA.
      const Range a = expr(e.args[0]), b = expr(e.args[1]), c = expr(e.args[2]);
      int n = 1;
      Range shaped{0, 1};
      for (const Range* x : {&a, &b, &c}) {
        if (x->kind == ViewKind::Array)
          bail("array arithmetic is unsupported by the register program");
        if (is_scalar(*x)) continue;
        if (n != 1 && x->len != n) bail("fma on different lengths");
        n = x->len;
        shaped = *x;
      }
      const int r = alloc(n);
      for (int i = 0; i < n; ++i)
        emit(Program::FMA, r + i, a.reg + (is_scalar(a) ? 0 : i),
             b.reg + (is_scalar(b) ? 0 : i), c.reg + (is_scalar(c) ? 0 : i));
      Range out = shaped;
      out.reg = r;
      out.len = n;
      return typed(out, e.unsized);
    }
    if (e.name == "log_mix" && e.args.size() == 3) {
      const Range theta = expr(e.args[0]), a = expr(e.args[1]),
                  b = expr(e.args[2]);
      if (!is_scalar(theta) || !is_scalar(a) || !is_scalar(b))
        bail("log_mix on containers");
      const int r = alloc(1);
      emit(Program::LOG_MIX, r, theta.reg, a.reg, b.reg);
      return {r, 1};
    }
    if ((e.name == "diag_pre_multiply" || e.name == "diag_post_multiply") &&
        e.args.size() == 2) {
      // One instruction preserves stan-math's diagonal-product callback as a
      // unit. Scalar MULs would accumulate the repeated vector's adjoint in
      // tape order instead of the callback's rowwise/colwise reduction order.
      const bool pre = e.name == "diag_pre_multiply";
      const Range v = expr(e.args[pre ? 0 : 1]);
      const Range m = expr(e.args[pre ? 1 : 0]);
      if (m.kind != ViewKind::Matrix)
        bail(e.name + " requires a matrix argument");
      if (v.kind != ViewKind::Vector && v.kind != ViewKind::RowVector)
        bail(e.name + " requires a vector argument");
      const int64_t expected = pre ? m.rows : m.cols;
      if (v.len != expected)
        bail(e.name + " vector length does not match the matrix");
      const int r = alloc(m.len);
      // A zero-size multiplication has no values or adjoints. In particular,
      // do not narrow an arbitrarily large empty dimension into Instr.
      if (m.len != 0)
        p.code.push_back(Program::Instr{
            pre ? Program::DIAG_PRE_MULTIPLY : Program::DIAG_POST_MULTIPLY, r,
            v.reg, m.reg, (int32_t)m.rows, (int32_t)m.cols});
      Range out = m;
      out.reg = r;
      return typed(out, e.unsized);
    }
    if (e.args.size() == 2) {
      // An int-typed binary is integer arithmetic, and `divide` truncates
      // where the real DIV below does not: `divide(7, 2)` is 3, not 3.5.
      // The register file holds only reals, so fold the integer answer
      // whenever cint can reach it rather than emitting real arithmetic.
      if (mir::is_unsized(e, mir::UnsizedLeaf::Int)) {
        long v;
        if (try_cint(e, &v)) return {konst((double)v), 1};
      }
      const Range a = expr(e.args[0]), b = expr(e.args[1]);
      const bool a_scalar = is_scalar(a);
      const bool b_scalar = is_scalar(b);
      if (a.kind == ViewKind::Array || b.kind == ViewKind::Array)
        bail("array arithmetic is unsupported by the register program");
      if ((e.name == "Times__" || e.name == "multiply") && !a_scalar &&
          !b_scalar) {
        // The shared product resolver classifies the matvec/GEMM/outer/
        // inner forms and validates the inner dimension; the register
        // emission below stays the explicit MUL/ADD chain the adjoint
        // machinery prices.
        BuiltinProductMap map;
        try {
          map = builtin_product_map(builtin_argument_shape(e.args[0], a),
                                    builtin_argument_shape(e.args[1], b));
        } catch (const std::invalid_argument& error) {
          bail(builtin_error_message(e.name, error));
        }
        const int64_t rows = map.m, inner = map.k, cols = map.n;
        const ViewKind result_kind =
            map.result.container == FunctionContainerKind::Matrix
                ? ViewKind::Matrix
            : map.result.container == FunctionContainerKind::Vector
                ? ViewKind::Vector
            : map.result.container == FunctionContainerKind::RowVector
                ? ViewKind::RowVector
                : ViewKind::Flat;

        const int64_t width = rows * cols;
        if (width > kMaxRegs) bail("matrix product needs too many registers");
        const int r = alloc((int)width);
        const auto left = [&](int64_t i, int64_t k) {
          if (a.kind == ViewKind::Matrix) return a.reg + (int)(k * a.rows + i);
          return a.reg + (int)(a.kind == ViewKind::Vector ? i : k);
        };
        const auto right = [&](int64_t k, int64_t j) {
          if (b.kind == ViewKind::Matrix) return b.reg + (int)(j * b.rows + k);
          return b.reg + (int)(b.kind == ViewKind::Vector ? k : j);
        };
        for (int64_t j = 0; j < cols; ++j)
          for (int64_t i = 0; i < rows; ++i) {
            const int dst = r + (int)(j * rows + i);
            if (inner == 0) {
              const double zero = 0.0;
              emit_const(dst, &zero, 1);
              continue;
            }
            emit(Program::MUL, dst, left(i, 0), right(0, j));
            for (int64_t k = 1; k < inner; ++k) {
              const int term = alloc(1);
              emit(Program::MUL, term, left(i, k), right(k, j));
              emit(Program::ADD, dst, dst, term);
            }
          }
        Range out{r, (int)width};
        out.kind = result_kind;
        if (result_kind == ViewKind::Matrix) {
          out.rows = rows;
          out.cols = cols;
        }
        return typed(out, e.unsized);
      }
      if (!a_scalar && !b_scalar && !same_view(a, b))
        bail("binary " + e.name + " on different logical views");
      // `multiply` rides with `Times__` here for the same reason it does
      // in the graph lowering: on two containers it is linear algebra,
      // not the elementwise MUL the register file would emit.
      const int n = a_scalar ? b.len : (b_scalar ? a.len : a.len);
      Program::Code c;
      // The named spellings of the operators, on the same opcodes: a
      // region whose control flow depends on a parameter has to compile
      // here or not at all, so a gap is a hard error rather than a slow
      // path.
      if (e.name == "Plus__" || e.name == "add")
        c = Program::ADD;
      else if (e.name == "Minus__" || e.name == "subtract")
        c = Program::SUB;
      else if (e.name == "Times__" || e.name == "EltTimes__" ||
               e.name == "multiply" || e.name == "elt_multiply")
        c = Program::MUL;
      else if (e.name == "IntDivide__" || e.name == "Divide__" ||
               e.name == "EltDivide__" || e.name == "divide" ||
               e.name == "elt_divide")
        c = mir::is_unsized(e, mir::UnsizedLeaf::Int) ? Program::IDIV
                                                      : Program::DIV;
      else if (e.name == "Modulo__")
        c = Program::IMOD;
      else if (e.name == "Pow__" || e.name == "pow")
        c = Program::POW;
      else if (e.name == "fmax")
        c = Program::FMAX;
      else if (e.name == "fmin")
        c = Program::FMIN;
      else if (e.name == "log_sum_exp")
        c = Program::LSE2;
      else if (e.name == "log_diff_exp")
        c = Program::LOG_DIFF_EXP;
      else if (const BuiltinSpec* pred = shaped_builtin_spec(
                   e.name, 2, BuiltinShapePolicy::Predicate)) {
        // Comparisons on the comparison opcodes, for both the operator
        // spellings and the logical_* library names. logical_and and
        // logical_or fold each side's zero-ness first: both sides are
        // always evaluated, Stan Math's own (non-short-circuit) rule.
        switch (pred->predicate) {
          case BuiltinPredicate::Gt:
            c = Program::GT;
            break;
          case BuiltinPredicate::Gte:
            c = Program::GE;
            break;
          case BuiltinPredicate::Lt:
            c = Program::LT;
            break;
          case BuiltinPredicate::Lte:
            c = Program::LE;
            break;
          case BuiltinPredicate::Eq:
            c = Program::EQ;
            break;
          case BuiltinPredicate::Neq:
            c = Program::NE;
            break;
          case BuiltinPredicate::And:
          case BuiltinPredicate::Or: {
            const int zero = konst(0.0);
            const int lhs_set = alloc(n), rhs_set = alloc(n), both = alloc(n);
            for (int i = 0; i < n; ++i) {
              emit(Program::NE, lhs_set + i, a.reg + (a_scalar ? 0 : i), zero);
              emit(Program::NE, rhs_set + i, b.reg + (b_scalar ? 0 : i), zero);
              if (pred->predicate == BuiltinPredicate::And) {
                emit(Program::MUL, both + i, lhs_set + i, rhs_set + i);
              } else {
                emit(Program::ADD, both + i, lhs_set + i, rhs_set + i);
                emit(Program::NE, both + i, both + i, zero);
              }
            }
            return typed(Range{both, n}, e.unsized);
          }
          default:
            bail("function " + e.name);
        }
      } else
        bail("function " + e.name);
      if (mir::is_unsized(e, mir::UnsizedLeaf::Int)) c = integer_instruction(c);
      const int law = c == Program::POW
                          ? mir::pow_zero_base_law(e.args[0], e.args[1],
                                                   !e.args[1].data_only)
                      : c == Program::FMAX || c == Program::FMIN
                          ? (e.args[0].data_only ? 0 : 0x1) |
                                (e.args[1].data_only ? 0 : 0x2)
                          : 0;
      const int r = alloc(n);
      for (int i = 0; i < n; ++i)
        emit(c, r + i, a.reg + (a_scalar ? 0 : i), b.reg + (b_scalar ? 0 : i),
             0, law);
      Range out{r, n};
      if (a_scalar && !b_scalar)
        out = b;
      else if (b_scalar && !a_scalar)
        out = a;
      else if (same_view(a, b))
        out = a;
      out.reg = r;
      out.len = n;
      return typed(out, e.unsized);
    }
    if (e.args.size() == 1) {
      const Range a = expr(e.args[0]);
      if (e.name == "log_sum_exp") {
        if (a.len == 0)
          return {konst(-std::numeric_limits<double>::infinity()), 1};
        const int r = alloc(1);
        p.code.push_back(
            Program::Instr{Program::LSE_RANGE, r, a.reg, 0, 0, a.len});
        return {r, 1};
      }
      if (e.name == "sum") {
        if (a.len == 0) return {konst(0.0), 1};
        const int r = alloc(1);
        emit(Program::MOV, r, a.reg);
        const auto add = mir::is_unsized(e, mir::UnsizedLeaf::Int)
                             ? Program::IADD
                             : Program::ADD;
        for (int i = 1; i < a.len; ++i) emit(add, r, r, a.reg + i);
        return {r, 1};
      }
      // Registered unary predicates, spelled on the comparison opcodes
      // rather than opcodes of their own: both read through value_of, so
      // neither carries an adjoint edge, which is what a 0/1 answer wants.
      // x != x holds for NaN alone.
      if (const BuiltinSpec* pred =
              shaped_builtin_spec(e.name, 1, BuiltinShapePolicy::Predicate)) {
        const bool nan = pred->predicate == BuiltinPredicate::IsNan;
        const int rhs = nan ? -1 : konst(0.0);
        const Program::Code c = nan ? Program::NE
                                : pred->predicate == BuiltinPredicate::IntStep
                                    ? Program::GT
                                    : Program::EQ;
        const int r = alloc(a.len);
        if (pred->predicate == BuiltinPredicate::IsInf) {
          const int pos = konst(std::numeric_limits<double>::infinity());
          const int neg = konst(-std::numeric_limits<double>::infinity());
          const int eq_pos = alloc(a.len), eq_neg = alloc(a.len);
          for (int i = 0; i < a.len; ++i) {
            emit(Program::EQ, eq_pos + i, a.reg + i, pos);
            emit(Program::EQ, eq_neg + i, a.reg + i, neg);
            emit(Program::ADD, r + i, eq_pos + i, eq_neg + i);
          }
        } else {
          for (int i = 0; i < a.len; ++i)
            emit(c, r + i, a.reg + i, rhs < 0 ? a.reg + i : rhs);
        }
        Range out = a;
        out.reg = r;
        return typed(out, e.unsized);
      }
      Program::Code c;
      if (e.name == "PMinus__")
        c = Program::NEG;
      else if (e.name == "PPlus__")
        c = Program::MOV;
      else if (e.name == "exp")
        c = Program::EXP;
      else if (e.name == "log")
        c = Program::LOG;
      else if (e.name == "sqrt")
        c = Program::SQRT;
      else if (e.name == "square")
        c = Program::SQUARE;
      else if (e.name == "inv")
        c = Program::INV;
      else if (e.name == "fabs" || e.name == "abs")
        c = Program::FABS;
      else if (e.name == "inv_logit")
        c = Program::INV_LOGIT;
      else if (e.name == "log1p_exp")
        c = Program::LOG1P_EXP;
      else
        bail("function " + e.name);
      const int r = alloc(a.len);
      for (int i = 0; i < a.len; ++i) emit(c, r + i, a.reg + i);
      Range out = a;
      out.reg = r;
      return typed(out, e.unsized);
    }
    bail("function " + e.name);
  }

  // ---- statements ----------------------------------------------------------
  // Whether control can reach the statement after the one just compiled.
  // Ended follows a runtime function exit on every path through it; branch
  // and loop handlers still compile their other reachable paths. Returned
  // is a single unconditional return whose value is the frame's result; it
  // needs no join, so nothing after it in the function is compiled.
  enum class [[nodiscard]] Path { Open, Ended, Returned };
  struct ReturnFrame {
    const std::vector<mir::Stmt>* body = nullptr;
    ReturnFrame* parent = nullptr;
    size_t loop_base = 0;
    bool has_result = false;
    Range result;
    // A function with several results: one range each.
    std::vector<Range> results;
    std::vector<int> jumps;
  };
  ReturnFrame* return_frame = nullptr;
  const mir::Stmt* checked_region = nullptr;

  void require_runtime_integer_contract() {
    if (!return_frame && !checked_region)
      bail("runtime for/integer binding needs a checked function scope");
    std::set<const mir::FunDef*> visited;
    if (checked_region)
      check_program_integer_contract({*checked_region}, funs, visited);
    for (auto* frame = return_frame; frame; frame = frame->parent)
      check_program_integer_contract(*frame->body, funs, visited);
  }

  void compile_loop_body(const mir::Stmt& loop, int index) {
    known_reals.erase(loop.loopvar);
    require_runtime_integer_contract();
    ints.erase(loop.loopvar);
    reals[loop.loopvar] = Range{index, 1};
    loops.push_back({});
    loops.back().structured = true;
    structured_while_seen = true;
    ++structured_while_depth;
    ++runtime_for_depth;
    (void)compile_body(loop.body);
    --runtime_for_depth;
    --structured_while_depth;
    loops.pop_back();
    reals.erase(loop.loopvar);
    int_decl_at.erase(loop.loopvar);
  }

  void emit_function_return(Range value) {
    emit_function_return(std::vector<Range>{value}, false);
  }

  // A return reached at run time copies its values into the frame's result
  // registers and jumps to the function's end.
  void emit_function_return(const std::vector<Range>& values, bool several) {
    if (!return_frame) bail("runtime return has no function scope");
    auto& frame = *return_frame;
    std::vector<Range> single;
    if (!several && frame.has_result) single.push_back(frame.result);
    std::vector<Range>& slots = several ? frame.results : single;
    if (!frame.has_result) {
      slots = values;
      for (Range& slot : slots) slot.reg = alloc(slot.len);
      frame.has_result = true;
    } else if (slots.size() != values.size()) {
      bail("function returns give different numbers of values");
    } else {
      for (size_t k = 0; k < values.size(); ++k)
        if (!same_view(slots[k], values[k]))
          bail("function returns have different logical views");
    }
    for (size_t k = 0; k < values.size(); ++k)
      for (int i = 0; i < values[k].len; ++i)
        emit(Program::MOV, slots[k].reg + i, values[k].reg + i);
    if (!several) frame.result = slots[0];
    frame.jumps.push_back(emit(Program::JMP, 0));
  }

  Range function_body(const std::vector<mir::Stmt>& body,
                      bool void_call = false,
                      std::vector<Range>* results = nullptr) {
    ReturnFrame frame;
    frame.body = &body;
    frame.parent = return_frame;
    frame.loop_base = loops.size();
    struct Scope {
      ReturnFrame*& current;
      ReturnFrame* saved;
      ~Scope() { current = saved; }
    } scope{return_frame, return_frame};
    return_frame = &frame;
    const size_t start = p.code.size();
    const Path path = compile_body(body);
    if (results) *results = frame.results;
    if (path == Path::Returned) return frame.result;
    if (path == Path::Open && !void_call)
      bail("function can finish without returning a value");
    if (!frame.has_result && !void_call) bail("function returned no value");
    // Fall through at the final return instead of dispatching a no-op jump.
    // Path::Ended propagates only through all-return paths; handlers with a
    // fallthrough edge to code.size() consume it before reaching here. A
    // branch already patched to land just after the jump, as the one for
    // `if (c) return;` is, moves back with it. Only the body's own code can
    // branch there.
    if (!frame.jumps.empty() && frame.jumps.back() == (int)p.code.size() - 1) {
      const int after = (int)p.code.size();
      p.code.pop_back();
      frame.jumps.pop_back();
      for (size_t i = start; i < p.code.size(); ++i) {
        Program::Instr& I = p.code[i];
        if ((I.code == Program::JZ || I.code == Program::JMP) && I.dst == after)
          I.dst = after - 1;
      }
    }
    for (int jump : frame.jumps) p.code[(size_t)jump].dst = (int)p.code.size();
    return frame.result;
  }

  struct CompileBreak {};
  struct CompileContinue {};

  int64_t sized_len(const mir::SizedType& t);

  // Declare (or redeclare) a real variable of `len` registers. Stan's
  // uninitialized real value is NaN; callers may provide another fill only
  // when the surrounding lowering has an explicit initialized-value policy.
  Range declare(const std::string& name, int len, Range view = {},
                double fill = std::numeric_limits<double>::quiet_NaN());

  // Close the program: prepend the NaN fills the zero-length adoption in
  // Assignment (below) deferred.
  // Every caller runs this once the region has compiled and before the
  // program runs; it is idempotent, and a region with no adoption pays
  // nothing. The fills go in front rather than at the declaration because
  // the width is only known once the assignment inside the branch has
  // compiled, and the jumps are the only instructions that name a code
  // position (CONST/CONSTR's `a` is a pool index, CALL's is a call index).
  void finish();

  // Entry point for a region statement. A region has no function scope:
  // a return in it bails, so no path through it ends.
  void stmt(const mir::Stmt& s) { (void)compile_stmt(s); }

  Path compile_body(const std::vector<mir::Stmt>& body) {
    for (const auto& child : body)
      if (const Path path = compile_stmt(child); path != Path::Open)
        return path;
    return Path::Open;
  }

  // A MultiAssign result being assigned to its target: the assignment
  // compile_multi_assign builds for it, and the registers it assigns.
  const mir::Stmt* given_stmt = nullptr;
  Range given_value;
  bool given(const mir::Stmt& s) const { return &s == given_stmt; }
  Range assignment_value(const mir::Stmt& s) {
    return given(s) ? given_value : expr(s.rhs);
  }

  // Stan evaluates the whole right-hand side before it assigns, but an
  // indexed write moves one cell at a time. A value of several cells that
  // shares registers with the destination (`x[2:4] = x[1:3]`,
  // `v[{3, 2, 1}] = v`) is copied out first, so later moves do not read
  // cells that earlier moves overwrote.
  Range copied_if_overlapping(const Range& v, const Range& dst) {
    if (v.len < 2 || v.reg >= dst.reg + dst.len || dst.reg >= v.reg + v.len)
      return v;
    Range copy = v;
    copy.reg = alloc(v.len);
    for (int k = 0; k < v.len; ++k) emit(Program::MOV, copy.reg + k, v.reg + k);
    return copy;
  }

  void emit_range_reject(const RangeSelection& range, SelectorSite site,
                         bool upfrom, bool assign, int64_t extent);

  // `b[lo:hi]` or `b[lo:]` on a vector, row vector or array of scalars: a
  // view of the base's registers.
  Range one_dimensional_range(const Range& b, const mir::Expr& e);

  // Stan checks an assignment's sizes when the statement runs, and the
  // statement may sit in a branch that never runs, so a mismatch compiles
  // to a run-time rejection rather than a refusal. A destination whose size
  // the region chose has no single size, so stanli refuses it instead.
  void refuse_second_size(const std::string& name) {
    if (region_sized.count(name))
      bail(name +
           " is given containers of different sizes under a condition that "
           "depends on a parameter, which stanli does not support");
  }

  void emit_assign_size_reject(const std::string& name,
                               const std::string& message) {
    refuse_second_size(name);
    Program::Message m;
    m.spec.chunks = {message};
    m.spec.error = MessageError::InvalidArgument;
    p.messages.push_back(std::move(m));
    emit(Program::REJECT, 0, (int)p.messages.size() - 1);
  }

  static std::vector<int64_t> assign_dims(const Range& r) {
    switch (r.kind) {
      case ViewKind::Array:
        return r.dims.empty() ? std::vector<int64_t>{r.len} : r.dims;
      case ViewKind::Matrix:
        return {r.rows, r.cols};
      case ViewKind::Vector:
      case ViewKind::RowVector:
        return {r.len};
      case ViewKind::Flat:
        break;
    }
    return {};
  }

  // stan::model::assign's message for a whole-variable assignment whose
  // sizes differ, or "" when the two views are not comparable.
  static std::string whole_assign_size_message(const std::string& name,
                                               const Range& dst,
                                               const Range& v) {
    const ViewKind leaf = dst.kind == ViewKind::Array ? dst.leaf : dst.kind;
    const AssignLeaf assign_leaf =
        leaf == ViewKind::Matrix      ? AssignLeaf::Matrix
        : leaf == ViewKind::Vector    ? AssignLeaf::Vector
        : leaf == ViewKind::RowVector ? AssignLeaf::RowVector
                                      : AssignLeaf::Scalar;
    const size_t rank = leaf_rank(leaf);
    const std::vector<int64_t> dst_dims = assign_dims(dst);
    if (dst_dims.size() < rank) return "";
    return assign_size_mismatch(name, assign_leaf, dst_dims.size() - rank,
                                dst_dims, assign_dims(v));
  }

  // Stan Math's message for a one-index assignment whose selection and
  // right-hand side differ in size. `array` selects the std::vector rules.
  static std::string selected_assign_size_message(const std::string& name,
                                                  const mir::Expr& index,
                                                  bool array, int64_t want,
                                                  int64_t got) {
    const bool empty_range = index.kind == mir::Expr::IndexBetween && want == 0;
    std::string label;
    if (array)
      label = empty_range ? "array[negative_min_max, ...] assign"
                          : "array[multi, ...] assign";
    else if (index.kind == mir::Expr::IndexBetween)
      label = empty_range ? "vector[negative_min_max] assign"
                          : "vector[min_max] assign";
    else if (index.kind == mir::Expr::IndexUpfrom)
      label = "vector[min] assign";
    else
      label = "vector[multi] assign";
    const bool size_word = array && !empty_range;
    return label + ": assigning variable " + name + " (" +
           std::to_string(want) + ") and right hand side" +
           (size_word ? " size" : "") + " (" + std::to_string(got) +
           ") must match in size";
  }

  // The statements a call returning several values needs, kept out of
  // compile_stmt: a register-program refusal unwinds through every
  // compile_stmt frame, and each statement compiled inline there makes every
  // such unwind slower.
  [[gnu::noinline]] Path compile_multi_return(const mir::Stmt& s,
                                              bool runtime_exit) {
    std::vector<Range> values;
    for (const auto& value : s.fn_args) values.push_back(expr(value));
    if (return_frame && (runtime_exit || return_frame->has_result)) {
      emit_function_return(values, true);
      return Path::Ended;
    }
    if (!return_frame) bail("return without a function scope");
    return_frame->results = std::move(values);
    return Path::Returned;
  }

  [[gnu::noinline]] Path compile_multi_assign(const mir::Stmt& s) {
    std::vector<Range> values;
    if (s.rhs.fn_lib == mir::Expr::Lib::StanLib) {
      const BuiltinSpec* matrix = shaped_builtin_spec(
          s.rhs.name, s.rhs.args.size(), BuiltinShapePolicy::MatrixOp);
      if (matrix == nullptr)
        bail(s.rhs.name +
             ", which returns a tuple, is not supported in a "
             "runtime-control region");
      BuiltinMatrixMap map;
      const Range packed = matrix_op_call(s.rhs, matrix, &map);
      int offset = 0;
      for (const BuiltinArgumentShape& part : map.parts) {
        values.push_back(
            shaped(Range{packed.reg + offset, (int)part.storage_size}, part));
        offset += (int)part.storage_size;
      }
    } else if (s.rhs.fn_lib == mir::Expr::Lib::UserDefined) {
      auto it = funs.find(mir::function_key(s.rhs));
      if (it == funs.end()) bail("unknown function " + s.rhs.name);
      (void)inline_call(*it->second, inline_arguments(s.rhs.args), false,
                        propto_ctx && mir::inherits_propto(s.rhs), &values);
    } else {
      bail(s.rhs.name + ", which returns a tuple, is not supported yet");
    }
    if (values.size() != s.fn_args.size())
      bail(s.rhs.name + " returned the wrong number of values");
    // Each result is assigned as `target = value`: the assignment path takes
    // the result's registers in place of evaluating a right side.
    struct Given {
      const mir::Stmt*& stmt;
      ~Given() { stmt = nullptr; }
    } given_scope{given_stmt};
    for (size_t k = 0; k < values.size(); ++k) {
      mir::Stmt assign;
      assign.kind = mir::Stmt::Assignment;
      assign.lhs = s.fn_args[k].name;
      assign.raw = s.raw;
      given_stmt = &assign;
      given_value = values[k];
      const Path path = compile_stmt(assign);
      given_stmt = nullptr;
      if (path != Path::Open) bail("a MultiAssign target left the scope");
    }
    return Path::Open;
  }

  // A statement whose compile-time integer raises a math error, such as a
  // division by zero, raises it when the statement runs. The statement may
  // sit in a branch that never runs, so it compiles to a run-time rejection.
  Path compile_stmt(const mir::Stmt& s) {
    try {
      return compile_stmt_folded(s);
    } catch (const std::domain_error& error) {
      Program::Message m;
      m.spec.chunks = {error.what()};
      m.spec.error = MessageError::Domain;
      p.messages.push_back(std::move(m));
      emit(Program::REJECT, 0, (int)p.messages.size() - 1);
      return Path::Open;
    }
  }

  Path compile_stmt_folded(const mir::Stmt& s) {
    switch (s.kind) {
      case mir::Stmt::Decl: {
        // A declaration shadows every compile-time fact retained for an
        // earlier optimized symbol with the same name.
        // It also shadows either runtime representation: --O1 may reuse one
        // symbol id for a scalar int in one block and an array/container in
        // the next, and the later assignment must not see the old binding.
        reals.erase(s.decl_id);
        ints.erase(s.decl_id);
        known_reals.erase(s.decl_id);
        deferred_shapes.erase(s.decl_id);
        region_sized.erase(s.decl_id);
        known_int_arrays.erase(s.decl_id);
        known_int_array_dims.erase(s.decl_id);
        int_array_names.erase(s.decl_id);
        int_decl_at.erase(s.decl_id);
        int_reset_at.erase(s.decl_id);
        if (s.decl_type.base.empty() &&
            s.decl_type.unsized.leaf != mir::UnsizedLeaf::Unknown) {
          const mir::UnsizedView view = s.decl_type.unsized;
          if (view.leaf == mir::UnsizedLeaf::Complex)
            bail("complex unsized declaration " + s.decl_id);
          // Scalar Unsized declarations are normalized to SInt/SReal by the
          // reader.  Refuse an unnormalized scalar here rather than treating
          // its fixed width as a deferred container shape.
          if (view.depth == 0 && view.leaf != mir::UnsizedLeaf::Vector &&
              view.leaf != mir::UnsizedLeaf::RowVector &&
              view.leaf != mir::UnsizedLeaf::Matrix)
            bail("scalar unsized declaration " + s.decl_id);

          if (view.depth != 0 && view.leaf == mir::UnsizedLeaf::Int) {
            int_array_names.insert(s.decl_id);
            int_decl_at[s.decl_id] = {branch_depth, loops.size()};
          }
          if (!s.has_init) {
            if (runtime_for_depth)
              bail("runtime for needs a sized container declaration");
            reals[s.decl_id] = Range{};
            deferred_shapes[s.decl_id] = view;
            return Path::Open;
          }

          const Range v = expr(s.init);
          if (!unsized_accepts(view, v))
            bail("declaration logical view mismatch for " + s.decl_id);
          const double fill =
              int_array_names.count(s.decl_id)
                  ? static_cast<double>(std::numeric_limits<int>::min())
                  : std::numeric_limits<double>::quiet_NaN();
          const Range d = declare(s.decl_id, v.len, v, fill);
          for (int k = 0; k < v.len; ++k)
            emit(Program::MOV, d.reg + k, v.reg + k);
          if (int_array_names.count(s.decl_id)) {
            std::vector<long> values;
            if (try_cints(s.init, &values) && values.size() == (size_t)v.len) {
              known_int_arrays[s.decl_id] = std::move(values);
              known_int_array_dims[s.decl_id] = v.dims;
            }
          }
          return Path::Open;
        }
        if (s.decl_type.base.empty())
          bail("declaration has no sized or unsized logical view: " +
               s.decl_id);
        if (s.decl_type.base == "SInt") {
          int_decl_at[s.decl_id] = {branch_depth, loops.size()};
          if (runtime_for_depth) {
            const Range d =
                declare(s.decl_id, 1, Range{},
                        static_cast<double>(std::numeric_limits<int>::min()));
            if (s.has_init) {
              const Range v = expr(s.init);
              if (!is_scalar(v)) bail("integer declaration is not scalar");
              emit(Program::MOV, d.reg, v.reg);
            }
            return Path::Open;
          }
          if (s.has_init) {
            long folded;
            if (try_cint(s.init, &folded)) {
              ints[s.decl_id] = {folded};
              mark_int_reset(s.decl_id);
              return Path::Open;
            }
            // A declaration after or inside a structured loop may read
            // loop-carried state (for example `int any = hits != 0`). Only
            // that genuinely runtime initializer needs a register. Keeping
            // foldable locals as ints is essential for foreach indices used
            // in later matrix subscripts inside ctsem's integration loop.
            Range view;
            const Range d =
                declare(s.decl_id, 1, view,
                        static_cast<double>(std::numeric_limits<int>::min()));
            const Range v = expr(s.init);
            if (!is_scalar(v)) bail("integer declaration is not scalar");
            emit(Program::MOV, d.reg, v.reg);
            return Path::Open;
          }
          ints[s.decl_id] = {std::numeric_limits<int>::min()};
          mark_int_reset(s.decl_id);
          return Path::Open;
        }
        const bool int_array =
            s.decl_type.base == "SArray" && s.decl_type.elem_base == "SInt";
        if (int_array) {
          int_array_names.insert(s.decl_id);
          int_decl_at[s.decl_id] = {branch_depth, loops.size()};
        }
        if (s.has_init) {
          const Range v = expr(s.init);
          const int want = (int)sized_len(s.decl_type);
          if (v.len != want)
            bail("declaration width mismatch for " + s.decl_id);
          Range expected;
          expected.len = want;
          expected = declared(expected, s.decl_type);
          if (!same_view(v, expected))
            bail("declaration logical view mismatch for " + s.decl_id);
          const Range d = declare(s.decl_id, want, expected);
          for (int k = 0; k < want; ++k)
            emit(Program::MOV, d.reg + k, v.reg + k);
          if (int_array) {
            std::vector<long> values;
            if (try_cints(s.init, &values) && values.size() == (size_t)want) {
              known_int_arrays[s.decl_id] = std::move(values);
              known_int_array_dims[s.decl_id] = expected.dims;
            }
          }
        } else {
          Range view;
          const double fill =
              s.decl_type.base == "SArray" && s.decl_type.elem_base == "SInt"
                  ? static_cast<double>(std::numeric_limits<int>::min())
                  : std::numeric_limits<double>::quiet_NaN();
          const Range expected = declared(view, s.decl_type);
          declare(s.decl_id, (int)sized_len(s.decl_type), expected, fill);
          if (int_array) {
            known_int_arrays[s.decl_id] =
                std::vector<long>((size_t)sized_len(s.decl_type),
                                  std::numeric_limits<int>::min());
            known_int_array_dims[s.decl_id] = expected.dims;
          }
        }
        return Path::Open;
      }
      case mir::Stmt::Assignment: {
        if (accumulator && s.lhs == accumulator->name) {
          if (given(s) || !s.lhs_idx.empty() || s.rhs.name != "Plus__" ||
              s.rhs.args.size() != 2 || s.rhs.args[0].name != s.lhs)
            bail("accumulator " + s.lhs + " is not only added to");
          const Range v = expr(s.rhs.args[1]);
          for (int k = 0; k < v.len; ++k)
            emit(Program::ADD, accumulator->reg, accumulator->reg, v.reg + k);
          return Path::Open;
        }
        if (deferred_shapes.count(s.lhs)) {
          if (!s.lhs_idx.empty())
            bail("indexed assignment before unsized shape adoption for " +
                 s.lhs);
          // Evaluating the value can inline a call, which replaces these
          // maps and leaves any iterator into them dangling.
          const Range v = assignment_value(s);
          auto deferred = deferred_shapes.find(s.lhs);
          if (!unsized_accepts(deferred->second, v))
            bail("assignment logical view mismatch for " + s.lhs);
          auto binding = reals.find(s.lhs);
          if (binding == reals.end()) bail("assignment to undeclared " + s.lhs);

          Range adopted = v;
          adopted.reg = alloc(v.len);
          // The assignment may be under a runtime jump.  Fill the adopted
          // run before control flow starts so backward replay and an untaken
          // path never observe uninitialized register cells.
          if (v.len != 0) late_bound.emplace_back(adopted.reg, v.len);
          for (int k = 0; k < v.len; ++k)
            emit(Program::MOV, adopted.reg + k, v.reg + k);
          binding->second = adopted;
          deferred_shapes.erase(deferred);
          region_sized.insert(s.lhs);

          if (int_array_names.count(s.lhs)) {
            std::vector<long> values;
            if (fold_is_certain(s.lhs) && !given(s) &&
                try_cints(s.rhs, &values) && values.size() == (size_t)v.len) {
              known_int_arrays[s.lhs] = std::move(values);
              known_int_array_dims[s.lhs] = adopted.dims;
            } else {
              known_int_arrays.erase(s.lhs);
              known_int_array_dims.erase(s.lhs);
            }
          }
          return Path::Open;
        }
        if (ints.count(s.lhs) && s.lhs_idx.empty()) {
          long ignored;
          // A conditional write must preserve the old value on the untaken
          // path, so it cannot be folded into the single compile-time copy.
          // A runtime integer can also be assigned before its first loop
          // (for example a bound selected by a comparison of real inputs).
          // Reify any nonconstant scalar assignment; expr still decides
          // whether its integer operations have a supported implementation.
          const bool certain = fold_is_certain(s.lhs);
          const bool constant =
              certain && !given(s) && try_cint(s.rhs, &ignored);
          if (!certain || !constant) {
            if (certain && !constant && !structured_while_seen &&
                !in_write_array)
              require_runtime_integer_contract();
            reify_written_int(s.lhs);
          }
        }
        if (ints.count(s.lhs) && s.lhs_idx.empty()) {
          // This assignment is certain and its RHS stayed a compile-time
          // integer, so subsequent reads may use the folded value directly.
          ints[s.lhs] = {cint(s.rhs)};
          mark_int_reset(s.lhs);
          return Path::Open;
        }
        auto it = reals.find(s.lhs);
        if (it == reals.end()) {
          // Assigning to a name the region did not declare: it lives
          // outside, so bind it (its current value is a live-in -- the
          // untaken branch has to leave it alone) and assign into those
          // registers.
          Range ext;
          if (bind_extern && bind_extern(s.lhs, &ext)) {
            reals[s.lhs] = ext;
            extern_bound.insert(s.lhs);
            it = reals.find(s.lhs);
          }
        }
        if (it == reals.end() && s.lhs_idx.empty()) {
          // First touch of this name from here, and nothing to import as a
          // live-in either: a local whose only other write sat in a branch
          // the surrounding graph lowering folded away as unreachable, so
          // it never got a slot to hand this region. A full-variable write
          // with no prior binding is the zero-size "adopt the assigned
          // shape" case below, minus the placeholder declaration -- give it
          // one now.
          reals[s.lhs] = Range{};
          it = reals.find(s.lhs);
        }
        if (it == reals.end()) bail("assignment to undeclared " + s.lhs);
        const Range dst = it->second;
        const Range v = s.lhs_idx.empty()
                            ? assignment_value(s)
                            : copied_if_overlapping(assignment_value(s), dst);
        // Once written here, the imported value can no longer be answered
        // from the enclosing graph's preparation-time observation.
        extern_bound.erase(s.lhs);
        if (s.lhs_idx.empty()) {
          std::vector<long> folded_ints;
          const bool have_folded_ints = int_array_names.count(s.lhs) &&
                                        fold_is_certain(s.lhs) && !given(s) &&
                                        try_cints(s.rhs, &folded_ints) &&
                                        folded_ints.size() == (size_t)v.len;
          if (dst.len == 0 && (v.len != 0 || !same_view(v, dst))) {
            refuse_second_size(s.lhs);
            // stan::model::assign gives a zero-size destination the
            // right-hand side's size, so adopt the assigned shape. That
            // includes an empty value of another shape, such as a 0x0
            // matrix assigned to a name first written in this region.
            //
            // The assignment can sit inside a data-dependent
            // branch -- `cond ? udf(x) : y` inlines to an assignment under
            // `if (cond)` -- and then the arm that does not run leaves
            // these registers unwritten. They are the variable's live-out,
            // so the harvest reads them anyway: under the backward's var
            // replay that is a null (or a previous call's, already
            // recovered) vari. finish() fills them with NaN, which is what
            // Stan holds in a value it never computed.
            Range nd = v;
            nd.reg = alloc(v.len);
            late_bound.emplace_back(nd.reg, v.len);
            for (int k = 0; k < v.len; ++k)
              emit(Program::MOV, nd.reg + k, v.reg + k);
            reals[s.lhs] = nd;
            region_sized.insert(s.lhs);
            if (have_folded_ints) {
              known_int_arrays[s.lhs] = std::move(folded_ints);
              known_int_array_dims[s.lhs] = nd.dims;
            } else if (int_array_names.count(s.lhs)) {
              known_int_arrays.erase(s.lhs);
              known_int_array_dims.erase(s.lhs);
            }
            return Path::Open;
          }
          if (v.len != dst.len || !same_view(v, dst)) {
            const std::string message =
                whole_assign_size_message(s.lhs, dst, v);
            if (message.empty())
              bail("assignment logical view mismatch for " + s.lhs);
            emit_assign_size_reject(s.lhs, message);
            return Path::Open;
          }
          for (int k = 0; k < v.len; ++k)
            emit(Program::MOV, dst.reg + k, v.reg + k);
          if (have_folded_ints) {
            known_int_arrays[s.lhs] = std::move(folded_ints);
            known_int_array_dims[s.lhs] = dst.dims;
          } else if (int_array_names.count(s.lhs)) {
            known_int_arrays.erase(s.lhs);
            known_int_array_dims.erase(s.lhs);
          }
          return Path::Open;
        }
        // A single All is the complete destination view. This is reachable
        // in ODE functions and runtime-control statement islands, so it
        // must agree with both graph lowering and MirInterp rather than
        // forcing an otherwise supported region onto the fallback path.
        if (s.lhs_idx.size() == 1 && s.lhs_idx[0].kind == mir::Expr::IndexAll) {
          if (dst.kind == ViewKind::Flat)
            bail("full-span assignment needs a container for " + s.lhs);
          if (v.len != dst.len || !same_view(v, dst)) {
            const std::string message =
                whole_assign_size_message(s.lhs, dst, v);
            if (message.empty())
              bail("full-span assignment logical view mismatch for " + s.lhs);
            emit_assign_size_reject(s.lhs, message);
            return Path::Open;
          }
          for (int k = 0; k < v.len; ++k)
            emit(Program::MOV, dst.reg + k, v.reg + k);
          if (int_array_names.count(s.lhs)) {
            std::vector<long> values;
            if (fold_is_certain(s.lhs) && !given(s) &&
                try_cints(s.rhs, &values) && values.size() == (size_t)v.len) {
              known_int_arrays[s.lhs] = std::move(values);
              known_int_array_dims[s.lhs] = dst.dims;
            } else {
              known_int_arrays.erase(s.lhs);
              known_int_array_dims.erase(s.lhs);
            }
          }
          return Path::Open;
        }
        if ((dst.kind == ViewKind::Array || dst.kind == ViewKind::Matrix ||
             (s.lhs_idx.size() == 1 &&
              s.lhs_idx[0].kind == mir::Expr::IndexMulti)) &&
            has_runtime_selector(s.lhs_idx)) {
          const bool scalar_array =
              dst.kind == ViewKind::Array && dst.leaf == ViewKind::Flat &&
              (dst.dims.empty() || dst.dims.size() == 1) &&
              s.lhs_idx.size() == 1 &&
              s.lhs_idx[0].kind == mir::Expr::IndexSingle;
          if (scalar_array && is_scalar(v)) {
            const Range index = expr(s.lhs_idx[0].args.at(0));
            if (!is_scalar(index))
              bail("runtime assignment index must be scalar");
            emit(Program::DYN_SET, dst.reg, dst.reg, v.reg, index.reg, dst.len);
          } else {
            const Range updated = dynamic_index(dst, s.lhs_idx, &v);
            emit(Program::MOVR, dst.reg, updated.reg, 0, 0, dst.len);
          }
          known_int_arrays.erase(s.lhs);
          known_int_array_dims.erase(s.lhs);
          return Path::Open;
        }
        if (dst.kind == ViewKind::Matrix && s.lhs_idx.size() == 2) {
          const std::vector<int64_t> rows = matrix_positions(
              s.lhs_idx[0], dst.rows, "assignment row of " + s.lhs,
              SelectorSite::MatrixRow, true, true);
          const std::vector<int64_t> cols = matrix_positions(
              s.lhs_idx[1], dst.cols, "assignment column of " + s.lhs,
              SelectorSite::MatrixColumn, true, true);
          if (!rows.empty() && cols.size() > (size_t)kMaxRegs / rows.size())
            bail("matrix assignment selection is too large");
          const size_t width = rows.size() * cols.size();
          if (v.len != static_cast<int>(width)) {
            emit_assign_size_reject(
                s.lhs, "matrix assignment width mismatch for " + s.lhs + ": " +
                           std::to_string(width) + " selected, " +
                           std::to_string(v.len) + " assigned");
            return Path::Open;
          }
          int at = 0;
          for (int64_t j : cols)
            for (int64_t i : rows)
              emit(Program::MOV, dst.reg + (int)(j * dst.rows + i),
                   v.reg + at++);
          return Path::Open;
        }
        if (dst.kind == ViewKind::Array) {
          const std::vector<int64_t> dims =
              dst.dims.empty() ? std::vector<int64_t>{dst.len} : dst.dims;
          if (s.lhs_idx.size() > dims.size())
            bail("too many assignment indices for " + s.lhs);
          std::vector<std::vector<int64_t>> positions;
          positions.reserve(dims.size());
          for (size_t d = 0; d < dims.size(); ++d) {
            if (d < s.lhs_idx.size()) {
              positions.push_back(matrix_positions(
                  s.lhs_idx[d], dims[d], "assignment array of " + s.lhs,
                  selector_site(d, dims.size() - leaf_rank(dst.leaf),
                                dst.leaf == ViewKind::Matrix),
                  true, true));
            } else {
              if (dims[d] < 0 || dims[d] > kMaxRegs)
                bail("array assignment has an invalid omitted extent");
              positions.emplace_back();
              positions.back().reserve((size_t)dims[d]);
              for (int64_t k = 0; k < dims[d]; ++k)
                positions.back().push_back(k);
            }
          }
          size_t width = 1;
          for (const auto& axis : positions) {
            if (!axis.empty() && width > (size_t)kMaxRegs / axis.size())
              bail("array assignment selection is too large");
            width *= axis.size();
          }
          if (v.len != (int)width) {
            const int64_t got = v.kind == ViewKind::Array && !v.dims.empty()
                                    ? v.dims[0]
                                    : v.len;
            const bool leading_differs =
                s.lhs_idx[0].kind != mir::Expr::IndexSingle &&
                (int64_t)positions[0].size() != got;
            emit_assign_size_reject(
                s.lhs, leading_differs
                           ? selected_assign_size_message(
                                 s.lhs, s.lhs_idx[0], true,
                                 (int64_t)positions[0].size(), got)
                           : "array assignment width mismatch for " + s.lhs +
                                 ": " + std::to_string(width) + " selected, " +
                                 std::to_string(v.len) + " assigned");
            return Path::Open;
          }
          // The same shared index geometry the rvalue reads resolve
          // through; destination cells and the assigned run pair up in the
          // identical outer-major enumeration.
          const size_t leaf_axes =
              dst.leaf == ViewKind::Matrix ? 2
              : dst.leaf == ViewKind::Vector || dst.leaf == ViewKind::RowVector
                  ? 1
                  : 0;
          const BuiltinIndexMap store_map =
              builtin_index_map(dims, leaf_axes, positions,
                                std::vector<bool>(positions.size(), false),
                                SliceStorageOrder::OuterMajor);
          std::vector<int64_t> offsets;
          offsets.reserve((size_t)store_map.count);
          for (int64_t k = 0; k < store_map.count; ++k)
            offsets.push_back(store_map.kind ==
                                      BuiltinSliceMap::Kind::Contiguous
                                  ? store_map.offset + k
                              : store_map.kind == BuiltinSliceMap::Kind::Strided
                                  ? store_map.offset + k * store_map.stride
                                  : store_map.gather[(size_t)k]);
          for (size_t k = 0; k < offsets.size(); ++k)
            emit(Program::MOV, dst.reg + (int)offsets[k], v.reg + (int)k);
          if (int_array_names.count(s.lhs)) {
            std::vector<long> values;
            auto known = known_int_arrays.find(s.lhs);
            bool folded = false;
            if (offsets.size() == 1) {
              long value = 0;
              if (!given(s) && try_cint(s.rhs, &value)) {
                values = {value};
                folded = true;
              }
            } else {
              folded = !given(s) && try_cints(s.rhs, &values);
            }
            if (fold_is_certain(s.lhs) && folded &&
                values.size() == offsets.size() &&
                known != known_int_arrays.end()) {
              const std::vector<int64_t> known_offsets =
                  first_fast_array_offsets(dims, positions);
              for (size_t k = 0; k < known_offsets.size(); ++k)
                known->second[(size_t)known_offsets[k]] = values[k];
            } else {
              known_int_arrays.erase(s.lhs);
              known_int_array_dims.erase(s.lhs);
            }
          }
          return Path::Open;
        }
        if ((dst.kind == ViewKind::Vector || dst.kind == ViewKind::RowVector ||
             dst.kind == ViewKind::Flat) &&
            s.lhs_idx.size() == 1) {
          long fixed_index;
          if (s.lhs_idx[0].kind == mir::Expr::IndexSingle &&
              s.lhs_idx[0].args.size() == 1 &&
              !try_cint(s.lhs_idx[0].args[0], &fixed_index)) {
            if (!is_scalar(v)) bail("element assignment from a container");
            const Range index = expr(s.lhs_idx[0].args[0]);
            if (!is_scalar(index)) bail("non-scalar assignment index");
            // Reading and writing the full range in the opcode metadata
            // preserves untouched cells through register compaction. Replay
            // records the selected write, including an aliased RHS, normally.
            emit(Program::DYN_SET, dst.reg, dst.reg, v.reg, index.reg, dst.len);
            known_int_arrays.erase(s.lhs);
            return Path::Open;
          }
          const std::vector<int64_t> positions = matrix_positions(
              s.lhs_idx[0], dst.len, "assignment vector",
              dst.kind == ViewKind::Flat ? SelectorSite::Array
                                         : SelectorSite::Vector,
              true, true);
          if (v.len != static_cast<int>(positions.size())) {
            emit_assign_size_reject(
                s.lhs, selected_assign_size_message(
                           s.lhs, s.lhs_idx[0], dst.kind == ViewKind::Flat,
                           (int64_t)positions.size(), v.len));
            return Path::Open;
          }
          for (size_t k = 0; k < positions.size(); ++k)
            emit(Program::MOV, dst.reg + (int)positions[k], v.reg + (int)k);
          return Path::Open;
        }
        for (const auto& ix : s.lhs_idx)
          if (ix.kind != mir::Expr::IndexSingle)
            bail("assignment index form for " + s.lhs);
        if (!is_scalar(v)) bail("element assignment from a container");
        int64_t flat = 0;
        if (dst.kind == ViewKind::Matrix) {
          if (s.lhs_idx.size() != 2)
            bail("matrix assignment index form for " + s.lhs);
          const long i = cint(s.lhs_idx[0].args[0]);
          const long j = cint(s.lhs_idx[1].args[0]);
          if (i < 1 || i > dst.rows || j < 1 || j > dst.cols)
            bail("assignment index range for " + s.lhs);
          flat = (j - 1) * dst.rows + i - 1;
        } else {
          if (s.lhs_idx.size() != 1) bail("assignment index form for " + s.lhs);
          const long ix = cint(s.lhs_idx[0].args[0]);
          if (ix < 1 || ix > dst.len)
            bail("assignment index range for " + s.lhs);
          flat = ix - 1;
        }
        emit(Program::MOV, dst.reg + (int)flat, v.reg);
        if (int_array_names.count(s.lhs)) {
          long value = 0;
          auto known = known_int_arrays.find(s.lhs);
          if (fold_is_certain(s.lhs) && !given(s) && try_cint(s.rhs, &value) &&
              known != known_int_arrays.end() && flat >= 0 &&
              (size_t)flat < known->second.size())
            known->second[(size_t)flat] = value;
          else
            known_int_arrays.erase(s.lhs);
        }
        return Path::Open;
      }
      case mir::Stmt::Return: {
        bool runtime_exit =
            branch_depth || structured_while_depth > return_while_base;
        // A pending break or continue can bypass an apparently top-level
        // return in an unrolled loop. Finish and patch that loop before the
        // function's exit, rather than unwinding compilation immediately.
        if (return_frame)
          for (size_t i = return_frame->loop_base; i < loops.size(); ++i)
            runtime_exit = runtime_exit || !loops[i].breaks.empty() ||
                           !loops[i].continues.empty();
        if (!s.fn_args.empty()) return compile_multi_return(s, runtime_exit);
        if (return_frame && (runtime_exit || return_frame->has_result)) {
          emit_function_return(s.has_init ? expr(s.rhs) : Range{0, 0});
          return Path::Ended;
        }
        if (!return_frame) bail("return without a function scope");
        return_frame->result = s.has_init ? expr(s.rhs) : Range{0, 0};
        return Path::Returned;
      }
      case mir::Stmt::Break:
        if (loops.empty()) bail("break outside a loop");
        if (branch_depth > loops.back().branch_depth ||
            loops.back().structured) {
          loops.back().breaks.push_back(emit(Program::JMP, 0));
          return Path::Open;
        }
        throw CompileBreak{};
      case mir::Stmt::Continue:
        if (loops.empty()) bail("continue outside a loop");
        if (branch_depth > loops.back().branch_depth ||
            loops.back().structured) {
          loops.back().continues.push_back(emit(Program::JMP, 0));
          return Path::Open;
        }
        throw CompileContinue{};
      case mir::Stmt::For: {
        known_reals.erase(s.loopvar);
        long lo, hi;
        std::set<std::string> written;
        for (const auto& child : s.body) assigned_names(child, &written);
        const bool invariant_upper = invariant_for_bound(s.upper, written);
        if (!invariant_upper || !try_cint(s.lower, &lo) ||
            !try_cint(s.upper, &hi)) {
          // Every integer operation in the repeated body must have a typed
          // implementation; double storage alone does not prove this.
          require_runtime_integer_contract();
          // Evaluate and snapshot the lower bound exactly once.
          // In particular, the lower bound may be a variable assigned by the
          // body, so it cannot remain an aliased Range.
          const Range lower = expr(s.lower);
          if (!is_scalar(lower)) bail("for lower bound is not scalar");
          const int index = alloc(1);
          emit(Program::MOV, index, lower.reg);
          for (const auto& name : written) reify_loop_write(name);
          ints.erase(s.loopvar);
          reals[s.loopvar] = Range{index, 1};
          const int one = konst(1);
          const int condition = alloc(1);
          // The bound runs again on every trip, so a local of a function it
          // calls has to be reset there like a local of the body.
          structured_while_seen = true;
          ++structured_while_depth;
          const int head = (int)p.code.size();
          const Range upper = expr(s.upper);
          if (!is_scalar(upper)) bail("for upper bound is not scalar");
          emit(Program::LE, condition, index, upper.reg);
          const int empty = emit(Program::JZ, 0, condition);
          const int body_head = (int)p.code.size();
          loops.push_back({});
          loops.back().structured = true;
          ++runtime_for_depth;
          // Other paths may still continue or break; patch them below.
          (void)compile_body(s.body);
          --runtime_for_depth;
          --structured_while_depth;
          for (int jump : loops.back().continues)
            p.code[(size_t)jump].dst = (int)p.code.size();
          // The private double counter represents INT_MAX + 1 exactly; no
          // out-of-range loop variable reaches a body with an int upper bound.
          // Proven invariant bounds keep the cheaper counted-loop backedge.
          int done = -1;
          if (invariant_upper) {
            emit(Program::LT, condition, index, upper.reg);
            done = emit(Program::JZ, 0, condition);
          }
          emit(Program::ADD, index, index, one);
          emit(Program::JMP, invariant_upper ? body_head : head);
          const int end = (int)p.code.size();
          p.code[(size_t)empty].dst = end;
          if (done >= 0) p.code[(size_t)done].dst = end;
          for (int jump : loops.back().breaks) p.code[(size_t)jump].dst = end;
          loops.pop_back();
          reals.erase(s.loopvar);
          int_decl_at.erase(s.loopvar);
          return Path::Open;
        }
        loops.push_back({});
        loops.back().branch_depth = branch_depth;
        bool broken = false;
        bool returned = false;
        bool returned_directly = false;
        for (int64_t v = lo; v <= hi; ++v) {
          Path path = Path::Open;
          ints[s.loopvar] = {static_cast<long>(v)};
          int_decl_at[s.loopvar] = {branch_depth, loops.size()};
          try {
            path = compile_body(s.body);
          } catch (CompileContinue&) {
          } catch (CompileBreak&) {
            // A runtime continue earlier in this trip skips the break, so
            // the later trips stay reachable and the break becomes a jump.
            if (loops.back().continues.empty())
              broken = true;
            else
              loops.back().breaks.push_back(emit(Program::JMP, 0));
          }
          if (path == Path::Returned) {
            returned_directly = true;
            break;
          }
          const bool path_returned = path == Path::Ended;
          // A continue can reach another unrolled trip; a break reaches the
          // statements after the loop. Only an all-return iteration ends
          // the enclosing lexical path.
          if (path_returned && loops.back().continues.empty()) {
            returned = loops.back().breaks.empty();
            break;
          }
          for (int jump : loops.back().continues)
            p.code[(size_t)jump].dst = (int)p.code.size();
          loops.back().continues.clear();
          if (broken) break;
        }
        ints.erase(s.loopvar);
        int_decl_at.erase(s.loopvar);
        for (int jump : loops.back().breaks)
          p.code[(size_t)jump].dst = (int)p.code.size();
        loops.pop_back();
        if (returned_directly) return Path::Returned;
        return returned ? Path::Ended : Path::Open;
      }
      case mir::Stmt::While: {
        // The register program already has JZ/JMP and is replayed under
        // autodiff for an island.  Use those instructions directly so a
        // finite data loop and a parameter-sensitive recurrence have their
        // actual trip count, with no arbitrary lowering-time cap.
        std::set<std::string> written;
        for (const auto& child : s.body) assigned_names(child, &written);
        for (const std::string& name : written) reify_loop_write(name);

        // The condition runs again on every trip, like the body.
        structured_while_seen = true;
        ++structured_while_depth;
        const int head = (int)p.code.size();
        std::vector<int> exits;
        cond_jump(s.cond, false, &exits);
        loops.push_back({});
        loops.back().structured = true;
        const bool body_returned = compile_body(s.body) == Path::Ended;
        --structured_while_depth;
        for (int jump : loops.back().continues) p.code[(size_t)jump].dst = head;
        if (!body_returned) emit(Program::JMP, head);
        patch_jumps(exits);
        for (int jump : loops.back().breaks)
          p.code[(size_t)jump].dst = (int)p.code.size();
        loops.pop_back();
        return Path::Open;
      }
      case mir::Stmt::IfElse: {
        long c;
        if (try_cint(s.cond, &c)) {
          if (c != 0 && !s.body.empty()) return compile_stmt(s.body[0]);
          if (c == 0 && s.body.size() > 1) return compile_stmt(s.body[1]);
          return Path::Open;
        }
        std::vector<int> to_else;
        cond_jump(s.cond, false, &to_else);
        ++branch_depth;
        const bool then_exits =
            !s.body.empty() && compile_stmt(s.body[0]) == Path::Ended;
        bool else_exits = false;
        if (s.body.size() > 1) {
          const int jmp = then_exits ? -1 : emit(Program::JMP, 0);
          patch_jumps(to_else);
          else_exits = compile_stmt(s.body[1]) == Path::Ended;
          if (jmp >= 0) p.code[(size_t)jmp].dst = (int)p.code.size();
        } else {
          patch_jumps(to_else);
        }
        --branch_depth;
        return then_exits && else_exits ? Path::Ended : Path::Open;
      }
      case mir::Stmt::Block:
      case mir::Stmt::SList:
        return compile_body(s.body);
      case mir::Stmt::TargetPE: {
        // The region's own running total. The caller decides what it is
        // (the ODE side never sees one; lowering makes it a target term),
        // so all this does is accumulate.
        if (target_reg < 0) bail("target += is not available in this region");
        const Range v = expr(s.target);
        // `target += e` for a container adds `sum(e)`. Accumulating the
        // elements in ascending order is that sum, and it is the order
        // OP_SUM_VEC uses on the graph side, so the two paths agree to the
        // bit. A scalar is the one-element case of the same loop.
        for (int k = 0; k < v.len; ++k)
          emit(Program::ADD, target_reg, target_reg, v.reg + k);
        return Path::Open;
      }
      case mir::Stmt::NRFunApp:
        if (mir::internal_name(s) == "FnValidateSize") return Path::Open;
        if (const auto action = message_action(mir::internal_name(s))) {
          Program::Message message;
          std::vector<MessageShape> shapes;
          message.spec = lower_message_arguments(
              s.fn_args, s.print_layout, [&](const mir::Expr& argument) {
                const Range value = expr(argument);
                message.value_reg.push_back(value.reg);
                message.value_len.push_back(value.len);
                MessageShape shape;
                shape.integer = message_integer(argument);
                if (value.kind == ViewKind::Array) {
                  shape.dims = value.dims;
                  shape.matrix_leaf = value.leaf == ViewKind::Matrix;
                } else if (value.kind == ViewKind::Matrix) {
                  shape.dims = {value.rows, value.cols};
                  shape.matrix_leaf = true;
                } else if (!message_scalar(argument)) {
                  shape.dims = {value.len};
                }
                shapes.push_back(std::move(shape));
              });
          message.spec.error = message_error(*action);
          message.spec.shapes = std::move(shapes);
          p.messages.push_back(std::move(message));
          emit(*action != MessageAction::Print ? Program::REJECT
                                               : Program::PRINT,
               0, (int)p.messages.size() - 1);
          return Path::Open;
        }
        if (const auto call = mir::user_defined_statement_call(s)) {
          auto it = funs.find(mir::function_key(*call));
          if (it == funs.end()) bail("unknown function " + call->name);
          (void)inline_call(*it->second, inline_arguments(call->args), true,
                            propto_ctx && mir::inherits_propto(*call));
          return Path::Open;
        }
        bail("statement function " + s.fn_name +
             " requires the MIR interpreter");
      case mir::Stmt::MultiAssign:
        return compile_multi_assign(s);
      case mir::Stmt::Skip:
        return Path::Open;
      default:
        bail("statement");
    }
  }

  Range inline_call(const mir::FunDef& f, const std::vector<InlineArg>& args,
                    bool void_call = false, bool propto = true,
                    std::vector<Range>* results = nullptr) {
    if (args.size() != f.arg_names.size()) bail("function argument mismatch");
    if ((f.results.size() > 1) != (results != nullptr))
      bail("a call of " + f.name + " with the wrong number of results");
    if (++inline_depth > 32) {
      --inline_depth;
      bail("function inlining too deep");
    }
    // Callee scope: move the caller's bindings aside, install the
    // parameters, and move them back afterwards. Registers are never
    // reused, so nothing aliases.
    auto saved_reals = std::exchange(reals, {});
    auto saved_ints = std::exchange(ints, {});
    auto saved_known_reals = std::exchange(known_reals, {});
    auto saved_known_int_arrays = std::exchange(known_int_arrays, {});
    auto saved_known_int_array_dims = std::exchange(known_int_array_dims, {});
    auto saved_int_array_names = std::exchange(int_array_names, {});
    auto saved_deferred_shapes = std::exchange(deferred_shapes, {});
    auto saved_region_sized = std::exchange(region_sized, {});
    auto saved_int_decl_at = std::exchange(int_decl_at, {});
    auto saved_int_reset_at = std::exchange(int_reset_at, {});
    auto saved_extern_bound = std::exchange(extern_bound, {});
    auto saved_call_types = call_types;
    const bool saved_propto_ctx = propto_ctx;
    const int saved_branch_depth = branch_depth;
    const int saved_return_while_base = return_while_base;
    const size_t saved_loop_count = loops.size();
    return_while_base = structured_while_depth;
    call_types = CallTypes{};
    call_types->locals = false;
    for (size_t k = 0; k < args.size(); ++k) {
      const bool real_autodiff = !args[k].is_const_int && args[k].autodiff;
      call_types->formals[f.arg_names[k]] = real_autodiff;
      call_types->locals = call_types->locals || real_autodiff;
    }
    propto_ctx = propto;
    branch_depth = 0;
    inline_stack.push_back(f.name);
    std::set<std::string> assigned;
    for (const auto& statement : f.body) assigned_names(statement, &assigned);
    for (size_t k = 0; k < f.arg_names.size(); ++k) {
      if (args[k].is_const_int) {
        if (args[k].int_dims.empty()) {
          if (runtime_for_depth && assigned.count(f.arg_names[k]) &&
              args[k].ints.size() == 1) {
            const int reg = konst(static_cast<double>(args[k].ints[0]));
            reals[f.arg_names[k]] = Range{reg, 1};
          } else {
            ints[f.arg_names[k]] = args[k].ints;
            if (args[k].ints.size() == 1) mark_int_reset(f.arg_names[k]);
          }
        } else {
          known_int_arrays[f.arg_names[k]] = args[k].ints;
          known_int_array_dims[f.arg_names[k]] = args[k].int_dims;
          int_array_names.insert(f.arg_names[k]);
        }
        // The body starts here, inside whatever loop the call sits in.
        int_decl_at[f.arg_names[k]] = {0, loops.size()};
      } else {
        reals[f.arg_names[k]] = args[k].real;
        if (args[k].is_const_real && !assigned.count(f.arg_names[k]))
          known_reals[f.arg_names[k]] = args[k].const_real;
      }
    }
    Range out{0, 0};
    try {
      out = function_body(f.body, void_call, results);
    } catch (...) {
      inline_stack.pop_back();
      reals = std::move(saved_reals);
      ints = std::move(saved_ints);
      known_reals = std::move(saved_known_reals);
      known_int_arrays = std::move(saved_known_int_arrays);
      known_int_array_dims = std::move(saved_known_int_array_dims);
      int_array_names = std::move(saved_int_array_names);
      deferred_shapes = std::move(saved_deferred_shapes);
      region_sized = std::move(saved_region_sized);
      int_decl_at = std::move(saved_int_decl_at);
      int_reset_at = std::move(saved_int_reset_at);
      extern_bound = std::move(saved_extern_bound);
      call_types = std::move(saved_call_types);
      propto_ctx = saved_propto_ctx;
      branch_depth = saved_branch_depth;
      return_while_base = saved_return_while_base;
      loops.resize(saved_loop_count);
      --inline_depth;
      throw;
    }
    inline_stack.pop_back();
    reals = std::move(saved_reals);
    ints = std::move(saved_ints);
    known_reals = std::move(saved_known_reals);
    known_int_arrays = std::move(saved_known_int_arrays);
    known_int_array_dims = std::move(saved_known_int_array_dims);
    int_array_names = std::move(saved_int_array_names);
    deferred_shapes = std::move(saved_deferred_shapes);
    region_sized = std::move(saved_region_sized);
    int_decl_at = std::move(saved_int_decl_at);
    int_reset_at = std::move(saved_int_reset_at);
    extern_bound = std::move(saved_extern_bound);
    call_types = std::move(saved_call_types);
    propto_ctx = saved_propto_ctx;
    branch_depth = saved_branch_depth;
    return_while_base = saved_return_while_base;
    loops.resize(saved_loop_count);
    --inline_depth;
    return out;
  }
};

}  // namespace stanli

#endif
