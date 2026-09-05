// MIR -> Program: the shared front end.
//
// Two callers compile MIR into the register machine. An ODE right-hand
// side has to stay callable at runtime because the integrator picks the
// times (ode_prog.hpp). A region whose control flow depends on a
// parameter has to become a program because it cannot become graph ops
// at all: `if (theta > 0)` has no op-graph form, and until this existed
// it was a compile error (lower.cpp).
//
// The shape of the problem is what keeps this small: every size and loop
// bound is known at compile time. Most indices and integers are too. The
// one runtime-integer surface is a checked scalar read from a flat array;
// generated-quantities Viterbi backtracking needs exactly that operation.
//
// Names the compiler does not know are the one thing that differs
// between callers. The ODE side knows them all up front (t, y, theta,
// x_r, x_i). Lowering does not: a region reads model-block variables
// that already live in graph slots, so it hands over `bind_extern`,
// which allocates registers for such a name and records it as a live-in.
#ifndef STANLI_MIR_PROG_HPP
#define STANLI_MIR_PROG_HPP

#include <stanli/builtin_registry.hpp>
#include <stanli/mir_message.hpp>
#include <stanli/mir.hpp>
#include <stanli/density_registry.hpp>
#include <stanli/function_registry.hpp>
#include <stanli/function_view_shape.hpp>
#include <stanli/optable.hpp>
#include <stanli/program.hpp>
#include <stanli/rng_family.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <set>
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
};

