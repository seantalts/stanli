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
  bool autodiff(const mir::Expr& e) const;
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
                                                     const Range& value);

  // Rewrite a Range's geometry to a resolver-reported result shape; `reg`
  // and `len` stay the caller's.
  static Range shaped(Range out, const BuiltinArgumentShape& shape);

  BuiltinLayout resolved_builtin_layout(const mir::Expr& e,
                                        const BuiltinSpec& spec,
                                        const std::vector<Range>& values);

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
  InlineArg inline_argument(const mir::Expr& e);

  std::vector<InlineArg> inline_arguments(const std::vector<mir::Expr>& exprs,
                                          size_t begin = 0);

  void require_positive(const Range& value, const std::string& name);

  // Serial reduce_sum is exactly one call over the complete slice, matching
  // Stan Math without STAN_THREADS.  Only family-specific argument synthesis
  // lives here; callback lookup and UDF binding are shared with every other
  // backend and ordinary inline calls respectively.
  Range reduce_sum_call(const mir::Expr& e);

  // map_rect's job count and every input shape are fixed when the model is
  // lowered, so the serial implementation needs no retained runtime
  // algorithm: compile one ordinary callback invocation per job and
  // concatenate their vector results. This is also the exact execution
  // order of Stan Math's non-threaded map_rect path.
  Range map_rect_call(const mir::Expr& e);

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
                       BuiltinMatrixMap* map_out = nullptr);

  Range kernel_call(uint16_t opcode, const std::vector<Range>& args, Range out,
                    uint8_t variant = 0, uint8_t input_adjoint_mask = 0x3f,
                    std::vector<int> idata = {},
                    std::shared_ptr<void> udata = {},
                    const std::string& name = "function");

  // Every RNG spelling the region can carry, which is exactly the set the
  // graph's OP_RNG kernel speaks.
  static bool rng_call_name(const std::string& name);

  // A draw inside a runtime-control region, spelled as one Program::CALL on
  // the graph's own OP_RNG kernel rather than transcribed family by family.
  // The stream, the stan-math call and the argument contract are then the
  // kernel's, so the region and the graph cannot disagree about what a draw
  // is or where in the stream it lands. Generated quantities never runs a
  // gradient, and OP_RNG consequently needs no backward implementation.
  Range rng_call(const mir::Expr& e);

  // Program-native instructions cover the hot elementary subset. Everything
  // else in the shared regular-function registry reaches the exact same graph
  // kernel through CALL, so adding an optable entry also makes it available in
  // parameter-dependent control flow without another name table here.
  Range builtin_kernel_call(const mir::Expr& e, const BuiltinSpec& spec);

  static Program::Code integer_instruction(Program::Code code);

  static Program::Code native_math_code(uint16_t opcode);

  static std::optional<Program::Code> native_builtin_code(uint16_t opcode);

  Range native_builtin_call(const mir::Expr& e, const BuiltinSpec& spec,
                            Program::Code native_code);

  // Probability functions use their shared registry policy. Graph-backed
  // descriptors marshal register ranges and integer payloads into KernelCtx;
  // all-integer descriptors evaluate once as constants, with the same Stan
  // Math implementation used by graph lowering and MIR interpretation.
  Range density_call(const mir::Expr& e, const DensitySpec& spec);

  Range matrix_gram(const Range& m, bool transpose_first);

  Range inverse_transform_call(const mir::Expr& e);

  Range transform_call(const mir::Expr& e, const CallableTransformSpec& spec);

  Range fun(const mir::Expr& e);

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
  void emit_function_return(const std::vector<Range>& values, bool several);

  Range function_body(const std::vector<mir::Stmt>& body,
                      bool void_call = false,
                      std::vector<Range>* results = nullptr);

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
                    std::vector<Range>* results = nullptr);
};

}  // namespace stanli

#endif
