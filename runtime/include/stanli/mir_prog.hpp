// MIR -> Program: the shared front end.
//
// Two callers compile MIR into the register machine. An ODE right-hand
// side has to stay callable at runtime because the integrator picks the
// times (ode_prog.hpp). A region whose control flow depends on a
// parameter has to become a program because it cannot become graph ops
// at all: `if (theta > 0)` has no op-graph form, and until this existed
// it was a compile error (lower.cpp).
//
// The shape of the problem is what keeps this small: every size, every
// index and every integer is known at compile time -- loop bounds come
// from data, array lengths from declarations -- so only the reals need
// registers, and loops unroll as they compile.
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
};

// One UDF argument in source order. Compile-time integers live outside the
// register file; every other value is a Range. Keeping the tag beside the
// value prevents the old real/int partition from reordering mixed calls.
struct InlineArg {
  Range real;
  std::vector<long> ints;
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
  int branch_depth = 0;  // inside a branch on a runtime value
  int inline_depth = 0;
  // A name that is neither a local nor a compile-time integer. The ODE
  // caller leaves this empty (its arguments are all bound up front);
  // lowering installs a hook that allocates registers for the graph slot
  // backing the name and records it as a live-in. Returning false means
  // "no such value", and the compile bails.
  std::function<bool(const std::string&, Range*)> bind_extern;
  // Where `target +=` accumulates, or -1 when the region may not have
  // one. Set by the caller, which also seeds it to zero.
  int target_reg = -1;
  // Register runs allocated by the zero-length adoption in Assignment,
  // which is the one allocation site whose write can sit under a jump.
  // finish() fills them with NaN ahead of the program, restoring the
  // contract run_program states (program.hpp): every register is written
  // before it is read.
  std::vector<std::pair<int, int>> late_bound;

  // Registers are never recycled. Right-hand sides are a few lines over a
  // handful of states, so the count stays in the dozens; the cap is a
  // backstop against a pathological unroll, and trips into the interpreter
  // rather than into a huge allocation.
  static constexpr int kMaxRegs = 1 << 16;

  [[noreturn]] void bail(const std::string& why) { throw Bail{why}; }

  int alloc(int n) {
    if (p.n_regs + n > kMaxRegs)
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

  // ---- compile-time integers ----------------------------------------------
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
        if (e.args.size() != 2 || e.args[1].name != "IndexSingle")
          bail("integer index form");
        if (e.args[0].kind != mir::Expr::Var) bail("integer index base");
        auto it = ints.find(e.args[0].name);
        if (it == ints.end()) bail("integer array " + e.args[0].name);
        const long ix = cint(e.args[1].args[0]);
        if (ix < 1 || (size_t)ix > it->second.size())
          bail("integer index range");
        return it->second[(size_t)ix - 1];
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
        }
        if (e.args.size() == 1 && e.name == "PMinus__") return -cint(e.args[0]);
        bail("integer function " + e.name);
      default:
        bail("integer expression");
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