struct Bail {
  std::string why;
};

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
  int branch_depth = 0;  // inside a branch on a runtime value
  // A while is a genuinely runtime loop: its condition and state must be
  // evaluated again on every trip, rather than folded once while the program
  // is being built.  Scalar integers written by such a loop consequently
  // live in ordinary double registers (their producers still preserve Stan's
  // integer-valued operations where they are supported below).
  int structured_while_depth = 0;
  bool structured_while_seen = false;
  int inline_depth = 0;
  std::vector<std::string> inline_stack;
  struct LoopFrame {
    std::vector<int> breaks;
    std::vector<int> continues;
    bool structured = false;
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

  static void assigned_names(const mir::Stmt& s, std::set<std::string>* names) {
    if (s.kind == mir::Stmt::Assignment) names->insert(s.lhs);
    for (const auto& child : s.body) assigned_names(child, names);
  }

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
    hoisted_int_initializers.push_back(const_instr(r, &value, 1));
    reals[name] = Range{r, 1};
    ints.erase(it);
    int_decl_at.erase(name);
  }

  // Registers are never recycled. Right-hand sides are a few lines over a
  // handful of states, so the count stays in the dozens; the cap is a
  // backstop against a pathological unroll, and trips into the interpreter
  // rather than into a huge allocation.
  static constexpr int kMaxRegs = 1 << 16;

  [[noreturn]] void bail(const std::string& why) { throw Bail{why}; }

  int alloc(int n) {
    if (n < 0 || n > kMaxRegs - p.n_regs)
      bail("right-hand side needs too many registers");
    const int r = p.n_regs;
    p.n_regs += n;
    return r;
  }

  int emit(Program::Code c, int dst, int a = 0, int b = 0, int cc = 0) {
    p.code.push_back(Program::Instr{c, dst, a, b, cc, 0});
    return (int)p.code.size() - 1;
  }

  // Constants live in the pool, not in the instruction. Equal values share
  // an entry: `declare` zeroes every register it allocates, so a program
  // over a few arrays would otherwise carry hundreds of copies of 0.
  int pool_at(const double* v, int n) {
    for (size_t s = 0; s + (size_t)n <= p.pool.size(); ++s)
      if (std::equal(v, v + n, p.pool.begin() + (long)s)) return (int)s;
    const int at = (int)p.pool.size();
    p.pool.insert(p.pool.end(), v, v + n);
    return at;
  }

  // dst[0..n) = the given values, as one instruction.
  Program::Instr const_instr(int dst, const double* v, int n) {
    return Program::Instr{
        n == 1 ? Program::CONST : Program::CONSTR, dst, pool_at(v, n), 0, 0, n};
  }

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
  static bool is_shape_query(const mir::Expr& e) {
    if (e.args.size() != 1) return false;
    if (e.name == "FnLength") return true;
    const BuiltinSpec* query =
        shaped_builtin_spec(e.name, 1, BuiltinShapePolicy::ShapeQuery);
    return query != nullptr &&
           query->shape_query != BuiltinShapeQueryKind::Dims;
  }

  // The registered names answer through the shared resolver, matching the
  // graph's compile-time evaluator; FnLength keeps the size rule those
  // evaluators have always applied to it.
  long shape_query(const mir::Expr& e, Range v) {
    if (v.kind == ViewKind::Array && v.dims.empty()) v.dims = {v.len};
    const BuiltinSpec* query =
        shaped_builtin_spec(e.name == "FnLength" ? "size" : e.name, 1,
                            BuiltinShapePolicy::ShapeQuery);
    try {
      return (long)builtin_shape_query(*query,
                                       builtin_argument_shape(e.args[0], v))
          .front();
    } catch (const std::invalid_argument& error) {
      bail(e.name + ": " + std::string(error.what()));
    }
  }

  int64_t checked_shape_product(const std::vector<int64_t>& dims,
                                const std::string& what) {
    int64_t product = 1;
    for (int64_t extent : dims) {
      if (extent < 0) bail(what + " has a negative logical extent");
      if (product != 0 && extent != 0 && product > (int64_t)kMaxRegs / extent)
        bail(what + " has too many logical elements");
      product *= extent;
    }
    return product;
  }

  std::vector<int64_t> validated_int_array_dims(const std::vector<long>& values,
                                                std::vector<int64_t> dims,
                                                const std::string& what) {
    if (values.size() > (size_t)kMaxRegs)
      bail(what + " has too many stored elements");
    if (dims.empty()) dims = {(int64_t)values.size()};
    if (checked_shape_product(dims, what) != (int64_t)values.size())
      bail(what + " storage and logical shape disagree");
    return dims;
  }

  mir::Expr int_array_literal(const std::vector<long>& values,
                              const std::vector<int64_t>& dims) {
    const std::vector<int64_t> shape =
        validated_int_array_dims(values, dims, "integer array literal");
    std::vector<int64_t> stride(shape.size(), 1);
    for (size_t d = 1; d < shape.size(); ++d)
      stride[d] = stride[d - 1] * shape[d - 1];
    std::function<mir::Expr(size_t, int64_t)> make = [&](size_t d,
                                                         int64_t offset) {
      if (d == shape.size()) {
        mir::Expr cell;
        cell.kind = mir::Expr::LitInt;
        cell.lit_i = values.at((size_t)offset);
        cell.type_ = "UInt";
        cell.unsized = {0, mir::UnsizedLeaf::Int};
        cell.data_only = true;
        return cell;
      }
      mir::Expr literal;
      literal.kind = mir::Expr::FunApp;
      literal.fn_lib = mir::Expr::Lib::Internal;
      literal.name = "FnMakeArray";
      literal.type_ = "UArray";
      literal.unsized = {static_cast<uint8_t>(shape.size() - d),
                         mir::UnsizedLeaf::Int};
      literal.data_only = true;
      literal.args.reserve((size_t)shape[d]);
      for (int64_t k = 0; k < shape[d]; ++k)
        literal.args.push_back(make(d + 1, offset + k * stride[d]));
      return literal;
    };
    return make(0, 0);
  }

  void literalize_external_ints(mir::Expr* e) {
    if (e->kind == mir::Expr::Var) {
      auto scalar = ints.find(e->name);
      if (scalar != ints.end() && scalar->second.size() == 1) {
        const long value = scalar->second[0];
        *e = mir::Expr{};
        e->kind = mir::Expr::LitInt;
        e->lit_i = value;
        e->type_ = "UInt";
        e->unsized = {0, mir::UnsizedLeaf::Int};
        e->data_only = true;
        return;
      }
      auto array = known_int_arrays.find(e->name);
      auto dims = known_int_array_dims.find(e->name);
      if (array != known_int_arrays.end()) {
        *e = int_array_literal(
            array->second,
            dims == known_int_array_dims.end()
                ? std::vector<int64_t>{(int64_t)array->second.size()}
                : dims->second);
        return;
      }
    }
    for (mir::Expr& arg : e->args) literalize_external_ints(&arg);
  }

  bool external_int_array(const mir::Expr& e, std::vector<long>* values,
                          std::vector<int64_t>* dims) {
    if (!extern_ints) return false;
    if (e.kind == mir::Expr::Var) {
      auto known = known_int_arrays.find(e.name);
      if (known != known_int_arrays.end()) {
        auto shape = known_int_array_dims.find(e.name);
        *values = known->second;
        *dims = validated_int_array_dims(
            *values,
            shape == known_int_array_dims.end()
                ? std::vector<int64_t>{(int64_t)values->size()}
                : shape->second,
            "external integer array " + e.name);
        return true;
      }
    }
    mir::Expr literal = e;
    literalize_external_ints(&literal);
    if (!extern_ints(literal, values, dims)) return false;
    *dims = validated_int_array_dims(*values, std::move(*dims),
                                     "external integer array expression");
    return true;
  }

  // The view of a named value, without building it: a shape query in an
  // integer position (a declared extent, a loop bound, an index) may not
  // emit, because try_cint swallows a Bail and a half-built branch would
  // leave an unpatched jump in the program behind it. Only `len`, `kind`,
  // `rows`, `cols` and `dims` of the result mean anything; `reg` is not a
  // register, because no value was built.
  bool static_view(const mir::Expr& e, Range* out) {
    if (e.data_only && e.unsized.depth != 0 &&
        e.unsized.leaf == mir::UnsizedLeaf::Int && extern_ints) {
      std::vector<long> values;
      std::vector<int64_t> dims;
      if (external_int_array(e, &values, &dims)) {
        Range r{0, (int)values.size()};
        r.kind = ViewKind::Array;
        r.dims = std::move(dims);
        *out = std::move(r);
        return true;
      }
    }
    // A literal array, which stanc's inliner substitutes for an argument:
    // `size(when)` inside an inlined function arrives as `size({0})`.
    if (e.kind == mir::Expr::FunApp &&
        (e.name == "FnMakeArray" || e.name == "FnMakeRowVec")) {
      int64_t len = 0;
      for (const auto& a : e.args) {
        Range part;
        if (!static_view(a, &part)) return false;
        len += part.len;
      }
      Range r{0, (int)len};
      if (e.name == "FnMakeRowVec") {
        r.kind = ViewKind::RowVector;
      } else {
        r.kind = ViewKind::Array;
        r.dims = {(int64_t)e.args.size()};
      }
      *out = r;
      return true;
    }
    if (e.kind == mir::Expr::LitInt || e.kind == mir::Expr::LitReal) {
      *out = Range{0, 1};
      return true;
    }
    // A registered shape-policy call: the shared resolvers report result
    // geometry as a pure function of operand shapes, so the extent of, say,
    // `cols(to_matrix(v, 6, 1))` -- the form stanc's inliner leaves behind
    // for a matrix argument -- is known without building the value. A
    // resolver rejection is not swallowed semantically: the value
    // expression raises it wherever it is really evaluated, while a
    // shape-only use keeps the caller's ordinary diagnostics.
    if (e.kind == mir::Expr::FunApp) {
      if (const BuiltinSpec* slice = shaped_builtin_spec(
              e.name, e.args.size(), BuiltinShapePolicy::SliceView)) {
        Range a;
        if (!static_view(e.args[0], &a)) return false;
        const bool append = builtin_slice_is_append(slice->slice);
        Range b;
        if (append && !static_view(e.args[1], &b)) return false;
        std::vector<int64_t> indexes;
        indexes.reserve(e.args.size() - 1);
        for (size_t k = 1; !append && k < e.args.size(); ++k) {
          long index = 0;
          if (!try_cint(e.args[k], &index)) return false;
          indexes.push_back(index);
        }
        try {
          const BuiltinSliceMap map =
              append ? builtin_append_map(*slice,
                                          builtin_argument_shape(e.args[0], a),
                                          builtin_argument_shape(e.args[1], b),
                                          SliceStorageOrder::OuterMajor)
                     : builtin_slice_map(
                           *slice, builtin_argument_shape(e.args[0], a),
                           indexes, SliceStorageOrder::OuterMajor);
          *out = shaped(Range{0, (int)map.count}, map.result);
          return true;
        } catch (const std::exception&) {
          return false;
        }
      }
      if (const BuiltinSpec* grouped = shaped_builtin_spec(
              e.name, e.args.size(), BuiltinShapePolicy::GroupedReduction)) {
        Range a;
        if (!static_view(e.args[0], &a)) return false;
        Range b = a;
        if (grouped->arity == 2 && !static_view(e.args[1], &b)) return false;
        const mir::Expr& rhs = e.args[grouped->arity == 2 ? 1 : 0];
        try {
          const BuiltinGroupedDotMap map = builtin_grouped_dot_map(
              *grouped, builtin_argument_shape(e.args[0], a),
              builtin_argument_shape(rhs, b));
          *out = shaped(Range{0, (int)map.groups}, map.result);
          return true;
        } catch (const std::exception&) {
          return false;
        }
      }
      if (const BuiltinSpec* matrix = shaped_builtin_spec(
              e.name, e.args.size(), BuiltinShapePolicy::MatrixOp)) {
        std::vector<BuiltinArgumentShape> shapes;
        shapes.reserve(matrix->arity);
        for (size_t k = 0; k < e.args.size(); ++k) {
          Range operand;
          if (!static_view(e.args[k], &operand)) return false;
          shapes.push_back(builtin_argument_shape(e.args[k], operand));
        }
        try {
          const BuiltinMatrixMap map = builtin_matrix_map(*matrix, shapes);
          *out = shaped(Range{0, (int)map.result.storage_size}, map.result);
          return true;
        } catch (const std::exception&) {
          return false;
        }
      }
      if (const BuiltinSpec* ctor = shaped_builtin_spec(
              e.name, e.args.size(), BuiltinShapePolicy::Constructor)) {
        std::vector<double> arguments;
        arguments.reserve(e.args.size());
        for (size_t k = 0; k < e.args.size(); ++k) {
          double value = 0.0;
          if (ctor->arguments[k] == BuiltinArgumentKind::Integer) {
            long argument = 0;
            if (!try_cint(e.args[k], &argument)) return false;
            value = (double)argument;
          } else if (!try_creal(e.args[k], &value)) {
            return false;
          }
          arguments.push_back(value);
        }
        try {
          const ConstructorValue built =
              evaluate_constructor_builtin(*ctor, arguments);
          Range r{0, (int)built.values.size()};
          r.kind = function_view_kind(ctor->constructor_container);
          if (r.kind == ViewKind::Matrix) {
            r.rows = built.dimensions[0];
            r.cols = built.dimensions[1];
          } else if (r.kind == ViewKind::Array) {
            r.dims = built.dimensions;
          }
          *out = r;
          return true;
        } catch (const std::exception&) {
          return false;
        }
      }
      return false;
    }
    if (e.kind != mir::Expr::Var) return false;
    if (deferred_shapes.count(e.name)) return false;
    auto rt = reals.find(e.name);
    if (rt != reals.end()) {
      *out = rt->second;
      return true;
    }
    auto known = known_int_arrays.find(e.name);
    if (known != known_int_arrays.end()) {
      Range r{0, (int)known->second.size()};
      r.kind = ViewKind::Array;
      auto dims = known_int_array_dims.find(e.name);
      r.dims = dims == known_int_array_dims.end() ? std::vector<int64_t>{r.len}
                                                  : dims->second;
      *out = r;
      return true;
    }
    auto ii = ints.find(e.name);
    if (ii != ints.end()) {
      Range r{0, (int)ii->second.size()};
      if (e.unsized.depth != 0) {
        r.kind = ViewKind::Array;
        r.dims = {(int64_t)ii->second.size()};
      }
      *out = r;
      return true;
    }
    // An outside name: bind it the way expr() would. The binding is one
    // live-in whether it is the shape that is wanted or the value.
    Range ext;
    if (bind_extern && bind_extern(e.name, &ext)) {
      reals[e.name] = ext;
      extern_bound.insert(e.name);
      *out = ext;
      return true;
    }
    return false;
  }

  // ---- compile-time integers ----------------------------------------------
  // A comparison returns an integer whatever it compares, so the result
  // type does not say whether cint may answer it: `2.5 > 1` is UInt with
  // real operands, and cint reads a real literal by truncation, which
  // would make it false. The operands have to be integers themselves.
  static bool int_operand(const mir::Expr& e) {
    return e.type_ == "UInt" || e.unsized.leaf == mir::UnsizedLeaf::Int;
  }

  double creal(const mir::Expr& e) {
    if (e.data_only && extern_real && e.type_ == "UReal") {
      double value = 0.0;
      if (extern_real(e, &value)) return value;
    }
    switch (e.kind) {
      case mir::Expr::LitInt:
        return static_cast<double>(e.lit_i);
      case mir::Expr::LitReal:
        return e.lit;
      case mir::Expr::Var: {
        auto real = known_reals.find(e.name);
        if (real != known_reals.end()) return real->second;
        auto integer = ints.find(e.name);
        if (integer != ints.end() && integer->second.size() == 1)
          return static_cast<double>(integer->second[0]);
        bail("real " + e.name + " is not known at compile time");
      }
      case mir::Expr::Promotion:
        if (e.args.size() != 1) bail("real promotion form");
        return creal(e.args[0]);
      case mir::Expr::TernaryIf:
        if (e.args.size() != 3) bail("real conditional form");
        return creal(e.args[cint(e.args[0]) != 0 ? 1 : 2]);
      case mir::Expr::FunApp:
        if (const auto value = mir::nullary_constant(e)) return *value;
        if (e.args.size() == 1) {
          if (e.name == "PMinus__" || e.name == "minus")
            return -creal(e.args[0]);
          if (e.name == "PPlus__" || e.name == "plus") return creal(e.args[0]);
        }
        if (e.args.size() == 2) {
          const double lhs = creal(e.args[0]);
          const double rhs = creal(e.args[1]);
          if (e.name == "Plus__" || e.name == "add") return lhs + rhs;
          if (e.name == "Minus__" || e.name == "subtract") return lhs - rhs;
          if (e.name == "Times__" || e.name == "multiply" ||
              e.name == "elt_multiply")
            return lhs * rhs;
          if (e.name == "Divide__" || e.name == "divide" ||
              e.name == "elt_divide")
            return lhs / rhs;
        }
        bail("real function " + e.name + " is not known at compile time");
      default:
        bail("real expression is not known at compile time");
    }
  }

  bool try_creal(const mir::Expr& e, double* out) {
    try {
      *out = creal(e);
      return true;
    } catch (Bail&) {
      return false;
    }
  }

  long cint(const mir::Expr& e) {
    switch (e.kind) {
      case mir::Expr::LitInt:
        return e.lit_i;
      case mir::Expr::LitReal:
        return (long)e.lit;
      case mir::Expr::Var: {
        auto it = ints.find(e.name);
        if (it != ints.end() && it->second.size() == 1) return it->second[0];
        bail("integer " + e.name + " is not known at compile time");
      }
      case mir::Expr::Indexed: {
        if (e.args.size() < 2) bail("integer index form");
        // The same substituted literal, indexed rather than measured.
        if (e.args.size() == 2 && e.args[0].kind == mir::Expr::FunApp &&
            e.args[0].name == "FnMakeArray" &&
            e.args[1].name == "IndexSingle") {
          const long ix = cint(e.args[1].args[0]);
          if (ix < 1 || (size_t)ix > e.args[0].args.size())
            bail("integer index range");
          return cint(e.args[0].args[(size_t)ix - 1]);
        }
        if (e.args[0].kind != mir::Expr::Var) bail("integer index base");
        for (size_t k = 1; k < e.args.size(); ++k)
          if (e.args[k].name != "IndexSingle" || e.args[k].args.size() != 1)
            bail("integer index form");
        auto known = known_int_arrays.find(e.args[0].name);
        if (known != known_int_arrays.end()) {
          auto shape = known_int_array_dims.find(e.args[0].name);
          const std::vector<int64_t> dims =
              shape == known_int_array_dims.end()
                  ? std::vector<int64_t>{(int64_t)known->second.size()}
                  : shape->second;
          if (e.args.size() - 1 != dims.size()) bail("integer index form");
          int64_t flat = 0;
          int64_t stride = 1;
          for (size_t d = 0; d < dims.size(); ++d) {
            const long ix = cint(e.args[d + 1].args[0]);
            if (ix < 1 || ix > dims[d]) bail("integer index range");
            if (ix - 1 != 0 && stride > kMaxRegs / (ix - 1))
              bail("integer index offset overflow");
            const int64_t term = (ix - 1) * stride;
            if (flat > kMaxRegs - term) bail("integer index offset overflow");
            flat += term;
            if (d + 1 != dims.size() && dims[d] != 0 &&
                stride > kMaxRegs / dims[d])
              bail("integer index stride overflow");
            stride *= dims[d];
          }
          if (flat < 0 || (size_t)flat >= known->second.size())
            bail("integer index storage range");
          return known->second[(size_t)flat];
        }
        auto it = ints.find(e.args[0].name);
        if (it != ints.end()) {
          if (e.args.size() != 2) bail("integer index form");
          const long ix = cint(e.args[1].args[0]);
          if (ix < 1 || (size_t)ix > it->second.size())
            bail("integer index range");
          return it->second[(size_t)ix - 1];
        }
        // A read from a data array, at any rank: `ms[ri, 8]` where the
        // region unrolled the loop that produced `ri`. Resolving the
        // indices here and asking for a literal read keeps the array's
        // storage order the one place that already knows it.
        if (extern_int && (extern_bound.count(e.args[0].name) ||
                           !reals.count(e.args[0].name))) {
          mir::Expr literal = e;
          for (size_t k = 1; k < literal.args.size(); ++k) {
            mir::Expr& ix = literal.args[k].args[0];
            const long v = cint(ix);
            ix = mir::Expr{};
            ix.kind = mir::Expr::LitInt;
            ix.lit_i = v;
            ix.type_ = "UInt";
            ix.unsized = {0, mir::UnsizedLeaf::Int};
            ix.data_only = true;
          }
          long v;
          if (extern_int(literal, &v)) return v;
        }
        bail("integer array " + e.args[0].name);
      }
      case mir::Expr::Promotion:
        if (e.args.size() != 1) bail("integer promotion form");
        return cint(e.args[0]);
      case mir::Expr::TernaryIf:
        if (e.args.size() != 3) bail("integer conditional form");
        return cint(e.args[cint(e.args[0]) != 0 ? 1 : 2]);
      case mir::Expr::EAnd:
      case mir::Expr::EOr: {
        // Stan short-circuits these, and so does this: the second operand
        // of a decided `&&` is never evaluated, so it does not have to be
        // a compile-time integer -- or an integer at all.
        if (e.args.size() != 2 || !int_operand(e.args[0]))
          bail("integer logical form");
        const bool lhs = cint(e.args[0]) != 0;
        if (e.kind == mir::Expr::EAnd && !lhs) return 0;
        if (e.kind == mir::Expr::EOr && lhs) return 1;
        if (!int_operand(e.args[1])) bail("integer logical form");
        return cint(e.args[1]) != 0;
      }
      case mir::Expr::FunApp:
        if (const FunctionSpec* function = function_spec(e);
            function != nullptr && function->builtin() != nullptr &&
            function->builtin()->shape == BuiltinShapePolicy::Elementwise &&
            function->result() == FunctionArgumentKind::Integer) {
          const BuiltinSpec* spec = function->builtin();
          if (spec->arity == 1)
            return evaluate_integer_unary_builtin(
                *spec, static_cast<int>(cint(e.args[0])));
          if (spec->arity != 2) bail("integer builtin arity");
          return evaluate_integer_binary_builtin(
              *spec, static_cast<int>(cint(e.args[0])),
              static_cast<int>(cint(e.args[1])));
        }
        if (e.args.size() == 2) {
          // Each operator with the named spelling beside it: on ints the
          // alias is the operator, down to `divide`'s truncation.
          if (e.name == "Plus__" || e.name == "add")
            return cint(e.args[0]) + cint(e.args[1]);
          if (e.name == "Minus__" || e.name == "subtract")
            return cint(e.args[0]) - cint(e.args[1]);
          if (e.name == "Times__" || e.name == "multiply" ||
              e.name == "elt_multiply")
            return cint(e.args[0]) * cint(e.args[1]);
          if (e.name == "IntDivide__" || e.name == "Divide__" ||
              e.name == "divide" || e.name == "elt_divide")
            return cint(e.args[0]) / cint(e.args[1]);
          // The comparisons: on integers each is an integer in its own
          // right, and a `while` condition or an integer local written
          // with one -- `int found = (a[i] == k);` -- is as much a
          // compile-time value as its operands are.
          if (const BuiltinSpec* pred = shaped_builtin_spec(
                  e.name, 2, BuiltinShapePolicy::Predicate)) {
            if (int_operand(e.args[0]) && int_operand(e.args[1]))
              return evaluate_predicate_builtin(*pred, (double)cint(e.args[0]),
                                                (double)cint(e.args[1]));
            double lhs = 0.0, rhs = 0.0;
            if (try_creal(e.args[0], &lhs) && try_creal(e.args[1], &rhs))
              return evaluate_predicate_builtin(*pred, lhs, rhs);
          }
        }
        if (e.args.size() == 1 && e.name == "PMinus__") return -cint(e.args[0]);
        if (e.args.size() == 1) {
          if (const BuiltinSpec* pred = shaped_builtin_spec(
                  e.name, 1, BuiltinShapePolicy::Predicate)) {
            if (int_operand(e.args[0]) &&
                pred->predicate == BuiltinPredicate::Negation)
              return evaluate_predicate_builtin(*pred, (double)cint(e.args[0]));
            // The IEEE classifications (and negation of a real) fold over
            // any compile-time value.
            double value = 0.0;
            if (try_creal(e.args[0], &value))
              return evaluate_predicate_builtin(*pred, value);
          }
        }
        // A declared extent, a loop bound or an index written as a shape
        // query: `matrix[rows(m), cols(m)] out;`, `for (i in 1:rows(m))`.
        // Before this, only a shape query in a real-valued context was
        // answered, and these were refused as unknown integer functions.
        if (is_shape_query(e)) {
          Range v;
          if (static_view(e.args[0], &v)) return shape_query(e, v);
        }
        if (e.args.size() == 1 && e.name == "sum" &&
            e.args[0].unsized.leaf == mir::UnsizedLeaf::Int) {
          long total = 0;
          for (long value : cints(e.args[0])) total += value;
          return total;
        }
        bail("integer function " + e.name +
             (e.args.empty()
                  ? std::string()
                  : " arg-kind=" + std::to_string((int)e.args[0].kind) +
                        " arg-name=" + e.args[0].name +
                        " arg-type=" + e.args[0].type_));
      default:
        bail("integer expression kind=" +
             std::to_string(static_cast<int>(e.kind)) + " name=" + e.name +
             " type=" + e.type_);
    }
  }

  bool try_cint(const mir::Expr& e, long* out) {
    try {
      *out = cint(e);
      return true;
    } catch (Bail&) {
      return false;
    }
  }

  // A data-only integer array used by IndexMulti.  Literal arrays are the
  // form O1 emits for source selectors such as `{3, 1}`.  Named arrays cover
  // both ODE x_i arguments and statement-island data bindings; the latter are
  // read one scalar at a time through extern_int, the callback that already
  // owns their storage-order semantics.
  std::vector<long> cints(const mir::Expr& e) {
    if (e.data_only && e.unsized.depth != 0 &&
        e.unsized.leaf == mir::UnsizedLeaf::Int && extern_ints) {
      std::vector<long> values;
      std::vector<int64_t> dims;
      if (external_int_array(e, &values, &dims)) return values;
    }
    // dims of a statically shaped value is a compile-time integer array.
    if (e.kind == mir::Expr::FunApp && e.args.size() == 1) {
      if (const BuiltinSpec* query =
              shaped_builtin_spec(e.name, 1, BuiltinShapePolicy::ShapeQuery);
          query != nullptr &&
          query->shape_query == BuiltinShapeQueryKind::Dims) {
        Range v;
        if (static_view(e.args[0], &v)) {
          if (v.kind == ViewKind::Array && v.dims.empty()) v.dims = {v.len};
          const std::vector<int64_t> extents =
              builtin_shape_query(*query, builtin_argument_shape(e.args[0], v));
          return std::vector<long>(extents.begin(), extents.end());
        }
      }
    }
    if (e.kind == mir::Expr::Var) {
      auto known = known_int_arrays.find(e.name);
      if (known != known_int_arrays.end()) return known->second;
      auto held = ints.find(e.name);
      if (held != ints.end()) return held->second;

      Range view;
      if (static_view(e, &view) && view.kind == ViewKind::Array &&
          view.len >= 0) {
        std::vector<long> values;
        values.reserve((size_t)view.len);
        const std::vector<int64_t> dims =
            view.dims.empty() ? std::vector<int64_t>{view.len} : view.dims;
        std::vector<int64_t> chosen(dims.size());
        std::function<void(size_t)> gather = [&](size_t d) {
          if (d != dims.size()) {
            for (int64_t i = 1; i <= dims[d]; ++i) {
              chosen[d] = i;
              gather(d + 1);
            }
            return;
          }
          mir::Expr indexed;
          indexed.kind = mir::Expr::Indexed;
          indexed.type_ = "UInt";
          indexed.unsized = {0, mir::UnsizedLeaf::Int};
          indexed.data_only = true;
          indexed.args.push_back(e);
          for (int64_t position : chosen) {
            mir::Expr literal;
            literal.kind = mir::Expr::LitInt;
            literal.lit_i = position;
            literal.type_ = "UInt";
            literal.unsized = {0, mir::UnsizedLeaf::Int};
            literal.data_only = true;
            mir::Expr single;
            single.kind = mir::Expr::FunApp;
            single.name = "IndexSingle";
            single.args.push_back(std::move(literal));
            indexed.args.push_back(std::move(single));
          }
          values.push_back(cint(indexed));
        };
        gather(0);
        return values;
      }
      bail("integer array " + e.name + " is not known at compile time");
    }
    if (e.kind == mir::Expr::FunApp && e.name == "FnMakeArray") {
      std::vector<long> values;
      values.reserve(e.args.size());
      for (const auto& value : e.args) values.push_back(cint(value));
      return values;
    }
    if (e.kind == mir::Expr::FunApp && e.name == "append_array" &&
        e.args.size() == 2) {
      std::vector<long> values = cints(e.args[0]);
      std::vector<long> tail = cints(e.args[1]);
      values.insert(values.end(), tail.begin(), tail.end());
      return values;
    }
    if (e.kind == mir::Expr::Indexed && !e.args.empty() &&
        e.args[0].kind == mir::Expr::Var) {
      Range base;
      if (!static_view(e.args[0], &base) || base.kind != ViewKind::Array)
        bail("integer indexed selector has no static array view");
      const std::vector<int64_t> dims =
          base.dims.empty() ? std::vector<int64_t>{base.len} : base.dims;
      if (e.args.size() - 1 != dims.size())
        bail("integer indexed selector needs every array dimension");
      std::vector<std::vector<int64_t>> positions;
      positions.reserve(dims.size());
      for (size_t d = 0; d < dims.size(); ++d)
        positions.push_back(
            matrix_positions(e.args[d + 1], dims[d], "array selector"));

      std::vector<long> values;
      std::vector<int64_t> chosen(dims.size());
      std::function<void(int)> gather = [&](int d) {
        if (d >= 0) {
          for (int64_t position : positions[(size_t)d]) {
            chosen[(size_t)d] = position + 1;
            gather(d - 1);
          }
          return;
        }
        mir::Expr scalar;
        scalar.kind = mir::Expr::Indexed;
        scalar.type_ = "UInt";
        scalar.unsized = {0, mir::UnsizedLeaf::Int};
        scalar.data_only = true;
        scalar.args.push_back(e.args[0]);
        for (int64_t position : chosen) {
          mir::Expr literal;
          literal.kind = mir::Expr::LitInt;
          literal.lit_i = position;
          literal.type_ = "UInt";
          literal.unsized = {0, mir::UnsizedLeaf::Int};
          literal.data_only = true;
          mir::Expr single;
          single.kind = mir::Expr::FunApp;
          single.name = "IndexSingle";
          single.args.push_back(std::move(literal));
          scalar.args.push_back(std::move(single));
        }
        values.push_back(cint(scalar));
      };
      gather((int)dims.size() - 1);
      return values;
    }
    std::string detail =
        "integer gather selector is not known at compile time: kind=" +
        std::to_string(static_cast<int>(e.kind)) + " name=" + e.name +
        " type=" + e.type_ + " args=" + std::to_string(e.args.size());
    for (const auto& arg : e.args) detail += " [" + arg.name + "]";
    bail(detail);
  }

  bool try_cints(const mir::Expr& e, std::vector<long>* out) {
    try {
      *out = cints(e);
      return true;
    } catch (Bail&) {
      return false;
    }
  }

  std::vector<int64_t> matrix_positions(const mir::Expr& index, int64_t extent,
                                        const std::string& axis) {
    if (extent < 0 || extent > kMaxRegs)
      bail("matrix " + axis + " has an invalid extent");
    std::vector<long> values;
    if (index.name == "IndexAll") {
      values.reserve((size_t)extent);
      for (int64_t i = 1; i <= extent; ++i) values.push_back((long)i);
    } else if (index.name == "IndexSingle" && index.args.size() == 1) {
      values.push_back(cint(index.args[0]));
    } else if (index.name == "IndexBetween" && index.args.size() == 2) {
      const long lo = cint(index.args[0]), hi = cint(index.args[1]);
      if (hi >= lo) {
        if (lo < 1 || hi > extent)
          bail("matrix " + axis + " range is outside its extent");
        const uint64_t count = (uint64_t)hi - (uint64_t)lo + 1;
        if (count > (uint64_t)kMaxRegs)
          bail("matrix " + axis + " range is too large");
        values.reserve((size_t)count);
        for (long i = lo; i <= hi; ++i) values.push_back(i);
      }
    } else if (index.name == "IndexMulti" && index.args.size() == 1) {
      values = cints(index.args[0]);
    } else if (index.name == "IndexUpfrom" && index.args.size() == 1) {
      const long lo = cint(index.args[0]);
      if (lo < 1 || lo > extent + 1)
        bail("matrix " + axis + " upfrom is outside its extent");
      values.reserve((size_t)(extent - lo + 1));
      for (long i = lo; i <= extent; ++i) values.push_back(i);
    } else {
      bail("matrix " + axis + " index form");
    }
    std::vector<int64_t> positions;
    positions.reserve(values.size());
    for (long value : values) {
      if (value < 1 || value > extent) {
        std::string detail = "matrix " + axis + " gather index " +
                             std::to_string(value) +
                             " is outside 1:" + std::to_string(extent);
        for (const auto& [name, held] : ints)
          if (held.size() == 1)
            detail += " " + name + "=" + std::to_string(held[0]);
        bail(detail);
      }
      positions.push_back((int64_t)value - 1);
    }
    return positions;
  }

  std::vector<int64_t> graph_array_offsets(
      const std::vector<int64_t>& dims, ViewKind leaf,
      const std::vector<std::vector<int64_t>>& positions) {
    const size_t leaf_dims =
        leaf == ViewKind::Matrix
            ? 2
            : (leaf == ViewKind::Vector || leaf == ViewKind::RowVector ? 1 : 0);
    if (dims.size() < leaf_dims || positions.size() != dims.size())
      bail("array selection has inconsistent logical extents");
    checked_shape_product(dims, "array selection");
    const size_t n_array = dims.size() - leaf_dims;
    int64_t leaf_width = 1;
    for (size_t d = n_array; d < dims.size(); ++d) leaf_width *= dims[d];
    std::vector<int64_t> outer_stride(n_array, leaf_width);
    for (size_t d = n_array; d-- > 1;)
      outer_stride[d - 1] = outer_stride[d] * dims[d];

    std::vector<int64_t> offsets;
    std::function<void(size_t, int64_t)> arrays = [&](size_t d, int64_t off) {
      if (d != n_array) {
        for (int64_t position : positions[d])
          arrays(d + 1, off + position * outer_stride[d]);
        return;
      }
      if (leaf == ViewKind::Matrix) {
        const int64_t rows = dims[n_array];
        for (int64_t col : positions[n_array + 1])
          for (int64_t row : positions[n_array])
            offsets.push_back(off + col * rows + row);
      } else if (leaf == ViewKind::Vector || leaf == ViewKind::RowVector) {
        for (int64_t cell : positions[n_array]) offsets.push_back(off + cell);
      } else {
        offsets.push_back(off);
      }
    };
    arrays(0, 0);
    return offsets;
  }

  std::vector<int64_t> first_fast_array_offsets(
      const std::vector<int64_t>& dims,
      const std::vector<std::vector<int64_t>>& positions) {
    if (positions.size() != dims.size())
      bail("integer array selection has inconsistent logical extents");
    checked_shape_product(dims, "integer array selection");
    std::vector<int64_t> stride(dims.size(), 1);
    for (size_t d = 1; d < dims.size(); ++d)
      stride[d] = stride[d - 1] * dims[d - 1];
    std::vector<int64_t> offsets;
    std::function<void(int, int64_t)> gather = [&](int d, int64_t off) {
      if (d < 0) {
        offsets.push_back(off);
        return;
      }
      for (int64_t position : positions[(size_t)d])
        gather(d - 1, off + position * stride[(size_t)d]);
    };
    gather((int)dims.size() - 1, 0);
    return offsets;
  }

  std::vector<double> int_array_graph_values(const std::vector<long>& values,
                                             const std::vector<int64_t>& dims) {
    validated_int_array_dims(values, dims, "integer array graph value");
    if (values.empty()) return {};
    std::vector<std::vector<int64_t>> positions;
    positions.reserve(dims.size());
    for (int64_t extent : dims) {
      positions.emplace_back();
      positions.back().reserve((size_t)extent);
      for (int64_t k = 0; k < extent; ++k) positions.back().push_back(k);
    }
    // Scalar integer arrays have no container leaf, so graph_array_offsets
    // enumerates their cells outer-major.  Each returned coordinate's source
    // address is first-index-fast; reconstruct it while walking that order.
    std::vector<double> out;
    out.reserve(values.size());
    std::vector<int64_t> source_stride(dims.size(), 1);
    for (size_t d = 1; d < dims.size(); ++d)
      source_stride[d] = source_stride[d - 1] * dims[d - 1];
    std::function<void(size_t, int64_t)> gather = [&](size_t d,
                                                      int64_t source) {
      if (d == dims.size()) {
        out.push_back((double)values.at((size_t)source));
        return;
      }
      for (int64_t position : positions[d])
        gather(d + 1, source + position * source_stride[d]);
    };
    gather(0, 0);
    return out;
  }

  static bool same_view(const Range& a, const Range& b) {
    if (a.kind != b.kind) return false;
    if (a.kind == ViewKind::Flat) return a.len == 1 && b.len == 1;
    if (a.kind == ViewKind::Vector || a.kind == ViewKind::RowVector)
      return a.len == b.len;
    if (a.kind == ViewKind::Array)
      return a.len == b.len && a.dims == b.dims && a.leaf == b.leaf;
    return a.rows == b.rows && a.cols == b.cols;
  }

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

  static BuiltinLayout resolved_builtin_layout(
      const mir::Expr& e, const BuiltinSpec& spec,
      const std::vector<Range>& values) {
    std::vector<BuiltinArgumentShape> shapes;
    shapes.reserve(values.size());
    try {
      for (size_t k = 0; k < values.size(); ++k)
        shapes.push_back(builtin_argument_shape(e.args[k], values[k]));
      return builtin_layout(spec, shapes);
    } catch (const std::invalid_argument& error) {
      throw Bail{e.name + ": " + error.what()};
    }
  }

  // Does an assignment to `name` here run on every path that can reach a
  // read of it? It does when it sits at the declaration's own branch
  // depth, and when no loop entered since the declaration has already
  // taken a data-dependent break or continue -- either would jump from
  // ahead of the assignment to behind it, past a read that the fold has
  // already been applied to.
  bool fold_is_certain(const std::string& name) const {
    auto it = int_decl_at.find(name);
    const int depth = it == int_decl_at.end() ? 0 : it->second.first;
    const size_t enclosing = it == int_decl_at.end() ? 0 : it->second.second;
    if (branch_depth != depth) return false;
    for (size_t k = enclosing; k < loops.size(); ++k)
      if (!loops[k].breaks.empty() || !loops[k].continues.empty()) return false;
    return true;
  }

  static bool is_scalar(const Range& r) {
    return r.kind == ViewKind::Flat && r.len == 1;
  }

  Range typed(Range r, const std::string& type) {
    if (type == "UVector") {
      r.kind = ViewKind::Vector;
      r.rows = r.cols = 0;
    } else if (type == "URowVector") {
      r.kind = ViewKind::RowVector;
      r.rows = r.cols = 0;
    } else if (type == "UMatrix" && r.kind != ViewKind::Matrix) {
      bail("matrix expression has unknown logical extents");
    } else if (type == "UArray" && r.kind != ViewKind::Array) {
      bail("array expression has an unknown logical view");
    }
    return r;
  }

  static ViewKind leaf_of(const std::string& base) {
    if (base == "SVector") return ViewKind::Vector;
    if (base == "SRowVector") return ViewKind::RowVector;
    if (base == "SMatrix") return ViewKind::Matrix;
    return ViewKind::Flat;
  }

  static size_t leaf_rank(ViewKind leaf) {
    if (leaf == ViewKind::Matrix) return 2;
    if (leaf == ViewKind::Vector || leaf == ViewKind::RowVector) return 1;
    return 0;
  }

  Range declared(Range r, const mir::SizedType& type) {
    if (type.base == "SArray") {
      if (type.elem_base != "SReal" && type.elem_base != "SInt" &&
          leaf_of(type.elem_base) == ViewKind::Flat)
        bail(
            "only scalar-array and container-leaf declarations are supported "
            "by the register program");
      r.kind = ViewKind::Array;
      r.leaf = leaf_of(type.elem_base);
      r.dims.clear();
      for (const auto& d : type.dims) r.dims.push_back(cint(d));
      if (r.dims.size() <= leaf_rank(r.leaf))
        bail("array declaration lacks its leaf extents");
      return r;
    }
    if (type.base == "SVector")
      r.kind = ViewKind::Vector;
    else if (type.base == "SRowVector")
      r.kind = ViewKind::RowVector;
    else if (type.base == "SMatrix") {
      r.kind = ViewKind::Matrix;
      r.rows = cint(type.dims[0]);
      r.cols = cint(type.dims[1]);
    }
    return r;
  }

  static bool unsized_accepts(const mir::UnsizedView& type, const Range& r) {
    if (type.leaf == mir::UnsizedLeaf::Unknown ||
        type.leaf == mir::UnsizedLeaf::Complex)
      return false;
    if (type.depth != 0) {
      if (r.kind != ViewKind::Array) return false;
      const size_t leaf_rank = type.leaf == mir::UnsizedLeaf::Matrix ? 2
                               : type.leaf == mir::UnsizedLeaf::Vector ||
                                       type.leaf == mir::UnsizedLeaf::RowVector
                                   ? 1
                                   : 0;
      // Old register ranges did not stamp the sole scalar-array extent.
      // Retain that compatibility while requiring complete geometry for
      // every structural container newly represented here.
      if (r.dims.empty()) return type.depth == 1 && leaf_rank == 0;
      return r.dims.size() == (size_t)type.depth + leaf_rank;
    }
    if (type.leaf == mir::UnsizedLeaf::Vector)
      return r.kind == ViewKind::Vector;
    if (type.leaf == mir::UnsizedLeaf::RowVector)
      return r.kind == ViewKind::RowVector;
    if (type.leaf == mir::UnsizedLeaf::Matrix)
      return r.kind == ViewKind::Matrix;
    return is_scalar(r);
  }

  // ---- expressions ---------------------------------------------------------
  Range expr(const mir::Expr& e) {
    // A complete, concretely evaluable pure UDF is already executable by the
    // surrounding MIR interpreter. Materialize its result once instead of
    // expanding its call tree into a finite register program. The successful
    // callback is the proof of concreteness: stanc can label the recursive
    // remainder AutoDiffable even after all its actuals became data literals.
    if (e.kind == mir::Expr::FunApp &&
        e.fn_lib == mir::Expr::Lib::UserDefined && e.type_ == "UReal" &&
        extern_real) {
      double value = 0.0;
      if (extern_real(e, &value)) return {konst(value), 1};
    }
    switch (e.kind) {
      case mir::Expr::LitInt:
        return {konst((double)e.lit_i), 1};
      case mir::Expr::LitReal:
        return {konst(e.lit), 1};
      case mir::Expr::Var: {
        if (deferred_shapes.count(e.name))
          bail("unsized local read before its first assignment: " + e.name);
        auto it = reals.find(e.name);
        if (it != reals.end()) return it->second;
        auto known = known_int_arrays.find(e.name);
        if (known != known_int_arrays.end()) {
          auto dims = known_int_array_dims.find(e.name);
          const std::vector<int64_t> logical_dims =
              dims == known_int_array_dims.end()
                  ? std::vector<int64_t>{(int64_t)known->second.size()}
                  : dims->second;
          const std::vector<double> vals =
              int_array_graph_values(known->second, logical_dims);
          const int r = alloc((int)vals.size());
          emit_const(r, vals.data(), (int)vals.size());
          Range out{r, (int)vals.size()};
          out.kind = ViewKind::Array;
          out.dims = logical_dims;
          return out;
        }
        auto ii = ints.find(e.name);
        if (ii != ints.end()) {
          const std::vector<double> vals(ii->second.begin(), ii->second.end());
          const int r = alloc((int)vals.size());
          emit_const(r, vals.data(), (int)vals.size());
          Range out{r, (int)vals.size()};
          if (e.unsized.depth != 0) {
            out.kind = ViewKind::Array;
            out.dims = {(int64_t)vals.size()};
          }
          return out;
        }
        Range ext;
        if (bind_extern && bind_extern(e.name, &ext)) {
          reals[e.name] = ext;
          extern_bound.insert(e.name);
          return ext;
        }
        bail("unknown variable " + e.name);
      }
      case mir::Expr::Indexed: {
        // O1's index-composition pass may retain a base-only outer Indexed
        // node.  Its metadata is the final type while the inner node still
        // carries the pre-composition matrix type, so collapse it before any
        // logical-view decision.
        if (e.args.size() == 1 && e.args[0].kind == mir::Expr::Indexed) {
          mir::Expr composed = e.args[0];
          composed.type_ = e.type_;
          composed.unsized = e.unsized;
          composed.data_only = e.data_only;
          composed.promoted = e.promoted;
          composed.raw = e.raw;
          return expr(composed);
        }
        if (e.args.empty()) bail("index form");
        const Range b = expr(e.args[0]);
        if (e.args.size() == 2 && e.args[1].name == "IndexAll") return b;
        // General compile-time matrix selection. Registers are column-major,
        // so selected columns are outer and rows inner; this covers All,
        // Single, Between, and Multi in any pair while preserving selector
        // order and duplicate gather indices.
        if (b.kind == ViewKind::Matrix && e.args.size() == 3) {
          const std::vector<int64_t> rows =
              matrix_positions(e.args[1], b.rows, "row of " + e.args[0].name);
          const std::vector<int64_t> cols = matrix_positions(
              e.args[2], b.cols, "column of " + e.args[0].name);
          if (!rows.empty() && cols.size() > (size_t)kMaxRegs / rows.size())
            bail("matrix selection needs too many registers");
          const size_t width = rows.size() * cols.size();
          if (width == 1 && e.type_ != "UMatrix" && e.type_ != "UVector" &&
              e.type_ != "URowVector")
            return {b.reg + (int)(cols[0] * b.rows + rows[0]), 1};
          const int r = alloc((int)width);
          int at = 0;
          for (int64_t j : cols)
            for (int64_t i : rows)
              emit(Program::MOV, r + at++, b.reg + (int)(j * b.rows + i));
          Range out{r, (int)width};
          if (e.type_ == "UVector") {
            out.kind = ViewKind::Vector;
          } else if (e.type_ == "URowVector") {
            out.kind = ViewKind::RowVector;
          } else {
            out.kind = ViewKind::Matrix;
            out.rows = (int64_t)rows.size();
            out.cols = (int64_t)cols.size();
          }
          return out;
        }
        // A matrix row, `m[i]` or `m[i, ]`. Ahead of the all-single check
        // below, which the explicit `IndexAll` would not pass.
        if (b.kind == ViewKind::Matrix && e.args.size() >= 2 &&
            e.args[1].name == "IndexSingle" &&
            (e.args.size() == 2 ||
             (e.args.size() == 3 && e.args[2].name == "IndexAll")))
          return matrix_row(b, cint(e.args[1].args[0]));
        if ((b.kind == ViewKind::Vector || b.kind == ViewKind::RowVector ||
             b.kind == ViewKind::Flat) &&
            e.args.size() == 2 && e.args[1].name == "IndexBetween") {
          const long lo = cint(e.args[1].args[0]);
          const long hi = cint(e.args[1].args[1]);
          if (hi < lo) return typed(Range{b.reg, 0}, e.type_);
          if (lo < 1 || hi > b.len)
            bail("range index out of the declared range");
          return typed(Range{b.reg + (int)lo - 1, (int)(hi - lo + 1)}, e.type_);
        }
        if ((b.kind == ViewKind::Vector || b.kind == ViewKind::RowVector ||
             b.kind == ViewKind::Flat) &&
            e.args.size() == 2 && e.args[1].name == "IndexMulti") {
          const std::vector<long> indices = cints(e.args[1].args[0]);
          const int r = alloc((int)indices.size());
          for (size_t k = 0; k < indices.size(); ++k) {
            if (indices[k] < 1 || indices[k] > b.len)
              bail("gather index out of the declared range");
            emit(Program::MOV, r + (int)k, b.reg + (int)indices[k] - 1);
          }
          return typed(Range{r, (int)indices.size()}, e.type_);
        }
        if (b.kind == ViewKind::Matrix) {
          if (e.args.size() == 3) {
            const long i = cint(e.args[1].args[0]);
            const long j = cint(e.args[2].args[0]);
            if (i < 1 || i > b.rows || j < 1 || j > b.cols)
              bail("matrix index out of the declared range");
            return {b.reg + (int)((j - 1) * b.rows + i - 1), 1};
          }
          // Any remaining one-index matrix selection (a row range,
          // gather, or upfrom) resolves through the shared index geometry
          // over the column-major registers.
          if (e.args.size() == 2) {
            const std::vector<int64_t> rows =
                matrix_positions(e.args[1], b.rows, "row of " + e.args[0].name);
            const BuiltinIndexMap map = builtin_index_map(
                {b.rows, b.cols}, 2, {rows}, {e.args[1].name == "IndexSingle"},
                SliceStorageOrder::OuterMajor);
            if (map.count > kMaxRegs)
              bail("matrix selection needs too many registers");
            const int r = alloc((int)map.count);
            for (int64_t k = 0; k < map.count; ++k) {
              const int64_t cell = map.kind == BuiltinSliceMap::Kind::Contiguous
                                       ? map.offset + k
                                   : map.kind == BuiltinSliceMap::Kind::Strided
                                       ? map.offset + k * map.stride
                                       : map.gather[(size_t)k];
              emit(Program::MOV, r + (int)k, b.reg + (int)cell);
            }
            Range out{r, (int)map.count};
            if (e.type_ == "UMatrix" && map.dimensions.size() == 2) {
              out.kind = ViewKind::Matrix;
              out.rows = map.dimensions[0];
              out.cols = map.dimensions[1];
            } else {
              out = typed(out, e.type_);
            }
            return out;
          }
          bail("matrix index form");
        }

        if (b.kind == ViewKind::Vector || b.kind == ViewKind::RowVector ||
            b.kind == ViewKind::Flat) {
          if (e.args.size() != 2) bail("one-dimensional index form");
          if (e.args[1].name == "IndexUpfrom") {
            const long lo = cint(e.args[1].args[0]);
            if (lo < 1 || lo > b.len + 1)
              bail("upfrom index out of the declared range");
            return typed(Range{b.reg + (int)lo - 1, (int)(b.len - lo + 1)},
                         e.type_);
          }
          // Only a Single selects one element; any other index kind was
          // silently misread as one before this guard.
          if (e.args[1].name != "IndexSingle")
            bail("one-dimensional index form " + e.args[1].name);
          long ix;
          if (try_cint(e.args[1].args[0], &ix)) {
            if (ix < 1 || ix > b.len) bail("index out of the declared range");
            return {b.reg + (int)ix - 1, 1};
          }
          const Range iv = expr(e.args[1].args[0]);
          if (!is_scalar(iv)) bail("runtime index is not scalar");
          const int r = alloc(1);
          p.code.push_back(
              Program::Instr{Program::DYN_INDEX, r, b.reg, iv.reg, 0, b.len});
          return {r, 1};
        }

        if (b.kind != ViewKind::Array)
          bail("indexing this logical view is unsupported");
        const std::vector<int64_t> dims =
            b.dims.empty() ? std::vector<int64_t>{b.len} : b.dims;
        const size_t n_idx = e.args.size() - 1;
        if (n_idx > dims.size()) bail("too many array indices");

        // Register-file arrays are outer-major (graph order).  A selection
        // can therefore be strided even when it fixes a leading index, so
        // gather the complete result rather than pretending it is a
        // contiguous suffix.  This also covers array slices such as
        // `whichobs[row, 1:n]` and selections which continue into a
        // vector/matrix leaf.
        std::vector<std::vector<int64_t>> positions;
        std::vector<bool> drops;
        positions.reserve(dims.size());
        drops.reserve(dims.size());
        bool all_static = true;
        size_t runtime_dim = dims.size();
        for (size_t d = 0; d < n_idx; ++d) {
          const mir::Expr& index = e.args[d + 1];
          if (index.name == "IndexSingle" && index.args.size() == 1) {
            long ignored;
            if (!try_cint(index.args[0], &ignored)) {
              all_static = false;
              runtime_dim = d;
              break;
            }
          }
          positions.push_back(matrix_positions(index, dims[d], "array"));
          drops.push_back(index.name == "IndexSingle");
        }
        if (!all_static) {
          if (b.leaf != ViewKind::Flat || n_idx != dims.size() ||
              runtime_dim + 1 != dims.size() ||
              e.args[runtime_dim + 1].name != "IndexSingle" ||
              e.args[runtime_dim + 1].args.size() != 1)
            bail("runtime array index must be the final scalar index");
          positions.push_back({0});
          const std::vector<int64_t> base_offsets =
              graph_array_offsets(dims, b.leaf, positions);
          if (base_offsets.size() != 1)
            bail("runtime array index has an ambiguous base");
          const Range iv = expr(e.args[runtime_dim + 1].args[0]);
          if (!is_scalar(iv)) bail("runtime array index is not scalar");
          const int r = alloc(1);
          p.code.push_back(Program::Instr{Program::DYN_INDEX, r, b.reg, iv.reg,
                                          (int32_t)base_offsets[0],
                                          (int32_t)dims[runtime_dim]});
          return {r, 1};
        }
        const size_t leaf_axes =
            b.leaf == ViewKind::Matrix                                    ? 2
            : b.leaf == ViewKind::Vector || b.leaf == ViewKind::RowVector ? 1
                                                                          : 0;
        BuiltinIndexMap map;
        try {
          // The shared index geometry over outer-major storage; trailing
          // axes keep their full extent inside the resolver.
          map = builtin_index_map(dims, leaf_axes, positions, drops,
                                  SliceStorageOrder::OuterMajor);
        } catch (const std::invalid_argument& error) {
          bail("array selection: " + std::string(error.what()));
        }
        const int64_t width = map.count;
        if (width > kMaxRegs) bail("array selection needs too many registers");
        std::vector<int64_t> out_dims = map.dimensions;
        const int r = alloc((int)width);
        for (int64_t k = 0; k < width; ++k) {
          const int64_t cell = map.kind == BuiltinSliceMap::Kind::Contiguous
                                   ? map.offset + k
                               : map.kind == BuiltinSliceMap::Kind::Strided
                                   ? map.offset + k * map.stride
                                   : map.gather[(size_t)k];
          emit(Program::MOV, r + (int)k, b.reg + (int)cell);
        }
        if (width == 1 && (e.type_ == "UReal" || e.type_ == "UInt"))
          return {r, 1};
        Range out{r, (int)width};
        if (e.type_ == "UVector") {
          out.kind = ViewKind::Vector;
        } else if (e.type_ == "URowVector") {
          out.kind = ViewKind::RowVector;
        } else if (e.type_ == "UMatrix") {
          if (out_dims.size() != 2) bail("matrix index form");
          out.kind = ViewKind::Matrix;
          out.rows = out_dims[0];
          out.cols = out_dims[1];
        } else {
          out.kind = ViewKind::Array;
          out.leaf =
              e.unsized.leaf == mir::UnsizedLeaf::Matrix   ? ViewKind::Matrix
              : e.unsized.leaf == mir::UnsizedLeaf::Vector ? ViewKind::Vector
              : e.unsized.leaf == mir::UnsizedLeaf::RowVector
                  ? ViewKind::RowVector
                  : ViewKind::Flat;
          out.dims = std::move(out_dims);
        }
        return out;
      }
      case mir::Expr::TernaryIf: {
        long c;
        if (try_cint(e.args[0], &c)) return expr(e.args[c != 0 ? 1 : 2]);
        return branchy_select(e.args[0], e.args[1], e.args[2]);
      }
      case mir::Expr::EOr:
      case mir::Expr::EAnd: {
        const Range a = expr(e.args[0]);
        if (!is_scalar(a)) bail("logical operator on a container");
        const int z = konst(0.0), ta = alloc(1), r = alloc(1);
        emit(Program::NE, ta, a.reg, z);
        emit(Program::MOV, r, ta);

        // The result starts as the normalized left operand. AND is already
        // final when that value is false; OR is final when it is true. Only
        // the other case enters the right operand, preserving Stan's
        // short-circuit evaluation and any domain errors or effects there.
        int done = -1;
        if (e.kind == mir::Expr::EOr) {
          const int rhs = emit(Program::JZ, 0, ta);
          done = emit(Program::JMP, 0);
          p.code[(size_t)rhs].dst = (int)p.code.size();
        } else {
          done = emit(Program::JZ, 0, ta);
        }
        const Range b = expr(e.args[1]);
        if (!is_scalar(b)) bail("logical operator on a container");
        const int tb = alloc(1);
        emit(Program::NE, tb, b.reg, z);
        emit(Program::MOV, r, tb);
        p.code[(size_t)done].dst = (int)p.code.size();
        return {r, 1};
      }
      case mir::Expr::FunApp:
        return fun(e);
      default:
        bail("expression");
    }
  }

  // A ternary on a runtime condition: both arms write the same registers.
  Range branchy_select(const mir::Expr& c, const mir::Expr& a,
                       const mir::Expr& b) {
    const Range cv = expr(c);
    if (!is_scalar(cv)) bail("conditional on a container");
    // Compile the arms first to learn the width, then re-emit into place.
    const int jz = emit(Program::JZ, 0, cv.reg);
    const Range av = expr(a);
    const int dst = alloc(av.len);
    for (int k = 0; k < av.len; ++k) emit(Program::MOV, dst + k, av.reg + k);
    const int jmp = emit(Program::JMP, 0);
    p.code[(size_t)jz].dst = (int)p.code.size();
    const Range bv = expr(b);
    if (bv.len != av.len) bail("conditional arms of different widths");
    if (av.kind == ViewKind::Array || bv.kind == ViewKind::Array)
      bail("conditional arms of different logical views");
    if (!same_view(av, bv)) bail("conditional arms of different logical views");
    for (int k = 0; k < bv.len; ++k) emit(Program::MOV, dst + k, bv.reg + k);
    p.code[(size_t)jmp].dst = (int)p.code.size();
    Range out = av;
    out.reg = dst;
    return out;
  }

  // One row of a matrix, copied into a run of its own. Column-major
  // storage puts a row's elements `rows` apart, and a strided Eigen block
  // has no packet access -- so stan-math reduces such a row in ascending
  // scalar order, which is the order every reduction this compiler emits
  // walks a run in. That is what makes the copy safe rather than merely
  // convenient: `sum` accumulates ascending, `max` compares, and the rest
  // are elementwise. A reduction with its own grouping (DOT, SOFTMAX,
  // LSE_RANGE) is not one this compiler emits, and adding one would have
  // to answer this question again.
  Range matrix_row(const Range& m, long i) {
    if (i < 1 || i > m.rows) bail("matrix index out of the declared range");
    const int r = alloc((int)m.cols);
    for (int64_t j = 0; j < m.cols; ++j)
      emit(Program::MOV, r + (int)j, m.reg + (int)(j * m.rows + (i - 1)));
    Range out{r, (int)m.cols};
    out.kind = ViewKind::RowVector;
    return out;
  }

  // The ordinary UDF path and higher-order families must bind callback
  // arguments identically.  In particular, data integers retain their
  // compile-time values and data reals retain both their register and known
  // value, so nested calls automatically inherit every improvement made to
  // UDF argument handling here.
  InlineArg inline_argument(const mir::Expr& e) {
    InlineArg arg;
    long v;
    if (e.type_ == "UInt" && try_cint(e, &v)) {
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
      if (e.type_ == "UReal") arg.is_const_real = try_creal(e, &arg.const_real);
      arg.real = expr(e);
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

    bool propto = false;
    const std::string base =
        mir::reduce_sum_partial_name(e.args[0].name, &propto);
    const std::vector<mir::UnsizedView> views =
        mir::reduce_sum_partial_views(e);
    const mir::FunDef* f = mir::resolve_callback(funs, base, views);
    if (f == nullptr) bail("reduce_sum: unknown partial-sum function " + base);
    if (f->arg_names.size() != views.size())
      bail("reduce_sum: partial-sum arity does not match the call");

    std::vector<InlineArg> args;
    args.reserve(views.size());
    InlineArg sliced;
    sliced.real = slice;
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
    (void)propto;
    return inline_call(*f, args);
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
    const mir::FunDef* f = mir::resolve_callback(funs, e.args[0].name, views);
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
           name == "multi_normal_rng" || name == "dirichlet_rng";
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
      // An integer draw is runtime geometry: a size, an index or a branch
      // condition the region has no way to know. That is the interpreter's
      // remit, so leave the whole tranche there rather than quietly serving
      // one out of a double register.
      if (scalar_rng_is_int(*family))
        bail(e.name + ": an integer draw stays on WaInterp");
      if (args.size() != scalar_rng_arity(*family))
        bail(e.name + ": wrong number of arguments");
      for (const Range& a : args)
        if (!is_scalar(a))
          bail(e.name + ": container arguments stay on WaInterp");
      variant = static_cast<uint8_t>(*family);
    } else if (e.name == "categorical_rng") {
      bail("categorical_rng: an integer draw stays on WaInterp");
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
      variant = kMultiNormalRngVariant;
      out_len = args[0].len;
      out_kind = ViewKind::Vector;
      idata.push_back(out_len);
    }

    Range out{0, out_len};
    out.kind = out_kind;
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
    out = typed(out, e.type_);
    std::vector<int> idata;
    if (layout.integer_matrix_rows != 0)
      idata = {(int)layout.integer_matrix_rows,
               (int)layout.integer_matrix_cols};
    return kernel_call(spec.opcode, args, out, 0, spec.activity_mask,
                       std::move(idata), {}, e.name);
  }

  static std::optional<Program::Code> native_builtin_code(uint16_t opcode) {
    switch (opcode) {
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
    if (spec.opcode == OP_DIV && e.type_ == "UInt") code = Program::IDIV;
    const int result = alloc(n);
    for (int i = 0; i < n; ++i) {
      const int a = args[0].reg + (is_scalar(args[0]) ? 0 : i);
      const int b =
          args.size() == 2 ? args[1].reg + (is_scalar(args[1]) ? 0 : i) : 0;
      emit(code, result + i, a, b);
    }
    out.reg = result;
    out.len = n;
    return typed(out, e.type_);
  }

  // Probability functions use their shared registry policy. Graph-backed
  // descriptors marshal register ranges and integer payloads into KernelCtx;
  // all-integer descriptors evaluate once as constants, with the same Stan
  // Math implementation used by graph lowering and MIR interpretation.
  Range density_call(const mir::Expr& e, const DensitySpec& spec) {
    if ((int)e.args.size() != spec.arity)
      bail(e.name + ": wrong number of arguments");

    const auto integer_values = [&](const mir::Expr& arg) {
      const std::vector<long> source =
          arg.type_ == "UInt" && arg.unsized.depth == 0
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
          argument.active = !source.data_only;
          args.push_back(expr(source));
          argument.shape = builtin_argument_shape(source, args.back());
        }
        plan_arguments.push_back(std::move(argument));
      }
      const DensityCallPlan plan =
          density_call_plan(spec, plan_arguments, e.fn_propto);
      if (plan.empty_result) return {konst(0.0), 1};
      Range out{0, 1};
      return kernel_call(spec.opcode, args, out, plan.variant,
                         plan.activity_mask, plan.idata, {}, e.name);
    } catch (const std::exception& error) {
      bail(e.name + ": " + error.what());
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

  Range transform_call(const mir::Expr& e, const CallableTransformSpec& spec) {
    if (e.args.size() != spec.arity)
      bail(e.name + ": wrong number of arguments");
    if (spec.structured && spec.direction == TransformDirection::Unconstrain)
      bail(e.name +
           ": structured inverse is not supported in a runtime region");

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

    // Registered matrix operations: the same dedicated kernels the graph
    // emits, called over the operands' column-major ranges. The active
    // variant bit mirrors the graph's autodiff stamp -- any non-data
    // operand -- with the kernels' values_only() guard covering
    // values-only executions, exactly as it does for graph ops.
    if (const BuiltinSpec* matrix = shaped_builtin_spec(
            e.name, e.args.size(), BuiltinShapePolicy::MatrixOp)) {
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
        if (!argument.data_only) active = true;
      Range out{0, (int)map.result.storage_size};
      out = shaped(std::move(out), map.result);
      std::vector<int> idata;
      idata.reserve(map.idata.size());
      for (const int64_t value : map.idata) idata.push_back((int)value);
      return kernel_call(
          matrix->opcode, args, out,
          (uint8_t)(map.variant | (active ? map.active_variant : 0u)),
          matrix->activity_mask, std::move(idata), {}, e.name);
    }

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
        Range out{0, 1};
        return kernel_call(reduction->opcode, args, out, 0,
                           reduction->activity_mask, {}, {}, e.name);
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
          e.args[1].type_ == "UMatrix")) {
      if (const auto native = native_builtin_code(builtin->opcode))
        return native_builtin_call(e, *builtin, *native);
      return builtin_kernel_call(e, *builtin);
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
          e.args[1].type_ == "UMatrix")
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
      auto it = funs.find(e.name);
      if (it == funs.end()) bail("unknown function " + e.name);
      return inline_call(*it->second, inline_arguments(e.args));
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
        if (e.name == "FnMakeRowVec" && e.type_ == "UMatrix") {
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
      return typed(Range{r, 1}, e.type_);
    }
    // Ahead of the arity-keyed blocks below: those end in a bail on an
    // unknown name, so while this table sat after them a two-argument
    // density (exponential_lpdf) was unreachable -- listed as supported
    // and refused in practice.
    // Explicit density calls. `target += normal_lpdf(y | mu, s)` keeps
    // every constant, which is the propto-OFF instantiation the machine
    // has; a `~` statement's dropped-constant form depends on which
    // arguments are autodiff and is not expressible here.
    {
      const int dc = program_density_id_by_name(e.name);
      const int arity = program_density_arity(dc);
      if (arity) {
        // A `~` statement lowers to the same call with propto set, and
        // which constants it drops depends on which arguments are
        // autodiff -- a distinction the program cannot make, since it
        // binds every argument the same way. Getting this wrong is
        // invisible in the gradient and shows up only in lp, so refuse
        // rather than approximate. (Measured, before this check: lp off
        // by exactly log(2*pi)/2 on a normal.)
        if (e.fn_propto)
          bail(
              "`~` inside a runtime-control region (write it as "
              "`target += " +
              e.name +
              "(...)`, which keeps every "
              "constant and is what the region can reproduce)");
        if ((int)e.args.size() != arity)
          bail(e.name + " takes " + std::to_string(arity) + " arguments here");
        Range argv[kMaxDensityArgs];
        bool any_container = false;
        for (int k = 0; k < arity; ++k) {
          argv[k] = expr(e.args[(size_t)k]);
          if (!is_scalar(argv[k])) any_container = true;
        }
        if (any_container) {
          // One propto-OFF call, vectorized the way CmdStan's generated
          // code would call it (stan-math's own broadcasting over an
          // Eigen::Map per container argument -- program_density_vec),
          // not `len` scalar calls summed by hand, which would not sum in
          // the same order. Every container argument recycles to the same
          // length, Stan's own rule for a vectorized call; and the density
          // has to be one whose partials tier already pays for the extra
          // instantiations (program_density.cpp), the same affordability
          // line the mask-dispatched partials draw.
          int64_t len = 0;
          bool ok = program_density_container_capable(dc);
          for (int k = 0; ok && k < arity; ++k) {
            const Range& a = argv[k];
            if (is_scalar(a)) continue;
            if (a.kind != ViewKind::Vector && a.kind != ViewKind::RowVector) {
              ok = false;
            } else if (a.len <= 0) {
              ok = false;
            } else if (len == 0) {
              len = a.len;
            } else if (len != a.len) {
              ok = false;
            }
          }
          if (!ok) bail(e.name + " on a container");
          Program::VecDensity v;
          v.density_id = (uint16_t)dc;
          v.arity = (uint8_t)arity;
          v.len = (int32_t)len;
          for (int k = 0; k < arity; ++k) {
            v.arg_reg[k] = argv[k].reg;
            if (!is_scalar(argv[k])) v.container_mask |= (uint8_t)(1u << k);
          }
          const int r = alloc(1);
          p.vec_densities.push_back(v);
          p.code.push_back(Program::Instr{Program::DENSITY_VEC, r,
                                          (int)p.vec_densities.size() - 1, 0, 0,
                                          0});
          return {r, 1};
        }
        int argv_reg[kMaxDensityArgs];
        for (int k = 0; k < arity; ++k) argv_reg[k] = argv[k].reg;
        // Three arguments or fewer ride in the instruction; a fourth
        // needs the contiguous form, so copy them into a block.
        int a0 = argv_reg[0], a1 = arity > 1 ? argv_reg[1] : 0;
        int a2 = arity > 2 ? argv_reg[2] : 0;
        if (arity > 3) {
          a0 = alloc(arity);
          for (int k = 0; k < arity; ++k)
            emit(Program::MOV, a0 + k, argv_reg[k]);
          a1 = 0;
          a2 = 0;
        }
        const int r = alloc(1);
        p.code.push_back(Program::Instr{Program::DENSITY, r, a0, a1, a2, dc});
        return {r, 1};
      }
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
      return typed(out, e.type_);
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
      return typed(out, e.type_);
    }
    if (e.args.size() == 2) {
      // An int-typed binary is integer arithmetic, and `divide` truncates
      // where the real DIV below does not: `divide(7, 2)` is 3, not 3.5.
      // The register file holds only reals, so fold the integer answer
      // whenever cint can reach it rather than emitting real arithmetic.
      if (e.type_ == "UInt") {
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
          bail(e.name + ": " + error.what());
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
        return typed(out, e.type_);
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
        c = e.type_ == "UInt" ? Program::IDIV : Program::DIV;
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
            return typed(Range{both, n}, e.type_);
          }
          default:
            bail("function " + e.name);
        }
      } else
        bail("function " + e.name);
      const int r = alloc(n);
      for (int i = 0; i < n; ++i)
        emit(c, r + i, a.reg + (a_scalar ? 0 : i), b.reg + (b_scalar ? 0 : i));
      Range out{r, n};
      if (a_scalar && !b_scalar)
        out = b;
      else if (b_scalar && !a_scalar)
        out = a;
      else if (same_view(a, b))
        out = a;
      out.reg = r;
      out.len = n;
      return typed(out, e.type_);
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
        for (int i = 1; i < a.len; ++i) emit(Program::ADD, r, r, a.reg + i);
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
        const Program::Code c = nan ? Program::NE : Program::EQ;
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
        return typed(out, e.type_);
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
      return typed(out, e.type_);
    }
    bail("function " + e.name);
  }

  // ---- statements ----------------------------------------------------------
  struct Returned {
    Range r;
  };
  struct CompileBreak {};
  struct CompileContinue {};

  int64_t sized_len(const mir::SizedType& t) {
    int64_t n = 1;
    for (const auto& d : t.dims) {
      const int64_t extent = cint(d);
      if (extent < 0 || (n != 0 && extent != 0 && n > kMaxRegs / extent))
        bail("declaration has an invalid or oversized shape");
      n *= extent;
    }
    return n;
  }

  // Declare (or redeclare) a real variable of `len` registers. Stan's
  // uninitialized real value is NaN; callers may provide another fill only
  // when the surrounding lowering has an explicit initialized-value policy.
  Range declare(const std::string& name, int len, Range view = {},
                double fill = std::numeric_limits<double>::quiet_NaN()) {
    Range r = view;
    r.reg = alloc(len);
    r.len = len;
    const std::vector<double> init((size_t)len, fill);
    emit_const(r.reg, init.data(), len);
    reals[name] = r;
    return r;
  }

  // Close the program: prepend the NaN fills the zero-length adoption in
  // Assignment (below) deferred.
  // Every caller runs this once the region has compiled and before the
  // program runs; it is idempotent, and a region with no adoption pays
  // nothing. The fills go in front rather than at the declaration because
  // the width is only known once the assignment inside the branch has
  // compiled, and the jumps are the only instructions that name a code
  // position (CONST/CONSTR's `a` is a pool index, CALL's is a call index).
  void finish() {
    if (late_bound.empty() && hoisted_int_initializers.empty()) return;
    std::vector<Program::Instr> prologue = std::move(hoisted_int_initializers);
    for (const auto& [reg, len] : late_bound) {
      const std::vector<double> nan((size_t)len,
                                    std::numeric_limits<double>::quiet_NaN());
      prologue.push_back(const_instr(reg, nan.data(), len));
    }
    const int n = (int)prologue.size();
    for (auto& instr : p.code)
      if (instr.code == Program::JZ || instr.code == Program::JMP)
        instr.dst += n;
    p.code.insert(p.code.begin(), prologue.begin(), prologue.end());
    late_bound.clear();
  }

  void stmt(const mir::Stmt& s) {
    switch (s.kind) {
      case mir::Stmt::Decl: {
        // A declaration shadows every compile-time fact retained for an
        // earlier optimized symbol with the same name.
        // It also shadows either runtime representation: --O1 may reuse one
        // symbol id for a scalar int in one block and an array/container in
        // the next, and the later assignment must not see the old binding.
        reals.erase(s.decl_id);
        ints.erase(s.decl_id);
        deferred_shapes.erase(s.decl_id);
        known_int_arrays.erase(s.decl_id);
        known_int_array_dims.erase(s.decl_id);
        int_array_names.erase(s.decl_id);
        int_decl_at.erase(s.decl_id);
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
            reals[s.decl_id] = Range{};
            deferred_shapes[s.decl_id] = view;
            return;
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
          return;
        }
        if (s.decl_type.base.empty())
          bail("declaration has no sized or unsized logical view: " +
               s.decl_id);
        if (s.decl_type.base == "SInt") {
          int_decl_at[s.decl_id] = {branch_depth, loops.size()};
          if (s.has_init) {
            long folded;
            if (try_cint(s.init, &folded)) {
              ints[s.decl_id] = {folded};
              return;
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
            return;
          }
          ints[s.decl_id] = {std::numeric_limits<int>::min()};
          return;
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
        return;
      }
      case mir::Stmt::Assignment: {
        auto deferred = deferred_shapes.find(s.lhs);
        if (deferred != deferred_shapes.end()) {
          if (!s.lhs_idx.empty())
            bail("indexed assignment before unsized shape adoption for " +
                 s.lhs);
          const Range v = expr(s.rhs);
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

          if (int_array_names.count(s.lhs)) {
            std::vector<long> values;
            if (fold_is_certain(s.lhs) && try_cints(s.rhs, &values) &&
                values.size() == (size_t)v.len) {
              known_int_arrays[s.lhs] = std::move(values);
              known_int_array_dims[s.lhs] = adopted.dims;
            } else {
              known_int_arrays.erase(s.lhs);
              known_int_array_dims.erase(s.lhs);
            }
          }
          return;
        }
        if (ints.count(s.lhs) && s.lhs_idx.empty()) {
          long ignored;
          // A conditional write must preserve the old value on the untaken
          // path, so it cannot be folded into the single compile-time copy.
          // Likewise, an unconditional assignment after a structured while
          // may read loop-carried state that now lives in registers.  The
          // lowering can export scalar-int live-outs, so reify both cases
          // instead of refusing a representable integer recurrence.
          if (!fold_is_certain(s.lhs) ||
              (structured_while_seen && !try_cint(s.rhs, &ignored)))
            reify_written_int(s.lhs);
        }
        if (ints.count(s.lhs) && s.lhs_idx.empty()) {
          // This assignment is certain and its RHS stayed a compile-time
          // integer, so subsequent reads may use the folded value directly.
          ints[s.lhs] = {cint(s.rhs)};
          return;
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
          // live-in either: an --O1 inliner return-temp (or similar
          // compiler-introduced local) whose only other write sat in a
          // branch the surrounding graph lowering folded away as
          // unreachable, so it never got a slot to hand this region. A
          // full-variable write with no prior binding is exactly the
          // zero-length "adopt the assigned shape" case below, minus the
          // placeholder declaration -- give it one now.
          reals[s.lhs] = Range{};
          it = reals.find(s.lhs);
        }
        if (it == reals.end()) bail("assignment to undeclared " + s.lhs);
        const Range dst = it->second;
        const Range v = expr(s.rhs);
        if (s.lhs_idx.empty()) {
          std::vector<long> folded_ints;
          const bool have_folded_ints = int_array_names.count(s.lhs) &&
                                        fold_is_certain(s.lhs) &&
                                        try_cints(s.rhs, &folded_ints) &&
                                        folded_ints.size() == (size_t)v.len;
          if (dst.len == 0 && v.len != 0) {
            // The zero-length declaration is stanc3's --O1 inliner
            // leaving a return variable unsized (`vector[0]`) for the
            // assignment to size; adopt the assigned shape. The inliner
            // assigns it exactly once, right where the call was, so no
            // two branch arms can disagree about the size.
            //
            // That one assignment can still sit inside a data-dependent
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
            it->second = nd;
            if (have_folded_ints) {
              known_int_arrays[s.lhs] = std::move(folded_ints);
              known_int_array_dims[s.lhs] = nd.dims;
            } else if (int_array_names.count(s.lhs)) {
              known_int_arrays.erase(s.lhs);
              known_int_array_dims.erase(s.lhs);
            }
            return;
          }
          if (v.len != dst.len) bail("assignment width mismatch for " + s.lhs);
          if (!same_view(v, dst))
            bail("assignment logical view mismatch for " + s.lhs);
          for (int k = 0; k < v.len; ++k)
            emit(Program::MOV, dst.reg + k, v.reg + k);
          if (have_folded_ints) {
            known_int_arrays[s.lhs] = std::move(folded_ints);
            known_int_array_dims[s.lhs] = dst.dims;
          } else if (int_array_names.count(s.lhs)) {
            known_int_arrays.erase(s.lhs);
            known_int_array_dims.erase(s.lhs);
          }
          return;
        }
        // A single All is the complete destination view. This is reachable
        // in ODE functions and runtime-control statement islands, so it
        // must agree with both graph lowering and MirInterp rather than
        // forcing an otherwise supported region onto the fallback path.
        if (s.lhs_idx.size() == 1 && s.lhs_idx[0].name == "IndexAll") {
          if (dst.kind == ViewKind::Flat)
            bail("full-span assignment needs a container for " + s.lhs);
          if (v.len != dst.len)
            bail("full-span assignment width mismatch for " + s.lhs);
          if (!same_view(v, dst))
            bail("full-span assignment logical view mismatch for " + s.lhs);
          for (int k = 0; k < v.len; ++k)
            emit(Program::MOV, dst.reg + k, v.reg + k);
          if (int_array_names.count(s.lhs)) {
            std::vector<long> values;
            if (fold_is_certain(s.lhs) && try_cints(s.rhs, &values) &&
                values.size() == (size_t)v.len) {
              known_int_arrays[s.lhs] = std::move(values);
              known_int_array_dims[s.lhs] = dst.dims;
            } else {
              known_int_arrays.erase(s.lhs);
              known_int_array_dims.erase(s.lhs);
            }
          }
          return;
        }
        if (dst.kind == ViewKind::Matrix && s.lhs_idx.size() == 2) {
          const std::vector<int64_t> rows = matrix_positions(
              s.lhs_idx[0], dst.rows, "assignment row of " + s.lhs);
          const std::vector<int64_t> cols = matrix_positions(
              s.lhs_idx[1], dst.cols, "assignment column of " + s.lhs);
          if (!rows.empty() && cols.size() > (size_t)kMaxRegs / rows.size())
            bail("matrix assignment selection is too large");
          const size_t width = rows.size() * cols.size();
          if (v.len != static_cast<int>(width))
            bail("matrix assignment width mismatch for " + s.lhs);
          int at = 0;
          for (int64_t j : cols)
            for (int64_t i : rows)
              emit(Program::MOV, dst.reg + (int)(j * dst.rows + i),
                   v.reg + at++);
          return;
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
                  s.lhs_idx[d], dims[d], "assignment array of " + s.lhs));
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
          if (v.len != (int)width)
            bail("array assignment width mismatch for " + s.lhs);
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
              if (try_cint(s.rhs, &value)) {
                values = {value};
                folded = true;
              }
            } else {
              folded = try_cints(s.rhs, &values);
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
          return;
        }
        if ((dst.kind == ViewKind::Vector || dst.kind == ViewKind::RowVector ||
             dst.kind == ViewKind::Flat) &&
            s.lhs_idx.size() == 1) {
          const std::vector<int64_t> positions =
              matrix_positions(s.lhs_idx[0], dst.len, "assignment vector");
          if (v.len != static_cast<int>(positions.size()))
            bail("vector assignment width mismatch for " + s.lhs);
          for (size_t k = 0; k < positions.size(); ++k)
            emit(Program::MOV, dst.reg + (int)positions[k], v.reg + (int)k);
          return;
        }
        for (const auto& ix : s.lhs_idx)
          if (ix.name != "IndexSingle")
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
          if (fold_is_certain(s.lhs) && try_cint(s.rhs, &value) &&
              known != known_int_arrays.end() && flat >= 0 &&
              (size_t)flat < known->second.size())
            known->second[(size_t)flat] = value;
          else
            known_int_arrays.erase(s.lhs);
        }
        return;
      }
      case mir::Stmt::Return:
        // A return under a runtime branch is a control-flow join this flat
        // program has no way to express; the interpreter still handles it.
        if (branch_depth)
          bail("return inside a data-dependent branch" +
               (inline_stack.empty() ? std::string()
                                     : " in " + inline_stack.back()));
        throw Returned{s.has_init ? expr(s.rhs) : Range{0, 0}};
      case mir::Stmt::Break:
        if (loops.empty()) bail("break outside a loop");
        if (branch_depth || loops.back().structured) {
          loops.back().breaks.push_back(emit(Program::JMP, 0));
          return;
        }
        throw CompileBreak{};
      case mir::Stmt::Continue:
        if (loops.empty()) bail("continue outside a loop");
        if (branch_depth || loops.back().structured) {
          loops.back().continues.push_back(emit(Program::JMP, 0));
          return;
        }
        throw CompileContinue{};
      case mir::Stmt::For: {
        const long lo = cint(s.lower), hi = cint(s.upper);
        loops.push_back({});
        bool broken = false;
        for (long v = lo; v <= hi; ++v) {
          ints[s.loopvar] = {v};
          int_decl_at[s.loopvar] = {branch_depth, loops.size()};
          try {
            for (const auto& k : s.body) stmt(k);
          } catch (CompileContinue&) {
          } catch (CompileBreak&) {
            broken = true;
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
        return;
      }
      case mir::Stmt::While: {
        // The register program already has JZ/JMP and is replayed under
        // autodiff for an island.  Use those instructions directly so a
        // finite data loop and a parameter-sensitive recurrence have their
        // actual trip count, with no arbitrary lowering-time cap.
        std::set<std::string> written;
        for (const auto& child : s.body) assigned_names(child, &written);
        for (const std::string& name : written) reify_written_int(name);

        const int head = (int)p.code.size();
        const Range cv = expr(s.cond);
        if (!is_scalar(cv)) bail("while condition on a container");
        const int exit = emit(Program::JZ, 0, cv.reg);
        loops.push_back({});
        loops.back().structured = true;
        structured_while_seen = true;
        ++structured_while_depth;
        for (const auto& k : s.body) stmt(k);
        --structured_while_depth;
        for (int jump : loops.back().continues) p.code[(size_t)jump].dst = head;
        emit(Program::JMP, head);
        p.code[(size_t)exit].dst = (int)p.code.size();
        for (int jump : loops.back().breaks)
          p.code[(size_t)jump].dst = (int)p.code.size();
        loops.pop_back();
        return;
      }
      case mir::Stmt::IfElse: {
        long c;
        if (try_cint(s.cond, &c)) {
          if (c != 0 && !s.body.empty()) stmt(s.body[0]);
          if (c == 0 && s.body.size() > 1) stmt(s.body[1]);
          return;
        }
        if (s.body.size() == 2) {
          mir::Stmt then_effects = s.body[0];
          mir::Stmt else_effects = s.body[1];
          mir::Expr then_value, else_value;
          if (peel_terminal_return(&then_effects, &then_value) &&
              peel_terminal_return(&else_effects, &else_value)) {
            const Range cv = expr(s.cond);
            if (!is_scalar(cv)) bail("branch on a container");
            ++branch_depth;
            const int jz = emit(Program::JZ, 0, cv.reg);
            stmt(then_effects);
            const Range tv = expr(then_value);
            const int dst = alloc(tv.len);
            for (int k = 0; k < tv.len; ++k)
              emit(Program::MOV, dst + k, tv.reg + k);
            const int jmp = emit(Program::JMP, 0);
            p.code[(size_t)jz].dst = (int)p.code.size();
            stmt(else_effects);
            const Range ev = expr(else_value);
            if (!same_view(tv, ev))
              bail("conditional returns have different logical views");
            for (int k = 0; k < ev.len; ++k)
              emit(Program::MOV, dst + k, ev.reg + k);
            p.code[(size_t)jmp].dst = (int)p.code.size();
            --branch_depth;
            Range out = tv;
            out.reg = dst;
            throw Returned{out};
          }
        }
        const Range cv = expr(s.cond);
        if (!is_scalar(cv)) bail("branch on a container");
        ++branch_depth;
        const int jz = emit(Program::JZ, 0, cv.reg);
        if (!s.body.empty()) stmt(s.body[0]);
        if (s.body.size() > 1) {
          const int jmp = emit(Program::JMP, 0);
          p.code[(size_t)jz].dst = (int)p.code.size();
          stmt(s.body[1]);
          p.code[(size_t)jmp].dst = (int)p.code.size();
        } else {
          p.code[(size_t)jz].dst = (int)p.code.size();
        }
        --branch_depth;
        return;
      }
      case mir::Stmt::Block:
      case mir::Stmt::SList:
        for (const auto& k : s.body) stmt(k);
        return;
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
        return;
      }
      case mir::Stmt::NRFunApp:
        if (s.fn_name == "FnValidateSize") return;
        if (const auto action = message_action(s.fn_name)) {
          Program::Message message;
          message.spec = lower_message_arguments(
              s.fn_args, [&](const mir::Expr& argument) {
                const Range value = expr(argument);
                message.value_reg.push_back(value.reg);
                message.value_len.push_back(value.len);
              });
          p.messages.push_back(std::move(message));
          emit(*action == MessageAction::Reject ? Program::REJECT
                                                : Program::PRINT,
               0, (int)p.messages.size() - 1);
          return;
        }
        bail("statement function " + s.fn_name +
             " requires the MIR interpreter");
      case mir::Stmt::Skip:
        return;
      default:
        bail("statement");
    }
  }

  Range inline_call(const mir::FunDef& f, const std::vector<InlineArg>& args) {
    if (args.size() != f.arg_names.size()) bail("function argument mismatch");
    if (++inline_depth > 32) {
      --inline_depth;
      bail("function inlining too deep");
    }
    // Callee scope: save the caller's bindings, install the parameters, and
    // restore afterwards. Registers are never reused, so nothing aliases.
    auto saved_reals = reals;
    auto saved_ints = ints;
    auto saved_known_reals = known_reals;
    auto saved_known_int_arrays = known_int_arrays;
    auto saved_known_int_array_dims = known_int_array_dims;
    auto saved_int_array_names = int_array_names;
    auto saved_deferred_shapes = deferred_shapes;
    auto saved_int_decl_at = int_decl_at;
    auto saved_extern_bound = extern_bound;
    const int saved_branch_depth = branch_depth;
    reals.clear();
    ints.clear();
    known_reals.clear();
    known_int_arrays.clear();
    known_int_array_dims.clear();
    int_array_names.clear();
    deferred_shapes.clear();
    int_decl_at.clear();
    extern_bound.clear();
    branch_depth = 0;
    inline_stack.push_back(f.name);
    std::set<std::string> assigned;
    for (const auto& statement : f.body) assigned_names(statement, &assigned);
    for (size_t k = 0; k < f.arg_names.size(); ++k) {
      if (args[k].is_const_int) {
        if (args[k].int_dims.empty()) {
          ints[f.arg_names[k]] = args[k].ints;
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
      for (const auto& s : f.body) stmt(s);
      bail("function " + f.name + " returned no value");
    } catch (Returned& r) {
      out = r.r;
    } catch (...) {
      inline_stack.pop_back();
      reals = std::move(saved_reals);
      ints = std::move(saved_ints);
      known_reals = std::move(saved_known_reals);
      known_int_arrays = std::move(saved_known_int_arrays);
      known_int_array_dims = std::move(saved_known_int_array_dims);
      int_array_names = std::move(saved_int_array_names);
      deferred_shapes = std::move(saved_deferred_shapes);
      int_decl_at = std::move(saved_int_decl_at);
      extern_bound = std::move(saved_extern_bound);
      branch_depth = saved_branch_depth;
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
    int_decl_at = std::move(saved_int_decl_at);
    extern_bound = std::move(saved_extern_bound);
    branch_depth = saved_branch_depth;
    --inline_depth;
    return out;
  }
};

}  // namespace stanli

#endif
