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
void check_program_integer_contract(
    const std::vector<stanli::mir::Stmt>& body,
    const std::map<std::string, const stanli::mir::FunDef*>& functions,
    std::set<const stanli::mir::FunDef*>& visited);

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
  void mark_int_reset(const std::string& name);

  static void assigned_names(const mir::Stmt& s, std::set<std::string>* names);

  // Unrolling may capture an upper bound only when reevaluation cannot
  // change its value or produce effects. Other bounds run at the loop head.
  bool invariant_for_bound(const mir::Expr& e,
                           const std::set<std::string>& written);

  // Move an already-declared scalar integer into a register before compiling
  // a structured while which writes it.  Keeping it in `ints` would fold the
  // first assignment into every later use and turn a runtime recurrence back
  // into a compile-time unroll.
  void reify_written_int(const std::string& name);

  void reify_loop_write(const std::string& name);

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

  void require_runtime_integer_contract();

  void compile_loop_body(const mir::Stmt& loop, int index);

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

  Path compile_body(const std::vector<mir::Stmt>& body);

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
  Range copied_if_overlapping(const Range& v, const Range& dst);

  void emit_range_reject(const RangeSelection& range, SelectorSite site,
                         bool upfrom, bool assign, int64_t extent);

  // `b[lo:hi]` or `b[lo:]` on a vector, row vector or array of scalars: a
  // view of the base's registers.
  Range one_dimensional_range(const Range& b, const mir::Expr& e);

  // Stan checks an assignment's sizes when the statement runs, and the
  // statement may sit in a branch that never runs, so a mismatch compiles
  // to a run-time rejection rather than a refusal. A destination whose size
  // the region chose has no single size, so stanli refuses it instead.
  void refuse_second_size(const std::string& name);

  void emit_assign_size_reject(const std::string& name,
                               const std::string& message);

  static std::vector<int64_t> assign_dims(const Range& r);

  // stan::model::assign's message for a whole-variable assignment whose
  // sizes differ, or "" when the two views are not comparable.
  static std::string whole_assign_size_message(const std::string& name,
                                               const Range& dst,
                                               const Range& v);

  // Stan Math's message for a one-index assignment whose selection and
  // right-hand side differ in size. `array` selects the std::vector rules.
  static std::string selected_assign_size_message(const std::string& name,
                                                  const mir::Expr& index,
                                                  bool array, int64_t want,
                                                  int64_t got);

  // The statements a call returning several values needs, kept out of
  // compile_stmt: a register-program refusal unwinds through every
  // compile_stmt frame, and each statement compiled inline there makes every
  // such unwind slower.
  [[gnu::noinline]] Path compile_multi_return(const mir::Stmt& s,
                                              bool runtime_exit);

  [[gnu::noinline]] Path compile_multi_assign(const mir::Stmt& s);

  // A statement whose compile-time integer raises a math error, such as a
  // division by zero, raises it when the statement runs. The statement may
  // sit in a branch that never runs, so it compiles to a run-time rejection.
  Path compile_stmt(const mir::Stmt& s);

  Path compile_stmt_folded(const mir::Stmt& s);

  Range inline_call(const mir::FunDef& f, const std::vector<InlineArg>& args,
                    bool void_call = false, bool propto = true,
                    std::vector<Range>* results = nullptr);
};

}  // namespace stanli

#endif