  static bool same_view(const Range& a, const Range& b) {
    if (a.kind != b.kind) return false;
    if (a.kind == ViewKind::Flat) return a.len == 1 && b.len == 1;
    if (a.kind == ViewKind::Vector || a.kind == ViewKind::RowVector)
      return a.len == b.len;
    if (a.kind == ViewKind::Array) return a.len == b.len;
    return a.rows == b.rows && a.cols == b.cols;
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

  Range declared(Range r, const mir::SizedType& type) {
    if (type.base == "SArray") {
      if (type.elem_base != "SReal" || type.dims.size() != 1)
        bail(
            "only one-dimensional scalar-array declarations are supported by "
            "the register program");
      r.kind = ViewKind::Array;
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

  // ---- expressions ---------------------------------------------------------
  Range expr(const mir::Expr& e) {
    switch (e.kind) {
      case mir::Expr::LitInt:
        return {konst((double)e.lit_i), 1};
      case mir::Expr::LitReal:
        return {konst(e.lit), 1};
      case mir::Expr::Var: {
        auto it = reals.find(e.name);
        if (it != reals.end()) return it->second;
        auto ii = ints.find(e.name);
        if (ii != ints.end()) {
          const std::vector<double> vals(ii->second.begin(), ii->second.end());
          const int r = alloc((int)vals.size());
          emit_const(r, vals.data(), (int)vals.size());
          return {r, (int)vals.size()};
        }
        Range ext;
        if (bind_extern && bind_extern(e.name, &ext)) {
          reals[e.name] = ext;
          return ext;
        }
        bail("unknown variable " + e.name);
      }
      case mir::Expr::Indexed: {
        // A single index into a matrix selects a row in Stan. Range carries
        // only a flat width, so treating that index as one scalar silently
        // changes the expression. Refuse until ProgValue carries shape; the
        // MIR interpreter is the complete path for this operation.
        if (e.args.size() == 2 && e.args[1].name == "IndexSingle" &&
            e.args[0].type_ == "UMatrix")
          bail("matrix row indexing is unsupported by the register program");
        const Range b = expr(e.args[0]);
        if (e.args.size() == 2 && e.args[1].name == "IndexAll") return b;
        if (e.args.size() != 2 || e.args[1].name != "IndexSingle")
          bail("index form");
        const bool scalar_result =
            e.type_ == "UReal" || e.type_ == "UInt" || e.type_ == "UComplex";
        if (!scalar_result)
          bail("container element indexing requires a logical layout");
        if (b.kind != ViewKind::Array && b.kind != ViewKind::Vector &&
            b.kind != ViewKind::RowVector && b.kind != ViewKind::Flat)
          bail("indexing this logical view is unsupported");
        const long ix = cint(e.args[1].args[0]);
        if (ix < 1 || ix > b.len) bail("index out of the declared range");
        return {b.reg + (int)ix - 1, 1};
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

  Range fun(const mir::Expr& e) {
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
          total += parts.back().len;
        }
        if (e.name == "FnMakeArray") {
          for (const Range& q : parts)
            if (!is_scalar(q))
              bail(
                  "only flat scalar arrays are supported by the register "
                  "program");
        }
        const int r = alloc(total);
        int at = 0;
        for (const Range& q : parts)
          for (int k = 0; k < q.len; ++k)
            emit(Program::MOV, r + at++, q.reg + k);
        Range out{r, total};
        out.kind =
            e.name == "FnMakeRowVec" ? ViewKind::RowVector : ViewKind::Array;
        return out;
      }
      bail("internal function " + e.name);
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
              "`~` inside a parameter-dependent region (write it as "
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
      if (!a_scalar && !b_scalar && !same_view(a, b))
        bail("binary " + e.name + " on different logical views");
      // `multiply` rides with `Times__` here for the same reason it does
      // in the graph lowering: on two containers it is linear algebra,
      // not the elementwise MUL the register file would emit.
      if ((e.name == "Times__" || e.name == "multiply") && !a_scalar &&
          !b_scalar) {
        if (a.kind == ViewKind::Matrix || b.kind == ViewKind::Matrix)
          bail("matrix multiplication is unsupported by the register program");
        bail("container multiplication is unsupported by the register program");
      }
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
      else if (e.name == "Divide__" || e.name == "EltDivide__" ||
               e.name == "divide" || e.name == "elt_divide")
        c = Program::DIV;
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

  int64_t sized_len(const mir::SizedType& t) {
    int64_t n = 1;
    for (const auto& d : t.dims) n *= cint(d);
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
    if (late_bound.empty()) return;
    std::vector<Program::Instr> prologue;
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
        if (s.decl_type.base == "SInt" ||
            (s.decl_type.base == "SArray" && s.decl_type.elem_base == "SInt")) {
          if (s.has_init) {
            ints[s.decl_id] = {cint(s.init)};
          } else {
            ints[s.decl_id] =
                std::vector<long>((size_t)sized_len(s.decl_type), 0);
          }
          return;
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
        } else {
          Range view;
          declare(s.decl_id, (int)sized_len(s.decl_type),
                  declared(view, s.decl_type));
        }
        return;
      }
      case mir::Stmt::Assignment: {
        if (ints.count(s.lhs) && s.lhs_idx.empty()) {
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
            it = reals.find(s.lhs);
          }
        }
        if (it == reals.end()) bail("assignment to undeclared " + s.lhs);
        const Range dst = it->second;
        const Range v = expr(s.rhs);
        if (s.lhs_idx.empty()) {
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
            return;
          }
          if (v.len != dst.len) bail("assignment width mismatch for " + s.lhs);
          if (!same_view(v, dst))
            bail("assignment logical view mismatch for " + s.lhs);
          for (int k = 0; k < v.len; ++k)
            emit(Program::MOV, dst.reg + k, v.reg + k);
          return;
        }
        if (s.lhs_idx.size() != 1 || s.lhs_idx[0].name != "IndexSingle")
          bail("assignment index form for " + s.lhs);
        const long ix = cint(s.lhs_idx[0].args[0]);
        if (ix < 1 || ix > dst.len) bail("assignment index range for " + s.lhs);
        if (!is_scalar(v)) bail("element assignment from a container");
        emit(Program::MOV, dst.reg + (int)ix - 1, v.reg);
        return;
      }
      case mir::Stmt::Return:
        // A return under a runtime branch is a control-flow join this flat
        // program has no way to express; the interpreter still handles it.
        if (branch_depth) bail("return inside a data-dependent branch");
        throw Returned{s.has_init ? expr(s.rhs) : Range{0, 0}};
      case mir::Stmt::For: {
        const long lo = cint(s.lower), hi = cint(s.upper);
        for (long v = lo; v <= hi; ++v) {
          ints[s.loopvar] = {v};
          for (const auto& k : s.body) stmt(k);
        }
        ints.erase(s.loopvar);
        return;
      }
      case mir::Stmt::IfElse: {
        long c;
        if (try_cint(s.cond, &c)) {
          if (c != 0 && !s.body.empty()) stmt(s.body[0]);
          if (c == 0 && s.body.size() > 1) stmt(s.body[1]);
          return;
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
    const int saved_branch_depth = branch_depth;
    reals.clear();
    ints.clear();
    branch_depth = 0;
    for (size_t k = 0; k < f.arg_names.size(); ++k) {
      if (args[k].is_const_int)
        ints[f.arg_names[k]] = args[k].ints;
      else
        reals[f.arg_names[k]] = args[k].real;
    }
    Range out{0, 0};
    try {
      for (const auto& s : f.body) stmt(s);
      bail("function " + f.name + " returned no value");
    } catch (Returned& r) {
      out = r.r;
    } catch (...) {
      reals = std::move(saved_reals);
      ints = std::move(saved_ints);
      branch_depth = saved_branch_depth;
      --inline_depth;
      throw;
    }
    reals = std::move(saved_reals);
    ints = std::move(saved_ints);
    branch_depth = saved_branch_depth;
    --inline_depth;
    return out;
  }
};

}  // namespace stanli

#endif
