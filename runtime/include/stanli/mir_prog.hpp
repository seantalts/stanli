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

#include <stanli/mir.hpp>
#include <stanli/program.hpp>

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

enum class ViewKind : uint8_t { Flat, Vector, RowVector, Matrix, Array };

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
};

struct Bail {
  std::string why;
};

struct ProgramCompiler {
  Program& p;
  const std::map<std::string, const mir::FunDef*>& funs;
  std::map<std::string, Range> reals;
  std::map<std::string, std::vector<long>> ints;
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
  // Where `target +=` accumulates, or -1 when the region may not have
  // one. Set by the caller, which also seeds it to zero.
  int target_reg = -1;
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
    return e.args.size() == 1 &&
           (e.name == "rows" || e.name == "cols" || e.name == "size" ||
            e.name == "num_elements" || e.name == "FnLength");
  }

  // The answers match the graph lowering's (lower.cpp): `size` and
  // `FnLength` are an array's first extent and any other value's length,
  // `num_elements` is the total, and rows/cols read a matrix's declared
  // extents or a vector's orientation.
  long shape_query(const std::string& fn, const Range& v) {
    if (v.kind == ViewKind::Array) {
      const std::vector<int64_t> dims =
          v.dims.empty() ? std::vector<int64_t>{v.len} : v.dims;
      if (fn == "size" || fn == "FnLength") return (long)dims.front();
      if (fn == "num_elements") return v.len;
      bail(fn + " is undefined for an array value");
    }
    if (fn == "rows")
      return (long)(v.kind == ViewKind::Matrix      ? v.rows
                    : v.kind == ViewKind::RowVector ? 1
                                                    : v.len);
    if (fn == "cols")
      return (long)(v.kind == ViewKind::Matrix   ? v.cols
                    : v.kind == ViewKind::Vector ? 1
                                                 : v.len);
    return v.len;
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
          if (int_operand(e.args[0]) && int_operand(e.args[1])) {
            if (e.name == "Equals__") return cint(e.args[0]) == cint(e.args[1]);
            if (e.name == "NEquals__")
              return cint(e.args[0]) != cint(e.args[1]);
            if (e.name == "Less__") return cint(e.args[0]) < cint(e.args[1]);
            if (e.name == "Leq__") return cint(e.args[0]) <= cint(e.args[1]);
            if (e.name == "Greater__") return cint(e.args[0]) > cint(e.args[1]);
            if (e.name == "Geq__") return cint(e.args[0]) >= cint(e.args[1]);
          }
        }
        if (e.args.size() == 1 && e.name == "PMinus__") return -cint(e.args[0]);
        if (e.args.size() == 1 && int_operand(e.args[0]) &&
            (e.name == "PNot__" || e.name == "logical_negation"))
          return cint(e.args[0]) == 0;
        // A declared extent, a loop bound or an index written as a shape
        // query: `matrix[rows(m), cols(m)] out;`, `for (i in 1:rows(m))`.
        // Before this, only a shape query in a real-valued context was
        // answered, and these were refused as unknown integer functions.
        if (is_shape_query(e)) {
          Range v;
          if (static_view(e.args[0], &v)) return shape_query(e.name, v);
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
    } else if (type == "UArray") {
      bail("array expressions are unsupported by the register program");
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
          // A column, `m[, j]`, is a contiguous run this could return as a
          // view; nothing reaches it yet, so it stays refused rather than
          // untested.
          bail("matrix index form");
        }

        if (b.kind == ViewKind::Vector || b.kind == ViewKind::RowVector ||
            b.kind == ViewKind::Flat) {
          if (e.args.size() != 2) bail("one-dimensional index form");
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

        // Arrays use Stan's first-index-fastest storage order.  A selection
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
        for (size_t d = n_idx; d < dims.size(); ++d) {
          if (dims[d] < 0 || dims[d] > kMaxRegs)
            bail("array selection has an invalid omitted extent");
          positions.emplace_back();
          positions.back().reserve((size_t)dims[d]);
          for (int64_t k = 0; k < dims[d]; ++k) positions.back().push_back(k);
          drops.push_back(false);
        }
        int64_t width = 1;
        std::vector<int64_t> out_dims;
        for (size_t d = 0; d < positions.size(); ++d) {
          if (!positions[d].empty() &&
              width > kMaxRegs / (int64_t)positions[d].size())
            bail("array selection needs too many registers");
          width *= (int64_t)positions[d].size();
          if (!drops[d]) out_dims.push_back((int64_t)positions[d].size());
        }
        const int r = alloc((int)width);
        const std::vector<int64_t> offsets =
            graph_array_offsets(dims, b.leaf, positions);
        for (size_t k = 0; k < offsets.size(); ++k)
          emit(Program::MOV, r + (int)k, b.reg + (int)offsets[k]);
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

  // The diagonal, on the same terms as a row: column-major storage puts
  // its elements rows + 1 apart, and Eigen's stops at the shorter side.
  Range matrix_diagonal(const Range& m) {
    const int64_t n = m.rows < m.cols ? m.rows : m.cols;
    const int r = alloc((int)n);
    for (int64_t k = 0; k < n; ++k)
      emit(Program::MOV, r + (int)k, m.reg + (int)(k * (m.rows + 1)));
    Range out{r, (int)n};
    out.kind = ViewKind::Vector;
    return out;
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

  Range fun(const mir::Expr& e) {
    // A shape query is a constant whatever surrounds it. Ahead of every
    // other case because `FnLength` is an internal function and the rest
    // are library ones, and they are all answered the same way: from the
    // named value's view where there is one, and otherwise from the view
    // of the value the argument builds.
    if (is_shape_query(e)) {
      Range v;
      if (!static_view(e.args[0], &v)) v = expr(e.args[0]);
      return {konst((double)shape_query(e.name, v)), 1};
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
    if ((e.name == "Transpose__" || e.name == "transpose") &&
        e.args.size() == 1) {
      Range value = expr(e.args[0]);
      if (value.kind == ViewKind::Vector) {
        value.kind = ViewKind::RowVector;
        return value;
      }
      if (value.kind == ViewKind::RowVector) {
        value.kind = ViewKind::Vector;
        return value;
      }
      if (value.kind != ViewKind::Matrix)
        bail("transpose needs a vector, row vector, or matrix");
      const int r = alloc(value.len);
      for (int64_t j = 0; j < value.cols; ++j)
        for (int64_t i = 0; i < value.rows; ++i)
          emit(Program::MOV, r + (int)(i * value.cols + j),
               value.reg + (int)(j * value.rows + i));
      Range out{r, value.len};
      out.kind = ViewKind::Matrix;
      out.rows = value.cols;
      out.cols = value.rows;
      return out;
    }
    if (e.name == "tcrossprod" && e.args.size() == 1)
      return matrix_gram(expr(e.args[0]), false);
    if (e.name == "crossprod" && e.args.size() == 1)
      return matrix_gram(expr(e.args[0]), true);
    if (e.name == "add_diag" && e.args.size() == 2) {
      const Range input = expr(e.args[0]);
      const Range diagonal = expr(e.args[1]);
      if (input.kind != ViewKind::Matrix)
        bail("add_diag requires a matrix first argument");
      const int64_t n = std::min(input.rows, input.cols);
      if (!is_scalar(diagonal) && ((diagonal.kind != ViewKind::Vector &&
                                    diagonal.kind != ViewKind::RowVector) ||
                                   diagonal.len != n))
        bail("add_diag diagonal size mismatch");
      const int r = alloc(input.len);
      for (int k = 0; k < input.len; ++k)
        emit(Program::MOV, r + k, input.reg + k);
      for (int64_t k = 0; k < n; ++k)
        emit(Program::ADD, r + (int)(k * (input.rows + 1)),
             r + (int)(k * (input.rows + 1)),
             diagonal.reg + (is_scalar(diagonal) ? 0 : (int)k));
      Range out = input;
      out.reg = r;
      return out;
    }
    if (e.name == "matrix_exp" && e.args.size() == 1) {
      const Range input = expr(e.args[0]);
      if (input.kind != ViewKind::Matrix || input.rows != input.cols)
        bail("matrix_exp requires a square matrix");
      const int r = alloc(input.len);
      if (input.len != 0)
        p.code.push_back(Program::Instr{Program::MATRIX_EXP, r, input.reg,
                                        (int32_t)input.rows,
                                        (int32_t)input.cols, input.len});
      Range out = input;
      out.reg = r;
      return out;
    }
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
    if (e.name == "quad_form_sym" && e.args.size() == 2) {
      const Range a = expr(e.args[0]);
      const Range b = expr(e.args[1]);
      if (a.kind != ViewKind::Matrix || a.rows != a.cols)
        bail("quad_form_sym requires a square first matrix");
      if ((b.kind != ViewKind::Matrix && b.kind != ViewKind::Vector) ||
          (b.kind == ViewKind::Matrix && b.rows != a.rows) ||
          (b.kind == ViewKind::Vector && b.len != a.rows))
        bail("quad_form_sym second argument size mismatch");
      const int64_t ncol = b.kind == ViewKind::Vector ? 1 : b.cols;
      const int64_t width = ncol * ncol;
      if (width > kMaxRegs) bail("quad_form_sym result is too large");
      const int r = alloc((int)width);
      const int32_t encoded_rows =
          b.kind == ViewKind::Vector ? -(int32_t)a.rows : (int32_t)a.rows;
      if (a.rows == 0) {
        const std::vector<double> zero((size_t)width, 0.0);
        emit_const(r, zero.data(), (int)width);
      } else {
        p.code.push_back(Program::Instr{Program::QUAD_FORM_SYM, r, a.reg, b.reg,
                                        encoded_rows, (int32_t)width});
      }
      if (b.kind == ViewKind::Vector) return {r, 1};
      Range out{r, (int)width};
      out.kind = ViewKind::Matrix;
      out.rows = out.cols = ncol;
      return out;
    }
    if (e.fn_lib == mir::Expr::Lib::UserDefined) {
      auto it = funs.find(e.name);
      if (it == funs.end()) bail("unknown function " + e.name);
      std::vector<InlineArg> args;
      args.reserve(e.args.size());
      for (const auto& a : e.args) {
        InlineArg arg;
        long v;
        if (a.type_ == "UInt" && try_cint(a, &v)) {
          arg.is_const_int = true;
          arg.ints = {v};
        } else if (a.unsized.depth != 0 &&
                   a.unsized.leaf == mir::UnsizedLeaf::Int &&
                   try_cints(a, &arg.ints)) {
          // Function arguments are rebound in a fresh compiler scope. Carry
          // a data-only selector as values, not merely as its register Range,
          // so the callee (and a nested inline call) can still use it in
          // IndexMulti or a compile-time scalar indexed read.
          arg.is_const_int = true;
          Range view;
          if (!static_view(a, &view) || view.kind != ViewKind::Array)
            bail("integer function argument has no static array view");
          arg.int_dims =
              view.dims.empty() ? std::vector<int64_t>{view.len} : view.dims;
        } else {
          arg.real = expr(a);
        }
        args.push_back(std::move(arg));
      }
      return inline_call(*it->second, args);
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
            array_leaf = parts.front().kind;
            if (array_leaf == ViewKind::Array)
              bail(
                  "array literal element view is unsupported by the "
                  "register program");
            for (const Range& q : parts)
              if (!same_view(q, parts.front()))
                bail("array literal elements have different logical views");
            if (array_leaf == ViewKind::Matrix)
              array_leaf_dims = {parts.front().rows, parts.front().cols};
            else
              array_leaf_dims = {(int64_t)parts.front().len};
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
    if (e.args.empty() && e.name == "negative_infinity")
      return {konst(-std::numeric_limits<double>::infinity()), 1};
    if (e.args.size() == 2 && e.name == "append_array") {
      const Range a = expr(e.args[0]);
      const Range b = expr(e.args[1]);
      if (a.kind != ViewKind::Array || b.kind != ViewKind::Array)
        bail("append_array requires two array arguments");
      if (e.args[0].unsized.depth != e.args[1].unsized.depth ||
          e.args[0].unsized.leaf != e.args[1].unsized.leaf)
        bail("append_array element logical views differ");

      const auto dimensions = [&](const Range& value) {
        return value.dims.empty() ? std::vector<int64_t>{value.len}
                                  : value.dims;
      };
      const auto validate_dimensions = [&](const Range& value,
                                           const std::vector<int64_t>& dims) {
        int64_t product = 1;
        for (int64_t extent : dims) {
          if (extent < 0 ||
              (extent != 0 && product > (int64_t)kMaxRegs / extent))
            bail("append_array has an invalid element shape");
          product *= extent;
        }
        if (product != value.len)
          bail("append_array storage and logical shape disagree");
      };
      const std::vector<int64_t> adims = dimensions(a);
      const std::vector<int64_t> bdims = dimensions(b);
      validate_dimensions(a, adims);
      validate_dimensions(b, bdims);
      if (adims.size() != bdims.size())
        bail("append_array element shapes differ");
      const int64_t a_outer = adims.front();
      const int64_t b_outer = bdims.front();
      if (a_outer != 0 && b_outer != 0 &&
          !std::equal(adims.begin() + 1, adims.end(), bdims.begin() + 1))
        bail("append_array element shapes differ");
      if (a_outer > std::numeric_limits<int64_t>::max() - b_outer ||
          b.len > kMaxRegs - a.len)
        bail("append_array result is too large");

      std::vector<int64_t> out_dims =
          a_outer == 0 && b_outer != 0 ? bdims : adims;
      out_dims[0] = a_outer + b_outer;
      const int r = alloc(a.len + b.len);
      for (int k = 0; k < a.len; ++k) emit(Program::MOV, r + k, a.reg + k);
      for (int k = 0; k < b.len; ++k)
        emit(Program::MOV, r + a.len + k, b.reg + k);
      Range out{r, a.len + b.len};
      out.kind = ViewKind::Array;
      out.dims = std::move(out_dims);
      return out;
    }
    if (e.args.size() == 1 && e.name == "diagonal") {
      const Range a = expr(e.args[0]);
      if (a.kind != ViewKind::Matrix)
        bail("diagonal requires a matrix logical view");
      return matrix_diagonal(a);
    }
    if (e.args.size() == 1 && e.name == "diag_matrix") {
      const Range diagonal = expr(e.args[0]);
      if (diagonal.kind != ViewKind::Vector &&
          diagonal.kind != ViewKind::RowVector)
        bail("diag_matrix requires a vector");
      if (diagonal.len != 0 && diagonal.len > kMaxRegs / diagonal.len)
        bail("diag_matrix result is too large");
      const int n = diagonal.len;
      const int r = alloc(n * n);
      const double zero = 0.0;
      for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
          const int dst = r + j * n + i;
          if (i == j)
            emit(Program::MOV, dst, diagonal.reg + i);
          else
            emit_const(dst, &zero, 1);
        }
      Range out{r, n * n};
      out.kind = ViewKind::Matrix;
      out.rows = out.cols = n;
      return out;
    }
    if (e.args.size() == 2 && e.name == "rep_vector") {
      // The register file is a flat run of doubles, so a vector of one
      // repeated value is a run the compiler fills -- the same fill a
      // declaration's default uses. The length has to be a compile-time
      // integer, which is what every extent inside a region is.
      const long n = cint(e.args[1]);
      if (n < 0) bail("rep_vector of a negative length");
      Range out{0, (int)n};
      out.kind = ViewKind::Vector;
      if (n == 0) {
        out.reg = alloc(0);
        return out;
      }
      // A literal value is the whole run in one instruction; anything else
      // is computed once and copied, which is what its adjoint wants too:
      // each copy adds into the one source cell, summing the broadcast.
      if (e.args[0].kind == mir::Expr::LitInt ||
          e.args[0].kind == mir::Expr::LitReal) {
        const double v = e.args[0].kind == mir::Expr::LitInt
                             ? (double)e.args[0].lit_i
                             : e.args[0].lit;
        out.reg = alloc((int)n);
        const std::vector<double> fill((size_t)n, v);
        emit_const(out.reg, fill.data(), (int)n);
        return out;
      }
      const Range v = expr(e.args[0]);
      if (!is_scalar(v)) bail("rep_vector of a container");
      out.reg = alloc((int)n);
      for (long k = 0; k < n; ++k) emit(Program::MOV, out.reg + (int)k, v.reg);
      return out;
    }
    if (e.args.size() == 2 && e.name == "rep_row_vector") {
      const long n = cint(e.args[1]);
      if (n < 0) bail("rep_row_vector of a negative length");
      const Range value = expr(e.args[0]);
      if (!is_scalar(value)) bail("rep_row_vector of a container");
      const int r = alloc((int)n);
      for (long i = 0; i < n; ++i) emit(Program::MOV, r + (int)i, value.reg);
      Range out{r, (int)n};
      out.kind = ViewKind::RowVector;
      return out;
    }
    if (e.name == "rep_matrix" && (e.args.size() == 2 || e.args.size() == 3)) {
      const Range value = expr(e.args[0]);
      int64_t rows = 0, cols = 0;
      if (e.args.size() == 3) {
        if (!is_scalar(value)) bail("three-argument rep_matrix needs a scalar");
        rows = cint(e.args[1]);
        cols = cint(e.args[2]);
      } else if (value.kind == ViewKind::Vector) {
        rows = value.len;
        cols = cint(e.args[1]);
      } else if (value.kind == ViewKind::RowVector) {
        rows = cint(e.args[1]);
        cols = value.len;
      } else {
        bail("two-argument rep_matrix needs a vector or row vector");
      }
      if (rows < 0 || cols < 0 || (rows != 0 && cols > kMaxRegs / rows))
        bail("rep_matrix has an invalid or excessive size");
      const int r = alloc((int)(rows * cols));
      for (int64_t j = 0; j < cols; ++j)
        for (int64_t i = 0; i < rows; ++i) {
          int source = value.reg;
          if (value.kind == ViewKind::Vector) source += (int)i;
          if (value.kind == ViewKind::RowVector) source += (int)j;
          emit(Program::MOV, r + (int)(j * rows + i), source);
        }
      Range out{r, (int)(rows * cols)};
      out.kind = ViewKind::Matrix;
      out.rows = rows;
      out.cols = cols;
      return out;
    }
    if (e.args.size() == 1 && e.name == "max") {
      const Range a = expr(e.args[0]);
      const int r = alloc(1);
      p.code.push_back(
          Program::Instr{Program::MAX_RANGE, r, a.reg, 0, 0, a.len});
      return {r, 1};
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
        int argv[kMaxDensityArgs];
        for (int k = 0; k < arity; ++k) {
          const Range a = expr(e.args[(size_t)k]);
          // One lp per call: a vectorized density inside a branch would
          // have to sum over its arguments, which this does not do.
          if (!is_scalar(a)) bail(e.name + " on a container");
          argv[k] = a.reg;
        }
        // Three arguments or fewer ride in the instruction; a fourth
        // needs the contiguous form, so copy them into a block.
        int a0 = argv[0], a1 = arity > 1 ? argv[1] : 0;
        int a2 = arity > 2 ? argv[2] : 0;
        if (arity > 3) {
          a0 = alloc(arity);
          for (int k = 0; k < arity; ++k) emit(Program::MOV, a0 + k, argv[k]);
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
        int64_t rows = 0, inner = 0, cols = 0;
        ViewKind result_kind = ViewKind::Flat;
        if (a.kind == ViewKind::Matrix && b.kind == ViewKind::Matrix) {
          rows = a.rows;
          inner = a.cols;
          cols = b.cols;
          if (inner != b.rows) bail("matrix multiplication size mismatch");
          result_kind = ViewKind::Matrix;
        } else if (a.kind == ViewKind::Matrix && b.kind == ViewKind::Vector) {
          rows = a.rows;
          inner = a.cols;
          cols = 1;
          if (inner != b.len) bail("matrix-vector size mismatch");
          result_kind = ViewKind::Vector;
        } else if (a.kind == ViewKind::RowVector &&
                   b.kind == ViewKind::Matrix) {
          rows = 1;
          inner = a.len;
          cols = b.cols;
          if (inner != b.rows) bail("row-vector matrix size mismatch");
          result_kind = ViewKind::RowVector;
        } else if (a.kind == ViewKind::RowVector &&
                   b.kind == ViewKind::Vector) {
          rows = cols = 1;
          inner = a.len;
          if (inner != b.len) bail("dot-product size mismatch");
        } else if (a.kind == ViewKind::Vector &&
                   b.kind == ViewKind::RowVector) {
          rows = a.len;
          inner = 1;
          cols = b.len;
          result_kind = ViewKind::Matrix;
        } else {
          bail(
              "container multiplication is unsupported by the register "
              "program");
        }

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
      else if (e.name == "Greater__")
        c = Program::GT;
      else if (e.name == "Geq__")
        c = Program::GE;
      else if (e.name == "Less__")
        c = Program::LT;
      else if (e.name == "Leq__")
        c = Program::LE;
      else if (e.name == "Equals__")
        c = Program::EQ;
      else if (e.name == "NEquals__")
        c = Program::NE;
      else
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
      if (e.name == "sum") {
        if (a.len == 0) return {konst(0.0), 1};
        const int r = alloc(1);
        emit(Program::MOV, r, a.reg);
        for (int i = 1; i < a.len; ++i) emit(Program::ADD, r, r, a.reg + i);
        return {r, 1};
      }
      // Predicates, spelled on the comparison opcodes rather than opcodes of
      // their own: both read through value_of, so neither carries an adjoint
      // edge, which is what a 0/1 answer wants. x != x holds for NaN alone.
      if (e.name == "is_nan" || e.name == "PNot__") {
        const int rhs = e.name == "is_nan" ? -1 : konst(0.0);
        const Program::Code c = e.name == "is_nan" ? Program::NE : Program::EQ;
        const int r = alloc(a.len);
        for (int i = 0; i < a.len; ++i)
          emit(c, r + i, a.reg + i, rhs < 0 ? a.reg + i : rhs);
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

          reals.erase(s.decl_id);
          ints.erase(s.decl_id);
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
          const std::vector<int64_t> offsets =
              graph_array_offsets(dims, dst.leaf, positions);
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
    auto saved_known_int_arrays = known_int_arrays;
    auto saved_known_int_array_dims = known_int_array_dims;
    auto saved_int_array_names = int_array_names;
    auto saved_deferred_shapes = deferred_shapes;
    auto saved_int_decl_at = int_decl_at;
    auto saved_extern_bound = extern_bound;
    const int saved_branch_depth = branch_depth;
    reals.clear();
    ints.clear();
    known_int_arrays.clear();
    known_int_array_dims.clear();
    int_array_names.clear();
    deferred_shapes.clear();
    int_decl_at.clear();
    extern_bound.clear();
    branch_depth = 0;
    inline_stack.push_back(f.name);
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
